// SPDX-License-Identifier: GPL-2.0-only
/* Modern Chiron integration for TI/Xiaomi's TAS2559 + TAS2560 DSP core. */
#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/soc/qcom/smem.h>
#include <linux/soc/qcom/socinfo.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include "tas2559-core.h"
#include "tas2560.h"

/* Bring-up gate: removed only after mute/CRC/clock tests on the device. */
static bool playback;
module_param(playback, bool, 0600);
MODULE_PARM_DESC(playback, "Allow playback after manual muted bring-up validation");

static int tas_select(struct tas2559_priv *t, int chip, unsigned int reg)
{
	u8 *book = chip ? &t->mnDevBCurrentBook : &t->mnDevACurrentBook;
	u8 *page = chip ? &t->mnDevBCurrentPage : &t->mnDevACurrentPage;
	u8 b = TAS2559_BOOK_ID(reg), p = TAS2559_PAGE_ID(reg);
	int ret;

	if (*book != b) {
		ret = regmap_write(t->maps[chip], 0, 0);
		if (ret)
			return ret;
		*page = 0;
		ret = regmap_write(t->maps[chip], 127, b);
		if (ret)
			return ret;
		*book = b;
	}
	if (*page != p) {
		ret = regmap_write(t->maps[chip], 0, p);
		if (ret)
			return ret;
		*page = p;
	}
	return 0;
}

enum tas_io { TAS_READ, TAS_WRITE, TAS_BULK_READ, TAS_BULK_WRITE, TAS_UPDATE };

static int tas_io(struct tas2559_priv *t, enum channel ch, unsigned int reg,
		 enum tas_io op, unsigned int value, unsigned int mask,
		 void *buf, unsigned int len)
{
	int chip, ret = 0;
	unsigned int offset = TAS2559_PAGE_REG(reg);

	if (ch < DevA || ch > DevBoth || reg > TAS2559_REG(255, 255, 127) ||
	    len > 128 - offset || ((op == TAS_READ || op == TAS_BULK_READ) && ch == DevBoth))
		return -EINVAL;
	mutex_lock(&t->dev_lock);
	if (t->faulted || t->stopping) {
		ret = -EIO;
		goto out;
	}
	for (chip = 0; chip < 2; chip++) {
		if (!(ch & BIT(chip)))
			continue;
		ret = tas_select(t, chip, reg);
		if (ret)
			break;
		switch (op) {
		case TAS_READ: ret = regmap_read(t->maps[chip], offset, buf); break;
		case TAS_WRITE: ret = regmap_write(t->maps[chip], offset, value); break;
		case TAS_BULK_READ: ret = regmap_bulk_read(t->maps[chip], offset, buf, len); break;
		case TAS_BULK_WRITE: ret = regmap_bulk_write(t->maps[chip], offset, buf, len); break;
		case TAS_UPDATE: ret = regmap_update_bits(t->maps[chip], offset, mask, value); break;
		}
		if (ret) {
			t->mnErrCode |= chip ? ERROR_DEVB_I2C_COMM : ERROR_DEVA_I2C_COMM;
			break;
		}
		if (op != TAS_READ && op != TAS_BULK_READ &&
		    (offset == 0 || offset == 127 || reg == TAS2559_SW_RESET_REG)) {
			/* Explicit page/book writes and reset invalidate selection. */
			if (chip) { t->mnDevBCurrentBook = 255; t->mnDevBCurrentPage = 255; }
			else { t->mnDevACurrentBook = 255; t->mnDevACurrentPage = 255; }
		}
	}
out:
	mutex_unlock(&t->dev_lock);
	return ret;
}

static int tas2559_dev_read(struct tas2559_priv *t, enum channel c, unsigned int r, unsigned int *v)
{ return tas_io(t, c, r, TAS_READ, 0, 0, v, 1); }
static int tas_write(struct tas2559_priv *t, enum channel c, unsigned int r, unsigned int v)
{ return tas_io(t, c, r, TAS_WRITE, v, 0, NULL, 1); }
static int tas_bulk_read(struct tas2559_priv *t, enum channel c, unsigned int r, unsigned char *b, unsigned int n)
{ return tas_io(t, c, r, TAS_BULK_READ, 0, 0, b, n); }
static int tas_bulk_write(struct tas2559_priv *t, enum channel c, unsigned int r, unsigned char *b, unsigned int n)
{ return tas_io(t, c, r, TAS_BULK_WRITE, 0, 0, b, n); }
static int tas_update(struct tas2559_priv *t, enum channel c, unsigned int r, unsigned int m, unsigned int v)
{ return tas_io(t, c, r, TAS_UPDATE, v, m, NULL, 1); }

