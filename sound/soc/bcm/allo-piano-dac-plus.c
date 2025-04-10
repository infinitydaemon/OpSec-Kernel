/*
 * ALSA ASoC Machine Driver for Allo Piano DAC Plus Subwoofer
 *
 * Author:	Baswaraj K <jaikumar@cem-solutions.net>
 *		Copyright 2020
 *		based on code by David Knell <david.knell@gmail.com)
 *		based on code by Daniel Matuschek <info@crazy-audio.com>
 *		based on code by Florian Meier <florian.meier@koalo.de>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/gpio/consumer.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <linux/firmware.h>
#include <linux/delay.h>
#include <sound/tlv.h>
#include "../codecs/pcm512x.h"

#define P_DAC_LEFT_MUTE		0x10
#define P_DAC_RIGHT_MUTE	0x01
#define P_DAC_MUTE		0x11
#define P_DAC_UNMUTE		0x00
#define P_MUTE			1
#define P_UNMUTE		0

struct dsp_code {
	char i2c_addr;
	char offset;
	char val;
};

struct glb_pool {
	struct mutex lock;
	unsigned int dual_mode;
	unsigned int set_lowpass;
	unsigned int set_mode;
	unsigned int set_rate;
	unsigned int dsp_page_number;
};

static bool digital_gain_0db_limit = true;
bool glb_mclk;

static struct gpio_desc *mute_gpio[2];

static const char * const allo_piano_mode_texts[] = {
	"None",
	"2.0",
	"2.1",
	"2.2",
};

static SOC_ENUM_SINGLE_DECL(allo_piano_mode_enum,
		0, 0, allo_piano_mode_texts);

static const char * const allo_piano_dual_mode_texts[] = {
	"None",
	"Dual-Mono",
	"Dual-Stereo",
};

static SOC_ENUM_SINGLE_DECL(allo_piano_dual_mode_enum,
		0, 0, allo_piano_dual_mode_texts);

static const char * const allo_piano_dsp_low_pass_texts[] = {
	"60",
	"70",
	"80",
	"90",
	"100",
	"110",
	"120",
	"130",
	"140",
	"150",
	"160",
	"170",
	"180",
	"190",
	"200",
};

static SOC_ENUM_SINGLE_DECL(allo_piano_enum,
		0, 0, allo_piano_dsp_low_pass_texts);

static int __snd_allo_piano_dsp_program(struct snd_soc_pcm_runtime *rtd,
		unsigned int mode, unsigned int rate, unsigned int lowpass)
{
	const struct firmware *fw;
	struct snd_soc_card *card = rtd->card;
	struct glb_pool *glb_ptr = card->drvdata;
	char firmware_name[60];
	int ret = 0, dac = 0;

	if (rate <= 46000)
		rate = 44100;
	else if (rate <= 68000)
		rate = 48000;
	else if (rate <= 92000)
		rate = 88200;
	else if (rate <= 136000)
		rate = 96000;
	else if (rate <= 184000)
		rate = 176400;
	else
		rate = 192000;

	if (lowpass > 14)
		glb_ptr->set_lowpass = lowpass = 0;

	if (mode > 3)
		glb_ptr->set_mode = mode = 0;

	if (mode > 0)
		glb_ptr->dual_mode = 0;

	/* same configuration loaded */
	if ((rate == glb_ptr->set_rate) && (lowpass == glb_ptr->set_lowpass)
			&& (mode == glb_ptr->set_mode))
		return 0;

	switch (mode) {
	case 0: /* None */
		return 1;

	case 1: /* 2.0 */
		snd_soc_component_write(snd_soc_rtd_to_codec(rtd, 0)->component,
				PCM512x_MUTE, P_DAC_UNMUTE);
		snd_soc_component_write(snd_soc_rtd_to_codec(rtd, 1)->component,
				PCM512x_MUTE, P_DAC_MUTE);
		glb_ptr->set_rate = rate;
		glb_ptr->set_mode = mode;
		glb_ptr->set_lowpass = lowpass;
		return 1;

	default:
		snd_soc_component_write(snd_soc_rtd_to_codec(rtd, 0)->component,
				PCM512x_MUTE, P_DAC_UNMUTE);
		snd_soc_component_write(snd_soc_rtd_to_codec(rtd, 1)->component,
				PCM512x_MUTE, P_DAC_UNMUTE);
	}

	for (dac = 0; dac < rtd->dai_link->num_codecs; dac++) {
		struct dsp_code *dsp_code_read;
		int i = 1;

		if (dac == 0) { /* high */
			snprintf(firmware_name, sizeof(firmware_name),
				"allo/piano/2.2/allo-piano-dsp-%d-%d-%d.bin",
				rate, ((lowpass * 10) + 60), dac);
		} else { /* low */
			snprintf(firmware_name, sizeof(firmware_name),
				"allo/piano/2.%d/allo-piano-dsp-%d-%d-%d.bin",
				(mode - 1), rate, ((lowpass * 10) + 60), dac);
		}

		dev_info(rtd->card->dev, "Dsp Firmware File Name: %s\n",
				firmware_name);

		ret = request_firmware(&fw, firmware_name, rtd->card->dev);
		if (ret < 0) {
			dev_err(rtd->card->dev,
				"Error: Allo Piano Firmware %s missing. %d\n",
				firmware_name, ret);
			goto err;
		}

		while (i < (fw->size - 1)) {
			dsp_code_read = (struct dsp_code *)&fw->data[i];

			if (dsp_code_read->offset == 0) {
				glb_ptr->dsp_page_number = dsp_code_read->val;
				ret = snd_soc_component_write(snd_soc_rtd_to_codec(rtd, dac)->component,
						PCM512x_PAGE_BASE(0),
						dsp_code_read->val);

			} else if (dsp_code_read->offset != 0) {
				ret = snd_soc_component_write(snd_soc_rtd_to_codec(rtd, dac)->component,
					(PCM512x_PAGE_BASE(
						glb_ptr->dsp_page_number) +
					dsp_code_read->offset),
					dsp_code_read->val);
			}
			if (ret < 0) {
				dev_err(rtd->card->dev,
					"Failed to write Register: %d\n", ret);
				release_firmware(fw);
				goto err;
			}
			i = i + 3;
		}
		release_firmware(fw);
	}
	glb_ptr->set_rate = rate;
	glb_ptr->set_mode = mode;
	glb_ptr->set_lowpass = lowpass;
	return 1;

