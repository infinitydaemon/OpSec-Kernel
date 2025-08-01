/*
 * Copyright (C) 2013, NVIDIA Corporation.  All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sub license,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/media-bus-format.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>

#include <video/display_timing.h>
#include <video/of_display_timing.h>
#include <video/videomode.h>

#include <drm/drm_crtc.h>
#include <drm/drm_device.h>
#include <drm/drm_edid.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_panel.h>
#include <drm/drm_of.h>

/**
 * struct panel_desc - Describes a simple panel.
 */
struct panel_desc {
	/**
	 * @modes: Pointer to array of fixed modes appropriate for this panel.
	 *
	 * If only one mode then this can just be the address of the mode.
	 * NOTE: cannot be used with "timings" and also if this is specified
	 * then you cannot override the mode in the device tree.
	 */
	const struct drm_display_mode *modes;

	/** @num_modes: Number of elements in modes array. */
	unsigned int num_modes;

	/**
	 * @timings: Pointer to array of display timings
	 *
	 * NOTE: cannot be used with "modes" and also these will be used to
	 * validate a device tree override if one is present.
	 */
	const struct display_timing *timings;

	/** @num_timings: Number of elements in timings array. */
	unsigned int num_timings;

	/** @bpc: Bits per color. */
	unsigned int bpc;

	/** @size: Structure containing the physical size of this panel. */
	struct {
		/**
		 * @size.width: Width (in mm) of the active display area.
		 */
		unsigned int width;

		/**
		 * @size.height: Height (in mm) of the active display area.
		 */
		unsigned int height;
	} size;

	/** @delay: Structure containing various delay values for this panel. */
	struct {
		/**
		 * @delay.prepare: Time for the panel to become ready.
		 *
		 * The time (in milliseconds) that it takes for the panel to
		 * become ready and start receiving video data
		 */
		unsigned int prepare;

		/**
		 * @delay.enable: Time for the panel to display a valid frame.
		 *
		 * The time (in milliseconds) that it takes for the panel to
		 * display the first valid frame after starting to receive
		 * video data.
		 */
		unsigned int enable;

		/**
		 * @delay.disable: Time for the panel to turn the display off.
		 *
		 * The time (in milliseconds) that it takes for the panel to
		 * turn the display off (no content is visible).
		 */
		unsigned int disable;

		/**
		 * @delay.unprepare: Time to power down completely.
		 *
		 * The time (in milliseconds) that it takes for the panel
		 * to power itself down completely.
		 *
		 * This time is used to prevent a future "prepare" from
		 * starting until at least this many milliseconds has passed.
		 * If at prepare time less time has passed since unprepare
		 * finished, the driver waits for the remaining time.
		 */
		unsigned int unprepare;
	} delay;

	/** @bus_format: See MEDIA_BUS_FMT_... defines. */
	u32 bus_format;

	/** @bus_flags: See DRM_BUS_FLAG_... defines. */
	u32 bus_flags;

	/** @connector_type: LVDS, eDP, DSI, DPI, etc. */
	int connector_type;
};

struct panel_simple {
	struct drm_panel base;

	ktime_t unprepared_time;

	const struct panel_desc *desc;

	struct regulator *supply;
	struct i2c_adapter *ddc;

	struct gpio_desc *enable_gpio;

	const struct drm_edid *drm_edid;

	struct drm_display_mode override_mode;
};

static inline struct panel_simple *to_panel_simple(struct drm_panel *panel)
{
	return container_of(panel, struct panel_simple, base);
}

static unsigned int panel_simple_get_timings_modes(struct panel_simple *panel,
						   struct drm_connector *connector)
{
	struct drm_display_mode *mode;
	unsigned int i, num = 0;

	for (i = 0; i < panel->desc->num_timings; i++) {
		const struct display_timing *dt = &panel->desc->timings[i];
		struct videomode vm;

		videomode_from_timing(dt, &vm);
		mode = drm_mode_create(connector->dev);
		if (!mode) {
			dev_err(panel->base.dev, "failed to add mode %ux%u\n",
				dt->hactive.typ, dt->vactive.typ);
			continue;
		}

		drm_display_mode_from_videomode(&vm, mode);

		mode->type |= DRM_MODE_TYPE_DRIVER;

		if (panel->desc->num_timings == 1)
			mode->type |= DRM_MODE_TYPE_PREFERRED;

		drm_mode_probed_add(connector, mode);
		num++;
	}

	return num;
}

static unsigned int panel_simple_get_display_modes(struct panel_simple *panel,
						   struct drm_connector *connector)
{
	struct drm_display_mode *mode;
	unsigned int i, num = 0;

	for (i = 0; i < panel->desc->num_modes; i++) {
		const struct drm_display_mode *m = &panel->desc->modes[i];

		mode = drm_mode_duplicate(connector->dev, m);
		if (!mode) {
			dev_err(panel->base.dev, "failed to add mode %ux%u@%u\n",
				m->hdisplay, m->vdisplay,
				drm_mode_vrefresh(m));
			continue;
		}

		mode->type |= DRM_MODE_TYPE_DRIVER;

		if (panel->desc->num_modes == 1)
			mode->type |= DRM_MODE_TYPE_PREFERRED;

		drm_mode_set_name(mode);

		drm_mode_probed_add(connector, mode);
		num++;
	}

	return num;
}

static int panel_simple_get_non_edid_modes(struct panel_simple *panel,
					   struct drm_connector *connector)
{
	struct drm_display_mode *mode;
	bool has_override = panel->override_mode.type;
	unsigned int num = 0;

	if (!panel->desc)
		return 0;

	if (has_override) {
		mode = drm_mode_duplicate(connector->dev,
					  &panel->override_mode);
		if (mode) {
			drm_mode_probed_add(connector, mode);
			num = 1;
		} else {
			dev_err(panel->base.dev, "failed to add override mode\n");
		}
	}

	/* Only add timings if override was not there or failed to validate */
	if (num == 0 && panel->desc->num_timings)
		num = panel_simple_get_timings_modes(panel, connector);

	/*
	 * Only add fixed modes if timings/override added no mode.
	 *
	 * We should only ever have either the display timings specified
	 * or a fixed mode. Anything else is rather bogus.
	 */
	WARN_ON(panel->desc->num_timings && panel->desc->num_modes);
	if (num == 0)
		num = panel_simple_get_display_modes(panel, connector);

	connector->display_info.bpc = panel->desc->bpc;
	connector->display_info.width_mm = panel->desc->size.width;
	connector->display_info.height_mm = panel->desc->size.height;
	if (panel->desc->bus_format)
		drm_display_info_set_bus_formats(&connector->display_info,
						 &panel->desc->bus_format, 1);
	connector->display_info.bus_flags = panel->desc->bus_flags;

	return num;
}

static void panel_simple_wait(ktime_t start_ktime, unsigned int min_ms)
{
	ktime_t now_ktime, min_ktime;

	if (!min_ms)
		return;

	min_ktime = ktime_add(start_ktime, ms_to_ktime(min_ms));
	now_ktime = ktime_get_boottime();

	if (ktime_before(now_ktime, min_ktime))
		msleep(ktime_to_ms(ktime_sub(min_ktime, now_ktime)) + 1);
}

static int panel_simple_disable(struct drm_panel *panel)
{
	struct panel_simple *p = to_panel_simple(panel);

	if (p->desc->delay.disable)
		msleep(p->desc->delay.disable);

	return 0;
}

static int panel_simple_suspend(struct device *dev)
{
	struct panel_simple *p = dev_get_drvdata(dev);

	gpiod_set_value_cansleep(p->enable_gpio, 0);
	regulator_disable(p->supply);
	p->unprepared_time = ktime_get_boottime();

	drm_edid_free(p->drm_edid);
	p->drm_edid = NULL;

	return 0;
}

static int panel_simple_unprepare(struct drm_panel *panel)
{
	int ret;

	pm_runtime_mark_last_busy(panel->dev);
	ret = pm_runtime_put_autosuspend(panel->dev);
	if (ret < 0)
		return ret;

	return 0;
}

static int panel_simple_resume(struct device *dev)
{
	struct panel_simple *p = dev_get_drvdata(dev);
	int err;

	panel_simple_wait(p->unprepared_time, p->desc->delay.unprepare);

	err = regulator_enable(p->supply);
	if (err < 0) {
		dev_err(dev, "failed to enable supply: %d\n", err);
		return err;
	}

	gpiod_set_value_cansleep(p->enable_gpio, 1);

	if (p->desc->delay.prepare)
		msleep(p->desc->delay.prepare);

	return 0;
}

static int panel_simple_prepare(struct drm_panel *panel)
{
	int ret;

	ret = pm_runtime_get_sync(panel->dev);
	if (ret < 0) {
		pm_runtime_put_autosuspend(panel->dev);
		return ret;
	}

	return 0;
}

static int panel_simple_enable(struct drm_panel *panel)
{
	struct panel_simple *p = to_panel_simple(panel);

	if (p->desc->delay.enable)
		msleep(p->desc->delay.enable);

	return 0;
}

static int panel_simple_get_modes(struct drm_panel *panel,
				  struct drm_connector *connector)
{
	struct panel_simple *p = to_panel_simple(panel);
	int num = 0;

	/* probe EDID if a DDC bus is available */
	if (p->ddc) {
		pm_runtime_get_sync(panel->dev);

		if (!p->drm_edid)
			p->drm_edid = drm_edid_read_ddc(connector, p->ddc);

		drm_edid_connector_update(connector, p->drm_edid);

		num += drm_edid_connector_add_modes(connector);

		pm_runtime_mark_last_busy(panel->dev);
		pm_runtime_put_autosuspend(panel->dev);
	}

	/* add hard-coded panel modes */
	num += panel_simple_get_non_edid_modes(p, connector);

	return num;
}

static int panel_simple_get_timings(struct drm_panel *panel,
				    unsigned int num_timings,
				    struct display_timing *timings)
{
	struct panel_simple *p = to_panel_simple(panel);
	unsigned int i;

	if (p->desc->num_timings < num_timings)
		num_timings = p->desc->num_timings;

	if (timings)
		for (i = 0; i < num_timings; i++)
			timings[i] = p->desc->timings[i];

	return p->desc->num_timings;
}

static const struct drm_panel_funcs panel_simple_funcs = {
	.disable = panel_simple_disable,
	.unprepare = panel_simple_unprepare,
	.prepare = panel_simple_prepare,
	.enable = panel_simple_enable,
	.get_modes = panel_simple_get_modes,
	.get_timings = panel_simple_get_timings,
};

static struct panel_desc panel_dpi;

static int panel_dpi_probe(struct device *dev,
			   struct panel_simple *panel)
{
	struct display_timing *timing;
	const struct device_node *np;
	struct panel_desc *desc;
	unsigned int bus_flags;
	struct videomode vm;
	int ret;

	np = dev->of_node;
	desc = devm_kzalloc(dev, sizeof(*desc), GFP_KERNEL);
	if (!desc)
		return -ENOMEM;

	timing = devm_kzalloc(dev, sizeof(*timing), GFP_KERNEL);
	if (!timing)
		return -ENOMEM;

	ret = of_get_display_timing(np, "panel-timing", timing);
	if (ret < 0) {
		dev_err(dev, "%pOF: no panel-timing node found for \"panel-dpi\" binding\n",
			np);
		return ret;
	}

	desc->timings = timing;
	desc->num_timings = 1;

	of_property_read_u32(np, "width-mm", &desc->size.width);
	of_property_read_u32(np, "height-mm", &desc->size.height);
	of_property_read_u32(np, "bus-format", &desc->bus_format);

	/* Extract bus_flags from display_timing */
	bus_flags = 0;
	vm.flags = timing->flags;
	drm_bus_flags_from_videomode(&vm, &bus_flags);
	desc->bus_flags = bus_flags;

	/* We do not know the connector for the DT node, so guess it */
	desc->connector_type = DRM_MODE_CONNECTOR_DPI;
	/* Likewise for the bit depth. */
	desc->bpc = 8;

	panel->desc = desc;

	return 0;
}

#define PANEL_SIMPLE_BOUNDS_CHECK(to_check, bounds, field) \
	(to_check->field.typ >= bounds->field.min && \
	 to_check->field.typ <= bounds->field.max)
static void panel_simple_parse_panel_timing_node(struct device *dev,
						 struct panel_simple *panel,
						 const struct display_timing *ot)
{
	const struct panel_desc *desc = panel->desc;
	struct videomode vm;
	unsigned int i;

	if (WARN_ON(desc->num_modes)) {
		dev_err(dev, "Reject override mode: panel has a fixed mode\n");
		return;
	}
	if (WARN_ON(!desc->num_timings)) {
		dev_err(dev, "Reject override mode: no timings specified\n");
		return;
	}

	for (i = 0; i < panel->desc->num_timings; i++) {
		const struct display_timing *dt = &panel->desc->timings[i];

		if (!PANEL_SIMPLE_BOUNDS_CHECK(ot, dt, hactive) ||
		    !PANEL_SIMPLE_BOUNDS_CHECK(ot, dt, hfront_porch) ||
		    !PANEL_SIMPLE_BOUNDS_CHECK(ot, dt, hback_porch) ||
		    !PANEL_SIMPLE_BOUNDS_CHECK(ot, dt, hsync_len) ||
		    !PANEL_SIMPLE_BOUNDS_CHECK(ot, dt, vactive) ||
		    !PANEL_SIMPLE_BOUNDS_CHECK(ot, dt, vfront_porch) ||
		    !PANEL_SIMPLE_BOUNDS_CHECK(ot, dt, vback_porch) ||
		    !PANEL_SIMPLE_BOUNDS_CHECK(ot, dt, vsync_len))
			continue;

		if (ot->flags != dt->flags)
			continue;

		videomode_from_timing(ot, &vm);
		drm_display_mode_from_videomode(&vm, &panel->override_mode);
		panel->override_mode.type |= DRM_MODE_TYPE_DRIVER |
					     DRM_MODE_TYPE_PREFERRED;
		break;
	}

	if (WARN_ON(!panel->override_mode.type))
		dev_err(dev, "Reject override mode: No display_timing found\n");
}

static int panel_simple_override_nondefault_lvds_datamapping(struct device *dev,
							     struct panel_simple *panel)
{
	int ret, bpc;

	ret = drm_of_lvds_get_data_mapping(dev->of_node);
	if (ret < 0) {
		if (ret == -EINVAL)
			dev_warn(dev, "Ignore invalid data-mapping property\n");

		/*
		 * Ignore non-existing or malformatted property, fallback to
		 * default data-mapping, and return 0.
		 */
		return 0;
	}

	switch (ret) {
	default:
		WARN_ON(1);
		fallthrough;
	case MEDIA_BUS_FMT_RGB888_1X7X4_SPWG:
		fallthrough;
	case MEDIA_BUS_FMT_RGB888_1X7X4_JEIDA:
		bpc = 8;
		break;
	case MEDIA_BUS_FMT_RGB666_1X7X3_SPWG:
		bpc = 6;
	}

	if (panel->desc->bpc != bpc || panel->desc->bus_format != ret) {
		struct panel_desc *override_desc;

		override_desc = devm_kmemdup(dev, panel->desc, sizeof(*panel->desc), GFP_KERNEL);
		if (!override_desc)
			return -ENOMEM;

		override_desc->bus_format = ret;
		override_desc->bpc = bpc;
		panel->desc = override_desc;
	}

	return 0;
}

static int panel_simple_probe(struct device *dev, const struct panel_desc *desc)
{
	struct panel_simple *panel;
	struct display_timing dt;
	struct device_node *ddc;
	int connector_type;
	u32 bus_flags;
	int err;

	panel = devm_kzalloc(dev, sizeof(*panel), GFP_KERNEL);
	if (!panel)
		return -ENOMEM;

	panel->desc = desc;

	panel->supply = devm_regulator_get(dev, "power");
	if (IS_ERR(panel->supply))
		return PTR_ERR(panel->supply);

	panel->enable_gpio = devm_gpiod_get_optional(dev, "enable",
						     GPIOD_OUT_LOW);
	if (IS_ERR(panel->enable_gpio))
		return dev_err_probe(dev, PTR_ERR(panel->enable_gpio),
				     "failed to request GPIO\n");

	ddc = of_parse_phandle(dev->of_node, "ddc-i2c-bus", 0);
	if (ddc) {
		panel->ddc = of_find_i2c_adapter_by_node(ddc);
		of_node_put(ddc);

		if (!panel->ddc)
			return -EPROBE_DEFER;
	}

	if (desc == &panel_dpi) {
		/* Handle the generic panel-dpi binding */
		err = panel_dpi_probe(dev, panel);
		if (err)
			goto free_ddc;
		desc = panel->desc;
	} else {
		if (!of_get_display_timing(dev->of_node, "panel-timing", &dt))
			panel_simple_parse_panel_timing_node(dev, panel, &dt);
	}

	if (desc->connector_type == DRM_MODE_CONNECTOR_LVDS) {
		/* Optional data-mapping property for overriding bus format */
		err = panel_simple_override_nondefault_lvds_datamapping(dev, panel);
		if (err)
			goto free_ddc;
	}

	connector_type = desc->connector_type;
	/* Catch common mistakes for panels. */
	switch (connector_type) {
	case 0:
		dev_warn(dev, "Specify missing connector_type\n");
		connector_type = DRM_MODE_CONNECTOR_DPI;
		break;
	case DRM_MODE_CONNECTOR_LVDS:
		WARN_ON(desc->bus_flags &
			~(DRM_BUS_FLAG_DE_LOW |
			  DRM_BUS_FLAG_DE_HIGH |
			  DRM_BUS_FLAG_DATA_MSB_TO_LSB |
			  DRM_BUS_FLAG_DATA_LSB_TO_MSB));
		WARN_ON(desc->bus_format != MEDIA_BUS_FMT_RGB666_1X7X3_SPWG &&
			desc->bus_format != MEDIA_BUS_FMT_RGB888_1X7X4_SPWG &&
			desc->bus_format != MEDIA_BUS_FMT_RGB888_1X7X4_JEIDA);
		WARN_ON(desc->bus_format == MEDIA_BUS_FMT_RGB666_1X7X3_SPWG &&
			desc->bpc != 6);
		WARN_ON((desc->bus_format == MEDIA_BUS_FMT_RGB888_1X7X4_SPWG ||
			 desc->bus_format == MEDIA_BUS_FMT_RGB888_1X7X4_JEIDA) &&
			desc->bpc != 8);
		break;
	case DRM_MODE_CONNECTOR_eDP:
		dev_warn(dev, "eDP panels moved to panel-edp\n");
		err = -EINVAL;
		goto free_ddc;
	case DRM_MODE_CONNECTOR_DSI:
		if (desc->bpc != 6 && desc->bpc != 8)
			dev_warn(dev, "Expected bpc in {6,8} but got: %u\n", desc->bpc);
		break;
	case DRM_MODE_CONNECTOR_DPI:
		bus_flags = DRM_BUS_FLAG_DE_LOW |
			    DRM_BUS_FLAG_DE_HIGH |
			    DRM_BUS_FLAG_PIXDATA_SAMPLE_POSEDGE |
			    DRM_BUS_FLAG_PIXDATA_SAMPLE_NEGEDGE |
			    DRM_BUS_FLAG_DATA_MSB_TO_LSB |
			    DRM_BUS_FLAG_DATA_LSB_TO_MSB |
			    DRM_BUS_FLAG_SYNC_SAMPLE_POSEDGE |
			    DRM_BUS_FLAG_SYNC_SAMPLE_NEGEDGE;
		if (desc->bus_flags & ~bus_flags)
			dev_warn(dev, "Unexpected bus_flags(%d)\n", desc->bus_flags & ~bus_flags);
		if (!(desc->bus_flags & bus_flags))
			dev_warn(dev, "Specify missing bus_flags\n");
		if (desc->bus_format == 0)
			dev_warn(dev, "Specify missing bus_format\n");
		if (desc->bpc != 6 && desc->bpc != 8)
			dev_warn(dev, "Expected bpc in {6,8} but got: %u\n", desc->bpc);
		break;
	default:
		dev_warn(dev, "Specify a valid connector_type: %d\n", desc->connector_type);
		connector_type = DRM_MODE_CONNECTOR_DPI;
		break;
	}

	dev_set_drvdata(dev, panel);

	/*
	 * We use runtime PM for prepare / unprepare since those power the panel
	 * on and off and those can be very slow operations. This is important
	 * to optimize powering the panel on briefly to read the EDID before
	 * fully enabling the panel.
	 */
	pm_runtime_enable(dev);
	pm_runtime_set_autosuspend_delay(dev, 1000);
	pm_runtime_use_autosuspend(dev);

	drm_panel_init(&panel->base, dev, &panel_simple_funcs, connector_type);

	err = drm_panel_of_backlight(&panel->base);
	if (err) {
		dev_err_probe(dev, err, "Could not find backlight\n");
		goto disable_pm_runtime;
	}

	drm_panel_add(&panel->base);

	return 0;

disable_pm_runtime:
	pm_runtime_dont_use_autosuspend(dev);
	pm_runtime_disable(dev);
free_ddc:
	if (panel->ddc)
		put_device(&panel->ddc->dev);

	return err;
}

static void panel_simple_shutdown(struct device *dev)
{
	struct panel_simple *panel = dev_get_drvdata(dev);

	/*
	 * NOTE: the following two calls don't really belong here. It is the
	 * responsibility of a correctly written DRM modeset driver to call
	 * drm_atomic_helper_shutdown() at shutdown time and that should
	 * cause the panel to be disabled / unprepared if needed. For now,
	 * however, we'll keep these calls due to the sheer number of
	 * different DRM modeset drivers used with panel-simple. Once we've
	 * confirmed that all DRM modeset drivers using this panel properly
	 * call drm_atomic_helper_shutdown() we can simply delete the two
	 * calls below.
	 *
	 * TO BE EXPLICIT: THE CALLS BELOW SHOULDN'T BE COPIED TO ANY NEW
	 * PANEL DRIVERS.
	 *
	 * FIXME: If we're still haven't figured out if all DRM modeset
	 * drivers properly call drm_atomic_helper_shutdown() but we _have_
	 * managed to make sure that DRM modeset drivers get their shutdown()
	 * callback before the panel's shutdown() callback (perhaps using
	 * device link), we could add a WARN_ON here to help move forward.
	 */
	if (panel->base.enabled)
		drm_panel_disable(&panel->base);
	if (panel->base.prepared)
		drm_panel_unprepare(&panel->base);
}

static void panel_simple_remove(struct device *dev)
{
	struct panel_simple *panel = dev_get_drvdata(dev);

	drm_panel_remove(&panel->base);
	panel_simple_shutdown(dev);

	pm_runtime_dont_use_autosuspend(dev);
	pm_runtime_disable(dev);
	if (panel->ddc)
		put_device(&panel->ddc->dev);
}

static const struct drm_display_mode ampire_am_1280800n3tzqw_t00h_mode = {
	.clock = 71100,
	.hdisplay = 1280,
	.hsync_start = 1280 + 40,
	.hsync_end = 1280 + 40 + 80,
	.htotal = 1280 + 40 + 80 + 40,
	.vdisplay = 800,
	.vsync_start = 800 + 3,
	.vsync_end = 800 + 3 + 10,
	.vtotal = 800 + 3 + 10 + 10,
	.flags = DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC,
};

static const struct panel_desc ampire_am_1280800n3tzqw_t00h = {
	.modes = &ampire_am_1280800n3tzqw_t00h_mode,
	.num_modes = 1,
	.bpc = 8,
	.size = {
		.width = 217,
		.height = 136,
	},
	.bus_flags = DRM_BUS_FLAG_DE_HIGH,
	.bus_format = MEDIA_BUS_FMT_RGB888_1X7X4_SPWG,
	.connector_type = DRM_MODE_CONNECTOR_LVDS,
};

static const struct drm_display_mode ampire_am_480272h3tmqw_t01h_mode = {
	.clock = 9000,
	.hdisplay = 480,
	.hsync_start = 480 + 2,
	.hsync_end = 480 + 2 + 41,
	.htotal = 480 + 2 + 41 + 2,
	.vdisplay = 272,
	.vsync_start = 272 + 2,
	.vsync_end = 272 + 2 + 10,
	.vtotal = 272 + 2 + 10 + 2,
	.flags = DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC,
};

static const struct panel_desc ampire_am_480272h3tmqw_t01h = {
	.modes = &ampire_am_480272h3tmqw_t01h_mode,
	.num_modes = 1,
	.bpc = 8,
	.size = {
		.width = 99,
		.height = 58,
	},
	.bus_format = MEDIA_BUS_FMT_RGB888_1X24,
};

static const struct drm_display_mode ampire_am800480r3tmqwa1h_mode = {
	.clock = 33333,
	.hdisplay = 800,
	.hsync_start = 800 + 0,
	.hsync_end = 800 + 0 + 255,
	.htotal = 800 + 0 + 255 + 0,
	.vdisplay = 480,
	.vsync_start = 480 + 2,
	.vsync_end = 480 + 2 + 45,
	.vtotal = 480 + 2 + 45 + 0,
	.flags = DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC,
};

static const struct display_timing ampire_am_800480l1tmqw_t00h_timing = {
	.pixelclock = { 29930000, 33260000, 36590000 },
	.hactive = { 800, 800, 800 },
	.hfront_porch = { 1, 40, 168 },
	.hback_porch = { 88, 88, 88 },
	.hsync_len = { 1, 128, 128 },
	.vactive = { 480, 480, 480 },
	.vfront_porch = { 1, 35, 37 },
	.vback_porch = { 8, 8, 8 },
	.vsync_len = { 1, 2, 2 },
	.flags = DISPLAY_FLAGS_HSYNC_LOW | DISPLAY_FLAGS_VSYNC_LOW |
		 DISPLAY_FLAGS_DE_HIGH | DISPLAY_FLAGS_PIXDATA_POSEDGE |
		 DISPLAY_FLAGS_SYNC_POSEDGE,
};

