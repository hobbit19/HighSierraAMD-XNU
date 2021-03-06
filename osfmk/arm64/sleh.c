/*
 * Copyright (c) 2012-2016 Apple Inc. All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 *
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. The rights granted to you under the License
 * may not be used to create, or enable the creation or redistribution of,
 * unlawful or unlicensed copies of an Apple operating system, or to
 * circumvent, violate, or enable the circumvention or violation of, any
 * terms of an Apple operating system software license agreement.
 *
 * Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this file.
 *
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */

#include <arm/caches_internal.h>
#include <arm/cpu_data.h>
#include <arm/cpu_data_internal.h>
#include <arm/misc_protos.h>
#include <arm/thread.h>
#include <arm/rtclock.h>
#include <arm/trap.h> /* for IS_ARM_GDB_TRAP() et al */
#include <arm64/proc_reg.h>
#include <arm64/machine_machdep.h>
#include <arm64/monotonic.h>

#include <kern/debug.h>
#include <kern/thread.h>
#include <mach/exception.h>
#include <mach/vm_types.h>
#include <mach/machine/thread_status.h>

#include <machine/atomic.h>
#include <machine/machlimits.h>

#include <pexpert/arm/protos.h>

#include <vm/vm_page.h>
#include <vm/pmap.h>
#include <vm/vm_fault.h>
#include <vm/vm_kern.h>

#include <sys/kdebug.h>

#include <kern/policy_internal.h>
#if CONFIG_TELEMETRY
#include <kern/telemetry.h>
#endif

#include <prng/random.h>

#ifndef __arm64__
#error Should only be compiling for arm64.
#endif

#define TEST_CONTEXT32_SANITY(context) \
	(context->ss.ash.flavor == ARM_SAVED_STATE32 && context->ss.ash.count == ARM_SAVED_STATE32_COUNT && \
	 context->ns.nsh.flavor == ARM_NEON_SAVED_STATE32 && context->ns.nsh.count == ARM_NEON_SAVED_STATE32_COUNT)

#define TEST_CONTEXT64_SANITY(context) \
	(context->ss.ash.flavor == ARM_SAVED_STATE64 && context->ss.ash.count == ARM_SAVED_STATE64_COUNT && \
	 context->ns.nsh.flavor == ARM_NEON_SAVED_STATE64 && context->ns.nsh.count == ARM_NEON_SAVED_STATE64_COUNT)

#define ASSERT_CONTEXT_SANITY(context) \
	assert(TEST_CONTEXT32_SANITY(context) || TEST_CONTEXT64_SANITY(context))


#define COPYIN(src, dst, size)					\
	(PSR64_IS_KERNEL(get_saved_state_cpsr(state)))  ?   \
		copyin_kern(src, dst, size)			\
	:							\
		copyin(src, dst, size)

#define COPYOUT(src, dst, size)					\
	(PSR64_IS_KERNEL(get_saved_state_cpsr(state)))  ?   \
		copyout_kern(src, dst, size)			\
	:							\
		copyout(src, dst, size)

// Below is for concatenating a string param to a string literal
#define STR1(x) #x
#define STR(x) STR1(x)

void panic_with_thread_kernel_state(const char *msg, arm_saved_state_t *ss);

void sleh_synchronous_sp1(arm_context_t *, uint32_t, vm_offset_t);
void sleh_synchronous(arm_context_t *, uint32_t, vm_offset_t);
void sleh_irq(arm_saved_state_t *);
void sleh_fiq(arm_saved_state_t *);
void sleh_serror(arm_context_t *context, uint32_t esr, vm_offset_t far);
void sleh_invalid_stack(arm_context_t *context, uint32_t esr, vm_offset_t far);

static void sleh_interrupt_handler_prologue(arm_saved_state_t *, unsigned int type);
static void sleh_interrupt_handler_epilogue(void);

static void handle_svc(arm_saved_state_t *);
static void handle_mach_absolute_time_trap(arm_saved_state_t *);
static void handle_mach_continuous_time_trap(arm_saved_state_t *);

static void handle_msr_trap(arm_saved_state_t *state, uint32_t iss);

extern kern_return_t arm_fast_fault(pmap_t, vm_map_address_t, vm_prot_t, boolean_t);

static void handle_uncategorized(arm_saved_state_t *, boolean_t);
static void handle_breakpoint(arm_saved_state_t *);

typedef void(*abort_inspector_t)(uint32_t, fault_status_t *, vm_prot_t *);
static void inspect_instruction_abort(uint32_t, fault_status_t *, vm_prot_t *);
static void inspect_data_abort(uint32_t, fault_status_t *, vm_prot_t *);

static int is_vm_fault(fault_status_t);
static int is_alignment_fault(fault_status_t);

typedef void(*abort_handler_t)(arm_saved_state_t *, uint32_t, vm_offset_t, fault_status_t, vm_prot_t, vm_offset_t);
static void handle_user_abort(arm_saved_state_t *, uint32_t, vm_offset_t, fault_status_t, vm_prot_t, vm_offset_t);
static void handle_kernel_abort(arm_saved_state_t *, uint32_t, vm_offset_t, fault_status_t, vm_prot_t, vm_offset_t);

static void handle_pc_align(arm_saved_state_t *ss);
static void handle_sp_align(arm_saved_state_t *ss);
static void handle_sw_step_debug(arm_saved_state_t *ss);
static void handle_wf_trap(arm_saved_state_t *ss);

static void handle_watchpoint(vm_offset_t fault_addr);

static void handle_abort(arm_saved_state_t *, uint32_t, vm_offset_t, vm_offset_t, abort_inspector_t, abort_handler_t);

static void handle_user_trapped_instruction32(arm_saved_state_t *, uint32_t esr);

static void handle_simd_trap(arm_saved_state_t *, uint32_t esr);

extern void mach_kauth_cred_uthread_update(void);
void   mach_syscall_trace_exit(unsigned int retval, unsigned int call_number);

struct uthread;
struct proc;

extern void
unix_syscall(struct arm_saved_state * regs, thread_t thread_act,
	     struct uthread * uthread, struct proc * proc);

extern void
mach_syscall(struct arm_saved_state*);

volatile perfCallback    perfTrapHook = NULL;	/* Pointer to CHUD trap hook routine */

#if CONFIG_DTRACE
extern kern_return_t dtrace_user_probe(arm_saved_state_t* regs);
extern boolean_t dtrace_tally_fault(user_addr_t);

/* Traps for userland processing. Can't include bsd/sys/fasttrap_isa.h, so copy and paste the trap instructions
   over from that file. Need to keep these in sync! */
#define FASTTRAP_ARM32_INSTR 0xe7ffdefc
#define FASTTRAP_THUMB32_INSTR 0xdefc
#define FASTTRAP_ARM64_INSTR 0xe7eeee7e

#define FASTTRAP_ARM32_RET_INSTR 0xe7ffdefb
#define FASTTRAP_THUMB32_RET_INSTR 0xdefb
#define FASTTRAP_ARM64_RET_INSTR 0xe7eeee7d