err:
	return ret;
}

static int snd_allo_piano_dsp_program(struct snd_soc_pcm_runtime *rtd,
		unsigned int mode, unsigned int rate, unsigned int lowpass)
{
	struct snd_soc_card *card = rtd->card;
	struct glb_pool *glb_ptr = card->drvdata;
	int ret = 0;

	mutex_lock(&glb_ptr->lock);

	ret = __snd_allo_piano_dsp_program(rtd, mode, rate, lowpass);

	mutex_unlock(&glb_ptr->lock);

	return ret;
}

static int snd_allo_piano_dual_mode_get(struct snd_kcontrol *kcontrol,
		struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_card *card = snd_kcontrol_chip(kcontrol);
	struct glb_pool *glb_ptr = card->drvdata;

	ucontrol->value.integer.value[0] = glb_ptr->dual_mode;

	return 0;
}

static int snd_allo_piano_dual_mode_put(struct snd_kcontrol *kcontrol,
		struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_card *card = snd_kcontrol_chip(kcontrol);
	struct glb_pool *glb_ptr = card->drvdata;
	struct snd_soc_pcm_runtime *rtd;
	struct snd_card *snd_card_ptr = card->snd_card;
	struct snd_kcontrol *kctl;
	struct soc_mixer_control *mc;
	unsigned int left_val = 0, right_val = 0;

	rtd = snd_soc_get_pcm_runtime(card, &card->dai_link[0]);

	if (ucontrol->value.integer.value[0] > 0) {
		glb_ptr->dual_mode = ucontrol->value.integer.value[0];
		glb_ptr->set_mode = 0;
	} else {
		if (glb_ptr->set_mode <= 0) {
			glb_ptr->dual_mode = 1;
			glb_ptr->set_mode = 0;
		} else {
			glb_ptr->dual_mode = 0;
			return 0;
		}
	}

	if (glb_ptr->dual_mode == 1) { // Dual Mono
		snd_soc_component_write(snd_soc_rtd_to_codec(rtd, 0)->component,
				PCM512x_MUTE, P_DAC_RIGHT_MUTE);
		snd_soc_component_write(snd_soc_rtd_to_codec(rtd, 1)->component,
				PCM512x_MUTE, P_DAC_LEFT_MUTE);
		snd_soc_component_write(snd_soc_rtd_to_codec(rtd, 0)->component,
				PCM512x_DIGITAL_VOLUME_3, 0xff);
		snd_soc_component_write(snd_soc_rtd_to_codec(rtd, 1)->component,
				PCM512x_DIGITAL_VOLUME_2, 0xff);

		list_for_each_entry(kctl, &snd_card_ptr->controls, list) {
			if (!strncmp(kctl->id.name, "Main Digital Playback Volume",
				sizeof(kctl->id.name))) {
				mc = (struct soc_mixer_control *)
					kctl->private_value;
				mc->rreg = mc->reg;
				break;
			}
			if (!strncmp(kctl->id.name, "Sub Digital Playback Volume",
				sizeof(kctl->id.name))) {
				mc = (struct soc_mixer_control *)
					kctl->private_value;
				mc->rreg = mc->reg;
				break;
			}
		}
	} else {
		left_val = snd_soc_component_read(snd_soc_rtd_to_codec(rtd, 0)->component,
						PCM512x_DIGITAL_VOLUME_2);
		right_val = snd_soc_component_read(snd_soc_rtd_to_codec(rtd, 1)->component,
						PCM512x_DIGITAL_VOLUME_3);

		list_for_each_entry(kctl, &snd_card_ptr->controls, list) {
			if (!strncmp(kctl->id.name, "Main Digital Playback Volume",
				sizeof(kctl->id.name))) {
				mc = (struct soc_mixer_control *)
					kctl->private_value;
				mc->rreg = PCM512x_DIGITAL_VOLUME_3;
				break;
			}
			if (!strncmp(kctl->id.name, "Sub Digital Playback Volume",
				sizeof(kctl->id.name))) {
				mc = (struct soc_mixer_control *)
					kctl->private_value;
				mc->rreg = PCM512x_DIGITAL_VOLUME_2;
				break;
			}
		}

		snd_soc_component_write(snd_soc_rtd_to_codec(rtd, 0)->component,
				PCM512x_DIGITAL_VOLUME_3, left_val);
		snd_soc_component_write(snd_soc_rtd_to_codec(rtd, 1)->component,
				PCM512x_DIGITAL_VOLUME_2, right_val);
		snd_soc_component_write(snd_soc_rtd_to_codec(rtd, 0)->component,
				PCM512x_MUTE, P_DAC_UNMUTE);
		snd_soc_component_write(snd_soc_rtd_to_codec(rtd, 1)->component,
				PCM512x_MUTE, P_DAC_UNMUTE);
	}

	return 0;
}