static const struct panel_desc ampire_am_800480l1tmqw_t00h = {
	.timings = &ampire_am_800480l1tmqw_t00h_timing,
	.num_timings = 1,
	.bpc = 8,
	.size = {
		.width = 111,
		.height = 67,
	},
	.bus_format = MEDIA_BUS_FMT_RGB888_1X24,
	.bus_flags = DRM_BUS_FLAG_DE_HIGH |
		     DRM_BUS_FLAG_PIXDATA_SAMPLE_NEGEDGE |
		     DRM_BUS_FLAG_SYNC_SAMPLE_NEGEDGE,
	.connector_type = DRM_MODE_CONNECTOR_DPI,
};

static const struct panel_desc ampire_am800480r3tmqwa1h = {
	.modes = &ampire_am800480r3tmqwa1h_mode,
	.num_modes = 1,
	.bpc = 6,
	.size = {
		.width = 152,
		.height = 91,
	},
	.bus_format = MEDIA_BUS_FMT_RGB666_1X18,
};

static const struct display_timing ampire_am800600p5tmqw_tb8h_timing = {
	.pixelclock = { 34500000, 39600000, 50400000 },
	.hactive = { 800, 800, 800 },
	.hfront_porch = { 12, 112, 312 },
	.hback_porch = { 87, 87, 48 },
	.hsync_len = { 1, 1, 40 },
	.vactive = { 600, 600, 600 },
	.vfront_porch = { 1, 21, 61 },
	.vback_porch = { 38, 38, 19 },
	.vsync_len = { 1, 1, 20 },
	.flags = DISPLAY_FLAGS_HSYNC_LOW | DISPLAY_FLAGS_VSYNC_LOW |
		DISPLAY_FLAGS_DE_HIGH | DISPLAY_FLAGS_PIXDATA_POSEDGE |
		DISPLAY_FLAGS_SYNC_POSEDGE,
};

static const struct panel_desc ampire_am800600p5tmqwtb8h = {
	.timings = &ampire_am800600p5tmqw_tb8h_timing,
	.num_timings = 1,
	.bpc = 6,
	.size = {
		.width = 162,
		.height = 122,
	},
	.bus_format = MEDIA_BUS_FMT_RGB666_1X18,
	.bus_flags = DRM_BUS_FLAG_DE_HIGH |
		DRM_BUS_FLAG_PIXDATA_SAMPLE_NEGEDGE |
		DRM_BUS_FLAG_SYNC_SAMPLE_NEGEDGE,
	.connector_type = DRM_MODE_CONNECTOR_DPI,
};

static const struct display_timing santek_st0700i5y_rbslw_f_timing = {
	.pixelclock = { 26400000, 33300000, 46800000 },
	.hactive = { 800, 800, 800 },
	.hfront_porch = { 16, 210, 354 },
	.hback_porch = { 45, 36, 6 },
	.hsync_len = { 1, 10, 40 },
	.vactive = { 480, 480, 480 },
	.vfront_porch = { 7, 22, 147 },
	.vback_porch = { 22, 13, 3 },
	.vsync_len = { 1, 10, 20 },
	.flags = DISPLAY_FLAGS_HSYNC_LOW | DISPLAY_FLAGS_VSYNC_LOW |
		DISPLAY_FLAGS_DE_HIGH | DISPLAY_FLAGS_PIXDATA_POSEDGE
};

static const struct panel_desc armadeus_st0700_adapt = {
	.timings = &santek_st0700i5y_rbslw_f_timing,
	.num_timings = 1,
	.bpc = 6,
	.size = {
		.width = 154,
		.height = 86,
	},
	.bus_format = MEDIA_BUS_FMT_RGB666_1X18,
	.bus_flags = DRM_BUS_FLAG_DE_HIGH | DRM_BUS_FLAG_PIXDATA_SAMPLE_NEGEDGE,
};

static const struct drm_display_mode auo_b101aw03_mode = {
	.clock = 51450,
	.hdisplay = 1024,
	.hsync_start = 1024 + 156,
	.hsync_end = 1024 + 156 + 8,
	.htotal = 1024 + 156 + 8 + 156,
	.vdisplay = 600,
	.vsync_start = 600 + 16,
	.vsync_end = 600 + 16 + 6,
	.vtotal = 600 + 16 + 6 + 16,
};

static const struct panel_desc auo_b101aw03 = {
	.modes = &auo_b101aw03_mode,
	.num_modes = 1,
	.bpc = 6,
	.size = {
		.width = 223,
		.height = 125,
	},
	.bus_format = MEDIA_BUS_FMT_RGB666_1X7X3_SPWG,
	.bus_flags = DRM_BUS_FLAG_DE_HIGH,
	.connector_type = DRM_MODE_CONNECTOR_LVDS,
};

static const struct drm_display_mode auo_b101xtn01_mode = {
	.clock = 72000,
	.hdisplay = 1366,
	.hsync_start = 1366 + 20,
	.hsync_end = 1366 + 20 + 70,
	.htotal = 1366 + 20 + 70,
	.vdisplay = 768,
	.vsync_start = 768 + 14,
	.vsync_end = 768 + 14 + 42,
	.vtotal = 768 + 14 + 42,
	.flags = DRM_MODE_FLAG_NVSYNC | DRM_MODE_FLAG_NHSYNC,
};

static const struct panel_desc auo_b101xtn01 = {
	.modes = &auo_b101xtn01_mode,
	.num_modes = 1,
	.bpc = 6,
	.size = {
		.width = 223,
		.height = 125,
	},
};

static const struct drm_display_mode auo_b116xw03_mode = {
	.clock = 70589,
	.hdisplay = 1366,
	.hsync_start = 1366 + 40,
	.hsync_end = 1366 + 40 + 40,
	.htotal = 1366 + 40 + 40 + 32,
	.vdisplay = 768,
	.vsync_start = 768 + 10,
	.vsync_end = 768 + 10 + 12,
	.vtotal = 768 + 10 + 12 + 6,
	.flags = DRM_MODE_FLAG_NVSYNC | DRM_MODE_FLAG_NHSYNC,
};

static const struct panel_desc auo_b116xw03 = {
	.modes = &auo_b116xw03_mode,
	.num_modes = 1,
	.bpc = 6,
	.size = {
		.width = 256,
		.height = 144,
	},
	.delay = {
		.prepare = 1,
		.enable = 200,
		.disable = 200,
		.unprepare = 500,
	},
	.bus_format = MEDIA_BUS_FMT_RGB666_1X7X3_SPWG,
	.bus_flags = DRM_BUS_FLAG_DE_HIGH,
	.connector_type = DRM_MODE_CONNECTOR_LVDS,
};

static const struct display_timing auo_g070vvn01_timings = {
	.pixelclock = { 33300000, 34209000, 45000000 },
	.hactive = { 800, 800, 800 },
	.hfront_porch = { 20, 40, 200 },
	.hback_porch = { 87, 40, 1 },
	.hsync_len = { 1, 48, 87 },
	.vactive = { 480, 480, 480 },
	.vfront_porch = { 5, 13, 200 },
	.vback_porch = { 31, 31, 29 },
	.vsync_len = { 1, 1, 3 },
};

static const struct panel_desc auo_g070vvn01 = {
	.timings = &auo_g070vvn01_timings,
	.num_timings = 1,
	.bpc = 8,
	.size = {
		.width = 152,
		.height = 91,
	},
	.delay = {
		.prepare = 200,
		.enable = 50,
		.disable = 50,
		.unprepare = 1000,
	},
};

static const struct display_timing auo_g101evn010_timing = {
	.pixelclock = { 64000000, 68930000, 85000000 },
	.hactive = { 1280, 1280, 1280 },
	.hfront_porch = { 8, 64, 256 },
	.hback_porch = { 8, 64, 256 },
	.hsync_len = { 40, 168, 767 },
	.vactive = { 800, 800, 800 },
	.vfront_porch = { 4, 8, 100 },
	.vback_porch = { 4, 8, 100 },
	.vsync_len = { 8, 16, 223 },
};

static const struct panel_desc auo_g101evn010 = {
	.timings = &auo_g101evn010_timing,
	.num_timings = 1,
	.bpc = 6,
	.size = {
		.width = 216,
		.height = 135,
	},
	.bus_format = MEDIA_BUS_FMT_RGB666_1X7X3_SPWG,
	.bus_flags = DRM_BUS_FLAG_DE_HIGH,
	.connector_type = DRM_MODE_CONNECTOR_LVDS,
};

static const struct drm_display_mode auo_g104sn02_mode = {
	.clock = 40000,
	.hdisplay = 800,
	.hsync_start = 800 + 40,
	.hsync_end = 800 + 40 + 216,
	.htotal = 800 + 40 + 216 + 128,
	.vdisplay = 600,
	.vsync_start = 600 + 10,
	.vsync_end = 600 + 10 + 35,
	.vtotal = 600 + 10 + 35 + 2,
};

static const struct panel_desc auo_g104sn02 = {
	.modes = &auo_g104sn02_mode,
	.num_modes = 1,
	.bpc = 8,
	.size = {
		.width = 211,
		.height = 158,
	},
	.bus_format = MEDIA_BUS_FMT_RGB888_1X7X4_SPWG,
	.connector_type = DRM_MODE_CONNECTOR_LVDS,
};

static const struct drm_display_mode auo_g104stn01_mode = {
	.clock = 40000,
	.hdisplay = 800,
	.hsync_start = 800 + 40,
	.hsync_end = 800 + 40 + 88,
	.htotal = 800 + 40 + 88 + 128,
	.vdisplay = 600,
	.vsync_start = 600 + 1,
	.vsync_end = 600 + 1 + 23,
	.vtotal = 600 + 1 + 23 + 4,
};

static const struct panel_desc auo_g104stn01 = {
	.modes = &auo_g104stn01_mode,
	.num_modes = 1,
	.bpc = 8,
	.size = {
		.width = 211,
		.height = 158,
	},
	.bus_format = MEDIA_BUS_FMT_RGB888_1X7X4_SPWG,
	.connector_type = DRM_MODE_CONNECTOR_LVDS,
};

static const struct display_timing auo_g121ean01_timing = {
	.pixelclock = { 60000000, 74400000, 90000000 },
	.hactive = { 1280, 1280, 1280 },
	.hfront_porch = { 20, 50, 100 },
	.hback_porch = { 20, 50, 100 },
	.hsync_len = { 30, 100, 200 },
	.vactive = { 800, 800, 800 },
	.vfront_porch = { 2, 10, 25 },
	.vback_porch = { 2, 10, 25 },
	.vsync_len = { 4, 18, 50 },
};

static const struct panel_desc auo_g121ean01 = {
	.timings = &auo_g121ean01_timing,
	.num_timings = 1,
	.bpc = 8,
	.size = {
		.width = 261,
		.height = 163,
	},
	.bus_format = MEDIA_BUS_FMT_RGB888_1X7X4_SPWG,
	.connector_type = DRM_MODE_CONNECTOR_LVDS,
};

static const struct display_timing auo_g133han01_timings = {
	.pixelclock = { 134000000, 141200000, 149000000 },
	.hactive = { 1920, 1920, 1920 },
	.hfront_porch = { 39, 58, 77 },
	.hback_porch = { 59, 88, 117 },
	.hsync_len = { 28, 42, 56 },
	.vactive = { 1080, 1080, 1080 },
	.vfront_porch = { 3, 8, 11 },
	.vback_porch = { 5, 14, 19 },
	.vsync_len = { 4, 14, 19 },
};

static const struct panel_desc auo_g133han01 = {
	.timings = &auo_g133han01_timings,
	.num_timings = 1,
	.bpc = 8,
	.size = {
		.width = 293,
		.height = 165,
	},
	.delay = {
		.prepare = 200,
		.enable = 50,
		.disable = 50,
		.unprepare = 1000,
	},
	.bus_format = MEDIA_BUS_FMT_RGB888_1X7X4_JEIDA,
	.connector_type = DRM_MODE_CONNECTOR_LVDS,
};

static const struct display_timing auo_g156han04_timings = {
	.pixelclock = { 137000000, 141000000, 146000000 },
	.hactive = { 1920, 1920, 1920 },
	.hfront_porch = { 60, 60, 60 },
	.hback_porch = { 90, 92, 111 },
	.hsync_len =  { 32, 32, 32 },
	.vactive = { 1080, 1080, 1080 },
	.vfront_porch = { 12, 12, 12 },
	.vback_porch = { 24, 36, 56 },
	.vsync_len = { 8, 8, 8 },
};

static const struct panel_desc auo_g156han04 = {
	.timings = &auo_g156han04_timings,
	.num_timings = 1,
	.bpc = 8,
	.size = {
		.width = 344,
		.height = 194,
	},
	.delay = {
		.prepare = 50,		/* T2 */
		.enable = 200,		/* T3 */
		.disable = 110,		/* T10 */
		.unprepare = 1000,	/* T13 */
	},
	.bus_format = MEDIA_BUS_FMT_RGB888_1X7X4_SPWG,
	.bus_flags = DRM_BUS_FLAG_DE_HIGH,
	.connector_type = DRM_MODE_CONNECTOR_LVDS,
};

static const struct drm_display_mode auo_g156xtn01_mode = {
	.clock = 76000,
	.hdisplay = 1366,
	.hsync_start = 1366 + 33,
	.hsync_end = 1366 + 33 + 67,
	.htotal = 1560,
	.vdisplay = 768,
	.vsync_start = 768 + 4,
	.vsync_end = 768 + 4 + 4,
	.vtotal = 806,
};

static const struct panel_desc auo_g156xtn01 = {
	.modes = &auo_g156xtn01_mode,
	.num_modes = 1,
	.bpc = 8,
	.size = {
		.width = 344,
		.height = 194,
	},
	.bus_format = MEDIA_BUS_FMT_RGB888_1X7X4_SPWG,
	.connector_type = DRM_MODE_CONNECTOR_LVDS,
};

static const struct display_timing auo_g185han01_timings = {
	.pixelclock = { 120000000, 144000000, 175000000 },
	.hactive = { 1920, 1920, 1920 },
	.hfront_porch = { 36, 120, 148 },
	.hback_porch = { 24, 88, 108 },
	.hsync_len = { 20, 48, 64 },
	.vactive = { 1080, 1080, 1080 },
	.vfront_porch = { 6, 10, 40 },
	.vback_porch = { 2, 5, 20 },
	.vsync_len = { 2, 5, 20 },
};

static const struct panel_desc auo_g185han01 = {
	.timings = &auo_g185han01_timings,
	.num_timings = 1,
	.bpc = 8,
	.size = {
		.width = 409,
		.height = 230,
	},
	.delay = {
		.prepare = 50,
		.enable = 200,
		.disable = 110,
		.unprepare = 1000,
	},
	.bus_format = MEDIA_BUS_FMT_RGB888_1X7X4_SPWG,
	.connector_type = DRM_MODE_CONNECTOR_LVDS,
};

static const struct display_timing auo_g190ean01_timings = {
	.pixelclock = { 90000000, 108000000, 135000000 },
	.hactive = { 1280, 1280, 1280 },
	.hfront_porch = { 126, 184, 1266 },
	.hback_porch = { 84, 122, 844 },
	.hsync_len = { 70, 102, 704 },
	.vactive = { 1024, 1024, 1024 },
	.vfront_porch = { 4, 26, 76 },
	.vback_porch = { 2, 8, 25 },
	.vsync_len = { 2, 8, 25 },
};

static const struct panel_desc auo_g190ean01 = {
	.timings = &auo_g190ean01_timings,
	.num_timings = 1,
	.bpc = 8,
	.size = {
		.width = 376,
		.height = 301,
	},
	.delay = {
		.prepare = 50,
		.enable = 200,
		.disable = 110,
		.unprepare = 1000,
	},
	.bus_format = MEDIA_BUS_FMT_RGB888_1X7X4_SPWG,
	.connector_type = DRM_MODE_CONNECTOR_LVDS,
};

static const struct display_timing auo_p320hvn03_timings = {
	.pixelclock = { 106000000, 148500000, 164000000 },
	.hactive = { 1920, 1920, 1920 },
	.hfront_porch = { 25, 50, 130 },
	.hback_porch = { 25, 50, 130 },
	.hsync_len = { 20, 40, 105 },
	.vactive = { 1080, 1080, 1080 },
	.vfront_porch = { 8, 17, 150 },
	.vback_porch = { 8, 17, 150 },
	.vsync_len = { 4, 11, 100 },
};

static const struct panel_desc auo_p320hvn03 = {
	.timings = &auo_p320hvn03_timings,
	.num_timings = 1,
	.bpc = 8,
	.size = {
		.width = 698,
		.height = 393,
	},
	.delay = {
		.prepare = 1,
		.enable = 450,
		.unprepare = 500,
	},
	.bus_format = MEDIA_BUS_FMT_RGB888_1X7X4_SPWG,
	.connector_type = DRM_MODE_CONNECTOR_LVDS,
};

static const struct drm_display_mode auo_t215hvn01_mode = {
	.clock = 148800,
	.hdisplay = 1920,
	.hsync_start = 1920 + 88,
	.hsync_end = 1920 + 88 + 44,
	.htotal = 1920 + 88 + 44 + 148,
	.vdisplay = 1080,
	.vsync_start = 1080 + 4,
	.vsync_end = 1080 + 4 + 5,
	.vtotal = 1080 + 4 + 5 + 36,
};

static const struct panel_desc auo_t215hvn01 = {
	.modes = &auo_t215hvn01_mode,
	.num_modes = 1,
	.bpc = 8,
	.size = {
		.width = 430,
		.height = 270,
	},
	.delay = {
		.disable = 5,
		.unprepare = 1000,
	},
	.bus_format = MEDIA_BUS_FMT_RGB888_1X7X4_SPWG,
	.connector_type = DRM_MODE_CONNECTOR_LVDS,
};

static const struct drm_display_mode avic_tm070ddh03_mode = {
	.clock = 51200,
	.hdisplay = 1024,
	.hsync_start = 1024 + 160,
	.hsync_end = 1024 + 160 + 4,
	.htotal = 1024 + 160 + 4 + 156,
	.vdisplay = 600,
	.vsync_start = 600 + 17,
	.vsync_end = 600 + 17 + 1,
	.vtotal = 600 + 17 + 1 + 17,
};

static const struct panel_desc avic_tm070ddh03 = {
	.modes = &avic_tm070ddh03_mode,
	.num_modes = 1,
	.bpc = 8,
	.size = {
		.width = 154,
		.height = 90,
	},
	.delay = {
		.prepare = 20,
		.enable = 200,
		.disable = 200,
	},
};

static const struct drm_display_mode bananapi_s070wv20_ct16_mode = {
	.clock = 30000,
	.hdisplay = 800,
	.hsync_start = 800 + 40,
	.hsync_end = 800 + 40 + 48,
	.htotal = 800 + 40 + 48 + 40,
	.vdisplay = 480,
	.vsync_start = 480 + 13,
	.vsync_end = 480 + 13 + 3,
	.vtotal = 480 + 13 + 3 + 29,
};

static const struct panel_desc bananapi_s070wv20_ct16 = {
	.modes = &bananapi_s070wv20_ct16_mode,
	.num_modes = 1,
	.bpc = 6,
	.size = {
		.width = 154,
		.height = 86,
	},
};

static const struct drm_display_mode boe_bp101wx1_100_mode = {
	.clock = 78945,
	.hdisplay = 1280,
	.hsync_start = 1280 + 0,
	.hsync_end = 1280 + 0 + 2,
	.htotal = 1280 + 62 + 0 + 2,
	.vdisplay = 800,
	.vsync_start = 800 + 8,
	.vsync_end = 800 + 8 + 2,
	.vtotal = 800 + 6 + 8 + 2,
};

static const struct panel_desc boe_bp082wx1_100 = {
	.modes = &boe_bp101wx1_100_mode,
	.num_modes = 1,
	.bpc = 8,
	.size = {
		.width = 177,
		.height = 110,
	},
	.delay = {
		.enable = 50,
		.disable = 50,
	},
	.bus_format = MEDIA_BUS_FMT_RGB888_1X7X4_JEIDA,
	.bus_flags = DRM_BUS_FLAG_DE_HIGH,
	.connector_type = DRM_MODE_CONNECTOR_LVDS,
};

static const struct panel_desc boe_bp101wx1_100 = {
	.modes = &boe_bp101wx1_100_mode,
	.num_modes = 1,
	.bpc = 8,
	.size = {
		.width = 217,
		.height = 136,
	},
	.delay = {
		.enable = 50,
		.disable = 50,
	},
	.bus_format = MEDIA_BUS_FMT_RGB888_1X7X4_JEIDA,
	.bus_flags = DRM_BUS_FLAG_DE_HIGH,
	.connector_type = DRM_MODE_CONNECTOR_LVDS,
};

static const struct display_timing boe_ev121wxm_n10_1850_timing = {
	.pixelclock = { 69922000, 71000000, 72293000 },
	.hactive = { 1280, 1280, 1280 },
	.hfront_porch = { 48, 48, 48 },
	.hback_porch = { 80, 80, 80 },
	.hsync_len = { 32, 32, 32 },
	.vactive = { 800, 800, 800 },
	.vfront_porch = { 3, 3, 3 },
	.vback_porch = { 14, 14, 14 },
	.vsync_len = { 6, 6, 6 },
};

static const struct panel_desc boe_ev121wxm_n10_1850 = {
	.timings = &boe_ev121wxm_n10_1850_timing,
	.num_timings = 1,
	.bpc = 8,
	.size = {
		.width = 261,
		.height = 163,
	},
	.delay = {
		.prepare = 9,
		.enable = 300,
		.unprepare = 300,
		.disable = 560,
	},
	.bus_format = MEDIA_BUS_FMT_RGB888_1X7X4_SPWG,
	.bus_flags = DRM_BUS_FLAG_DE_HIGH,
	.connector_type = DRM_MODE_CONNECTOR_LVDS,
};

static const struct drm_display_mode boe_hv070wsa_mode = {
	.clock = 42105,
	.hdisplay = 1024,
	.hsync_start = 1024 + 30,
	.hsync_end = 1024 + 30 + 30,
	.htotal = 1024 + 30 + 30 + 30,
	.vdisplay = 600,
	.vsync_start = 600 + 10,
	.vsync_end = 600 + 10 + 10,
	.vtotal = 600 + 10 + 10 + 10,
};

static const struct panel_desc boe_hv070wsa = {
	.modes = &boe_hv070wsa_mode,
	.num_modes = 1,
	.bpc = 8,
	.size = {
		.width = 154,
		.height = 90,
	},
	.bus_format = MEDIA_BUS_FMT_RGB888_1X7X4_SPWG,
	.bus_flags = DRM_BUS_FLAG_DE_HIGH,
	.connector_type = DRM_MODE_CONNECTOR_LVDS,
};

static const struct display_timing cct_cmt430b19n00_timing = {
	.pixelclock = { 8000000, 9000000, 12000000 },
	.hactive = { 480, 480, 480 },
	.hfront_porch = { 2, 8, 75 },
	.hback_porch = { 3, 43, 43 },
	.hsync_len = { 2, 4, 75 },
	.vactive = { 272, 272, 272 },
	.vfront_porch = { 2, 8, 37 },
	.vback_porch = { 2, 12, 12 },
	.vsync_len = { 2, 4, 37 },
	.flags = DISPLAY_FLAGS_HSYNC_LOW | DISPLAY_FLAGS_VSYNC_LOW
};

static const struct panel_desc cct_cmt430b19n00 = {
	.timings = &cct_cmt430b19n00_timing,
	.num_timings = 1,
	.bpc = 8,
	.size = {
		.width = 95,
		.height = 53,
	},
	.bus_format = MEDIA_BUS_FMT_RGB888_1X24,
	.bus_flags = DRM_BUS_FLAG_DE_HIGH | DRM_BUS_FLAG_PIXDATA_DRIVE_NEGEDGE,
	.connector_type = DRM_MODE_CONNECTOR_DPI,
};

static const struct drm_display_mode cdtech_s043wq26h_ct7_mode = {
	.clock = 9000,
	.hdisplay = 480,
	.hsync_start = 480 + 5,
	.hsync_end = 480 + 5 + 5,
	.htotal = 480 + 5 + 5 + 40,
	.vdisplay = 272,
	.vsync_start = 272 + 8,
	.vsync_end = 272 + 8 + 8,
	.vtotal = 272 + 8 + 8 + 8,
	.flags = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC,
};

static const struct panel_desc cdtech_s043wq26h_ct7 = {
	.modes = &cdtech_s043wq26h_ct7_mode,
	.num_modes = 1,
	.bpc = 8,
	.size = {
		.width = 95,
		.height = 54,
	},
	.bus_flags = DRM_BUS_FLAG_PIXDATA_DRIVE_POSEDGE,
};

/* S070PWS19HP-FC21 2017/04/22 */
static const struct drm_display_mode cdtech_s070pws19hp_fc21_mode = {
	.clock = 51200,
	.hdisplay = 1024,
	.hsync_start = 1024 + 160,
	.hsync_end = 1024 + 160 + 20,
	.htotal = 1024 + 160 + 20 + 140,
	.vdisplay = 600,
	.vsync_start = 600 + 12,
	.vsync_end = 600 + 12 + 3,
	.vtotal = 600 + 12 + 3 + 20,
	.flags = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC,
};

static const struct panel_desc cdtech_s070pws19hp_fc21 = {
	.modes = &cdtech_s070pws19hp_fc21_mode,
	.num_modes = 1,
	.bpc = 6,
	.size = {
		.width = 154,
		.height = 86,
	},
	.bus_format = MEDIA_BUS_FMT_RGB666_1X18,
	.bus_flags = DRM_BUS_FLAG_DE_HIGH | DRM_BUS_FLAG_PIXDATA_SAMPLE_NEGEDGE,
	.connector_type = DRM_MODE_CONNECTOR_DPI,
};

