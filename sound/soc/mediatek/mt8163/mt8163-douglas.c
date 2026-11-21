// SPDX-License-Identifier: GPL-2.0-only
/*
 * Amazon Fire HD 8 (2017, douglas) MT8163 + MT6323 PMIC ASoC machine driver.
 */

#include <linux/delay.h>
#include <linux/mfd/mt6323/registers.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <sound/control.h>
#include <sound/pcm.h>
#include <sound/soc.h>

#define ABB_AFE_CON0		0x4000
#define ABB_AFE_CON1		0x4002
#define ABB_AFE_CON10		0x4014
#define ABB_AFE_CON11		0x4016
#define AFE_PMIC_NEWIF_CFG0	0x4024

struct douglas_audio {
	struct snd_soc_card card;
	struct regmap *pmic_regmap;
	struct mutex lock;
	bool dac_enabled;
	unsigned int volume;
};

static void douglas_apply_volume(struct douglas_audio *priv)
{
	unsigned int regval;
	unsigned int idx = priv->volume;

	if (idx > 7)
		idx = 7;
	regval = 7 - idx;
	regmap_update_bits(priv->pmic_regmap, MT6323_AUDTOP_CON5,
			   0x7700, (regval << 12) | (regval << 8));
}

static void douglas_pmic_dac_enable(struct douglas_audio *priv, bool enable)
{
	u32 val = 0;

	mutex_lock(&priv->lock);
	if (priv->dac_enabled == enable) {
		mutex_unlock(&priv->lock);
		return;
	}

	if (enable) {
		regmap_update_bits(priv->pmic_regmap, MT6323_TOP_CKPDN0,
				   BIT(0), BIT(0));
		regmap_write(priv->pmic_regmap, MT6323_TOP_CKPDN1_CLR, 0x0100);
		udelay(250);

		/* 48 kHz DL new interface & ABB AFE */
		regmap_write(priv->pmic_regmap, AFE_PMIC_NEWIF_CFG0, 0x8330);
		regmap_update_bits(priv->pmic_regmap, ABB_AFE_CON1,
				   0x000f, 0x000a);
		regmap_read(priv->pmic_regmap, ABB_AFE_CON11, &val);
		regmap_update_bits(priv->pmic_regmap, ABB_AFE_CON11,
				   0x0100, ((val & 1) ? 0 : 1) << 8);
		regmap_update_bits(priv->pmic_regmap, ABB_AFE_CON0,
				   0x0001, 0x0001);

		/* DC compensation + Analog Amp power-on */
		regmap_update_bits(priv->pmic_regmap, ABB_AFE_CON10,
				   0x0001, 0x0001);
		regmap_read(priv->pmic_regmap, ABB_AFE_CON11, &val);
		regmap_update_bits(priv->pmic_regmap, ABB_AFE_CON11,
				   0x0200, ((val & 2) ? 0 : 1) << 9);

		regmap_write(priv->pmic_regmap, MT6323_AUDTOP_CON6, 0xF7F2);
		regmap_update_bits(priv->pmic_regmap, MT6323_AUDTOP_CON0,
				   0xf000, 0x7000);
		regmap_write(priv->pmic_regmap, MT6323_AUDTOP_CON5, 0x0014);
		regmap_write(priv->pmic_regmap, MT6323_AUDTOP_CON4, 0x007C);
		mdelay(10);
		regmap_write(priv->pmic_regmap, MT6323_AUDTOP_CON6, 0xF5BA);
		regmap_write(priv->pmic_regmap, MT6323_AUDTOP_CON5, 0x2214);
		douglas_apply_volume(priv);
	} else {
		regmap_write(priv->pmic_regmap, MT6323_AUDTOP_CON5, 0x0014);
		regmap_write(priv->pmic_regmap, MT6323_AUDTOP_CON6, 0xF7F2);
		regmap_write(priv->pmic_regmap, MT6323_AUDTOP_CON4, 0x0000);
		regmap_update_bits(priv->pmic_regmap, MT6323_AUDTOP_CON0,
				   0x1000, 0x0000);
		regmap_write(priv->pmic_regmap, MT6323_AUDTOP_CON6, 0x37E2);

		regmap_update_bits(priv->pmic_regmap, ABB_AFE_CON0,
				   0x0001, 0x0000);
		regmap_read(priv->pmic_regmap, ABB_AFE_CON11, &val);
		regmap_update_bits(priv->pmic_regmap, ABB_AFE_CON11,
				   0x0100, ((val & 1) ? 0 : 1) << 8);
		regmap_write(priv->pmic_regmap, MT6323_TOP_CKPDN1_SET, 0x0100);
		regmap_update_bits(priv->pmic_regmap, MT6323_TOP_CKPDN0,
				   BIT(0), 0);
	}

	priv->dac_enabled = enable;
	mutex_unlock(&priv->lock);
}

static int douglas_dl1_prepare(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = asoc_substream_to_rtd(substream);
	struct douglas_audio *priv = snd_soc_card_get_drvdata(rtd->card);

	douglas_pmic_dac_enable(priv, true);
	return 0;
}

