// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2018, Linaro Limited

#include <linux/io.h>
#include <linux/pinctrl/consumer.h>
#include <sound/pcm_params.h>
#include <dt-bindings/sound/qcom,q6afe.h>
#include "qdsp6/q6afe.h"
#include <linux/module.h>
#include <linux/platform_device.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>
#include <sound/pcm.h>
#include "common.h"

#define SLIM_MAX_TX_PORTS 16
#define SLIM_MAX_RX_PORTS 16
#define WCD9335_DEFAULT_MCLK_RATE	9600000

static int apq8096_be_hw_params_fixup(struct snd_soc_pcm_runtime *rtd,
				      struct snd_pcm_hw_params *params)
{
	struct snd_interval *rate = hw_param_interval(params,
					SNDRV_PCM_HW_PARAM_RATE);
	struct snd_interval *channels = hw_param_interval(params,
					SNDRV_PCM_HW_PARAM_CHANNELS);

	rate->min = rate->max = 48000;
	channels->min = channels->max = 2;

	return 0;
}

static int msm_snd_hw_params(struct snd_pcm_substream *substream,
			     struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_dai *codec_dai = snd_soc_rtd_to_codec(rtd, 0);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	u32 rx_ch[SLIM_MAX_RX_PORTS], tx_ch[SLIM_MAX_TX_PORTS];
	u32 rx_ch_cnt = 0, tx_ch_cnt = 0;
	int ret = 0;

	ret = snd_soc_dai_get_channel_map(codec_dai,
				&tx_ch_cnt, tx_ch, &rx_ch_cnt, rx_ch);
	if (ret != 0 && ret != -ENOTSUPP) {
		pr_err("failed to get codec chan map, err:%d\n", ret);
		goto end;
	} else if (ret == -ENOTSUPP) {
		return 0;
	}

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		ret = snd_soc_dai_set_channel_map(cpu_dai, 0, NULL,
						  rx_ch_cnt, rx_ch);
	else
		ret = snd_soc_dai_set_channel_map(cpu_dai, tx_ch_cnt, tx_ch,
						  0, NULL);
	if (ret != 0 && ret != -ENOTSUPP)
		pr_err("Failed to set cpu chan map, err:%d\n", ret);
	else if (ret == -ENOTSUPP)
		ret = 0;
end:
	return ret;
}

static const struct snd_soc_ops apq8096_ops = {
	.hw_params = msm_snd_hw_params,
};

static int apq8096_init(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_dai *codec_dai = snd_soc_rtd_to_codec(rtd, 0);

	/*
	 * Codec SLIMBUS configuration
	 * RX1, RX2, RX3, RX4, RX5, RX6, RX7, RX8, RX9, RX10, RX11, RX12, RX13
	 * TX1, TX2, TX3, TX4, TX5, TX6, TX7, TX8, TX9, TX10, TX11, TX12, TX13
	 * TX14, TX15, TX16
	 */
	unsigned int rx_ch[SLIM_MAX_RX_PORTS] = {144, 145, 146, 147, 148, 149,
					150, 151, 152, 153, 154, 155, 156};
	unsigned int tx_ch[SLIM_MAX_TX_PORTS] = {128, 129, 130, 131, 132, 133,
					    134, 135, 136, 137, 138, 139,
					    140, 141, 142, 143};

	snd_soc_dai_set_channel_map(codec_dai, ARRAY_SIZE(tx_ch),
					tx_ch, ARRAY_SIZE(rx_ch), rx_ch);

	snd_soc_dai_set_sysclk(codec_dai, 0, WCD9335_DEFAULT_MCLK_RATE,
				SNDRV_PCM_STREAM_PLAYBACK);

	return 0;
}

struct chiron_audio {
	void __iomem *mux;
	struct pinctrl *pins;
	struct pinctrl_state *active, *sleep;
};

static int chiron_mi2s_fixup(struct snd_soc_pcm_runtime *rtd,
			   struct snd_pcm_hw_params *params)
{
	apq8096_be_hw_params_fixup(rtd, params);
	snd_mask_none(hw_param_mask(params, SNDRV_PCM_HW_PARAM_FORMAT));
	snd_mask_set_format(hw_param_mask(params, SNDRV_PCM_HW_PARAM_FORMAT), SNDRV_PCM_FORMAT_S16_LE);
	return 0;
}

static int chiron_mi2s_startup(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_dai *cpu = snd_soc_rtd_to_cpu(rtd, 0);
	struct snd_soc_dai *codec = snd_soc_rtd_to_codec(rtd, 0);
	struct chiron_audio *data = snd_soc_card_get_drvdata(rtd->card);
	int ret;

	ret = pinctrl_select_state(data->pins, data->active);
	if (ret) return ret;
	ret = snd_soc_dai_set_sysclk(cpu, Q6AFE_LPASS_CLK_ID_QUAD_MI2S_IBIT,
				    1536000, SNDRV_PCM_STREAM_PLAYBACK);
	if (ret) goto sleep;
	/* Original MSM8998 QUAT PCM/I2S mux: zero selects I2S. */
	writel(0, data->mux);
	readl(data->mux);
	/* Mainline q6afe interprets this from the CPU DAI's perspective. */
	ret = snd_soc_dai_set_fmt(cpu, SND_SOC_DAIFMT_BP_FP);
	if (ret) goto clock_off;
	ret = snd_soc_dai_set_fmt(codec, SND_SOC_DAIFMT_I2S |
				 SND_SOC_DAIFMT_NB_NF | SND_SOC_DAIFMT_CBC_CFC);
	if (!ret) return 0;
clock_off:
	snd_soc_dai_set_sysclk(cpu, Q6AFE_LPASS_CLK_ID_QUAD_MI2S_IBIT,
			      0, SNDRV_PCM_STREAM_PLAYBACK);
sleep:
	pinctrl_select_state(data->pins, data->sleep);
	return ret;
}