static int snd_allo_piano_mode_get(struct snd_kcontrol *kcontrol,
		struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_card *card = snd_kcontrol_chip(kcontrol);
	struct glb_pool *glb_ptr = card->drvdata;

	ucontrol->value.integer.value[0] = glb_ptr->set_mode;
	return 0;
}

static int snd_allo_piano_mode_put(struct snd_kcontrol *kcontrol,
		struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_card *card = snd_kcontrol_chip(kcontrol);
	struct snd_soc_pcm_runtime *rtd;
	struct glb_pool *glb_ptr = card->drvdata;
	struct snd_card *snd_card_ptr = card->snd_card;
	struct snd_kcontrol *kctl;
	struct soc_mixer_control *mc;
	unsigned int left_val = 0, right_val = 0;

	rtd = snd_soc_get_pcm_runtime(card, &card->dai_link[0]);

	if ((glb_ptr->dual_mode == 1) &&
			(ucontrol->value.integer.value[0] > 0)) {
		left_val = snd_soc_component_read(snd_soc_rtd_to_codec(rtd, 0)->component,
						PCM512x_DIGITAL_VOLUME_2);
		right_val = snd_soc_component_read(snd_soc_rtd_to_codec(rtd, 1)->component,
						PCM512x_DIGITAL_VOLUME_2);

		list_for_each_entry(kctl, &snd_card_ptr->controls, list) {
			if (!strncmp(kctl->id.name, "Main Digital Playback Volume",
				sizeof(kctl->id.name))) {
				mc = (struct soc_mixer_control *)
					kctl->private_value;
				mc->rreg = PCM512x_DIGITAL_VOLUME_3;
				break;
			}
			if (!strncmp(kctl->id.name, "Sub Digital Playback Volume",
				sizeof(kctl->id.name))) {
				mc = (struct soc_mixer_control *)
					kctl->private_value;
				mc->rreg = PCM512x_DIGITAL_VOLUME_2;
				break;
			}
		}
		snd_soc_component_write(snd_soc_rtd_to_codec(rtd, 0)->component,
				PCM512x_DIGITAL_VOLUME_3, left_val);
		snd_soc_component_write(snd_soc_rtd_to_codec(rtd, 1)->component,
				PCM512x_DIGITAL_VOLUME_3, right_val);
	}

	return(snd_allo_piano_dsp_program(rtd,
				ucontrol->value.integer.value[0],
				glb_ptr->set_rate, glb_ptr->set_lowpass));
}

static int snd_allo_piano_lowpass_get(struct snd_kcontrol *kcontrol,
		struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_card *card = snd_kcontrol_chip(kcontrol);
	struct glb_pool *glb_ptr = card->drvdata;

	ucontrol->value.integer.value[0] = glb_ptr->set_lowpass;
	return 0;
}

static int snd_allo_piano_lowpass_put(struct snd_kcontrol *kcontrol,
		struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_card *card = snd_kcontrol_chip(kcontrol);
	struct snd_soc_pcm_runtime *rtd;
	struct glb_pool *glb_ptr = card->drvdata;

	rtd = snd_soc_get_pcm_runtime(card, &card->dai_link[0]);
	return(snd_allo_piano_dsp_program(rtd,
				glb_ptr->set_mode, glb_ptr->set_rate,
				ucontrol->value.integer.value[0]));
}

