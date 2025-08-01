// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2024 Waveshare International Limited
 *
 * Based on rpi-panel-v2-regulator.c by Dave Stevenson <dave.stevenson@raspberrypi.com>
 */

#include <linux/backlight.h>
#include <linux/err.h>
#include <linux/gpio.h>
#include <linux/gpio/driver.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/regulator/driver.h>
#include <linux/of.h>

/* I2C registers of the microcontroller. */
#define REG_TP 0x94
#define REG_LCD 0x95
#define REG_PWM 0x96
#define REG_SIZE 0x97
#define REG_ID 0x98
#define REG_VERSION 0x99

#define NUM_GPIO 16 /* Treat BL_ENABLE, LCD_RESET, TP_RESET as GPIOs */

struct waveshare_panel_lcd {
	struct mutex dir_lock;
	struct mutex pwr_lock;
	struct regmap *regmap;
	u16 poweron_state;
	u16 direction_state;

	struct gpio_chip gc;
	struct gpio_desc *enable;
};

static const struct regmap_config waveshare_panel_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = REG_PWM,
};

static int waveshare_panel_gpio_direction_in(struct gpio_chip *gc,
					     unsigned int offset)
{
	struct waveshare_panel_lcd *state = gpiochip_get_data(gc);

	mutex_lock(&state->dir_lock);
	state->direction_state |= BIT(offset);
	mutex_unlock(&state->dir_lock);

	return 0;
}

static int waveshare_panel_gpio_direction_out(struct gpio_chip *gc,
					      unsigned int offset, int val)
{
	struct waveshare_panel_lcd *state = gpiochip_get_data(gc);
	u16 last_val;

	mutex_lock(&state->dir_lock);
	state->direction_state &= ~BIT(offset);
	mutex_unlock(&state->dir_lock);

	mutex_lock(&state->pwr_lock);
	last_val = state->poweron_state;
	if (val)
		last_val |= BIT(offset);
	else
		last_val &= ~BIT(offset);

	state->poweron_state = last_val;
	mutex_unlock(&state->pwr_lock);

	regmap_write(state->regmap, REG_TP, last_val >> 8);
	regmap_write(state->regmap, REG_LCD, last_val & 0xff);

	return 0;
}

static int waveshare_panel_gpio_get_direction(struct gpio_chip *gc,
					      unsigned int offset)
{
	struct waveshare_panel_lcd *state = gpiochip_get_data(gc);
	u16 last_val;

	mutex_lock(&state->dir_lock);
	last_val = state->direction_state;
	mutex_unlock(&state->dir_lock);

	if (last_val & BIT(offset))
		return GPIO_LINE_DIRECTION_IN;
	else
		return GPIO_LINE_DIRECTION_OUT;
}

static int waveshare_panel_gpio_get(struct gpio_chip *gc, unsigned int offset)
{
	struct waveshare_panel_lcd *state = gpiochip_get_data(gc);
	u16 pwr_state;

	mutex_lock(&state->pwr_lock);
	pwr_state = state->poweron_state & BIT(offset);
	mutex_unlock(&state->pwr_lock);

	if (pwr_state)
		return 1;
	else
		return 0;
}

static void waveshare_panel_gpio_set(struct gpio_chip *gc, unsigned int offset,
				     int value)
{
	struct waveshare_panel_lcd *state = gpiochip_get_data(gc);
	u16 last_val;

	if (offset >= NUM_GPIO)
		return;

	mutex_lock(&state->pwr_lock);

	last_val = state->poweron_state;
	if (value)
		last_val |= BIT(offset);
	else
		last_val &= ~BIT(offset);

	state->poweron_state = last_val;

	regmap_write(state->regmap, REG_TP, last_val >> 8);
	regmap_write(state->regmap, REG_LCD, last_val & 0xff);

	mutex_unlock(&state->pwr_lock);
}

static int waveshare_panel_update_status(struct backlight_device *bl)
{
	struct waveshare_panel_lcd *state = bl_get_data(bl);
	int brightness = bl->props.brightness;

	if (bl->props.power != FB_BLANK_UNBLANK ||
	    bl->props.state & (BL_CORE_SUSPENDED | BL_CORE_FBBLANK))
		brightness = 0;

	if (state->enable) {
		if (brightness)
			gpiod_set_value_cansleep(state->enable,
						 1); // Enable BL_EN
		else
			gpiod_set_value_cansleep(state->enable,
						 0); // Disable BL_EN
	}

	return regmap_write(state->regmap, REG_PWM, brightness);
}

static const struct backlight_ops waveshare_panel_bl = {
	.update_status = waveshare_panel_update_status,
};

static int waveshare_panel_i2c_read(struct i2c_client *client, u8 reg,
				    unsigned int *buf)
{
	struct i2c_msg msgs[1];
	u8 addr_buf[1] = { reg };
	u8 data_buf[1] = {
		0,
	};
	int ret;

	/* Write register address */
	msgs[0].addr = client->addr;
	msgs[0].flags = 0;
	msgs[0].len = ARRAY_SIZE(addr_buf);
	msgs[0].buf = addr_buf;

	ret = i2c_transfer(client->adapter, msgs, ARRAY_SIZE(msgs));
	if (ret != ARRAY_SIZE(msgs))
		return -EIO;

	usleep_range(5000, 10000);

	/* Read data from register */
	msgs[0].addr = client->addr;
	msgs[0].flags = I2C_M_RD;
	msgs[0].len = 1;
	msgs[0].buf = data_buf;

	ret = i2c_transfer(client->adapter, msgs, ARRAY_SIZE(msgs));
	if (ret != ARRAY_SIZE(msgs))
		return -EIO;

	*buf = data_buf[0];
	return 0;
}