/* See <rdar://problem/4613924> */
perfCallback tempDTraceTrapHook = NULL; /* Pointer to DTrace fbt trap hook routine */
#endif

#if CONFIG_PGTRACE
extern boolean_t pgtrace_enabled;
#endif

#if __ARM_PAN_AVAILABLE__
#endif

#if defined(APPLECYCLONE)
#define CPU_NAME	"Cyclone"
#elif defined(APPLETYPHOON)
#define CPU_NAME	"Typhoon"
#elif defined(APPLETWISTER)
#define CPU_NAME	"Twister"
#elif defined(APPLEHURRICANE)
#define CPU_NAME	"Hurricane"
#else
#define CPU_NAME	"Unknown"
#endif

#if (CONFIG_KERNEL_INTEGRITY && defined(KERNEL_INTEGRITY_WT))
#define ESR_WT_SERROR(esr) (((esr) & 0xffffff00) == 0xbf575400)
#define ESR_WT_REASON(esr) ((esr) & 0xff)

#define WT_REASON_NONE           0
#define WT_REASON_INTEGRITY_FAIL 1
#define WT_REASON_BAD_SYSCALL    2
#define WT_REASON_NOT_LOCKED     3
#define WT_REASON_ALREADY_LOCKED 4
#define WT_REASON_SW_REQ         5
#define WT_REASON_PT_INVALID     6
#define WT_REASON_PT_VIOLATION   7
#define WT_REASON_REG_VIOLATION  8
#endif


static inline unsigned
__ror(unsigned value, unsigned shift)
{
	return (((unsigned)(value) >> (unsigned)(shift)) |
	        (unsigned)(value) << ((unsigned)(sizeof(unsigned) * CHAR_BIT) - (unsigned)(shift)));
}

static void
arm64_implementation_specific_error(arm_saved_state_t *state, uint32_t esr, vm_offset_t far)
{
#if defined(APPLE_ARM64_ARCH_FAMILY)
	uint64_t fed_err_sts, mmu_err_sts, lsu_err_sts;
#if defined(NO_ECORE)
	uint64_t l2c_err_sts, l2c_err_adr, l2c_err_inf;

	mmu_err_sts = __builtin_arm_rsr64(STR(ARM64_REG_MMU_ERR_STS));
	l2c_err_sts = __builtin_arm_rsr64(STR(ARM64_REG_L2C_ERR_STS));
	l2c_err_adr = __builtin_arm_rsr64(STR(ARM64_REG_L2C_ERR_ADR));
	l2c_err_inf = __builtin_arm_rsr64(STR(ARM64_REG_L2C_ERR_INF));
	lsu_err_sts = __builtin_arm_rsr64(STR(ARM64_REG_LSU_ERR_STS));
	fed_err_sts = __builtin_arm_rsr64(STR(ARM64_REG_FED_ERR_STS));

	panic_plain("Unhandled " CPU_NAME
	            " implementation specific error. state=%p esr=%#x far=%p\n"
	            "\tlsu_err_sts:%p, fed_err_sts:%p, mmu_err_sts:%p\n"
	            "\tl2c_err_sts:%p, l2c_err_adr:%p, l2c_err_inf:%p\n",
	            state, esr, (void *)far,
	            (void *)lsu_err_sts, (void *)fed_err_sts, (void *)mmu_err_sts,
	            (void *)l2c_err_sts, (void *)l2c_err_adr, (void *)l2c_err_inf);

#elif defined(HAS_MIGSTS)
	uint64_t l2c_err_sts, l2c_err_adr, l2c_err_inf, mpidr, migsts;

	mpidr = __builtin_arm_rsr64("MPIDR_EL1");
	migsts = __builtin_arm_rsr64(STR(ARM64_REG_MIGSTS_EL1));
	mmu_err_sts = __builtin_arm_rsr64(STR(ARM64_REG_MMU_ERR_STS));
	l2c_err_sts = __builtin_arm_rsr64(STR(ARM64_REG_L2C_ERR_STS));
	l2c_err_adr = __builtin_arm_rsr64(STR(ARM64_REG_L2C_ERR_ADR));
	l2c_err_inf = __builtin_arm_rsr64(STR(ARM64_REG_L2C_ERR_INF));
	lsu_err_sts = __builtin_arm_rsr64(STR(ARM64_REG_LSU_ERR_STS));
	fed_err_sts = __builtin_arm_rsr64(STR(ARM64_REG_FED_ERR_STS));

	panic_plain("Unhandled " CPU_NAME
	            " implementation specific error. state=%p esr=%#x far=%p p-core?%d migsts=%p\n"
	            "\tlsu_err_sts:%p, fed_err_sts:%p, mmu_err_sts:%p\n"
	            "\tl2c_err_sts:%p, l2c_err_adr:%p, l2c_err_inf:%p\n",
	            state, esr, (void *)far, !!(mpidr & MPIDR_PNE), (void *)migsts,
	            (void *)lsu_err_sts, (void *)fed_err_sts, (void *)mmu_err_sts,
	            (void *)l2c_err_sts, (void *)l2c_err_adr, (void *)l2c_err_inf);
#else // !defined(NO_ECORE) && !defined(HAS_MIGSTS)
	uint64_t llc_err_sts, llc_err_adr, llc_err_inf, mpidr;

	mpidr = __builtin_arm_rsr64("MPIDR_EL1");

	if (mpidr & MPIDR_PNE) {
		mmu_err_sts = __builtin_arm_rsr64(STR(ARM64_REG_MMU_ERR_STS));
		lsu_err_sts = __builtin_arm_rsr64(STR(ARM64_REG_LSU_ERR_STS));
		fed_err_sts = __builtin_arm_rsr64(STR(ARM64_REG_FED_ERR_STS));
	} else {
		mmu_err_sts = __builtin_arm_rsr64(STR(ARM64_REG_E_MMU_ERR_STS));
		lsu_err_sts = __builtin_arm_rsr64(STR(ARM64_REG_E_LSU_ERR_STS));
		fed_err_sts = __builtin_arm_rsr64(STR(ARM64_REG_E_FED_ERR_STS));
	}

	llc_err_sts = __builtin_arm_rsr64(STR(ARM64_REG_L2C_ERR_STS));
	llc_err_adr = __builtin_arm_rsr64(STR(ARM64_REG_L2C_ERR_ADR));
	llc_err_inf = __builtin_arm_rsr64(STR(ARM64_REG_L2C_ERR_INF));

	panic_plain("Unhandled " CPU_NAME
	            " implementation specific error. state=%p esr=%#x far=%p p-core?%d\n"
	            "\tlsu_err_sts:%p, fed_err_sts:%p, mmu_err_sts:%p\n"
	            "\tllc_err_sts:%p, llc_err_adr:%p, llc_err_inf:%p\n",
	            state, esr, (void *)far, !!(mpidr & MPIDR_PNE),
	            (void *)lsu_err_sts, (void *)fed_err_sts, (void *)mmu_err_sts,
	            (void *)llc_err_sts, (void *)llc_err_adr, (void *)llc_err_inf);
#endif
#else // !defined(APPLE_ARM64_ARCH_FAMILY)
	panic_plain("Unhandled implementation specific error\n");
#endif
}