static int pcm512x_get_reg_sub(struct snd_kcontrol *kcontrol,
		struct snd_ctl_elem_value *ucontrol)
{
	struct soc_mixer_control *mc =
		(struct soc_mixer_control *)kcontrol->private_value;
	struct snd_soc_card *card = snd_kcontrol_chip(kcontrol);
	struct glb_pool *glb_ptr = card->drvdata;
	struct snd_soc_pcm_runtime *rtd;
	unsigned int left_val = 0;
	unsigned int right_val = 0;
	rtd = snd_soc_get_pcm_runtime(card, &card->dai_link[0]);

	right_val = snd_soc_component_read(snd_soc_rtd_to_codec(rtd, 1)->component,
			PCM512x_DIGITAL_VOLUME_3);
	if (glb_ptr->dual_mode != 1) {
		left_val = snd_soc_component_read(snd_soc_rtd_to_codec(rtd, 1)->component,
				PCM512x_DIGITAL_VOLUME_2);

	} else {
		left_val = right_val;
	}

	ucontrol->value.integer.value[0] =
				(~(left_val >> mc->shift)) & mc->max;
	ucontrol->value.integer.value[1] =
				(~(right_val >> mc->shift)) & mc->max;

	return 0;
}

static int pcm512x_set_reg_sub(struct snd_kcontrol *kcontrol,
		struct snd_ctl_elem_value *ucontrol)
{
	struct soc_mixer_control *mc =
		(struct soc_mixer_control *)kcontrol->private_value;
	struct snd_soc_card *card = snd_kcontrol_chip(kcontrol);
	struct glb_pool *glb_ptr = card->drvdata;
	struct snd_soc_pcm_runtime *rtd;
	unsigned int left_val = (ucontrol->value.integer.value[0] & mc->max);
	unsigned int right_val = (ucontrol->value.integer.value[1] & mc->max);
	int ret = 0;

	rtd = snd_soc_get_pcm_runtime(card, &card->dai_link[0]);

	// When in Dual Mono, Sub vol control should not set anything.
	if (glb_ptr->dual_mode != 1) { //Not in Dual Mono mode

		ret = snd_soc_component_write(snd_soc_rtd_to_codec(rtd, 1)->component,
				PCM512x_DIGITAL_VOLUME_2, (~left_val));
		if (ret < 0)
			return ret;

		ret = snd_soc_component_write(snd_soc_rtd_to_codec(rtd, 1)->component,
				PCM512x_DIGITAL_VOLUME_3, (~right_val));
		if (ret < 0)
			return ret;

	}

	return 1;
}

static int pcm512x_get_reg_sub_switch(struct snd_kcontrol *kcontrol,
		struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_card *card = snd_kcontrol_chip(kcontrol);
	struct snd_soc_pcm_runtime *rtd;
	int val = 0;

	rtd = snd_soc_get_pcm_runtime(card, &card->dai_link[0]);
	val = snd_soc_component_read(snd_soc_rtd_to_codec(rtd, 1)->component, PCM512x_MUTE);

	ucontrol->value.integer.value[0] =
			(val & P_DAC_LEFT_MUTE) ? P_UNMUTE : P_MUTE;
	ucontrol->value.integer.value[1] =
			(val & P_DAC_RIGHT_MUTE) ? P_UNMUTE : P_MUTE;

	return val;
}

static int pcm512x_set_reg_sub_switch(struct snd_kcontrol *kcontrol,
		struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_card *card = snd_kcontrol_chip(kcontrol);
	struct snd_soc_pcm_runtime *rtd;
	struct glb_pool *glb_ptr = card->drvdata;
	unsigned int left_val = (ucontrol->value.integer.value[0]);
	unsigned int right_val = (ucontrol->value.integer.value[1]);
	int ret = 0;

	rtd = snd_soc_get_pcm_runtime(card, &card->dai_link[0]);
	if (glb_ptr->set_mode != 1) {
		ret = snd_soc_component_write(snd_soc_rtd_to_codec(rtd, 1)->component, PCM512x_MUTE,
				~((left_val & 0x01)<<4 | (right_val & 0x01)));
		if (ret < 0)
			return ret;
	}
	return 1;

}

static int pcm512x_get_reg_master(struct snd_kcontrol *kcontrol,
		struct snd_ctl_elem_value *ucontrol)
{
	struct soc_mixer_control *mc =
		(struct soc_mixer_control *)kcontrol->private_value;
	struct snd_soc_card *card = snd_kcontrol_chip(kcontrol);
	struct glb_pool *glb_ptr = card->drvdata;
	struct snd_soc_pcm_runtime *rtd;
	unsigned int left_val = 0, right_val = 0;

