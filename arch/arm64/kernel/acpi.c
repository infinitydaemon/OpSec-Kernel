// SPDX-License-Identifier: GPL-2.0-only
/*
 * ARM64 Specific Low-Level ACPI Boot Support
 *
 * Copyright (C) 2013-2014, Linaro Ltd.
 * Author: Al Stone <al.stone@linaro.org>
 * Author: Graeme Gregory <graeme.gregory@linaro.org>
 * Author: Hanjun Guo <hanjun.guo@linaro.org>
 * Author: Tomasz Nowicki <tomasz.nowicki@linaro.org>
 * Author: Naresh Bhat <naresh.bhat@linaro.org>
 */

#define pr_fmt(fmt) "ACPI: " fmt

#include <linux/acpi.h>
#include <linux/arm-smccc.h>
#include <linux/cpumask.h>
#include <linux/efi.h>
#include <linux/efi-bgrt.h>
#include <linux/init.h>
#include <linux/irq.h>
#include <linux/irqdomain.h>
#include <linux/irq_work.h>
#include <linux/memblock.h>
#include <linux/of_fdt.h>
#include <linux/libfdt.h>
#include <linux/smp.h>
#include <linux/serial_core.h>
#include <linux/pgtable.h>

#include <acpi/ghes.h>
#include <asm/cputype.h>
#include <asm/cpu_ops.h>
#include <asm/daifflags.h>
#include <asm/smp_plat.h>

int acpi_noirq = 1;      /* skip ACPI IRQ initialization */
int acpi_disabled = 1;
EXPORT_SYMBOL(acpi_disabled);

int acpi_pci_disabled = 1; /* skip ACPI PCI scan and IRQ initialization */
EXPORT_SYMBOL(acpi_pci_disabled);

static bool param_acpi_off __initdata;
static bool param_acpi_on __initdata;
static bool param_acpi_force __initdata;

static int __init parse_acpi(char *arg) {
    if (!arg)
        return -EINVAL;

    if (strcmp(arg, "off") == 0)
        param_acpi_off = true;
    else if (strcmp(arg, "on") == 0)
        param_acpi_on = true;
    else if (strcmp(arg, "force") == 0)
        param_acpi_force = true;
    else
        return -EINVAL;

    return 0;
}
early_param("acpi", parse_acpi);

static bool __init dt_is_stub(void) {
    int node;

    fdt_for_each_subnode(node, initial_boot_params, 0) {
        const char *name = fdt_get_name(initial_boot_params, node, NULL);
        if (strcmp(name, "chosen") == 0)
            continue;
        if (strcmp(name, "hypervisor") == 0 && of_flat_dt_is_compatible(node, "xen,xen"))
            continue;

        return false;
    }

    return true;
}

void __init __iomem *__acpi_map_table(unsigned long phys, unsigned long size) {
    if (!size)
        return NULL;

    return early_memremap(phys, size);
}

void __init __acpi_unmap_table(void __iomem *map, unsigned long size) {
    if (!map || !size)
        return;

    early_memunmap(map, size);
}

bool __init acpi_psci_present(void) {
    return acpi_gbl_FADT.arm_boot_flags & ACPI_FADT_PSCI_COMPLIANT;
}

bool acpi_psci_use_hvc(void) {
    return acpi_gbl_FADT.arm_boot_flags & ACPI_FADT_PSCI_USE_HVC;
}

static int __init acpi_fadt_sanity_check(void) {
    struct acpi_table_header *table;
    struct acpi_table_fadt *fadt;
    acpi_status status;
    int ret = 0;

    status = acpi_get_table(ACPI_SIG_FADT, 0, &table);
    if (ACPI_FAILURE(status)) {
        pr_err("Failed to get FADT table, %s\n", acpi_format_exception(status));
        return -ENODEV;
    }

    fadt = (struct acpi_table_fadt *)table;

    if (table->revision < 5 || (table->revision == 5 && fadt->minor_revision < 1)) {
        pr_err(FW_BUG "Unsupported FADT revision %d.%d, should be 5.1+\n", table->revision, fadt->minor_revision);
        if (!fadt->arm_boot_flags) {
            ret = -EINVAL;
            goto out;
        }
        pr_err("FADT has ARM boot flags set, assuming 5.1\n");
    }

    if (!(fadt->flags & ACPI_FADT_HW_REDUCED)) {
        pr_err("FADT not ACPI hardware reduced compliant\n");
        ret = -EINVAL;
    }

out:
    acpi_put_table(table);
    return ret;
}

