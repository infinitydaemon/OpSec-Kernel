// SPDX-License-Identifier: GPL-2.0-only
/*
 * Uniprocessor-only support functions.  The counterpart to kernel/smp.c
 */

#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/smp.h>
#include <linux/hypervisor.h>

int smp_call_function_single(int cpu, void (*func)(void *info), void *info, int wait)
{
    if (cpu != 0)
        return -ENXIO;

    raw_local_irq_disable();
    func(info);
    raw_local_irq_enable();

    return 0;
}
EXPORT_SYMBOL(smp_call_function_single);

int smp_call_function_single_async(int cpu, struct __call_single_data *csd)
{
    if (cpu != 0)
        return -ENXIO;

    raw_local_irq_disable();
    csd->func(csd->info);
    raw_local_irq_enable();

    return 0;
}
EXPORT_SYMBOL(smp_call_function_single_async);

void on_each_cpu_cond_mask(smp_cond_func_t cond_func, smp_call_func_t func, void *info, bool wait, const struct cpumask *mask)
{
    unsigned long flags;

    preempt_disable();
    if ((!cond_func || cond_func(0, info)) && cpumask_test_cpu(0, mask)) {
        raw_local_irq_save(flags);
        func(info);
        raw_local_irq_restore(flags);
    }
    preempt_enable();
}
EXPORT_SYMBOL(on_each_cpu_cond_mask);

int smp_call_on_cpu(unsigned int cpu, int (*func)(void *), void *par, bool phys)
{
    int ret;

    if (cpu != 0)
        return -ENXIO;

    if (phys)
        hypervisor_pin_vcpu(0);

    ret = func(par);

    if (phys)
        hypervisor_pin_vcpu(-1);

    return ret;
}
EXPORT_SYMBOL_GPL(smp_call_on_cpu);

