// SPDX-License-Identifier: GPL-2.0-only
/*
 * Raspberry Pi PIO PWM.
 *
 * Copyright (C) 2024 Raspberry Pi Ltd.
 *
 * Author: Phil Elwell (phil@raspberrypi.com)
 *
 * Based on the pwm-rp1 driver by:
 *   Naushir Patuck <naush@raspberrypi.com>
 * and on the pwm-gpio driver by:
 *   Vincent Whitchurch <vincent.whitchurch@axis.com>
 */

#include <linux/err.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/pio_rp1.h>
#include <linux/platform_device.h>
#include <linux/pwm.h>

struct pwm_pio_rp1 {
	struct pwm_chip chip;
	struct device *dev;
	struct gpio_desc *gpiod;
	struct mutex mutex;
	PIO pio;
	uint sm;
	uint offset;
	uint gpio;
	uint32_t period;		/* In SM cycles */
	uint32_t duty_cycle;		/* In SM cycles */
	enum pwm_polarity polarity;
	bool enabled;
};

/* Generated from pwm.pio by pioasm */
#define pwm_wrap_target 0
#define pwm_wrap 6
#define pwm_loop_ticks 3

static const uint16_t pwm_program_instructions[] = {
		//     .wrap_target
	0x9080, //  0: pull   noblock         side 0
	0xa027, //  1: mov    x, osr
	0xa046, //  2: mov    y, isr
	0x00a5, //  3: jmp    x != y, 5
	0x1806, //  4: jmp    6               side 1
	0xa042, //  5: nop
	0x0083, //  6: jmp    y--, 3
		//     .wrap
};

static const struct pio_program pwm_program = {
	.instructions = pwm_program_instructions,
	.length = 7,
	.origin = -1,
};

static unsigned int pwm_pio_resolution __read_mostly;

static inline pio_sm_config pwm_program_get_default_config(uint offset)
{
	pio_sm_config c = pio_get_default_sm_config();

	sm_config_set_wrap(&c, offset + pwm_wrap_target, offset + pwm_wrap);
	sm_config_set_sideset(&c, 2, true, false);
	return c;
}

static inline void pwm_program_init(PIO pio, uint sm, uint offset, uint pin)
{
	pio_gpio_init(pio, pin);

	pio_sm_set_consecutive_pindirs(pio, sm, pin, 1, true);
	pio_sm_config c = pwm_program_get_default_config(offset);

	sm_config_set_sideset_pins(&c, pin);
	pio_sm_init(pio, sm, offset, &c);
}

/* Write `period` to the input shift register - must be disabled */
static void pio_pwm_set_period(PIO pio, uint sm, uint32_t period)
{
	pio_sm_put_blocking(pio, sm, period);
	pio_sm_exec(pio, sm, pio_encode_pull(false, false));
	pio_sm_exec(pio, sm, pio_encode_out(pio_isr, 32));
}

/* Write `level` to TX FIFO. State machine will copy this into X. */
static void pio_pwm_set_level(PIO pio, uint sm, uint32_t level)
{
	pio_sm_put_blocking(pio, sm, level);
}

static int pwm_pio_rp1_apply(struct pwm_chip *chip, struct pwm_device *pwm,
			  const struct pwm_state *state)
{
	struct pwm_pio_rp1 *ppwm = container_of(chip, struct pwm_pio_rp1, chip);
	uint32_t new_duty_cycle;
	uint32_t new_period;

	if (state->duty_cycle && state->duty_cycle < pwm_pio_resolution)
		return -EINVAL;

	if (state->duty_cycle != state->period &&
	    (state->period - state->duty_cycle < pwm_pio_resolution))
		return -EINVAL;

	new_period = state->period / pwm_pio_resolution;
	new_duty_cycle = state->duty_cycle / pwm_pio_resolution;

	mutex_lock(&ppwm->mutex);

	if ((ppwm->enabled && !state->enabled) || new_period != ppwm->period) {
		pio_sm_set_enabled(ppwm->pio, ppwm->sm, false);
		ppwm->enabled = false;
	}

	if (new_period != ppwm->period) {
		pio_pwm_set_period(ppwm->pio, ppwm->sm, new_period);
		ppwm->period = new_period;
	}

	if (state->enabled && new_duty_cycle != ppwm->duty_cycle) {
		pio_pwm_set_level(ppwm->pio, ppwm->sm, new_duty_cycle);
		ppwm->duty_cycle = new_duty_cycle;
	}

	if (state->polarity != ppwm->polarity) {
		pio_gpio_set_outover(ppwm->pio, ppwm->gpio,
			(state->polarity == PWM_POLARITY_INVERSED) ?
			GPIO_OVERRIDE_INVERT : GPIO_OVERRIDE_NORMAL);
		ppwm->polarity = state->polarity;
	}

	if (!ppwm->enabled && state->enabled) {
		pio_sm_set_enabled(ppwm->pio, ppwm->sm, true);
		ppwm->enabled = true;
	}

	mutex_unlock(&ppwm->mutex);

	return 0;
}