void __init acpi_boot_table_init(void) {
    if (param_acpi_off || (!param_acpi_on && !param_acpi_force && !dt_is_stub()))
        goto done;

    enable_acpi();

    if (acpi_table_init() || acpi_fadt_sanity_check()) {
        pr_err("Failed to init ACPI tables\n");
        if (!param_acpi_force)
            disable_acpi();
    }

done:
    if (acpi_disabled) {
        if (earlycon_acpi_spcr_enable)
            early_init_dt_scan_chosen_stdout();
    } else {
        acpi_parse_spcr(earlycon_acpi_spcr_enable, true);
        if (IS_ENABLED(CONFIG_ACPI_BGRT))
            acpi_table_parse(ACPI_SIG_BGRT, acpi_parse_bgrt);
    }
}

static pgprot_t __acpi_get_writethrough_mem_attribute(void) {
    pr_warn_once("No MAIR allocation for EFI_MEMORY_WT; treating as Normal Non-cacheable\n");
    return __pgprot(PROT_NORMAL_NC);
}

pgprot_t __acpi_get_mem_attribute(phys_addr_t addr) {
    u64 attr = efi_mem_attributes(addr);

    if (attr & EFI_MEMORY_WB)
        return PAGE_KERNEL;
    if (attr & EFI_MEMORY_WC)
        return __pgprot(PROT_NORMAL_NC);
    if (attr & EFI_MEMORY_WT)
        return __acpi_get_writethrough_mem_attribute();
    return __pgprot(PROT_DEVICE_nGnRnE);
}

void __iomem *acpi_os_ioremap(acpi_physical_address phys, acpi_size size) {
    efi_memory_desc_t *md, *region = NULL;
    pgprot_t prot;

    if (WARN_ON_ONCE(!efi_enabled(EFI_MEMMAP)))
        return NULL;

    for_each_efi_memory_desc(md) {
        u64 end = md->phys_addr + (md->num_pages << EFI_PAGE_SHIFT);

        if (phys < md->phys_addr || phys >= end)
            continue;

        if (phys + size > end) {
            pr_warn(FW_BUG "requested region covers multiple EFI memory regions\n");
            return NULL;
        }
        region = md;
        break;
    }

    prot = __pgprot(PROT_DEVICE_nGnRnE);
    if (region) {
        switch (region->type) {
            case EFI_LOADER_CODE:
            case EFI_LOADER_DATA:
            case EFI_BOOT_SERVICES_CODE:
            case EFI_BOOT_SERVICES_DATA:
            case EFI_CONVENTIONAL_MEMORY:
            case EFI_PERSISTENT_MEMORY:
                if (memblock_is_map_memory(phys) || !memblock_is_region_memory(phys, size)) {
                    pr_warn(FW_BUG "requested region covers kernel memory @ %pa\n", &phys);
                    return NULL;
                }
                prot = PAGE_KERNEL_RO;
                break;
            case EFI_RUNTIME_SERVICES_CODE:
                prot = PAGE_KERNEL_RO;
                break;
            case EFI_ACPI_RECLAIM_MEMORY:
                if (memblock_is_map_memory(phys))
                    return (void __iomem *)__phys_to_virt(phys);
                prot = PAGE_KERNEL;
                break;
            default:
                if (region->attribute & EFI_MEMORY_WB)
                    prot = PAGE_KERNEL;
                else if (region->attribute & EFI_MEMORY_WC)
                    prot = __pgprot(PROT_NORMAL_NC);
                else if (region->attribute & EFI_MEMORY_WT)
                    prot = __acpi_get_writethrough_mem_attribute();
        }
    }
    return ioremap_prot(phys, size, pgprot_val(prot));
}

int apei_claim_sea(struct pt_regs *regs) {
    int err = -ENOENT;
    bool return_to_irqs_enabled;
    unsigned long current_flags;

    if (!IS_ENABLED(CONFIG_ACPI_APEI_GHES))
        return err;

    current_flags = local_daif_save_flags();
    return_to_irqs_enabled = !irqs_disabled_flags(arch_local_save_flags());

    if (regs)
        return_to_irqs_enabled = interrupts_enabled(regs);

    local_daif_restore(DAIF_ERRCTX);
    nmi_enter();
    err = ghes_notify_sea();
    nmi_exit();

    if (!err) {
        if (return_to_irqs_enabled) {
            local_daif_restore(DAIF_PROCCTX_NOIRQ);
            __irq_enter();
            irq_work_run();
            __irq_exit();
        } else {
            pr_warn_ratelimited("APEI work queued but not completed");
            err = -EINPROGRESS;
        }
    }

    local_daif_restore(current_flags);
    return err;
}