	rtd = snd_soc_get_pcm_runtime(card, &card->dai_link[0]);

	left_val = snd_soc_component_read(snd_soc_rtd_to_codec(rtd, 0)->component,
			PCM512x_DIGITAL_VOLUME_2);

	if (glb_ptr->dual_mode == 1) {  // in Dual Mono mode
		right_val = snd_soc_component_read(snd_soc_rtd_to_codec(rtd, 1)->component,
				PCM512x_DIGITAL_VOLUME_3);
	} else {
		right_val = snd_soc_component_read(snd_soc_rtd_to_codec(rtd, 0)->component,
				PCM512x_DIGITAL_VOLUME_3);
	}

	ucontrol->value.integer.value[0] =
		(~(left_val  >> mc->shift)) & mc->max;
	ucontrol->value.integer.value[1] =
		(~(right_val >> mc->shift)) & mc->max;

	return 0;
}

static int pcm512x_set_reg_master(struct snd_kcontrol *kcontrol,
		struct snd_ctl_elem_value *ucontrol)
{
	struct soc_mixer_control *mc =
		(struct soc_mixer_control *)kcontrol->private_value;
	struct snd_soc_card *card = snd_kcontrol_chip(kcontrol);
	struct glb_pool *glb_ptr = card->drvdata;
	struct snd_soc_pcm_runtime *rtd;
	unsigned int left_val = (ucontrol->value.integer.value[0] & mc->max);
	unsigned int right_val = (ucontrol->value.integer.value[1] & mc->max);
	int ret = 0;

	rtd = snd_soc_get_pcm_runtime(card, &card->dai_link[0]);

	if (glb_ptr->dual_mode == 1) { //in Dual Mono Mode

		ret = snd_soc_component_write(snd_soc_rtd_to_codec(rtd, 0)->component,
				PCM512x_DIGITAL_VOLUME_2, (~left_val));
		if (ret < 0)
			return ret;

		ret = snd_soc_component_write(snd_soc_rtd_to_codec(rtd, 1)->component,
				PCM512x_DIGITAL_VOLUME_3, (~right_val));
		if (ret < 0)
			return ret;

	} else {

		ret = snd_soc_component_write(snd_soc_rtd_to_codec(rtd, 0)->component,
				PCM512x_DIGITAL_VOLUME_2, (~left_val));
		if (ret < 0)
			return ret;

		ret = snd_soc_component_write(snd_soc_rtd_to_codec(rtd, 0)->component,
				PCM512x_DIGITAL_VOLUME_3, (~right_val));
		if (ret < 0)
			return ret;

	}
	return 1;
}

static int pcm512x_get_reg_master_switch(struct snd_kcontrol *kcontrol,
		struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_card *card = snd_kcontrol_chip(kcontrol);
	struct glb_pool *glb_ptr = card->drvdata;
	struct snd_soc_pcm_runtime *rtd;
	int val = 0;

	rtd = snd_soc_get_pcm_runtime(card, &card->dai_link[0]);

	val = snd_soc_component_read(snd_soc_rtd_to_codec(rtd, 0)->component, PCM512x_MUTE);

	ucontrol->value.integer.value[0] =
			(val & P_DAC_LEFT_MUTE) ? P_UNMUTE : P_MUTE;

	if (glb_ptr->dual_mode == 1) {
		val = snd_soc_component_read(snd_soc_rtd_to_codec(rtd, 1)->component, PCM512x_MUTE);
	}
	ucontrol->value.integer.value[1] =
			(val & P_DAC_RIGHT_MUTE) ? P_UNMUTE : P_MUTE;

	return val;
}

static int pcm512x_set_reg_master_switch(struct snd_kcontrol *kcontrol,
		struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_card *card = snd_kcontrol_chip(kcontrol);
	struct snd_soc_pcm_runtime *rtd;
	struct glb_pool *glb_ptr = card->drvdata;
	unsigned int left_val = (ucontrol->value.integer.value[0]);
	unsigned int right_val = (ucontrol->value.integer.value[1]);
	int ret = 0;

	rtd = snd_soc_get_pcm_runtime(card, &card->dai_link[0]);
	if (glb_ptr->dual_mode == 1) {
		ret = snd_soc_component_write(snd_soc_rtd_to_codec(rtd, 0)->component, PCM512x_MUTE,
				~((left_val & 0x01)<<4));
		if (ret < 0)
			return ret;
		ret = snd_soc_component_write(snd_soc_rtd_to_codec(rtd, 1)->component, PCM512x_MUTE,
				~((right_val & 0x01)));
		if (ret < 0)
			return ret;

	} else if (glb_ptr->set_mode == 1) {
		ret = snd_soc_component_write(snd_soc_rtd_to_codec(rtd, 0)->component, PCM512x_MUTE,
				~((left_val & 0x01)<<4 | (right_val & 0x01)));
		if (ret < 0)
			return ret;

	} else {
		ret = snd_soc_component_write(snd_soc_rtd_to_codec(rtd, 0)->component, PCM512x_MUTE,
				~((left_val & 0x01)<<4 | (right_val & 0x01)));
		if (ret < 0)
			return ret;

		ret = snd_soc_component_write(snd_soc_rtd_to_codec(rtd, 1)->component, PCM512x_MUTE,
				~((left_val & 0x01)<<4 | (right_val & 0x01)));
		if (ret < 0)
			return ret;
	}
	return 1;
}

