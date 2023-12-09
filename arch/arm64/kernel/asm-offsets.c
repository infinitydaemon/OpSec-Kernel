// SPDX-License-Identifier: GPL-2.0-only
/*
 * Based on arch/arm/kernel/asm-offsets.c
 *
 * Copyright (C) 1995-2003 Russell King
 *               2001-2002 Keith Owens
 * Copyright (C) 2012 ARM Ltd.
 */

#include <linux/arm_sdei.h>
#include <linux/sched.h>
#include <linux/ftrace.h>
#include <linux/kexec.h>
#include <linux/mm.h>
#include <linux/dma-mapping.h>
#include <linux/kvm_host.h>
#include <linux/preempt.h>
#include <linux/suspend.h>
#include <asm/cpufeature.h>
#include <asm/fixmap.h>
#include <asm/thread_info.h>
#include <asm/memory.h>
#include <asm/signal32.h>
#include <asm/smp_plat.h>
#include <asm/suspend.h>
#include <linux/kbuild.h>
#include <linux/arm-smccc.h>

int main(void)
{
  // Define structure offsets with meaningful names
  DEFINE(TASK_ACTIVE_MM_OFFSET, offsetof(struct task_struct, active_mm));
  DEFINE(TASK_THREAD_INFO_CPU_OFFSET, offsetof(struct task_struct, thread_info.cpu));
  DEFINE(TASK_THREAD_INFO_FLAGS_OFFSET, offsetof(struct task_struct, thread_info.flags));
  DEFINE(TASK_THREAD_INFO_PREEMPT_OFFSET, offsetof(struct task_struct, thread_info.preempt_count));
#ifdef CONFIG_ARM64_SW_TTBR0_PAN
  DEFINE(TASK_THREAD_INFO_TTBR0_OFFSET, offsetof(struct task_struct, thread_info.ttbr0));
#endif
#ifdef CONFIG_SHADOW_CALL_STACK
  DEFINE(TASK_THREAD_INFO_SCS_BASE_OFFSET, offsetof(struct task_struct, thread_info.scs_base));
  DEFINE(TASK_THREAD_INFO_SCS_SP_OFFSET, offsetof(struct task_struct, thread_info.scs_sp));
#endif
  DEFINE(TASK_STACK_OFFSET, offsetof(struct task_struct, stack));
#ifdef CONFIG_STACKPROTECTOR
  DEFINE(TASK_STACK_CANARY_OFFSET, offsetof(struct task_struct, stack_canary));
#endif
  DEFINE(TASK_THREAD_CPU_CONTEXT_OFFSET, offsetof(struct task_struct, thread.cpu_context));
  DEFINE(TASK_THREAD_SCTLR_USER_OFFSET, offsetof(struct task_struct, thread.sctlr_user));
#ifdef CONFIG_ARM64_PTR_AUTH
  DEFINE(TASK_THREAD_KEYS_USER_OFFSET, offsetof(struct task_struct, thread.keys_user));
#endif
#ifdef CONFIG_ARM64_PTR_AUTH_KERNEL
  DEFINE(TASK_THREAD_KEYS_KERNEL_OFFSET, offsetof(struct task_struct, thread.keys_kernel));
#endif
#ifdef CONFIG_ARM64_MTE
  DEFINE(TASK_THREAD_MTE_CTRL_OFFSET, offsetof(struct task_struct, thread.mte_ctrl));
#endif
  DEFINE(PT_REGS_REGS_OFFSET, offsetof(struct pt_regs, regs));
  DEFINE(PT_REGS_SP_OFFSET, offsetof(struct pt_regs, sp));
  DEFINE(PT_REGS_PSTATE_OFFSET, offsetof(struct pt_regs, pstate));
  DEFINE(PT_REGS_PC_OFFSET, offsetof(struct pt_regs, pc));
  DEFINE(PT_REGS_SYSCALLNO_OFFSET, offsetof(struct pt_regs, syscallno));
  DEFINE(PT_REGS_SDEI_TTBR1_OFFSET, offsetof(struct pt_regs, sdei_ttbr1));
  DEFINE(PT_REGS_PMR_SAVE_OFFSET, offsetof(struct pt_regs, pmr_save));
  DEFINE(PT_REGS_STACKFRAME_OFFSET, offsetof(struct pt_regs, stackframe));
  DEFINE(PT_REGS_SIZE, sizeof(struct pt_regs));
#ifdef CONFIG_DYNAMIC_FTRACE_WITH_ARGS
  DEFINE(FTRACE_REGS_REGS_OFFSET, offsetof(struct ftrace_regs, regs));
  DEFINE(FTRACE_REGS_FP_OFFSET, offsetof(struct ftrace_regs, fp));
  DEFINE(FTRACE_REGS_LR_OFFSET, offsetof(struct ftrace_regs, lr));
  DEFINE(FTRACE_REGS_SP_OFFSET, offsetof(struct ftrace_regs, sp));
  DEFINE(FTRACE_REGS_PC_OFFSET, offsetof(struct ftrace_regs, pc));
#ifdef CONFIG_DYNAMIC_FTRACE_WITH_DIRECT_CALLS
  DEFINE(FTRACE_REGS_DIRECT_TRAMP_OFFSET, offsetof(struct ftrace_regs, direct_tramp));
#endif
  DEFINE(FTRACE_REGS_SIZE, sizeof(struct ftrace_regs));
#endif
#ifdef CONFIG_COMPAT
  DEFINE(COMPAT_SIGFRAME_REGS_OFFSET, offsetof(struct compat_sigframe, uc.uc_mcontext.arm_r0));
  DEFINE(COMPAT_RT_SIGFRAME_REGS_OFFSET, offsetof(struct compat_rt_sigframe, sig.uc.uc_mcontext.arm_r0));
#endif
  DEFINE(MM_CONTEXT_ID_OFFSET, offsetof(struct mm_struct, context.id.counter));
  DEFINE(VMA_VM_MM_OFFSET, offsetof(struct vm_area_struct, vm_mm));
  DEFINE(VMA_VM_FLAGS_OFFSET, offsetof(struct vm_area_struct, vm_flags));
  DEFINE(VM_EXEC, VM_EXEC);
  DEFINE(PAGE_SZ, PAGE_SIZE);
  DEFINE(DMA_TO_DEVICE, DMA_TO_DEVICE);
  DEFINE(DMA_FROM_DEVICE, DMA_FROM_DEVICE);
  DEFINE(PREEMPT_DISABLE_OFFSET, PREEMPT_DISABLE_OFFSET);
  DEFINE(SOFTIRQ_SHIFT, SOFTIRQ_SHIFT);
  DEFINE(IRQ_CPUSTAT_SOFTIRQ_PENDING_OFFSET, offsetof(irq_cpustat_t, __softirq_pending));
  DEFINE(CPU_BOOT_TASK_OFFSET, offsetof(struct secondary_data, task));
  DEFINE(FTR_OVR_VAL_OFFSET, offsetof(struct arm64_ftr_override, val));
  DEFINE(FTR_OVR_MASK_OFFSET, offsetof(struct arm64_ftr_override, mask));
#ifdef CONFIG_KVM
  DEFINE(VCPU_CONTEXT_OFFSET, offsetof(struct kvm_vcpu, arch.ctxt));
  DEFINE(VCPU_FAULT_DISR_OFFSET, offsetof(struct kvm_vcpu, arch.fault.disr_el1));
  DEFINE(VCPU_HCR_EL2_OFFSET, offsetof(struct kvm_vcpu, arch.hcr_el2));
  DEFINE(CPU_USER_PT_REGS_OFFSET, offsetof(struct kvm_cpu_context, regs));
  DEFINE(CPU_RGSR_EL1_OFFSET, offsetof(struct kvm_cpu_context, sys_regs[RGSR_EL1]));
  DEFINE(CPU_GCR_EL1_OFFSET, offsetof(struct kvm_cpu_context, sys_regs[GCR_EL1]));
  DEFINE(CPU_APIAKEYLO_EL1_OFFSET, offsetof(struct kvm_cpu_context, sys_regs[APIAKEYLO_EL1]));
  DEFINE(CPU_APIBKEYLO_EL1_OFFSET, offsetof(struct kvm_cpu_context, sys_regs[APIBKEYLO_EL1]));
  DEFINE(CPU_APDAKEYLO_EL1_OFFSET, offsetof(struct kvm_cpu_context, sys_regs[APDAKEYLO_EL1]));
  DEFINE(CPU_APDBKEYLO_EL1_OFFSET, offsetof(struct kvm_cpu_context, sys_regs[APDBKEYLO_EL1]));
  DEFINE(CPU_APGAKEYLO_EL1_OFFSET, offsetof(struct kvm_cpu_context, sys_regs[APGAKEYLO_EL1]));
  DEFINE(HOST_CONTEXT_VCPU_OFFSET, offsetof(struct kvm_cpu_context, __hyp_running_vcpu));
  DEFINE(HOST_DATA_CONTEXT_OFFSET, offsetof(struct kvm_host_data, host_ctxt));
  DEFINE(NVHE_INIT_MAIR_EL2_OFFSET, offsetof(struct kvm_nvhe_init_params, mair_el2));
  DEFINE(NVHE_INIT_TCR_EL2_OFFSET, offsetof(struct kvm_nvhe_init_params, tcr_el2));
  DEFINE(NVHE_INIT_TPIDR_EL2_OFFSET, offsetof(struct kvm_nvhe_init_params, tpidr_el2));
  DEFINE(NVHE_INIT_STACK_HYP_VA_OFFSET, offsetof(struct kvm_nvhe_init_params, stack_hyp_va));
  DEFINE(NVHE_INIT_PGD_PA_OFFSET, offsetof(struct kvm_nvhe_init_params, pgd_pa));
  DEFINE(NVHE_INIT_HCR_EL2_OFFSET, offsetof(struct kvm_nvhe_init_params, hcr_el2));
  DEFINE(NVHE_INIT_VTTBR_OFFSET, offsetof(struct kvm_nvhe_init_params, vttbr));
  DEFINE(NVHE_INIT_VTCR_OFFSET, offsetof(struct kvm_nvhe_init_params, vtcr));
#endif
#ifdef CONFIG_CPU_PM
  DEFINE(CPU_CTX_SP_OFFSET, offsetof(struct cpu_suspend_ctx, sp));
  DEFINE(MPIDR_HASH_MASK_OFFSET, offsetof(struct mpidr_hash, mask));
  DEFINE(MPIDR_HASH_SHIFTS_OFFSET, offsetof(struct mpidr_hash, shift_aff));
  DEFINE(SLEEP_STACK_DATA_SYSTEM_REGS_OFFSET, offsetof(struct sleep_stack_data, system_regs));
  DEFINE(SLEEP_STACK_DATA_CALLEE_REGS_OFFSET, offsetof(struct sleep_stack_data, callee_saved_regs));
#endif
  DEFINE(ARM_SMCCC_RES_X0_OFFSET, offsetof(struct arm_smccc_res, a0));
  DEFINE(ARM_SMCCC_RES_X2_OFFSET, offsetof(struct arm_smccc_res, a2));
  DEFINE(ARM_SMCCC_QUIRK_ID_OFFSET, offsetof(struct arm_smccc_quirk, id));
  DEFINE(ARM_SMCCC_QUIRK_STATE_OFFSET, offsetof(struct arm_smccc_quirk, state));
  DEFINE(ARM_SMCCC_1_2_REGS_X0_OFFSET, offsetof(struct arm_smccc_1_2_regs, a0));
  DEFINE(ARM_SMCCC_1_2_REGS_X2_OFFSET, offsetof(struct arm_smccc_1_2_regs, a2));
  DEFINE(ARM_SMCCC_1_2_REGS_X4_OFFSET, offsetof(struct arm_smccc_1_2_regs, a4));
  DEFINE(ARM_SMCCC_1_2_REGS_X6_OFFSET, offsetof(struct arm_smccc_1_2_regs, a6));
  DEFINE(ARM_SMCCC_1_2_REGS_X8_OFFSET, offsetof(struct arm_smccc_1_2_regs, a8));
  DEFINE(ARM_SMCCC_1_2_REGS_X10_OFFSET, offsetof(struct arm_smccc_1_2_regs, a10));
  DEFINE(ARM_SMCCC_1_2_REGS_X12_OFFSET, offsetof(struct arm_smccc_1_2_regs, a12));
  DEFINE(ARM_SMCCC_1_2_REGS_X14_OFFSET, offsetof(struct arm_smccc_1_2_regs, a14));
  DEFINE(ARM_SMCCC_1_2_REGS_X16_OFFSET, offsetof(struct arm_smccc_1_2_regs, a16));
  DEFINE(HIBERN_PBE_ORIG_OFFSET, offsetof(struct pbe, orig_address));
  DEFINE(HIBERN_PBE_ADDR_OFFSET, offsetof(struct pbe, address));
  DEFINE(HIBERN_PBE_NEXT_OFFSET, offsetof(struct pbe, next));
  DEFINE(ARM64_FTR_SYSVAL_OFFSET, offsetof(struct arm64_ftr_reg, sys_val));
#ifdef CONFIG_UNMAP_KERNEL_AT_EL0
  DEFINE(TRAMP_VALIAS_OFFSET, TRAMP_VALIAS);
#endif
#ifdef CONFIG_ARM_SDE_INTERFACE
  DEFINE(SDEI_EVENT_INTREGS_OFFSET, offsetof(struct sdei_registered_event, interrupted_regs));
  DEFINE(SDEI_EVENT_PRIORITY_OFFSET, offsetof(struct sdei_registered_event, priority));
#endif
#ifdef CONFIG_ARM64_PTR_AUTH
  DEFINE(PTRAUTH_USER_KEY_APIA_OFFSET, offsetof(struct ptrauth_keys_user, apia));
#ifdef CONFIG_ARM64_PTR_AUTH_KERNEL
  DEFINE(PTRAUTH_KERNEL_KEY_APIA_OFFSET, offsetof(struct ptrauth_keys_kernel, apia));
#endif
#endif
#ifdef CONFIG_KEXEC_CORE
  DEFINE(KIMAGE_ARCH_DTB_MEM_OFFSET, offsetof(struct kimage, arch.dtb_mem));
  DEFINE(KIMAGE_ARCH_EL2_VECTORS_OFFSET, offsetof(struct kimage, arch.el2_vectors));
  DEFINE(KIMAGE_ARCH_ZERO_PAGE_OFFSET, offsetof(struct kimage, arch.zero_page));
  DEFINE(KIMAGE_ARCH_PHYS_OFFSET_OFFSET, offsetof(struct kimage, arch.phys_offset));
  DEFINE(KIMAGE_ARCH_TTBR1_OFFSET, offsetof(struct kimage, arch.ttbr1));
  DEFINE(KIMAGE_HEAD_OFFSET, offsetof(struct kimage, head));
  DEFINE(KIMAGE_START_OFFSET, offsetof(struct kimage, start));
#endif
#ifdef CONFIG_FUNCTION_TRACER
  DEFINE(FTRACE_OPS_FUNC_OFFSET, offsetof(struct ftrace_ops, func));
#endif
#ifdef CONFIG_FUNCTION_GRAPH_TRACER
  DEFINE(FGRET_REGS_X0_OFFSET, offsetof(struct fgraph_ret_regs, regs[0]));
  DEFINE(FGRET_REGS_X1_OFFSET, offsetof(struct fgraph_ret_regs, regs[1]));
  DEFINE(FGRET_REGS_X2_OFFSET, offsetof(struct fgraph_ret_regs, regs[2]));
  DEFINE(FGRET_REGS_X3_OFFSET, offsetof(struct fgraph_ret_regs, regs[3]));
  DEFINE(FGRET_REGS_X4_OFFSET, offsetof(struct fgraph_ret_regs, regs[4]));
  DEFINE(FGRET_REGS_X5_OFFSET, offsetof(struct fgraph_ret_regs, regs[5]));
  DEFINE(FGRET_REGS_X6_OFFSET, offsetof(struct fgraph_ret_regs, regs[6]));
  DEFINE(FGRET_REGS_X7_OFFSET, offsetof(struct fgraph_ret_regs, regs[7]));
  DEFINE(FGRET_REGS_FP_OFFSET, offsetof(struct fgraph_ret_regs, fp));
  DEFINE(FGRET_REGS_SIZE, sizeof(struct fgraph_ret_regs));
#endif
#ifdef CONFIG_DYNAMIC_FTRACE_WITH_DIRECT_CALLS
  DEFINE(FTRACE_OPS_DIRECT_CALL_OFFSET, offsetof(struct ftrace_ops, direct_call));
#endif

  return 0;
}
