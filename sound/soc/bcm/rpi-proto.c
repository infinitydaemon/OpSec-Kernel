/*
 * ASoC driver for PROTO AudioCODEC (with a WM8731)
 * connected to a Raspberry Pi
 *
 * Author:      Florian Meier, <koalo@koalo.de>
 *	      Copyright 2013
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/module.h>
#include <linux/platform_device.h>

#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/soc.h>
#include <sound/jack.h>

#include "../codecs/wm8731.h"

static const unsigned int wm8731_rates_12288000[] = {
	8000, 32000, 48000, 96000,
};

static struct snd_pcm_hw_constraint_list wm8731_constraints_12288000 = {
	.list = wm8731_rates_12288000,
	.count = ARRAY_SIZE(wm8731_rates_12288000),
};

static int snd_rpi_proto_startup(struct snd_pcm_substream *substream)
{
	/* Setup constraints, because there is a 12.288 MHz XTAL on the board */
	snd_pcm_hw_constraint_list(substream->runtime, 0,
				SNDRV_PCM_HW_PARAM_RATE,
				&wm8731_constraints_12288000);
	return 0;
}

static int snd_rpi_proto_hw_params(struct snd_pcm_substream *substream,
				       struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *codec_dai = snd_soc_rtd_to_codec(rtd, 0);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	int sysclk = 12288000; /* This is fixed on this board */

	/* Set proto bclk */
	int ret = snd_soc_dai_set_bclk_ratio(cpu_dai,32*2);
	if (ret < 0){
		dev_err(rtd->card->dev,
				"Failed to set BCLK ratio %d\n", ret);
		return ret;
	}

	/* Set proto sysclk */
	ret = snd_soc_dai_set_sysclk(codec_dai, WM8731_SYSCLK_XTAL,
			sysclk, SND_SOC_CLOCK_IN);
	if (ret < 0) {
		dev_err(rtd->card->dev,
				"Failed to set WM8731 SYSCLK: %d\n", ret);
		return ret;
	}

	return 0;
}

/* machine stream operations */
static struct snd_soc_ops snd_rpi_proto_ops = {
	.startup = snd_rpi_proto_startup,
	.hw_params = snd_rpi_proto_hw_params,
};

SND_SOC_DAILINK_DEFS(rpi_proto,
	DAILINK_COMP_ARRAY(COMP_CPU("bcm2708-i2s.0")),
	DAILINK_COMP_ARRAY(COMP_CODEC("wm8731.1-001a", "wm8731-hifi")),
	DAILINK_COMP_ARRAY(COMP_PLATFORM("bcm2708-i2s.0")));

static struct snd_soc_dai_link snd_rpi_proto_dai[] = {
{
	.name		= "WM8731",
	.stream_name	= "WM8731 HiFi",
	.dai_fmt	= SND_SOC_DAIFMT_I2S
				| SND_SOC_DAIFMT_NB_NF
				| SND_SOC_DAIFMT_CBM_CFM,
	.ops		= &snd_rpi_proto_ops,
	SND_SOC_DAILINK_REG(rpi_proto),
},
};

/* audio machine driver */
static struct snd_soc_card snd_rpi_proto = {
	.name		= "snd_rpi_proto",
	.owner		= THIS_MODULE,
	.dai_link	= snd_rpi_proto_dai,
	.num_links	= ARRAY_SIZE(snd_rpi_proto_dai),
};

static int snd_rpi_proto_probe(struct platform_device *pdev)
{
	int ret = 0;

	snd_rpi_proto.dev = &pdev->dev;

	if (pdev->dev.of_node) {
		struct device_node *i2s_node;
		struct snd_soc_dai_link *dai = &snd_rpi_proto_dai[0];
		i2s_node = of_parse_phandle(pdev->dev.of_node,
				            "i2s-controller", 0);

		if (i2s_node) {
			dai->cpus->dai_name = NULL;
			dai->cpus->of_node = i2s_node;
			dai->platforms->name = NULL;
			dai->platforms->of_node = i2s_node;
		}
	}

	ret = devm_snd_soc_register_card(&pdev->dev, &snd_rpi_proto);
	if (ret && ret != -EPROBE_DEFER)
		dev_err(&pdev->dev,
				"snd_soc_register_card() failed: %d\n", ret);

	return ret;
}

static const struct of_device_id snd_rpi_proto_of_match[] = {
	{ .compatible = "rpi,rpi-proto", },
	{},
};
MODULE_DEVICE_TABLE(of, snd_rpi_proto_of_match);

static struct platform_driver snd_rpi_proto_driver = {
	.driver = {
		.name   = "snd-rpi-proto",
		.owner  = THIS_MODULE,
		.of_match_table = snd_rpi_proto_of_match,
	},
	.probe	  = snd_rpi_proto_probe,
};

module_platform_driver(snd_rpi_proto_driver);

MODULE_AUTHOR("Florian Meier");
MODULE_DESCRIPTION("ASoC Driver for Raspberry Pi connected to PROTO board (WM8731)");
MODULE_LICENSE("GPL");