static void chiron_mi2s_shutdown(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_dai *cpu = snd_soc_rtd_to_cpu(rtd, 0);
	struct chiron_audio *data = snd_soc_card_get_drvdata(rtd->card);
	int ret;

	ret = snd_soc_dai_set_sysclk(cpu, Q6AFE_LPASS_CLK_ID_QUAD_MI2S_IBIT,
				    0, SNDRV_PCM_STREAM_PLAYBACK);
	if (ret) dev_err(rtd->card->dev, "QUAT clock shutdown failed: %d\n", ret);
	ret = pinctrl_select_state(data->pins, data->sleep);
	if (ret) dev_err(rtd->card->dev, "QUAT pin sleep failed: %d\n", ret);
}

static int chiron_mi2s_prepare(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_dai *codec = snd_soc_rtd_to_codec(rtd, 0);

	/* q6afe prepare stops an already-running port before restarting it.
	 * Silence/power down the amplifier before that clock interruption,
	 * including repeated prepare after an underrun. ASoC unmutes only
	 * after the CPU DAI has successfully prepared the port again.
	 */
	return snd_soc_dai_digital_mute(codec, 1, substream->stream);
}

static const struct snd_soc_ops chiron_mi2s_ops = {
	.startup = chiron_mi2s_startup, .shutdown = chiron_mi2s_shutdown,
	.prepare = chiron_mi2s_prepare,
};

static void apq8096_add_be_ops(struct snd_soc_card *card)
{
	struct snd_soc_dai_link *link;
	int i;

	for_each_card_prelinks(card, i, link) {
		if (link->no_pcm && link->id == QUATERNARY_MI2S_RX &&
		    snd_soc_card_get_drvdata(card)) {
			link->be_hw_params_fixup = chiron_mi2s_fixup;
			link->ops = &chiron_mi2s_ops;
			link->ignore_suspend = 0;
			continue;
		}
		if (link->no_pcm == 1) {
			link->be_hw_params_fixup = apq8096_be_hw_params_fixup;
			link->init = apq8096_init;
			link->ops = &apq8096_ops;
		}
	}
}

static int apq8096_platform_probe(struct platform_device *pdev)
{
	struct snd_soc_card *card;
	struct device *dev = &pdev->dev;
	int ret;

	card = devm_kzalloc(dev, sizeof(*card), GFP_KERNEL);
	if (!card)
		return -ENOMEM;

	card->driver_name = of_device_is_compatible(dev->of_node,
			"xiaomi,chiron-wcd9335-sndcard") ?
			"chiron-wcd9335" : "apq8096";
	card->dev = dev;
	card->owner = THIS_MODULE;
	dev_set_drvdata(dev, card);
	ret = qcom_snd_parse_of(card);
	if (ret)
		return ret;

	if (of_device_is_compatible(dev->of_node, "xiaomi,chiron-tas2559-sndcard")) {
		struct chiron_audio *data;
		data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
		if (!data) return -ENOMEM;
		data->mux = devm_platform_ioremap_resource(pdev, 0);
		if (IS_ERR(data->mux)) return PTR_ERR(data->mux);
		data->pins = devm_pinctrl_get(dev);
		if (IS_ERR(data->pins)) return PTR_ERR(data->pins);
		data->active = pinctrl_lookup_state(data->pins, "default");
		data->sleep = pinctrl_lookup_state(data->pins, "sleep");
		if (IS_ERR(data->active)) return PTR_ERR(data->active);
		if (IS_ERR(data->sleep)) return PTR_ERR(data->sleep);
		ret = pinctrl_select_state(data->pins, data->sleep);
		if (ret) return ret;
		snd_soc_card_set_drvdata(card, data);
		card->driver_name = "chiron-tas2559";
	}
	apq8096_add_be_ops(card);
	return devm_snd_soc_register_card(dev, card);
}

static const struct of_device_id msm_snd_apq8096_dt_match[] = {
	{.compatible = "qcom,apq8096-sndcard"},
	{.compatible = "xiaomi,chiron-wcd9335-sndcard"},
	{.compatible = "xiaomi,chiron-tas2559-sndcard"},
	{}
};

MODULE_DEVICE_TABLE(of, msm_snd_apq8096_dt_match);

static struct platform_driver msm_snd_apq8096_driver = {
	.probe  = apq8096_platform_probe,
	.driver = {
		.name = "msm-snd-apq8096",
		.of_match_table = msm_snd_apq8096_dt_match,
	},
};
module_platform_driver(msm_snd_apq8096_driver);
MODULE_AUTHOR("Srinivas Kandagatla <srinivas.kandagatla@linaro.org");
MODULE_DESCRIPTION("APQ8096 ASoC Machine Driver");
MODULE_LICENSE("GPL");