static void tas2559_enableIRQ(struct tas2559_priv *t, enum channel ch, bool on)
{
	on = on && !t->stopping && !t->faulted;
	if (on == t->mbIRQEnable)
		return;
	t->mbIRQEnable = on;
	if (on) {
		enable_irq(t->mnDevAIRQ);
		enable_irq(t->mnDevBIRQ);
	} else {
		disable_irq_nosync(t->mnDevAIRQ);
		disable_irq_nosync(t->mnDevBIRQ);
	}
}

static void tas_reset(struct tas2559_priv *t)
{
	gpiod_set_value_cansleep(t->reset, 1);
	msleep(5);
	gpiod_set_value_cansleep(t->reset, 0);
	msleep(2);
	t->mnDevACurrentBook = t->mnDevACurrentPage = 255;
	t->mnDevBCurrentBook = t->mnDevBCurrentPage = 255;
}

void tas2559_force_off(struct tas2559_priv *t)
{
	tas2559_enableIRQ(t, DevBoth, false);
	t->faulted = true;
	t->mbPowerUp = false;
	hrtimer_cancel(&t->mtimer);
	gpiod_set_value_cansleep(t->reset, 1);
	dev_err(t->dev, "amplifiers held in reset after fault %#x\n", t->mnErrCode);
}

static void tas_clear_irq(struct tas2559_priv *t)
{
	unsigned int v;
	int ret = tas2559_dev_read(t, DevA, TAS2559_FLAGS_1, &v);
	if (!ret) ret = tas2559_dev_read(t, DevA, TAS2559_FLAGS_2, &v);
	if (!ret) ret = tas2559_dev_read(t, DevB, TAS2560_FLAGS_1, &v);
	if (!ret) ret = tas2559_dev_read(t, DevB, TAS2560_FLAGS_2, &v);
	if (ret) tas2559_force_off(t);
}

#include "tas2559-monitor.h"

static irqreturn_t tas_irq_thread(int irq, void *data)
{
	struct tas2559_priv *t = data;
	mutex_lock(&t->codec_lock);
	tas2559_enableIRQ(t, DevBoth, false);
	if (!t->stopping && !t->faulted)
		mod_delayed_work(system_wq, &t->irq_work, msecs_to_jiffies(10));
	mutex_unlock(&t->codec_lock);
	return IRQ_HANDLED;
}

static int tas_mute(struct snd_soc_dai *dai, int mute, int stream)
{
	struct tas2559_priv *t = snd_soc_component_get_drvdata(dai->component);
	int ret;

	mutex_lock(&t->codec_lock);
	if (!mute && !READ_ONCE(playback)) {
		ret = -EACCES;
		goto out;
	}
	if (t->stopping || t->faulted || !t->initialized) {
		ret = mute ? 0 : -EIO;
		goto out;
	}
	ret = tas2559_enable(t, !mute);
	if (ret || t->faulted) {
		tas2559_force_off(t);
		ret = ret ?: -EIO;
	}
out:
	mutex_unlock(&t->codec_lock);
	return ret;
}

static int tas_hw_params(struct snd_pcm_substream *s, struct snd_pcm_hw_params *p,
			 struct snd_soc_dai *dai)
{
	if (params_rate(p) != 48000 || params_channels(p) != 2 || params_width(p) != 16)
		return -EINVAL;
	return 0;
}

static int tas_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
	if ((fmt & SND_SOC_DAIFMT_FORMAT_MASK) != SND_SOC_DAIFMT_I2S ||
	    (fmt & SND_SOC_DAIFMT_CLOCK_PROVIDER_MASK) != SND_SOC_DAIFMT_CBC_CFC ||
	    (fmt & SND_SOC_DAIFMT_INV_MASK) != SND_SOC_DAIFMT_NB_NF)
		return -EINVAL;
	return 0;
}

