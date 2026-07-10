// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 qdtft International Limited
 *
 * Based on panel-raspberrypi-touchscreen by Broadcom
 */

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>

#include <linux/gpio/consumer.h>
#include <linux/regulator/consumer.h>

#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>

#include <video/mipi_display.h>

struct qd_panel_desc {
	const struct panel_init_cmd *init;
	const struct drm_display_mode *mode;
	const unsigned long mode_flags;
	unsigned int lanes;
	enum mipi_dsi_pixel_format format;
};

struct qd_panel {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	const struct qd_panel_desc *desc;

	struct regulator *power;
	struct gpio_desc *reset;
	struct gpio_desc *iovcc;
	struct gpio_desc *avdd;

	enum drm_panel_orientation orientation;
};

enum dsi_cmd_type {
	INIT_DCS_CMD,
	DELAY_CMD,
};

struct panel_init_cmd {
	enum dsi_cmd_type type;
	size_t len;
	const char *data;
};

#define _INIT_DCS_CMD(...)                                                    \
	{                                                                     \
		.type = INIT_DCS_CMD, .len = sizeof((char[]){ __VA_ARGS__ }), \
		.data = (char[])                                              \
		{                                                             \
			__VA_ARGS__                                           \
		}                                                             \
	}

#define _INIT_DELAY_CMD(...)                                               \
	{                                                                  \
		.type = DELAY_CMD, .len = sizeof((char[]){ __VA_ARGS__ }), \
		.data = (char[])                                           \
		{                                                          \
			__VA_ARGS__                                        \
		}                                                          \
	}

static const struct panel_init_cmd qd_panel_start_init[] = {
	//	_INIT_DCS_CMD(0xFF, 0x98, 0x81, 0x01),
	//	_INIT_DCS_CMD(0xB7, 0x03),
	//	_INIT_DCS_CMD(0xFF, 0x98, 0x81, 0x00),
		_INIT_DCS_CMD(0x11),
		_INIT_DELAY_CMD(120),
		_INIT_DCS_CMD(0x29),
		_INIT_DELAY_CMD(60),
		{},
};

static inline struct qd_panel *panel_to_qd(struct drm_panel *panel)
{
	return container_of(panel, struct qd_panel, panel);
}

static int qd_panel_init_dcs_cmd(struct qd_panel *ts)
{
	struct mipi_dsi_device *dsi = ts->dsi;
	struct drm_panel *panel = &ts->panel;
	int i, err = 0;

	if (ts->desc->init) {
		const struct panel_init_cmd *init_cmds = ts->desc->init;

		for (i = 0; init_cmds[i].len != 0; i++) {
			const struct panel_init_cmd *cmd = &init_cmds[i];

			switch (cmd->type) {
			case DELAY_CMD:
				msleep(cmd->data[0]);
				err = 0;
				break;

			case INIT_DCS_CMD:
				err = mipi_dsi_dcs_write(
					dsi, cmd->data[0],
					cmd->len <= 1 ? NULL : &cmd->data[1],
					cmd->len - 1);
				break;

			default:
				err = -EINVAL;
			}

			if (err < 0) {
				dev_err(panel->dev,
					"failed to write command %u\n", i);
				return err;
			}
		}
	}
	return 0;
}

static int qd_panel_prepare(struct drm_panel *panel)
{
	struct qd_panel *ctx = panel_to_qd(panel);
	int ret;

	if (ctx->iovcc) {
		gpiod_set_value_cansleep(ctx->iovcc, 1);
		msleep(20);
	}

	if (ctx->avdd) {
		gpiod_set_value_cansleep(ctx->avdd, 1);
		msleep(20);
	}

	/* And reset it */
	if (ctx->reset) {
		gpiod_set_value_cansleep(ctx->reset, 0);
		msleep(60);
		gpiod_set_value_cansleep(ctx->reset, 1);
		msleep(60);
	}

	ret = qd_panel_init_dcs_cmd(ctx);
	if (ret < 0)
		dev_err(panel->dev, "failed to init panel: %d\n", ret);

	return 0;
}

static int qd_panel_unprepare(struct drm_panel *panel)
{
	struct qd_panel *ctx = panel_to_qd(panel);

	mipi_dsi_dcs_set_display_off(ctx->dsi);
	mipi_dsi_dcs_enter_sleep_mode(ctx->dsi);

	if (ctx->reset) {
		gpiod_set_value_cansleep(ctx->reset, 0);
		msleep(20);
	}

	if (ctx->avdd) {
		gpiod_set_value_cansleep(ctx->avdd, 0);
		msleep(20);
	}

	if (ctx->iovcc) {
		gpiod_set_value_cansleep(ctx->iovcc, 0);
		msleep(20);
	}

	return 0;
}


static const struct drm_display_mode qd_panel_720x1280_mode = {
	.clock = 83333,
	.hdisplay = 720,
	.hsync_start = 720 + 120,
	.hsync_end = 720 + 120 + 100,
	.htotal = 720 + 120 + 100 + 100,
	.vdisplay = 1280,
	.vsync_start = 1280 + 10,
	.vsync_end = 1280 + 10 + 10,
	.vtotal = 1280 + 10 + 10 + 10,
	.width_mm = 85,
	.height_mm = 154,
};

static int qd_panel_get_modes(struct drm_panel *panel,
			      struct drm_connector *connector)
{
	struct qd_panel *ctx = panel_to_qd(panel);
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(connector->dev, ctx->desc->mode);
	if (!mode) {
		dev_err(&ctx->dsi->dev, "failed to add mode %ux%ux@%u\n",
			ctx->desc->mode->hdisplay, ctx->desc->mode->vdisplay,
			drm_mode_vrefresh(ctx->desc->mode));
		return -ENOMEM;
	}

