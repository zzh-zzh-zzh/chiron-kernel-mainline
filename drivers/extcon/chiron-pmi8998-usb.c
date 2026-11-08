// SPDX-License-Identifier: GPL-2.0-only
/*
 * MIX 2 USB2 cable detection using the PMI8998 SMB2 Type-C state machine.
 * Register layout: Xiaomi downstream smb-reg.h/smb-lib.c.  The extcon
 * interface serves both the legacy Qualcomm glue and its DWC3 child.
 * Only passive USB2 Rd/Open adapters are sourced, at 5 V / 500 mA.
 * USB PD, powered cables, VCONN and alternate modes are not advertised.
 */
#include <linux/delay.h>
#include <linux/extcon-provider.h>
#include <linux/interrupt.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pm_wakeirq.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/workqueue.h>

#define STATUS_1                 0x0b
#define STATUS_2                 0x0c
#define STATUS_4                 0x0e
#define SOURCE_MODE              BIT(7)
#define VBUS_PRESENT             BIT(6)
#define CC_ATTACHED              BIT(0)
#define SOURCE_RP_MASK           GENMASK(7, 5)
#define SINK_RD_OPEN             BIT(3)
#define SINK_MASK                GENMASK(3, 0)
#define TYPE_C_CFG               0x58
#define FACTORY_DETECT           BIT(5)
#define TYPE_C_CFG_2             0x59
#define SOURCE_CURRENT_1P5A      BIT(0)
#define TYPE_C_IRQ_ENABLE        0x67
#define CC_STATE_IRQ             BIT(2)
#define VBUS_EDGE_IRQS           GENMASK(1, 0)
#define TYPE_C_SW_CTRL           0x68
#define ROLE_COMMANDS            GENMASK(2, 0)

struct chiron_usb {
	struct device *dev;
	struct regmap *regmap;
	struct extcon_dev *extcon;
	struct regulator *vbus;
	struct delayed_work work;
	struct mutex lock;
	u32 base;
	unsigned int saved_cfg, saved_cfg2, saved_irq, saved_ctrl;
	bool source_enabled;
	bool faulted;
	bool stopping;
	int typec_irq;
	int fault_irqs[2];
};

static const unsigned int chiron_usb_cables[] = {
	EXTCON_USB, EXTCON_USB_HOST, EXTCON_NONE,
};

static void chiron_usb_stop_source(struct chiron_usb *usb)
{
	int ret;

	extcon_set_state_sync(usb->extcon, EXTCON_USB_HOST, false);
	if (!usb->source_enabled)
		return;
	ret = regulator_disable(usb->vbus);
	if (ret) {
		dev_err(usb->dev, "VBUS disable failed: %d\n", ret);
		return;
	}
	usb->source_enabled = false;
}

static void chiron_usb_update(struct work_struct *work)
{
	struct chiron_usb *usb = container_of(to_delayed_work(work),
										 struct chiron_usb, work);
	unsigned int stat1, stat2, stat4;
	bool host, device;
	int ret;

	mutex_lock(&usb->lock);
	if (usb->stopping)
		goto out;
	ret = regmap_read(usb->regmap, usb->base + STATUS_4, &stat4);
	if (ret)
		goto error;
	ret = regmap_read(usb->regmap, usb->base + STATUS_2, &stat2);
	if (ret)
		goto error;
	ret = regmap_read(usb->regmap, usb->base + STATUS_1, &stat1);
	if (ret)
		goto error;

	if (!(stat4 & CC_ATTACHED))
		usb->faulted = false;
	host = (stat4 & CC_ATTACHED) && (stat4 & SOURCE_MODE) &&
		   ((stat2 & SINK_MASK) == SINK_RD_OPEN) && !usb->faulted;
	device = (stat4 & CC_ATTACHED) && !(stat4 & SOURCE_MODE) &&
			 (stat4 & VBUS_PRESENT) && (stat1 & SOURCE_RP_MASK);

	if (!host) {
		chiron_usb_stop_source(usb);
		extcon_set_state_sync(usb->extcon, EXTCON_USB,
							  device && !usb->source_enabled);
		goto out;
	}
	extcon_set_state_sync(usb->extcon, EXTCON_USB, false);
	if (!usb->source_enabled) {
		/* Never turn on our boost into an independently powered bus. */
		if (stat4 & VBUS_PRESENT) {
			usb->faulted = true;
			dev_warn(usb->dev, "Unexpected VBUS before host boost\n");
			goto out;
		}
		ret = regulator_enable(usb->vbus);
		if (ret) {
			usb->faulted = true;
			goto error;
		}
		usb->source_enabled = true;
	}
	extcon_set_state_sync(usb->extcon, EXTCON_USB_HOST, true);
	goto out;
error:
	chiron_usb_stop_source(usb);
	extcon_set_state_sync(usb->extcon, EXTCON_USB, false);
	dev_err_ratelimited(usb->dev, "USB cable update failed: %d\n", ret);
out:
	mutex_unlock(&usb->lock);
}

static irqreturn_t chiron_usb_irq(int irq, void *data)
{
	struct chiron_usb *usb = data;

	mutex_lock(&usb->lock);
	if (irq != usb->typec_irq && usb->source_enabled) {
		usb->faulted = true;
		chiron_usb_stop_source(usb);
		dev_warn(usb->dev, "OTG fault; reconnect the adapter to retry\n");
	}
	if (!usb->stopping)
		mod_delayed_work(system_freezable_wq, &usb->work, msecs_to_jiffies(10));
	mutex_unlock(&usb->lock);
	return IRQ_HANDLED;
}

