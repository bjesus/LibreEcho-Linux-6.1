// SPDX-License-Identifier: GPL-2.0-only
/*
 * DRM panel driver for Amazon Fire HD 8 (2017, douglas) 800x1280 NT35521 DSI panel.
 */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>

#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>

struct douglas_nt35521 {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	struct regulator *vgp3;
	struct gpio_desc *pwr_gpio;
	struct gpio_desc *reset_gpio;
	bool prepared;
	bool enabled;
};

static inline struct douglas_nt35521 *to_douglas_nt35521(struct drm_panel *panel)
{
	return container_of(panel, struct douglas_nt35521, panel);
}

static int douglas_nt35521_prepare(struct drm_panel *panel)
{
	struct douglas_nt35521 *ctx = to_douglas_nt35521(panel);
	struct mipi_dsi_device *dsi = ctx->dsi;
	int ret;

	if (ctx->prepared)
		return 0;

	if (ctx->vgp3) {
		ret = regulator_enable(ctx->vgp3);
		if (ret < 0)
			return ret;
	}

	gpiod_set_value_cansleep(ctx->pwr_gpio, 1);
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	msleep(20);

	dsi->mode_flags |= MIPI_DSI_MODE_LPM;
	ret = mipi_dsi_dcs_exit_sleep_mode(dsi);
	if (ret < 0) {
		dev_err(&dsi->dev, "Failed to exit sleep mode: %d\n", ret);
		return ret;
	}
	msleep(120);

	ret = mipi_dsi_dcs_set_display_on(dsi);
	if (ret < 0) {
		dev_err(&dsi->dev, "Failed to set display on: %d\n", ret);
		return ret;
	}
	msleep(20);

	ctx->prepared = true;
	return 0;
}

static int douglas_nt35521_unprepare(struct drm_panel *panel)
{
	struct douglas_nt35521 *ctx = to_douglas_nt35521(panel);
	struct mipi_dsi_device *dsi = ctx->dsi;
	int ret;

	if (!ctx->prepared)
		return 0;

	dsi->mode_flags |= MIPI_DSI_MODE_LPM;
	ret = mipi_dsi_dcs_set_display_off(dsi);
	if (ret < 0)
		dev_warn(&dsi->dev, "Failed to set display off: %d\n", ret);
	msleep(20);

	ret = mipi_dsi_dcs_enter_sleep_mode(dsi);
	if (ret < 0)
		dev_warn(&dsi->dev, "Failed to enter sleep mode: %d\n", ret);
	msleep(120);

	ctx->prepared = false;
	return 0;
}

static int douglas_nt35521_enable(struct drm_panel *panel)
{
	struct douglas_nt35521 *ctx = to_douglas_nt35521(panel);

	ctx->enabled = true;
	return 0;
}

static int douglas_nt35521_disable(struct drm_panel *panel)
{
	struct douglas_nt35521 *ctx = to_douglas_nt35521(panel);

	ctx->enabled = false;
	return 0;
}

static const struct drm_display_mode douglas_nt35521_mode = {
	.clock = 69854,
	.hdisplay = 800,
	.hsync_start = 800 + 28,
	.hsync_end = 800 + 28 + 26,
	.htotal = 800 + 28 + 26 + 28,
	.vdisplay = 1280,
	.vsync_start = 1280 + 21,
	.vsync_end = 1280 + 21 + 4,
	.vtotal = 1280 + 21 + 4 + 15,
	.width_mm = 108,
	.height_mm = 172,
};

static int douglas_nt35521_get_modes(struct drm_panel *panel,
				     struct drm_connector *connector)
{
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(connector->dev, &douglas_nt35521_mode);
	if (!mode)
		return -ENOMEM;

	drm_mode_set_name(mode);
	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	connector->display_info.width_mm = mode->width_mm;
	connector->display_info.height_mm = mode->height_mm;
	drm_mode_probed_add(connector, mode);

	return 1;
}

static const struct drm_panel_funcs douglas_nt35521_panel_funcs = {
	.prepare = douglas_nt35521_prepare,
	.unprepare = douglas_nt35521_unprepare,
	.enable = douglas_nt35521_enable,
	.disable = douglas_nt35521_disable,
	.get_modes = douglas_nt35521_get_modes,
};

static int douglas_nt35521_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct douglas_nt35521 *ctx;
	int ret;

	ctx = devm_kzalloc(dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->vgp3 = devm_regulator_get_optional(dev, "vgp3");
	if (IS_ERR(ctx->vgp3)) {
		if (PTR_ERR(ctx->vgp3) == -ENODEV)
			ctx->vgp3 = NULL;
		else
			return dev_err_probe(dev, PTR_ERR(ctx->vgp3),
					     "Failed to get vgp3 regulator\n");
	}

	ctx->pwr_gpio = devm_gpiod_get_optional(dev, "power", GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->pwr_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->pwr_gpio),
				     "Failed to get power-gpios\n");

	/* Keep reset deasserted so LK's panel initialization is preserved */
	ctx->reset_gpio = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(ctx->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->reset_gpio),
				     "Failed to get reset-gpios\n");

	ctx->dsi = dsi;
	mipi_dsi_set_drvdata(dsi, ctx);

	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_NO_EOT_PACKET |
			  MIPI_DSI_CLOCK_NON_CONTINUOUS;

	drm_panel_init(&ctx->panel, dev, &douglas_nt35521_panel_funcs,
		       DRM_MODE_CONNECTOR_DSI);

	ret = drm_panel_of_backlight(&ctx->panel);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to get backlight\n");

	drm_panel_add(&ctx->panel);

	ret = mipi_dsi_attach(dsi);
	if (ret < 0) {
		dev_err(dev, "Failed to attach to DSI host: %d\n", ret);
		drm_panel_remove(&ctx->panel);
		return ret;
	}

	return 0;
}

static void douglas_nt35521_remove(struct mipi_dsi_device *dsi)
{
	struct douglas_nt35521 *ctx = mipi_dsi_get_drvdata(dsi);
	int ret;

	ret = mipi_dsi_detach(dsi);
	if (ret < 0)
		dev_err(&dsi->dev, "Failed to detach from DSI host: %d\n", ret);

	drm_panel_remove(&ctx->panel);
}

static const struct of_device_id douglas_nt35521_of_match[] = {
	{ .compatible = "amazon,douglas-nt35521" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, douglas_nt35521_of_match);

static struct mipi_dsi_driver douglas_nt35521_driver = {
	.probe = douglas_nt35521_probe,
	.remove = douglas_nt35521_remove,
	.driver = {
		.name = "panel-amazon-douglas-nt35521",
		.of_match_table = douglas_nt35521_of_match,
	},
};
module_mipi_dsi_driver(douglas_nt35521_driver);

MODULE_DESCRIPTION("DRM driver for Amazon Fire HD 8 (douglas) NT35521 panel");
MODULE_LICENSE("GPL");