/* S070SWV29HG-DC44 2017/09/21 */
static const struct drm_display_mode cdtech_s070swv29hg_dc44_mode = {
	.clock = 33300,
	.hdisplay = 800,
	.hsync_start = 800 + 210,
	.hsync_end = 800 + 210 + 2,
	.htotal = 800 + 210 + 2 + 44,
	.vdisplay = 480,
	.vsync_start = 480 + 22,
	.vsync_end = 480 + 22 + 2,
	.vtotal = 480 + 22 + 2 + 21,
	.flags = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC,
};

static const struct panel_desc cdtech_s070swv29hg_dc44 = {
	.modes = &cdtech_s070swv29hg_dc44_mode,
	.num_modes = 1,
	.bpc = 6,
	.size = {
		.width = 154,
		.height = 86,
	},
	.bus_format = MEDIA_BUS_FMT_RGB666_1X18,
	.bus_flags = DRM_BUS_FLAG_DE_HIGH | DRM_BUS_FLAG_PIXDATA_SAMPLE_NEGEDGE,
	.connector_type = DRM_MODE_CONNECTOR_DPI,
};

static const struct drm_display_mode cdtech_s070wv95_ct16_mode = {
	.clock = 35000,
	.hdisplay = 800,
	.hsync_start = 800 + 40,
	.hsync_end = 800 + 40 + 40,
	.htotal = 800 + 40 + 40 + 48,
	.vdisplay = 480,
	.vsync_start = 480 + 29,
	.vsync_end = 480 + 29 + 13,
	.vtotal = 480 + 29 + 13 + 3,
	.flags = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC,
};

static const struct panel_desc cdtech_s070wv95_ct16 = {
	.modes = &cdtech_s070wv95_ct16_mode,
	.num_modes = 1,
	.bpc = 8,
	.size = {
		.width = 154,
		.height = 85,
	},
};

static const struct display_timing chefree_ch101olhlwh_002_timing = {
	.pixelclock = { 68900000, 71100000, 73400000 },
	.hactive = { 1280, 1280, 1280 },
	.hfront_porch = { 65, 80, 95 },
	.hback_porch = { 64, 79, 94 },
	.hsync_len = { 1, 1, 1 },
	.vactive = { 800, 800, 800 },
	.vfront_porch = { 7, 11, 14 },
	.vback_porch = { 7, 11, 14 },
	.vsync_len = { 1, 1, 1 },
	.flags = DISPLAY_FLAGS_DE_HIGH,
};

static const struct panel_desc chefree_ch101olhlwh_002 = {
	.timings = &chefree_ch101olhlwh_002_timing,
	.num_timings = 1,
	.bpc = 8,
	.size = {
		.width = 217,
		.height = 135,
	},
	.delay = {
		.enable = 200,
		.disable = 200,
	},
	.bus_flags = DRM_BUS_FLAG_DE_HIGH,
	.bus_format = MEDIA_BUS_FMT_RGB888_1X7X4_SPWG,
	.connector_type = DRM_MODE_CONNECTOR_LVDS,
};

static const struct drm_display_mode chunghwa_claa070wp03xg_mode = {
	.clock = 66770,
	.hdisplay = 800,
	.hsync_start = 800 + 49,
	.hsync_end = 800 + 49 + 33,
	.htotal = 800 + 49 + 33 + 17,
	.vdisplay = 1280,
	.vsync_start = 1280 + 1,
	.vsync_end = 1280 + 1 + 7,
	.vtotal = 1280 + 1 + 7 + 15,
	.flags = DRM_MODE_FLAG_NVSYNC | DRM_MODE_FLAG_NHSYNC,
};

static const struct panel_desc chunghwa_claa070wp03xg = {
	.modes = &chunghwa_claa070wp03xg_mode,
	.num_modes = 1,
	.bpc = 6,
	.size = {
		.width = 94,
		.height = 150,
	},
	.bus_format = MEDIA_BUS_FMT_RGB666_1X7X3_SPWG,
	.bus_flags = DRM_BUS_FLAG_DE_HIGH,
	.connector_type = DRM_MODE_CONNECTOR_LVDS,
};

static const struct drm_display_mode chunghwa_claa101wa01a_mode = {
	.clock = 72070,
	.hdisplay = 1366,
	.hsync_start = 1366 + 58,
	.hsync_end = 1366 + 58 + 58,
	.htotal = 1366 + 58 + 58 + 58,
	.vdisplay = 768,
	.vsync_start = 768 + 4,
	.vsync_end = 768 + 4 + 4,
	.vtotal = 768 + 4 + 4 + 4,
};

static const struct panel_desc chunghwa_claa101wa01a = {
	.modes = &chunghwa_claa101wa01a_mode,
	.num_modes = 1,
	.bpc = 6,
	.size = {
		.width = 220,
		.height = 120,
	},
	.bus_format = MEDIA_BUS_FMT_RGB666_1X7X3_SPWG,
	.bus_flags = DRM_BUS_FLAG_DE_HIGH,
	.connector_type = DRM_MODE_CONNECTOR_LVDS,
};

static const struct drm_display_mode chunghwa_claa101wb01_mode = {
	.clock = 69300,
	.hdisplay = 1366,
	.hsync_start = 1366 + 48,
	.hsync_end = 1366 + 48 + 32,
	.htotal = 1366 + 48 + 32 + 20,
	.vdisplay = 768,
	.vsync_start = 768 + 16,
	.vsync_end = 768 + 16 + 8,
	.vtotal = 768 + 16 + 8 + 16,
};

static const struct panel_desc chunghwa_claa101wb01 = {
	.modes = &chunghwa_claa101wb01_mode,
	.num_modes = 1,
	.bpc = 6,
	.size = {
		.width = 223,
		.height = 125,
	},
	.bus_format = MEDIA_BUS_FMT_RGB666_1X7X3_SPWG,
	.bus_flags = DRM_BUS_FLAG_DE_HIGH,
	.connector_type = DRM_MODE_CONNECTOR_LVDS,
};

static const struct display_timing dataimage_fg040346dsswbg04_timing = {
	.pixelclock = { 5000000, 9000000, 12000000 },
	.hactive = { 480, 480, 480 },
	.hfront_porch = { 12, 12, 12 },
	.hback_porch = { 12, 12, 12 },
	.hsync_len = { 21, 21, 21 },
	.vactive = { 272, 272, 272 },
	.vfront_porch = { 4, 4, 4 },
	.vback_porch = { 4, 4, 4 },
	.vsync_len = { 8, 8, 8 },
};

static const struct panel_desc dataimage_fg040346dsswbg04 = {
	.timings = &dataimage_fg040346dsswbg04_timing,
	.num_timings = 1,
	.bpc = 8,
	.size = {
		.width = 95,
		.height = 54,
	},
	.bus_format = MEDIA_BUS_FMT_RGB888_1X24,
	.bus_flags = DRM_BUS_FLAG_DE_HIGH | DRM_BUS_FLAG_PIXDATA_DRIVE_POSEDGE,
	.connector_type = DRM_MODE_CONNECTOR_DPI,
};

static const struct display_timing dataimage_fg1001l0dsswmg01_timing = {
	.pixelclock = { 68900000, 71110000, 73400000 },
	.hactive = { 1280, 1280, 1280 },
	.vactive = { 800, 800, 800 },
	.hback_porch = { 100, 100, 100 },
	.hfront_porch = { 100, 100, 100 },
	.vback_porch = { 5, 5, 5 },
	.vfront_porch = { 5, 5, 5 },
	.hsync_len = { 24, 24, 24 },
	.vsync_len = { 3, 3, 3 },
	.flags = DISPLAY_FLAGS_DE_HIGH | DISPLAY_FLAGS_PIXDATA_POSEDGE |
		 DISPLAY_FLAGS_HSYNC_LOW | DISPLAY_FLAGS_VSYNC_LOW,
};

static const struct panel_desc dataimage_fg1001l0dsswmg01 = {
	.timings = &dataimage_fg1001l0dsswmg01_timing,
	.num_timings = 1,
	.bpc = 8,
	.size = {
		.width = 217,
		.height = 136,
	},
};

static const struct drm_display_mode dataimage_scf0700c48ggu18_mode = {
	.clock = 33260,
	.hdisplay = 800,
	.hsync_start = 800 + 40,
	.hsync_end = 800 + 40 + 128,
	.htotal = 800 + 40 + 128 + 88,
	.vdisplay = 480,
	.vsync_start = 480 + 10,
	.vsync_end = 480 + 10 + 2,
	.vtotal = 480 + 10 + 2 + 33,
	.flags = DRM_MODE_FLAG_NVSYNC | DRM_MODE_FLAG_NHSYNC,
};

static const struct panel_desc dataimage_scf0700c48ggu18 = {
	.modes = &dataimage_scf0700c48ggu18_mode,
	.num_modes = 1,
	.bpc = 8,
	.size = {
		.width = 152,
		.height = 91,
	},
	.bus_format = MEDIA_BUS_FMT_RGB888_1X24,
	.bus_flags = DRM_BUS_FLAG_DE_HIGH | DRM_BUS_FLAG_PIXDATA_DRIVE_POSEDGE,
};

static const struct display_timing dlc_dlc0700yzg_1_timing = {
	.pixelclock = { 45000000, 51200000, 57000000 },
	.hactive = { 1024, 1024, 1024 },
	.hfront_porch = { 100, 106, 113 },
	.hback_porch = { 100, 106, 113 },
	.hsync_len = { 100, 108, 114 },
	.vactive = { 600, 600, 600 },
	.vfront_porch = { 8, 11, 15 },
	.vback_porch = { 8, 11, 15 },
	.vsync_len = { 9, 13, 15 },
	.flags = DISPLAY_FLAGS_DE_HIGH,
};

static const struct panel_desc dlc_dlc0700yzg_1 = {
	.timings = &dlc_dlc0700yzg_1_timing,
	.num_timings = 1,
	.bpc = 6,
	.size = {
		.width = 154,
		.height = 86,
	},
	.delay = {
		.prepare = 30,
		.enable = 200,
		.disable = 200,
	},
	.bus_format = MEDIA_BUS_FMT_RGB666_1X7X3_SPWG,
	.connector_type = DRM_MODE_CONNECTOR_LVDS,
};

static const struct display_timing dlc_dlc1010gig_timing = {
	.pixelclock = { 68900000, 71100000, 73400000 },
	.hactive = { 1280, 1280, 1280 },
	.hfront_porch = { 43, 53, 63 },
	.hback_porch = { 43, 53, 63 },
	.hsync_len = { 44, 54, 64 },
	.vactive = { 800, 800, 800 },
	.vfront_porch = { 5, 8, 11 },
	.vback_porch = { 5, 8, 11 },
	.vsync_len = { 5, 7, 11 },
	.flags = DISPLAY_FLAGS_DE_HIGH,
};

static const struct panel_desc dlc_dlc1010gig = {
	.timings = &dlc_dlc1010gig_timing,
	.num_timings = 1,
	.bpc = 8,
	.size = {
		.width = 216,
		.height = 135,
	},
	.delay = {
		.prepare = 60,
		.enable = 150,
		.disable = 100,
		.unprepare = 60,
	},
	.bus_format = MEDIA_BUS_FMT_RGB888_1X7X4_SPWG,
	.connector_type = DRM_MODE_CONNECTOR_LVDS,
};

static const struct drm_display_mode edt_et035012dm6_mode = {
	.clock = 6500,
	.hdisplay = 320,
	.hsync_start = 320 + 20,
	.hsync_end = 320 + 20 + 30,
	.htotal = 320 + 20 + 68,
	.vdisplay = 240,
	.vsync_start = 240 + 4,
	.vsync_end = 240 + 4 + 4,
	.vtotal = 240 + 4 + 4 + 14,
	.flags = DRM_MODE_FLAG_NVSYNC | DRM_MODE_FLAG_NHSYNC,
};

static const struct panel_desc edt_et035012dm6 = {
	.modes = &edt_et035012dm6_mode,
	.num_modes = 1,
	.bpc = 8,
	.size = {
		.width = 70,
		.height = 52,
	},
	.bus_format = MEDIA_BUS_FMT_RGB888_1X24,
	.bus_flags = DRM_BUS_FLAG_DE_LOW | DRM_BUS_FLAG_PIXDATA_SAMPLE_POSEDGE,
};

static const struct drm_display_mode edt_etm0350g0dh6_mode = {
	.clock = 6520,
	.hdisplay = 320,
	.hsync_start = 320 + 20,
	.hsync_end = 320 + 20 + 68,
	.htotal = 320 + 20 + 68,
	.vdisplay = 240,
	.vsync_start = 240 + 4,
	.vsync_end = 240 + 4 + 18,
	.vtotal = 240 + 4 + 18,
	.flags = DRM_MODE_FLAG_NVSYNC | DRM_MODE_FLAG_NHSYNC,
};

static const struct panel_desc edt_etm0350g0dh6 = {
	.modes = &edt_etm0350g0dh6_mode,
	.num_modes = 1,
	.bpc = 6,
	.size = {
		.width = 70,
		.height = 53,
	},
	.bus_format = MEDIA_BUS_FMT_RGB888_1X24,
	.bus_flags = DRM_BUS_FLAG_DE_HIGH | DRM_BUS_FLAG_PIXDATA_DRIVE_NEGEDGE,
	.connector_type = DRM_MODE_CONNECTOR_DPI,
};

static const struct drm_display_mode edt_etm043080dh6gp_mode = {
	.clock = 10870,
	.hdisplay = 480,
	.hsync_start = 480 + 8,
	.hsync_end = 480 + 8 + 4,
	.htotal = 480 + 8 + 4 + 41,

	/*
	 * IWG22M: Y resolution changed for "dc_linuxfb" module crashing while
	 * fb_align
	 */

	.vdisplay = 288,
	.vsync_start = 288 + 2,
	.vsync_end = 288 + 2 + 4,
	.vtotal = 288 + 2 + 4 + 10,
};

static const struct panel_desc edt_etm043080dh6gp = {
	.modes = &edt_etm043080dh6gp_mode,
	.num_modes = 1,
	.bpc = 8,
	.size = {
		.width = 100,
		.height = 65,
	},
	.bus_format = MEDIA_BUS_FMT_RGB666_1X18,
	.connector_type = DRM_MODE_CONNECTOR_DPI,
};

static const struct drm_display_mode edt_etm0430g0dh6_mode = {
	.clock = 9000,
	.hdisplay = 480,
	.hsync_start = 480 + 2,
	.hsync_end = 480 + 2 + 41,
	.htotal = 480 + 2 + 41 + 2,
	.vdisplay = 272,
	.vsync_start = 272 + 2,
	.vsync_end = 272 + 2 + 10,
	.vtotal = 272 + 2 + 10 + 2,
	.flags = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC,
};

static const struct panel_desc edt_etm0430g0dh6 = {
	.modes = &edt_etm0430g0dh6_mode,
	.num_modes = 1,
	.bpc = 6,
	.size = {
		.width = 95,
		.height = 54,
	},
	.bus_format = MEDIA_BUS_FMT_RGB666_1X18,
	.bus_flags = DRM_BUS_FLAG_DE_HIGH | DRM_BUS_FLAG_PIXDATA_SAMPLE_POSEDGE,
	.connector_type = DRM_MODE_CONNECTOR_DPI,
};

static const struct drm_display_mode edt_et057090dhu_mode = {
	.clock = 25175,
	.hdisplay = 640,
	.hsync_start = 640 + 16,
	.hsync_end = 640 + 16 + 30,
	.htotal = 640 + 16 + 30 + 114,
	.vdisplay = 480,
	.vsync_start = 480 + 10,
	.vsync_end = 480 + 10 + 3,
	.vtotal = 480 + 10 + 3 + 32,
	.flags = DRM_MODE_FLAG_NVSYNC | DRM_MODE_FLAG_NHSYNC,
};

static const struct panel_desc edt_et057090dhu = {
	.modes = &edt_et057090dhu_mode,
	.num_modes = 1,
	.bpc = 6,
	.size = {
		.width = 115,
		.height = 86,
	},
	.bus_format = MEDIA_BUS_FMT_RGB666_1X18,
	.bus_flags = DRM_BUS_FLAG_DE_HIGH | DRM_BUS_FLAG_PIXDATA_DRIVE_NEGEDGE,
	.connector_type = DRM_MODE_CONNECTOR_DPI,
};

static const struct drm_display_mode edt_etm0700g0dh6_mode = {
	.clock = 33260,
	.hdisplay = 800,
	.hsync_start = 800 + 40,
	.hsync_end = 800 + 40 + 128,
	.htotal = 800 + 40 + 128 + 88,
	.vdisplay = 480,
	.vsync_start = 480 + 10,
	.vsync_end = 480 + 10 + 2,
	.vtotal = 480 + 10 + 2 + 33,
	.flags = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC,
};

static const struct panel_desc edt_etm0700g0dh6 = {
	.modes = &edt_etm0700g0dh6_mode,
	.num_modes = 1,
	.bpc = 6,
	.size = {
		.width = 152,
		.height = 91,
	},
	.bus_format = MEDIA_BUS_FMT_RGB666_1X18,
	.bus_flags = DRM_BUS_FLAG_DE_HIGH | DRM_BUS_FLAG_PIXDATA_DRIVE_NEGEDGE,
	.connector_type = DRM_MODE_CONNECTOR_DPI,
};

static const struct panel_desc edt_etm0700g0bdh6 = {
	.modes = &edt_etm0700g0dh6_mode,
	.num_modes = 1,
	.bpc = 6,
	.size = {
		.width = 152,
		.height = 91,
	},
	.bus_format = MEDIA_BUS_FMT_RGB666_1X18,
	.bus_flags = DRM_BUS_FLAG_DE_HIGH | DRM_BUS_FLAG_PIXDATA_DRIVE_POSEDGE,
	.connector_type = DRM_MODE_CONNECTOR_DPI,
};

static const struct display_timing edt_etml0700y5dha_timing = {
	.pixelclock = { 40800000, 51200000, 67200000 },
	.hactive = { 1024, 1024, 1024 },
	.hfront_porch = { 30, 106, 125 },
	.hback_porch = { 30, 106, 125 },
	.hsync_len = { 30, 108, 126 },
	.vactive = { 600, 600, 600 },
	.vfront_porch = { 3, 12, 67},
	.vback_porch = { 3, 12, 67 },
	.vsync_len = { 4, 11, 66 },
	.flags = DISPLAY_FLAGS_HSYNC_LOW | DISPLAY_FLAGS_VSYNC_LOW |
		 DISPLAY_FLAGS_DE_HIGH,
};

static const struct panel_desc edt_etml0700y5dha = {
	.timings = &edt_etml0700y5dha_timing,
	.num_timings = 1,
	.bpc = 8,
	.size = {
		.width = 155,
		.height = 86,
	},
	.bus_format = MEDIA_BUS_FMT_RGB888_1X7X4_SPWG,
	.connector_type = DRM_MODE_CONNECTOR_LVDS,
};

static const struct display_timing edt_etml1010g3dra_timing = {
	.pixelclock = { 66300000, 72400000, 78900000 },
	.hactive = { 1280, 1280, 1280 },
	.hfront_porch = { 12, 72, 132 },
	.hback_porch = { 86, 86, 86 },
	.hsync_len = { 2, 2, 2 },
	.vactive = { 800, 800, 800 },
	.vfront_porch = { 1, 15, 49 },
	.vback_porch = { 21, 21, 21 },
	.vsync_len = { 2, 2, 2 },
	.flags = DISPLAY_FLAGS_VSYNC_LOW | DISPLAY_FLAGS_HSYNC_LOW |
		 DISPLAY_FLAGS_DE_HIGH,
};

static const struct panel_desc edt_etml1010g3dra = {
	.timings = &edt_etml1010g3dra_timing,
	.num_timings = 1,
	.bpc = 8,
	.size = {
		.width = 216,
		.height = 135,
	},
	.bus_format = MEDIA_BUS_FMT_RGB888_1X7X4_SPWG,
	.bus_flags = DRM_BUS_FLAG_DE_HIGH,
	.connector_type = DRM_MODE_CONNECTOR_LVDS,
};

static const struct drm_display_mode edt_etmv570g2dhu_mode = {
	.clock = 25175,
	.hdisplay = 640,
	.hsync_start = 640,
	.hsync_end = 640 + 16,
	.htotal = 640 + 16 + 30 + 114,
	.vdisplay = 480,
	.vsync_start = 480 + 10,
	.vsync_end = 480 + 10 + 3,
	.vtotal = 480 + 10 + 3 + 35,
	.flags = DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PHSYNC,
};

static const struct panel_desc edt_etmv570g2dhu = {
	.modes = &edt_etmv570g2dhu_mode,
	.num_modes = 1,
	.bpc = 6,
	.size = {
		.width = 115,
		.height = 86,
	},
	.bus_format = MEDIA_BUS_FMT_RGB888_1X24,
	.bus_flags = DRM_BUS_FLAG_DE_HIGH | DRM_BUS_FLAG_PIXDATA_DRIVE_NEGEDGE,
	.connector_type = DRM_MODE_CONNECTOR_DPI,
};

static const struct display_timing eink_vb3300_kca_timing = {
	.pixelclock = { 40000000, 40000000, 40000000 },
	.hactive = { 334, 334, 334 },
	.hfront_porch = { 1, 1, 1 },
	.hback_porch = { 1, 1, 1 },
	.hsync_len = { 1, 1, 1 },
	.vactive = { 1405, 1405, 1405 },
	.vfront_porch = { 1, 1, 1 },
	.vback_porch = { 1, 1, 1 },
	.vsync_len = { 1, 1, 1 },
	.flags = DISPLAY_FLAGS_HSYNC_LOW | DISPLAY_FLAGS_VSYNC_LOW |
		 DISPLAY_FLAGS_DE_HIGH | DISPLAY_FLAGS_PIXDATA_POSEDGE,
};

static const struct panel_desc eink_vb3300_kca = {
	.timings = &eink_vb3300_kca_timing,
	.num_timings = 1,
	.bpc = 6,
	.size = {
		.width = 157,
		.height = 209,
	},
	.bus_format = MEDIA_BUS_FMT_RGB888_1X24,
	.bus_flags = DRM_BUS_FLAG_DE_HIGH | DRM_BUS_FLAG_PIXDATA_DRIVE_POSEDGE,
	.connector_type = DRM_MODE_CONNECTOR_DPI,
};

static const struct display_timing evervision_vgg644804_timing = {
	.pixelclock = { 25175000, 25175000, 25175000 },
	.hactive = { 640, 640, 640 },
	.hfront_porch = { 16, 16, 16 },
	.hback_porch = { 82, 114, 170 },
	.hsync_len = { 5, 30, 30 },
	.vactive = { 480, 480, 480 },
	.vfront_porch = { 10, 10, 10 },
	.vback_porch = { 30, 32, 34 },
	.vsync_len = { 1, 3, 5 },
	.flags = DISPLAY_FLAGS_HSYNC_LOW | DISPLAY_FLAGS_VSYNC_LOW |
		 DISPLAY_FLAGS_DE_HIGH | DISPLAY_FLAGS_PIXDATA_POSEDGE |
		 DISPLAY_FLAGS_SYNC_POSEDGE,
};

static const struct panel_desc evervision_vgg644804 = {
	.timings = &evervision_vgg644804_timing,
	.num_timings = 1,
	.bpc = 6,
	.size = {
		.width = 115,
		.height = 86,
	},
	.bus_format = MEDIA_BUS_FMT_RGB666_1X7X3_SPWG,
	.bus_flags = DRM_BUS_FLAG_DE_HIGH,
	.connector_type = DRM_MODE_CONNECTOR_LVDS,
};

static const struct display_timing evervision_vgg804821_timing = {
	.pixelclock = { 27600000, 33300000, 50000000 },
	.hactive = { 800, 800, 800 },
	.hfront_porch = { 40, 66, 70 },
	.hback_porch = { 40, 67, 70 },
	.hsync_len = { 40, 67, 70 },
	.vactive = { 480, 480, 480 },
	.vfront_porch = { 6, 10, 10 },
	.vback_porch = { 7, 11, 11 },
	.vsync_len = { 7, 11, 11 },
	.flags = DISPLAY_FLAGS_HSYNC_HIGH | DISPLAY_FLAGS_VSYNC_HIGH |
		 DISPLAY_FLAGS_DE_HIGH | DISPLAY_FLAGS_PIXDATA_NEGEDGE |
		 DISPLAY_FLAGS_SYNC_NEGEDGE,
};

static const struct panel_desc evervision_vgg804821 = {
	.timings = &evervision_vgg804821_timing,
	.num_timings = 1,
	.bpc = 8,
	.size = {
		.width = 108,
		.height = 64,
	},
	.bus_format = MEDIA_BUS_FMT_RGB888_1X24,
	.bus_flags = DRM_BUS_FLAG_DE_HIGH | DRM_BUS_FLAG_PIXDATA_SAMPLE_POSEDGE,
};

static const struct drm_display_mode foxlink_fl500wvr00_a0t_mode = {
	.clock = 32260,
	.hdisplay = 800,
	.hsync_start = 800 + 168,
	.hsync_end = 800 + 168 + 64,
	.htotal = 800 + 168 + 64 + 88,
	.vdisplay = 480,
	.vsync_start = 480 + 37,
	.vsync_end = 480 + 37 + 2,
	.vtotal = 480 + 37 + 2 + 8,
};

static const struct panel_desc foxlink_fl500wvr00_a0t = {
	.modes = &foxlink_fl500wvr00_a0t_mode,
	.num_modes = 1,
	.bpc = 8,
	.size = {
		.width = 108,
		.height = 65,
	},
	.bus_format = MEDIA_BUS_FMT_RGB888_1X24,
};