#if CONFIG_KERNEL_INTEGRITY
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
static void
kernel_integrity_error_handler(uint32_t esr, vm_offset_t far) {
#if defined(KERNEL_INTEGRITY_WT)
#if (DEVELOPMENT || DEBUG)
	if (ESR_WT_SERROR(esr)) {
		switch (ESR_WT_REASON(esr)) {
		case WT_REASON_INTEGRITY_FAIL:
			panic_plain("Kernel integrity, violation in frame 0x%016lx.", far);
		case WT_REASON_BAD_SYSCALL:
			panic_plain("Kernel integrity, bad syscall.");
		case WT_REASON_NOT_LOCKED:
			panic_plain("Kernel integrity, not locked.");
		case WT_REASON_ALREADY_LOCKED:
			panic_plain("Kernel integrity, already locked.");
		case WT_REASON_SW_REQ:
			panic_plain("Kernel integrity, software request.");
		case WT_REASON_PT_INVALID:
			panic_plain("Kernel integrity, encountered invalid TTE/PTE while "
				"walking 0x%016lx.", far);
		case WT_REASON_PT_VIOLATION:
			panic_plain("Kernel integrity, violation in mapping 0x%016lx.",
				far);
		case WT_REASON_REG_VIOLATION:
			panic_plain("Kernel integrity, violation in system register %d.",
				(unsigned) far);
		default:
			panic_plain("Kernel integrity, unknown (esr=0x%08x).", esr);
		}
	}
#else
	if (ESR_WT_SERROR(esr)) {
		panic_plain("SError esr: 0x%08x far: 0x%016lx.", esr, far);
	}
#endif
#endif
}
#pragma clang diagnostic pop
#endif

static void
arm64_platform_error(arm_saved_state_t *state, uint32_t esr, vm_offset_t far)
{
	cpu_data_t	*cdp = getCpuDatap();

#if CONFIG_KERNEL_INTEGRITY
	kernel_integrity_error_handler(esr, far);
#endif

	if (cdp->platform_error_handler != (platform_error_handler_t) NULL)
		(*(platform_error_handler_t)cdp->platform_error_handler) (cdp->cpu_id, far);
	else
		arm64_implementation_specific_error(state, esr, far);
}

void
panic_with_thread_kernel_state(const char *msg, arm_saved_state_t *ss)
{
	boolean_t ss_valid;

	ss_valid = is_saved_state64(ss);
	arm_saved_state64_t *state = saved_state64(ss);

	panic_plain("%s (saved state: %p%s)\n"
		"\t  x0: 0x%016llx  x1:  0x%016llx  x2:  0x%016llx  x3:  0x%016llx\n"
		"\t  x4: 0x%016llx  x5:  0x%016llx  x6:  0x%016llx  x7:  0x%016llx\n"
		"\t  x8: 0x%016llx  x9:  0x%016llx  x10: 0x%016llx  x11: 0x%016llx\n"
		"\t  x12: 0x%016llx x13: 0x%016llx  x14: 0x%016llx  x15: 0x%016llx\n"
		"\t  x16: 0x%016llx x17: 0x%016llx  x18: 0x%016llx  x19: 0x%016llx\n"
		"\t  x20: 0x%016llx x21: 0x%016llx  x22: 0x%016llx  x23: 0x%016llx\n"
		"\t  x24: 0x%016llx x25: 0x%016llx  x26: 0x%016llx  x27: 0x%016llx\n"
		"\t  x28: 0x%016llx fp:  0x%016llx  lr:  0x%016llx  sp:  0x%016llx\n"
		"\t  pc:  0x%016llx cpsr: 0x%08x         esr: 0x%08x          far: 0x%016llx\n",
			msg, ss, (ss_valid ? "" : " INVALID"),
			state->x[0], state->x[1], state->x[2], state->x[3],
			state->x[4], state->x[5], state->x[6], state->x[7],
			state->x[8], state->x[9], state->x[10], state->x[11],
			state->x[12], state->x[13], state->x[14], state->x[15],
			state->x[16], state->x[17], state->x[18], state->x[19],
			state->x[20], state->x[21], state->x[22], state->x[23],
			state->x[24], state->x[25], state->x[26], state->x[27],
			state->x[28], state->fp, state->lr, state->sp,
			state->pc, state->cpsr, state->esr, state->far);
}


void
sleh_synchronous_sp1(arm_context_t *context, uint32_t esr, vm_offset_t far __unused)
{
	esr_exception_class_t	class = ESR_EC(esr);
	arm_saved_state_t	*state = &context->ss;

	switch (class) {
	case ESR_EC_UNCATEGORIZED:
	{
		uint32_t instr = *((uint32_t*)get_saved_state_pc(state));
		if (IS_ARM_GDB_TRAP(instr))
			DebuggerCall(EXC_BREAKPOINT, state);
		// Intentionally fall through to panic if we return from the debugger
	}
	default:
		panic_with_thread_kernel_state("Synchronous exception taken while SP1 selected", state);
	}
}

