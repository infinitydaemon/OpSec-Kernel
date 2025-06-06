/*
 * ASoC Driver for AudioInjector Pi add on soundcard
 *
 *  Created on: 13-May-2016
 *      Author: flatmax@flatmax.org
 *              based on code by  Cliff Cai <Cliff.Cai@analog.com> for the ssm2602 machine blackfin.
 *              with help from Lars-Peter Clausen for simplifying the original code to use the dai_fmt field.
 *		i2s_node code taken from the other sound/soc/bcm machine drivers.
 *
 * Copyright (C) 2016 Flatmax Pty. Ltd.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */

#include <linux/module.h>
#include <linux/types.h>

#include <sound/core.h>
#include <sound/soc.h>
#include <sound/pcm_params.h>
#include <sound/control.h>

#include "../codecs/wm8731.h"

static const unsigned int bcm2835_rates_12000000[] = {
	8000, 16000, 32000, 44100, 48000, 96000, 88200,
};

static struct snd_pcm_hw_constraint_list bcm2835_constraints_12000000 = {
	.list = bcm2835_rates_12000000,
	.count = ARRAY_SIZE(bcm2835_rates_12000000),
};

static int snd_audioinjector_pi_soundcard_startup(struct snd_pcm_substream *substream)
{
	/* Setup constraints, because there is a 12 MHz XTAL on the board */
	snd_pcm_hw_constraint_list(substream->runtime, 0,
				SNDRV_PCM_HW_PARAM_RATE,
				&bcm2835_constraints_12000000);
	return 0;
}

static int snd_audioinjector_pi_soundcard_hw_params(struct snd_pcm_substream *substream,
				       struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);

	switch (params_rate(params)){
		case 8000:
			return snd_soc_dai_set_bclk_ratio(cpu_dai, 1);
		case 16000:
			return snd_soc_dai_set_bclk_ratio(cpu_dai, 750);
		case 32000:
			return snd_soc_dai_set_bclk_ratio(cpu_dai, 375);
		case 44100:
			return snd_soc_dai_set_bclk_ratio(cpu_dai, 272);
		case 48000:
			return snd_soc_dai_set_bclk_ratio(cpu_dai, 250);
		case 88200:
			return snd_soc_dai_set_bclk_ratio(cpu_dai, 136);
		case 96000:
			return snd_soc_dai_set_bclk_ratio(cpu_dai, 125);
		default:
			return snd_soc_dai_set_bclk_ratio(cpu_dai, 125);
	}
}

/* machine stream operations */
static struct snd_soc_ops snd_audioinjector_pi_soundcard_ops = {
	.startup = snd_audioinjector_pi_soundcard_startup,
	.hw_params = snd_audioinjector_pi_soundcard_hw_params,
};

static int audioinjector_pi_soundcard_dai_init(struct snd_soc_pcm_runtime *rtd)
{
	return snd_soc_dai_set_sysclk(snd_soc_rtd_to_codec(rtd, 0), WM8731_SYSCLK_XTAL, 12000000, SND_SOC_CLOCK_IN);
}

SND_SOC_DAILINK_DEFS(audioinjector_pi,
	DAILINK_COMP_ARRAY(COMP_CPU("bcm2708-i2s.0")),
	DAILINK_COMP_ARRAY(COMP_CODEC("wm8731.1-001a", "wm8731-hifi")),
	DAILINK_COMP_ARRAY(COMP_PLATFORM("bcm2835-i2s.0")));

static struct snd_soc_dai_link audioinjector_pi_soundcard_dai[] = {
	{
		.name = "AudioInjector audio",
		.stream_name = "AudioInjector audio",
		.ops = &snd_audioinjector_pi_soundcard_ops,
		.init = audioinjector_pi_soundcard_dai_init,
		.dai_fmt = SND_SOC_DAIFMT_CBM_CFM|SND_SOC_DAIFMT_I2S|SND_SOC_DAIFMT_NB_NF,
		SND_SOC_DAILINK_REG(audioinjector_pi),
	},
};

static const struct snd_soc_dapm_widget wm8731_dapm_widgets[] = {
	SND_SOC_DAPM_HP("Headphone Jack", NULL),
	SND_SOC_DAPM_SPK("Ext Spk", NULL),
	SND_SOC_DAPM_LINE("Line In Jacks", NULL),
	SND_SOC_DAPM_MIC("Microphone", NULL),
};

static const struct snd_soc_dapm_route audioinjector_audio_map[] = {
	/* headphone connected to LHPOUT, RHPOUT */
	{"Headphone Jack", NULL, "LHPOUT"},
	{"Headphone Jack", NULL, "RHPOUT"},

	/* speaker connected to LOUT, ROUT */
	{"Ext Spk", NULL, "ROUT"},
	{"Ext Spk", NULL, "LOUT"},

	/* line inputs */
	{"Line In Jacks", NULL, "Line Input"},

	/* mic is connected to Mic Jack, with WM8731 Mic Bias */
	{"Microphone", NULL, "Mic Bias"},
};

static struct snd_soc_card snd_soc_audioinjector = {
	.name = "audioinjector-pi-soundcard",
	.owner = THIS_MODULE,
	.dai_link = audioinjector_pi_soundcard_dai,
	.num_links = ARRAY_SIZE(audioinjector_pi_soundcard_dai),

	.dapm_widgets = wm8731_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(wm8731_dapm_widgets),
	.dapm_routes = audioinjector_audio_map,
	.num_dapm_routes = ARRAY_SIZE(audioinjector_audio_map),
};

static int audioinjector_pi_soundcard_probe(struct platform_device *pdev)
{
	struct snd_soc_card *card = &snd_soc_audioinjector;
	int ret;
	
	card->dev = &pdev->dev;

	if (pdev->dev.of_node) {
		struct snd_soc_dai_link *dai = &audioinjector_pi_soundcard_dai[0];
		struct device_node *i2s_node = of_parse_phandle(pdev->dev.of_node,
								"i2s-controller", 0);

		if (i2s_node) {
			dai->cpus->dai_name = NULL;
			dai->cpus->of_node = i2s_node;
			dai->platforms->name = NULL;
			dai->platforms->of_node = i2s_node;
		} else
			if (!dai->cpus->of_node) {
				dev_err(&pdev->dev, "Property 'i2s-controller' missing or invalid\n");
				return -EINVAL;
			}
	}

	if ((ret = devm_snd_soc_register_card(&pdev->dev, card)))
		return dev_err_probe(&pdev->dev, ret, "%s\n", __func__);

	dev_info(&pdev->dev, "successfully loaded\n");

	return ret;
}

static const struct of_device_id audioinjector_pi_soundcard_of_match[] = {
	{ .compatible = "ai,audioinjector-pi-soundcard", },
	{},
};
MODULE_DEVICE_TABLE(of, audioinjector_pi_soundcard_of_match);

static struct platform_driver audioinjector_pi_soundcard_driver = {
       .driver         = {
		.name   = "audioinjector-stereo",
		.owner  = THIS_MODULE,
		.of_match_table = audioinjector_pi_soundcard_of_match,
       },
       .probe          = audioinjector_pi_soundcard_probe,
};

module_platform_driver(audioinjector_pi_soundcard_driver);
MODULE_AUTHOR("Matt Flax <flatmax@flatmax.org>");
MODULE_DESCRIPTION("AudioInjector.net Pi Soundcard");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:audioinjector-pi-soundcard");

