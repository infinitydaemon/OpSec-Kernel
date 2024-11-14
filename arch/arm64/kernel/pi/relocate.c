// SPDX-License-Identifier: GPL-2.0-only
// Copyright 2023 Google LLC
// Authors: Ard Biesheuvel <ardb@google.com>
//          Peter Collingbourne <pcc@google.com>

#include <linux/elf.h>
#include <linux/init.h>
#include <linux/types.h>

#include "pi.h"

extern const Elf64_Rela rela_start[], rela_end[];
extern const u64 relr_start[], relr_end[];

void __init relocate_kernel(u64 offset)
{
	if (!offset)
		return;

	// Process RELA relocations (absolute, addend-adjusted relocations)
	for (const Elf64_Rela *rela = rela_start; rela < rela_end; ++rela) {
		if (ELF64_R_TYPE(rela->r_info) == R_AARCH64_RELATIVE) {
			u64 *target = (u64 *)(rela->r_offset + offset);
			*target = rela->r_addend + offset;
		}
	}

	if (!IS_ENABLED(CONFIG_RELR))
		return;

	// Apply RELR relocations (compressed, bitmap-based relocations)
	u64 *place = NULL;
	for (const u64 *relr = relr_start; relr < relr_end; ++relr) {
		if ((*relr & 1) == 0) {
			// Base address for next relocations
			place = (u64 *)(*relr + offset);
			*place += offset;
			++place;
		} else {
			// Process bitmap for 63 relocations after base address
			for (u64 bitmap = *relr >> 1; bitmap; bitmap >>= 1, ++place) {
				if (bitmap & 1)
					*place += offset;
			}
		}
	}
}
