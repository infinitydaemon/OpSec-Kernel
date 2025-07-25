// SPDX-License-Identifier: GPL-2.0-only
/*
 *  linux/drivers/mmc/core/sd.c
 *
 *  Copyright (C) 2003-2004 Russell King, All Rights Reserved.
 *  SD support Copyright (C) 2004 Ian Molton, All Rights Reserved.
 *  Copyright (C) 2005-2007 Pierre Ossman, All Rights Reserved.
 */

#include <linux/err.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/stat.h>
#include <linux/pm_runtime.h>
#include <linux/random.h>
#include <linux/scatterlist.h>
#include <linux/sysfs.h>

#include <linux/mmc/host.h>
#include <linux/mmc/card.h>
#include <linux/mmc/mmc.h>
#include <linux/mmc/sd.h>

#include "core.h"
#include "card.h"
#include "host.h"
#include "bus.h"
#include "mmc_ops.h"
#include "quirks.h"
#include "sd.h"
#include "sd_ops.h"

static const unsigned int tran_exp[] = {
	10000,		100000,		1000000,	10000000,
	0,		0,		0,		0
};

static const unsigned char tran_mant[] = {
	0,	10,	12,	13,	15,	20,	25,	30,
	35,	40,	45,	50,	55,	60,	70,	80,
};

static const unsigned int taac_exp[] = {
	1,	10,	100,	1000,	10000,	100000,	1000000, 10000000,
};

static const unsigned int taac_mant[] = {
	0,	10,	12,	13,	15,	20,	25,	30,
	35,	40,	45,	50,	55,	60,	70,	80,
};

static const unsigned int sd_au_size[] = {
	0,		SZ_16K / 512,		SZ_32K / 512,	SZ_64K / 512,
	SZ_128K / 512,	SZ_256K / 512,		SZ_512K / 512,	SZ_1M / 512,
	SZ_2M / 512,	SZ_4M / 512,		SZ_8M / 512,	(SZ_8M + SZ_4M) / 512,
	SZ_16M / 512,	(SZ_16M + SZ_8M) / 512,	SZ_32M / 512,	SZ_64M / 512,
};

#define SD_POWEROFF_NOTIFY_TIMEOUT_MS 1000
#define SD_WRITE_EXTR_SINGLE_TIMEOUT_MS 1000

struct sd_busy_data {
	struct mmc_card *card;
	u8 *reg_buf;
};

/*
 * Given the decoded CSD structure, decode the raw CID to our CID structure.
 */
void mmc_decode_cid(struct mmc_card *card)
{
	u32 *resp = card->raw_cid;

	/*
	 * Add the raw card ID (cid) data to the entropy pool. It doesn't
	 * matter that not all of it is unique, it's just bonus entropy.
	 */
	add_device_randomness(&card->raw_cid, sizeof(card->raw_cid));

	/*
	 * SD doesn't currently have a version field so we will
	 * have to assume we can parse this.
	 */
	card->cid.manfid		= unstuff_bits(resp, 120, 8);
	card->cid.oemid			= unstuff_bits(resp, 104, 16);
	card->cid.prod_name[0]		= unstuff_bits(resp, 96, 8);
	card->cid.prod_name[1]		= unstuff_bits(resp, 88, 8);
	card->cid.prod_name[2]		= unstuff_bits(resp, 80, 8);
	card->cid.prod_name[3]		= unstuff_bits(resp, 72, 8);
	card->cid.prod_name[4]		= unstuff_bits(resp, 64, 8);
	card->cid.hwrev			= unstuff_bits(resp, 60, 4);
	card->cid.fwrev			= unstuff_bits(resp, 56, 4);
	card->cid.serial		= unstuff_bits(resp, 24, 32);
	card->cid.year			= unstuff_bits(resp, 12, 8);
	card->cid.month			= unstuff_bits(resp, 8, 4);

	card->cid.year += 2000; /* SD cards year offset */
}

/*
 * Given a 128-bit response, decode to our card CSD structure.
 */
static int mmc_decode_csd(struct mmc_card *card, bool is_sduc)
{
	struct mmc_csd *csd = &card->csd;
	unsigned int e, m, csd_struct;
	u32 *resp = card->raw_csd;

	csd_struct = unstuff_bits(resp, 126, 2);

	switch (csd_struct) {
	case 0:
		m = unstuff_bits(resp, 115, 4);
		e = unstuff_bits(resp, 112, 3);
		csd->taac_ns	 = (taac_exp[e] * taac_mant[m] + 9) / 10;
		csd->taac_clks	 = unstuff_bits(resp, 104, 8) * 100;

		m = unstuff_bits(resp, 99, 4);
		e = unstuff_bits(resp, 96, 3);
		csd->max_dtr	  = tran_exp[e] * tran_mant[m];
		csd->cmdclass	  = unstuff_bits(resp, 84, 12);

		e = unstuff_bits(resp, 47, 3);
		m = unstuff_bits(resp, 62, 12);
		csd->capacity	  = (1 + m) << (e + 2);

		csd->read_blkbits = unstuff_bits(resp, 80, 4);
		csd->read_partial = unstuff_bits(resp, 79, 1);
		csd->write_misalign = unstuff_bits(resp, 78, 1);
		csd->read_misalign = unstuff_bits(resp, 77, 1);
		csd->dsr_imp = unstuff_bits(resp, 76, 1);
		csd->r2w_factor = unstuff_bits(resp, 26, 3);
		csd->write_blkbits = unstuff_bits(resp, 22, 4);
		csd->write_partial = unstuff_bits(resp, 21, 1);

		if (unstuff_bits(resp, 46, 1)) {
			csd->erase_size = 1;
		} else if (csd->write_blkbits >= 9) {
			csd->erase_size = unstuff_bits(resp, 39, 7) + 1;
			csd->erase_size <<= csd->write_blkbits - 9;
		}

		if (unstuff_bits(resp, 13, 1))
			mmc_card_set_readonly(card);
		break;
	case 1:
	case 2:
		/*
		 * This is a block-addressed SDHC, SDXC or SDUC card.
		 * Most interesting fields are unused and have fixed
		 * values. To avoid getting tripped by buggy cards,
		 * we assume those fixed values ourselves.
		 */
		mmc_card_set_blockaddr(card);

		csd->taac_ns	 = 0; /* Unused */
		csd->taac_clks	 = 0; /* Unused */

		m = unstuff_bits(resp, 99, 4);
		e = unstuff_bits(resp, 96, 3);
		csd->max_dtr	  = tran_exp[e] * tran_mant[m];
		csd->cmdclass	  = unstuff_bits(resp, 84, 12);

		if (csd_struct == 1)
			m = unstuff_bits(resp, 48, 22);
		else
			m = unstuff_bits(resp, 48, 28);
		csd->c_size = m;

		if (csd->c_size >= 0x400000 && is_sduc)
			mmc_card_set_ult_capacity(card);
		else if (csd->c_size >= 0xFFFF)
			mmc_card_set_ext_capacity(card);

		csd->capacity     = (1 + (typeof(sector_t))m) << 10;

		csd->read_blkbits = 9;
		csd->read_partial = 0;
		csd->write_misalign = 0;
		csd->read_misalign = 0;
		csd->r2w_factor = 4; /* Unused */
		csd->write_blkbits = 9;
		csd->write_partial = 0;
		csd->erase_size = 1;

		if (unstuff_bits(resp, 13, 1))
			mmc_card_set_readonly(card);
		break;
	default:
		pr_err("%s: unrecognised CSD structure version %d\n",
			mmc_hostname(card->host), csd_struct);
		return -EINVAL;
	}

	card->erase_size = csd->erase_size;

	return 0;
}

