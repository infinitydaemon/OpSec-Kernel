// SPDX-License-Identifier: GPL-2.0-only
// Author: Ard Biesheuvel <ardb@google.com>

#include <linux/init.h>
#include <linux/libfdt.h>
#include <linux/linkage.h>
#include <linux/types.h>
#include <linux/sizes.h>
#include <linux/string.h>

#include <asm/memory.h>
#include <asm/pgalloc.h>
#include <asm/pgtable.h>
#include <asm/tlbflush.h>

#include "pi.h"

extern const u8 __eh_frame_start[], __eh_frame_end[];
extern void idmap_cpu_replace_ttbr1(void *pgdir);

static void __init map_segment(pgd_t *pg_dir, u64 *pgd, u64 va_offset,
			       void *start, void *end, pgprot_t prot,
			       bool may_use_cont, int root_level)
{
	map_range(pgd, ((u64)start + va_offset) & ~PAGE_OFFSET,
		  ((u64)end + va_offset) & ~PAGE_OFFSET, (u64)start,
		  prot, root_level, (pte_t *)pg_dir, may_use_cont, 0);
}

static void __init unmap_segment(pgd_t *pg_dir, u64 va_offset, void *start,
				 void *end, int root_level)
{
	map_segment(pg_dir, NULL, va_offset, start, end, __pgprot(0),
		    false, root_level);
}

static bool __init kernel_requires_shadow_scs(void)
{
	if (IS_ENABLED(CONFIG_ARM64_PTR_AUTH_KERNEL) && cpu_has_pac())
		return false;
	if (IS_ENABLED(CONFIG_ARM64_BTI_KERNEL) && cpu_has_bti())
		return false;
	return IS_ENABLED(CONFIG_UNWIND_PATCH_PAC_INTO_SCS);
}

static void __init set_text_protection(pgprot_t *text_prot)
{
	if (IS_ENABLED(CONFIG_ARM64_BTI_KERNEL) && cpu_has_bti())
		*text_prot = __pgprot_modify(*text_prot, PTE_GP, PTE_GP);

	if (arm64_test_sw_feature_override(ARM64_SW_FEATURE_OVERRIDE_RODATA_OFF))
		*text_prot = PAGE_KERNEL_EXEC;
}

static void __init map_kernel_memory(pgd_t *pg_dir, u64 va_offset,
				     pgprot_t text_prot, pgprot_t data_prot,
				     bool twopass, int root_level)
{
	map_segment(pg_dir, (u64 *)pg_dir + PAGE_SIZE, va_offset, _stext, _etext, twopass ? data_prot : text_prot,
		    !twopass, root_level);
	map_segment(pg_dir, (u64 *)pg_dir + PAGE_SIZE, va_offset, __start_rodata, __inittext_begin,
		    data_prot, false, root_level);
	map_segment(pg_dir, (u64 *)pg_dir + PAGE_SIZE, va_offset, __inittext_begin, __inittext_end,
		    twopass ? data_prot : text_prot, false, root_level);
	map_segment(pg_dir, (u64 *)pg_dir + PAGE_SIZE, va_offset, __initdata_begin, __initdata_end,
		    data_prot, false, root_level);
	map_segment(pg_dir, (u64 *)pg_dir + PAGE_SIZE, va_offset, _data, _end, data_prot, true, root_level);
}

static void __init finalize_mapping(pgd_t *pg_dir, u64 va_offset, bool enable_scs, bool twopass,
				    pgprot_t text_prot, int root_level)
{
	if (twopass) {
		if (IS_ENABLED(CONFIG_RELOCATABLE))
			relocate_kernel(va_offset);

		if (enable_scs) {
			scs_patch(__eh_frame_start + va_offset, __eh_frame_end - __eh_frame_start);
			asm("ic ialluis");
			dynamic_scs_is_enabled = true;
		}

		unmap_segment(pg_dir, va_offset, _stext, _etext, root_level);
		dsb(ishst);
		isb();
		__tlbi(vmalle1);
		isb();

		map_segment(pg_dir, NULL, va_offset, _stext, _etext, text_prot, true, root_level);
		map_segment(pg_dir, NULL, va_offset, __inittext_begin, __inittext_end, text_prot, false, root_level);
	}

	memcpy((void *)swapper_pg_dir + va_offset, pg_dir, PAGE_SIZE);
	dsb(ishst);
	idmap_cpu_replace_ttbr1(swapper_pg_dir);
}