static const struct drm_display_mode frida_frd350h54004_modes[] = {
	{ /* 60 Hz */
		.clock = 6000,
		.hdisplay = 320,
		.hsync_start = 320 + 44,
		.hsync_end = 320 + 44 + 16,
		.htotal = 320 + 44 + 16 + 20,
		.vdisplay = 240,
		.vsync_start = 240 + 2,
		.vsync_end = 240 + 2 + 6,
		.vtotal = 240 + 2 + 6 + 2,
		.flags = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC,
	},
	{ /* 50 Hz */
		.clock = 5400,
		.hdisplay = 320,
		.hsync_start = 320 + 56,
		.hsync_end = 320 + 56 + 16,
		.htotal = 320 + 56 + 16 + 40,
		.vdisplay = 240,
		.vsync_start = 240 + 2,
		.vsync_end = 240 + 2 + 6,
		.vtotal = 240 + 2 + 6 + 2,
		.flags = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC,
	},
};

static const struct panel_desc frida_frd350h54004 = {
	.modes = frida_frd350h54004_modes,
	.num_modes = ARRAY_SIZE(frida_frd350h54004_modes),
	.bpc = 8,
	.size = {
		.width = 77,
		.height = 64,
	},
	.bus_format = MEDIA_BUS_FMT_RGB888_1X24,
	.bus_flags = DRM_BUS_FLAG_DE_HIGH | DRM_BUS_FLAG_PIXDATA_SAMPLE_NEGEDGE,
	.connector_type = DRM_MODE_CONNECTOR_DPI,
};

static const struct drm_display_mode friendlyarm_hd702e_mode = {
	.clock		= 67185,
	.hdisplay	= 800,
	.hsync_start	= 800 + 20,
	.hsync_end	= 800 + 20 + 24,
	.htotal		= 800 + 20 + 24 + 20,
	.vdisplay	= 1280,
	.vsync_start	= 1280 + 4,
	.vsync_end	= 1280 + 4 + 8,
	.vtotal		= 1280 + 4 + 8 + 4,
	.flags		= DRM_MODE_FLAG_NVSYNC | DRM_MODE_FLAG_NHSYNC,
};

static const struct panel_desc friendlyarm_hd702e = {
	.modes = &friendlyarm_hd702e_mode,
	.num_modes = 1,
	.size = {
		.width	= 94,
		.height	= 151,
	},
};

static const struct drm_display_mode geekworm_mzp280_mode = {
	.clock = 32000,
	.hdisplay = 480,
	.hsync_start = 480 + 41,
	.hsync_end = 480 + 41 + 20,
	.htotal = 480 + 41 + 20 + 60,
	.vdisplay = 640,
	.vsync_start = 640 + 5,
	.vsync_end = 640 + 5 + 10,
	.vtotal = 640 + 5 + 10 + 10,
	.flags = DRM_MODE_FLAG_NVSYNC | DRM_MODE_FLAG_NHSYNC,
};

static const struct panel_desc geekworm_mzp280 = {
	.modes = &geekworm_mzp280_mode,
	.num_modes = 1,
	.bpc = 6,
	.size = {
		.width = 47,
		.height = 61,
	},
	.bus_format = MEDIA_BUS_FMT_RGB565_1X24_CPADHI,
	.bus_flags = DRM_BUS_FLAG_DE_HIGH | DRM_BUS_FLAG_PIXDATA_DRIVE_NEGEDGE,
	.connector_type = DRM_MODE_CONNECTOR_DPI,
};

static const struct drm_display_mode giantplus_gpg482739qs5_mode = {
	.clock = 9000,
	.hdisplay = 480,
	.hsync_start = 480 + 5,
	.hsync_end = 480 + 5 + 1,
	.htotal = 480 + 5 + 1 + 40,
	.vdisplay = 272,
	.vsync_start = 272 + 8,
	.vsync_end = 272 + 8 + 1,
	.vtotal = 272 + 8 + 1 + 8,
};

static const struct panel_desc giantplus_gpg482739qs5 = {
	.modes = &giantplus_gpg482739qs5_mode,
	.num_modes = 1,
	.bpc = 8,
	.size = {
		.width = 95,
		.height = 54,
	},
	.bus_format = MEDIA_BUS_FMT_RGB888_1X24,
};

static const struct display_timing giantplus_gpm940b0_timing = {
	.pixelclock = { 13500000, 27000000, 27500000 },
	.hactive = { 320, 320, 320 },
	.hfront_porch = { 14, 686, 718 },
	.hback_porch = { 50, 70, 255 },
	.hsync_len = { 1, 1, 1 },
	.vactive = { 240, 240, 240 },
	.vfront_porch = { 1, 1, 179 },
	.vback_porch = { 1, 21, 31 },
	.vsync_len = { 1, 1, 6 },
	.flags = DISPLAY_FLAGS_HSYNC_LOW | DISPLAY_FLAGS_VSYNC_LOW,
};

static const struct panel_desc giantplus_gpm940b0 = {
	.timings = &giantplus_gpm940b0_timing,
	.num_timings = 1,
	.bpc = 8,
	.size = {
		.width = 60,
		.height = 45,
	},
	.bus_format = MEDIA_BUS_FMT_RGB888_3X8,
	.bus_flags = DRM_BUS_FLAG_DE_HIGH | DRM_BUS_FLAG_PIXDATA_SAMPLE_POSEDGE,
};

static const struct display_timing hannstar_hsd070pww1_timing = {
	.pixelclock = { 64300000, 71100000, 82000000 },
	.hactive = { 1280, 1280, 1280 },
	.hfront_porch = { 1, 1, 10 },
	.hback_porch = { 1, 1, 10 },
	/*
	 * According to the data sheet, the minimum horizontal blanking interval
	 * is 54 clocks (1 + 52 + 1), but tests with a Nitrogen6X have shown the
	 * minimum working horizontal blanking interval to be 60 clocks.
	 */
	.hsync_len = { 58, 158, 661 },
	.vactive = { 800, 800, 800 },
	.vfront_porch = { 1, 1, 10 },
	.vback_porch = { 1, 1, 10 },
	.vsync_len = { 1, 21, 203 },
	.flags = DISPLAY_FLAGS_DE_HIGH,
};

static const struct panel_desc hannstar_hsd070pww1 = {
	.timings = &hannstar_hsd070pww1_timing,
	.num_timings = 1,
	.bpc = 6,
	.size = {
		.width = 151,
		.height = 94,
	},
	.bus_format = MEDIA_BUS_FMT_RGB666_1X7X3_SPWG,
	.connector_type = DRM_MODE_CONNECTOR_LVDS,
};

static const struct display_timing hannstar_hsd100pxn1_timing = {
	.pixelclock = { 55000000, 65000000, 75000000 },
	.hactive = { 1024, 1024, 1024 },
	.hfront_porch = { 40, 40, 40 },
	.hback_porch = { 220, 220, 220 },
	.hsync_len = { 20, 60, 100 },
	.vactive = { 768, 768, 768 },
	.vfront_porch = { 7, 7, 7 },
	.vback_porch = { 21, 21, 21 },
	.vsync_len = { 10, 10, 10 },
	.flags = DISPLAY_FLAGS_DE_HIGH,
};

static const struct panel_desc hannstar_hsd100pxn1 = {
	.timings = &hannstar_hsd100pxn1_timing,
	.num_timings = 1,
	.bpc = 6,
	.size = {
		.width = 203,
		.height = 152,
	},
	.bus_format = MEDIA_BUS_FMT_RGB666_1X7X3_SPWG,
	.connector_type = DRM_MODE_CONNECTOR_LVDS,
};

static const struct display_timing hannstar_hsd101pww2_timing = {
	.pixelclock = { 64300000, 71100000, 82000000 },
	.hactive = { 1280, 1280, 1280 },
	.hfront_porch = { 1, 1, 10 },
	.hback_porch = { 1, 1, 10 },
	.hsync_len = { 58, 158, 661 },
	.vactive = { 800, 800, 800 },
	.vfront_porch = { 1, 1, 10 },
	.vback_porch = { 1, 1, 10 },
	.vsync_len = { 1, 21, 203 },
	.flags = DISPLAY_FLAGS_DE_HIGH,
};

static const struct panel_desc hannstar_hsd101pww2 = {
	.timings = &hannstar_hsd101pww2_timing,
	.num_timings = 1,
	.bpc = 8,
	.size = {
		.width = 217,
		.height = 136,
	},
	.bus_format = MEDIA_BUS_FMT_RGB888_1X7X4_SPWG,
	.connector_type = DRM_MODE_CONNECTOR_LVDS,
};

static const struct drm_display_mode hitachi_tx23d38vm0caa_mode = {
	.clock = 33333,
	.hdisplay = 800,
	.hsync_start = 800 + 85,
	.hsync_end = 800 + 85 + 86,
	.htotal = 800 + 85 + 86 + 85,
	.vdisplay = 480,
	.vsync_start = 480 + 16,
	.vsync_end = 480 + 16 + 13,
	.vtotal = 480 + 16 + 13 + 16,
};

static const struct panel_desc hitachi_tx23d38vm0caa = {
	.modes = &hitachi_tx23d38vm0caa_mode,
	.num_modes = 1,
	.bpc = 6,
	.size = {
		.width = 195,
		.height = 117,
	},
	.delay = {
		.enable = 160,
		.disable = 160,
	},
};

static const struct drm_display_mode innolux_at043tn24_mode = {
	.clock = 9000,
	.hdisplay = 480,
	.hsync_start = 480 + 2,
	.hsync_end = 480 + 2 + 41,
	.htotal = 480 + 2 + 41 + 2,
	.vdisplay = 272,
	.vsync_start = 272 + 2,
	.vsync_end = 272 + 2 + 10,
	.vtotal = 272 + 2 + 10 + 2,
	.flags = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC,
};

static const struct panel_desc innolux_at043tn24 = {
	.modes = &innolux_at043tn24_mode,
	.num_modes = 1,
	.bpc = 8,
	.size = {
		.width = 95,
		.height = 54,
	},
	.bus_format = MEDIA_BUS_FMT_RGB888_1X24,
	.connector_type = DRM_MODE_CONNECTOR_DPI,
	.bus_flags = DRM_BUS_FLAG_DE_HIGH | DRM_BUS_FLAG_PIXDATA_DRIVE_POSEDGE,
};

static const struct display_timing innolux_at056tn53v1_timing = {
	.pixelclock = { 39700000, 39700000, 39700000},
	.hactive = { 640, 640, 640 },
	.hfront_porch = { 16, 16, 16 },
	.hback_porch = { 134, 134, 134 },
	.hsync_len = { 10, 10, 10},
	.vactive = { 480, 480, 480 },
	.vfront_porch = { 32, 32, 32},
	.vback_porch = { 11, 11, 11 },
	.vsync_len = { 2, 2, 2 },
	.flags = DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PHSYNC,
};

static const struct panel_desc innolux_at056tn53v1 = {
	.timings = &innolux_at056tn53v1_timing,
	.num_timings = 1,
	.bpc = 6,
	.size = {
		.width = 112,
		.height = 84,
	},
	.delay = {
		.prepare = 50,
		.enable = 200,
		.disable = 110,
		.unprepare = 200,
	},
	.bus_format = MEDIA_BUS_FMT_BGR666_1X24_CPADHI,
	.bus_flags = DRM_BUS_FLAG_PIXDATA_SAMPLE_POSEDGE,
	.connector_type = DRM_MODE_CONNECTOR_DPI,
};

static const struct drm_display_mode innolux_at070tn92_mode = {
	.clock = 33333,
	.hdisplay = 800,
	.hsync_start = 800 + 210,
	.hsync_end = 800 + 210 + 20,
	.htotal = 800 + 210 + 20 + 46,
	.vdisplay = 480,
	.vsync_start = 480 + 22,
	.vsync_end = 480 + 22 + 10,
	.vtotal = 480 + 22 + 23 + 10,
};

static const struct panel_desc innolux_at070tn92 = {
	.modes = &innolux_at070tn92_mode,
	.num_modes = 1,
	.size = {
		.width = 154,
		.height = 86,
	},
	.bus_format = MEDIA_BUS_FMT_RGB888_1X24,
};

static const struct display_timing innolux_g070ace_l01_timing = {
	.pixelclock = { 25200000, 35000000, 35700000 },
	.hactive = { 800, 800, 800 },
	.hfront_porch = { 30, 32, 87 },
	.hback_porch = { 30, 32, 87 },
	.hsync_len = { 1, 1, 1 },
	.vactive = { 480, 480, 480 },
	.vfront_porch = { 3, 3, 3 },
	.vback_porch = { 13, 13, 13 },
	.vsync_len = { 1, 1, 4 },
	.flags = DISPLAY_FLAGS_DE_HIGH,
};

static const struct panel_desc innolux_g070ace_l01 = {
	.timings = &innolux_g070ace_l01_timing,
	.num_timings = 1,
	.bpc = 8,
	.size = {
		.width = 152,
		.height = 91,
	},
	.delay = {
		.prepare = 10,
		.enable = 50,
		.disable = 50,
		.unprepare = 500,
	},
	.bus_format = MEDIA_BUS_FMT_RGB888_1X7X4_SPWG,
	.bus_flags = DRM_BUS_FLAG_DE_HIGH,
	.connector_type = DRM_MODE_CONNECTOR_LVDS,
};

static const struct display_timing innolux_g070y2_l01_timing = {
	.pixelclock = { 28000000, 29500000, 32000000 },
	.hactive = { 800, 800, 800 },
	.hfront_porch = { 61, 91, 141 },
	.hback_porch = { 60, 90, 140 },
	.hsync_len = { 12, 12, 12 },
	.vactive = { 480, 480, 480 },
	.vfront_porch = { 4, 9, 30 },
	.vback_porch = { 4, 8, 28 },
	.vsync_len = { 2, 2, 2 },
	.flags = DISPLAY_FLAGS_DE_HIGH,
};

static const struct panel_desc innolux_g070y2_l01 = {
	.timings = &innolux_g070y2_l01_timing,
	.num_timings = 1,
	.bpc = 8,
	.size = {
		.width = 152,
		.height = 91,
	},
	.delay = {
		.prepare = 10,
		.enable = 100,
		.disable = 100,
		.unprepare = 800,
	},
	.bus_format = MEDIA_BUS_FMT_RGB888_1X7X4_SPWG,
	.bus_flags = DRM_BUS_FLAG_DE_HIGH,
	.connector_type = DRM_MODE_CONNECTOR_LVDS,
};

static const struct display_timing innolux_g070ace_lh3_timing = {
	.pixelclock = { 25200000, 25400000, 35700000 },
	.hactive = { 800, 800, 800 },
	.hfront_porch = { 30, 32, 87 },
	.hback_porch = { 29, 31, 86 },
	.hsync_len = { 1, 1, 1 },
	.vactive = { 480, 480, 480 },
	.vfront_porch = { 4, 5, 65 },
	.vback_porch = { 3, 4, 65 },
	.vsync_len = { 1, 1, 1 },
	.flags = DISPLAY_FLAGS_DE_HIGH,
};

static const struct panel_desc innolux_g070ace_lh3 = {
	.timings = &innolux_g070ace_lh3_timing,
	.num_timings = 1,
	.bpc = 8,
	.size = {
		.width = 152,
		.height = 91,
	},
	.delay = {
		.prepare = 10,
		.enable = 450,
		.disable = 200,
		.unprepare = 510,
	},
	.bus_format = MEDIA_BUS_FMT_RGB888_1X7X4_SPWG,
	.bus_flags = DRM_BUS_FLAG_DE_HIGH,
	.connector_type = DRM_MODE_CONNECTOR_LVDS,
};

static const struct drm_display_mode innolux_g070y2_t02_mode = {
	.clock = 33333,
	.hdisplay = 800,
	.hsync_start = 800 + 210,
	.hsync_end = 800 + 210 + 20,
	.htotal = 800 + 210 + 20 + 46,
	.vdisplay = 480,
	.vsync_start = 480 + 22,
	.vsync_end = 480 + 22 + 10,
	.vtotal = 480 + 22 + 23 + 10,
};

static const struct panel_desc innolux_g070y2_t02 = {
	.modes = &innolux_g070y2_t02_mode,
	.num_modes = 1,
	.bpc = 8,
	.size = {
		.width = 152,
		.height = 92,
	},
	.bus_format = MEDIA_BUS_FMT_RGB888_1X24,
	.bus_flags = DRM_BUS_FLAG_DE_HIGH | DRM_BUS_FLAG_PIXDATA_DRIVE_POSEDGE,
	.connector_type = DRM_MODE_CONNECTOR_DPI,
};

static const struct display_timing innolux_g101ice_l01_timing = {
	.pixelclock = { 60400000, 71100000, 74700000 },
	.hactive = { 1280, 1280, 1280 },
	.hfront_porch = { 30, 60, 70 },
	.hback_porch = { 30, 60, 70 },
	.hsync_len = { 22, 40, 60 },
	.vactive = { 800, 800, 800 },
	.vfront_porch = { 3, 8, 14 },
	.vback_porch = { 3, 8, 14 },
	.vsync_len = { 4, 7, 12 },
	.flags = DISPLAY_FLAGS_DE_HIGH,
};

static const struct panel_desc innolux_g101ice_l01 = {
	.timings = &innolux_g101ice_l01_timing,
	.num_timings = 1,
	.bpc = 8,
	.size = {
		.width = 217,
		.height = 135,
	},
	.delay = {
		.enable = 200,
		.disable = 200,
	},
	.bus_format = MEDIA_BUS_FMT_RGB888_1X7X4_SPWG,
	.bus_flags = DRM_BUS_FLAG_DE_HIGH,
	.connector_type = DRM_MODE_CONNECTOR_LVDS,
};

static const struct display_timing innolux_g121i1_l01_timing = {
	.pixelclock = { 67450000, 71000000, 74550000 },
	.hactive = { 1280, 1280, 1280 },
	.hfront_porch = { 40, 80, 160 },
	.hback_porch = { 39, 79, 159 },
	.hsync_len = { 1, 1, 1 },
	.vactive = { 800, 800, 800 },
	.vfront_porch = { 5, 11, 100 },
	.vback_porch = { 4, 11, 99 },
	.vsync_len = { 1, 1, 1 },
};

static const struct panel_desc innolux_g121i1_l01 = {
	.timings = &innolux_g121i1_l01_timing,
	.num_timings = 1,
	.bpc = 6,
	.size = {
		.width = 261,
		.height = 163,
	},
	.delay = {
		.enable = 200,
		.disable = 20,
	},
	.bus_format = MEDIA_BUS_FMT_RGB666_1X7X3_SPWG,
	.connector_type = DRM_MODE_CONNECTOR_LVDS,
};

static const struct display_timing innolux_g121x1_l03_timings = {
	.pixelclock = { 57500000, 64900000, 74400000 },
	.hactive = { 1024, 1024, 1024 },
	.hfront_porch = { 90, 140, 190 },
	.hback_porch = { 90, 140, 190 },
	.hsync_len = { 36, 40, 60 },
	.vactive = { 768, 768, 768 },
	.vfront_porch = { 2, 15, 30 },
	.vback_porch = { 2, 15, 30 },
	.vsync_len = { 2, 8, 20 },
	.flags = DISPLAY_FLAGS_HSYNC_LOW | DISPLAY_FLAGS_VSYNC_LOW,
};

static const struct panel_desc innolux_g121x1_l03 = {
	.timings = &innolux_g121x1_l03_timings,
	.num_timings = 1,
	.bpc = 6,
	.size = {
		.width = 246,
		.height = 185,
	},
	.delay = {
		.enable = 200,
		.unprepare = 200,
		.disable = 400,
	},
	.bus_format = MEDIA_BUS_FMT_RGB666_1X7X3_SPWG,
	.bus_flags = DRM_BUS_FLAG_DE_HIGH,
	.connector_type = DRM_MODE_CONNECTOR_LVDS,
};

static const struct panel_desc innolux_g121xce_l01 = {
	.timings = &innolux_g121x1_l03_timings,
	.num_timings = 1,
	.bpc = 8,
	.size = {
		.width = 246,
		.height = 185,
	},
	.delay = {
		.enable = 200,
		.unprepare = 200,
		.disable = 400,
	},
	.bus_format = MEDIA_BUS_FMT_RGB888_1X7X4_SPWG,
	.bus_flags = DRM_BUS_FLAG_DE_HIGH,
	.connector_type = DRM_MODE_CONNECTOR_LVDS,
};

static const struct display_timing innolux_g156hce_l01_timings = {
	.pixelclock = { 120000000, 141860000, 150000000 },
	.hactive = { 1920, 1920, 1920 },
	.hfront_porch = { 80, 90, 100 },
	.hback_porch = { 80, 90, 100 },
	.hsync_len = { 20, 30, 30 },
	.vactive = { 1080, 1080, 1080 },
	.vfront_porch = { 3, 10, 20 },
	.vback_porch = { 3, 10, 20 },
	.vsync_len = { 4, 10, 10 },
};

static const struct panel_desc innolux_g156hce_l01 = {
	.timings = &innolux_g156hce_l01_timings,
	.num_timings = 1,
	.bpc = 8,
	.size = {
		.width = 344,
		.height = 194,
	},
	.delay = {
		.prepare = 1,		/* T1+T2 */
		.enable = 450,		/* T5 */
		.disable = 200,		/* T6 */
		.unprepare = 10,	/* T3+T7 */
	},
	.bus_format = MEDIA_BUS_FMT_RGB888_1X7X4_SPWG,
	.bus_flags = DRM_BUS_FLAG_DE_HIGH,
	.connector_type = DRM_MODE_CONNECTOR_LVDS,
};

static const struct drm_display_mode innolux_n156bge_l21_mode = {
	.clock = 69300,
	.hdisplay = 1366,
	.hsync_start = 1366 + 16,
	.hsync_end = 1366 + 16 + 34,
	.htotal = 1366 + 16 + 34 + 50,
	.vdisplay = 768,
	.vsync_start = 768 + 2,
	.vsync_end = 768 + 2 + 6,
	.vtotal = 768 + 2 + 6 + 12,
};

static const struct panel_desc innolux_n156bge_l21 = {
	.modes = &innolux_n156bge_l21_mode,
	.num_modes = 1,
	.bpc = 6,
	.size = {
		.width = 344,
		.height = 193,
	},
	.bus_format = MEDIA_BUS_FMT_RGB666_1X7X3_SPWG,
	.bus_flags = DRM_BUS_FLAG_DE_HIGH,
	.connector_type = DRM_MODE_CONNECTOR_LVDS,
};

static const struct drm_display_mode innolux_zj070na_01p_mode = {
	.clock = 51501,
	.hdisplay = 1024,
	.hsync_start = 1024 + 128,
	.hsync_end = 1024 + 128 + 64,
	.htotal = 1024 + 128 + 64 + 128,
	.vdisplay = 600,
	.vsync_start = 600 + 16,
	.vsync_end = 600 + 16 + 4,
	.vtotal = 600 + 16 + 4 + 16,
};

static const struct panel_desc innolux_zj070na_01p = {
	.modes = &innolux_zj070na_01p_mode,
	.num_modes = 1,
	.bpc = 6,
	.size = {
		.width = 154,
		.height = 90,
	},
};

static const struct display_timing koe_tx14d24vm1bpa_timing = {
	.pixelclock = { 5580000, 5850000, 6200000 },
	.hactive = { 320, 320, 320 },
	.hfront_porch = { 30, 30, 30 },
	.hback_porch = { 30, 30, 30 },
	.hsync_len = { 1, 5, 17 },
	.vactive = { 240, 240, 240 },
	.vfront_porch = { 6, 6, 6 },
	.vback_porch = { 5, 5, 5 },
	.vsync_len = { 1, 2, 11 },
	.flags = DISPLAY_FLAGS_DE_HIGH,
};

static const struct panel_desc koe_tx14d24vm1bpa = {
	.timings = &koe_tx14d24vm1bpa_timing,
	.num_timings = 1,
	.bpc = 6,
	.size = {
		.width = 115,
		.height = 86,
	},
};

static const struct display_timing koe_tx26d202vm0bwa_timing = {
	.pixelclock = { 151820000, 156720000, 159780000 },
	.hactive = { 1920, 1920, 1920 },
	.hfront_porch = { 105, 130, 142 },
	.hback_porch = { 45, 70, 82 },
	.hsync_len = { 30, 30, 30 },
	.vactive = { 1200, 1200, 1200},
	.vfront_porch = { 3, 5, 10 },
	.vback_porch = { 2, 5, 10 },
	.vsync_len = { 5, 5, 5 },
	.flags = DISPLAY_FLAGS_DE_HIGH,
};

static const struct panel_desc koe_tx26d202vm0bwa = {
	.timings = &koe_tx26d202vm0bwa_timing,
	.num_timings = 1,
	.bpc = 8,
	.size = {
		.width = 217,
		.height = 136,
	},
	.delay = {
		.prepare = 1000,
		.enable = 1000,
		.unprepare = 1000,
		.disable = 1000,
	},
	.bus_format = MEDIA_BUS_FMT_RGB888_1X7X4_SPWG,
	.bus_flags = DRM_BUS_FLAG_DE_HIGH,
	.connector_type = DRM_MODE_CONNECTOR_LVDS,
};

static const struct display_timing koe_tx31d200vm0baa_timing = {
	.pixelclock = { 39600000, 43200000, 48000000 },
	.hactive = { 1280, 1280, 1280 },
	.hfront_porch = { 16, 36, 56 },
	.hback_porch = { 16, 36, 56 },
	.hsync_len = { 8, 8, 8 },
	.vactive = { 480, 480, 480 },
	.vfront_porch = { 6, 21, 33 },
	.vback_porch = { 6, 21, 33 },
	.vsync_len = { 8, 8, 8 },
	.flags = DISPLAY_FLAGS_DE_HIGH,
};

static const struct panel_desc koe_tx31d200vm0baa = {
	.timings = &koe_tx31d200vm0baa_timing,
	.num_timings = 1,
	.bpc = 6,
	.size = {
		.width = 292,
		.height = 109,
	},
	.bus_format = MEDIA_BUS_FMT_RGB666_1X7X3_SPWG,
	.connector_type = DRM_MODE_CONNECTOR_LVDS,
};