/*
 * Given a 64-bit response, decode to our card SCR structure.
 */
static int mmc_decode_scr(struct mmc_card *card)
{
	struct sd_scr *scr = &card->scr;
	unsigned int scr_struct;
	u32 resp[4];

	resp[3] = card->raw_scr[1];
	resp[2] = card->raw_scr[0];

	scr_struct = unstuff_bits(resp, 60, 4);
	if (scr_struct != 0) {
		pr_err("%s: unrecognised SCR structure version %d\n",
			mmc_hostname(card->host), scr_struct);
		return -EINVAL;
	}

	scr->sda_vsn = unstuff_bits(resp, 56, 4);
	scr->bus_widths = unstuff_bits(resp, 48, 4);
	if (scr->sda_vsn == SCR_SPEC_VER_2)
		/* Check if Physical Layer Spec v3.0 is supported */
		scr->sda_spec3 = unstuff_bits(resp, 47, 1);

	if (scr->sda_spec3) {
		scr->sda_spec4 = unstuff_bits(resp, 42, 1);
		scr->sda_specx = unstuff_bits(resp, 38, 4);
	}

	if (unstuff_bits(resp, 55, 1))
		card->erased_byte = 0xFF;
	else
		card->erased_byte = 0x0;

	if (scr->sda_spec4)
		scr->cmds = unstuff_bits(resp, 32, 4);
	else if (scr->sda_spec3)
		scr->cmds = unstuff_bits(resp, 32, 2);

	/* SD Spec says: any SD Card shall set at least bits 0 and 2 */
	if (!(scr->bus_widths & SD_SCR_BUS_WIDTH_1) ||
	    !(scr->bus_widths & SD_SCR_BUS_WIDTH_4)) {
		pr_err("%s: invalid bus width\n", mmc_hostname(card->host));
		return -EINVAL;
	}

	return 0;
}

/*
 * Fetch and process SD Status register.
 */
static int mmc_read_ssr(struct mmc_card *card)
{
	unsigned int au, es, et, eo;
	__be32 *raw_ssr;
	u32 resp[4] = {};
	u8 discard_support;
	int i;

	if (!(card->csd.cmdclass & CCC_APP_SPEC)) {
		pr_warn("%s: card lacks mandatory SD Status function\n",
			mmc_hostname(card->host));
		return 0;
	}

	raw_ssr = kmalloc(sizeof(card->raw_ssr), GFP_KERNEL);
	if (!raw_ssr)
		return -ENOMEM;

	if (mmc_app_sd_status(card, raw_ssr)) {
		pr_warn("%s: problem reading SD Status register\n",
			mmc_hostname(card->host));
		kfree(raw_ssr);
		return 0;
	}

	for (i = 0; i < 16; i++)
		card->raw_ssr[i] = be32_to_cpu(raw_ssr[i]);

	kfree(raw_ssr);

	/*
	 * unstuff_bits only works with four u32s so we have to offset the
	 * bitfield positions accordingly.
	 */
	au = unstuff_bits(card->raw_ssr, 428 - 384, 4);
	if (au) {
		if (au <= 9 || card->scr.sda_spec3) {
			card->ssr.au = sd_au_size[au];
			es = unstuff_bits(card->raw_ssr, 408 - 384, 16);
			et = unstuff_bits(card->raw_ssr, 402 - 384, 6);
			if (es && et) {
				eo = unstuff_bits(card->raw_ssr, 400 - 384, 2);
				card->ssr.erase_timeout = (et * 1000) / es;
				card->ssr.erase_offset = eo * 1000;
			}
		} else {
			pr_warn("%s: SD Status: Invalid Allocation Unit size\n",
				mmc_hostname(card->host));
		}
	}

	/*
	 * starting SD5.1 discard is supported if DISCARD_SUPPORT (b313) is set
	 */
	resp[3] = card->raw_ssr[6];
	discard_support = unstuff_bits(resp, 313 - 288, 1);
	card->erase_arg = (card->scr.sda_specx && discard_support) ?
			    SD_DISCARD_ARG : SD_ERASE_ARG;

	return 0;
}

/*
 * Fetches and decodes switch information
 */
static int mmc_read_switch(struct mmc_card *card)
{
	int err;
	u8 *status;

	if (card->scr.sda_vsn < SCR_SPEC_VER_1)
		return 0;

	if (!(card->csd.cmdclass & CCC_SWITCH)) {
		pr_warn("%s: card lacks mandatory switch function, performance might suffer\n",
			mmc_hostname(card->host));
		return 0;
	}

	status = kmalloc(64, GFP_KERNEL);
	if (!status)
		return -ENOMEM;

	/*
	 * Find out the card's support bits with a mode 0 operation.
	 * The argument does not matter, as the support bits do not
	 * change with the arguments.
	 */
	err = mmc_sd_switch(card, SD_SWITCH_CHECK, 0, 0, status);
	if (err) {
		/*
		 * If the host or the card can't do the switch,
		 * fail more gracefully.
		 */
		if (err != -EINVAL && err != -ENOSYS && err != -EFAULT)
			goto out;

		pr_warn("%s: problem reading Bus Speed modes\n",
			mmc_hostname(card->host));
		err = 0;

		goto out;
	}

	if (status[13] & SD_MODE_HIGH_SPEED)
		card->sw_caps.hs_max_dtr = HIGH_SPEED_MAX_DTR;

	if (card->scr.sda_spec3) {
		card->sw_caps.sd3_bus_mode = status[13];
		/* Driver Strengths supported by the card */
		card->sw_caps.sd3_drv_type = status[9];
		card->sw_caps.sd3_curr_limit = status[7] | status[6] << 8;
	}

out:
	kfree(status);

	return err;
}

/*
 * Test if the card supports high-speed mode and, if so, switch to it.
 */
int mmc_sd_switch_hs(struct mmc_card *card)
{
	int err;
	u8 *status;

	if (card->scr.sda_vsn < SCR_SPEC_VER_1)
		return 0;

	if (!(card->csd.cmdclass & CCC_SWITCH))
		return 0;

	if (!(card->host->caps & MMC_CAP_SD_HIGHSPEED))
		return 0;

	if (card->sw_caps.hs_max_dtr == 0)
		return 0;

	status = kmalloc(64, GFP_KERNEL);
	if (!status)
		return -ENOMEM;

	err = mmc_sd_switch(card, SD_SWITCH_SET, 0,
			HIGH_SPEED_BUS_SPEED, status);
	if (err)
		goto out;

	if ((status[16] & 0xF) != HIGH_SPEED_BUS_SPEED) {
		pr_warn("%s: Problem switching card into high-speed mode!\n",
			mmc_hostname(card->host));
		err = 0;
	} else {
		err = 1;
	}

out:
	kfree(status);

	return err;
}

static int sd_select_driver_type(struct mmc_card *card, u8 *status)
{
	int card_drv_type, drive_strength, drv_type;
	int err;

	card->drive_strength = 0;

	card_drv_type = card->sw_caps.sd3_drv_type | SD_DRIVER_TYPE_B;

	drive_strength = mmc_select_drive_strength(card,
						   card->sw_caps.uhs_max_dtr,
						   card_drv_type, &drv_type);

	if (drive_strength) {
		err = mmc_sd_switch(card, SD_SWITCH_SET, 2,
				drive_strength, status);
		if (err)
			return err;
		if ((status[15] & 0xF) != drive_strength) {
			pr_warn("%s: Problem setting drive strength!\n",
				mmc_hostname(card->host));
			return 0;
		}
		card->drive_strength = drive_strength;
	}

	if (drv_type)
		mmc_set_driver_type(card->host, drv_type);

	return 0;
}