static const struct snd_soc_dai_ops tas_dai_ops = {
	.mute_stream = tas_mute, .hw_params = tas_hw_params, .set_fmt = tas_fmt,
	.no_capture_mute = 1,
};
static struct snd_soc_dai_driver tas_dai = {
	.name = "tas2559-asi1",
	.playback = { .stream_name = "ASI1 Playback", .channels_min = 2,
		.channels_max = 2, .rates = SNDRV_PCM_RATE_48000,
		.formats = SNDRV_PCM_FMTBIT_S16_LE },
	.ops = &tas_dai_ops,
};

static const struct snd_soc_dapm_widget tas_widgets[] = {
	SND_SOC_DAPM_OUTPUT("OUT"),
};
static const struct snd_soc_dapm_route tas_routes[] = {
	{ "OUT", NULL, "ASI1 Playback" },
};
static int tas_suspend(struct snd_soc_component *c)
{
	struct tas2559_priv *t = snd_soc_component_get_drvdata(c);
	int ret = 0;

	mutex_lock(&t->codec_lock);
	t->mbRuntimeSuspend = true;
	if (!t->faulted && !t->stopping)
		ret = tas2559_enable(t, false);
	if (ret)
		tas2559_force_off(t);
	tas2559_enableIRQ(t, DevBoth, false);
	mutex_unlock(&t->codec_lock);
	hrtimer_cancel(&t->mtimer);
	cancel_work_sync(&t->mtimerwork);
	cancel_delayed_work_sync(&t->irq_work);
	return ret;
}

static int tas_resume(struct snd_soc_component *c)
{
	struct tas2559_priv *t = snd_soc_component_get_drvdata(c);
	mutex_lock(&t->codec_lock);
	t->mbRuntimeSuspend = false;
	mutex_unlock(&t->codec_lock);
	return 0;
}

static int tas_gain_info(struct snd_kcontrol *k, struct snd_ctl_elem_info *v)
{
	struct tas2559_priv *t = snd_soc_component_get_drvdata(snd_kcontrol_chip(k));

	v->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	v->count = 1;
	v->value.integer.min = 0;
	v->value.integer.max = t->media_gain_max;
	return 0;
}

static int tas_gain_get(struct snd_kcontrol *k, struct snd_ctl_elem_value *v)
{
	struct tas2559_priv *t = snd_soc_component_get_drvdata(snd_kcontrol_chip(k));

	mutex_lock(&t->codec_lock);
	v->value.integer.value[0] = t->gain;
	mutex_unlock(&t->codec_lock);
	return 0;
}

static int tas_gain_put(struct snd_kcontrol *k, struct snd_ctl_elem_value *v)
{
	struct tas2559_priv *t = snd_soc_component_get_drvdata(snd_kcontrol_chip(k));
	long gain = v->value.integer.value[0];
	int ret;

	mutex_lock(&t->codec_lock);
	if (gain < 0 || gain > t->media_gain_max) {
		ret = -EINVAL;
		goto out;
	}
	if (t->stopping || t->faulted || !t->initialized) {
		ret = -EIO;
		goto out;
	}
	/* Change analogue gain only while the amplifiers are powered down. */
	if (t->mbPowerUp) {
		ret = -EBUSY;
		goto out;
	}
	if (gain == t->gain) {
		ret = 0;
		goto out;
	}
	ret = tas2559_set_DAC_gain(t, DevBoth, gain);
	if (ret) {
		tas2559_force_off(t);
		goto out;
	}
	t->gain = gain;
	t->mnDevGain = gain;
	t->mnDevCurrentGain = gain;
	ret = 1;
out:
	mutex_unlock(&t->codec_lock);
	return ret;
}

static const struct snd_kcontrol_new tas_controls[] = {
	{ .iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = "Chiron Media Gain",
	  .info = tas_gain_info, .get = tas_gain_get, .put = tas_gain_put },
};

