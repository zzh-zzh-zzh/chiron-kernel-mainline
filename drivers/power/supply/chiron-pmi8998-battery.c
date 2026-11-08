// SPDX-License-Identifier: GPL-2.0-only
/*
 * Experimental MIX 2 PMI8998 v2 battery observation.
 * Register layout and units follow the Qualcomm FG Gen3 downstream driver.
 * This driver does not initialize the gauge, load a profile, control charging,
 * or claim a calibrated state of charge. Capacity requests use a fresh cached
 * battery ID and the built-in model allowlist. They do not read the resident
 * IMA profile, whose DMA path is broken on this phone, and never control charging.
 */
#include <linux/err.h>
#include <linux/math64.h>
#include <linux/iio/consumer.h>
#include <linux/mutex.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/platform_device.h>
#include <linux/pm_wakeirq.h>
#include <linux/power_supply.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/unaligned.h>
#include <soc/qcom/qcom-spmi-pmic.h>
#include "chiron-soc-models.h"

#define FG_SOC_SUBTYPE		0x005
#define FG_SOC_ENABLE		0x046
#define FG_INFO_SUBTYPE		0x105
#define FG_INFO_TEMP_STATUS	0x106
#define FG_INFO_SYS_BATT		0x107
#define FG_INFO_TEMP		0x150
#define FG_INFO_VOLTAGE		0x1a0

struct chiron_battery {
	struct regmap *regmap;
	u32 base;
	struct device *dev;
	struct iio_channel *id_chan;
	struct mutex capacity_lock;
};

static int chiron_soc_percent(u8 raw)
{
	if (!raw) return 0;
	if (raw == 255) return 100;
	/* Match downstream endpoints: raw 1..254 maps to 1..99. */
	return DIV_ROUND_CLOSEST((raw - 1) * 98, 253) + 1;
}

static int chiron_id_ohms(const u8 *codes, int raw)
{
	u16 code5 = get_unaligned_le16(codes);
	u16 code15 = get_unaligned_le16(codes + 2);
	u16 code150 = get_unaligned_le16(codes + 4);
	int code, microamps;

	if (code5 > 1023 || code15 > 1023 || code150 > 1023)
		return -ENODATA;
	if (code150 <= 820) { code = code150; microamps = 150; }
	else if (code15 <= 820) { code = code15; microamps = 15; }
	else { code = code5; microamps = 5; }
	if (!code || raw != code) return -EAGAIN;
	/* Preserve the mainline RRADC conversion's kOhm rounding. */
	return (code * 2500 / (1024 * microamps)) * 1000;
}

static int chiron_model_valid(int ohms)
{
	int i;

	if (ohms <= 0) return -ENODATA;
	for (i = 0; i < ARRAY_SIZE(chiron_soc_models); i++) {
		const struct chiron_soc_model *model = &chiron_soc_models[i];

		if (abs(ohms - model->id_ohms) * 100 <= model->id_ohms * 15)
			return 0;
	}
	return -ENODATA;
}

static int chiron_soc_available(struct chiron_battery *bat)
{
	unsigned int status;
	int ret;

	ret = regmap_read(bat->regmap, bat->base + FG_INFO_SYS_BATT, &status);
	if (ret) return ret;
	if (status & BIT(2)) return -ENODATA;
	ret = regmap_read(bat->regmap, bat->base + FG_SOC_ENABLE, &status);
	if (ret) return ret;
	if (!(status & BIT(7))) return -ENODATA;
	ret = regmap_read(bat->regmap, bat->base + 0x48, &status);
	if (ret) return ret;
	return status & BIT(0) ? -EAGAIN : 0;
}