static const DECLARE_TLV_DB_SCALE(digital_tlv_sub, -10350, 50, 1);
static const DECLARE_TLV_DB_SCALE(digital_tlv_master, -10350, 50, 1);

static const struct snd_kcontrol_new allo_piano_controls[] = {
	SOC_ENUM_EXT("Subwoofer mode Route",
			allo_piano_mode_enum,
			snd_allo_piano_mode_get,
			snd_allo_piano_mode_put),

	SOC_ENUM_EXT("Dual Mode Route",
			allo_piano_dual_mode_enum,
			snd_allo_piano_dual_mode_get,
			snd_allo_piano_dual_mode_put),

	SOC_ENUM_EXT("Lowpass Route", allo_piano_enum,
			snd_allo_piano_lowpass_get,
			snd_allo_piano_lowpass_put),

	SOC_DOUBLE_R_EXT_TLV("Subwoofer Playback Volume",
			PCM512x_DIGITAL_VOLUME_2,
			PCM512x_DIGITAL_VOLUME_3, 0, 255, 1,
			pcm512x_get_reg_sub,
			pcm512x_set_reg_sub,
			digital_tlv_sub),

	SOC_DOUBLE_EXT("Subwoofer Playback Switch",
			PCM512x_MUTE,
			PCM512x_RQML_SHIFT,
			PCM512x_RQMR_SHIFT, 1, 1,
			pcm512x_get_reg_sub_switch,
			pcm512x_set_reg_sub_switch),

	SOC_DOUBLE_R_EXT_TLV("Master Playback Volume",
			PCM512x_DIGITAL_VOLUME_2,
			PCM512x_DIGITAL_VOLUME_3, 0, 255, 1,
			pcm512x_get_reg_master,
			pcm512x_set_reg_master,
			digital_tlv_master),

	SOC_DOUBLE_EXT("Master Playback Switch",
			PCM512x_MUTE,
			PCM512x_RQML_SHIFT,
			PCM512x_RQMR_SHIFT, 1, 1,
			pcm512x_get_reg_master_switch,
			pcm512x_set_reg_master_switch),
};

static const char * const codec_ctl_pfx[] = { "Main", "Sub" };
static const char * const codec_ctl_name[] = {
	"Digital Playback Volume",
	"Digital Playback Switch",
	"Auto Mute Mono Switch",
	"Auto Mute Switch",
	"Auto Mute Time Left",
	"Auto Mute Time Right",
	"Clock Missing Period",
	"Max Overclock DAC",
	"Max Overclock DSP",
	"Max Overclock PLL",
	"Volume Ramp Down Emergency Rate",
	"Volume Ramp Down Emergency Step",
	"Volume Ramp Up Rate",
	"Volume Ramp Down Rate",
	"Volume Ramp Up Step",
	"Volume Ramp Down Step"
};

static int snd_allo_piano_dac_init(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_card *card = rtd->card;
	struct glb_pool *glb_ptr;
	struct snd_kcontrol *kctl;
	int i, j;

	glb_ptr = kzalloc(sizeof(struct glb_pool), GFP_KERNEL);
	if (!glb_ptr)
		return -ENOMEM;

	card->drvdata = glb_ptr;
	glb_ptr->dual_mode = 2;
	glb_ptr->set_mode = 0;

	mutex_init(&glb_ptr->lock);

	// Remove codec controls
	for (i = 0; i < ARRAY_SIZE(codec_ctl_pfx); i++) {
		for (j = 0; j < ARRAY_SIZE(codec_ctl_name); j++) {
			char cname[256];

			sprintf(cname, "%s %s", codec_ctl_pfx[i], codec_ctl_name[j]);
			kctl = snd_soc_card_get_kcontrol(card, cname);
			if (kctl) {
				kctl->vd[0].access =
					SNDRV_CTL_ELEM_ACCESS_READWRITE;
				snd_ctl_remove(card->snd_card, kctl);
			}
		}
	}

	return 0;
}

static void snd_allo_piano_gpio_mute(struct snd_soc_card *card)
{
	if (mute_gpio[0])
		gpiod_set_value_cansleep(mute_gpio[0], P_MUTE);

	if (mute_gpio[1])
		gpiod_set_value_cansleep(mute_gpio[1], P_MUTE);
}

