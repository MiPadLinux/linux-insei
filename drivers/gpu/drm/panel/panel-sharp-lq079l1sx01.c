/*
 * Copyright (C) 2018 Alexandrov Dmitry
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/backlight.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>

#include <linux/of_gpio.h>
#include <linux/gpio.h>

#include <drm/drmP.h>
#include <drm/drm_crtc.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_panel.h>

#include <video/mipi_display.h>
#include <linux/kernel.h>

#define TO_DSI(x)			((x) & 0xFF)
#define MIPI_DCS_RSP_WRITE_DISPLAY_BRIGHTNESS 0x51
#define MIPI_DCS_RSP_WRITE_CONTROL_DISPLAY		0x53
#define MIPI_DCS_RSP_WRITE_ADAPTIVE_BRIGHTNESS_CONTROL	0x55
#include <linux/reboot.h>

struct sharp_panel {
	struct drm_panel base;
	/* the datasheet refers to them as DSI-LINK1 and DSI-LINK2 */
	struct mipi_dsi_device *link1;
	struct mipi_dsi_device *link2;

	struct backlight_device *backlight;
	struct regulator *avdd_lcd_vsp_5v5;
	struct regulator *avdd_lcd_vsn_5v5;
	struct regulator *dvdd_lcd_1v8;
	int reset_gpio;

	bool prepared;
	bool enabled;

	const struct drm_display_mode *mode;
};

static inline struct sharp_panel *to_sharp_panel(struct drm_panel *panel)
{
	return container_of(panel, struct sharp_panel, base);
}

static void sharp_wait_frames(struct sharp_panel *sharp, unsigned int frames)
{
	unsigned int refresh = drm_mode_vrefresh(sharp->mode);

	if (WARN_ON(frames > refresh))
		return;

	msleep(1000 / (refresh / frames));
}

static int sharp_panel_disable(struct drm_panel *panel)
{
	struct sharp_panel *sharp = to_sharp_panel(panel);

	if (!sharp->enabled)
		return 0;

	if (sharp->backlight) {
		sharp->backlight->props.power = FB_BLANK_POWERDOWN;
		backlight_update_status(sharp->backlight);
	}

	sharp->enabled = false;

	return 0;
}

static int sharp_panel_unprepare(struct drm_panel *panel)
{
	struct sharp_panel *sharp = to_sharp_panel(panel);

	if (!sharp->prepared)
		return 0;


	mipi_dsi_dcs_set_display_off(sharp->link1);
	mipi_dsi_dcs_set_display_off(sharp->link2);
	msleep(100);
	mipi_dsi_dcs_enter_sleep_mode(sharp->link1);
	mipi_dsi_dcs_enter_sleep_mode(sharp->link2);
	msleep(150);

	regulator_disable(sharp->avdd_lcd_vsn_5v5);
	regulator_disable(sharp->avdd_lcd_vsp_5v5);
	regulator_disable(sharp->dvdd_lcd_1v8);

	sharp->prepared = false;

	return 0;
}

static int sharp_setup_symmetrical_split(struct mipi_dsi_device *left,
					 struct mipi_dsi_device *right,
					 const struct drm_display_mode *mode)
{
	int err;

	err = mipi_dsi_dcs_set_column_address(left, 0, mode->hdisplay / 2 - 1);
	if (err < 0) {
		dev_err(&left->dev, "failed to set column address: %d\n", err);
		return err;
	}

	err = mipi_dsi_dcs_set_page_address(left, 0, mode->vdisplay - 1);
	if (err < 0) {
		dev_err(&left->dev, "failed to set page address: %d\n", err);
		return err;
	}

	err = mipi_dsi_dcs_set_column_address(right, mode->hdisplay / 2,
					      mode->hdisplay - 1);
	if (err < 0) {
		dev_err(&right->dev, "failed to set column address: %d\n", err);
		return err;
	}

	err = mipi_dsi_dcs_set_page_address(right, 0, mode->vdisplay - 1);
	if (err < 0) {
		dev_err(&right->dev, "failed to set page address: %d\n", err);
		return err;
	}

	return 0;
}

static int panel_sharp_write_adaptive_brightness_control(struct sharp_panel *panel)
{
	int ret;
	u8 data;

	data = TO_DSI(0x01);

	ret = mipi_dsi_dcs_write(panel->link1,
			MIPI_DCS_RSP_WRITE_ADAPTIVE_BRIGHTNESS_CONTROL, &data,
			1);
	if (ret < 1) {
		DRM_INFO("failed to set adaptive brightness ctrl: %d\n", ret);
		return ret;
	}

	ret = mipi_dsi_dcs_write(panel->link2,
			MIPI_DCS_RSP_WRITE_ADAPTIVE_BRIGHTNESS_CONTROL, &data,
			1);
	if (ret < 1) {
		DRM_INFO("failed to set adaptive brightness ctrl: %d\n", ret);
		return ret;
	}

	return 0;
}