static const struct display_timing kyo_tcg121xglp_timing = {
	.pixelclock = { 52000000, 65000000, 71000000 },
	.hactive = { 1024, 1024, 1024 },
	.hfront_porch = { 2, 2, 2 },
	.hback_porch = { 2, 2, 2 },
	.hsync_len = { 86, 124, 244 },
	.vactive = { 768, 768, 768 },
	.vfront_porch = { 2, 2, 2 },
	.vback_porch = { 2, 2, 2 },
	.vsync_len = { 6, 34, 73 },
	.flags = DISPLAY_FLAGS_DE_HIGH,
};

static const struct panel_desc kyo_tcg121xglp = {
	.timings = &kyo_tcg121xglp_timing,
	.num_timings = 1,
	.bpc = 8,
	.size = {
		.width = 246,
		.height = 184,
	},
	.bus_format = MEDIA_BUS_FMT_RGB888_1X7X4_SPWG,
	.connector_type = DRM_MODE_CONNECTOR_LVDS,
};

static const struct drm_display_mode lemaker_bl035_rgb_002_mode = {
	.clock = 7000,
	.hdisplay = 320,
	.hsync_start = 320 + 20,
	.hsync_end = 320 + 20 + 30,
	.htotal = 320 + 20 + 30 + 38,
	.vdisplay = 240,
	.vsync_start = 240 + 4,
	.vsync_end = 240 + 4 + 3,
	.vtotal = 240 + 4 + 3 + 15,
};

static const struct panel_desc lemaker_bl035_rgb_002 = {
	.modes = &lemaker_bl035_rgb_002_mode,
	.num_modes = 1,
	.size = {
		.width = 70,
		.height = 52,
	},
	.bus_format = MEDIA_BUS_FMT_RGB888_1X24,
	.bus_flags = DRM_BUS_FLAG_DE_LOW,
};

static const struct display_timing lg_lb070wv8_timing = {
	.pixelclock = { 31950000, 33260000, 34600000 },
	.hactive = { 800, 800, 800 },
	.hfront_porch = { 88, 88, 88 },
	.hback_porch = { 88, 88, 88 },
	.hsync_len = { 80, 80, 80 },
	.vactive = { 480, 480, 480 },
	.vfront_porch = { 10, 10, 10 },
	.vback_porch = { 10, 10, 10 },
	.vsync_len = { 25, 25, 25 },
};

static const struct panel_desc lg_lb070wv8 = {
	.timings = &lg_lb070wv8_timing,
	.num_timings = 1,
	.bpc = 8,
	.size = {
		.width = 151,
		.height = 91,
	},
	.bus_format = MEDIA_BUS_FMT_RGB888_1X7X4_SPWG,
	.connector_type = DRM_MODE_CONNECTOR_LVDS,
};

static const struct drm_display_mode lincolntech_lcd185_101ct_mode = {
	.clock = 155127,
	.hdisplay = 1920,
	.hsync_start = 1920 + 128,
	.hsync_end = 1920 + 128 + 20,
	.htotal = 1920 + 128 + 20 + 12,
	.vdisplay = 1200,
	.vsync_start = 1200 + 19,
	.vsync_end = 1200 + 19 + 4,
	.vtotal = 1200 + 19 + 4 + 20,
};

static const struct panel_desc lincolntech_lcd185_101ct = {
	.modes = &lincolntech_lcd185_101ct_mode,
	.bpc = 8,
	.num_modes = 1,
	.size = {
		.width = 217,
		.height = 136,
	},
	.delay = {
		.prepare = 50,
		.disable = 50,
	},
	.bus_flags = DRM_BUS_FLAG_DE_HIGH,
	.bus_format = MEDIA_BUS_FMT_RGB888_1X7X4_SPWG,
	.connector_type = DRM_MODE_CONNECTOR_LVDS,
};

static const struct display_timing logictechno_lt161010_2nh_timing = {
	.pixelclock = { 26400000, 33300000, 46800000 },
	.hactive = { 800, 800, 800 },
	.hfront_porch = { 16, 210, 354 },
	.hback_porch = { 46, 46, 46 },
	.hsync_len = { 1, 20, 40 },
	.vactive = { 480, 480, 480 },
	.vfront_porch = { 7, 22, 147 },
	.vback_porch = { 23, 23, 23 },
	.vsync_len = { 1, 10, 20 },
	.flags = DISPLAY_FLAGS_HSYNC_LOW | DISPLAY_FLAGS_VSYNC_LOW |
		 DISPLAY_FLAGS_DE_HIGH | DISPLAY_FLAGS_PIXDATA_POSEDGE |
		 DISPLAY_FLAGS_SYNC_POSEDGE,
};

static const struct panel_desc logictechno_lt161010_2nh = {
	.timings = &logictechno_lt161010_2nh_timing,
	.num_timings = 1,
	.bpc = 6,
	.size = {
		.width = 154,
		.height = 86,
	},
	.bus_format = MEDIA_BUS_FMT_RGB666_1X18,
	.bus_flags = DRM_BUS_FLAG_DE_HIGH |
		     DRM_BUS_FLAG_PIXDATA_SAMPLE_NEGEDGE |
		     DRM_BUS_FLAG_SYNC_SAMPLE_NEGEDGE,
	.connector_type = DRM_MODE_CONNECTOR_DPI,
};

static const struct display_timing logictechno_lt170410_2whc_timing = {
	.pixelclock = { 68900000, 71100000, 73400000 },
	.hactive = { 1280, 1280, 1280 },
	.hfront_porch = { 23, 60, 71 },
	.hback_porch = { 23, 60, 71 },
	.hsync_len = { 15, 40, 47 },
	.vactive = { 800, 800, 800 },
	.vfront_porch = { 5, 7, 10 },
	.vback_porch = { 5, 7, 10 },
	.vsync_len = { 6, 9, 12 },
	.flags = DISPLAY_FLAGS_HSYNC_LOW | DISPLAY_FLAGS_VSYNC_LOW |
		 DISPLAY_FLAGS_DE_HIGH | DISPLAY_FLAGS_PIXDATA_POSEDGE |
		 DISPLAY_FLAGS_SYNC_POSEDGE,
};

static const struct panel_desc logictechno_lt170410_2whc = {
	.timings = &logictechno_lt170410_2whc_timing,
	.num_timings = 1,
	.bpc = 8,
	.size = {
		.width = 217,
		.height = 136,
	},
	.bus_format = MEDIA_BUS_FMT_RGB888_1X7X4_SPWG,
	.bus_flags = DRM_BUS_FLAG_DE_HIGH,
	.connector_type = DRM_MODE_CONNECTOR_LVDS,
};

static const struct drm_display_mode logictechno_lttd800480070_l2rt_mode = {
	.clock = 33000,
	.hdisplay = 800,
	.hsync_start = 800 + 112,
	.hsync_end = 800 + 112 + 3,
	.htotal = 800 + 112 + 3 + 85,
	.vdisplay = 480,
	.vsync_start = 480 + 38,
	.vsync_end = 480 + 38 + 3,
	.vtotal = 480 + 38 + 3 + 29,
	.flags = DRM_MODE_FLAG_NVSYNC | DRM_MODE_FLAG_NHSYNC,
};

static const struct panel_desc logictechno_lttd800480070_l2rt = {
	.modes = &logictechno_lttd800480070_l2rt_mode,
	.num_modes = 1,
	.bpc = 8,
	.size = {
		.width = 154,
		.height = 86,
	},
	.delay = {
		.prepare = 45,
		.enable = 100,
		.disable = 100,
		.unprepare = 45
	},
	.bus_format = MEDIA_BUS_FMT_RGB888_1X24,
	.bus_flags = DRM_BUS_FLAG_PIXDATA_SAMPLE_NEGEDGE,
	.connector_type = DRM_MODE_CONNECTOR_DPI,
};

static const struct drm_display_mode logictechno_lttd800480070_l6wh_rt_mode = {
	.clock = 33000,
	.hdisplay = 800,
	.hsync_start = 800 + 154,
	.hsync_end = 800 + 154 + 3,
	.htotal = 800 + 154 + 3 + 43,
	.vdisplay = 480,
	.vsync_start = 480 + 47,
	.vsync_end = 480 + 47 + 3,
	.vtotal = 480 + 47 + 3 + 20,
	.flags = DRM_MODE_FLAG_NVSYNC | DRM_MODE_FLAG_NHSYNC,
};

static const struct panel_desc logictechno_lttd800480070_l6wh_rt = {
	.modes = &logictechno_lttd800480070_l6wh_rt_mode,
	.num_modes = 1,
	.bpc = 8,
	.size = {
		.width = 154,
		.height = 86,
	},
	.delay = {
		.prepare = 45,
		.enable = 100,
		.disable = 100,
		.unprepare = 45
	},
	.bus_format = MEDIA_BUS_FMT_RGB888_1X24,
	.bus_flags = DRM_BUS_FLAG_PIXDATA_SAMPLE_NEGEDGE,
	.connector_type = DRM_MODE_CONNECTOR_DPI,
};

static const struct drm_display_mode logicpd_type_28_mode = {
	.clock = 9107,
	.hdisplay = 480,
	.hsync_start = 480 + 3,
	.hsync_end = 480 + 3 + 42,
	.htotal = 480 + 3 + 42 + 2,

	.vdisplay = 272,
	.vsync_start = 272 + 2,
	.vsync_end = 272 + 2 + 11,
	.vtotal = 272 + 2 + 11 + 3,
	.flags = DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC,
};

static const struct panel_desc logicpd_type_28 = {
	.modes = &logicpd_type_28_mode,
	.num_modes = 1,
	.bpc = 8,
	.size = {
		.width = 105,
		.height = 67,
	},
	.delay = {
		.prepare = 200,
		.enable = 200,
		.unprepare = 200,
		.disable = 200,
	},
	.bus_format = MEDIA_BUS_FMT_RGB888_1X24,
	.bus_flags = DRM_BUS_FLAG_DE_HIGH | DRM_BUS_FLAG_PIXDATA_DRIVE_POSEDGE |
		     DRM_BUS_FLAG_SYNC_DRIVE_NEGEDGE,
	.connector_type = DRM_MODE_CONNECTOR_DPI,
};

static const struct drm_display_mode microtips_mf_101hiebcaf0_c_mode = {
	.clock = 150275,
	.hdisplay = 1920,
	.hsync_start = 1920 + 32,
	.hsync_end = 1920 + 32 + 52,
	.htotal = 1920 + 32 + 52 + 24,
	.vdisplay = 1200,
	.vsync_start = 1200 + 24,
	.vsync_end = 1200 + 24 + 8,
	.vtotal = 1200 + 24 + 8 + 3,
};

static const struct panel_desc microtips_mf_101hiebcaf0_c = {
	.modes = &microtips_mf_101hiebcaf0_c_mode,
	.bpc = 8,
	.num_modes = 1,
	.size = {
		.width = 217,
		.height = 136,
	},
	.delay = {
		.prepare = 50,
		.disable = 50,
	},
	.bus_flags = DRM_BUS_FLAG_DE_HIGH,
	.bus_format = MEDIA_BUS_FMT_RGB888_1X7X4_SPWG,
	.connector_type = DRM_MODE_CONNECTOR_LVDS,
};

static const struct drm_display_mode microtips_mf_103hieb0ga0_mode = {
	.clock = 93301,
	.hdisplay = 1920,
	.hsync_start = 1920 + 72,
	.hsync_end = 1920 + 72 + 72,
	.htotal = 1920 + 72 + 72 + 72,
	.vdisplay = 720,
	.vsync_start = 720 + 3,
	.vsync_end = 720 + 3 + 3,
	.vtotal = 720 + 3 + 3 + 2,
};

static const struct panel_desc microtips_mf_103hieb0ga0 = {
	.modes = &microtips_mf_103hieb0ga0_mode,
	.bpc = 8,
	.num_modes = 1,
	.size = {
		.width = 244,
		.height = 92,
	},
	.delay = {
		.prepare = 50,
		.disable = 50,
	},
	.bus_flags = DRM_BUS_FLAG_DE_HIGH,
	.bus_format = MEDIA_BUS_FMT_RGB888_1X7X4_SPWG,
	.connector_type = DRM_MODE_CONNECTOR_LVDS,
};

static const struct drm_display_mode mitsubishi_aa070mc01_mode = {
	.clock = 30400,
	.hdisplay = 800,
	.hsync_start = 800 + 0,
	.hsync_end = 800 + 1,
	.htotal = 800 + 0 + 1 + 160,
	.vdisplay = 480,
	.vsync_start = 480 + 0,
	.vsync_end = 480 + 48 + 1,
	.vtotal = 480 + 48 + 1 + 0,
	.flags = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC,
};

static const struct panel_desc mitsubishi_aa070mc01 = {
	.modes = &mitsubishi_aa070mc01_mode,
	.num_modes = 1,
	.bpc = 8,
	.size = {
		.width = 152,
		.height = 91,
	},

	.delay = {
		.enable = 200,
		.unprepare = 200,
		.disable = 400,
	},
	.bus_format = MEDIA_BUS_FMT_RGB888_1X7X4_SPWG,
	.connector_type = DRM_MODE_CONNECTOR_LVDS,
	.bus_flags = DRM_BUS_FLAG_DE_HIGH,
};

static const struct drm_display_mode mitsubishi_aa084xe01_mode = {
	.clock = 56234,
	.hdisplay = 1024,
	.hsync_start = 1024 + 24,
	.hsync_end = 1024 + 24 + 63,
	.htotal = 1024 + 24 + 63 + 1,
	.vdisplay = 768,
	.vsync_start = 768 + 3,
	.vsync_end = 768 + 3 + 6,
	.vtotal = 768 + 3 + 6 + 1,
	.flags = DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC,
};

static const struct panel_desc mitsubishi_aa084xe01 = {
	.modes = &mitsubishi_aa084xe01_mode,
	.num_modes = 1,
	.bpc = 8,
	.size = {
		.width = 1024,
		.height = 768,
	},
	.bus_format = MEDIA_BUS_FMT_RGB565_1X16,
	.connector_type = DRM_MODE_CONNECTOR_DPI,
	.bus_flags = DRM_BUS_FLAG_DE_HIGH | DRM_BUS_FLAG_PIXDATA_SAMPLE_NEGEDGE,
};

static const struct display_timing multi_inno_mi0700s4t_6_timing = {
	.pixelclock = { 29000000, 33000000, 38000000 },
	.hactive = { 800, 800, 800 },
	.hfront_porch = { 180, 210, 240 },
	.hback_porch = { 16, 16, 16 },
	.hsync_len = { 30, 30, 30 },
	.vactive = { 480, 480, 480 },
	.vfront_porch = { 12, 22, 32 },
	.vback_porch = { 10, 10, 10 },
	.vsync_len = { 13, 13, 13 },
	.flags = DISPLAY_FLAGS_HSYNC_LOW | DISPLAY_FLAGS_VSYNC_LOW |
		 DISPLAY_FLAGS_DE_HIGH | DISPLAY_FLAGS_PIXDATA_POSEDGE |
		 DISPLAY_FLAGS_SYNC_POSEDGE,
};

static const struct panel_desc multi_inno_mi0700s4t_6 = {
	.timings = &multi_inno_mi0700s4t_6_timing,
	.num_timings = 1,
	.bpc = 8,
	.size = {
		.width = 154,
		.height = 86,
	},
	.bus_format = MEDIA_BUS_FMT_RGB888_1X24,
	.bus_flags = DRM_BUS_FLAG_DE_HIGH |
		     DRM_BUS_FLAG_PIXDATA_SAMPLE_NEGEDGE |
		     DRM_BUS_FLAG_SYNC_SAMPLE_NEGEDGE,
	.connector_type = DRM_MODE_CONNECTOR_DPI,
};

static const struct display_timing multi_inno_mi0800ft_9_timing = {
	.pixelclock = { 32000000, 40000000, 50000000 },
	.hactive = { 800, 800, 800 },
	.hfront_porch = { 16, 210, 354 },
	.hback_porch = { 6, 26, 45 },
	.hsync_len = { 1, 20, 40 },
	.vactive = { 600, 600, 600 },
	.vfront_porch = { 1, 12, 77 },
	.vback_porch = { 3, 13, 22 },
	.vsync_len = { 1, 10, 20 },
	.flags = DISPLAY_FLAGS_HSYNC_LOW | DISPLAY_FLAGS_VSYNC_LOW |
		 DISPLAY_FLAGS_DE_HIGH | DISPLAY_FLAGS_PIXDATA_POSEDGE |
		 DISPLAY_FLAGS_SYNC_POSEDGE,
};

static const struct panel_desc multi_inno_mi0800ft_9 = {
	.timings = &multi_inno_mi0800ft_9_timing,
	.num_timings = 1,
	.bpc = 8,
	.size = {
		.width = 162,
		.height = 122,
	},
	.bus_format = MEDIA_BUS_FMT_RGB888_1X24,
	.bus_flags = DRM_BUS_FLAG_DE_HIGH |
		     DRM_BUS_FLAG_PIXDATA_SAMPLE_NEGEDGE |
		     DRM_BUS_FLAG_SYNC_SAMPLE_NEGEDGE,
	.connector_type = DRM_MODE_CONNECTOR_DPI,
};

static const struct display_timing multi_inno_mi1010ait_1cp_timing = {
	.pixelclock = { 68900000, 70000000, 73400000 },
	.hactive = { 1280, 1280, 1280 },
	.hfront_porch = { 30, 60, 71 },
	.hback_porch = { 30, 60, 71 },
	.hsync_len = { 10, 10, 48 },
	.vactive = { 800, 800, 800 },
	.vfront_porch = { 5, 10, 10 },
	.vback_porch = { 5, 10, 10 },
	.vsync_len = { 5, 6, 13 },
	.flags = DISPLAY_FLAGS_HSYNC_LOW | DISPLAY_FLAGS_VSYNC_LOW |
		 DISPLAY_FLAGS_DE_HIGH,
};

static const struct panel_desc multi_inno_mi1010ait_1cp = {
	.timings = &multi_inno_mi1010ait_1cp_timing,
	.num_timings = 1,
	.bpc = 8,
	.size = {
		.width = 217,
		.height = 136,
	},
	.delay = {
		.enable = 50,
		.disable = 50,
	},
	.bus_format = MEDIA_BUS_FMT_RGB888_1X7X4_SPWG,
	.bus_flags = DRM_BUS_FLAG_DE_HIGH,
	.connector_type = DRM_MODE_CONNECTOR_LVDS,
};

static const struct display_timing nec_nl12880bc20_05_timing = {
	.pixelclock = { 67000000, 71000000, 75000000 },
	.hactive = { 1280, 1280, 1280 },
	.hfront_porch = { 2, 30, 30 },
	.hback_porch = { 6, 100, 100 },
	.hsync_len = { 2, 30, 30 },
	.vactive = { 800, 800, 800 },
	.vfront_porch = { 5, 5, 5 },
	.vback_porch = { 11, 11, 11 },
	.vsync_len = { 7, 7, 7 },
};

static const struct panel_desc nec_nl12880bc20_05 = {
	.timings = &nec_nl12880bc20_05_timing,
	.num_timings = 1,
	.bpc = 8,
	.size = {
		.width = 261,
		.height = 163,
	},
	.delay = {
		.enable = 50,
		.disable = 50,
	},
	.bus_format = MEDIA_BUS_FMT_RGB888_1X7X4_SPWG,
	.connector_type = DRM_MODE_CONNECTOR_LVDS,
};

static const struct drm_display_mode nec_nl4827hc19_05b_mode = {
	.clock = 10870,
	.hdisplay = 480,
	.hsync_start = 480 + 2,
	.hsync_end = 480 + 2 + 41,
	.htotal = 480 + 2 + 41 + 2,
	.vdisplay = 272,
	.vsync_start = 272 + 2,
	.vsync_end = 272 + 2 + 4,
	.vtotal = 272 + 2 + 4 + 2,
	.flags = DRM_MODE_FLAG_NVSYNC | DRM_MODE_FLAG_NHSYNC,
};

static const struct panel_desc nec_nl4827hc19_05b = {
	.modes = &nec_nl4827hc19_05b_mode,
	.num_modes = 1,
	.bpc = 8,
	.size = {
		.width = 95,
		.height = 54,
	},
	.bus_format = MEDIA_BUS_FMT_RGB888_1X24,
	.bus_flags = DRM_BUS_FLAG_PIXDATA_DRIVE_POSEDGE,
};

static const struct drm_display_mode netron_dy_e231732_mode = {
	.clock = 66000,
	.hdisplay = 1024,
	.hsync_start = 1024 + 160,
	.hsync_end = 1024 + 160 + 70,
	.htotal = 1024 + 160 + 70 + 90,
	.vdisplay = 600,
	.vsync_start = 600 + 127,
	.vsync_end = 600 + 127 + 20,
	.vtotal = 600 + 127 + 20 + 3,
};

static const struct panel_desc netron_dy_e231732 = {
	.modes = &netron_dy_e231732_mode,
	.num_modes = 1,
	.size = {
		.width = 154,
		.height = 87,
	},
	.bus_format = MEDIA_BUS_FMT_RGB666_1X18,
};

static const struct drm_display_mode newhaven_nhd_43_480272ef_atxl_mode = {
	.clock = 9000,
	.hdisplay = 480,
	.hsync_start = 480 + 2,
	.hsync_end = 480 + 2 + 41,
	.htotal = 480 + 2 + 41 + 2,
	.vdisplay = 272,
	.vsync_start = 272 + 2,
	.vsync_end = 272 + 2 + 10,
	.vtotal = 272 + 2 + 10 + 2,
	.flags = DRM_MODE_FLAG_NVSYNC | DRM_MODE_FLAG_NHSYNC,
};

static const struct panel_desc newhaven_nhd_43_480272ef_atxl = {
	.modes = &newhaven_nhd_43_480272ef_atxl_mode,
	.num_modes = 1,
	.bpc = 8,
	.size = {
		.width = 95,
		.height = 54,
	},
	.bus_format = MEDIA_BUS_FMT_RGB888_1X24,
	.bus_flags = DRM_BUS_FLAG_DE_HIGH | DRM_BUS_FLAG_PIXDATA_DRIVE_POSEDGE |
		     DRM_BUS_FLAG_SYNC_DRIVE_POSEDGE,
	.connector_type = DRM_MODE_CONNECTOR_DPI,
};

static const struct display_timing nlt_nl192108ac18_02d_timing = {
	.pixelclock = { 130000000, 148350000, 163000000 },
	.hactive = { 1920, 1920, 1920 },
	.hfront_porch = { 80, 100, 100 },
	.hback_porch = { 100, 120, 120 },
	.hsync_len = { 50, 60, 60 },
	.vactive = { 1080, 1080, 1080 },
	.vfront_porch = { 12, 30, 30 },
	.vback_porch = { 4, 10, 10 },
	.vsync_len = { 4, 5, 5 },
};

static const struct panel_desc nlt_nl192108ac18_02d = {
	.timings = &nlt_nl192108ac18_02d_timing,
	.num_timings = 1,
	.bpc = 8,
	.size = {
		.width = 344,
		.height = 194,
	},
	.delay = {
		.unprepare = 500,
	},
	.bus_format = MEDIA_BUS_FMT_RGB888_1X7X4_SPWG,
	.connector_type = DRM_MODE_CONNECTOR_LVDS,
};

static const struct drm_display_mode nvd_9128_mode = {
	.clock = 29500,
	.hdisplay = 800,
	.hsync_start = 800 + 130,
	.hsync_end = 800 + 130 + 98,
	.htotal = 800 + 0 + 130 + 98,
	.vdisplay = 480,
	.vsync_start = 480 + 10,
	.vsync_end = 480 + 10 + 50,
	.vtotal = 480 + 0 + 10 + 50,
};

static const struct panel_desc nvd_9128 = {
	.modes = &nvd_9128_mode,
	.num_modes = 1,
	.bpc = 8,
	.size = {
		.width = 156,
		.height = 88,
	},
	.bus_format = MEDIA_BUS_FMT_RGB888_1X7X4_SPWG,
	.connector_type = DRM_MODE_CONNECTOR_LVDS,
};

static const struct display_timing okaya_rs800480t_7x0gp_timing = {
	.pixelclock = { 30000000, 30000000, 40000000 },
	.hactive = { 800, 800, 800 },
	.hfront_porch = { 40, 40, 40 },
	.hback_porch = { 40, 40, 40 },
	.hsync_len = { 1, 48, 48 },
	.vactive = { 480, 480, 480 },
	.vfront_porch = { 13, 13, 13 },
	.vback_porch = { 29, 29, 29 },
	.vsync_len = { 3, 3, 3 },
	.flags = DISPLAY_FLAGS_DE_HIGH,
};

static const struct panel_desc okaya_rs800480t_7x0gp = {
	.timings = &okaya_rs800480t_7x0gp_timing,
	.num_timings = 1,
	.bpc = 6,
	.size = {
		.width = 154,
		.height = 87,
	},
	.delay = {
		.prepare = 41,
		.enable = 50,
		.unprepare = 41,
		.disable = 50,
	},
	.bus_format = MEDIA_BUS_FMT_RGB666_1X18,
};

static const struct drm_display_mode olimex_lcd_olinuxino_43ts_mode = {
	.clock = 9000,
	.hdisplay = 480,
	.hsync_start = 480 + 5,
	.hsync_end = 480 + 5 + 30,
	.htotal = 480 + 5 + 30 + 10,
	.vdisplay = 272,
	.vsync_start = 272 + 8,
	.vsync_end = 272 + 8 + 5,
	.vtotal = 272 + 8 + 5 + 3,
};

static const struct panel_desc olimex_lcd_olinuxino_43ts = {
	.modes = &olimex_lcd_olinuxino_43ts_mode,
	.num_modes = 1,
	.size = {
		.width = 95,
		.height = 54,
	},
	.bus_format = MEDIA_BUS_FMT_RGB888_1X24,
};