static void douglas_dl1_shutdown(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = asoc_substream_to_rtd(substream);
	struct douglas_audio *priv = snd_soc_card_get_drvdata(rtd->card);

	douglas_pmic_dac_enable(priv, false);
}

static const struct snd_soc_ops douglas_dl1_ops = {
	.prepare = douglas_dl1_prepare,
	.shutdown = douglas_dl1_shutdown,
};

static int douglas_vol_info(struct snd_kcontrol *kcontrol,
			    struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = 1;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = 7;
	return 0;
}

static int douglas_vol_get(struct snd_kcontrol *kcontrol,
			   struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_card *card = snd_kcontrol_chip(kcontrol);
	struct douglas_audio *priv = snd_soc_card_get_drvdata(card);

	ucontrol->value.integer.value[0] = priv->volume;
	return 0;
}

static int douglas_vol_put(struct snd_kcontrol *kcontrol,
			   struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_card *card = snd_kcontrol_chip(kcontrol);
	struct douglas_audio *priv = snd_soc_card_get_drvdata(card);
	unsigned int val = ucontrol->value.integer.value[0];

	if (val > 7)
		val = 7;
	mutex_lock(&priv->lock);
	priv->volume = val;
	if (priv->dac_enabled)
		douglas_apply_volume(priv);
	mutex_unlock(&priv->lock);
	return 1;
}

static const struct snd_kcontrol_new douglas_controls[] = {
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "Master Playback Volume",
		.info = douglas_vol_info,
		.get = douglas_vol_get,
		.put = douglas_vol_put,
	},
};

SND_SOC_DAILINK_DEFS(douglas_playback,
	DAILINK_COMP_ARRAY(COMP_CPU("mt-soc-dl1dai-driver")),
	DAILINK_COMP_ARRAY(COMP_DUMMY()),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));

static struct snd_soc_dai_link douglas_dai_links[] = {
	{
		.name = "Douglas Playback",
		.stream_name = "Douglas Playback",
		.ops = &douglas_dl1_ops,
		.playback_only = 1,
		.nonatomic = true,
		SND_SOC_DAILINK_REG(douglas_playback),
	},
};

static int douglas_audio_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *afe_np, *pmic_np;
	struct platform_device *pmic_pdev;
	struct douglas_audio *priv;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;
	mutex_init(&priv->lock);
	priv->volume = 5;

	afe_np = of_parse_phandle(dev->of_node, "mediatek,afe", 0);
	if (!afe_np)
		return dev_err_probe(dev, -ENODEV, "missing mediatek,afe\n");

	pmic_np = of_parse_phandle(dev->of_node, "mediatek,pmic", 0);
	if (!pmic_np) {
		of_node_put(afe_np);
		return dev_err_probe(dev, -ENODEV, "missing mediatek,pmic\n");
	}
	pmic_pdev = of_find_device_by_node(pmic_np);
	of_node_put(pmic_np);
	if (!pmic_pdev) {
		of_node_put(afe_np);
		return dev_err_probe(dev, -EPROBE_DEFER, "PMIC not ready\n");
	}
	priv->pmic_regmap = dev_get_regmap(pmic_pdev->dev.parent, NULL);
	put_device(&pmic_pdev->dev);
	if (!priv->pmic_regmap) {
		of_node_put(afe_np);
		return dev_err_probe(dev, -EPROBE_DEFER, "PMIC regmap not ready\n");
	}

	douglas_dai_links[0].cpus->of_node = afe_np;
	douglas_dai_links[0].platforms->of_node = afe_np;

	priv->card.name = "mt8163-douglas";
	priv->card.owner = THIS_MODULE;
	priv->card.dev = dev;
	priv->card.dai_link = douglas_dai_links;
	priv->card.num_links = ARRAY_SIZE(douglas_dai_links);
	priv->card.controls = douglas_controls;
	priv->card.num_controls = ARRAY_SIZE(douglas_controls);
	snd_soc_card_set_drvdata(&priv->card, priv);

	ret = devm_snd_soc_register_card(dev, &priv->card);
	of_node_put(afe_np);
	if (ret)
		return dev_err_probe(dev, ret, "failed to register sound card\n");

	dev_info(dev, "Douglas MT8163 + MT6323 sound card registered\n");
	return 0;
}

static const struct of_device_id douglas_audio_of_match[] = {
	{ .compatible = "amazon,mt8163-douglas-audio" },
	{ }
};
MODULE_DEVICE_TABLE(of, douglas_audio_of_match);

static struct platform_driver douglas_audio_driver = {
	.probe = douglas_audio_probe,
	.driver = {
		.name = "mt8163-douglas-audio",
		.of_match_table = douglas_audio_of_match,
	},
};
module_platform_driver(douglas_audio_driver);

MODULE_DESCRIPTION("Amazon Fire HD 8 (douglas) MT8163 + MT6323 audio driver");
MODULE_LICENSE("GPL");
