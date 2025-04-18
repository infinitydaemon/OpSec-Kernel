/*
 * ASoC Driver for RRA DigiDAC1
 * Copyright 2016
 * Author: José M. Tasende <vintage@redrocksaudio.es>
 * based on the HifiBerry DAC driver by Florian Meier <florian.meier@koalo.de>
 * and the Wolfson card driver by Nikesh Oswal, <Nikesh.Oswal@wolfsonmicro.com>
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
#include <linux/platform_device.h>
#include <linux/i2c.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/jack.h>
#include <sound/soc-dapm.h>
#include <sound/tlv.h>
#include <linux/regulator/consumer.h>

#include "../codecs/wm8804.h"
#include "../codecs/wm8741.h"

#define WM8741_NUM_SUPPLIES 2

/* codec private data */
struct wm8741_priv {
	struct wm8741_platform_data pdata;
	struct regmap *regmap;
	struct regulator_bulk_data supplies[WM8741_NUM_SUPPLIES];
	unsigned int sysclk;
	const struct snd_pcm_hw_constraint_list *sysclk_constraints;
};

static int samplerate = 44100;

/* New Alsa Controls not exposed by original wm8741 codec driver	*/
/* in actual driver the att. adjustment is wrong because		*/
/* this DAC has a coarse attenuation register with 4dB steps		*/
/* and a fine level register with 0.125dB steps				*/
/* each register has 32 steps so combining both we have	1024 steps	*/
/* of 0.125 dB.								*/
/* The original level controls from driver are removed at startup	*/
/* and replaced by the corrected ones.					*/
/* The same wm8741 driver can be used for wm8741 and wm8742 devices	*/

static const DECLARE_TLV_DB_SCALE(dac_tlv_fine, 0, 13, 0);
static const DECLARE_TLV_DB_SCALE(dac_tlv_coarse, -12700, 400, 1);
static const char *w8741_dither[4] = {"Off", "RPDF", "TPDF", "HPDF"};
static const char *w8741_filter[5] = {
		"Type 1", "Type 2", "Type 3", "Type 4", "Type 5"};
static const char *w8741_switch[2] = {"Off", "On"};
static const struct soc_enum w8741_enum[] = {
SOC_ENUM_SINGLE(WM8741_MODE_CONTROL_2, 0, 4, w8741_dither),/* dithering type */
SOC_ENUM_SINGLE(WM8741_FILTER_CONTROL, 0, 5, w8741_filter),/* filter type */
SOC_ENUM_SINGLE(WM8741_FORMAT_CONTROL, 6, 2, w8741_switch),/* phase invert */
SOC_ENUM_SINGLE(WM8741_VOLUME_CONTROL, 0, 2, w8741_switch),/* volume ramp */
SOC_ENUM_SINGLE(WM8741_VOLUME_CONTROL, 3, 2, w8741_switch),/* soft mute */
};

static const struct snd_kcontrol_new w8741_snd_controls_stereo[] = {
SOC_DOUBLE_R_TLV("DAC Fine Playback Volume", WM8741_DACLLSB_ATTENUATION,
		WM8741_DACRLSB_ATTENUATION, 0, 31, 1, dac_tlv_fine),
SOC_DOUBLE_R_TLV("Digital Playback Volume", WM8741_DACLMSB_ATTENUATION,
		WM8741_DACRMSB_ATTENUATION, 0, 31, 1, dac_tlv_coarse),
SOC_ENUM("DAC Dither", w8741_enum[0]),
SOC_ENUM("DAC Digital Filter", w8741_enum[1]),
SOC_ENUM("DAC Phase Invert", w8741_enum[2]),
SOC_ENUM("DAC Volume Ramp", w8741_enum[3]),
SOC_ENUM("DAC Soft Mute", w8741_enum[4]),
};

static const struct snd_kcontrol_new w8741_snd_controls_mono_left[] = {
SOC_SINGLE_TLV("DAC Fine Playback Volume", WM8741_DACLLSB_ATTENUATION,
		0, 31, 0, dac_tlv_fine),
SOC_SINGLE_TLV("Digital Playback Volume", WM8741_DACLMSB_ATTENUATION,
		0, 31, 1, dac_tlv_coarse),
SOC_ENUM("DAC Dither", w8741_enum[0]),
SOC_ENUM("DAC Digital Filter", w8741_enum[1]),
SOC_ENUM("DAC Phase Invert", w8741_enum[2]),
SOC_ENUM("DAC Volume Ramp", w8741_enum[3]),
SOC_ENUM("DAC Soft Mute", w8741_enum[4]),
};