static int chiron_read_capacity(struct chiron_battery *bat, int *value)
{
	u8 codes[6], verify[6], soc[2];
	int ret, raw, ohms, i;

	if (mutex_lock_interruptible(&bat->capacity_lock)) return -ERESTARTSYS;
	pm_stay_awake(bat->dev);
	ret = chiron_soc_available(bat);
	if (ret) goto out;
	ret = iio_read_channel_raw(bat->id_chan, &raw);
	if (ret < 0) goto out;
	ret = regmap_bulk_read(bat->regmap, 0x4566, codes, sizeof(codes));
	if (ret) goto out;
	ret = regmap_bulk_read(bat->regmap, 0x4566, verify, sizeof(verify));
	if (ret) goto out;
	if (memcmp(codes, verify, sizeof(codes))) { ret = -EAGAIN; goto out; }
	ohms = chiron_id_ohms(codes, raw);
	if (ohms < 0) { ret = ohms; goto out; }
	ret = chiron_model_valid(ohms);
	if (ret) goto out;
	for (i = 0; i < 5; i++) {
		ret = regmap_bulk_read(bat->regmap, bat->base + 9, soc, sizeof(soc));
		if (ret) goto out;
		if (soc[0] == soc[1]) break;
	}
	if (i == 5) { ret = -EAGAIN; goto out; }
	ret = chiron_soc_available(bat);
	if (!ret) *value = chiron_soc_percent(soc[0]);
out:
	pm_relax(bat->dev);
	mutex_unlock(&bat->capacity_lock);
	return ret;
}

static int chiron_voltage_uv(u16 raw)
{
	return div_u64((u64)raw * 122070, 1000);
}

static int chiron_current_ua(u16 raw)
{
	/* FG Gen3 is positive for discharge; power_supply is the inverse. */
	return -div_s64((s64)(s16)raw * 488281, 1000);
}

static int chiron_temperature_decic(u16 raw)
{
	/* Preserve downstream's whole-Kelvin rounding and Celsius offset. */
	return (DIV_ROUND_CLOSEST(raw & 0x7ff, 4) - 273) * 10;
}

static int chiron_read_vi(struct chiron_battery *bat, bool read_current, int *value)
{
	u8 buf[10];
	int ret, i;

	/* Reject measurements that changed between primary and shadow reads. */
	for (i = 0; i < 5; i++) {
		ret = regmap_bulk_read(bat->regmap, bat->base + FG_INFO_VOLTAGE,
				       buf, sizeof(buf));
		if (ret)
			return ret;
		if (memcmp(buf, buf + 6, 4))
			continue;
		if (read_current)
			*value = chiron_current_ua(get_unaligned_le16(buf + 2));
		else
			*value = chiron_voltage_uv(get_unaligned_le16(buf));
		return 0;
	}
	return -EAGAIN;
}

static int chiron_read_temp(struct chiron_battery *bat, int *value)
{
	u8 first[2], second[2];
	unsigned int status;
	int ret, i;

	ret = regmap_read(bat->regmap, bat->base + FG_INFO_TEMP_STATUS, &status);
	if (ret)
		return ret;
	if (!(status & BIT(0)))
		return -ENODATA;
	for (i = 0; i < 5; i++) {
		ret = regmap_bulk_read(bat->regmap, bat->base + FG_INFO_TEMP, first, 2);
		if (ret)
			return ret;
		ret = regmap_bulk_read(bat->regmap, bat->base + FG_INFO_TEMP, second, 2);
		if (ret)
			return ret;
		if (memcmp(first, second, 2))
			continue;
		*value = chiron_temperature_decic(get_unaligned_le16(first));
		return 0;
	}
	return -EAGAIN;
}

static int chiron_charger_property(enum power_supply_property prop,
				   union power_supply_propval *val)
{
	struct power_supply *charger = power_supply_get_by_name("chiron-charger");
	int ret;

	if (!charger)
		return -ENODATA;
	ret = power_supply_get_property(charger, prop, val);
	power_supply_put(charger);
	return ret;
}

static int chiron_battery_get_property(struct power_supply *psy,
				      enum power_supply_property prop,
				      union power_supply_propval *val)
{
	struct chiron_battery *bat = power_supply_get_drvdata(psy);
	unsigned int status;
	int ret;

	ret = regmap_read(bat->regmap, bat->base + FG_INFO_SYS_BATT, &status);
	if (ret)
		return ret;
	if (prop == POWER_SUPPLY_PROP_PRESENT) {
		val->intval = !(status & BIT(2));
		return 0;
	}
	if (status & BIT(2))
		return -ENODATA;
	ret = regmap_read(bat->regmap, bat->base + FG_SOC_ENABLE, &status);
	if (ret)
		return ret;
	if (!(status & BIT(7)))
		return -ENODATA;