static const struct snd_soc_component_driver tas_component = {
	.name = "chiron-tas2559", .dapm_widgets = tas_widgets,
	.num_dapm_widgets = ARRAY_SIZE(tas_widgets), .dapm_routes = tas_routes,
	.num_dapm_routes = ARRAY_SIZE(tas_routes), .idle_bias_on = 0,
	.endianness = 1,
	.controls = tas_controls, .num_controls = ARRAY_SIZE(tas_controls),
	.suspend = tas_suspend, .resume = tas_resume,
};

static void tas_quiesce(struct tas2559_priv *t)
{
	mutex_lock(&t->codec_lock);
	t->stopping = true;
	tas2559_enableIRQ(t, DevBoth, false);
	t->mbPowerUp = false;
	gpiod_set_value_cansleep(t->reset, 1);
	mutex_unlock(&t->codec_lock);
	hrtimer_cancel(&t->mtimer);
	cancel_work_sync(&t->mtimerwork);
	cancel_delayed_work_sync(&t->irq_work);
}

static void tas_cleanup(void *data)
{
	struct tas2559_priv *t = data;
	tas_quiesce(t);
	tas2559_free_firmware(t);
}

static const struct regmap_config tas_regmap = {
	.reg_bits = 8, .val_bits = 8, .max_register = 127, .cache_type = REGCACHE_NONE,
};