static void sd_update_bus_speed_mode(struct mmc_card *card)
{
	/*
	 * If the host doesn't support any of the UHS-I modes, fallback on
	 * default speed.
	 */
	if (!mmc_host_uhs(card->host)) {
		card->sd_bus_speed = 0;
		return;
	}

	if ((card->host->caps & MMC_CAP_UHS_SDR104) &&
	    (card->sw_caps.sd3_bus_mode & SD_MODE_UHS_SDR104)) {
			card->sd_bus_speed = UHS_SDR104_BUS_SPEED;
	} else if ((card->host->caps & MMC_CAP_UHS_DDR50) &&
		   (card->sw_caps.sd3_bus_mode & SD_MODE_UHS_DDR50)) {
			card->sd_bus_speed = UHS_DDR50_BUS_SPEED;
	} else if ((card->host->caps & (MMC_CAP_UHS_SDR104 |
		    MMC_CAP_UHS_SDR50)) && (card->sw_caps.sd3_bus_mode &
		    SD_MODE_UHS_SDR50)) {
			card->sd_bus_speed = UHS_SDR50_BUS_SPEED;
	} else if ((card->host->caps & (MMC_CAP_UHS_SDR104 |
		    MMC_CAP_UHS_SDR50 | MMC_CAP_UHS_SDR25)) &&
		   (card->sw_caps.sd3_bus_mode & SD_MODE_UHS_SDR25)) {
			card->sd_bus_speed = UHS_SDR25_BUS_SPEED;
	} else if ((card->host->caps & (MMC_CAP_UHS_SDR104 |
		    MMC_CAP_UHS_SDR50 | MMC_CAP_UHS_SDR25 |
		    MMC_CAP_UHS_SDR12)) && (card->sw_caps.sd3_bus_mode &
		    SD_MODE_UHS_SDR12)) {
			card->sd_bus_speed = UHS_SDR12_BUS_SPEED;
	}
}

static int sd_set_bus_speed_mode(struct mmc_card *card, u8 *status)
{
	int err;
	unsigned int timing = 0;

	switch (card->sd_bus_speed) {
	case UHS_SDR104_BUS_SPEED:
		timing = MMC_TIMING_UHS_SDR104;
		card->sw_caps.uhs_max_dtr = UHS_SDR104_MAX_DTR;
		break;
	case UHS_DDR50_BUS_SPEED:
		timing = MMC_TIMING_UHS_DDR50;
		card->sw_caps.uhs_max_dtr = UHS_DDR50_MAX_DTR;
		break;
	case UHS_SDR50_BUS_SPEED:
		timing = MMC_TIMING_UHS_SDR50;
		card->sw_caps.uhs_max_dtr = UHS_SDR50_MAX_DTR;
		break;
	case UHS_SDR25_BUS_SPEED:
		timing = MMC_TIMING_UHS_SDR25;
		card->sw_caps.uhs_max_dtr = UHS_SDR25_MAX_DTR;
		break;
	case UHS_SDR12_BUS_SPEED:
		timing = MMC_TIMING_UHS_SDR12;
		card->sw_caps.uhs_max_dtr = UHS_SDR12_MAX_DTR;
		break;
	default:
		return 0;
	}

	err = mmc_sd_switch(card, SD_SWITCH_SET, 0, card->sd_bus_speed, status);
	if (err)
		return err;

	if ((status[16] & 0xF) != card->sd_bus_speed)
		pr_warn("%s: Problem setting bus speed mode!\n",
			mmc_hostname(card->host));
	else {
		mmc_set_timing(card->host, timing);
		mmc_set_clock(card->host, card->sw_caps.uhs_max_dtr);
	}

	return 0;
}

/* Get host's max current setting at its current voltage */
static u32 sd_get_host_max_current(struct mmc_host *host)
{
	u32 voltage, max_current;

	voltage = 1 << host->ios.vdd;
	switch (voltage) {
	case MMC_VDD_165_195:
		max_current = host->max_current_180;
		break;
	case MMC_VDD_29_30:
	case MMC_VDD_30_31:
		max_current = host->max_current_300;
		break;
	case MMC_VDD_32_33:
	case MMC_VDD_33_34:
		max_current = host->max_current_330;
		break;
	default:
		max_current = 0;
	}

	return max_current;
}

static int sd_set_current_limit(struct mmc_card *card, u8 *status)
{
	int current_limit = SD_SET_CURRENT_NO_CHANGE;
	int err;
	u32 max_current;

	/*
	 * Current limit switch is only defined for SDR50, SDR104, and DDR50
	 * bus speed modes. For other bus speed modes, we do not change the
	 * current limit.
	 */
	if ((card->sd_bus_speed != UHS_SDR50_BUS_SPEED) &&
	    (card->sd_bus_speed != UHS_SDR104_BUS_SPEED) &&
	    (card->sd_bus_speed != UHS_DDR50_BUS_SPEED))
		return 0;

	/*
	 * Host has different current capabilities when operating at
	 * different voltages, so find out its max current first.
	 */
	max_current = sd_get_host_max_current(card->host);

	/*
	 * We only check host's capability here, if we set a limit that is
	 * higher than the card's maximum current, the card will be using its
	 * maximum current, e.g. if the card's maximum current is 300ma, and
	 * when we set current limit to 200ma, the card will draw 200ma, and
	 * when we set current limit to 400/600/800ma, the card will draw its
	 * maximum 300ma from the host.
	 *
	 * The above is incorrect: if we try to set a current limit that is
	 * not supported by the card, the card can rightfully error out the
	 * attempt, and remain at the default current limit.  This results
	 * in a 300mA card being limited to 200mA even though the host
	 * supports 800mA. Failures seen with SanDisk 8GB UHS cards with
	 * an iMX6 host. --rmk
	 */
	if (max_current >= 800 &&
	    card->sw_caps.sd3_curr_limit & SD_MAX_CURRENT_800)
		current_limit = SD_SET_CURRENT_LIMIT_800;
	else if (max_current >= 600 &&
		 card->sw_caps.sd3_curr_limit & SD_MAX_CURRENT_600)
		current_limit = SD_SET_CURRENT_LIMIT_600;
	else if (max_current >= 400 &&
		 card->sw_caps.sd3_curr_limit & SD_MAX_CURRENT_400)
		current_limit = SD_SET_CURRENT_LIMIT_400;
	else if (max_current >= 200 &&
		 card->sw_caps.sd3_curr_limit & SD_MAX_CURRENT_200)
		current_limit = SD_SET_CURRENT_LIMIT_200;

	if (current_limit != SD_SET_CURRENT_NO_CHANGE) {
		err = mmc_sd_switch(card, SD_SWITCH_SET, 3,
				current_limit, status);
		if (err)
			return err;

		if (((status[15] >> 4) & 0x0F) != current_limit)
			pr_warn("%s: Problem setting current limit!\n",
				mmc_hostname(card->host));

	}

	return 0;
}

/*
 * Determine if the card should tune or not.
 */
static bool mmc_sd_use_tuning(struct mmc_card *card)
{
	/*
	 * SPI mode doesn't define CMD19 and tuning is only valid for SDR50 and
	 * SDR104 mode SD-cards. Note that tuning is mandatory for SDR104.
	 */
	if (mmc_host_is_spi(card->host))
		return false;

	switch (card->host->ios.timing) {
	case MMC_TIMING_UHS_SDR50:
	case MMC_TIMING_UHS_SDR104:
		return true;
	case MMC_TIMING_UHS_DDR50:
		return !mmc_card_no_uhs_ddr50_tuning(card);
	}

	return false;
}

/*
 * UHS-I specific initialization procedure
 */
