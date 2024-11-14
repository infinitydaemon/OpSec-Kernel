// SPDX-License-Identifier: GPL-2.0-only
/*
 * Low-level idle sequences for CPU
 * 
 * This code provides an idle handler that puts the CPU in a low-power
 * state while waiting for an interrupt. It saves and restores the IRQ
 * context to ensure that the CPU can respond to interrupts appropriately.
 */

#include <linux/cpu.h>
#include <linux/irqflags.h>
#include <asm/barrier.h>
#include <asm/cpuidle.h>
#include <asm/cpufeature.h>
#include <asm/sysreg.h>

/*
 * cpu_do_idle
 *
 * Idle the processor (wait for interrupt).
 *
 * If the CPU supports priority masking, we must ensure that interrupts
 * are not masked at the PMR level; otherwise, the core will not wake up 
 * due to a blocked wakeup signal in the interrupt controller.
 */
void noinstr cpu_do_idle(void)
{
	struct arm_cpuidle_irq_context context;

	// Save the current interrupt context
	arm_cpuidle_save_irq_context(&context);

	// Ensure all memory accesses complete before idle
	dsb(sy);
	// Wait for an interrupt to resume processing
	wfi();

	// Restore the interrupt context after waking
	arm_cpuidle_restore_irq_context(&context);
}

/*
 * arch_cpu_idle - Default CPU idle handler
 *
 * This function calls cpu_do_idle(), which executes the necessary
 * steps to switch the CPU clock and enter an idle state waiting for an
 * interrupt. This is intended as the primary idle function for the architecture.
 */
void noinstr arch_cpu_idle(void)
{
	cpu_do_idle();
}