	switch (prop) {
	case POWER_SUPPLY_PROP_STATUS:
	case POWER_SUPPLY_PROP_HEALTH:
		return chiron_charger_property(prop, val);
	case POWER_SUPPLY_PROP_CAPACITY:
		return chiron_read_capacity(bat, &val->intval);
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		return chiron_read_vi(bat, false, &val->intval);
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		return chiron_read_vi(bat, true, &val->intval);
	case POWER_SUPPLY_PROP_TEMP:
		return chiron_read_temp(bat, &val->intval);
	default:
		return -EINVAL;
	}
}

static enum power_supply_property chiron_battery_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_CAPACITY,
};

static const struct power_supply_desc chiron_battery_desc = {
	.name = "chiron-battery",
	.type = POWER_SUPPLY_TYPE_BATTERY,
	.properties = chiron_battery_props,
	.num_properties = ARRAY_SIZE(chiron_battery_props),
	.get_property = chiron_battery_get_property,
};

static int chiron_battery_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct power_supply_config config = {};
	const struct qcom_spmi_pmic *pmic;
	struct power_supply *psy;
	struct chiron_battery *bat;
	unsigned int subtype;
	int ret;

	pmic = qcom_pmic_get(dev);
	if (IS_ERR(pmic))
		return dev_err_probe(dev, PTR_ERR(pmic), "PMIC revision unavailable\n");
	/* v1 has byte-swapped samples; this experimental port targets v2 only. */
	if (pmic->subtype != PMI8998_SUBTYPE || pmic->major != 2)
		return dev_err_probe(dev, -ENODEV, "requires PMI8998 v2\n");
	bat = devm_kzalloc(dev, sizeof(*bat), GFP_KERNEL);
	if (!bat)
		return -ENOMEM;
	bat->dev = dev;
	bat->regmap = dev_get_regmap(dev->parent, NULL);
	if (!bat->regmap)
		return -ENODEV;
	ret = device_property_read_u32(dev, "reg", &bat->base);
	if (ret)
		return ret;
	if (bat->base != 0x4000)
		return -EINVAL;
	ret = regmap_read(bat->regmap, bat->base + FG_SOC_SUBTYPE, &subtype);
	if (ret)
		return ret;
	if (subtype != 0x10)
		return -ENODEV;
	ret = regmap_read(bat->regmap, bat->base + FG_INFO_SUBTYPE, &subtype);
	if (ret)
		return ret;
	if (subtype != 0x11)
		return -ENODEV;
	bat->id_chan = devm_iio_channel_get(dev, "batt-id");
	if (IS_ERR(bat->id_chan))
		return dev_err_probe(dev, PTR_ERR(bat->id_chan), "battery ID unavailable\n");
	mutex_init(&bat->capacity_lock);
	ret = devm_device_init_wakeup(dev);
	if (ret) return ret;
	config.drv_data = bat;
	config.fwnode = dev_fwnode(dev);
	psy = devm_power_supply_register(dev, &chiron_battery_desc, &config);
	if (IS_ERR(psy))
		return dev_err_probe(dev, PTR_ERR(psy), "battery registration failed\n");
	dev_info(dev, "PMI8998 v2 battery monitor with ID-allowlisted SOC estimate; resident profile DMA bypassed; no charger control\n");
	return 0;
}

static const struct of_device_id chiron_battery_match[] = {
	{ .compatible = "xiaomi,chiron-battery-monitor" },
	{ }
};
MODULE_DEVICE_TABLE(of, chiron_battery_match);

static struct platform_driver chiron_battery_driver = {
	.probe = chiron_battery_probe,
	.driver = {
		.name = "chiron-pmi8998-battery",
		.of_match_table = chiron_battery_match,
	},
};
module_platform_driver(chiron_battery_driver);

MODULE_DESCRIPTION("Experimental PMI8998 v2 battery monitor with guarded SOC for MIX 2");
MODULE_LICENSE("GPL");