static const struct snd_kcontrol_new w8741_snd_controls_mono_right[] = {
SOC_SINGLE_TLV("DAC Fine Playback Volume", WM8741_DACRLSB_ATTENUATION,
	0, 31, 0, dac_tlv_fine),
SOC_SINGLE_TLV("Digital Playback Volume", WM8741_DACRMSB_ATTENUATION,
	0, 31, 1, dac_tlv_coarse),
SOC_ENUM("DAC Dither", w8741_enum[0]),
SOC_ENUM("DAC Digital Filter", w8741_enum[1]),
SOC_ENUM("DAC Phase Invert", w8741_enum[2]),
SOC_ENUM("DAC Volume Ramp", w8741_enum[3]),
SOC_ENUM("DAC Soft Mute", w8741_enum[4]),
};

static int w8741_add_controls(struct snd_soc_component *component)
{
	struct wm8741_priv *wm8741 = snd_soc_component_get_drvdata(component);

	switch (wm8741->pdata.diff_mode) {
	case WM8741_DIFF_MODE_STEREO:
	case WM8741_DIFF_MODE_STEREO_REVERSED:
		snd_soc_add_component_controls(component,
				w8741_snd_controls_stereo,
				ARRAY_SIZE(w8741_snd_controls_stereo));
		break;
	case WM8741_DIFF_MODE_MONO_LEFT:
		snd_soc_add_component_controls(component,
				w8741_snd_controls_mono_left,
				ARRAY_SIZE(w8741_snd_controls_mono_left));
		break;
	case WM8741_DIFF_MODE_MONO_RIGHT:
		snd_soc_add_component_controls(component,
				w8741_snd_controls_mono_right,
				ARRAY_SIZE(w8741_snd_controls_mono_right));
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int digidac1_soundcard_init(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_component *component = snd_soc_rtd_to_codec(rtd, 0)->component;
	struct snd_soc_card *card = rtd->card;
	struct snd_soc_pcm_runtime *wm8741_rtd;
	struct snd_soc_component *wm8741_component;
	struct snd_card *sound_card = card->snd_card;
	struct snd_kcontrol *kctl;
	int ret;

	wm8741_rtd = snd_soc_get_pcm_runtime(card, &card->dai_link[1]);
	if (!wm8741_rtd) {
		dev_warn(card->dev, "digidac1_soundcard_init: couldn't get wm8741 rtd\n");
		return -EFAULT;
	}
	wm8741_component = snd_soc_rtd_to_codec(wm8741_rtd, 0)->component;
	ret = w8741_add_controls(wm8741_component);
	if (ret < 0)
		dev_warn(card->dev, "Failed to add new wm8741 controls: %d\n",
		ret);

	/* enable TX output */
	snd_soc_component_update_bits(component, WM8804_PWRDN, 0x4, 0x0);

	kctl = snd_soc_card_get_kcontrol(card,
		"Playback Volume");
	if (kctl) {
		kctl->vd[0].access = SNDRV_CTL_ELEM_ACCESS_READWRITE;
		snd_ctl_remove(sound_card, kctl);
		}
	kctl = snd_soc_card_get_kcontrol(card,
		"Fine Playback Volume");
	if (kctl) {
		kctl->vd[0].access = SNDRV_CTL_ELEM_ACCESS_READWRITE;
		snd_ctl_remove(sound_card, kctl);
		}
	return 0;
}

static int digidac1_soundcard_startup(struct snd_pcm_substream *substream)
{
	/* turn on wm8804 digital output */
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_component *component = snd_soc_rtd_to_codec(rtd, 0)->component;
	struct snd_soc_card *card = rtd->card;
	struct snd_soc_pcm_runtime *wm8741_rtd;
	struct snd_soc_component *wm8741_component;

	snd_soc_component_update_bits(component, WM8804_PWRDN, 0x3c, 0x00);
	wm8741_rtd = snd_soc_get_pcm_runtime(card, &card->dai_link[1]);
	if (!wm8741_rtd) {
		dev_warn(card->dev, "digidac1_soundcard_startup: couldn't get WM8741 rtd\n");
		return -EFAULT;
	}
	wm8741_component = snd_soc_rtd_to_codec(wm8741_rtd, 0)->component;

	/* latch wm8741 level */
	snd_soc_component_update_bits(wm8741_component, WM8741_DACLLSB_ATTENUATION,
		WM8741_UPDATELL, WM8741_UPDATELL);
	snd_soc_component_update_bits(wm8741_component, WM8741_DACLMSB_ATTENUATION,
		WM8741_UPDATELM, WM8741_UPDATELM);
	snd_soc_component_update_bits(wm8741_component, WM8741_DACRLSB_ATTENUATION,
		WM8741_UPDATERL, WM8741_UPDATERL);
	snd_soc_component_update_bits(wm8741_component, WM8741_DACRMSB_ATTENUATION,
		WM8741_UPDATERM, WM8741_UPDATERM);

	return 0;
}

static void digidac1_soundcard_shutdown(struct snd_pcm_substream *substream)
{
	/* turn off wm8804 digital output */
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_component *component = snd_soc_rtd_to_codec(rtd, 0)->component;

	snd_soc_component_update_bits(component, WM8804_PWRDN, 0x3c, 0x3c);
}

static int digidac1_soundcard_hw_params(struct snd_pcm_substream *substream,
				       struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *codec_dai = snd_soc_rtd_to_codec(rtd, 0);
	struct snd_soc_component *component = snd_soc_rtd_to_codec(rtd, 0)->component;
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	struct snd_soc_card *card = rtd->card;
	struct snd_soc_pcm_runtime *wm8741_rtd;
	struct snd_soc_component *wm8741_component;

	int sysclk = 27000000;
	long mclk_freq = 0;
	int mclk_div = 1;
	int sampling_freq = 1;
	int ret;

	wm8741_rtd = snd_soc_get_pcm_runtime(card, &card->dai_link[1]);
	if (!wm8741_rtd) {
		dev_warn(card->dev, "digidac1_soundcard_hw_params: couldn't get WM8741 rtd\n");
		return -EFAULT;
	}
	wm8741_component = snd_soc_rtd_to_codec(wm8741_rtd, 0)->component;
	samplerate = params_rate(params);

	if (samplerate <= 96000) {
		mclk_freq = samplerate*256;
		mclk_div = WM8804_MCLKDIV_256FS;
	} else {
		mclk_freq = samplerate*128;
		mclk_div = WM8804_MCLKDIV_128FS;
		}

	switch (samplerate) {
	case 32000:
		sampling_freq = 0x03;
		break;
	case 44100:
		sampling_freq = 0x00;
		break;
	case 48000:
		sampling_freq = 0x02;
		break;
	case 88200:
		sampling_freq = 0x08;
		break;
	case 96000:
		sampling_freq = 0x0a;
		break;
	case 176400:
		sampling_freq = 0x0c;
		break;
	case 192000:
		sampling_freq = 0x0e;
		break;
	default:
		dev_err(card->dev,
		"Failed to set WM8804 SYSCLK, unsupported samplerate %d\n",
		samplerate);
	}

	snd_soc_dai_set_clkdiv(codec_dai, WM8804_MCLK_DIV, mclk_div);
	snd_soc_dai_set_pll(codec_dai, 0, 0, sysclk, mclk_freq);

	ret = snd_soc_dai_set_sysclk(codec_dai, WM8804_TX_CLKSRC_PLL,
		sysclk, SND_SOC_CLOCK_OUT);
	if (ret < 0) {
		dev_err(card->dev,
		"Failed to set WM8804 SYSCLK: %d\n", ret);
		return ret;
	}
	/* Enable wm8804 TX output */
	snd_soc_component_update_bits(component, WM8804_PWRDN, 0x4, 0x0);

	/* wm8804 Power on */
	snd_soc_component_update_bits(component, WM8804_PWRDN, 0x9, 0);

	/* wm8804 set sampling frequency status bits */
	snd_soc_component_update_bits(component, WM8804_SPDTX4, 0x0f, sampling_freq);

	/* Now update wm8741 registers for the correct oversampling */
	if (samplerate <= 48000)
		snd_soc_component_update_bits(wm8741_component, WM8741_MODE_CONTROL_1,
		 WM8741_OSR_MASK, 0x00);
	else if (samplerate <= 96000)
		snd_soc_component_update_bits(wm8741_component, WM8741_MODE_CONTROL_1,
		 WM8741_OSR_MASK, 0x20);
	else
		snd_soc_component_update_bits(wm8741_component, WM8741_MODE_CONTROL_1,
		 WM8741_OSR_MASK, 0x40);

	/* wm8741 bit size */
	switch (params_width(params)) {
	case 16:
		snd_soc_component_update_bits(wm8741_component, WM8741_FORMAT_CONTROL,
		 WM8741_IWL_MASK, 0x00);
		break;
	case 20:
		snd_soc_component_update_bits(wm8741_component, WM8741_FORMAT_CONTROL,
		 WM8741_IWL_MASK, 0x01);
		break;
	case 24:
		snd_soc_component_update_bits(wm8741_component, WM8741_FORMAT_CONTROL,
		 WM8741_IWL_MASK, 0x02);
		break;
	case 32:
		snd_soc_component_update_bits(wm8741_component, WM8741_FORMAT_CONTROL,
		 WM8741_IWL_MASK, 0x03);
		break;
	default:
		dev_dbg(card->dev, "wm8741_hw_params:    Unsupported bit size param = %d",
			params_width(params));
		return -EINVAL;
	}

	return snd_soc_dai_set_bclk_ratio(cpu_dai, 64);
}
/* machine stream operations */
static struct snd_soc_ops digidac1_soundcard_ops = {
	.hw_params	= digidac1_soundcard_hw_params,
	.startup	= digidac1_soundcard_startup,
	.shutdown	= digidac1_soundcard_shutdown,
};

SND_SOC_DAILINK_DEFS(digidac1,
	DAILINK_COMP_ARRAY(COMP_CPU("bcm2708-i2s.0")),
	DAILINK_COMP_ARRAY(COMP_CODEC("wm8804.1-003b", "wm8804-spdif")),
	DAILINK_COMP_ARRAY(COMP_PLATFORM("bcm2835-i2s.0")));

SND_SOC_DAILINK_DEFS(digidac11,
	DAILINK_COMP_ARRAY(COMP_CPU("wm8804-spdif")),
	DAILINK_COMP_ARRAY(COMP_CODEC("wm8741.1-001a", "wm8741")),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));

static struct snd_soc_dai_link digidac1_soundcard_dai[] = {
	{
	.name		= "RRA DigiDAC1",
	.stream_name	= "RRA DigiDAC1 HiFi",
	.dai_fmt	= SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF |
				SND_SOC_DAIFMT_CBM_CFM,
	.ops		= &digidac1_soundcard_ops,
	.init		= digidac1_soundcard_init,
	SND_SOC_DAILINK_REG(digidac1),
	},
	{
	.name		= "RRA DigiDAC11",
	.stream_name	= "RRA DigiDAC11 HiFi",
	.dai_fmt	= SND_SOC_DAIFMT_I2S
			| SND_SOC_DAIFMT_NB_NF
			| SND_SOC_DAIFMT_CBS_CFS,
	SND_SOC_DAILINK_REG(digidac11),
	},
};

/* audio machine driver */
static struct snd_soc_card digidac1_soundcard = {
	.name		= "digidac1-soundcard",
	.owner		= THIS_MODULE,
	.dai_link	= digidac1_soundcard_dai,
	.num_links	= ARRAY_SIZE(digidac1_soundcard_dai),
};

static int digidac1_soundcard_probe(struct platform_device *pdev)
{
	int ret = 0;

	digidac1_soundcard.dev = &pdev->dev;

	if (pdev->dev.of_node) {
		struct device_node *i2s_node;
		struct snd_soc_dai_link *dai = &digidac1_soundcard_dai[0];

		i2s_node = of_parse_phandle(pdev->dev.of_node,
					"i2s-controller", 0);

		if (i2s_node) {
			dai->cpus->dai_name = NULL;
			dai->cpus->of_node = i2s_node;
			dai->platforms->name = NULL;
			dai->platforms->of_node = i2s_node;
		}
	}

	ret = devm_snd_soc_register_card(&pdev->dev, &digidac1_soundcard);
	if (ret && ret != -EPROBE_DEFER)
		dev_err(&pdev->dev, "snd_soc_register_card() failed: %d\n",
			ret);

	return ret;
}

static const struct of_device_id digidac1_soundcard_of_match[] = {
	{ .compatible = "rra,digidac1-soundcard", },
	{},
};
MODULE_DEVICE_TABLE(of, digidac1_soundcard_of_match);

static struct platform_driver digidac1_soundcard_driver = {
	.driver = {
			.name		= "digidac1-audio",
			.owner		= THIS_MODULE,
			.of_match_table	= digidac1_soundcard_of_match,
	},
	.probe		= digidac1_soundcard_probe,
};

module_platform_driver(digidac1_soundcard_driver);

MODULE_AUTHOR("José M. Tasende <vintage@redrocksaudio.es>");
MODULE_DESCRIPTION("ASoC Driver for RRA DigiDAC1");
MODULE_LICENSE("GPL v2");