static int mmc_sd_init_uhs_card(struct mmc_card *card)
{
	int err;
	u8 *status;

	if (!(card->csd.cmdclass & CCC_SWITCH))
		return 0;

	status = kmalloc(64, GFP_KERNEL);
	if (!status)
		return -ENOMEM;

	/* Set 4-bit bus width */
	err = mmc_app_set_bus_width(card, MMC_BUS_WIDTH_4);
	if (err)
		goto out;

	mmc_set_bus_width(card->host, MMC_BUS_WIDTH_4);

	/*
	 * Select the bus speed mode depending on host
	 * and card capability.
	 */
	sd_update_bus_speed_mode(card);

	/* Set the driver strength for the card */
	err = sd_select_driver_type(card, status);
	if (err)
		goto out;

	/* Set current limit for the card */
	err = sd_set_current_limit(card, status);
	if (err)
		goto out;

	/* Set bus speed mode of the card */
	err = sd_set_bus_speed_mode(card, status);
	if (err)
		goto out;

	if (mmc_sd_use_tuning(card)) {
		err = mmc_execute_tuning(card);

		/*
		 * As SD Specifications Part1 Physical Layer Specification
		 * Version 3.01 says, CMD19 tuning is available for unlocked
		 * cards in transfer state of 1.8V signaling mode. The small
		 * difference between v3.00 and 3.01 spec means that CMD19
		 * tuning is also available for DDR50 mode.
		 */
		if (err && card->host->ios.timing == MMC_TIMING_UHS_DDR50) {
			pr_warn("%s: ddr50 tuning failed\n",
				mmc_hostname(card->host));
			err = 0;
		}
	}

out:
	kfree(status);

	return err;
}

MMC_DEV_ATTR(cid, "%08x%08x%08x%08x\n", card->raw_cid[0], card->raw_cid[1],
	card->raw_cid[2], card->raw_cid[3]);
MMC_DEV_ATTR(csd, "%08x%08x%08x%08x\n", card->raw_csd[0], card->raw_csd[1],
	card->raw_csd[2], card->raw_csd[3]);
MMC_DEV_ATTR(scr, "%08x%08x\n", card->raw_scr[0], card->raw_scr[1]);
MMC_DEV_ATTR(ssr,
	"%08x%08x%08x%08x%08x%08x%08x%08x%08x%08x%08x%08x%08x%08x%08x%08x\n",
		card->raw_ssr[0], card->raw_ssr[1], card->raw_ssr[2],
		card->raw_ssr[3], card->raw_ssr[4], card->raw_ssr[5],
		card->raw_ssr[6], card->raw_ssr[7], card->raw_ssr[8],
		card->raw_ssr[9], card->raw_ssr[10], card->raw_ssr[11],
		card->raw_ssr[12], card->raw_ssr[13], card->raw_ssr[14],
		card->raw_ssr[15]);
MMC_DEV_ATTR(date, "%02d/%04d\n", card->cid.month, card->cid.year);
MMC_DEV_ATTR(erase_size, "%u\n", card->erase_size << 9);
MMC_DEV_ATTR(preferred_erase_size, "%u\n", card->pref_erase << 9);
MMC_DEV_ATTR(fwrev, "0x%x\n", card->cid.fwrev);
MMC_DEV_ATTR(hwrev, "0x%x\n", card->cid.hwrev);
MMC_DEV_ATTR(manfid, "0x%06x\n", card->cid.manfid);
MMC_DEV_ATTR(name, "%s\n", card->cid.prod_name);
MMC_DEV_ATTR(oemid, "0x%04x\n", card->cid.oemid);
MMC_DEV_ATTR(serial, "0x%08x\n", card->cid.serial);
MMC_DEV_ATTR(ocr, "0x%08x\n", card->ocr);
MMC_DEV_ATTR(rca, "0x%04x\n", card->rca);
MMC_DEV_ATTR(ext_perf, "%02x\n", card->ext_perf.feature_support);
MMC_DEV_ATTR(ext_power, "%02x\n", card->ext_power.feature_support);

static ssize_t mmc_dsr_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	struct mmc_card *card = mmc_dev_to_card(dev);
	struct mmc_host *host = card->host;

	if (card->csd.dsr_imp && host->dsr_req)
		return sysfs_emit(buf, "0x%x\n", host->dsr);
	/* return default DSR value */
	return sysfs_emit(buf, "0x%x\n", 0x404);
}

static DEVICE_ATTR(dsr, S_IRUGO, mmc_dsr_show, NULL);

MMC_DEV_ATTR(vendor, "0x%04x\n", card->cis.vendor);
MMC_DEV_ATTR(device, "0x%04x\n", card->cis.device);
MMC_DEV_ATTR(revision, "%u.%u\n", card->major_rev, card->minor_rev);