static void snd_allo_piano_gpio_unmute(struct snd_soc_card *card)
{
	if (mute_gpio[0])
		gpiod_set_value_cansleep(mute_gpio[0], P_UNMUTE);

	if (mute_gpio[1])
		gpiod_set_value_cansleep(mute_gpio[1], P_UNMUTE);
}

static int snd_allo_piano_set_bias_level(struct snd_soc_card *card,
	struct snd_soc_dapm_context *dapm, enum snd_soc_bias_level level)
{
	struct snd_soc_pcm_runtime *rtd;
	struct snd_soc_dai *codec_dai;

	rtd = snd_soc_get_pcm_runtime(card, &card->dai_link[0]);
	codec_dai = snd_soc_rtd_to_codec(rtd, 0);

	if (dapm->dev != codec_dai->dev)
		return 0;

	switch (level) {
	case SND_SOC_BIAS_PREPARE:
		if (dapm->bias_level != SND_SOC_BIAS_STANDBY)
			break;
		/* UNMUTE DAC */
		snd_allo_piano_gpio_unmute(card);
		break;

	case SND_SOC_BIAS_STANDBY:
		if (dapm->bias_level != SND_SOC_BIAS_PREPARE)
			break;
		/* MUTE DAC */
		snd_allo_piano_gpio_mute(card);
		break;

	default:
		break;
	}

	return 0;
}

static int snd_allo_piano_dac_startup(
	struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_card *card = rtd->card;

	snd_allo_piano_gpio_mute(card);

	return 0;
}

static int snd_allo_piano_dac_hw_params(
		struct snd_pcm_substream *substream,
		struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	unsigned int rate = params_rate(params);
	struct snd_soc_card *card = rtd->card;
	struct glb_pool *glb_ptr = card->drvdata;
	int ret = 0, val = 0, dac;

	for (dac = 0; (glb_mclk && dac < 2); dac++) {
		/* Configure the PLL clock reference for both the Codecs */
		val = snd_soc_component_read(snd_soc_rtd_to_codec(rtd, dac)->component,
					PCM512x_RATE_DET_4);

		if (val & 0x40) {
			snd_soc_component_write(snd_soc_rtd_to_codec(rtd, dac)->component,
					PCM512x_PLL_REF,
					PCM512x_SREF_BCK);

			dev_info(snd_soc_rtd_to_codec(rtd, dac)->component->dev,
				"Setting BCLK as input clock & Enable PLL\n");
		} else {
			snd_soc_component_write(snd_soc_rtd_to_codec(rtd, dac)->component,
					PCM512x_PLL_EN,
					0x00);

			snd_soc_component_write(snd_soc_rtd_to_codec(rtd, dac)->component,
					PCM512x_PLL_REF,
					PCM512x_SREF_SCK);

			dev_info(snd_soc_rtd_to_codec(rtd, dac)->component->dev,
				"Setting SCLK as input clock & disabled PLL\n");
		}
	}

	ret = snd_allo_piano_dsp_program(rtd, glb_ptr->set_mode, rate,
						glb_ptr->set_lowpass);
	if (ret < 0)
		return ret;

	return ret;
}

static int snd_allo_piano_dac_prepare(
	struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_card *card = rtd->card;

	snd_allo_piano_gpio_unmute(card);

	return 0;
}

/* machine stream operations */
static struct snd_soc_ops snd_allo_piano_dac_ops = {
	.startup = snd_allo_piano_dac_startup,
	.hw_params = snd_allo_piano_dac_hw_params,
	.prepare = snd_allo_piano_dac_prepare,
};

static struct snd_soc_dai_link_component allo_piano_2_1_codecs[] = {
	{
		.dai_name = "pcm512x-hifi",
	},
	{
		.dai_name = "pcm512x-hifi",
	},
};

SND_SOC_DAILINK_DEFS(allo_piano_dai_plus,
	DAILINK_COMP_ARRAY(COMP_EMPTY()),
	DAILINK_COMP_ARRAY(COMP_CODEC("pcm512x.1-004c", "pcm512x-hifi"),
			   COMP_CODEC("pcm512x.1-004d", "pcm512x-hifi")),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));

static struct snd_soc_dai_link snd_allo_piano_dac_dai[] = {
	{
		.name		= "PianoDACPlus",
		.stream_name	= "PianoDACPlus",
		.dai_fmt	= SND_SOC_DAIFMT_I2S |
				SND_SOC_DAIFMT_NB_NF |
				SND_SOC_DAIFMT_CBS_CFS,
		.ops		= &snd_allo_piano_dac_ops,
		.init		= snd_allo_piano_dac_init,
		SND_SOC_DAILINK_REG(allo_piano_dai_plus),
	},
};