static void __init remap_idmap_for_lpa2(void)
{
	pteval_t mask = PTE_SHARED;
	create_init_idmap(init_pg_dir, mask);
	dsb(ishst);
	set_ttbr0_for_lpa2((u64)init_pg_dir);

	memset(init_idmap_pg_dir, 0, (u64)init_idmap_pg_end - (u64)init_idmap_pg_dir);
	create_init_idmap(init_idmap_pg_dir, mask);
	dsb(ishst);

	set_ttbr0_for_lpa2((u64)init_idmap_pg_dir);
	memset(init_pg_dir, 0, (u64)init_pg_end - (u64)init_pg_dir);
}

static void __init map_fdt(u64 fdt)
{
	static u8 ptes[INIT_IDMAP_FDT_SIZE] __initdata __aligned(PAGE_SIZE);
	u64 efdt = fdt + MAX_FDT_SIZE;
	u64 ptep = (u64)ptes;

	map_range(&ptep, fdt, (u64)_text > fdt ? min((u64)_text, efdt) : efdt,
		  fdt, PAGE_KERNEL, IDMAP_ROOT_LEVEL,
		  (pte_t *)init_idmap_pg_dir, false, 0);
	dsb(ishst);
}

asmlinkage void __init early_map_kernel(u64 boot_status, void *fdt)
{
	static const char chosen_str[] __initconst = "/chosen";
	u64 va_base, pa_base = (u64)&_text;
	u64 kaslr_offset = pa_base % MIN_KIMG_ALIGN;
	int root_level = 4 - CONFIG_PGTABLE_LEVELS;
	int va_bits = VA_BITS;
	int chosen;

	map_fdt((u64)fdt);
	memset(__bss_start, 0, (u64)init_pg_end - (u64)__bss_start);

	chosen = fdt_path_offset(fdt, chosen_str);
	init_feature_override(boot_status, fdt, chosen);

	if (IS_ENABLED(CONFIG_ARM64_64K_PAGES) && !cpu_has_lva())
		va_bits = VA_BITS_MIN;
	else if (IS_ENABLED(CONFIG_ARM64_LPA2) && !cpu_has_lpa2()) {
		va_bits = VA_BITS_MIN;
		root_level++;
	}

	if (va_bits > VA_BITS_MIN)
		sysreg_clear_set(tcr_el1, TCR_T1SZ_MASK, TCR_T1SZ(va_bits));

	if (IS_ENABLED(CONFIG_RANDOMIZE_BASE)) {
		u64 kaslr_seed = kaslr_early_init(fdt, chosen);
		if (kaslr_seed && kaslr_requires_kpti())
			arm64_use_ng_mappings = true;
		kaslr_offset |= kaslr_seed & ~(MIN_KIMG_ALIGN - 1);
	}

	if (IS_ENABLED(CONFIG_ARM64_LPA2) && va_bits > VA_BITS_MIN)
		remap_idmap_for_lpa2();

	va_base = KIMAGE_VADDR + kaslr_offset;

	pgprot_t text_prot = PAGE_KERNEL_ROX;
	set_text_protection(&text_prot);
	bool enable_scs = kernel_requires_shadow_scs();
	map_kernel_memory(init_pg_dir, va_base - pa_base, text_prot, PAGE_KERNEL, enable_scs, root_level);
	finalize_mapping(init_pg_dir, va_base - pa_base, enable_scs, enable_scs, text_prot, root_level);
}