static int panel_sharp_write_display_brightness(struct sharp_panel *panel)
{
	int ret;
	u8 data;

	data = TO_DSI(0xFF);

	ret = mipi_dsi_dcs_write(panel->link1,
			MIPI_DCS_RSP_WRITE_DISPLAY_BRIGHTNESS, &data, 1);
	if (ret < 1) {
		DRM_INFO("failed to write display brightness: %d\n", ret);
		return ret;
	}

	ret = mipi_dsi_dcs_write(panel->link2,
			MIPI_DCS_RSP_WRITE_DISPLAY_BRIGHTNESS, &data, 1);
	if (ret < 1) {
		DRM_INFO("failed to write display brightness: %d\n", ret);
		return ret;
	}

	return 0;
}

static int panel_sharp_write_control_display(struct sharp_panel *panel)
{
	int ret;
	u8 data;

	data = TO_DSI(0x01);

	ret = mipi_dsi_dcs_write(panel->link1, MIPI_DCS_RSP_WRITE_CONTROL_DISPLAY,
			&data, 1);
	if (ret < 1) {
		DRM_INFO("failed to write control display: %d\n", ret);
		return ret;
	}

	ret = mipi_dsi_dcs_write(panel->link2, MIPI_DCS_RSP_WRITE_CONTROL_DISPLAY,
			&data, 1);
	if (ret < 1) {
		DRM_INFO("failed to write control display: %d\n", ret);
		return ret;
	}

	return 0;
}

static int sharp_panel_prepare(struct drm_panel *panel)
{
	struct sharp_panel *sharp = to_sharp_panel(panel);
	u8 format = MIPI_DCS_PIXEL_FMT_24BIT;
	int err;

	if (sharp->prepared)
		return 0;

	err = regulator_enable(sharp->dvdd_lcd_1v8);
	if (err < 0) {
		DRM_INFO("failed to enable dvdd_lcd_1v8: %d\n", err);
		return err;
	}
	msleep(12);
	err = regulator_enable(sharp->avdd_lcd_vsp_5v5);
	if (err < 0) {
		DRM_INFO("failed to enable avdd_lcd_vsp_5v5: %d\n", err);
		return err;
	}
	msleep(12);
	err = regulator_enable(sharp->avdd_lcd_vsn_5v5);
	if (err < 0) {
		DRM_INFO("failed to enable avdd_lcd_vsn_5v5: %d\n", err);
		return err;
	}
	msleep(70);
	
	if (gpio_get_value(sharp->reset_gpio) == 0) {
		pr_info("panel: %s\n", __func__);
		gpio_direction_output(sharp->reset_gpio, 1);
		usleep_range(1000, 3000);
		gpio_set_value(sharp->reset_gpio, 0);
		usleep_range(1000, 3000);
		gpio_set_value(sharp->reset_gpio, 1);
		msleep(32);
	}
	
	err = sharp_setup_symmetrical_split(sharp->link2, sharp->link1, sharp->mode);
	if (err < 0) {
		dev_err(panel->dev, "failed to set up symmetrical split: %d\n",
			err);
		goto poweroff;
	}

	// Exit from SLEEP MODE
	mipi_dsi_dcs_exit_sleep_mode(sharp->link1);
	mipi_dsi_dcs_exit_sleep_mode(sharp->link2);
	sharp_wait_frames(sharp, 6);
	
	/* Set pixel format */
	err = mipi_dsi_dcs_set_pixel_format(sharp->link1, format);
	if (err < 0) {
		dev_err(panel->dev, "failed to set pixel format: %d\n", err);
		goto poweroff;
	}

	err = mipi_dsi_dcs_set_pixel_format(sharp->link2, format);
	if (err < 0) {
		dev_err(panel->dev, "failed to set pixel format: %d\n", err);
		goto poweroff;
	}

	/* Set brightness */
	err = panel_sharp_write_display_brightness(sharp);
	if (err < 0) {
		dev_err(panel->dev, "failed to write display brightness: %d\n", err);
		goto poweroff;
	}
	msleep(20);
	/* Set adaptive brightness */
	err = panel_sharp_write_adaptive_brightness_control(sharp);
	if (err < 0) {
		dev_err(panel->dev, "failed to set adaptive brightness ctrl: %d\n", err);
		goto poweroff;
	}
	msleep(20);
	/* Enable Brightness */
	err = panel_sharp_write_control_display(sharp);
	if (err < 0) {
		dev_err(panel->dev, "failed to write control display: %d\n", err);
		goto poweroff;
	}
	msleep(20);

	/* Set display on */
	err = mipi_dsi_dcs_set_display_on(sharp->link1);
	if (err < 0) {
		dev_err(panel->dev, "failed to set display on: %d\n", err);
		goto poweroff;
	}
	err = mipi_dsi_dcs_set_display_on(sharp->link2);
	if (err < 0) {
		dev_err(panel->dev, "failed to set display on: %d\n", err);
		goto poweroff;
	}
	msleep(150);

	sharp->prepared = true;
	return 0;

poweroff:
	regulator_disable(sharp->avdd_lcd_vsn_5v5);
	regulator_disable(sharp->avdd_lcd_vsp_5v5);
	regulator_disable(sharp->dvdd_lcd_1v8);
	dev_err(panel->dev, "ERROR, Poweroff\n");
	return err;
}