static void chiron_usb_cleanup(void *data)
{
	struct chiron_usb *usb = data;

	mutex_lock(&usb->lock);
	usb->stopping = true;
	mutex_unlock(&usb->lock);
	cancel_delayed_work_sync(&usb->work);
	mutex_lock(&usb->lock);
	chiron_usb_stop_source(usb);
	extcon_set_state_sync(usb->extcon, EXTCON_USB, false);
	regmap_update_bits(usb->regmap, usb->base + TYPE_C_IRQ_ENABLE,
					  CC_STATE_IRQ | VBUS_EDGE_IRQS, usb->saved_irq);
	regmap_update_bits(usb->regmap, usb->base + TYPE_C_SW_CTRL,
					  ROLE_COMMANDS, usb->saved_ctrl);
	regmap_update_bits(usb->regmap, usb->base + TYPE_C_CFG,
					  FACTORY_DETECT, usb->saved_cfg);
	regmap_update_bits(usb->regmap, usb->base + TYPE_C_CFG_2,
					  SOURCE_CURRENT_1P5A, usb->saved_cfg2);
	mutex_unlock(&usb->lock);
	dev_pm_clear_wake_irq(usb->dev);
	device_init_wakeup(usb->dev, false);
}

static int chiron_usb_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct chiron_usb *usb;
	static const char * const names[] = { "type-c-change", "otg-fail", "otg-oc" };
	int i, irq, ret;

	usb = devm_kzalloc(dev, sizeof(*usb), GFP_KERNEL);
	if (!usb)
		return -ENOMEM;
	usb->dev = dev;
	usb->regmap = dev_get_regmap(dev->parent, NULL);
	if (!usb->regmap)
		return -ENODEV;
	ret = device_property_read_u32(dev, "reg", &usb->base);
	if (ret)
		return ret;
	usb->vbus = devm_regulator_get(dev, "vbus");
	if (IS_ERR(usb->vbus))
		return dev_err_probe(dev, PTR_ERR(usb->vbus), "VBUS supply unavailable\n");
	usb->extcon = devm_extcon_dev_allocate(dev, chiron_usb_cables);
	if (IS_ERR(usb->extcon))
		return PTR_ERR(usb->extcon);
	ret = devm_extcon_dev_register(dev, usb->extcon);
	if (ret)
		return ret;
	mutex_init(&usb->lock);
	INIT_DELAYED_WORK(&usb->work, chiron_usb_update);

	ret = regmap_read(usb->regmap, usb->base + TYPE_C_CFG, &usb->saved_cfg);
	if (ret)
		return ret;
	ret = regmap_read(usb->regmap, usb->base + TYPE_C_CFG_2, &usb->saved_cfg2);
	if (ret)
		return ret;
	ret = regmap_read(usb->regmap, usb->base + TYPE_C_IRQ_ENABLE, &usb->saved_irq);
	if (ret)
		return ret;
	ret = regmap_read(usb->regmap, usb->base + TYPE_C_SW_CTRL, &usb->saved_ctrl);
	if (ret)
		return ret;
	ret = devm_add_action_or_reset(dev, chiron_usb_cleanup, usb);
	if (ret)
		return ret;
	for (i = 0; i < ARRAY_SIZE(names); i++) {
		irq = platform_get_irq_byname(pdev, names[i]);
		if (irq < 0)
			return irq;
		if (!i)
			usb->typec_irq = irq;
		else
			usb->fault_irqs[i - 1] = irq;
		ret = devm_request_threaded_irq(dev, irq, NULL, chiron_usb_irq,
										IRQF_ONESHOT | IRQF_NO_AUTOEN, names[i], usb);
		if (ret)
			return ret;
	}
	ret = regmap_update_bits(usb->regmap, usb->base + TYPE_C_CFG,
							 FACTORY_DETECT, 0);
	if (ret)
		return ret;
	ret = regmap_update_bits(usb->regmap, usb->base + TYPE_C_CFG_2,
							 SOURCE_CURRENT_1P5A, 0);
	if (ret)
		return ret;
	/* Hardware DRP, preserving all VCONN and charging configuration. */
	ret = regmap_update_bits(usb->regmap, usb->base + TYPE_C_SW_CTRL,
							 ROLE_COMMANDS, 0);
	if (ret)
		return ret;
	ret = regmap_update_bits(usb->regmap, usb->base + TYPE_C_IRQ_ENABLE,
							 CC_STATE_IRQ | VBUS_EDGE_IRQS,
							 CC_STATE_IRQ | VBUS_EDGE_IRQS);
	if (ret)
		return ret;
	platform_set_drvdata(pdev, usb);
	device_init_wakeup(dev, true);
	ret = dev_pm_set_wake_irq(dev, usb->typec_irq);
	if (ret)
		return ret;
	enable_irq(usb->typec_irq);
	enable_irq(usb->fault_irqs[0]);
	enable_irq(usb->fault_irqs[1]);
	mod_delayed_work(system_freezable_wq, &usb->work, 0);
	dev_info(dev, "PMI8998 USB2 cable detection enabled, source limit 500 mA\n");
	return 0;
}

static void chiron_usb_shutdown(struct platform_device *pdev)
{
	chiron_usb_cleanup(platform_get_drvdata(pdev));
}

static const struct of_device_id chiron_usb_match[] = {
	{ .compatible = "xiaomi,chiron-pmi8998-usb" },
	{ }
};
MODULE_DEVICE_TABLE(of, chiron_usb_match);

static struct platform_driver chiron_usb_driver = {
	.probe = chiron_usb_probe,
	.shutdown = chiron_usb_shutdown,
	.driver = {
		.name = "chiron-pmi8998-usb",
		.of_match_table = chiron_usb_match,
	},
};
module_platform_driver(chiron_usb_driver);
MODULE_DESCRIPTION("MIX 2 PMI8998 USB2 role and passive OTG cable detection");
MODULE_LICENSE("GPL");