void
sleh_synchronous(arm_context_t *context, uint32_t esr, vm_offset_t far)
{
	esr_exception_class_t 	class = ESR_EC(esr);
	arm_saved_state_t 	  	*state = &context->ss;
	vm_offset_t				recover = 0;
	thread_t				thread = current_thread();

	ASSERT_CONTEXT_SANITY(context);

	/* Don't run exception handler with recover handler set in case of double fault */
	if (thread->recover) {
		recover = thread->recover;
		thread->recover = (vm_offset_t)NULL;
	}

	/* Inherit the interrupt masks from previous context */
	if (SPSR_INTERRUPTS_ENABLED(get_saved_state_cpsr(state)))
		ml_set_interrupts_enabled(TRUE);

	switch (class) {
	case ESR_EC_SVC_64:
		if (!is_saved_state64(state) || !PSR64_IS_USER(get_saved_state_cpsr(state))) {
			panic("Invalid SVC_64 context");
		}

		handle_svc(state);
		break;

	case ESR_EC_DABORT_EL0:
		handle_abort(state, esr, far, recover, inspect_data_abort, handle_user_abort);
		assert(0); /* Unreachable */

	case ESR_EC_MSR_TRAP:
		handle_msr_trap(state, ESR_ISS(esr));
		break;

	case ESR_EC_IABORT_EL0:
		handle_abort(state, esr, far, recover, inspect_instruction_abort, handle_user_abort);
		assert(0); /* Unreachable */

	case ESR_EC_IABORT_EL1:
		panic("Kernel instruction fetch abort: pc=%p iss=0x%x far=%p. Note: the faulting frame may be missing in the backtrace.",
			  (void *)get_saved_state_pc(state), ESR_ISS(esr), (void*)far);

	case ESR_EC_PC_ALIGN:
		handle_pc_align(state);
		assert(0); /* Unreachable */
		break;

	case ESR_EC_DABORT_EL1:
		handle_abort(state, esr, far, recover, inspect_data_abort, handle_kernel_abort);
		break;

	case ESR_EC_UNCATEGORIZED:
		assert(!ESR_ISS(esr));

		handle_uncategorized(&context->ss, ESR_INSTR_IS_2BYTES(esr));
		/* TODO: Uncomment this after stackshot uses a brk instruction
		 * rather than an undefined instruction, as stackshot is the
		 * only case where we want to return to the first-level handler.
		 */
		//assert(0); /* Unreachable */
		break;

	case ESR_EC_SP_ALIGN:
		handle_sp_align(state);
		assert(0); /* Unreachable */
		break;

	case ESR_EC_BKPT_AARCH32:
		handle_breakpoint(state);
		assert(0); /* Unreachable */
		break;

	case ESR_EC_BRK_AARCH64:
		if (PSR64_IS_KERNEL(get_saved_state_cpsr(state))) {

			kprintf("Breakpoint instruction exception from kernel.  Hanging here (by design).\n");
			for (;;);

			__unreachable_ok_push
			DebuggerCall(EXC_BREAKPOINT, &context->ss);
			break;
			__unreachable_ok_pop
		} else {
			handle_breakpoint(state);
			assert(0); /* Unreachable */
		}

	case ESR_EC_BKPT_REG_MATCH_EL0:
		if (FSC_DEBUG_FAULT == ISS_SSDE_FSC(esr)) {
			handle_breakpoint(state);
			assert(0); /* Unreachable */
		}
		panic("Unsupported Class %u event code. state=%p class=%u esr=%u far=%p",
			  class, state, class, esr, (void *)far);
		assert(0); /* Unreachable */
		break;

	case ESR_EC_BKPT_REG_MATCH_EL1:
		if (FSC_DEBUG_FAULT == ISS_SSDE_FSC(esr)) {
			kprintf("Hardware Breakpoint Debug exception from kernel.  Hanging here (by design).\n");
			for (;;);

			__unreachable_ok_push
			DebuggerCall(EXC_BREAKPOINT, &context->ss);
			break;
			__unreachable_ok_pop
		}
		panic("Unsupported Class %u event code. state=%p class=%u esr=%u far=%p",
			  class, state, class, esr, (void *)far);
		assert(0); /* Unreachable */
		break;

	case ESR_EC_SW_STEP_DEBUG_EL0:
		if (FSC_DEBUG_FAULT == ISS_SSDE_FSC(esr)) {
			handle_sw_step_debug(state);
			assert(0); /* Unreachable */
		}
		panic("Unsupported Class %u event code. state=%p class=%u esr=%u far=%p",
			  class, state, class, esr, (void *)far);
		assert(0); /* Unreachable */
		break;

	case ESR_EC_SW_STEP_DEBUG_EL1:
		if (FSC_DEBUG_FAULT == ISS_SSDE_FSC(esr)) {
			kprintf("Software Step Debug exception from kernel.  Hanging here (by design).\n");
			for (;;);

			__unreachable_ok_push
			DebuggerCall(EXC_BREAKPOINT, &context->ss);
			break;
			__unreachable_ok_pop
		}
		panic("Unsupported Class %u event code. state=%p class=%u esr=%u far=%p",
			  class, state, class, esr, (void *)far);
		assert(0); /* Unreachable */
		break;

	case ESR_EC_WATCHPT_MATCH_EL0:
		if (FSC_DEBUG_FAULT == ISS_SSDE_FSC(esr)) {
			handle_watchpoint(far);
			assert(0); /* Unreachable */
		}
		panic("Unsupported Class %u event code. state=%p class=%u esr=%u far=%p",
			  class, state, class, esr, (void *)far);
		assert(0); /* Unreachable */
		break;

	case ESR_EC_WATCHPT_MATCH_EL1:
		/*
		 * If we hit a watchpoint in kernel mode, probably in a copyin/copyout which we don't want to
		 * abort.  Turn off watchpoints and keep going; we'll turn them back on in return_from_exception..
		 */
		if (FSC_DEBUG_FAULT == ISS_SSDE_FSC(esr)) {
			arm_debug_set(NULL);
			break; /* return to first level handler */
		}
		panic("Unsupported Class %u event code. state=%p class=%u esr=%u far=%p",
			  class, state, class, esr, (void *)far);
		assert(0); /* Unreachable */
		break;

	case ESR_EC_TRAP_SIMD_FP:
		handle_simd_trap(state, esr);
		assert(0);
		break;

	case ESR_EC_ILLEGAL_INSTR_SET:
		if (EXCB_ACTION_RERUN != 
			ex_cb_invoke(EXCB_CLASS_ILLEGAL_INSTR_SET, far)) {
			// instruction is not re-executed
			panic("Illegal instruction set exception. state=%p class=%u esr=%u far=%p spsr=0x%x",
				state, class, esr, (void *)far, get_saved_state_cpsr(state));
			assert(0);
		}
		// must clear this fault in PSR to re-run
		set_saved_state_cpsr(state, get_saved_state_cpsr(state) & (~PSR64_IL));
		break;

	case ESR_EC_MCR_MRC_CP15_TRAP:
	case ESR_EC_MCRR_MRRC_CP15_TRAP:
	case ESR_EC_MCR_MRC_CP14_TRAP:
	case ESR_EC_LDC_STC_CP14_TRAP:
	case ESR_EC_MCRR_MRRC_CP14_TRAP:
		handle_user_trapped_instruction32(state, esr);
		assert(0);
		break;

	case ESR_EC_WFI_WFE:
		// Use of WFI or WFE instruction when they have been disabled for EL0
		handle_wf_trap(state);
		assert(0);	/* Unreachable */
		break;

	default:
		panic("Unsupported synchronous exception. state=%p class=%u esr=%u far=%p",
			  state, class, esr, (void *)far);
		assert(0); /* Unreachable */
		break;
	}

	if (recover)
		thread->recover = recover;
}

/*
 * Uncategorized exceptions are a catch-all for general execution errors.
 * ARM64_TODO: For now, we assume this is for undefined instruction exceptions.
 */