static int sharp_panel_enable(struct drm_panel *panel)
{
	struct sharp_panel *sharp = to_sharp_panel(panel);

	if (sharp->enabled)
		return 0;
		
	if (sharp->backlight) {
		sharp->backlight->props.power = FB_BLANK_UNBLANK;
		backlight_update_status(sharp->backlight);
	}

	sharp->enabled = true;
	dev_err(panel->dev, "Enable\n");
	return 0;
}

static const struct drm_display_mode default_mode = {
/*	
 *               Active                 Front           Sync           Back
 *              Region                 Porch                          Porch
 *     <-----------------------><----------------><-------------><-------------->
 *       //////////////////////|
 *      ////////////////////// |
 *     //////////////////////  |..................               ................
 *                                                _______________
 *     <----- [hv]display ----->
 *     <------------- [hv]sync_start ------------>
 *     <--------------------- [hv]sync_end --------------------->
 *     <-------------------------------- [hv]total ----------------------------->*
 */
	.clock = 214825,
	.hdisplay = 1536,
	.hsync_start = 1536 + 136,
	.hsync_end = 1536 + 136 + 28,
	.htotal = 1536 + 136 + 28 + 28,
	.vdisplay = 2048,
	.vsync_start = 2048 + 14,
	.vsync_end = 2048 + 14 + 2,
	.vtotal = 2048 + 14 + 2 + 8,
	.vrefresh = 60,
};

static int sharp_panel_get_modes(struct drm_panel *panel)
{
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(panel->drm, &default_mode);
	if (!mode) {
		dev_err(panel->drm->dev, "failed to add mode %ux%ux@%u\n",
			default_mode.hdisplay, default_mode.vdisplay,
			default_mode.vrefresh);
		return -ENOMEM;
	}

	drm_mode_set_name(mode);

	drm_mode_probed_add(panel->connector, mode);

	panel->connector->display_info.width_mm = 120;
	panel->connector->display_info.height_mm = 160;

	return 1;
}

static const struct drm_panel_funcs sharp_panel_funcs = {
	.disable = sharp_panel_disable,
	.unprepare = sharp_panel_unprepare,
	.prepare = sharp_panel_prepare,
	.enable = sharp_panel_enable,
	.get_modes = sharp_panel_get_modes,
};

static const struct of_device_id sharp_of_match[] = {
	{ .compatible = "sharp,lq079l1sx01", },
	{ }
};
MODULE_DEVICE_TABLE(of, sharp_of_match);

