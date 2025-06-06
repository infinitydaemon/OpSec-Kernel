// SPDX-License-Identifier: GPL-2.0+
/*
 * GPIO driver wrapping PWM API
 *
 * PWM 0% and PWM 100% are equivalent to digital GPIO
 * outputs, and there are times where it is useful to use
 * PWM outputs as straight GPIOs (eg outputs of NXP PCA9685
 * I2C PWM chip). This driver wraps the PWM API as a GPIO
 * controller.
 *
 * Copyright (C) 2021 Raspberry Pi (Trading) Ltd.
 */

#include <linux/err.h>
#include <linux/gpio/driver.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pwm.h>

struct pwm_gpio {
	struct gpio_chip gc;
	struct pwm_device **pwm;
};

static int pwm_gpio_get_direction(struct gpio_chip *gc, unsigned int off)
{
	return GPIO_LINE_DIRECTION_OUT;
}

static void pwm_gpio_set(struct gpio_chip *gc, unsigned int off, int val)
{
	struct pwm_gpio *pwm_gpio = gpiochip_get_data(gc);
	struct pwm_state state;

	pwm_get_state(pwm_gpio->pwm[off], &state);
	state.duty_cycle = val ? state.period : 0;
	pwm_apply_might_sleep(pwm_gpio->pwm[off], &state);
}

static int pwm_gpio_parse_dt(struct pwm_gpio *pwm_gpio,
			     struct device *dev)
{
	struct device_node *node = dev->of_node;
	struct pwm_state state;
	int ret = 0, i, num_gpios;
	const char *pwm_name;

	if (!node)
		return -ENODEV;

	num_gpios = of_property_count_strings(node, "pwm-names");
	if (num_gpios <= 0)
		return 0;

	pwm_gpio->pwm = devm_kzalloc(dev,
				     sizeof(*pwm_gpio->pwm) * num_gpios,
				     GFP_KERNEL);
	if (!pwm_gpio->pwm)
		return -ENOMEM;

	for (i = 0; i < num_gpios; i++) {
		ret = of_property_read_string_index(node, "pwm-names", i,
						    &pwm_name);
		if (ret) {
			dev_err(dev, "unable to get pwm device index %d, name %s",
				i, pwm_name);
			goto error;
		}

		pwm_gpio->pwm[i] = devm_pwm_get(dev, pwm_name);
		if (IS_ERR(pwm_gpio->pwm[i])) {
			ret = PTR_ERR(pwm_gpio->pwm[i]);
			if (ret != -EPROBE_DEFER)
				dev_err(dev, "unable to request PWM\n");
			goto error;
		}

		/* Sync up PWM state. */
		pwm_init_state(pwm_gpio->pwm[i], &state);

		state.duty_cycle = 0;
		pwm_apply_might_sleep(pwm_gpio->pwm[i], &state);
	}

	pwm_gpio->gc.ngpio = num_gpios;

error:
	return ret;
}

static int pwm_gpio_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct pwm_gpio *pwm_gpio;
	int ret;

	pwm_gpio = devm_kzalloc(dev, sizeof(*pwm_gpio), GFP_KERNEL);
	if (!pwm_gpio)
		return -ENOMEM;

	pwm_gpio->gc.parent = dev;
	pwm_gpio->gc.label = "pwm-gpio";
	pwm_gpio->gc.owner = THIS_MODULE;
	pwm_gpio->gc.fwnode = dev->fwnode;
	pwm_gpio->gc.base = -1;

	pwm_gpio->gc.get_direction = pwm_gpio_get_direction;
	pwm_gpio->gc.set = pwm_gpio_set;
	pwm_gpio->gc.can_sleep = true;

	ret = pwm_gpio_parse_dt(pwm_gpio, dev);
	if (ret)
		return ret;

	if (!pwm_gpio->gc.ngpio)
		return 0;

	return devm_gpiochip_add_data(dev, &pwm_gpio->gc, pwm_gpio);
}

static void pwm_gpio_remove(struct platform_device *pdev)
{
}

static const struct of_device_id pwm_gpio_of_match[] = {
	{ .compatible = "pwm-gpio" },
	{ }
};
MODULE_DEVICE_TABLE(of, pwm_gpio_of_match);

static struct platform_driver pwm_gpio_driver = {
	.driver	= {
		.name		= "pwm-gpio",
		.of_match_table	= of_match_ptr(pwm_gpio_of_match),
	},
	.probe	= pwm_gpio_probe,
	.remove	= pwm_gpio_remove,
};
module_platform_driver(pwm_gpio_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Dave Stevenson <dave.stevenson@raspberrypi.com>");
MODULE_DESCRIPTION("PWM GPIO driver");