/*
 * I2C driver interface functions
 */
static int waveshare_panel_i2c_probe(struct i2c_client *i2c)
{
	struct backlight_properties props = {};
	struct backlight_device *bl;
	struct waveshare_panel_lcd *state;
	struct regmap *regmap;
	unsigned int data;
	int ret;

	state = devm_kzalloc(&i2c->dev, sizeof(*state), GFP_KERNEL);
	if (!state)
		return -ENOMEM;

	mutex_init(&state->dir_lock);
	mutex_init(&state->pwr_lock);

	i2c_set_clientdata(i2c, state);

	regmap = devm_regmap_init_i2c(i2c, &waveshare_panel_regmap_config);
	if (IS_ERR(regmap)) {
		ret = PTR_ERR(regmap);
		dev_err(&i2c->dev, "Failed to allocate register map: %d\n",
			ret);
		goto error;
	}

	ret = waveshare_panel_i2c_read(i2c, REG_ID, &data);
	if (ret == 0)
		dev_info(&i2c->dev, "waveshare panel hw id = 0x%x\n", data);

	ret = waveshare_panel_i2c_read(i2c, REG_SIZE, &data);
	if (ret == 0)
		dev_info(&i2c->dev, "waveshare panel size = %d\n", data);

	ret = waveshare_panel_i2c_read(i2c, REG_VERSION, &data);
	if (ret == 0)
		dev_info(&i2c->dev, "waveshare panel mcu version = 0x%x\n",
			 data);

	state->direction_state = 0;
	state->poweron_state = BIT(9) | BIT(8); // Enable VCC
	regmap_write(regmap, REG_TP, state->poweron_state >> 8);
	regmap_write(regmap, REG_LCD, state->poweron_state & 0xff);
	msleep(20);

	state->regmap = regmap;
	state->gc.parent = &i2c->dev;
	state->gc.label = i2c->name;
	state->gc.owner = THIS_MODULE;
	state->gc.base = -1;
	state->gc.ngpio = NUM_GPIO;

	state->gc.get = waveshare_panel_gpio_get;
	state->gc.set = waveshare_panel_gpio_set;
	state->gc.direction_input = waveshare_panel_gpio_direction_in;
	state->gc.direction_output = waveshare_panel_gpio_direction_out;
	state->gc.get_direction = waveshare_panel_gpio_get_direction;
	state->gc.can_sleep = true;

	ret = devm_gpiochip_add_data(&i2c->dev, &state->gc, state);
	if (ret) {
		dev_err(&i2c->dev, "Failed to create gpiochip: %d\n", ret);
		goto error;
	}

	state->enable =
		devm_gpiod_get_optional(&i2c->dev, "enable", GPIOD_OUT_LOW);
	if (IS_ERR(state->enable))
		return dev_err_probe(&i2c->dev, PTR_ERR(state->enable),
				     "Couldn't get our enable GPIO\n");

	props.type = BACKLIGHT_RAW;
	props.max_brightness = 255;
	bl = devm_backlight_device_register(&i2c->dev, dev_name(&i2c->dev),
					    &i2c->dev, state,
					    &waveshare_panel_bl, &props);
	if (IS_ERR(bl))
		return PTR_ERR(bl);

	bl->props.brightness = 255;

	return 0;

error:
	mutex_destroy(&state->dir_lock);
	mutex_destroy(&state->pwr_lock);
	return ret;
}

static void waveshare_panel_i2c_remove(struct i2c_client *client)
{
	struct waveshare_panel_lcd *state = i2c_get_clientdata(client);

	mutex_destroy(&state->dir_lock);
	mutex_destroy(&state->pwr_lock);
}

static void waveshare_panel_i2c_shutdown(struct i2c_client *client)
{
	struct waveshare_panel_lcd *state = i2c_get_clientdata(client);

	regmap_write(state->regmap, REG_PWM, 0);
}

static const struct of_device_id waveshare_panel_dt_ids[] = {
	{ .compatible = "waveshare,touchscreen-panel-regulator" },
	{},
};
MODULE_DEVICE_TABLE(of, waveshare_panel_dt_ids);

static struct i2c_driver waveshare_panel_regulator_driver = {
	.driver = {
		.name = "waveshare_touchscreen",
		.of_match_table = of_match_ptr(waveshare_panel_dt_ids),
	},
	.probe = waveshare_panel_i2c_probe,
	.remove	= waveshare_panel_i2c_remove,
	.shutdown = waveshare_panel_i2c_shutdown,
};

module_i2c_driver(waveshare_panel_regulator_driver);

MODULE_AUTHOR("Waveshare Team <support@waveshare.com>");
MODULE_DESCRIPTION("Regulator device driver for Waveshare touchscreen");
MODULE_LICENSE("GPL");