static void
handle_uncategorized(arm_saved_state_t *state, boolean_t instrLen2)
{
	exception_type_t 			exception = EXC_BAD_INSTRUCTION;
	mach_exception_data_type_t 	codes[2] = {EXC_ARM_UNDEFINED};
	mach_msg_type_number_t 		numcodes = 2;
	uint32_t					instr;

	if (instrLen2) {
		uint16_t	instr16;
		COPYIN(get_saved_state_pc(state), (char *)&instr16, sizeof(instr16));

		instr = instr16;
	} else {
		COPYIN(get_saved_state_pc(state), (char *)&instr, sizeof(instr));
	}

#if CONFIG_DTRACE
	if (tempDTraceTrapHook && (tempDTraceTrapHook(exception, state, 0, 0) == KERN_SUCCESS)) {
		return;
	}

	if (PSR64_IS_USER64(get_saved_state_cpsr(state))) {
		/*
		 * For a 64bit user process, we care about all 4 bytes of the
		 * instr.
		 */
		if (instr == FASTTRAP_ARM64_INSTR || instr == FASTTRAP_ARM64_RET_INSTR) {
			if (dtrace_user_probe(state) == KERN_SUCCESS)
				return;
		}
	} else if (PSR64_IS_USER32(get_saved_state_cpsr(state))) {
		/*
		 * For a 32bit user process, we check for thumb mode, in
		 * which case we only care about a 2 byte instruction length.
		 * For non-thumb mode, we care about all 4 bytes of the instructin.
		 */
		if (get_saved_state_cpsr(state) & PSR64_MODE_USER32_THUMB) {
			if (((uint16_t)instr == FASTTRAP_THUMB32_INSTR) ||
			    ((uint16_t)instr == FASTTRAP_THUMB32_RET_INSTR)) {
				if (dtrace_user_probe(state) == KERN_SUCCESS) {
					return;
				}
			}
		} else {
			if ((instr == FASTTRAP_ARM32_INSTR) ||
			    (instr == FASTTRAP_ARM32_RET_INSTR)) {
				if (dtrace_user_probe(state) == KERN_SUCCESS) {
					return;
				}
			}
		}
	}

#endif /* CONFIG_DTRACE */

	if (PSR64_IS_KERNEL(get_saved_state_cpsr(state))) {
		if (IS_ARM_GDB_TRAP(instr)) {
			boolean_t interrupt_state;
			vm_offset_t kstackptr;
			exception = EXC_BREAKPOINT;

			interrupt_state = ml_set_interrupts_enabled(FALSE);

			/* Save off the context here (so that the debug logic
			 * can see the original state of this thread).
			 */
			kstackptr = (vm_offset_t) current_thread()->machine.kstackptr;
			if (kstackptr) {
				((thread_kernel_state_t) kstackptr)->machine.ss = *state;
			}

			/* Hop into the debugger (typically either due to a
			 * fatal exception, an explicit panic, or a stackshot
			 * request.
			 */
			DebuggerCall(exception, state);

			(void) ml_set_interrupts_enabled(interrupt_state);
			return;
		} else {
			panic("Undefined kernel instruction: pc=%p instr=%x\n", (void*)get_saved_state_pc(state), instr);
		}
	}

	/*
	 * Check for GDB  breakpoint via illegal opcode.
	 */
	if (instrLen2) {
		if (IS_THUMB_GDB_TRAP(instr)) {
			exception = EXC_BREAKPOINT;
			codes[0] = EXC_ARM_BREAKPOINT;
			codes[1] = instr;
		} else {
			codes[1] = instr;
		}
	} else {
		if (IS_ARM_GDB_TRAP(instr)) {
			exception = EXC_BREAKPOINT;
			codes[0] = EXC_ARM_BREAKPOINT;
			codes[1] = instr;
		} else if (IS_THUMB_GDB_TRAP((instr & 0xFFFF))) {
			exception = EXC_BREAKPOINT;
			codes[0] = EXC_ARM_BREAKPOINT;
			codes[1] = instr & 0xFFFF;
		} else if (IS_THUMB_GDB_TRAP((instr >> 16))) {
			exception = EXC_BREAKPOINT;
			codes[0] = EXC_ARM_BREAKPOINT;
			codes[1] = instr >> 16;
		} else {
			codes[1] = instr;
		}
	}

	exception_triage(exception, codes, numcodes);
	assert(0); /* NOTREACHED */
}

static void
handle_breakpoint(arm_saved_state_t *state)
{
	exception_type_t 			exception = EXC_BREAKPOINT;
	mach_exception_data_type_t 	codes[2] = {EXC_ARM_BREAKPOINT};
	mach_msg_type_number_t 		numcodes = 2;

	codes[1] = get_saved_state_pc(state);
	exception_triage(exception, codes, numcodes);
	assert(0); /* NOTREACHED */
}

static void
handle_watchpoint(vm_offset_t fault_addr)
{
	exception_type_t 			exception = EXC_BREAKPOINT;
	mach_exception_data_type_t 	codes[2] = {EXC_ARM_DA_DEBUG};
	mach_msg_type_number_t 		numcodes = 2;

	codes[1] = fault_addr;
	exception_triage(exception, codes, numcodes);
	assert(0); /* NOTREACHED */
}

static void
handle_abort(arm_saved_state_t *state, uint32_t esr, vm_offset_t fault_addr, vm_offset_t recover,
			 abort_inspector_t inspect_abort, abort_handler_t handler)
{
	fault_status_t		fault_code;
	vm_prot_t			fault_type;

	inspect_abort(ESR_ISS(esr), &fault_code, &fault_type);
	handler(state, esr, fault_addr, fault_code, fault_type, recover);
}

static void
inspect_instruction_abort(uint32_t iss, fault_status_t *fault_code, vm_prot_t *fault_type)
{
	getCpuDatap()->cpu_stat.instr_ex_cnt++;
	*fault_code = ISS_IA_FSC(iss);
	*fault_type = (VM_PROT_READ | VM_PROT_EXECUTE);
}

static void
inspect_data_abort(uint32_t iss, fault_status_t *fault_code, vm_prot_t *fault_type)
{
	getCpuDatap()->cpu_stat.data_ex_cnt++;
	*fault_code = ISS_DA_FSC(iss);

	/* Cache operations report faults as write access. Change these to read access. */
	if ((iss & ISS_DA_WNR) && !(iss & ISS_DA_CM)) {
		*fault_type = (VM_PROT_READ | VM_PROT_WRITE);
	} else {
		*fault_type = (VM_PROT_READ);
	}
}

static void
handle_pc_align(arm_saved_state_t *ss)
{
	exception_type_t exc;
	mach_exception_data_type_t codes[2];
	mach_msg_type_number_t numcodes = 2;

	if (!PSR64_IS_USER(get_saved_state_cpsr(ss))) {
		panic_with_thread_kernel_state("PC alignment exception from kernel.", ss);
	}

	exc = EXC_BAD_ACCESS;
	codes[0] = EXC_ARM_DA_ALIGN;
	codes[1] = get_saved_state_pc(ss);

	exception_triage(exc, codes, numcodes);
	assert(0); /* NOTREACHED */
}

static void
handle_sp_align(arm_saved_state_t *ss)
{
	exception_type_t exc;
	mach_exception_data_type_t codes[2];
	mach_msg_type_number_t numcodes = 2;

	if (!PSR64_IS_USER(get_saved_state_cpsr(ss))) {
		panic_with_thread_kernel_state("SP alignment exception from kernel.", ss);
	}

	exc = EXC_BAD_ACCESS;
	codes[0] = EXC_ARM_SP_ALIGN;
	codes[1] = get_saved_state_sp(ss);

	exception_triage(exc, codes, numcodes);
	assert(0); /* NOTREACHED */
}

static void
handle_wf_trap(arm_saved_state_t *ss)
{
	exception_type_t exc;
	mach_exception_data_type_t codes[2];
	mach_msg_type_number_t numcodes = 2;

	exc = EXC_BAD_INSTRUCTION;
	codes[0] = EXC_ARM_UNDEFINED;
	codes[1] = get_saved_state_sp(ss);

	exception_triage(exc, codes, numcodes);
	assert(0); /* NOTREACHED */
}


