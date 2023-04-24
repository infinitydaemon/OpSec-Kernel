/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * CPU reset routines
 *
 * Copyright (C) 2001 Deep Blue Solutions Ltd.
 * Copyright (C) 2012 ARM Ltd.
 * Copyright (C) 2015 Huawei Futurewei Technologies.
 * Copyright (C) 2023 CWD SYSTEMS.
 */

#include <linux/linkage.h>
#include <linux/cfi_types.h>
#include <asm/assembler.h>
#include <asm/sysreg.h>
#include <asm/virt.h>

/**
 * cpu_soft_restart() - Put the CPU into the same state as it would be if it had been reset,
 * and branch to what would be the reset vector.
 *
 * @el2_switch: Flag to indicate a switch to EL2 is needed.
 * @entry: Location to jump to for soft reset.
 * @arg0: First argument passed to @entry. (relocation list)
 * @arg1: Second argument passed to @entry.(physical kernel entry)
 * @arg2: Third argument passed to @entry. (physical dtb address)
 *
 * This function puts the CPU into the same state as it would be if it had been reset, and
 * branches to what would be the reset vector. It must be executed with the flat identity mapping.
 */
asmlinkage void cpu_soft_restart(bool el2_switch, void *entry, void *arg0, void *arg1, void *arg2)
{
	u64 sctlr_el1;
	void (*reset_vector)(void *arg0, void *arg1, void *arg2);

	/* Disable MMU */
	sctlr_el1 = read_sysreg_s(SYS_REG_SCTLR_EL1);
	write_sysreg_s(sctlr_el1 & ~SCTLR_M, SYS_REG_SCTLR_EL1);
	isb();

	/* Switch to EL2 if needed */
	if (el2_switch) {
		__asm__ __volatile__("hvc %x0" : : "r"((u32)HVC_SOFT_RESTART));
	}

	reset_vector = entry;
	reset_vector(arg0, arg1, arg2);
}