/* audio machine driver */
static struct snd_soc_card snd_allo_piano_dac = {
	.name = "PianoDACPlus",
	.owner = THIS_MODULE,
	.dai_link = snd_allo_piano_dac_dai,
	.num_links = ARRAY_SIZE(snd_allo_piano_dac_dai),
	.controls = allo_piano_controls,
	.num_controls = ARRAY_SIZE(allo_piano_controls),
};

static int snd_allo_piano_dac_probe(struct platform_device *pdev)
{
	struct snd_soc_card *card = &snd_allo_piano_dac;
	int ret = 0, i = 0;

	card->dev = &pdev->dev;
	platform_set_drvdata(pdev, &snd_allo_piano_dac);

	if (pdev->dev.of_node) {
		struct device_node *i2s_node;
		struct snd_soc_dai_link *dai;

		dai = &snd_allo_piano_dac_dai[0];
		i2s_node = of_parse_phandle(pdev->dev.of_node,
						"i2s-controller", 0);
		if (i2s_node) {
			for (i = 0; i < card->num_links; i++) {
				dai->cpus->dai_name = NULL;
				dai->cpus->of_node = i2s_node;
				dai->platforms->name = NULL;
				dai->platforms->of_node = i2s_node;
			}
		}
		digital_gain_0db_limit =
			!of_property_read_bool(pdev->dev.of_node,
						"allo,24db_digital_gain");

		glb_mclk = of_property_read_bool(pdev->dev.of_node,
						"allo,glb_mclk");

		allo_piano_2_1_codecs[0].of_node =
			of_parse_phandle(pdev->dev.of_node, "audio-codec", 0);
		allo_piano_2_1_codecs[1].of_node =
			of_parse_phandle(pdev->dev.of_node, "audio-codec", 1);
		if (!allo_piano_2_1_codecs[0].of_node || !allo_piano_2_1_codecs[1].of_node)
			return dev_err_probe(&pdev->dev, -EINVAL,
				"Property 'audio-codec' missing or invalid\n");

		mute_gpio[0] = devm_gpiod_get_optional(&pdev->dev, "mute1",
							GPIOD_OUT_LOW);
		if (IS_ERR(mute_gpio[0]))
			return dev_err_probe(&pdev->dev, PTR_ERR(mute_gpio[0]),
				"failed to get mute1 gpio\n");

		mute_gpio[1] = devm_gpiod_get_optional(&pdev->dev, "mute2",
							GPIOD_OUT_LOW);
		if (IS_ERR(mute_gpio[1]))
			return dev_err_probe(&pdev->dev, PTR_ERR(mute_gpio[1]),
				"failed to get mute2 gpio\n");

		if (mute_gpio[0] && mute_gpio[1])
			snd_allo_piano_dac.set_bias_level =
				snd_allo_piano_set_bias_level;

		ret = snd_soc_register_card(&snd_allo_piano_dac);
		if (ret < 0)
			return dev_err_probe(&pdev->dev, ret, "snd_soc_register_card() failed\n");

		if (digital_gain_0db_limit) {
			ret = snd_soc_limit_volume(card, "Master Playback Volume",
						207);
			if (ret < 0)
				dev_warn(card->dev, "Failed to set master volume limit: %d\n",
					ret);

			ret = snd_soc_limit_volume(card, "Subwoofer Playback Volume",
						207);
			if (ret < 0)
				dev_warn(card->dev, "Failed to set subwoofer volume limit: %d\n",
					ret);
		}

		if ((mute_gpio[0]) && (mute_gpio[1]))
			snd_allo_piano_gpio_mute(&snd_allo_piano_dac);

		return 0;
	}

	return -EINVAL;
}

static void snd_allo_piano_dac_remove(struct platform_device *pdev)
{
	struct snd_soc_card *card = platform_get_drvdata(pdev);

	kfree(&card->drvdata);
	snd_allo_piano_gpio_mute(&snd_allo_piano_dac);
	snd_soc_unregister_card(&snd_allo_piano_dac);
}

static const struct of_device_id snd_allo_piano_dac_of_match[] = {
	{ .compatible = "allo,piano-dac-plus", },
	{ /* sentinel */ },
};

MODULE_DEVICE_TABLE(of, snd_allo_piano_dac_of_match);

static struct platform_driver snd_allo_piano_dac_driver = {
	.driver = {
		.name = "snd-allo-piano-dac-plus",
		.owner = THIS_MODULE,
		.of_match_table = snd_allo_piano_dac_of_match,
	},
	.probe = snd_allo_piano_dac_probe,
	.remove = snd_allo_piano_dac_remove,
};

module_platform_driver(snd_allo_piano_dac_driver);

MODULE_AUTHOR("Baswaraj K <jaikumar@cem-solutions.net>");
MODULE_DESCRIPTION("ALSA ASoC Machine Driver for Allo Piano DAC Plus");
MODULE_LICENSE("GPL v2");