static const struct display_timing ontat_kd50g21_40nt_a1_timing = {
	.pixelclock = { 30000000, 30000000, 50000000 },
	.hactive = { 800, 800, 800 },
	.hfront_porch = { 1, 40, 255 },
	.hback_porch = { 1, 40, 87 },
	.hsync_len = { 1, 48, 87 },
	.vactive = { 480, 480, 480 },
	.vfront_porch = { 1, 13, 255 },
	.vback_porch = { 1, 29, 29 },
	.vsync_len = { 3, 3, 31 },
	.flags = DISPLAY_FLAGS_HSYNC_LOW | DISPLAY_FLAGS_VSYNC_LOW |
		 DISPLAY_FLAGS_DE_HIGH | DISPLAY_FLAGS_PIXDATA_POSEDGE,
};

static const struct panel_desc ontat_kd50g21_40nt_a1 = {
	.timings = &ontat_kd50g21_40nt_a1_timing,
	.num_timings = 1,
	.bpc = 8,
	.size = {
		.width = 108,
		.height = 65,
	},
	.delay = {
		.prepare = 147,		/* 5 VSDs */
		.enable = 147,		/* 5 VSDs */
		.disable = 88,		/* 3 VSDs */
		.unprepare = 117,	/* 4 VSDs */
	},
	.bus_format = MEDIA_BUS_FMT_RGB888_1X24,
	.bus_flags = DRM_BUS_FLAG_DE_HIGH | DRM_BUS_FLAG_PIXDATA_SAMPLE_NEGEDGE,
	.connector_type = DRM_MODE_CONNECTOR_DPI,
};

/*
 * 800x480 CVT. The panel appears to be quite accepting, at least as far as
 * pixel clocks, but this is the timing that was being used in the Adafruit
 * installation instructions.
 */
static const struct drm_display_mode ontat_yx700wv03_mode = {
	.clock = 29500,
	.hdisplay = 800,
	.hsync_start = 824,
	.hsync_end = 896,
	.htotal = 992,
	.vdisplay = 480,
	.vsync_start = 483,
	.vsync_end = 493,
	.vtotal = 500,
	.flags = DRM_MODE_FLAG_NVSYNC | DRM_MODE_FLAG_NHSYNC,
};

/*
 * Specification at:
 * https://www.adafruit.com/images/product-files/2406/c3163.pdf
 */
static const struct panel_desc ontat_yx700wv03 = {
	.modes = &ontat_yx700wv03_mode,
	.num_modes = 1,
	.bpc = 8,
	.size = {
		.width = 154,
		.height = 83,
	},
	.bus_format = MEDIA_BUS_FMT_RGB666_1X18,
};

static const struct drm_display_mode ortustech_com37h3m_mode  = {
	.clock = 22230,
	.hdisplay = 480,
	.hsync_start = 480 + 40,
	.hsync_end = 480 + 40 + 10,
	.htotal = 480 + 40 + 10 + 40,
	.vdisplay = 640,
	.vsync_start = 640 + 4,
	.vsync_end = 640 + 4 + 2,
	.vtotal = 640 + 4 + 2 + 4,
	.flags = DRM_MODE_FLAG_NVSYNC | DRM_MODE_FLAG_NHSYNC,
};

static const struct panel_desc ortustech_com37h3m = {
	.modes = &ortustech_com37h3m_mode,
	.num_modes = 1,
	.bpc = 8,
	.size = {
		.width = 56,	/* 56.16mm */
		.height = 75,	/* 74.88mm */
	},
	.bus_format = MEDIA_BUS_FMT_RGB888_1X24,
	.bus_flags = DRM_BUS_FLAG_DE_HIGH | DRM_BUS_FLAG_PIXDATA_SAMPLE_NEGEDGE |
		     DRM_BUS_FLAG_SYNC_DRIVE_POSEDGE,
};

static const struct drm_display_mode ortustech_com43h4m85ulc_mode  = {
	.clock = 25000,
	.hdisplay = 480,
	.hsync_start = 480 + 10,
	.hsync_end = 480 + 10 + 10,
	.htotal = 480 + 10 + 10 + 15,
	.vdisplay = 800,
	.vsync_start = 800 + 3,
	.vsync_end = 800 + 3 + 3,
	.vtotal = 800 + 3 + 3 + 3,
};

static const struct panel_desc ortustech_com43h4m85ulc = {
	.modes = &ortustech_com43h4m85ulc_mode,
	.num_modes = 1,
	.bpc = 6,
	.size = {
		.width = 56,
		.height = 93,
	},
	.bus_format = MEDIA_BUS_FMT_RGB666_1X18,
	.bus_flags = DRM_BUS_FLAG_DE_HIGH | DRM_BUS_FLAG_PIXDATA_DRIVE_POSEDGE,
	.connector_type = DRM_MODE_CONNECTOR_DPI,
};

static const struct drm_display_mode osddisplays_osd070t1718_19ts_mode  = {
	.clock = 33000,
	.hdisplay = 800,
	.hsync_start = 800 + 210,
	.hsync_end = 800 + 210 + 30,
	.htotal = 800 + 210 + 30 + 16,
	.vdisplay = 480,
	.vsync_start = 480 + 22,
	.vsync_end = 480 + 22 + 13,
	.vtotal = 480 + 22 + 13 + 10,
	.flags = DRM_MODE_FLAG_NVSYNC | DRM_MODE_FLAG_NHSYNC,
};

static const struct panel_desc osddisplays_osd070t1718_19ts = {
	.modes = &osddisplays_osd070t1718_19ts_mode,
	.num_modes = 1,
	.bpc = 8,
	.size = {
		.width = 152,
		.height = 91,
	},
	.bus_format = MEDIA_BUS_FMT_RGB888_1X24,
	.bus_flags = DRM_BUS_FLAG_DE_HIGH | DRM_BUS_FLAG_PIXDATA_DRIVE_POSEDGE |
		DRM_BUS_FLAG_SYNC_DRIVE_POSEDGE,
	.connector_type = DRM_MODE_CONNECTOR_DPI,
};

static const struct drm_display_mode pda_91_00156_a0_mode = {
	.clock = 33300,
	.hdisplay = 800,
	.hsync_start = 800 + 1,
	.hsync_end = 800 + 1 + 64,
	.htotal = 800 + 1 + 64 + 64,
	.vdisplay = 480,
	.vsync_start = 480 + 1,
	.vsync_end = 480 + 1 + 23,
	.vtotal = 480 + 1 + 23 + 22,
};

static const struct panel_desc pda_91_00156_a0  = {
	.modes = &pda_91_00156_a0_mode,
	.num_modes = 1,
	.size = {
		.width = 152,
		.height = 91,
	},
	.bus_format = MEDIA_BUS_FMT_RGB888_1X24,
};

static const struct drm_display_mode powertip_ph128800t006_zhc01_mode = {
	.clock = 66500,
	.hdisplay = 1280,
	.hsync_start = 1280 + 12,
	.hsync_end = 1280 + 12 + 20,
	.htotal = 1280 + 12 + 20 + 56,
	.vdisplay = 800,
	.vsync_start = 800 + 1,
	.vsync_end = 800 + 1 + 3,
	.vtotal = 800 + 1 + 3 + 20,
	.flags = DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC,
};

static const struct panel_desc powertip_ph128800t006_zhc01 = {
	.modes = &powertip_ph128800t006_zhc01_mode,
	.num_modes = 1,
	.bpc = 8,
	.size = {
		.width = 216,
		.height = 135,
	},
	.bus_format = MEDIA_BUS_FMT_RGB888_1X7X4_SPWG,
	.bus_flags = DRM_BUS_FLAG_DE_HIGH,
	.connector_type = DRM_MODE_CONNECTOR_LVDS,
};

static const struct drm_display_mode powertip_ph800480t013_idf02_mode = {
	.clock = 24750,
	.hdisplay = 800,
	.hsync_start = 800 + 54,
	.hsync_end = 800 + 54 + 2,
	.htotal = 800 + 54 + 2 + 44,
	.vdisplay = 480,
	.vsync_start = 480 + 49,
	.vsync_end = 480 + 49 + 2,
	.vtotal = 480 + 49 + 2 + 22,
	.flags = DRM_MODE_FLAG_NVSYNC | DRM_MODE_FLAG_NHSYNC,
};

static const struct panel_desc powertip_ph800480t013_idf02  = {
	.modes = &powertip_ph800480t013_idf02_mode,
	.num_modes = 1,
	.bpc = 8,
	.size = {
		.width = 152,
		.height = 91,
	},
	.bus_flags = DRM_BUS_FLAG_DE_HIGH |
		     DRM_BUS_FLAG_PIXDATA_SAMPLE_NEGEDGE |
		     DRM_BUS_FLAG_SYNC_SAMPLE_NEGEDGE,
	.bus_format = MEDIA_BUS_FMT_RGB888_1X24,
	.connector_type = DRM_MODE_CONNECTOR_DPI,
};

static const struct drm_display_mode primeview_pm070wl4_mode = {
	.clock = 32000,
	.hdisplay = 800,
	.hsync_start = 800 + 42,
	.hsync_end = 800 + 42 + 128,
	.htotal = 800 + 42 + 128 + 86,
	.vdisplay = 480,
	.vsync_start = 480 + 10,
	.vsync_end = 480 + 10 + 2,
	.vtotal = 480 + 10 + 2 + 33,
	.flags = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC,
};

static const struct panel_desc primeview_pm070wl4 = {
	.modes = &primeview_pm070wl4_mode,
	.num_modes = 1,
	.bpc = 6,
	.size = {
		.width = 152,
		.height = 91,
	},
	.bus_flags = DRM_BUS_FLAG_DE_HIGH | DRM_BUS_FLAG_PIXDATA_SAMPLE_POSEDGE,
	.bus_format = MEDIA_BUS_FMT_RGB666_1X18,
	.connector_type = DRM_MODE_CONNECTOR_DPI,
};

static const struct drm_display_mode qd43003c0_40_mode = {
	.clock = 9000,
	.hdisplay = 480,
	.hsync_start = 480 + 8,
	.hsync_end = 480 + 8 + 4,
	.htotal = 480 + 8 + 4 + 39,
	.vdisplay = 272,
	.vsync_start = 272 + 4,
	.vsync_end = 272 + 4 + 10,
	.vtotal = 272 + 4 + 10 + 2,
};

static const struct panel_desc qd43003c0_40 = {
	.modes = &qd43003c0_40_mode,
	.num_modes = 1,
	.bpc = 8,
	.size = {
		.width = 95,
		.height = 53,
	},
	.bus_format = MEDIA_BUS_FMT_RGB888_1X24,
};

static const struct drm_display_mode qishenglong_gopher2b_lcd_modes[] = {
	{ /* 60 Hz */
		.clock = 10800,
		.hdisplay = 480,
		.hsync_start = 480 + 77,
		.hsync_end = 480 + 77 + 41,
		.htotal = 480 + 77 + 41 + 2,
		.vdisplay = 272,
		.vsync_start = 272 + 16,
		.vsync_end = 272 + 16 + 10,
		.vtotal = 272 + 16 + 10 + 2,
		.flags = DRM_MODE_FLAG_NVSYNC | DRM_MODE_FLAG_NHSYNC,
	},
	{ /* 50 Hz */
		.clock = 10800,
		.hdisplay = 480,
		.hsync_start = 480 + 17,
		.hsync_end = 480 + 17 + 41,
		.htotal = 480 + 17 + 41 + 2,
		.vdisplay = 272,
		.vsync_start = 272 + 116,
		.vsync_end = 272 + 116 + 10,
		.vtotal = 272 + 116 + 10 + 2,
		.flags = DRM_MODE_FLAG_NVSYNC | DRM_MODE_FLAG_NHSYNC,
	},
};

static const struct panel_desc qishenglong_gopher2b_lcd = {
	.modes = qishenglong_gopher2b_lcd_modes,
	.num_modes = ARRAY_SIZE(qishenglong_gopher2b_lcd_modes),
	.bpc = 8,
	.size = {
		.width = 95,
		.height = 54,
	},
	.bus_format = MEDIA_BUS_FMT_RGB888_1X24,
	.bus_flags = DRM_BUS_FLAG_DE_HIGH | DRM_BUS_FLAG_PIXDATA_SAMPLE_NEGEDGE,
	.connector_type = DRM_MODE_CONNECTOR_DPI,
};

static const struct display_timing rocktech_rk043fn48h_timing = {
	.pixelclock = { 6000000, 9000000, 12000000 },
	.hactive = { 480, 480, 480 },
	.hback_porch = { 8, 43, 43 },
	.hfront_porch = { 2, 8, 10 },
	.hsync_len = { 1, 1, 1 },
	.vactive = { 272, 272, 272 },
	.vback_porch = { 2, 12, 26 },
	.vfront_porch = { 1, 4, 4 },
	.vsync_len = { 1, 10, 10 },
	.flags = DISPLAY_FLAGS_VSYNC_LOW | DISPLAY_FLAGS_HSYNC_LOW |
		 DISPLAY_FLAGS_DE_HIGH | DISPLAY_FLAGS_PIXDATA_POSEDGE |
		 DISPLAY_FLAGS_SYNC_POSEDGE,
};

static const struct panel_desc rocktech_rk043fn48h = {
	.timings = &rocktech_rk043fn48h_timing,
	.num_timings = 1,
	.bpc = 8,
	.size = {
		.width = 95,
		.height = 54,
	},
	.bus_format = MEDIA_BUS_FMT_RGB888_1X24,
	.connector_type = DRM_MODE_CONNECTOR_DPI,
};

static const struct drm_display_mode raspberrypi_7inch_mode = {
	.clock = 30000,
	.hdisplay = 800,
	.hsync_start = 800 + 131,
	.hsync_end = 800 + 131 + 2,
	.htotal = 800 + 131 + 2 + 45,
	.vdisplay = 480,
	.vsync_start = 480 + 7,
	.vsync_end = 480 + 7 + 2,
	.vtotal = 480 + 7 + 2 + 22,
	.flags = DRM_MODE_FLAG_NVSYNC | DRM_MODE_FLAG_NHSYNC,
};

static const struct panel_desc raspberrypi_7inch = {
	.modes = &raspberrypi_7inch_mode,
	.num_modes = 1,
	.bpc = 8,
	.size = {
		.width = 154,
		.height = 86,
	},
	.bus_format = MEDIA_BUS_FMT_RGB888_1X24,
	.connector_type = DRM_MODE_CONNECTOR_DSI,
};

static const struct display_timing rocktech_rk070er9427_timing = {
	.pixelclock = { 26400000, 33300000, 46800000 },
	.hactive = { 800, 800, 800 },
	.hfront_porch = { 16, 210, 354 },
	.hback_porch = { 46, 46, 46 },
	.hsync_len = { 1, 1, 1 },
	.vactive = { 480, 480, 480 },
	.vfront_porch = { 7, 22, 147 },
	.vback_porch = { 23, 23, 23 },
	.vsync_len = { 1, 1, 1 },
	.flags = DISPLAY_FLAGS_DE_HIGH,
};

static const struct panel_desc rocktech_rk070er9427 = {
	.timings = &rocktech_rk070er9427_timing,
	.num_timings = 1,
	.bpc = 6,
	.size = {
		.width = 154,
		.height = 86,
	},
	.delay = {
		.prepare = 41,
		.enable = 50,
		.unprepare = 41,
		.disable = 50,
	},
	.bus_format = MEDIA_BUS_FMT_RGB666_1X18,
};

static const struct drm_display_mode rocktech_rk101ii01d_ct_mode = {
	.clock = 71100,
	.hdisplay = 1280,
	.hsync_start = 1280 + 48,
	.hsync_end = 1280 + 48 + 32,
	.htotal = 1280 + 48 + 32 + 80,
	.vdisplay = 800,
	.vsync_start = 800 + 2,
	.vsync_end = 800 + 2 + 5,
	.vtotal = 800 + 2 + 5 + 16,
};

static const struct panel_desc rocktech_rk101ii01d_ct = {
	.modes = &rocktech_rk101ii01d_ct_mode,
	.bpc = 8,
	.num_modes = 1,
	.size = {
		.width = 217,
		.height = 136,
	},
	.delay = {
		.prepare = 50,
		.disable = 50,
	},
	.bus_flags = DRM_BUS_FLAG_DE_HIGH,
	.bus_format = MEDIA_BUS_FMT_RGB888_1X7X4_SPWG,
	.connector_type = DRM_MODE_CONNECTOR_LVDS,
};

static const struct display_timing samsung_ltl101al01_timing = {
	.pixelclock = { 66663000, 66663000, 66663000 },
	.hactive = { 1280, 1280, 1280 },
	.hfront_porch = { 18, 18, 18 },
	.hback_porch = { 36, 36, 36 },
	.hsync_len = { 16, 16, 16 },
	.vactive = { 800, 800, 800 },
	.vfront_porch = { 4, 4, 4 },
	.vback_porch = { 16, 16, 16 },
	.vsync_len = { 3, 3, 3 },
	.flags = DISPLAY_FLAGS_HSYNC_LOW | DISPLAY_FLAGS_VSYNC_LOW,
};

static const struct panel_desc samsung_ltl101al01 = {
	.timings = &samsung_ltl101al01_timing,
	.num_timings = 1,
	.bpc = 8,
	.size = {
		.width = 217,
		.height = 135,
	},
	.delay = {
		.prepare = 40,
		.enable = 300,
		.disable = 200,
		.unprepare = 600,
	},
	.bus_format = MEDIA_BUS_FMT_RGB888_1X7X4_SPWG,
	.connector_type = DRM_MODE_CONNECTOR_LVDS,
};

static const struct drm_display_mode samsung_ltn101nt05_mode = {
	.clock = 54030,
	.hdisplay = 1024,
	.hsync_start = 1024 + 24,
	.hsync_end = 1024 + 24 + 136,
	.htotal = 1024 + 24 + 136 + 160,
	.vdisplay = 600,
	.vsync_start = 600 + 3,
	.vsync_end = 600 + 3 + 6,
	.vtotal = 600 + 3 + 6 + 61,
};

static const struct panel_desc samsung_ltn101nt05 = {
	.modes = &samsung_ltn101nt05_mode,
	.num_modes = 1,
	.bpc = 6,
	.size = {
		.width = 223,
		.height = 125,
	},
	.bus_format = MEDIA_BUS_FMT_RGB666_1X7X3_SPWG,
	.bus_flags = DRM_BUS_FLAG_DE_HIGH,
	.connector_type = DRM_MODE_CONNECTOR_LVDS,
};

static const struct display_timing satoz_sat050at40h12r2_timing = {
	.pixelclock = {33300000, 33300000, 50000000},
	.hactive = {800, 800, 800},
	.hfront_porch = {16, 210, 354},
	.hback_porch = {46, 46, 46},
	.hsync_len = {1, 1, 40},
	.vactive = {480, 480, 480},
	.vfront_porch = {7, 22, 147},
	.vback_porch = {23, 23, 23},
	.vsync_len = {1, 1, 20},
};

static const struct panel_desc satoz_sat050at40h12r2 = {
	.timings = &satoz_sat050at40h12r2_timing,
	.num_timings = 1,
	.bpc = 8,
	.size = {
		.width = 108,
		.height = 65,
	},
	.bus_format = MEDIA_BUS_FMT_RGB888_1X7X4_SPWG,
	.connector_type = DRM_MODE_CONNECTOR_LVDS,
};

static const struct drm_display_mode sharp_lq070y3dg3b_mode = {
	.clock = 33260,
	.hdisplay = 800,
	.hsync_start = 800 + 64,
	.hsync_end = 800 + 64 + 128,
	.htotal = 800 + 64 + 128 + 64,
	.vdisplay = 480,
	.vsync_start = 480 + 8,
	.vsync_end = 480 + 8 + 2,
	.vtotal = 480 + 8 + 2 + 35,
	.flags = DISPLAY_FLAGS_PIXDATA_POSEDGE,
};

static const struct panel_desc sharp_lq070y3dg3b = {
	.modes = &sharp_lq070y3dg3b_mode,
	.num_modes = 1,
	.bpc = 8,
	.size = {
		.width = 152,	/* 152.4mm */
		.height = 91,	/* 91.4mm */
	},
	.bus_format = MEDIA_BUS_FMT_RGB888_1X24,
	.bus_flags = DRM_BUS_FLAG_DE_HIGH | DRM_BUS_FLAG_PIXDATA_SAMPLE_NEGEDGE |
		     DRM_BUS_FLAG_SYNC_DRIVE_POSEDGE,
};

static const struct drm_display_mode sharp_lq035q7db03_mode = {
	.clock = 5500,
	.hdisplay = 240,
	.hsync_start = 240 + 16,
	.hsync_end = 240 + 16 + 7,
	.htotal = 240 + 16 + 7 + 5,
	.vdisplay = 320,
	.vsync_start = 320 + 9,
	.vsync_end = 320 + 9 + 1,
	.vtotal = 320 + 9 + 1 + 7,
};

static const struct panel_desc sharp_lq035q7db03 = {
	.modes = &sharp_lq035q7db03_mode,
	.num_modes = 1,
	.bpc = 6,
	.size = {
		.width = 54,
		.height = 72,
	},
	.bus_format = MEDIA_BUS_FMT_RGB666_1X18,
};

static const struct display_timing sharp_lq101k1ly04_timing = {
	.pixelclock = { 60000000, 65000000, 80000000 },
	.hactive = { 1280, 1280, 1280 },
	.hfront_porch = { 20, 20, 20 },
	.hback_porch = { 20, 20, 20 },
	.hsync_len = { 10, 10, 10 },
	.vactive = { 800, 800, 800 },
	.vfront_porch = { 4, 4, 4 },
	.vback_porch = { 4, 4, 4 },
	.vsync_len = { 4, 4, 4 },
	.flags = DISPLAY_FLAGS_PIXDATA_POSEDGE,
};

static const struct panel_desc sharp_lq101k1ly04 = {
	.timings = &sharp_lq101k1ly04_timing,
	.num_timings = 1,
	.bpc = 8,
	.size = {
		.width = 217,
		.height = 136,
	},
	.bus_format = MEDIA_BUS_FMT_RGB888_1X7X4_JEIDA,
	.connector_type = DRM_MODE_CONNECTOR_LVDS,
};

static const struct drm_display_mode sharp_ls020b1dd01d_modes[] = {
	{ /* 50 Hz */
		.clock = 3000,
		.hdisplay = 240,
		.hsync_start = 240 + 58,
		.hsync_end = 240 + 58 + 1,
		.htotal = 240 + 58 + 1 + 1,
		.vdisplay = 160,
		.vsync_start = 160 + 24,
		.vsync_end = 160 + 24 + 10,
		.vtotal = 160 + 24 + 10 + 6,
		.flags = DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NVSYNC,
	},
	{ /* 60 Hz */
		.clock = 3000,
		.hdisplay = 240,
		.hsync_start = 240 + 8,
		.hsync_end = 240 + 8 + 1,
		.htotal = 240 + 8 + 1 + 1,
		.vdisplay = 160,
		.vsync_start = 160 + 24,
		.vsync_end = 160 + 24 + 10,
		.vtotal = 160 + 24 + 10 + 6,
		.flags = DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NVSYNC,
	},
};

static const struct panel_desc sharp_ls020b1dd01d = {
	.modes = sharp_ls020b1dd01d_modes,
	.num_modes = ARRAY_SIZE(sharp_ls020b1dd01d_modes),
	.bpc = 6,
	.size = {
		.width = 42,
		.height = 28,
	},
	.bus_format = MEDIA_BUS_FMT_RGB565_1X16,
	.bus_flags = DRM_BUS_FLAG_DE_HIGH
		   | DRM_BUS_FLAG_PIXDATA_SAMPLE_POSEDGE
		   | DRM_BUS_FLAG_SHARP_SIGNALS,
};

static const struct drm_display_mode shelly_sca07010_bfn_lnn_mode = {
	.clock = 33300,
	.hdisplay = 800,
	.hsync_start = 800 + 1,
	.hsync_end = 800 + 1 + 64,
	.htotal = 800 + 1 + 64 + 64,
	.vdisplay = 480,
	.vsync_start = 480 + 1,
	.vsync_end = 480 + 1 + 23,
	.vtotal = 480 + 1 + 23 + 22,
};

static const struct panel_desc shelly_sca07010_bfn_lnn = {
	.modes = &shelly_sca07010_bfn_lnn_mode,
	.num_modes = 1,
	.size = {
		.width = 152,
		.height = 91,
	},
	.bus_format = MEDIA_BUS_FMT_RGB666_1X18,
};

static const struct drm_display_mode starry_kr070pe2t_mode = {
	.clock = 33000,
	.hdisplay = 800,
	.hsync_start = 800 + 209,
	.hsync_end = 800 + 209 + 1,
	.htotal = 800 + 209 + 1 + 45,
	.vdisplay = 480,
	.vsync_start = 480 + 22,
	.vsync_end = 480 + 22 + 1,
	.vtotal = 480 + 22 + 1 + 22,
};

static const struct panel_desc starry_kr070pe2t = {
	.modes = &starry_kr070pe2t_mode,
	.num_modes = 1,
	.bpc = 8,
	.size = {
		.width = 152,
		.height = 86,
	},
	.bus_format = MEDIA_BUS_FMT_RGB888_1X24,
	.bus_flags = DRM_BUS_FLAG_DE_HIGH | DRM_BUS_FLAG_PIXDATA_DRIVE_NEGEDGE,
	.connector_type = DRM_MODE_CONNECTOR_DPI,
};

static const struct display_timing startek_kd070wvfpa_mode = {
	.pixelclock = { 25200000, 27200000, 30500000 },
	.hactive = { 800, 800, 800 },
	.hfront_porch = { 19, 44, 115 },
	.hback_porch = { 5, 16, 101 },
	.hsync_len = { 1, 2, 100 },
	.vactive = { 480, 480, 480 },
	.vfront_porch = { 5, 43, 67 },
	.vback_porch = { 5, 5, 67 },
	.vsync_len = { 1, 2, 66 },
	.flags = DISPLAY_FLAGS_HSYNC_LOW | DISPLAY_FLAGS_VSYNC_LOW |
		 DISPLAY_FLAGS_DE_HIGH | DISPLAY_FLAGS_PIXDATA_POSEDGE |
		 DISPLAY_FLAGS_SYNC_POSEDGE,
};

