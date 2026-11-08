// SPDX-License-Identifier: GPL-2.0-only
/* Xiaomi MIX 2 JDI panel. Timings and DCS packets from stock MIUI boot DTB. */
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/regulator/consumer.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>
#include <drm/drm_probe_helper.h>

struct chiron_cmd {
	u8 len;
	u8 delay_ms;
	u8 data[2];
};
#include "chiron-nt35596s-commands.h"

struct chiron_panel {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	struct gpio_desc *reset;
	struct regulator_bulk_data supplies[3];
};

static int chiron_commands(struct chiron_panel *ctx,
			   const struct chiron_cmd *cmds, size_t count)
{
	size_t i;
	ssize_t ret;

	for (i = 0; i < count; i++) {
		ret = mipi_dsi_dcs_write_buffer(ctx->dsi, cmds[i].data, cmds[i].len);
		if (ret < 0) {
			dev_err(&ctx->dsi->dev, "DSI command %zu (0x%02x) failed: %zd\n",
				i, cmds[i].data[0], ret);
			return ret;
		}
		if (cmds[i].delay_ms)
			msleep(cmds[i].delay_ms);
	}
	return 0;
}

static void chiron_power_off(struct chiron_panel *ctx)
{
	gpiod_set_value_cansleep(ctx->reset, 1);
	msleep(5);
	regulator_disable(ctx->supplies[2].consumer);
	regulator_disable(ctx->supplies[1].consumer);
	msleep(15);
	regulator_disable(ctx->supplies[0].consumer);
}

static int chiron_prepare(struct drm_panel *panel)
{
	struct chiron_panel *ctx = container_of(panel, struct chiron_panel, panel);
	int ret;

	ret = regulator_enable(ctx->supplies[0].consumer);
	if (ret)
		return ret;
	usleep_range(1000, 2000);
	ret = regulator_enable(ctx->supplies[1].consumer);
	if (ret)
		goto disable_io;
	ret = regulator_enable(ctx->supplies[2].consumer);
	if (ret)
		goto disable_pos;

	/* Active-low descriptor: stock physical reset is low 10 ms, high 10 ms. */
	gpiod_set_value_cansleep(ctx->reset, 1);
	msleep(10);
	gpiod_set_value_cansleep(ctx->reset, 0);
	msleep(10);
	ctx->dsi->mode_flags |= MIPI_DSI_MODE_LPM;
	ret = chiron_commands(ctx, chiron_on_cmds, ARRAY_SIZE(chiron_on_cmds));
	if (ret) {
		chiron_power_off(ctx);
		return ret;
	}
	dev_info(&ctx->dsi->dev, "Chiron JDI 1080x2160 initialization complete\n");
	return 0;

disable_pos:
	regulator_disable(ctx->supplies[1].consumer);
disable_io:
	regulator_disable(ctx->supplies[0].consumer);
	return ret;
}

static int chiron_unprepare(struct drm_panel *panel)
{
	struct chiron_panel *ctx = container_of(panel, struct chiron_panel, panel);

	ctx->dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;
	chiron_commands(ctx, chiron_off_cmds, ARRAY_SIZE(chiron_off_cmds));
	chiron_power_off(ctx);
	return 0;
}

static const struct drm_display_mode chiron_mode = {
	.clock = (1080 + 16 + 28 + 40) * (2160 + 7 + 4 + 24) * 60 / 1000,
	.hdisplay = 1080,
	.hsync_start = 1080 + 16,
	.hsync_end = 1080 + 16 + 28,
	.htotal = 1080 + 16 + 28 + 40,
	.vdisplay = 2160,
	.vsync_start = 2160 + 7,
	.vsync_end = 2160 + 7 + 4,
	.vtotal = 2160 + 7 + 4 + 24,
	.width_mm = 68,
	.height_mm = 136,
	.type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED,
};

static int chiron_get_modes(struct drm_panel *panel, struct drm_connector *connector)
{
	return drm_connector_helper_get_modes_fixed(connector, &chiron_mode);
}

static const struct drm_panel_funcs chiron_funcs = {
	.prepare = chiron_prepare,
	.unprepare = chiron_unprepare,
	.get_modes = chiron_get_modes,
};

static int chiron_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct chiron_panel *ctx;
	int ret;

	ctx = devm_drm_panel_alloc(dev, struct chiron_panel, panel, &chiron_funcs,
				 DRM_MODE_CONNECTOR_DSI);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);
	ctx->dsi = dsi;
	ctx->supplies[0].supply = "vddio";
	ctx->supplies[1].supply = "vddpos";
	ctx->supplies[2].supply = "vddneg";
	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(ctx->supplies), ctx->supplies);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to get panel supplies\n");
	ctx->reset = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->reset))
		return dev_err_probe(dev, PTR_ERR(ctx->reset), "Failed to get reset GPIO\n");
	ret = drm_panel_of_backlight(&ctx->panel);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to get WLED backlight\n");
	mipi_dsi_set_drvdata(dsi, ctx);
	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_BURST |
			  MIPI_DSI_MODE_VIDEO_SYNC_PULSE;
	ctx->panel.prepare_prev_first = true;
	drm_panel_add(&ctx->panel);
	ret = mipi_dsi_attach(dsi);
	if (ret)
		drm_panel_remove(&ctx->panel);
	return ret;
}

static void chiron_remove(struct mipi_dsi_device *dsi)
{
	struct chiron_panel *ctx = mipi_dsi_get_drvdata(dsi);

	mipi_dsi_detach(dsi);
	drm_panel_remove(&ctx->panel);
}

static const struct of_device_id chiron_of_match[] = {
	{ .compatible = "xiaomi,chiron-jdi-nt35596s" },
	{ }
};
MODULE_DEVICE_TABLE(of, chiron_of_match);

static struct mipi_dsi_driver chiron_driver = {
	.probe = chiron_probe,
	.remove = chiron_remove,
	.driver = {
		.name = "panel-xiaomi-chiron-nt35596s",
		.of_match_table = chiron_of_match,
	},
};
module_mipi_dsi_driver(chiron_driver);
MODULE_DESCRIPTION("Xiaomi MIX 2 JDI NT35596S panel bring-up");
MODULE_LICENSE("GPL");