static const struct pwm_ops pwm_pio_rp1_ops = {
	.apply = pwm_pio_rp1_apply,
};

static int pwm_pio_rp1_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct of_phandle_args of_args = { 0 };
	struct device *dev = &pdev->dev;
	struct pwm_pio_rp1 *ppwm;
	struct pwm_chip *chip;
	bool is_rp1;

	chip = devm_pwmchip_alloc(dev, 1, sizeof(*ppwm));
	if (IS_ERR(chip))
		return PTR_ERR(chip);

	ppwm = pwmchip_get_drvdata(chip);

	mutex_init(&ppwm->mutex);

	ppwm->gpiod = devm_gpiod_get(dev, NULL, GPIOD_ASIS);
	/* Need to check that this is an RP1 GPIO in the first bank, and retrieve the offset */
	/* Unfortunately I think this has to be done by parsing the gpios property */
	if (IS_ERR(ppwm->gpiod))
		return dev_err_probe(dev, PTR_ERR(ppwm->gpiod),
				     "could not get a gpio\n");

	/* This really shouldn't fail, given that we have a gpiod */
	if (of_parse_phandle_with_args(np, "gpios", "#gpio-cells", 0, &of_args))
		return dev_err_probe(dev, -EINVAL,
				     "can't find gpio declaration\n");

	is_rp1 = of_device_is_compatible(of_args.np, "raspberrypi,rp1-gpio");
	of_node_put(of_args.np);
	if (!is_rp1 || of_args.args_count != 2)
		return dev_err_probe(dev, -EINVAL,
				     "not an RP1 gpio\n");

	ppwm->gpio = of_args.args[0];

	ppwm->pio = pio_open();
	if (IS_ERR(ppwm->pio))
		return dev_err_probe(dev, PTR_ERR(ppwm->pio),
				     "%pfw: could not open PIO\n",
				     dev_fwnode(dev));

	ppwm->sm = pio_claim_unused_sm(ppwm->pio, false);
	if ((int)ppwm->sm < 0) {
		pio_close(ppwm->pio);
		return dev_err_probe(dev, -EBUSY,
				     "%pfw: no free PIO SM\n",
				     dev_fwnode(dev));
	}

	ppwm->offset = pio_add_program(ppwm->pio, &pwm_program);
	if (ppwm->offset == PIO_ORIGIN_ANY) {
		pio_close(ppwm->pio);
		return dev_err_probe(dev, -EBUSY,
				     "%pfw: not enough PIO program space\n",
				     dev_fwnode(dev));
	}

	pwm_program_init(ppwm->pio, ppwm->sm, ppwm->offset, ppwm->gpio);

	pwm_pio_resolution = (1000u * 1000 * 1000 * pwm_loop_ticks) / clock_get_hz(clk_sys);

	chip->ops = &pwm_pio_rp1_ops;
	chip->atomic = true;
	chip->npwm = 1;

	platform_set_drvdata(pdev, ppwm);

	return devm_pwmchip_add(dev, chip);
}

static void pwm_pio_rp1_remove(struct platform_device *pdev)
{
	struct pwm_pio_rp1 *ppwm = platform_get_drvdata(pdev);

	pio_close(ppwm->pio);
}

static const struct of_device_id pwm_pio_rp1_dt_ids[] = {
	{ .compatible = "raspberrypi,pwm-pio-rp1" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, pwm_pio_rp1_dt_ids);

static struct platform_driver pwm_pio_rp1_driver = {
	.driver = {
		.name = "pwm-pio-rp1",
		.of_match_table = pwm_pio_rp1_dt_ids,
	},
	.probe = pwm_pio_rp1_probe,
	.remove_new = pwm_pio_rp1_remove,
};
module_platform_driver(pwm_pio_rp1_driver);

MODULE_DESCRIPTION("PWM PIO RP1 driver");
MODULE_AUTHOR("Phil Elwell");
MODULE_LICENSE("GPL");