static void
handle_sw_step_debug(arm_saved_state_t *state)
{
	thread_t thread = current_thread();
	exception_type_t exc;
	mach_exception_data_type_t codes[2];
	mach_msg_type_number_t numcodes = 2;

	if (!PSR64_IS_USER(get_saved_state_cpsr(state))) {
		panic_with_thread_kernel_state("SW_STEP_DEBUG exception from kernel.", state);
	}

	// Disable single step and unmask interrupts (in the saved state, anticipating next exception return)
	if (thread->machine.DebugData != NULL) {
		thread->machine.DebugData->uds.ds64.mdscr_el1 &= ~0x1;
	} else {
		panic_with_thread_kernel_state("SW_STEP_DEBUG exception thread DebugData is NULL.", state);
	}

	set_saved_state_cpsr((thread->machine.upcb),
	    get_saved_state_cpsr((thread->machine.upcb)) & ~(PSR64_SS | DAIF_IRQF | DAIF_FIQF));

	// Special encoding for gdb single step event on ARM
	exc = EXC_BREAKPOINT;
	codes[0] = 1;
	codes[1] = 0;

	exception_triage(exc, codes, numcodes);
	assert(0); /* NOTREACHED */
}

static int
is_vm_fault(fault_status_t status)
{
	switch (status) {
	case FSC_TRANSLATION_FAULT_L0:
	case FSC_TRANSLATION_FAULT_L1:
	case FSC_TRANSLATION_FAULT_L2:
	case FSC_TRANSLATION_FAULT_L3:
	case FSC_ACCESS_FLAG_FAULT_L1:
	case FSC_ACCESS_FLAG_FAULT_L2:
	case FSC_ACCESS_FLAG_FAULT_L3:
	case FSC_PERMISSION_FAULT_L1:
	case FSC_PERMISSION_FAULT_L2:
	case FSC_PERMISSION_FAULT_L3:
		return TRUE;
	default:
		return FALSE;
	}
}

#if __ARM_PAN_AVAILABLE__
static int
is_permission_fault(fault_status_t status)
{
	switch (status) {
	case FSC_PERMISSION_FAULT_L1:
	case FSC_PERMISSION_FAULT_L2:
	case FSC_PERMISSION_FAULT_L3:
		return TRUE;
	default:
		return FALSE;
	}
}
#endif

static int
is_alignment_fault(fault_status_t status)
{
	return (status == FSC_ALIGNMENT_FAULT);
}

static int
is_parity_error(fault_status_t status)
{
	switch (status) {
	case FSC_SYNC_PARITY:
	case FSC_ASYNC_PARITY:
	case FSC_SYNC_PARITY_TT_L1:
	case FSC_SYNC_PARITY_TT_L2:
	case FSC_SYNC_PARITY_TT_L3:
		return TRUE;
	default:
		return FALSE;
	}
}

static void
handle_user_abort(arm_saved_state_t *state, uint32_t esr, vm_offset_t fault_addr,
				  fault_status_t fault_code, vm_prot_t fault_type, vm_offset_t recover)
{
	exception_type_t		exc = EXC_BAD_ACCESS;
	mach_exception_data_type_t	codes[2];
	mach_msg_type_number_t 		numcodes = 2;
	thread_t			thread = current_thread();

	(void)esr;
	(void)state;

	if (ml_at_interrupt_context())
		panic_with_thread_kernel_state("Apparently on interrupt stack when taking user abort!\n", state);

	thread->iotier_override = THROTTLE_LEVEL_NONE; /* Reset IO tier override before handling abort from userspace */

	if (is_vm_fault(fault_code)) {
		kern_return_t	result = KERN_FAILURE;
		vm_map_t		map = thread->map;
		vm_offset_t		vm_fault_addr = fault_addr;

		assert(map != kernel_map);

		if (!(fault_type & VM_PROT_EXECUTE) && user_tbi_enabled())
				vm_fault_addr = tbi_clear(fault_addr);

#if CONFIG_DTRACE
		if (thread->options & TH_OPT_DTRACE) {	/* Executing under dtrace_probe? */
			if (dtrace_tally_fault(vm_fault_addr)) { /* Should a user mode fault under dtrace be ignored? */
				if (recover) {
					set_saved_state_pc(state, recover);
				} else {
					boolean_t intr = ml_set_interrupts_enabled(FALSE);
					panic_with_thread_kernel_state("copyin/out has no recovery point", state);
					(void) ml_set_interrupts_enabled(intr);
				}
				return;
			} else {
				boolean_t intr = ml_set_interrupts_enabled(FALSE);
				panic_with_thread_kernel_state("Unexpected UMW page fault under dtrace_probe", state);
				(void) ml_set_interrupts_enabled(intr);
				return;
			}
		}
#else
		(void)recover;
#endif

#if CONFIG_PGTRACE
		if (pgtrace_enabled) {
			/* Check to see if trace bit is set */
			result = pmap_pgtrace_fault(map->pmap, fault_addr, state);
			if (result == KERN_SUCCESS) return;
		}
#endif

		/* check to see if it is just a pmap ref/modify fault */

		if (result != KERN_SUCCESS) {
			result = arm_fast_fault(map->pmap, trunc_page(vm_fault_addr), fault_type, TRUE);
		}
		if (result != KERN_SUCCESS) {

			{
				/* We have to fault the page in */
				result = vm_fault(map, vm_fault_addr, fault_type,
				                  /* change_wiring */ FALSE, VM_KERN_MEMORY_NONE, THREAD_ABORTSAFE,
				                  /* caller_pmap */ NULL, /* caller_pmap_addr */ 0);
			}
		}
		if (result == KERN_SUCCESS || result == KERN_ABORTED) {
			thread_exception_return();
			/* NOTREACHED */
		}

		codes[0] = result;
	} else if (is_alignment_fault(fault_code)) {
		codes[0] = EXC_ARM_DA_ALIGN;
	} else if (is_parity_error(fault_code)) {
#if defined(APPLE_ARM64_ARCH_FAMILY)
		if (fault_code == FSC_SYNC_PARITY) {
			arm64_platform_error(state, esr, fault_addr);
			thread_exception_return();
			/* NOTREACHED */
		}
#else
		panic("User parity error.");
#endif
	} else {
		codes[0] = KERN_FAILURE;
	}

	codes[1] = fault_addr;
	exception_triage(exc, codes, numcodes);
	assert(0); /* NOTREACHED */
}

#if __ARM_PAN_AVAILABLE__
static int
is_pan_fault(arm_saved_state_t *state, uint32_t esr, vm_offset_t fault_addr, fault_status_t fault_code)
{
	// PAN (Privileged Access Never) fault occurs for data read/write in EL1 to
	// virtual address that is readable/writeable from both EL1 and EL0

	// To check for PAN fault, we evaluate if the following conditions are true:
	// 1. This is a permission fault
	// 2. PAN is enabled
	// 3. AT instruction (on which PAN has no effect) on the same faulting address
	// succeeds

	vm_offset_t pa;

	if (!(is_permission_fault(fault_code) && get_saved_state_cpsr(state) & PSR64_PAN)) {
		return FALSE;
	}

	if (esr & ISS_DA_WNR) {
		pa = mmu_kvtop_wpreflight(fault_addr);
	} else {
		pa = mmu_kvtop(fault_addr);
	}
	return (pa)? TRUE: FALSE;
}
#endif

