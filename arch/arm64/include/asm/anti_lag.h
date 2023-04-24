#ifndef RPI_CPU_THROTTLE_H
#define RPI_CPU_THROTTLE_H

#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>

#define RPI4_CPU_MIN_FREQ 1200000

static struct platform_device *rpi4_cpufreq_device;

static int __init rpi4_cpu_throttle_init(void)
{
    struct device_node *np;
    u32 val;
    int ret;

    np = of_find_node_by_name(NULL, "cpufreq");
    if (!np) {
        pr_err("Failed to find cpufreq node\n");
        return -ENODEV;
    }

    ret = of_property_read_u32(np, "cpu_freq_max", &val);
    if (ret < 0) {
        pr_err("Failed to read cpu_freq_max property\n");
        of_node_put(np);
        return ret;
    }

    if (val >= RPI4_CPU_MIN_FREQ) {
        of_node_put(np);
        pr_info("CPU frequency is not throttled below %d kHz\n", RPI4_CPU_MIN_FREQ / 1000);
        return 0;
    }

    pr_info("Disabling CPU frequency throttling below %d kHz\n", RPI4_CPU_MIN_FREQ / 1000);
    ret = of_property_write_u32(np, "cpu_freq_min", RPI4_CPU_MIN_FREQ);
    if (ret < 0) {
        pr_err("Failed to write cpu_freq_min property\n");
        of_node_put(np);
        return ret;
    }

    of_node_put(np);
    msleep(1000);
    pr_info("CPU frequency throttle disabled\n");

    return 0;
}

static void __exit rpi4_cpu_throttle_exit(void)
{
    struct device_node *np;

    np = of_find_node_by_name(NULL, "cpufreq");
    if (!np) {
        pr_err("Failed to find cpufreq node\n");
        return;
    }

    of_property_delete(np, "cpu_freq_min");
    of_node_put(np);

    pr_info("CPU frequency throttle re-enabled\n");
}

module_init(rpi4_cpu_throttle_init);
module_exit(rpi4_cpu_throttle_exit);

MODULE_AUTHOR("Your Name Here");
MODULE_DESCRIPTION("Disables CPU frequency throttling below 1.2 GHz on Raspberry Pi 4");
MODULE_LICENSE("GPL");

#endif /* RPI_CPU_THROTTLE_H */
