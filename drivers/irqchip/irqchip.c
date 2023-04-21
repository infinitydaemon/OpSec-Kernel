#include <linux/acpi.h>
#include <linux/init.h>
#include <linux/of_device.h>
#include <linux/of_irq.h>
#include <linux/irqchip.h>
#include <linux/platform_device.h>

/* This special of_device_id is the sentinel at the end of the
 * of_device_id[] array of all irqchips. It is automatically placed at
 * the end of the array by the linker, thanks to being part of a
 * special section. */
static const struct of_device_id irqchip_of_match_end __used __section("__irqchip_of_table_end");

extern struct of_device_id __irqchip_of_table[];

/* Initialize the irqchip module */
void __init irqchip_init(void)
{
    of_irq_init(__irqchip_of_table);
    acpi_probe_device_table(irqchip);
}

/* Probe a platform device for an irqchip */
int platform_irqchip_probe(struct platform_device *pdev)
{
    struct device_node *np = pdev->dev.of_node;
    struct device_node *par_np = of_irq_find_parent(np);
    of_irq_init_cb_t irq_init_cb = of_device_get_match_data(&pdev->dev);
    int ret = 0;

    if (!irq_init_cb) {
        dev_err(&pdev->dev, "no irq_init_cb found\n");
        ret = -EINVAL;
        goto out_put_parent;
    }

    if (par_np == np) {
        par_np = NULL;
    }

    /* If there's a parent interrupt controller and none of the parent irq
     * domains have been registered, that means the parent interrupt
     * controller has not been initialized yet. It's not time for this
     * interrupt controller to initialize. So, defer probe of this
     * interrupt controller. The actual initialization callback of this
     * interrupt controller can check for specific domains as necessary. */
    if (par_np && !irq_find_matching_host(par_np, DOMAIN_BUS_ANY)) {
        dev_info(&pdev->dev, "deferring probe due to unregistered parent domains\n");
        ret = -EPROBE_DEFER;
        goto out_put_parent;
    }

    ret = irq_init_cb(np, par_np);

out_put_parent:
    of_node_put(par_np);
    return ret;
}
EXPORT_SYMBOL_GPL(platform_irqchip_probe);