static void
handle_kernel_abort(arm_saved_state_t *state, uint32_t esr, vm_offset_t fault_addr,
					fault_status_t fault_code, vm_prot_t fault_type, vm_offset_t recover)
{
	thread_t		thread = current_thread();
	(void)esr;

#if CONFIG_DTRACE
	if (is_vm_fault(fault_code) && thread->options & TH_OPT_DTRACE) {	/* Executing under dtrace_probe? */
		if (dtrace_tally_fault(fault_addr)) { /* Should a fault under dtrace be ignored? */
			/*
			 * Point to next instruction, or recovery handler if set.
			 */
			if (recover) {
				set_saved_state_pc(state, recover);
			} else {
				set_saved_state_pc(state, get_saved_state_pc(state) + 4);
			}
			return;
		} else {
			boolean_t intr = ml_set_interrupts_enabled(FALSE);
			panic_with_thread_kernel_state("Unexpected page fault under dtrace_probe", state);
			(void) ml_set_interrupts_enabled(intr);
			return;
		}
	}
#endif

#if !CONFIG_PGTRACE /* This will be moved next to pgtrace fault evaluation */
	if (ml_at_interrupt_context())
		panic_with_thread_kernel_state("Unexpected abort while on interrupt stack.", state);
#endif

	if (is_vm_fault(fault_code)) {
		kern_return_t		result;
		vm_map_t		map;
		int 			interruptible;

		/*
		 * Ensure no faults in the physical aperture. This could happen if
		 * a page table is incorrectly allocated from the read only region
		 * when running with KTRR.
		 */


		if (fault_addr >= gVirtBase && fault_addr < (gVirtBase+gPhysSize)) {
			panic_with_thread_kernel_state("Unexpected fault in kernel static region\n",state);
		}

		if (VM_KERNEL_ADDRESS(fault_addr) || thread == THREAD_NULL) {
			map = kernel_map;
			interruptible = THREAD_UNINT;
		} else {
			map = thread->map;
			interruptible = THREAD_ABORTSAFE;
		}

#if CONFIG_PGTRACE
		if (pgtrace_enabled) {
			/* Check to see if trace bit is set */
			result = pmap_pgtrace_fault(map->pmap, fault_addr, state);
			if (result == KERN_SUCCESS) return;
		}

		if (ml_at_interrupt_context())
			panic_with_thread_kernel_state("Unexpected abort while on interrupt stack.", state);
#endif

		/* check to see if it is just a pmap ref/modify fault */
		result = arm_fast_fault(map->pmap, trunc_page(fault_addr), fault_type, FALSE);
		if (result == KERN_SUCCESS) return;

		{
			/*
			 *  We have to "fault" the page in.
			 */
			result = vm_fault(map, fault_addr, fault_type,
			                  /* change_wiring */ FALSE, VM_KERN_MEMORY_NONE, interruptible,
			                  /* caller_pmap */ NULL, /* caller_pmap_addr */ 0);
		}

		if (result == KERN_SUCCESS) return;

		/*
		 *  If we have a recover handler, invoke it now.
		 */
		if (recover) {
			set_saved_state_pc(state, recover);
			return;
		}

#if __ARM_PAN_AVAILABLE__
		if (is_pan_fault(state, esr, fault_addr, fault_code)) {
			panic_with_thread_kernel_state("Privileged access never abort.", state);
		}
#endif

#if CONFIG_PGTRACE
	} else if (ml_at_interrupt_context()) {
		panic_with_thread_kernel_state("Unexpected abort while on interrupt stack.", state);
#endif
	} else if (is_alignment_fault(fault_code)) {
		panic_with_thread_kernel_state("Unaligned kernel data abort.", state);
	} else if (is_parity_error(fault_code)) {
#if defined(APPLE_ARM64_ARCH_FAMILY)
		if (fault_code == FSC_SYNC_PARITY) {
			arm64_platform_error(state, esr, fault_addr);
			return;
		}
#else
		panic_with_thread_kernel_state("Kernel parity error.", state);
#endif
	} else {
		kprintf("Unclassified kernel abort (fault_code=0x%x)\n", fault_code);
	}

	panic_with_thread_kernel_state("Kernel data abort.", state);
}

extern void syscall_trace(struct arm_saved_state * regs);

static void
handle_svc(arm_saved_state_t *state)
{
	int trap_no = get_saved_state_svc_number(state);
	thread_t thread = current_thread();
	struct proc *p;

#define handle_svc_kprintf(x...) /* kprintf("handle_svc: " x) */

#define TRACE_SYSCALL 1
#if TRACE_SYSCALL
	syscall_trace(state);
#endif

	thread->iotier_override = THROTTLE_LEVEL_NONE; /* Reset IO tier override before handling SVC from userspace */

	if (trap_no == (int)PLATFORM_SYSCALL_TRAP_NO) {
		platform_syscall(state);
		panic("Returned from platform_syscall()?");
	}

	mach_kauth_cred_uthread_update();

	if (trap_no < 0) {
		if (trap_no == -3) {
			handle_mach_absolute_time_trap(state);
			return;
		} else if (trap_no == -4) {
			handle_mach_continuous_time_trap(state);
			return;
		}

		/* Counting perhaps better in the handler, but this is how it's been done */
		thread->syscalls_mach++;
		mach_syscall(state);
	} else {
		/* Counting perhaps better in the handler, but this is how it's been done */
		thread->syscalls_unix++;
		p = get_bsdthreadtask_info(thread);

		assert(p);

		unix_syscall(state, thread, (struct uthread*)thread->uthread, p);
	}
}

static void
handle_mach_absolute_time_trap(arm_saved_state_t *state)
{
	uint64_t now = mach_absolute_time();
	saved_state64(state)->x[0] = now;
}

static void
handle_mach_continuous_time_trap(arm_saved_state_t *state)
{
	uint64_t now = mach_continuous_time();
	saved_state64(state)->x[0] = now;
}

static void
handle_msr_trap(arm_saved_state_t *state, uint32_t iss)
{
	exception_type_t 			exception = EXC_BAD_INSTRUCTION;
	mach_exception_data_type_t 	codes[2] = {EXC_ARM_UNDEFINED};
	mach_msg_type_number_t 		numcodes = 2;
	uint32_t					instr;

	(void)iss;

	if (!is_saved_state64(state)) {
		panic("MSR/MRS trap (EC 0x%x) from 32-bit state\n", ESR_EC_MSR_TRAP);
	}

	if (PSR64_IS_KERNEL(get_saved_state_cpsr(state))) {
		panic("MSR/MRS trap (EC 0x%x) from kernel\n", ESR_EC_MSR_TRAP);
	}

	COPYIN(get_saved_state_pc(state), (char *)&instr, sizeof(instr));
	codes[1] = instr;

	exception_triage(exception, codes, numcodes);
}