void arch_reserve_mem_area(acpi_physical_address addr, size_t size) {
    memblock_mark_nomap(addr, size);
}

#ifdef CONFIG_ACPI_FFH
struct acpi_ffh_data {
    struct acpi_ffh_info info;
    void (*invoke_ffh_fn)(unsigned long a0, unsigned long a1,
                          unsigned long a2, unsigned long a3,
                          unsigned long a4, unsigned long a5,
                          unsigned long a6, unsigned long a7,
                          struct arm_smccc_res *args,
                          struct arm_smccc_quirk *res);
    void (*invoke_ffh64_fn)(const struct arm_smccc_1_2_regs *args,
                            struct arm_smccc_1_2_regs *res);
};

int acpi_ffh_address_space_arch_setup(void *handler_ctxt, void **region_ctxt) {
    enum arm_smccc_conduit conduit;
    struct acpi_ffh_data *ffh_ctxt;

    if (arm_smccc_get_version() < ARM_SMCCC_VERSION_1_2)
        return -EOPNOTSUPP;

    conduit = arm_smccc_1_1_get_conduit();
    if (conduit == SMCCC_CONDUIT_NONE) {
        pr_err("%s: invalid SMCCC conduit\n", __func__);
        return -EOPNOTSUPP;
    }

    ffh_ctxt = kzalloc(sizeof(*ffh_ctxt), GFP_KERNEL);
    if (!ffh_ctxt)
        return -ENOMEM;

    if (conduit == SMCCC_CONDUIT_SMC) {
        ffh_ctxt->invoke_ffh_fn = __arm_smccc_smc;
        ffh_ctxt->invoke_ffh64_fn = arm_smccc_1_2_smc;
    } else {
        ffh_ctxt->invoke_ffh_fn = __arm_smccc_hvc;
        ffh_ctxt->invoke_ffh64_fn = arm_smccc_1_2_hvc;
    }

    memcpy(ffh_ctxt, handler_ctxt, sizeof(ffh_ctxt->info));

    *region_ctxt = ffh_ctxt;
    return AE_OK;
}

static bool acpi_ffh_smccc_owner_allowed(u32 fid) {
    int owner = ARM_SMCCC_OWNER_NUM(fid);

    return owner == ARM_SMCCC_OWNER_STANDARD ||
           owner == ARM_SMCCC_OWNER_SIP || owner == ARM_SMCCC_OWNER_OEM;
}

int acpi_ffh_address_space_arch_handler(acpi_integer *value, void *region_context) {
    int ret = 0;
    struct acpi_ffh_data *ffh_ctxt = region_context;

    if (ffh_ctxt->info.offset == 0) {
        struct arm_smccc_res res;
        u32 a[8] = {0}, *ptr = (u32 *)value;

        if (!ARM_SMCCC_IS_FAST_CALL(*ptr) || ARM_SMCCC_IS_64(*ptr) ||
            !acpi_ffh_smccc_owner_allowed(*ptr) ||
            ffh_ctxt->info.length > 32) {
            ret = AE_ERROR;
        } else {
            int idx, len = ffh_ctxt->info.length >> 2;

            for (idx = 0; idx < len; idx++)
                a[idx] = *(ptr + idx);

            ffh_ctxt->invoke_ffh_fn(a[0], a[1], a[2], a[3], a[4],
                                    a[5], a[6], a[7], &res, NULL);
            memcpy(value, &res, sizeof(res));
        }

    } else if (ffh_ctxt->info.offset == 1) {
        struct arm_smccc_1_2_regs *r = (struct arm_smccc_1_2_regs *)value;

        if (!ARM_SMCCC_IS_FAST_CALL(r->a0) || !ARM_SMCCC_IS_64(r->a0) ||
            !acpi_ffh_smccc_owner_allowed(r->a0) ||
            ffh_ctxt->info.length > sizeof(*r)) {
            ret = AE_ERROR;
        } else {
            ffh_ctxt->invoke_ffh64_fn(r, r);
            memcpy(value, r, ffh_ctxt->info.length);
        }
    } else {
        ret = AE_ERROR;
    }

    return ret;
}
#endif /* CONFIG_ACPI_FFH */