#define sdio_info_attr(num)									\
static ssize_t info##num##_show(struct device *dev, struct device_attribute *attr, char *buf)	\
{												\
	struct mmc_card *card = mmc_dev_to_card(dev);						\
												\
	if (num > card->num_info)								\
		return -ENODATA;								\
	if (!card->info[num - 1][0])								\
		return 0;									\
	return sysfs_emit(buf, "%s\n", card->info[num - 1]);					\
}												\
static DEVICE_ATTR_RO(info##num)

sdio_info_attr(1);
sdio_info_attr(2);
sdio_info_attr(3);
sdio_info_attr(4);

static struct attribute *sd_std_attrs[] = {
	&dev_attr_vendor.attr,
	&dev_attr_device.attr,
	&dev_attr_revision.attr,
	&dev_attr_info1.attr,
	&dev_attr_info2.attr,
	&dev_attr_info3.attr,
	&dev_attr_info4.attr,
	&dev_attr_cid.attr,
	&dev_attr_csd.attr,
	&dev_attr_scr.attr,
	&dev_attr_ssr.attr,
	&dev_attr_date.attr,
	&dev_attr_erase_size.attr,
	&dev_attr_preferred_erase_size.attr,
	&dev_attr_fwrev.attr,
	&dev_attr_hwrev.attr,
	&dev_attr_manfid.attr,
	&dev_attr_name.attr,
	&dev_attr_oemid.attr,
	&dev_attr_serial.attr,
	&dev_attr_ocr.attr,
	&dev_attr_rca.attr,
	&dev_attr_dsr.attr,
	&dev_attr_ext_perf.attr,
	&dev_attr_ext_power.attr,
	NULL,
};

static umode_t sd_std_is_visible(struct kobject *kobj, struct attribute *attr,
				 int index)
{
	struct device *dev = kobj_to_dev(kobj);
	struct mmc_card *card = mmc_dev_to_card(dev);

	/* CIS vendor and device ids, revision and info string are available only for Combo cards */
	if ((attr == &dev_attr_vendor.attr ||
	     attr == &dev_attr_device.attr ||
	     attr == &dev_attr_revision.attr ||
	     attr == &dev_attr_info1.attr ||
	     attr == &dev_attr_info2.attr ||
	     attr == &dev_attr_info3.attr ||
	     attr == &dev_attr_info4.attr
	    ) &&!mmc_card_sd_combo(card))
		return 0;

	return attr->mode;
}

static const struct attribute_group sd_std_group = {
	.attrs = sd_std_attrs,
	.is_visible = sd_std_is_visible,
};
__ATTRIBUTE_GROUPS(sd_std);

const struct device_type sd_type = {
	.groups = sd_std_groups,
};

/*
 * Fetch CID from card.
 */
int mmc_sd_get_cid(struct mmc_host *host, u32 ocr, u32 *cid, u32 *rocr)
{
	int err;
	u32 max_current;
	int retries = 10;
	u32 pocr = ocr;

try_again:
	if (!retries) {
		ocr &= ~SD_OCR_S18R;
		pr_warn("%s: Skipping voltage switch\n", mmc_hostname(host));
	}

	/*
	 * Since we're changing the OCR value, we seem to
	 * need to tell some cards to go back to the idle
	 * state.  We wait 1ms to give cards time to
	 * respond.
	 */
	mmc_go_idle(host);

	/*
	 * If SD_SEND_IF_COND indicates an SD 2.0
	 * compliant card and we should set bit 30
	 * of the ocr to indicate that we can handle
	 * block-addressed SDHC cards.
	 */
	err = mmc_send_if_cond(host, ocr);
	if (!err)
		ocr |= SD_OCR_CCS;

	/*
	 * If the host supports one of UHS-I modes, request the card
	 * to switch to 1.8V signaling level. If the card has failed
	 * repeatedly to switch however, skip this.
	 */
	if (retries && mmc_host_uhs(host))
		ocr |= SD_OCR_S18R;

	/*
	 * If the host can supply more than 150mA at current voltage,
	 * XPC should be set to 1.
	 */
	max_current = sd_get_host_max_current(host);
	if (max_current > 150)
		ocr |= SD_OCR_XPC;

	err = mmc_send_app_op_cond(host, ocr, rocr);
	if (err)
		return err;

	/*
	 * In case the S18A bit is set in the response, let's start the signal
	 * voltage switch procedure. SPI mode doesn't support CMD11.
	 * Note that, according to the spec, the S18A bit is not valid unless
	 * the CCS bit is set as well. We deliberately deviate from the spec in
	 * regards to this, which allows UHS-I to be supported for SDSC cards.
	 */
	if (!mmc_host_is_spi(host) && (ocr & SD_OCR_S18R) &&
	    rocr && (*rocr & SD_ROCR_S18A)) {
		err = mmc_set_uhs_voltage(host, pocr);
		if (err == -EAGAIN) {
			retries--;
			goto try_again;
		} else if (err) {
			retries = 0;
			goto try_again;
		}
	}

	err = mmc_send_cid(host, cid);
	return err;
}

int mmc_sd_get_csd(struct mmc_card *card, bool is_sduc)
{
	int err;

	/*
	 * Fetch CSD from card.
	 */
	err = mmc_send_csd(card, card->raw_csd);
	if (err)
		return err;

	err = mmc_decode_csd(card, is_sduc);
	if (err)
		return err;

	return 0;
}

static int mmc_sd_get_ro(struct mmc_host *host)
{
	int ro;

	/*
	 * Some systems don't feature a write-protect pin and don't need one.
	 * E.g. because they only have micro-SD card slot. For those systems
	 * assume that the SD card is always read-write.
	 */
	if (host->caps2 & MMC_CAP2_NO_WRITE_PROTECT)
		return 0;

	if (!host->ops->get_ro)
		return -1;

	ro = host->ops->get_ro(host);

	return ro;
}

int mmc_sd_setup_card(struct mmc_host *host, struct mmc_card *card,
	bool reinit)
{
	int err;

	if (!reinit) {
		/*
		 * Fetch SCR from card.
		 */
		err = mmc_app_send_scr(card);
		if (err)
			return err;

		err = mmc_decode_scr(card);
		if (err)
			return err;

		/*
		 * Fetch and process SD Status register.
		 */
		err = mmc_read_ssr(card);
		if (err)
			return err;

		/* Erase init depends on CSD and SSR */
		mmc_init_erase(card);
	}

	/*
	 * Fetch switch information from card. Note, sd3_bus_mode can change if
	 * voltage switch outcome changes, so do this always.
	 */
	err = mmc_read_switch(card);
	if (err)
		return err;

	/*
	 * For SPI, enable CRC as appropriate.
	 * This CRC enable is located AFTER the reading of the
	 * card registers because some SDHC cards are not able
	 * to provide valid CRCs for non-512-byte blocks.
	 */
	if (mmc_host_is_spi(host)) {
		err = mmc_spi_set_crc(host, use_spi_crc);
		if (err)
			return err;
	}

	/*
	 * Check if read-only switch is active.
	 */
	if (!reinit) {
		int ro = mmc_sd_get_ro(host);

		if (ro < 0) {
			pr_warn("%s: host does not support reading read-only switch, assuming write-enable\n",
				mmc_hostname(host));
		} else if (ro > 0) {
			mmc_card_set_readonly(card);
		}
	}

	return 0;
}

unsigned mmc_sd_get_max_clock(struct mmc_card *card)
{
	unsigned max_dtr = (unsigned int)-1;

	if (mmc_card_hs(card)) {
		if (max_dtr > card->sw_caps.hs_max_dtr)
			max_dtr = card->sw_caps.hs_max_dtr;
	} else if (max_dtr > card->csd.max_dtr) {
		max_dtr = card->csd.max_dtr;
	}

	return max_dtr;
}

static bool mmc_sd_card_using_v18(struct mmc_card *card)
{
	/*
	 * According to the SD spec., the Bus Speed Mode (function group 1) bits
	 * 2 to 4 are zero if the card is initialized at 3.3V signal level. Thus
	 * they can be used to determine if the card has already switched to
	 * 1.8V signaling.
	 */
	return card->sw_caps.sd3_bus_mode &
	       (SD_MODE_UHS_SDR50 | SD_MODE_UHS_SDR104 | SD_MODE_UHS_DDR50);
}

static int sd_parse_ext_reg_power(struct mmc_card *card, u8 fno, u8 page,
				  u16 offset)
{
	int err;
	u8 *reg_buf;

	reg_buf = card->ext_reg_buf;

	/* Read the extension register for power management function. */
	err = mmc_sd_read_ext_reg(card, fno, page, offset, 512, reg_buf);
	if (err) {
		pr_warn("%s: error %d reading PM func of ext reg\n",
			mmc_hostname(card->host), err);
		goto out;
	}

	/* PM revision consists of 4 bits. */
	card->ext_power.rev = reg_buf[0] & 0xf;

	/* Power Off Notification support at bit 4. */
	if ((reg_buf[1] & BIT(4)) && !mmc_card_broken_sd_poweroff_notify(card))
		card->ext_power.feature_support |= SD_EXT_POWER_OFF_NOTIFY;

	/* Power Sustenance support at bit 5. */
	if (reg_buf[1] & BIT(5))
		card->ext_power.feature_support |= SD_EXT_POWER_SUSTENANCE;

	/* Power Down Mode support at bit 6. */
	if (reg_buf[1] & BIT(6))
		card->ext_power.feature_support |= SD_EXT_POWER_DOWN_MODE;

	card->ext_power.fno = fno;
	card->ext_power.page = page;
	card->ext_power.offset = offset;

out:
	return err;
}

static int sd_parse_ext_reg_perf(struct mmc_card *card, u8 fno, u8 page,
				 u16 offset)
{
	int err;
	u8 *reg_buf;

	reg_buf = card->ext_reg_buf;

	err = mmc_sd_read_ext_reg(card, fno, page, offset, 512, reg_buf);
	if (err) {
		pr_warn("%s: error %d reading PERF func of ext reg\n",
			mmc_hostname(card->host), err);
		goto out;
	}

	/* PERF revision. */
	card->ext_perf.rev = reg_buf[0];

	/* FX_EVENT support at bit 0. */
	if (reg_buf[1] & BIT(0))
		card->ext_perf.feature_support |= SD_EXT_PERF_FX_EVENT;

	/* Card initiated self-maintenance support at bit 0. */
	if (reg_buf[2] & BIT(0))
		card->ext_perf.feature_support |= SD_EXT_PERF_CARD_MAINT;

	/* Host initiated self-maintenance support at bit 1. */
	if (reg_buf[2] & BIT(1))
		card->ext_perf.feature_support |= SD_EXT_PERF_HOST_MAINT;

	/* Cache support at bit 0. */
	if ((reg_buf[4] & BIT(0)) && !mmc_card_broken_sd_cache(card))
		card->ext_perf.feature_support |= SD_EXT_PERF_CACHE;

	/*
	 * Command queue support indicated via queue depth bits (0 to 4).
	 * Qualify this with the other mandatory required features.
	 */
	if (reg_buf[6] & 0x1f && card->ext_power.feature_support & SD_EXT_POWER_OFF_NOTIFY &&
	    card->ext_perf.feature_support & SD_EXT_PERF_CACHE) {
		card->ext_perf.feature_support |= SD_EXT_PERF_CMD_QUEUE;
		card->ext_csd.cmdq_depth = reg_buf[6] & 0x1f;
		card->ext_csd.cmdq_support = true;
		pr_debug("%s: Command Queue supported depth %u\n",
			 mmc_hostname(card->host),
			 card->ext_csd.cmdq_depth);
		/*
		 * If CQ is enabled, there is a contract between host and card such that
		 * VDD will be maintained and removed only if a power off notification
		 * is provided. An SD card in an accessible slot means surprise removal
		 * is a possibility. As a middle ground, keep the default maximum of 1
		 * posted write unless the card is "hardwired".
		 */
		if (!mmc_card_is_removable(card->host))
			card->max_posted_writes = card->ext_csd.cmdq_depth;
	}

	card->ext_perf.fno = fno;
	card->ext_perf.page = page;
	card->ext_perf.offset = offset;

out:
	return err;
}

static int sd_parse_ext_reg(struct mmc_card *card, u8 *gen_info_buf,
			    u16 *next_ext_addr)
{
	u8 num_regs, fno, page;
	u16 sfc, offset, ext = *next_ext_addr;
	u32 reg_addr;

	/*
	 * Parse only one register set per extension, as that is sufficient to
	 * support the standard functions. This means another 48 bytes in the
	 * buffer must be available.
	 */
	if (ext + 48 > 512)
		return -EFAULT;

	/* Standard Function Code */
	memcpy(&sfc, &gen_info_buf[ext], 2);

	/* Address to the next extension. */
	memcpy(next_ext_addr, &gen_info_buf[ext + 40], 2);

	/* Number of registers for this extension. */
	num_regs = gen_info_buf[ext + 42];

	/* We support only one register per extension. */
	if (num_regs != 1)
		return 0;

	/* Extension register address. */
	memcpy(&reg_addr, &gen_info_buf[ext + 44], 4);

	/* 9 bits (0 to 8) contains the offset address. */
	offset = reg_addr & 0x1ff;

	/* 8 bits (9 to 16) contains the page number. */
	page = reg_addr >> 9 & 0xff ;

	/* 4 bits (18 to 21) contains the function number. */
	fno = reg_addr >> 18 & 0xf;

	/* Standard Function Code for power management. */
	if (sfc == 0x1)
		return sd_parse_ext_reg_power(card, fno, page, offset);

	/* Standard Function Code for performance enhancement. */
	if (sfc == 0x2)
		return sd_parse_ext_reg_perf(card, fno, page, offset);

	return 0;
}

static int mmc_sd_read_ext_regs(struct mmc_card *card)
{
	int err, i;
	u8 num_ext, *gen_info_buf;
	u16 rev, len, next_ext_addr;

	if (mmc_host_is_spi(card->host))
		return 0;

	if (!(card->scr.cmds & SD_SCR_CMD48_SUPPORT))
		return 0;

	gen_info_buf = kzalloc(1024, GFP_KERNEL);
	if (!gen_info_buf)
		return -ENOMEM;

	card->ext_reg_buf = kzalloc(512, GFP_KERNEL);
	if (!card->ext_reg_buf) {
		err = -ENOMEM;
		goto out;
	}

	/*
	 * Read 512 bytes of general info, which is found at function number 0,
	 * at page 0 and with no offset.
	 */
	err = mmc_sd_read_ext_reg(card, 0, 0, 0, 512, gen_info_buf);
	if (err) {
		pr_err("%s: error %d reading general info of SD ext reg\n",
			mmc_hostname(card->host), err);
		goto out;
	}

	/* General info structure revision. */
	memcpy(&rev, &gen_info_buf[0], 2);

	/* Length of general info in bytes. */
	memcpy(&len, &gen_info_buf[2], 2);

	/* Number of extensions to be find. */
	num_ext = gen_info_buf[4];

	/*
	 * We only support revision 0 and up to the spec-defined maximum of 1K.
	 * No matter what, let's return zero to allow us to continue using the
	 * card, even if we can't support the features from the SD function
	 * extensions registers.
	 */
	if (rev != 0 || len > 1024) {
		pr_warn("%s: non-supported SD ext reg layout rev %u length %u\n",
			mmc_hostname(card->host), rev, len);
		goto out;
	}

	/* If the General Information block spills into the next page, read the rest */
	if (len > 512)
		err = mmc_sd_read_ext_reg(card, 0, 1, 0, 512, &gen_info_buf[512]);
	if (err) {
		pr_err("%s: error %d reading page 1 of general info of SD ext reg\n",
			mmc_hostname(card->host), err);
		goto out;
	}

	/*
	 * Parse the extension registers. The first extension should start
	 * immediately after the general info header (16 bytes).
	 */
	next_ext_addr = 16;
	for (i = 0; i < num_ext; i++) {
		err = sd_parse_ext_reg(card, gen_info_buf, &next_ext_addr);
		if (err) {
			pr_err("%s: error %d parsing SD ext reg\n",
				mmc_hostname(card->host), err);
			goto out;
		}
	}

out:
	kfree(gen_info_buf);
	return err;
}

static bool sd_cache_enabled(struct mmc_host *host)
{
	return host->card->ext_perf.feature_enabled & SD_EXT_PERF_CACHE;
}

static int sd_flush_cache(struct mmc_host *host)
{
	struct mmc_card *card = host->card;
	u8 *reg_buf, fno, page;
	u16 offset;
	int err;

	if (!sd_cache_enabled(host))
		return 0;

	reg_buf = card->ext_reg_buf;

	/*
	 * Set Flush Cache at bit 0 in the performance enhancement register at
	 * 261 bytes offset.
	 */
	fno = card->ext_perf.fno;
	page = card->ext_perf.page;
	offset = card->ext_perf.offset + 261;

	err = mmc_sd_write_ext_reg(card, fno, page, offset, BIT(0));
	if (err) {
		pr_warn("%s: error %d writing Cache Flush bit\n",
			mmc_hostname(host), err);
		goto out;
	}

	err = mmc_poll_for_busy(card, SD_WRITE_EXTR_SINGLE_TIMEOUT_MS, false,
				MMC_BUSY_EXTR_SINGLE);
	if (err)
		goto out;

	/*
	 * Read the Flush Cache bit. The card shall reset it, to confirm that
	 * it's has completed the flushing of the cache.
	 */
	err = mmc_sd_read_ext_reg(card, fno, page, offset, 1, reg_buf);
	if (err) {
		pr_warn("%s: error %d reading Cache Flush bit\n",
			mmc_hostname(host), err);
		goto out;
	}

	if (reg_buf[0] & BIT(0))
		err = -ETIMEDOUT;
out:
	return err;
}

static int sd_enable_cache(struct mmc_card *card)
{
	int err;

	card->ext_perf.feature_enabled &= ~SD_EXT_PERF_CACHE;

	/*
	 * Set Cache Enable at bit 0 in the performance enhancement register at
	 * 260 bytes offset.
	 */
	err = mmc_sd_write_ext_reg(card, card->ext_perf.fno, card->ext_perf.page,
			       card->ext_perf.offset + 260, BIT(0));
	if (err) {
		pr_warn("%s: error %d writing Cache Enable bit\n",
			mmc_hostname(card->host), err);
		goto out;
	}

	err = mmc_poll_for_busy(card, SD_WRITE_EXTR_SINGLE_TIMEOUT_MS, false,
				MMC_BUSY_EXTR_SINGLE);
	if (!err)
		card->ext_perf.feature_enabled |= SD_EXT_PERF_CACHE;

out:
	return err;
}

/*
 * Handle the detection and initialisation of a card.
 *
 * In the case of a resume, "oldcard" will contain the card
 * we're trying to reinitialise.
 */
static int mmc_sd_init_card(struct mmc_host *host, u32 ocr,
	struct mmc_card *oldcard)
{
	struct mmc_card *card;
	int err;
	u32 cid[4];
	u32 rocr = 0;
	bool v18_fixup_failed = false;

	WARN_ON(!host->claimed);
retry:
	err = mmc_sd_get_cid(host, ocr, cid, &rocr);
	if (err)
		return err;

	if (oldcard) {
		if (memcmp(cid, oldcard->raw_cid, sizeof(cid)) != 0) {
			pr_debug("%s: Perhaps the card was replaced\n",
				mmc_hostname(host));
			return -ENOENT;
		}

		card = oldcard;
	} else {
		/*
		 * Allocate card structure.
		 */
		card = mmc_alloc_card(host, &sd_type);
		if (IS_ERR(card))
			return PTR_ERR(card);

		card->ocr = ocr;
		card->type = MMC_TYPE_SD;
		card->max_posted_writes = 1;
		memcpy(card->raw_cid, cid, sizeof(card->raw_cid));
	}

	/*
	 * Call the optional HC's init_card function to handle quirks.
	 */
	if (host->ops->init_card)
		host->ops->init_card(host, card);

	/*
	 * For native busses:  get card RCA and quit open drain mode.
	 */
	if (!mmc_host_is_spi(host)) {
		err = mmc_send_relative_addr(host, &card->rca);
		if (err)
			goto free_card;
	}

	if (!oldcard) {
		err = mmc_sd_get_csd(card, false);
		if (err)
			goto free_card;

		mmc_decode_cid(card);
	}

	/*
	 * handling only for cards supporting DSR and hosts requesting
	 * DSR configuration
	 */
	if (card->csd.dsr_imp && host->dsr_req)
		mmc_set_dsr(host);

	/*
	 * Select card, as all following commands rely on that.
	 */
	if (!mmc_host_is_spi(host)) {
		err = mmc_select_card(card);
		if (err)
			goto free_card;
	}

	/* Apply quirks prior to card setup */
	mmc_fixup_device(card, mmc_sd_fixups);

	err = mmc_sd_setup_card(host, card, oldcard != NULL);
	if (err)
		goto free_card;

	/*
	 * If the card has not been power cycled, it may still be using 1.8V
	 * signaling. Detect that situation and try to initialize a UHS-I (1.8V)
	 * transfer mode.
	 */
	if (!v18_fixup_failed && !mmc_host_is_spi(host) && mmc_host_uhs(host) &&
	    mmc_sd_card_using_v18(card) &&
	    host->ios.signal_voltage != MMC_SIGNAL_VOLTAGE_180) {
		if (mmc_host_set_uhs_voltage(host) ||
		    mmc_sd_init_uhs_card(card)) {
			v18_fixup_failed = true;
			mmc_power_cycle(host, ocr);
			if (!oldcard)
				mmc_remove_card(card);
			goto retry;
		}
		goto cont;
	}

	/* Initialization sequence for UHS-I cards */
	if (rocr & SD_ROCR_S18A && mmc_host_uhs(host)) {
		err = mmc_sd_init_uhs_card(card);
		if (err)
			goto free_card;
	} else {
		/*
		 * Attempt to change to high-speed (if supported)
		 */
		err = mmc_sd_switch_hs(card);
		if (err > 0)
			mmc_set_timing(card->host, MMC_TIMING_SD_HS);
		else if (err)
			goto free_card;

		/*
		 * Set bus speed.
		 */
		mmc_set_clock(host, mmc_sd_get_max_clock(card));

		if (host->ios.timing == MMC_TIMING_SD_HS &&
			host->ops->prepare_sd_hs_tuning) {
			err = host->ops->prepare_sd_hs_tuning(host, card);
			if (err)
				goto free_card;
		}

		/*
		 * Switch to wider bus (if supported).
		 */
		if ((host->caps & MMC_CAP_4_BIT_DATA) &&
			(card->scr.bus_widths & SD_SCR_BUS_WIDTH_4)) {
			err = mmc_app_set_bus_width(card, MMC_BUS_WIDTH_4);
			if (err)
				goto free_card;

			mmc_set_bus_width(host, MMC_BUS_WIDTH_4);
		}

		if (host->ios.timing == MMC_TIMING_SD_HS &&
			host->ops->execute_sd_hs_tuning) {
			err = host->ops->execute_sd_hs_tuning(host, card);
			if (err)
				goto free_card;
		}
	}
cont:
	if (!oldcard) {
		/* Read/parse the extension registers. */
		err = mmc_sd_read_ext_regs(card);
		if (err)
			goto free_card;
	}

	/* Enable internal SD cache if supported. */
	if (card->ext_perf.feature_support & SD_EXT_PERF_CACHE) {
		err = sd_enable_cache(card);
		if (err)
			goto free_card;
	}

	/* Disallow command queueing on unvetted cards unless overridden */
	if (!(host->caps2 & MMC_CAP2_SD_CQE_PERMISSIVE) && !mmc_card_working_sd_cq(card))
		card->ext_csd.cmdq_support = false;

	/* Enable command queueing if supported */
	if (card->ext_csd.cmdq_support && host->caps2 & MMC_CAP2_CQE) {
		/*
		 * Right now the MMC block layer uses DCMDs to issue
		 * cache-flush commands specific to eMMC devices.
		 * Turning off DCMD support avoids generating Illegal Command
		 * errors on SD, and flushing is instead done synchronously
		 * by mmc_blk_issue_flush().
		 */
		host->caps2 &= ~MMC_CAP2_CQE_DCMD;
		err = mmc_sd_cmdq_enable(card);
		if (err && err != -EBADMSG)
			goto free_card;
		if (err) {
			pr_warn("%s: Enabling CMDQ failed\n",
				mmc_hostname(card->host));
			card->ext_csd.cmdq_support = false;
			card->ext_csd.cmdq_depth = 0;
		}
	}
	card->reenable_cmdq = card->ext_csd.cmdq_en;

	if (host->cqe_ops && !host->cqe_enabled) {
		err = host->cqe_ops->cqe_enable(host, card);
		if (!err) {
			host->cqe_enabled = true;

			if (card->ext_csd.cmdq_en) {
				pr_info("%s: Command Queue Engine enabled, %u tags\n",
					mmc_hostname(host), card->ext_csd.cmdq_depth);
			} else {
				host->hsq_enabled = true;
				pr_info("%s: Host Software Queue enabled\n",
					mmc_hostname(host));
			}
		}
	}

	if (host->caps2 & MMC_CAP2_AVOID_3_3V &&
	    host->ios.signal_voltage == MMC_SIGNAL_VOLTAGE_330) {
		pr_err("%s: Host failed to negotiate down from 3.3V\n",
			mmc_hostname(host));
		err = -EINVAL;
		goto free_card;
	}

	host->card = card;
	return 0;

free_card:
	if (!oldcard)
		mmc_remove_card(card);

	return err;
}

/*
 * Host is being removed. Free up the current card.
 */
static void mmc_sd_remove(struct mmc_host *host)
{
	mmc_remove_card(host->card);
	host->card = NULL;
}

/*
 * Card detection - card is alive.
 */
static int mmc_sd_alive(struct mmc_host *host)
{
	return mmc_send_status(host->card, NULL);
}

/*
 * Card detection callback from host.
 */
static void mmc_sd_detect(struct mmc_host *host)
{
	int err;

	mmc_get_card(host->card, NULL);

	/*
	 * Just check if our card has been removed.
	 */
	err = _mmc_detect_card_removed(host);

	mmc_put_card(host->card, NULL);

	if (err) {
		mmc_sd_remove(host);

		mmc_claim_host(host);
		mmc_detach_bus(host);
		mmc_power_off(host);
		mmc_release_host(host);
	}
}

static int sd_can_poweroff_notify(struct mmc_card *card)
{
	return card->ext_power.feature_support & SD_EXT_POWER_OFF_NOTIFY;
}

static int sd_busy_poweroff_notify_cb(void *cb_data, bool *busy)
{
	struct sd_busy_data *data = cb_data;
	struct mmc_card *card = data->card;
	int err;

	/*
	 * Read the status register for the power management function. It's at
	 * one byte offset and is one byte long. The Power Off Notification
	 * Ready is bit 0.
	 */
	err = mmc_sd_read_ext_reg(card, card->ext_power.fno, card->ext_power.page,
			      card->ext_power.offset + 1, 1, data->reg_buf);
	if (err) {
		pr_warn("%s: error %d reading status reg of PM func\n",
			mmc_hostname(card->host), err);
		return err;
	}

	*busy = !(data->reg_buf[0] & BIT(0));
	return 0;
}

static int sd_poweroff_notify(struct mmc_card *card)
{
	struct sd_busy_data cb_data;
	u8 *reg_buf;
	int err;

	reg_buf = kzalloc(512, GFP_KERNEL);
	if (!reg_buf)
		return -ENOMEM;

	/*
	 * Set the Power Off Notification bit in the power management settings
	 * register at 2 bytes offset.
	 */
	err = mmc_sd_write_ext_reg(card, card->ext_power.fno, card->ext_power.page,
			       card->ext_power.offset + 2, BIT(0));
	if (err) {
		pr_warn("%s: error %d writing Power Off Notify bit\n",
			mmc_hostname(card->host), err);
		goto out;
	}

	/* Find out when the command is completed. */
	err = mmc_poll_for_busy(card, SD_WRITE_EXTR_SINGLE_TIMEOUT_MS, false,
				MMC_BUSY_EXTR_SINGLE);
	if (err)
		goto out;

	cb_data.card = card;
	cb_data.reg_buf = reg_buf;
	err = __mmc_poll_for_busy(card->host, 0, SD_POWEROFF_NOTIFY_TIMEOUT_MS,
				  &sd_busy_poweroff_notify_cb, &cb_data);

out:
	kfree(reg_buf);
	return err;
}

static int _mmc_sd_suspend(struct mmc_host *host)
{
	struct mmc_card *card = host->card;
	int err = 0;

	mmc_claim_host(host);

	if (mmc_card_suspended(card))
		goto out;

	if (sd_can_poweroff_notify(card))
		err = sd_poweroff_notify(card);
	else if (!mmc_host_is_spi(host))
		err = mmc_deselect_cards(host);

	if (!err) {
		mmc_power_off(host);
		mmc_card_set_suspended(card);
	}

out:
	mmc_release_host(host);
	return err;
}

/*
 * Callback for suspend
 */
static int mmc_sd_suspend(struct mmc_host *host)
{
	int err;

	err = _mmc_sd_suspend(host);
	if (!err) {
		pm_runtime_disable(&host->card->dev);
		pm_runtime_set_suspended(&host->card->dev);
	}

	return err;
}

/*
 * This function tries to determine if the same card is still present
 * and, if so, restore all state to it.
 */
static int _mmc_sd_resume(struct mmc_host *host)
{
	int err = 0;

	mmc_claim_host(host);

	if (!mmc_card_suspended(host->card))
		goto out;

	mmc_power_up(host, host->card->ocr);
	err = mmc_sd_init_card(host, host->card->ocr, host->card);
	mmc_card_clr_suspended(host->card);

out:
	mmc_release_host(host);
	return err;
}

/*
 * Callback for resume
 */
static int mmc_sd_resume(struct mmc_host *host)
{
	pm_runtime_enable(&host->card->dev);
	return 0;
}

/*
 * Callback for runtime_suspend.
 */
static int mmc_sd_runtime_suspend(struct mmc_host *host)
{
	int err;

	if (!(host->caps & MMC_CAP_AGGRESSIVE_PM))
		return 0;

	err = _mmc_sd_suspend(host);
	if (err)
		pr_err("%s: error %d doing aggressive suspend\n",
			mmc_hostname(host), err);

	return err;
}

/*
 * Callback for runtime_resume.
 */
static int mmc_sd_runtime_resume(struct mmc_host *host)
{
	int err;

	err = _mmc_sd_resume(host);
	if (err && err != -ENOMEDIUM)
		pr_err("%s: error %d doing runtime resume\n",
			mmc_hostname(host), err);

	return 0;
}

static int mmc_sd_hw_reset(struct mmc_host *host)
{
	mmc_power_cycle(host, host->card->ocr);
	return mmc_sd_init_card(host, host->card->ocr, host->card);
}

static const struct mmc_bus_ops mmc_sd_ops = {
	.remove = mmc_sd_remove,
	.detect = mmc_sd_detect,
	.runtime_suspend = mmc_sd_runtime_suspend,
	.runtime_resume = mmc_sd_runtime_resume,
	.suspend = mmc_sd_suspend,
	.resume = mmc_sd_resume,
	.alive = mmc_sd_alive,
	.shutdown = mmc_sd_suspend,
	.hw_reset = mmc_sd_hw_reset,
	.cache_enabled = sd_cache_enabled,
	.flush_cache = sd_flush_cache,
};

/*
 * Starting point for SD card init.
 */
int mmc_attach_sd(struct mmc_host *host)
{
	int err;
	u32 ocr, rocr;

	WARN_ON(!host->claimed);

	err = mmc_send_app_op_cond(host, 0, &ocr);
	if (err)
		return err;

	mmc_attach_bus(host, &mmc_sd_ops);
	if (host->ocr_avail_sd)
		host->ocr_avail = host->ocr_avail_sd;

	/*
	 * We need to get OCR a different way for SPI.
	 */
	if (mmc_host_is_spi(host)) {
		mmc_go_idle(host);

		err = mmc_spi_read_ocr(host, 0, &ocr);
		if (err)
			goto err;
	}

	/*
	 * Some SD cards claims an out of spec VDD voltage range. Let's treat
	 * these bits as being in-valid and especially also bit7.
	 */
	ocr &= ~0x7FFF;

	rocr = mmc_select_voltage(host, ocr);

	/*
	 * Can we support the voltage(s) of the card(s)?
	 */
	if (!rocr) {
		err = -EINVAL;
		goto err;
	}

	/*
	 * Detect and init the card.
	 */
	err = mmc_sd_init_card(host, rocr, NULL);
	if (err)
		goto err;

	mmc_release_host(host);
	err = mmc_add_card(host->card);
	if (err)
		goto remove_card;

	mmc_claim_host(host);
	return 0;

remove_card:
	mmc_remove_card(host->card);
	host->card = NULL;
	mmc_claim_host(host);
err:
	mmc_detach_bus(host);

	pr_err("%s: error %d whilst initialising SD card\n",
		mmc_hostname(host), err);

	return err;
}