static int tas_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct tas2559_priv *t;
	struct i2c_client *second;
	const struct firmware *fw;
	struct socinfo *soc;
	size_t size;
	const char *name;
	unsigned int platform, id_a, id_b;
	int i, ret;

	if (client->addr != 0x4c)
		return -EINVAL;
	soc = qcom_smem_get(QCOM_SMEM_HOST_ANY, SMEM_HW_SW_BUILD_ID, &size);
	if (IS_ERR(soc))
		return dev_err_probe(dev, PTR_ERR(soc), "socinfo unavailable\n");
	if (size < offsetofend(struct socinfo, hw_plat))
		return -EINVAL;
	platform = le32_to_cpu(soc->hw_plat);
	if (platform == 32) name = "qcom/chiron/tas2559_uCDSP.bin";
	else if (platform == 33) name = "qcom/chiron/tas2559_s_uCDSP.bin";
	else return dev_err_probe(dev, -ENODEV, "unsupported platform %u\n", platform);
	t = devm_kzalloc(dev, sizeof(*t), GFP_KERNEL);
	if (!t) return -ENOMEM;
	t->dev = dev;
	t->gain = 0; /* Minimum analogue gain for first hardware validation. */
	t->mnBitRate = 16;
	t->mbYCRCEnable = true;
	t->mbCalibrationLoaded = true;
	t->mnDevAAddr = 0x4c; t->mnDevBAddr = 0x4d;
	t->mnDevAChl = 0; t->mnDevBChl = 1;
	mutex_init(&t->dev_lock); mutex_init(&t->codec_lock);
	INIT_DELAYED_WORK(&t->irq_work, irq_work_routine);
	INIT_WORK(&t->mtimerwork, timer_work_routine);
	hrtimer_setup(&t->mtimer, temperature_timer_func, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	i2c_set_clientdata(client, t);
	t->mpFirmware = devm_kzalloc(dev, sizeof(*t->mpFirmware), GFP_KERNEL);
	t->mpCalFirmware = devm_kzalloc(dev, sizeof(*t->mpCalFirmware), GFP_KERNEL);
	if (!t->mpFirmware || !t->mpCalFirmware) return -ENOMEM;
	t->reset = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(t->reset)) return dev_err_probe(dev, PTR_ERR(t->reset), "reset\n");
	t->maps[0] = devm_regmap_init_i2c(client, &tas_regmap);
	if (IS_ERR(t->maps[0])) return PTR_ERR(t->maps[0]);
	second = devm_i2c_new_dummy_device(dev, client->adapter, 0x4d);
	if (IS_ERR(second)) return PTR_ERR(second);
	t->maps[1] = devm_regmap_init_i2c(second, &tas_regmap);
	if (IS_ERR(t->maps[1])) return PTR_ERR(t->maps[1]);
	t->read = tas2559_dev_read; t->write = tas_write;
	t->bulk_read = tas_bulk_read; t->bulk_write = tas_bulk_write;
	t->update_bits = tas_update; t->hw_reset = tas_reset;
	t->enableIRQ = tas2559_enableIRQ; t->clearIRQ = tas_clear_irq;
	for (i = 0; i < 2; i++) {
		int irq;
		t->irq_gpio[i] = devm_gpiod_get_index(dev, "irq", i, GPIOD_IN);
		if (IS_ERR(t->irq_gpio[i])) return PTR_ERR(t->irq_gpio[i]);
		irq = gpiod_to_irq(t->irq_gpio[i]);
		if (irq < 0) return irq;
		if (i) t->mnDevBIRQ = irq; else t->mnDevAIRQ = irq;
		ret = devm_request_threaded_irq(dev, irq, NULL, tas_irq_thread,
			IRQF_ONESHOT | IRQF_TRIGGER_HIGH | IRQF_NO_AUTOEN, dev_name(dev), t);
		if (ret) return ret;
	}
	/* Registered after IRQ/resources so cleanup runs before devm releases them. */
	ret = devm_add_action_or_reset(dev, tas_cleanup, t);
	if (ret) return ret;
	ret = request_firmware(&fw, name, dev);
	if (ret) return ret;
	ret = tas2559_parse_firmware(t, fw);
	release_firmware(fw);
	if (ret) return dev_err_probe(dev, ret, "invalid original firmware\n");
	if (t->mpFirmware->mnConfigurations <= 4 ||
	    t->mpFirmware->mpConfigurations[4].mnProgram != 0 ||
	    t->mpFirmware->mpConfigurations[4].mnDevices != DevBoth)
		return -EINVAL;
	tas_reset(t);
	ret = tas_write(t, DevBoth, TAS2559_SW_RESET_REG, 1);
	if (ret) return ret;
	msleep(1);
	ret = tas2559_dev_read(t, DevA, TAS2559_REV_PGID_REG, &id_a);
	if (!ret) ret = tas2559_dev_read(t, DevB, TAS2560_ID_REG, &id_b);
	if (ret) return ret;
	/* Stock logs TAS2560 PGID without treating revision zero as absent.
	 * I2C transfer errors and the original DSP CRC checks remain fatal.
	 */
	if (!id_a || id_a == 255 || id_b == 255)
		return dev_err_probe(dev, -ENODEV, "invalid chip IDs %02x/%02x\n", id_a, id_b);
	t->mnDevAPGID = id_a; t->mnDevBPGID = id_b;
	ret = tas2559_set_program(t, 0, 4);
	if (!ret) {
		unsigned char stock_b;

		/* Stock media mixer sets DevA to15; DevB retains firmware gain.
		 * A shared control must not exceed either original setting.
		 */
		ret = tas2559_get_DAC_gain(t, DevB, &stock_b);
		if (!ret) {
			t->media_gain_max = min_t(unsigned int, 15, stock_b);
			dev_info(dev, "stock media shared gain limit %u\n", t->media_gain_max);
		}
	}
	if (!ret) ret = tas2559_set_DAC_gain(t, DevBoth, t->gain);
	if (!ret) ret = tas2559_DevMute(t, DevBoth, true);
	if (ret || t->faulted) return dev_err_probe(dev, ret ?: -EIO, "DSP initialization\n");
	t->initialized = true;
	dev_info(dev, "platform %u, TAS2559=%02x TAS2560=%02x, stock program0/config4 loaded muted\n",
		platform, id_a, id_b);
	return devm_snd_soc_register_component(dev, &tas_component, &tas_dai, 1);
}

static void tas_shutdown(struct i2c_client *client)
{
	tas_quiesce(i2c_get_clientdata(client));
}

static const struct of_device_id tas_of_match[] = {
	{ .compatible = "xiaomi,chiron-tas2559" }, { }
};
MODULE_DEVICE_TABLE(of, tas_of_match);
static struct i2c_driver tas_driver = {
	.driver = { .name = "chiron-tas2559", .of_match_table = tas_of_match },
	.probe = tas_probe, .shutdown = tas_shutdown,
};
module_i2c_driver(tas_driver);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Chiron original TAS2559/TAS2560 amplifier integration");