static const struct panel_desc startek_kd070wvfpa = {
	.timings = &startek_kd070wvfpa_mode,
	.num_timings = 1,
	.bpc = 8,
	.size = {
		.width = 152,
		.height = 91,
	},
	.delay = {
		.prepare = 20,
		.enable = 200,
		.disable = 200,
	},
	.bus_format = MEDIA_BUS_FMT_RGB888_1X24,
	.connector_type = DRM_MODE_CONNECTOR_DPI,
	.bus_flags = DRM_BUS_FLAG_DE_HIGH |
		     DRM_BUS_FLAG_PIXDATA_SAMPLE_NEGEDGE |
		     DRM_BUS_FLAG_SYNC_SAMPLE_NEGEDGE,
};

static const struct display_timing tsd_tst043015cmhx_timing = {
	.pixelclock = { 5000000, 9000000, 12000000 },
	.hactive = { 480, 480, 480 },
	.hfront_porch = { 4, 5, 65 },
	.hback_porch = { 36, 40, 255 },
	.hsync_len = { 1, 1, 1 },
	.vactive = { 272, 272, 272 },
	.vfront_porch = { 2, 8, 97 },
	.vback_porch = { 3, 8, 31 },
	.vsync_len = { 1, 1, 1 },

	.flags = DISPLAY_FLAGS_HSYNC_LOW | DISPLAY_FLAGS_VSYNC_LOW |
		 DISPLAY_FLAGS_DE_HIGH | DISPLAY_FLAGS_PIXDATA_POSEDGE,
};

static const struct panel_desc tsd_tst043015cmhx = {
	.timings = &tsd_tst043015cmhx_timing,
	.num_timings = 1,
	.bpc = 8,
	.size = {
		.width = 105,
		.height = 67,
	},
	.bus_format = MEDIA_BUS_FMT_RGB888_1X24,
	.bus_flags = DRM_BUS_FLAG_DE_HIGH | DRM_BUS_FLAG_PIXDATA_SAMPLE_NEGEDGE,
};

static const struct drm_display_mode tfc_s9700rtwv43tr_01b_mode = {
	.clock = 30000,
	.hdisplay = 800,
	.hsync_start = 800 + 39,
	.hsync_end = 800 + 39 + 47,
	.htotal = 800 + 39 + 47 + 39,
	.vdisplay = 480,
	.vsync_start = 480 + 13,
	.vsync_end = 480 + 13 + 2,
	.vtotal = 480 + 13 + 2 + 29,
};

static const struct panel_desc tfc_s9700rtwv43tr_01b = {
	.modes = &tfc_s9700rtwv43tr_01b_mode,
	.num_modes = 1,
	.bpc = 8,
	.size = {
		.width = 155,
		.height = 90,
	},
	.bus_format = MEDIA_BUS_FMT_RGB888_1X24,
	.bus_flags = DRM_BUS_FLAG_DE_HIGH | DRM_BUS_FLAG_PIXDATA_SAMPLE_NEGEDGE,
};

static const struct display_timing tianma_tm070jdhg30_timing = {
	.pixelclock = { 62600000, 68200000, 78100000 },
	.hactive = { 1280, 1280, 1280 },
	.hfront_porch = { 15, 64, 159 },
	.hback_porch = { 5, 5, 5 },
	.hsync_len = { 1, 1, 256 },
	.vactive = { 800, 800, 800 },
	.vfront_porch = { 3, 40, 99 },
	.vback_porch = { 2, 2, 2 },
	.vsync_len = { 1, 1, 128 },
	.flags = DISPLAY_FLAGS_DE_HIGH,
};

static const struct panel_desc tianma_tm070jdhg30 = {
	.timings = &tianma_tm070jdhg30_timing,
	.num_timings = 1,
	.bpc = 8,
	.size = {
		.width = 151,
		.height = 95,
	},
	.bus_format = MEDIA_BUS_FMT_RGB888_1X7X4_SPWG,
	.connector_type = DRM_MODE_CONNECTOR_LVDS,
	.bus_flags = DRM_BUS_FLAG_DE_HIGH,
};

static const struct panel_desc tianma_tm070jvhg33 = {
	.timings = &tianma_tm070jdhg30_timing,
	.num_timings = 1,
	.bpc = 8,
	.size = {
		.width = 150,
		.height = 94,
	},
	.bus_format = MEDIA_BUS_FMT_RGB888_1X7X4_SPWG,
	.connector_type = DRM_MODE_CONNECTOR_LVDS,
	.bus_flags = DRM_BUS_FLAG_DE_HIGH,
};

static const struct display_timing tianma_tm070rvhg71_timing = {
	.pixelclock = { 27700000, 29200000, 39600000 },
	.hactive = { 800, 800, 800 },
	.hfront_porch = { 12, 40, 212 },
	.hback_porch = { 88, 88, 88 },
	.hsync_len = { 1, 1, 40 },
	.vactive = { 480, 480, 480 },
	.vfront_porch = { 1, 13, 88 },
	.vback_porch = { 32, 32, 32 },
	.vsync_len = { 1, 1, 3 },
	.flags = DISPLAY_FLAGS_DE_HIGH,
};

static const struct panel_desc tianma_tm070rvhg71 = {
	.timings = &tianma_tm070rvhg71_timing,
	.num_timings = 1,
	.bpc = 8,
	.size = {
		.width = 154,
		.height = 86,
	},
	.bus_format = MEDIA_BUS_FMT_RGB888_1X7X4_SPWG,
	.connector_type = DRM_MODE_CONNECTOR_LVDS,
};

static const struct drm_display_mode ti_nspire_cx_lcd_mode[] = {
	{
		.clock = 10000,
		.hdisplay = 320,
		.hsync_start = 320 + 50,
		.hsync_end = 320 + 50 + 6,
		.htotal = 320 + 50 + 6 + 38,
		.vdisplay = 240,
		.vsync_start = 240 + 3,
		.vsync_end = 240 + 3 + 1,
		.vtotal = 240 + 3 + 1 + 17,
		.flags = DRM_MODE_FLAG_NVSYNC | DRM_MODE_FLAG_NHSYNC,
	},
};

static const struct panel_desc ti_nspire_cx_lcd_panel = {
	.modes = ti_nspire_cx_lcd_mode,
	.num_modes = 1,
	.bpc = 8,
	.size = {
		.width = 65,
		.height = 49,
	},
	.bus_format = MEDIA_BUS_FMT_RGB888_1X24,
	.bus_flags = DRM_BUS_FLAG_PIXDATA_SAMPLE_POSEDGE,
};

static const struct drm_display_mode ti_nspire_classic_lcd_mode[] = {
	{
		.clock = 10000,
		.hdisplay = 320,
		.hsync_start = 320 + 6,
		.hsync_end = 320 + 6 + 6,
		.htotal = 320 + 6 + 6 + 6,
		.vdisplay = 240,
		.vsync_start = 240 + 0,
		.vsync_end = 240 + 0 + 1,
		.vtotal = 240 + 0 + 1 + 0,
		.flags = DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC,
	},
};

static const struct panel_desc ti_nspire_classic_lcd_panel = {
	.modes = ti_nspire_classic_lcd_mode,
	.num_modes = 1,
	/* The grayscale panel has 8 bit for the color .. Y (black) */
	.bpc = 8,
	.size = {
		.width = 71,
		.height = 53,
	},
	/* This is the grayscale bus format */
	.bus_format = MEDIA_BUS_FMT_Y8_1X8,
	.bus_flags = DRM_BUS_FLAG_PIXDATA_SAMPLE_NEGEDGE,
};

static const struct drm_display_mode toshiba_lt089ac29000_mode = {
	.clock = 79500,
	.hdisplay = 1280,
	.hsync_start = 1280 + 192,
	.hsync_end = 1280 + 192 + 128,
	.htotal = 1280 + 192 + 128 + 64,
	.vdisplay = 768,
	.vsync_start = 768 + 20,
	.vsync_end = 768 + 20 + 7,
	.vtotal = 768 + 20 + 7 + 3,
};

static const struct panel_desc toshiba_lt089ac29000 = {
	.modes = &toshiba_lt089ac29000_mode,
	.num_modes = 1,
	.size = {
		.width = 194,
		.height = 116,
	},
	.bus_format = MEDIA_BUS_FMT_RGB888_1X7X4_JEIDA,
	.bus_flags = DRM_BUS_FLAG_DE_HIGH,
	.connector_type = DRM_MODE_CONNECTOR_LVDS,
};

static const struct drm_display_mode tpk_f07a_0102_mode = {
	.clock = 33260,
	.hdisplay = 800,
	.hsync_start = 800 + 40,
	.hsync_end = 800 + 40 + 128,
	.htotal = 800 + 40 + 128 + 88,
	.vdisplay = 480,
	.vsync_start = 480 + 10,
	.vsync_end = 480 + 10 + 2,
	.vtotal = 480 + 10 + 2 + 33,
};

static const struct panel_desc tpk_f07a_0102 = {
	.modes = &tpk_f07a_0102_mode,
	.num_modes = 1,
	.size = {
		.width = 152,
		.height = 91,
	},
	.bus_flags = DRM_BUS_FLAG_PIXDATA_DRIVE_POSEDGE,
};

static const struct drm_display_mode tpk_f10a_0102_mode = {
	.clock = 45000,
	.hdisplay = 1024,
	.hsync_start = 1024 + 176,
	.hsync_end = 1024 + 176 + 5,
	.htotal = 1024 + 176 + 5 + 88,
	.vdisplay = 600,
	.vsync_start = 600 + 20,
	.vsync_end = 600 + 20 + 5,
	.vtotal = 600 + 20 + 5 + 25,
};

static const struct panel_desc tpk_f10a_0102 = {
	.modes = &tpk_f10a_0102_mode,
	.num_modes = 1,
	.size = {
		.width = 223,
		.height = 125,
	},
};

static const struct display_timing urt_umsh_8596md_timing = {
	.pixelclock = { 33260000, 33260000, 33260000 },
	.hactive = { 800, 800, 800 },
	.hfront_porch = { 41, 41, 41 },
	.hback_porch = { 216 - 128, 216 - 128, 216 - 128 },
	.hsync_len = { 71, 128, 128 },
	.vactive = { 480, 480, 480 },
	.vfront_porch = { 10, 10, 10 },
	.vback_porch = { 35 - 2, 35 - 2, 35 - 2 },
	.vsync_len = { 2, 2, 2 },
	.flags = DISPLAY_FLAGS_DE_HIGH | DISPLAY_FLAGS_PIXDATA_NEGEDGE |
		DISPLAY_FLAGS_HSYNC_LOW | DISPLAY_FLAGS_VSYNC_LOW,
};

static const struct panel_desc urt_umsh_8596md_lvds = {
	.timings = &urt_umsh_8596md_timing,
	.num_timings = 1,
	.bpc = 6,
	.size = {
		.width = 152,
		.height = 91,
	},
	.bus_format = MEDIA_BUS_FMT_RGB666_1X7X3_SPWG,
	.connector_type = DRM_MODE_CONNECTOR_LVDS,
};

static const struct panel_desc urt_umsh_8596md_parallel = {
	.timings = &urt_umsh_8596md_timing,
	.num_timings = 1,
	.bpc = 6,
	.size = {
		.width = 152,
		.height = 91,
	},
	.bus_format = MEDIA_BUS_FMT_RGB666_1X18,
};

static const struct drm_display_mode vivax_tpc9150_panel_mode = {
	.clock = 60000,
	.hdisplay = 1024,
	.hsync_start = 1024 + 160,
	.hsync_end = 1024 + 160 + 100,
	.htotal = 1024 + 160 + 100 + 60,
	.vdisplay = 600,
	.vsync_start = 600 + 12,
	.vsync_end = 600 + 12 + 10,
	.vtotal = 600 + 12 + 10 + 13,
};

static const struct panel_desc vivax_tpc9150_panel = {
	.modes = &vivax_tpc9150_panel_mode,
	.num_modes = 1,
	.bpc = 6,
	.size = {
		.width = 200,
		.height = 115,
	},
	.bus_format = MEDIA_BUS_FMT_RGB666_1X7X3_SPWG,
	.bus_flags = DRM_BUS_FLAG_DE_HIGH,
	.connector_type = DRM_MODE_CONNECTOR_LVDS,
};

static const struct drm_display_mode vl050_8048nt_c01_mode = {
	.clock = 33333,
	.hdisplay = 800,
	.hsync_start = 800 + 210,
	.hsync_end = 800 + 210 + 20,
	.htotal = 800 + 210 + 20 + 46,
	.vdisplay =  480,
	.vsync_start = 480 + 22,
	.vsync_end = 480 + 22 + 10,
	.vtotal = 480 + 22 + 10 + 23,
	.flags = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC,
};

static const struct panel_desc vl050_8048nt_c01 = {
	.modes = &vl050_8048nt_c01_mode,
	.num_modes = 1,
	.bpc = 8,
	.size = {
		.width = 120,
		.height = 76,
	},
	.bus_format = MEDIA_BUS_FMT_RGB888_1X24,
	.bus_flags = DRM_BUS_FLAG_DE_HIGH | DRM_BUS_FLAG_PIXDATA_SAMPLE_NEGEDGE,
};

static const struct drm_display_mode winstar_wf35ltiacd_mode = {
	.clock = 6410,
	.hdisplay = 320,
	.hsync_start = 320 + 20,
	.hsync_end = 320 + 20 + 30,
	.htotal = 320 + 20 + 30 + 38,
	.vdisplay = 240,
	.vsync_start = 240 + 4,
	.vsync_end = 240 + 4 + 3,
	.vtotal = 240 + 4 + 3 + 15,
	.flags = DRM_MODE_FLAG_NVSYNC | DRM_MODE_FLAG_NHSYNC,
};

static const struct panel_desc winstar_wf35ltiacd = {
	.modes = &winstar_wf35ltiacd_mode,
	.num_modes = 1,
	.bpc = 8,
	.size = {
		.width = 70,
		.height = 53,
	},
	.bus_format = MEDIA_BUS_FMT_RGB888_1X24,
};

static const struct drm_display_mode yes_optoelectronics_ytc700tlag_05_201c_mode = {
	.clock = 51200,
	.hdisplay = 1024,
	.hsync_start = 1024 + 100,
	.hsync_end = 1024 + 100 + 100,
	.htotal = 1024 + 100 + 100 + 120,
	.vdisplay = 600,
	.vsync_start = 600 + 10,
	.vsync_end = 600 + 10 + 10,
	.vtotal = 600 + 10 + 10 + 15,
	.flags = DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC,
};

static const struct panel_desc yes_optoelectronics_ytc700tlag_05_201c = {
	.modes = &yes_optoelectronics_ytc700tlag_05_201c_mode,
	.num_modes = 1,
	.bpc = 8,
	.size = {
		.width = 154,
		.height = 90,
	},
	.bus_flags = DRM_BUS_FLAG_DE_HIGH,
	.bus_format = MEDIA_BUS_FMT_RGB888_1X7X4_SPWG,
	.connector_type = DRM_MODE_CONNECTOR_LVDS,
};

static const struct drm_display_mode mchp_ac69t88a_mode = {
	.clock = 25000,
	.hdisplay = 800,
	.hsync_start = 800 + 88,
	.hsync_end = 800 + 88 + 5,
	.htotal = 800 + 88 + 5 + 40,
	.vdisplay = 480,
	.vsync_start = 480 + 23,
	.vsync_end = 480 + 23 + 5,
	.vtotal = 480 + 23 + 5 + 1,
};

static const struct panel_desc mchp_ac69t88a = {
	.modes = &mchp_ac69t88a_mode,
	.num_modes = 1,
	.bpc = 8,
	.size = {
		.width = 108,
		.height = 65,
	},
	.bus_flags = DRM_BUS_FLAG_DE_HIGH,
	.bus_format = MEDIA_BUS_FMT_RGB888_1X7X4_JEIDA,
	.connector_type = DRM_MODE_CONNECTOR_LVDS,
};

static const struct drm_display_mode arm_rtsm_mode[] = {
	{
		.clock = 65000,
		.hdisplay = 1024,
		.hsync_start = 1024 + 24,
		.hsync_end = 1024 + 24 + 136,
		.htotal = 1024 + 24 + 136 + 160,
		.vdisplay = 768,
		.vsync_start = 768 + 3,
		.vsync_end = 768 + 3 + 6,
		.vtotal = 768 + 3 + 6 + 29,
		.flags = DRM_MODE_FLAG_NVSYNC | DRM_MODE_FLAG_NHSYNC,
	},
};

static const struct panel_desc arm_rtsm = {
	.modes = arm_rtsm_mode,
	.num_modes = 1,
	.bpc = 8,
	.size = {
		.width = 400,
		.height = 300,
	},
	.bus_format = MEDIA_BUS_FMT_RGB888_1X24,
};

static const struct of_device_id platform_of_match[] = {
	{
		.compatible = "ampire,am-1280800n3tzqw-t00h",
		.data = &ampire_am_1280800n3tzqw_t00h,
	}, {
		.compatible = "ampire,am-480272h3tmqw-t01h",
		.data = &ampire_am_480272h3tmqw_t01h,
	}, {
		.compatible = "ampire,am-800480l1tmqw-t00h",
		.data = &ampire_am_800480l1tmqw_t00h,
	}, {
		.compatible = "ampire,am800480r3tmqwa1h",
		.data = &ampire_am800480r3tmqwa1h,
	}, {
		.compatible = "ampire,am800600p5tmqw-tb8h",
		.data = &ampire_am800600p5tmqwtb8h,
	}, {
		.compatible = "arm,rtsm-display",
		.data = &arm_rtsm,
	}, {
		.compatible = "armadeus,st0700-adapt",
		.data = &armadeus_st0700_adapt,
	}, {
		.compatible = "auo,b101aw03",
		.data = &auo_b101aw03,
	}, {
		.compatible = "auo,b101xtn01",
		.data = &auo_b101xtn01,
	}, {
		.compatible = "auo,b116xw03",
		.data = &auo_b116xw03,
	}, {
		.compatible = "auo,g070vvn01",
		.data = &auo_g070vvn01,
	}, {
		.compatible = "auo,g101evn010",
		.data = &auo_g101evn010,
	}, {
		.compatible = "auo,g104sn02",
		.data = &auo_g104sn02,
	}, {
		.compatible = "auo,g104stn01",
		.data = &auo_g104stn01,
	}, {
		.compatible = "auo,g121ean01",
		.data = &auo_g121ean01,
	}, {
		.compatible = "auo,g133han01",
		.data = &auo_g133han01,
	}, {
		.compatible = "auo,g156han04",
		.data = &auo_g156han04,
	}, {
		.compatible = "auo,g156xtn01",
		.data = &auo_g156xtn01,
	}, {
		.compatible = "auo,g185han01",
		.data = &auo_g185han01,
	}, {
		.compatible = "auo,g190ean01",
		.data = &auo_g190ean01,
	}, {
		.compatible = "auo,p320hvn03",
		.data = &auo_p320hvn03,
	}, {
		.compatible = "auo,t215hvn01",
		.data = &auo_t215hvn01,
	}, {
		.compatible = "avic,tm070ddh03",
		.data = &avic_tm070ddh03,
	}, {
		.compatible = "bananapi,s070wv20-ct16",
		.data = &bananapi_s070wv20_ct16,
	}, {
		.compatible = "boe,bp082wx1-100",
		.data = &boe_bp082wx1_100,
	}, {
		.compatible = "boe,bp101wx1-100",
		.data = &boe_bp101wx1_100,
	}, {
		.compatible = "boe,ev121wxm-n10-1850",
		.data = &boe_ev121wxm_n10_1850,
	}, {
		.compatible = "boe,hv070wsa-100",
		.data = &boe_hv070wsa
	}, {
		.compatible = "cct,cmt430b19n00",
		.data = &cct_cmt430b19n00,
	}, {
		.compatible = "cdtech,s043wq26h-ct7",
		.data = &cdtech_s043wq26h_ct7,
	}, {
		.compatible = "cdtech,s070pws19hp-fc21",
		.data = &cdtech_s070pws19hp_fc21,
	}, {
		.compatible = "cdtech,s070swv29hg-dc44",
		.data = &cdtech_s070swv29hg_dc44,
	}, {
		.compatible = "cdtech,s070wv95-ct16",
		.data = &cdtech_s070wv95_ct16,
	}, {
		.compatible = "chefree,ch101olhlwh-002",
		.data = &chefree_ch101olhlwh_002,
	}, {
		.compatible = "chunghwa,claa070wp03xg",
		.data = &chunghwa_claa070wp03xg,
	}, {
		.compatible = "chunghwa,claa101wa01a",
		.data = &chunghwa_claa101wa01a
	}, {
		.compatible = "chunghwa,claa101wb01",
		.data = &chunghwa_claa101wb01
	}, {
		.compatible = "dataimage,fg040346dsswbg04",
		.data = &dataimage_fg040346dsswbg04,
	}, {
		.compatible = "dataimage,fg1001l0dsswmg01",
		.data = &dataimage_fg1001l0dsswmg01,
	}, {
		.compatible = "dataimage,scf0700c48ggu18",
		.data = &dataimage_scf0700c48ggu18,
	}, {
		.compatible = "dlc,dlc0700yzg-1",
		.data = &dlc_dlc0700yzg_1,
	}, {
		.compatible = "dlc,dlc1010gig",
		.data = &dlc_dlc1010gig,
	}, {
		.compatible = "edt,et035012dm6",
		.data = &edt_et035012dm6,
	}, {
		.compatible = "edt,etm0350g0dh6",
		.data = &edt_etm0350g0dh6,
	}, {
		.compatible = "edt,etm043080dh6gp",
		.data = &edt_etm043080dh6gp,
	}, {
		.compatible = "edt,etm0430g0dh6",
		.data = &edt_etm0430g0dh6,
	}, {
		.compatible = "edt,et057090dhu",
		.data = &edt_et057090dhu,
	}, {
		.compatible = "edt,et070080dh6",
		.data = &edt_etm0700g0dh6,
	}, {
		.compatible = "edt,etm0700g0dh6",
		.data = &edt_etm0700g0dh6,
	}, {
		.compatible = "edt,etm0700g0bdh6",
		.data = &edt_etm0700g0bdh6,
	}, {
		.compatible = "edt,etm0700g0edh6",
		.data = &edt_etm0700g0bdh6,
	}, {
		.compatible = "edt,etml0700y5dha",
		.data = &edt_etml0700y5dha,
	}, {
		.compatible = "edt,etml1010g3dra",
		.data = &edt_etml1010g3dra,
	}, {
		.compatible = "edt,etmv570g2dhu",
		.data = &edt_etmv570g2dhu,
	}, {
		.compatible = "eink,vb3300-kca",
		.data = &eink_vb3300_kca,
	}, {
		.compatible = "evervision,vgg644804",
		.data = &evervision_vgg644804,
	}, {
		.compatible = "evervision,vgg804821",
		.data = &evervision_vgg804821,
	}, {
		.compatible = "foxlink,fl500wvr00-a0t",
		.data = &foxlink_fl500wvr00_a0t,
	}, {
		.compatible = "frida,frd350h54004",
		.data = &frida_frd350h54004,
	}, {
		.compatible = "friendlyarm,hd702e",
		.data = &friendlyarm_hd702e,
	}, {
		.compatible = "geekworm,mzp280",
		.data = &geekworm_mzp280,
	}, {
		.compatible = "giantplus,gpg482739qs5",
		.data = &giantplus_gpg482739qs5
	}, {
		.compatible = "giantplus,gpm940b0",
		.data = &giantplus_gpm940b0,
	}, {
		.compatible = "hannstar,hsd070pww1",
		.data = &hannstar_hsd070pww1,
	}, {
		.compatible = "hannstar,hsd100pxn1",
		.data = &hannstar_hsd100pxn1,
	}, {
		.compatible = "hannstar,hsd101pww2",
		.data = &hannstar_hsd101pww2,
	}, {
		.compatible = "hit,tx23d38vm0caa",
		.data = &hitachi_tx23d38vm0caa
	}, {
		.compatible = "innolux,at043tn24",
		.data = &innolux_at043tn24,
	}, {
		.compatible = "innolux,at056tn53v1",
		.data = &innolux_at056tn53v1,
	}, {
		.compatible = "innolux,at070tn92",
		.data = &innolux_at070tn92,
	}, {
		.compatible = "innolux,g070ace-l01",
		.data = &innolux_g070ace_l01,
	}, {
		.compatible = "innolux,g070ace-lh3",
		.data = &innolux_g070ace_lh3,
	}, {
		.compatible = "innolux,g070y2-l01",
		.data = &innolux_g070y2_l01,
	}, {
		.compatible = "innolux,g070y2-t02",
		.data = &innolux_g070y2_t02,
	}, {
		.compatible = "innolux,g101ice-l01",
		.data = &innolux_g101ice_l01
	}, {
		.compatible = "innolux,g121i1-l01",
		.data = &innolux_g121i1_l01
	}, {
		.compatible = "innolux,g121x1-l03",
		.data = &innolux_g121x1_l03,
	}, {
		.compatible = "innolux,g121xce-l01",
		.data = &innolux_g121xce_l01,
	}, {
		.compatible = "innolux,g156hce-l01",
		.data = &innolux_g156hce_l01,
	}, {
		.compatible = "innolux,n156bge-l21",
		.data = &innolux_n156bge_l21,
	}, {
		.compatible = "innolux,zj070na-01p",
		.data = &innolux_zj070na_01p,
	}, {
		.compatible = "koe,tx14d24vm1bpa",
		.data = &koe_tx14d24vm1bpa,
	}, {
		.compatible = "koe,tx26d202vm0bwa",
		.data = &koe_tx26d202vm0bwa,
	}, {
		.compatible = "koe,tx31d200vm0baa",
		.data = &koe_tx31d200vm0baa,
	}, {
		.compatible = "kyo,tcg121xglp",
		.data = &kyo_tcg121xglp,
	}, {
		.compatible = "lemaker,bl035-rgb-002",
		.data = &lemaker_bl035_rgb_002,
	}, {
		.compatible = "lg,lb070wv8",
		.data = &lg_lb070wv8,
	}, {
		.compatible = "lincolntech,lcd185-101ct",
		.data = &lincolntech_lcd185_101ct,
	}, {
		.compatible = "logicpd,type28",
		.data = &logicpd_type_28,
	}, {
		.compatible = "logictechno,lt161010-2nhc",
		.data = &logictechno_lt161010_2nh,
	}, {
		.compatible = "logictechno,lt161010-2nhr",
		.data = &logictechno_lt161010_2nh,
	}, {
		.compatible = "logictechno,lt170410-2whc",
		.data = &logictechno_lt170410_2whc,
	}, {
		.compatible = "logictechno,lttd800480070-l2rt",
		.data = &logictechno_lttd800480070_l2rt,
	}, {
		.compatible = "logictechno,lttd800480070-l6wh-rt",
		.data = &logictechno_lttd800480070_l6wh_rt,
	}, {
		.compatible = "microtips,mf-101hiebcaf0",
		.data = &microtips_mf_101hiebcaf0_c,
	}, {
		.compatible = "microtips,mf-103hieb0ga0",
		.data = &microtips_mf_103hieb0ga0,
	}, {
		.compatible = "mitsubishi,aa070mc01-ca1",
		.data = &mitsubishi_aa070mc01,
	}, {
		.compatible = "mitsubishi,aa084xe01",
		.data = &mitsubishi_aa084xe01,
	}, {
		.compatible = "multi-inno,mi0700s4t-6",
		.data = &multi_inno_mi0700s4t_6,
	}, {
		.compatible = "multi-inno,mi0800ft-9",
		.data = &multi_inno_mi0800ft_9,
	}, {
		.compatible = "multi-inno,mi1010ait-1cp",
		.data = &multi_inno_mi1010ait_1cp,
	}, {
		.compatible = "nec,nl12880bc20-05",
		.data = &nec_nl12880bc20_05,
	}, {
		.compatible = "nec,nl4827hc19-05b",
		.data = &nec_nl4827hc19_05b,
	}, {
		.compatible = "netron-dy,e231732",
		.data = &netron_dy_e231732,
	}, {
		.compatible = "newhaven,nhd-4.3-480272ef-atxl",
		.data = &newhaven_nhd_43_480272ef_atxl,
	}, {
		.compatible = "nlt,nl192108ac18-02d",
		.data = &nlt_nl192108ac18_02d,
	}, {
		.compatible = "nvd,9128",
		.data = &nvd_9128,
	}, {
		.compatible = "okaya,rs800480t-7x0gp",
		.data = &okaya_rs800480t_7x0gp,
	}, {
		.compatible = "olimex,lcd-olinuxino-43-ts",
		.data = &olimex_lcd_olinuxino_43ts,
	}, {
		.compatible = "ontat,kd50g21-40nt-a1",
		.data = &ontat_kd50g21_40nt_a1,
	}, {
		.compatible = "ontat,yx700wv03",
		.data = &ontat_yx700wv03,
	}, {
		.compatible = "ortustech,com37h3m05dtc",
		.data = &ortustech_com37h3m,
	}, {
		.compatible = "ortustech,com37h3m99dtc",
		.data = &ortustech_com37h3m,
	}, {
		.compatible = "ortustech,com43h4m85ulc",
		.data = &ortustech_com43h4m85ulc,
	}, {
		.compatible = "osddisplays,osd070t1718-19ts",
		.data = &osddisplays_osd070t1718_19ts,
	}, {
		.compatible = "pda,91-00156-a0",
		.data = &pda_91_00156_a0,
	}, {
		.compatible = "powertip,ph128800t006-zhc01",
		.data = &powertip_ph128800t006_zhc01,
	}, {
		.compatible = "powertip,ph800480t013-idf02",
		.data = &powertip_ph800480t013_idf02,
	}, {
		.compatible = "primeview,pm070wl4",
		.data = &primeview_pm070wl4,
	}, {
		.compatible = "qiaodian,qd43003c0-40",
		.data = &qd43003c0_40,
	}, {
		.compatible = "qishenglong,gopher2b-lcd",
		.data = &qishenglong_gopher2b_lcd,
	}, {
		.compatible = "rocktech,rk043fn48h",
		.data = &rocktech_rk043fn48h,
	}, {
		.compatible = "raspberrypi,7inch-dsi",
		.data = &raspberrypi_7inch,
	}, {
		.compatible = "rocktech,rk070er9427",
		.data = &rocktech_rk070er9427,
	}, {
		.compatible = "rocktech,rk101ii01d-ct",
		.data = &rocktech_rk101ii01d_ct,
	}, {
		.compatible = "samsung,ltl101al01",
		.data = &samsung_ltl101al01,
	}, {
		.compatible = "samsung,ltn101nt05",
		.data = &samsung_ltn101nt05,
	}, {
		.compatible = "satoz,sat050at40h12r2",
		.data = &satoz_sat050at40h12r2,
	}, {
		.compatible = "sharp,lq035q7db03",
		.data = &sharp_lq035q7db03,
	}, {
		.compatible = "sharp,lq070y3dg3b",
		.data = &sharp_lq070y3dg3b,
	}, {
		.compatible = "sharp,lq101k1ly04",
		.data = &sharp_lq101k1ly04,
	}, {
		.compatible = "sharp,ls020b1dd01d",
		.data = &sharp_ls020b1dd01d,
	}, {
		.compatible = "shelly,sca07010-bfn-lnn",
		.data = &shelly_sca07010_bfn_lnn,
	}, {
		.compatible = "starry,kr070pe2t",
		.data = &starry_kr070pe2t,
	}, {
		.compatible = "startek,kd070wvfpa",
		.data = &startek_kd070wvfpa,
	}, {
		.compatible = "team-source-display,tst043015cmhx",
		.data = &tsd_tst043015cmhx,
	}, {
		.compatible = "tfc,s9700rtwv43tr-01b",
		.data = &tfc_s9700rtwv43tr_01b,
	}, {
		.compatible = "tianma,tm070jdhg30",
		.data = &tianma_tm070jdhg30,
	}, {
		.compatible = "tianma,tm070jvhg33",
		.data = &tianma_tm070jvhg33,
	}, {
		.compatible = "tianma,tm070rvhg71",
		.data = &tianma_tm070rvhg71,
	}, {
		.compatible = "ti,nspire-cx-lcd-panel",
		.data = &ti_nspire_cx_lcd_panel,
	}, {
		.compatible = "ti,nspire-classic-lcd-panel",
		.data = &ti_nspire_classic_lcd_panel,
	}, {
		.compatible = "toshiba,lt089ac29000",
		.data = &toshiba_lt089ac29000,
	}, {
		.compatible = "tpk,f07a-0102",
		.data = &tpk_f07a_0102,
	}, {
		.compatible = "tpk,f10a-0102",
		.data = &tpk_f10a_0102,
	}, {
		.compatible = "urt,umsh-8596md-t",
		.data = &urt_umsh_8596md_parallel,
	}, {
		.compatible = "urt,umsh-8596md-1t",
		.data = &urt_umsh_8596md_parallel,
	}, {
		.compatible = "urt,umsh-8596md-7t",
		.data = &urt_umsh_8596md_parallel,
	}, {
		.compatible = "urt,umsh-8596md-11t",
		.data = &urt_umsh_8596md_lvds,
	}, {
		.compatible = "urt,umsh-8596md-19t",
		.data = &urt_umsh_8596md_lvds,
	}, {
		.compatible = "urt,umsh-8596md-20t",
		.data = &urt_umsh_8596md_parallel,
	}, {
		.compatible = "vivax,tpc9150-panel",
		.data = &vivax_tpc9150_panel,
	}, {
		.compatible = "vxt,vl050-8048nt-c01",
		.data = &vl050_8048nt_c01,
	}, {
		.compatible = "winstar,wf35ltiacd",
		.data = &winstar_wf35ltiacd,
	}, {
		.compatible = "yes-optoelectronics,ytc700tlag-05-201c",
		.data = &yes_optoelectronics_ytc700tlag_05_201c,
	}, {
		.compatible = "microchip,ac69t88a",
		.data = &mchp_ac69t88a,
	}, {
		/* Must be the last entry */
		.compatible = "panel-dpi",
		.data = &panel_dpi,
	}, {
		/* sentinel */
	}
};
MODULE_DEVICE_TABLE(of, platform_of_match);