static int sharp_panel_add(struct sharp_panel *sharp)
{
	struct device_node *np;
	int err;
	sharp->mode = &default_mode;
	
	sharp->avdd_lcd_vsp_5v5 = devm_regulator_get(&sharp->link1->dev, "avdd_lcd_vsp_5v5");
	if (IS_ERR_OR_NULL(sharp->avdd_lcd_vsp_5v5)) {
		pr_err("avdd_lcd_vsp_5v5 regulator get failed\n");
		return PTR_ERR(sharp->avdd_lcd_vsp_5v5);
	};

	sharp->avdd_lcd_vsn_5v5 = devm_regulator_get(&sharp->link1->dev, "avdd_lcd_vsn_5v5");
	if (IS_ERR_OR_NULL(sharp->avdd_lcd_vsn_5v5)) {
		pr_err("avdd_lcd_vsn_5v5 regulator get failed\n");
		return PTR_ERR(sharp->avdd_lcd_vsn_5v5);
	};

	sharp->dvdd_lcd_1v8 = devm_regulator_get(&sharp->link1->dev, "dvdd_lcd_1v8");
	if (IS_ERR_OR_NULL(sharp->dvdd_lcd_1v8)) {
		sharp->dvdd_lcd_1v8 = NULL;
	};

	sharp->reset_gpio = of_get_named_gpio(sharp->link1->dev.of_node, "reset-gpio", 0);

	np = of_parse_phandle(sharp->link1->dev.of_node, "backlight", 0);
	if (np) {
		sharp->backlight = of_find_backlight_by_node(np);
		of_node_put(np);

		if (!sharp->backlight)
			return -EPROBE_DEFER;
	}

	drm_panel_init(&sharp->base);
	sharp->base.funcs = &sharp_panel_funcs;
	sharp->base.dev = &sharp->link1->dev;

	err = drm_panel_add(&sharp->base);
	if (err < 0) {
		goto put_backlight;
	};
	
	return 0;

put_backlight:
	if (sharp->backlight)
		put_device(&sharp->backlight->dev);

	return err;
}

static void sharp_panel_del(struct sharp_panel *sharp)
{
	if (sharp->base.dev)
		drm_panel_remove(&sharp->base);

	if (sharp->backlight)
		put_device(&sharp->backlight->dev);

	if (sharp->link2)
		put_device(&sharp->link2->dev);
}

static int sharp_panel_probe(struct mipi_dsi_device *dsi)
{
	struct mipi_dsi_device *secondary = NULL;
	struct sharp_panel *sharp;
	struct device_node *np;
	int err;
	
	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO;

	/* Find DSI-LINK1 */
	np = of_parse_phandle(dsi->dev.of_node, "link2", 0);
	if (np) {
		secondary = of_find_mipi_dsi_device_by_node(np);
		of_node_put(np);

		if (!secondary) {
			return -EPROBE_DEFER;
		}
	}
	/* register a panel for only the DSI-LINK1 interface */
	if (secondary) {
		sharp = devm_kzalloc(&dsi->dev, sizeof(*sharp), GFP_KERNEL);
		if (!sharp) {
			put_device(&secondary->dev);
			return -ENOMEM;
		}

		mipi_dsi_set_drvdata(dsi, sharp);

		sharp->link2 = secondary;
		sharp->link1 = dsi;

		err = sharp_panel_add(sharp);
		if (err < 0) {
			put_device(&secondary->dev);
			return err;
		}
	}
	err = mipi_dsi_attach(dsi);
	if (err < 0) {
		if (secondary)
			sharp_panel_del(sharp);

		return err;
	}
	dev_err(&dsi->dev, "probe succesfull\n");
	return 0;
}

static int sharp_panel_remove(struct mipi_dsi_device *dsi)
{
	struct sharp_panel *sharp = mipi_dsi_get_drvdata(dsi);
	int err;

	/* only detach from host for the DSI-LINK2 interface */
	if (!sharp) {
		mipi_dsi_detach(dsi);
		return 0;
	}

	err = sharp_panel_disable(&sharp->base);
	if (err < 0)
		dev_err(&dsi->dev, "failed to disable panel: %d\n", err);

	err = mipi_dsi_detach(dsi);
	if (err < 0)
		dev_err(&dsi->dev, "failed to detach from DSI host: %d\n", err);

	drm_panel_detach(&sharp->base);
	sharp_panel_del(sharp);

	return 0;
}

static void sharp_panel_shutdown(struct mipi_dsi_device *dsi)
{
	struct sharp_panel *sharp = mipi_dsi_get_drvdata(dsi);

	/* nothing to do for DSI-LINK2 */
	if (!sharp)
		return;

	gpio_set_value(sharp->reset_gpio, 0);

	sharp_panel_disable(&sharp->base);
}

static struct mipi_dsi_driver sharp_panel_driver = {
	.driver = {
		.name = "panel-sharp-lq079l1sx01",
		.of_match_table = sharp_of_match,
	},
	.probe = sharp_panel_probe,
	.remove = sharp_panel_remove,
	.shutdown = sharp_panel_shutdown,
};
module_mipi_dsi_driver(sharp_panel_driver);

MODULE_AUTHOR("Dmitriy Alexandrov <goodmobiledevices@gmail.com>");
MODULE_DESCRIPTION("DRM Driver for sharp LQ079L1SX01");
MODULE_LICENSE("GPL v2");
