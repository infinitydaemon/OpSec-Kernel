// SPDX-License-Identifier: GPL-2.0-only
/*
 * CPU kernel entry/exit control
 *
 * Copyright (C) 2013 ARM Ltd.
 */

#include <linux/acpi.h>
#include <linux/cache.h>
#include <linux/errno.h>
#include <linux/of.h>
#include <linux/string.h>
#include <asm/acpi.h>
#include <asm/cpu_ops.h>
#include <asm/smp_plat.h>

// External declarations of CPU operation structures
extern const struct cpu_operations smp_spin_table_ops;
#ifdef CONFIG_ARM64_ACPI_PARKING_PROTOCOL
extern const struct cpu_operations acpi_parking_protocol_ops;
#endif
extern const struct cpu_operations cpu_psci_ops;

// Array to store CPU operations for each CPU
static const struct cpu_operations *cpu_ops[NR_CPUS] __ro_after_init;

// Arrays defining supported CPU operations for device tree and ACPI
static const struct cpu_operations *const dt_supported_cpu_ops[] __initconst = {
    &smp_spin_table_ops,
    &cpu_psci_ops,
    NULL,
};

static const struct cpu_operations *const acpi_supported_cpu_ops[] __initconst = {
#ifdef CONFIG_ARM64_ACPI_PARKING_PROTOCOL
    &acpi_parking_protocol_ops,
#endif
    &cpu_psci_ops,
    NULL,
};

// Function to obtain CPU operations based on the specified method
static const struct cpu_operations * __init cpu_get_ops(const char *name) {
    const struct cpu_operations *const *ops;

    ops = acpi_disabled ? dt_supported_cpu_ops : acpi_supported_cpu_ops;

    while (*ops) {
        if (!strcmp(name, (*ops)->name))
            return *ops;

        ops++;
    }

    return NULL;
}

// Function to read a CPU's enable method
static const char *__init cpu_read_enable_method(int cpu) {
    const char *enable_method;

    if (acpi_disabled) {
        struct device_node *dn = of_get_cpu_node(cpu, NULL);

        if (!dn) {
            if (!cpu)
                pr_err("Failed to find device node for boot cpu\n");
            return NULL;
        }

        enable_method = of_get_property(dn, "enable-method", NULL);
        if (!enable_method && cpu != 0)
            pr_err("%pOF: missing enable-method property\n", dn);

        of_node_put(dn);
    } else {
        enable_method = acpi_get_enable_method(cpu);
        if (!enable_method && cpu != 0)
            pr_err("Unsupported ACPI enable-method\n");
    }

    return enable_method;
}

// Function to initialize CPU operations
int __init init_cpu_ops(int cpu) {
    const char *enable_method = cpu_read_enable_method(cpu);

    if (!enable_method)
        return -ENODEV;

    cpu_ops[cpu] = cpu_get_ops(enable_method);
    if (!cpu_ops[cpu]) {
        pr_warn("Unsupported enable-method: %s\n", enable_method);
        return -EOPNOTSUPP;
    }

    return 0;
}

// Function to retrieve CPU operations for a specific CPU
const struct cpu_operations *get_cpu_ops(int cpu) {
    return cpu_ops[cpu];
}