static int panel_simple_platform_probe(struct platform_device *pdev)
{
	const struct panel_desc *desc;

	desc = of_device_get_match_data(&pdev->dev);
	if (!desc)
		return -ENODEV;

	return panel_simple_probe(&pdev->dev, desc);
}

static void panel_simple_platform_remove(struct platform_device *pdev)
{
	panel_simple_remove(&pdev->dev);
}

static void panel_simple_platform_shutdown(struct platform_device *pdev)
{
	panel_simple_shutdown(&pdev->dev);
}

static const struct dev_pm_ops panel_simple_pm_ops = {
	SET_RUNTIME_PM_OPS(panel_simple_suspend, panel_simple_resume, NULL)
	SET_SYSTEM_SLEEP_PM_OPS(pm_runtime_force_suspend,
				pm_runtime_force_resume)
};

static struct platform_driver panel_simple_platform_driver = {
	.driver = {
		.name = "panel-simple",
		.of_match_table = platform_of_match,
		.pm = &panel_simple_pm_ops,
	},
	.probe = panel_simple_platform_probe,
	.remove_new = panel_simple_platform_remove,
	.shutdown = panel_simple_platform_shutdown,
};

struct panel_desc_dsi {
	struct panel_desc desc;

	unsigned long flags;
	enum mipi_dsi_pixel_format format;
	unsigned int lanes;
};

static const struct drm_display_mode auo_b080uan01_mode = {
	.clock = 154500,
	.hdisplay = 1200,
	.hsync_start = 1200 + 62,
	.hsync_end = 1200 + 62 + 4,
	.htotal = 1200 + 62 + 4 + 62,
	.vdisplay = 1920,
	.vsync_start = 1920 + 9,
	.vsync_end = 1920 + 9 + 2,
	.vtotal = 1920 + 9 + 2 + 8,
};

static const struct panel_desc_dsi auo_b080uan01 = {
	.desc = {
		.modes = &auo_b080uan01_mode,
		.num_modes = 1,
		.bpc = 8,
		.size = {
			.width = 108,
			.height = 272,
		},
		.connector_type = DRM_MODE_CONNECTOR_DSI,
	},
	.flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_CLOCK_NON_CONTINUOUS,
	.format = MIPI_DSI_FMT_RGB888,
	.lanes = 4,
};

static const struct drm_display_mode boe_tv080wum_nl0_mode = {
	.clock = 160000,
	.hdisplay = 1200,
	.hsync_start = 1200 + 120,
	.hsync_end = 1200 + 120 + 20,
	.htotal = 1200 + 120 + 20 + 21,
	.vdisplay = 1920,
	.vsync_start = 1920 + 21,
	.vsync_end = 1920 + 21 + 3,
	.vtotal = 1920 + 21 + 3 + 18,
	.flags = DRM_MODE_FLAG_NVSYNC | DRM_MODE_FLAG_NHSYNC,
};

static const struct panel_desc_dsi boe_tv080wum_nl0 = {
	.desc = {
		.modes = &boe_tv080wum_nl0_mode,
		.num_modes = 1,
		.size = {
			.width = 107,
			.height = 172,
		},
		.connector_type = DRM_MODE_CONNECTOR_DSI,
	},
	.flags = MIPI_DSI_MODE_VIDEO |
		 MIPI_DSI_MODE_VIDEO_BURST |
		 MIPI_DSI_MODE_VIDEO_SYNC_PULSE,
	.format = MIPI_DSI_FMT_RGB888,
	.lanes = 4,
};

static const struct drm_display_mode lg_ld070wx3_sl01_mode = {
	.clock = 71000,
	.hdisplay = 800,
	.hsync_start = 800 + 32,
	.hsync_end = 800 + 32 + 1,
	.htotal = 800 + 32 + 1 + 57,
	.vdisplay = 1280,
	.vsync_start = 1280 + 28,
	.vsync_end = 1280 + 28 + 1,
	.vtotal = 1280 + 28 + 1 + 14,
};

static const struct panel_desc_dsi lg_ld070wx3_sl01 = {
	.desc = {
		.modes = &lg_ld070wx3_sl01_mode,
		.num_modes = 1,
		.bpc = 8,
		.size = {
			.width = 94,
			.height = 151,
		},
		.connector_type = DRM_MODE_CONNECTOR_DSI,
	},
	.flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_CLOCK_NON_CONTINUOUS,
	.format = MIPI_DSI_FMT_RGB888,
	.lanes = 4,
};

static const struct drm_display_mode lg_lh500wx1_sd03_mode = {
	.clock = 67000,
	.hdisplay = 720,
	.hsync_start = 720 + 12,
	.hsync_end = 720 + 12 + 4,
	.htotal = 720 + 12 + 4 + 112,
	.vdisplay = 1280,
	.vsync_start = 1280 + 8,
	.vsync_end = 1280 + 8 + 4,
	.vtotal = 1280 + 8 + 4 + 12,
};

static const struct panel_desc_dsi lg_lh500wx1_sd03 = {
	.desc = {
		.modes = &lg_lh500wx1_sd03_mode,
		.num_modes = 1,
		.bpc = 8,
		.size = {
			.width = 62,
			.height = 110,
		},
		.connector_type = DRM_MODE_CONNECTOR_DSI,
	},
	.flags = MIPI_DSI_MODE_VIDEO,
	.format = MIPI_DSI_FMT_RGB888,
	.lanes = 4,
};

static const struct drm_display_mode panasonic_vvx10f004b00_mode = {
	.clock = 157200,
	.hdisplay = 1920,
	.hsync_start = 1920 + 154,
	.hsync_end = 1920 + 154 + 16,
	.htotal = 1920 + 154 + 16 + 32,
	.vdisplay = 1200,
	.vsync_start = 1200 + 17,
	.vsync_end = 1200 + 17 + 2,
	.vtotal = 1200 + 17 + 2 + 16,
};

static const struct panel_desc_dsi panasonic_vvx10f004b00 = {
	.desc = {
		.modes = &panasonic_vvx10f004b00_mode,
		.num_modes = 1,
		.bpc = 8,
		.size = {
			.width = 217,
			.height = 136,
		},
		.connector_type = DRM_MODE_CONNECTOR_DSI,
	},
	.flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_SYNC_PULSE |
		 MIPI_DSI_CLOCK_NON_CONTINUOUS,
	.format = MIPI_DSI_FMT_RGB888,
	.lanes = 4,
};

static const struct drm_display_mode lg_acx467akm_7_mode = {
	.clock = 150000,
	.hdisplay = 1080,
	.hsync_start = 1080 + 2,
	.hsync_end = 1080 + 2 + 2,
	.htotal = 1080 + 2 + 2 + 2,
	.vdisplay = 1920,
	.vsync_start = 1920 + 2,
	.vsync_end = 1920 + 2 + 2,
	.vtotal = 1920 + 2 + 2 + 2,
};

static const struct panel_desc_dsi lg_acx467akm_7 = {
	.desc = {
		.modes = &lg_acx467akm_7_mode,
		.num_modes = 1,
		.bpc = 8,
		.size = {
			.width = 62,
			.height = 110,
		},
		.connector_type = DRM_MODE_CONNECTOR_DSI,
	},
	.flags = 0,
	.format = MIPI_DSI_FMT_RGB888,
	.lanes = 4,
};

static const struct drm_display_mode osd101t2045_53ts_mode = {
	.clock = 154500,
	.hdisplay = 1920,
	.hsync_start = 1920 + 112,
	.hsync_end = 1920 + 112 + 16,
	.htotal = 1920 + 112 + 16 + 32,
	.vdisplay = 1200,
	.vsync_start = 1200 + 16,
	.vsync_end = 1200 + 16 + 2,
	.vtotal = 1200 + 16 + 2 + 16,
	.flags = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC,
};

static const struct panel_desc_dsi osd101t2045_53ts = {
	.desc = {
		.modes = &osd101t2045_53ts_mode,
		.num_modes = 1,
		.bpc = 8,
		.size = {
			.width = 217,
			.height = 136,
		},
		.connector_type = DRM_MODE_CONNECTOR_DSI,
	},
	.flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_BURST |
		 MIPI_DSI_MODE_VIDEO_SYNC_PULSE |
		 MIPI_DSI_MODE_NO_EOT_PACKET,
	.format = MIPI_DSI_FMT_RGB888,
	.lanes = 4,
};

// for panels using generic panel-dsi binding
static struct panel_desc_dsi panel_dsi;

static const struct of_device_id dsi_of_match[] = {
	{
		.compatible = "auo,b080uan01",
		.data = &auo_b080uan01
	}, {
		.compatible = "boe,tv080wum-nl0",
		.data = &boe_tv080wum_nl0
	}, {
		.compatible = "lg,ld070wx3-sl01",
		.data = &lg_ld070wx3_sl01
	}, {
		.compatible = "lg,lh500wx1-sd03",
		.data = &lg_lh500wx1_sd03
	}, {
		.compatible = "panasonic,vvx10f004b00",
		.data = &panasonic_vvx10f004b00
	}, {
		.compatible = "lg,acx467akm-7",
		.data = &lg_acx467akm_7
	}, {
		.compatible = "osddisplays,osd101t2045-53ts",
		.data = &osd101t2045_53ts
	}, {
		/* Must be the last entry */
		.compatible = "panel-dsi",
		.data = &panel_dsi,
	}, {
		/* sentinel */
	}
};
MODULE_DEVICE_TABLE(of, dsi_of_match);


/* Checks for DSI panel definition in device-tree, analog to panel_dpi */
static int panel_dsi_dt_probe(struct device *dev,
			  struct panel_desc_dsi *desc_dsi)
{
	struct panel_desc *desc;
	struct display_timing *timing;
	const struct device_node *np;
	const char *dsi_color_format;
	const char *dsi_mode_flags;
	struct property *prop;
	int dsi_lanes, ret;

	np = dev->of_node;

	desc = devm_kzalloc(dev, sizeof(*desc), GFP_KERNEL);
	if (!desc)
		return -ENOMEM;

	timing = devm_kzalloc(dev, sizeof(*timing), GFP_KERNEL);
	if (!timing)
		return -ENOMEM;

	ret = of_get_display_timing(np, "panel-timing", timing);
	if (ret < 0) {
		dev_err(dev, "%pOF: no panel-timing node found for \"panel-dsi\" binding\n",
			np);
		return ret;
	}

	desc->timings = timing;
	desc->num_timings = 1;

	of_property_read_u32(np, "width-mm", &desc->size.width);
	of_property_read_u32(np, "height-mm", &desc->size.height);

	dsi_lanes = drm_of_get_data_lanes_count_ep(np, 0, 0, 1, 4);

	if (dsi_lanes < 0) {
		dev_err(dev, "%pOF: no or too many data-lanes defined", np);
		return dsi_lanes;
	}

	desc_dsi->lanes = dsi_lanes;

	of_property_read_string(np, "dsi-color-format", &dsi_color_format);
	if (!strcmp(dsi_color_format, "RGB888")) {
		desc_dsi->format = MIPI_DSI_FMT_RGB888;
		desc->bpc = 8;
	} else if (!strcmp(dsi_color_format, "RGB565")) {
		desc_dsi->format = MIPI_DSI_FMT_RGB565;
		desc->bpc = 6;
	} else if (!strcmp(dsi_color_format, "RGB666")) {
		desc_dsi->format = MIPI_DSI_FMT_RGB666;
		desc->bpc = 6;
	} else if (!strcmp(dsi_color_format, "RGB666_PACKED")) {
		desc_dsi->format = MIPI_DSI_FMT_RGB666_PACKED;
		desc->bpc = 6;
	} else {
		dev_err(dev, "%pOF: no valid dsi-color-format defined", np);
		return -EINVAL;
	}


	of_property_for_each_string(np, "mode", prop, dsi_mode_flags) {
		if (!strcmp(dsi_mode_flags, "MODE_VIDEO"))
			desc_dsi->flags |= MIPI_DSI_MODE_VIDEO;
		else if (!strcmp(dsi_mode_flags, "MODE_VIDEO_BURST"))
			desc_dsi->flags |= MIPI_DSI_MODE_VIDEO_BURST;
		else if (!strcmp(dsi_mode_flags, "MODE_VIDEO_SYNC_PULSE"))
			desc_dsi->flags |= MIPI_DSI_MODE_VIDEO_SYNC_PULSE;
		else if (!strcmp(dsi_mode_flags, "MODE_VIDEO_AUTO_VERT"))
			desc_dsi->flags |= MIPI_DSI_MODE_VIDEO_AUTO_VERT;
		else if (!strcmp(dsi_mode_flags, "MODE_VIDEO_HSE"))
			desc_dsi->flags |= MIPI_DSI_MODE_VIDEO_HSE;
		else if (!strcmp(dsi_mode_flags, "MODE_VIDEO_NO_HFP"))
			desc_dsi->flags |= MIPI_DSI_MODE_VIDEO_NO_HFP;
		else if (!strcmp(dsi_mode_flags, "MODE_VIDEO_NO_HBP"))
			desc_dsi->flags |= MIPI_DSI_MODE_VIDEO_NO_HBP;
		else if (!strcmp(dsi_mode_flags, "MODE_VIDEO_NO_HSA"))
			desc_dsi->flags |= MIPI_DSI_MODE_VIDEO_NO_HSA;
		else if (!strcmp(dsi_mode_flags, "MODE_VSYNC_FLUSH"))
			desc_dsi->flags |= MIPI_DSI_MODE_VSYNC_FLUSH;
		else if (!strcmp(dsi_mode_flags, "MODE_NO_EOT_PACKET"))
			desc_dsi->flags |= MIPI_DSI_MODE_NO_EOT_PACKET;
		else if (!strcmp(dsi_mode_flags, "CLOCK_NON_CONTINUOUS"))
			desc_dsi->flags |= MIPI_DSI_CLOCK_NON_CONTINUOUS;
		else if (!strcmp(dsi_mode_flags, "MODE_LPM"))
			desc_dsi->flags |= MIPI_DSI_MODE_LPM;
		else if (!strcmp(dsi_mode_flags, "HS_PKT_END_ALIGNED"))
			desc_dsi->flags |= MIPI_DSI_HS_PKT_END_ALIGNED;
	}

	desc->connector_type = DRM_MODE_CONNECTOR_DSI;
	desc_dsi->desc = *desc;

	return 0;
}

static int panel_simple_dsi_probe(struct mipi_dsi_device *dsi)
{
	const struct panel_desc_dsi *desc;
	struct panel_desc_dsi *dt_desc;
	int err;

	desc = of_device_get_match_data(&dsi->dev);
	if (!desc)
		return -ENODEV;

	if (desc == &panel_dsi) {
		/* Handle the generic panel-dsi binding */
		dt_desc = devm_kzalloc(&dsi->dev, sizeof(*dt_desc), GFP_KERNEL);
		if (!dt_desc)
			return -ENOMEM;

		err = panel_dsi_dt_probe(&dsi->dev, dt_desc);
		if (err < 0)
			return err;

		desc = dt_desc;
	}

	err = panel_simple_probe(&dsi->dev, &desc->desc);
	if (err < 0)
		return err;

	dsi->mode_flags = desc->flags;
	dsi->format = desc->format;
	dsi->lanes = desc->lanes;

	err = mipi_dsi_attach(dsi);
	if (err) {
		struct panel_simple *panel = mipi_dsi_get_drvdata(dsi);

		drm_panel_remove(&panel->base);
	}

	return err;
}

static void panel_simple_dsi_remove(struct mipi_dsi_device *dsi)
{
	int err;

	err = mipi_dsi_detach(dsi);
	if (err < 0)
		dev_err(&dsi->dev, "failed to detach from DSI host: %d\n", err);

	panel_simple_remove(&dsi->dev);
}

static void panel_simple_dsi_shutdown(struct mipi_dsi_device *dsi)
{
	panel_simple_shutdown(&dsi->dev);
}

static struct mipi_dsi_driver panel_simple_dsi_driver = {
	.driver = {
		.name = "panel-simple-dsi",
		.of_match_table = dsi_of_match,
		.pm = &panel_simple_pm_ops,
	},
	.probe = panel_simple_dsi_probe,
	.remove = panel_simple_dsi_remove,
	.shutdown = panel_simple_dsi_shutdown,
};

static int __init panel_simple_init(void)
{
	int err;

	err = platform_driver_register(&panel_simple_platform_driver);
	if (err < 0)
		return err;

	if (IS_ENABLED(CONFIG_DRM_MIPI_DSI)) {
		err = mipi_dsi_driver_register(&panel_simple_dsi_driver);
		if (err < 0)
			goto err_did_platform_register;
	}

	return 0;

err_did_platform_register:
	platform_driver_unregister(&panel_simple_platform_driver);

	return err;
}
module_init(panel_simple_init);

static void __exit panel_simple_exit(void)
{
	if (IS_ENABLED(CONFIG_DRM_MIPI_DSI))
		mipi_dsi_driver_unregister(&panel_simple_dsi_driver);

	platform_driver_unregister(&panel_simple_platform_driver);
}
module_exit(panel_simple_exit);

MODULE_AUTHOR("Thierry Reding <treding@nvidia.com>");
MODULE_DESCRIPTION("DRM Driver for Simple Panels");
MODULE_LICENSE("GPL and additional rights");