	drm_mode_set_name(mode);

	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	drm_mode_probed_add(connector, mode);

	connector->display_info.width_mm = mode->width_mm;
	connector->display_info.height_mm = mode->height_mm;

	drm_connector_set_panel_orientation(connector, ctx->orientation);

	return 1;
}

static enum drm_panel_orientation qd_panel_get_orientation(struct drm_panel *panel)
{
	struct qd_panel *ctx = panel_to_qd(panel);

	return ctx->orientation;
}

static const struct drm_panel_funcs qd_panel_funcs = {
	.prepare = qd_panel_prepare,
	.unprepare = qd_panel_unprepare,
	.get_modes = qd_panel_get_modes,
	.get_orientation = qd_panel_get_orientation,
};

static int qd_panel_dsi_probe(struct mipi_dsi_device *dsi)
{
	struct qd_panel *ctx;
	int ret;

	dev_info(&dsi->dev, "dsi panel: %s\n",
		 (char *)of_get_property(dsi->dev.of_node, "compatible", NULL));

	ctx = devm_kzalloc(&dsi->dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	mipi_dsi_set_drvdata(dsi, ctx);
	ctx->dsi = dsi;
	ctx->desc = of_device_get_match_data(&dsi->dev);

	ctx->panel.prepare_prev_first = true;
	drm_panel_init(&ctx->panel, &dsi->dev, &qd_panel_funcs,
		       DRM_MODE_CONNECTOR_DSI);

	ctx->reset = devm_gpiod_get_optional(&dsi->dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(ctx->reset))
		return dev_err_probe(&dsi->dev, PTR_ERR(ctx->reset),
				     "Couldn't get reset GPIO number\n");

	ctx->iovcc = devm_gpiod_get_optional(&dsi->dev, "iovcc", GPIOD_OUT_LOW);
	if (IS_ERR(ctx->iovcc))
		return dev_err_probe(&dsi->dev, PTR_ERR(ctx->iovcc),
				     "Couldn't get iovcc GPIO number\n");

	ctx->avdd = devm_gpiod_get_optional(&dsi->dev, "avdd", GPIOD_OUT_LOW);
	if (IS_ERR(ctx->avdd))
		return dev_err_probe(&dsi->dev, PTR_ERR(ctx->avdd),
				     "Couldn't get avdd GPIO number\n");

	ret = of_drm_get_panel_orientation(dsi->dev.of_node, &ctx->orientation);
	if (ret) {
		dev_err(&dsi->dev, "%pOF: failed to get orientation: %d\n",
			dsi->dev.of_node, ret);
		return ret;
	}

	ret = drm_panel_of_backlight(&ctx->panel);
	if (ret)
		return ret;

	drm_panel_add(&ctx->panel);

	dsi->mode_flags = ctx->desc->mode_flags;
	dsi->format = ctx->desc->format;
	dsi->lanes = ctx->desc->lanes;
	dev_info(&dsi->dev, "lanes: %d\n", dsi->lanes);

	ret = mipi_dsi_attach(dsi);
	if (ret)
		drm_panel_remove(&ctx->panel);

	return ret;
}

static void qd_panel_dsi_remove(struct mipi_dsi_device *dsi)
{
	struct qd_panel *ctx = mipi_dsi_get_drvdata(dsi);

	mipi_dsi_detach(dsi);
	drm_panel_remove(&ctx->panel);
	if (ctx->reset) {
		gpiod_set_value_cansleep(ctx->reset, 0);
		msleep(20);
	}

	if (ctx->avdd) {
		gpiod_set_value_cansleep(ctx->avdd, 0);
		msleep(20);
	}

	if (ctx->iovcc) {
		gpiod_set_value_cansleep(ctx->iovcc, 0);
		msleep(20);
	}
}

static void qd_panel_dsi_shutdown(struct mipi_dsi_device *dsi)
{
	struct qd_panel *ctx = mipi_dsi_get_drvdata(dsi);

	if (ctx->reset) {
		dev_info(&dsi->dev, "shutdown\n");
		gpiod_set_value_cansleep(ctx->reset, 0);
		msleep(20);
	}

	if (ctx->avdd) {
		gpiod_set_value_cansleep(ctx->avdd, 0);
		msleep(20);
	}

	if (ctx->iovcc) {
		gpiod_set_value_cansleep(ctx->iovcc, 0);
		msleep(20);
	}
}

static const struct qd_panel_desc qd_panel_720x1280_desc = {
	.init = qd_panel_start_init,
	.mode = &qd_panel_720x1280_mode,
	.mode_flags = MIPI_DSI_MODE_VIDEO_HSE | MIPI_DSI_MODE_VIDEO |
		      MIPI_DSI_MODE_LPM | MIPI_DSI_CLOCK_NON_CONTINUOUS,
	.lanes = 2,
	.format = MIPI_DSI_FMT_RGB888,
};


static const struct of_device_id qd_panel_of_match[] = {
	{ .compatible = "qdtft,720x1280-dsi-touch", &qd_panel_720x1280_desc },
	{}
};
MODULE_DEVICE_TABLE(of, qd_panel_of_match);

static struct mipi_dsi_driver qd_panel_dsi_driver = {
	.probe		= qd_panel_dsi_probe,
	.remove		= qd_panel_dsi_remove,
	.shutdown	= qd_panel_dsi_shutdown,
	.driver = {
		.name		= "qdtft-dsi",
		.of_match_table	= qd_panel_of_match,
	},
};
module_mipi_dsi_driver(qd_panel_dsi_driver);

MODULE_AUTHOR("QDTFT Team <support@qdtft.com>");
MODULE_DESCRIPTION("QDTFT DSI panel driver");
MODULE_LICENSE("GPL");