static void
handle_user_trapped_instruction32(arm_saved_state_t *state, uint32_t esr)
{
	exception_type_t 			exception = EXC_BAD_INSTRUCTION;
	mach_exception_data_type_t 	codes[2] = {EXC_ARM_UNDEFINED};
	mach_msg_type_number_t 		numcodes = 2;
	uint32_t					instr;

	if (is_saved_state64(state)) {
		panic("ESR (0x%x) for instruction trapped from U32, but saved state is 64-bit.", esr);
	}

	if (PSR64_IS_KERNEL(get_saved_state_cpsr(state))) {
		panic("ESR (0x%x) for instruction trapped from U32, actually came from kernel?", esr);
	}

	COPYIN(get_saved_state_pc(state), (char *)&instr, sizeof(instr));
	codes[1] = instr;

	exception_triage(exception, codes, numcodes);
}

static void
handle_simd_trap(arm_saved_state_t *state, uint32_t esr)
{
	exception_type_t 			exception = EXC_BAD_INSTRUCTION;
	mach_exception_data_type_t 	codes[2] = {EXC_ARM_UNDEFINED};
	mach_msg_type_number_t 		numcodes = 2;
	uint32_t					instr;

	if (PSR64_IS_KERNEL(get_saved_state_cpsr(state))) {
		panic("ESR (0x%x) for SIMD trap from userland, actually came from kernel?", esr);
	}

	COPYIN(get_saved_state_pc(state), (char *)&instr, sizeof(instr));
	codes[1] = instr;

	exception_triage(exception, codes, numcodes);
}

void
sleh_irq(arm_saved_state_t *state)
{
	uint64_t     timestamp            = 0;
	uint32_t     old_entropy_data     = 0;
	uint32_t *   old_entropy_data_ptr = NULL;
	uint32_t *   new_entropy_data_ptr = NULL;
	cpu_data_t * cdp                  = getCpuDatap();

	sleh_interrupt_handler_prologue(state, DBG_INTR_TYPE_OTHER);

	/* Run the registered interrupt handler. */
	cdp->interrupt_handler(cdp->interrupt_target,
	                       cdp->interrupt_refCon,
	                       cdp->interrupt_nub,
	                       cdp->interrupt_source);

	/* We use interrupt timing as an entropy source. */
	timestamp = ml_get_timebase();

	/*
	 * The buffer index is subject to races, but as these races should only
	 * result in multiple CPUs updating the same location, the end result
	 * should be that noise gets written into the entropy buffer.  As this
	 * is the entire point of the entropy buffer, we will not worry about
	 * these races for now.
	 */
	old_entropy_data_ptr = EntropyData.index_ptr;
	new_entropy_data_ptr = old_entropy_data_ptr + 1;

	if (new_entropy_data_ptr >= &EntropyData.buffer[ENTROPY_BUFFER_SIZE]) {
		new_entropy_data_ptr = EntropyData.buffer;
	}

	EntropyData.index_ptr = new_entropy_data_ptr;

	/* Mix the timestamp data and the old data together. */
	old_entropy_data = *old_entropy_data_ptr;
	*old_entropy_data_ptr = (uint32_t)timestamp ^ __ror(old_entropy_data, 9);

	sleh_interrupt_handler_epilogue();
}

void
sleh_fiq(arm_saved_state_t *state)
{
	unsigned int type   = DBG_INTR_TYPE_UNKNOWN;
	if (ml_get_timer_pending()) {
		type = DBG_INTR_TYPE_TIMER;
	}

	sleh_interrupt_handler_prologue(state, type);

	{
		/*
		 * We don't know that this is a timer, but we don't have insight into
		 * the other interrupts that go down this path.
		 */


		cpu_data_t *cdp = getCpuDatap();

		cdp->cpu_decrementer = -1; /* Large */

		/*
		 * ARM64_TODO: whether we're coming from userland is ignored right now.
		 * We can easily thread it through, but not bothering for the
		 * moment (AArch32 doesn't either).
		 */
		rtclock_intr(TRUE);
	}

	sleh_interrupt_handler_epilogue();
}

void
sleh_serror(arm_context_t *context, uint32_t esr, vm_offset_t far)
{
	arm_saved_state_t 	  	*state = &context->ss;

	ASSERT_CONTEXT_SANITY(context);
	arm64_platform_error(state, esr, far);
}

void
mach_syscall_trace_exit(
			unsigned int retval,
			unsigned int call_number)
{
	KERNEL_DEBUG_CONSTANT_IST(KDEBUG_TRACE,
		MACHDBG_CODE(DBG_MACH_EXCP_SC, (call_number)) | DBG_FUNC_END,
		retval, 0, 0, 0, 0);
}

__attribute__((noreturn))
void
thread_syscall_return(kern_return_t error)
{
	thread_t thread;
	struct arm_saved_state *state;

	thread = current_thread();
	state = get_user_regs(thread);

	assert(is_saved_state64(state));
	saved_state64(state)->x[0] = error;

#if DEBUG || DEVELOPMENT
	kern_allocation_name_t
	prior __assert_only = thread_get_kernel_state(thread)->allocation_name;
	assertf(prior == NULL, "thread_set_allocation_name(\"%s\") not cleared", kern_allocation_get_name(prior));
#endif /* DEBUG || DEVELOPMENT */

	if (kdebug_enable) {
		/* Invert syscall number (negative for a mach syscall) */
		mach_syscall_trace_exit(error, (-1) * get_saved_state_svc_number(state));
	}

	thread_exception_return();
}

void
syscall_trace(
	      struct arm_saved_state * regs __unused)
{
	/* kprintf("syscall: %d\n", saved_state64(regs)->x[16]);  */
}

static void
sleh_interrupt_handler_prologue(arm_saved_state_t *state, unsigned int type)
{
	uint64_t     is_user = PSR64_IS_USER(get_saved_state_cpsr(state));

	uint64_t pc = is_user ? get_saved_state_pc(state) :
	              VM_KERNEL_UNSLIDE(get_saved_state_pc(state));

	KDBG_RELEASE(MACHDBG_CODE(DBG_MACH_EXCP_INTR, 0) | DBG_FUNC_START,
	             0, pc, is_user, type);

#if CONFIG_TELEMETRY
	if (telemetry_needs_record) {
		telemetry_mark_curthread((boolean_t)is_user);
	}
#endif /* CONFIG_TELEMETRY */
}

static void
sleh_interrupt_handler_epilogue(void)
{
	KDBG_RELEASE(MACHDBG_CODE(DBG_MACH_EXCP_INTR, 0) | DBG_FUNC_END);
}

void
sleh_invalid_stack(arm_context_t *context, uint32_t esr __unused, vm_offset_t far __unused)
{
	thread_t thread = current_thread();
	vm_offset_t kernel_stack_bottom, sp;

	sp = get_saved_state_sp(&context->ss);
	kernel_stack_bottom = round_page(thread->machine.kstackptr) - KERNEL_STACK_SIZE;

	if ((sp < kernel_stack_bottom) && (sp >= (kernel_stack_bottom - PAGE_SIZE))) {
		panic_with_thread_kernel_state("Invalid kernel stack pointer (probable overflow).", &context->ss);
	}

	panic_with_thread_kernel_state("Invalid kernel stack pointer (probable corruption).", &context->ss);
}

