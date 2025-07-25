// SPDX-License-Identifier: GPL-2.0
/*
 * PIO driver for RP1
 *
 * Copyright (C) 2023-2024 Raspberry Pi Ltd.
 *
 * Parts of this driver are based on:
 *  - vcio.c, by Noralf Trønnes
 *    Copyright (C) 2010 Broadcom
 *    Copyright (C) 2015 Noralf Trønnes
 *    Copyright (C) 2021 Raspberry Pi (Trading) Ltd.
 *  - bcm2835_smi.c & bcm2835_smi_dev.c by Luke Wren
 *    Copyright (c) 2015 Raspberry Pi (Trading) Ltd.
 */

#include <linux/cdev.h>
#include <linux/compat.h>
#include <linux/device.h>
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/ioctl.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pio_rp1.h>
#include <linux/platform_device.h>
#include <linux/rp1-firmware.h>
#include <linux/semaphore.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include <uapi/misc/rp1_pio_if.h>

#include "rp1-fw-pio.h"

#define DRIVER_NAME		"rp1-pio"

#define RP1_PIO_SMS_COUNT	4
#define RP1_PIO_INSTR_COUNT	32

#define MAX_ARG_SIZE		256

#define RP1_PIO_FIFO_TX0	0x00
#define RP1_PIO_FIFO_TX1	0x04
#define RP1_PIO_FIFO_TX2	0x08
#define RP1_PIO_FIFO_TX3	0x0c
#define RP1_PIO_FIFO_RX0	0x10
#define RP1_PIO_FIFO_RX1	0x14
#define RP1_PIO_FIFO_RX2	0x18
#define RP1_PIO_FIFO_RX3	0x1c

#define RP1_PIO_DMACTRL_DEFAULT	0x80000104

#define HANDLER(_n, _f) \
	[_IOC_NR(PIO_IOC_ ## _n)] = { #_n, rp1_pio_ ## _f, _IOC_SIZE(PIO_IOC_ ## _n) }


#define ROUND_UP(x, y) (((x) + (y) - 1) - (((x) + (y) - 1) % (y)))

#define DMA_BOUNCE_BUFFER_SIZE 0x1000
#define DMA_BOUNCE_BUFFER_COUNT 4

struct dma_xfer_state {
	struct dma_info *dma;
	void (*callback)(void *param);
	void *callback_param;
};

struct dma_buf_info {
	void *buf;
	dma_addr_t dma_addr;
	struct scatterlist sgl;
};

struct dma_info {
	struct semaphore buf_sem;
	struct dma_chan *chan;
	size_t buf_size;
	size_t buf_count;
	unsigned int head_idx;
	unsigned int tail_idx;
	struct dma_buf_info bufs[DMA_BOUNCE_BUFFER_COUNT];
};

struct rp1_pio_device {
	struct platform_device *pdev;
	struct rp1_firmware *fw;
	uint16_t fw_pio_base;
	uint16_t fw_pio_count;
	dev_t dev_num;
	struct class *dev_class;
	struct cdev cdev;
	phys_addr_t phys_addr;
	uint32_t claimed_sms;
	uint32_t claimed_dmas;
	spinlock_t lock;
	struct mutex instr_mutex;
	struct dma_info dma_configs[RP1_PIO_SMS_COUNT][RP1_PIO_DIR_COUNT];
	uint32_t used_instrs;
	uint8_t instr_refcounts[RP1_PIO_INSTR_COUNT];
	uint16_t instrs[RP1_PIO_INSTR_COUNT];
	uint client_count;
};

struct rp1_pio_client {
	struct rp1_pio_device *pio;
	uint32_t claimed_sms;
	uint32_t claimed_instrs;
	uint32_t claimed_dmas;
	int error;
};

static struct rp1_pio_device *g_pio;

static int rp1_pio_message(struct rp1_pio_device *pio,
			   uint16_t op, const void *data, unsigned int data_len)
{
	uint32_t rc;
	int ret;

	if (op >= pio->fw_pio_count)
		return -EOPNOTSUPP;
	ret = rp1_firmware_message(pio->fw, pio->fw_pio_base + op,
				   data, data_len,
				   &rc, sizeof(rc));
	if (ret == 4)
		ret = rc;
	return ret;
}

static int rp1_pio_message_resp(struct rp1_pio_device *pio,
				uint16_t op, const void *data, unsigned int data_len,
				void *resp, void __user *userbuf, unsigned int resp_len)
{
	uint32_t resp_buf[1 + 32];
	int ret;

	if (op >= pio->fw_pio_count)
		return -EOPNOTSUPP;
	if (resp_len + 4 >= sizeof(resp_buf))
		return -EINVAL;
	if (!resp && !userbuf)
		return -EINVAL;
	ret = rp1_firmware_message(pio->fw, pio->fw_pio_base + op,
				   data, data_len,
				   resp_buf, resp_len + 4);
	if (ret >= 4 && !resp_buf[0]) {
		ret -= 4;
		if (resp)
			memcpy(resp, &resp_buf[1], ret);
		else if (copy_to_user(userbuf, &resp_buf[1], ret))
			ret = -EFAULT;
	} else if (ret >= 0) {
		ret = -EIO;
	}
	return ret;
}

static int rp1_pio_read_hw(struct rp1_pio_client *client, void *param)
{
	struct rp1_pio_device *pio = client->pio;
	struct rp1_access_hw_args *args = param;

	return rp1_pio_message_resp(pio, READ_HW,
				    args, 8, NULL, args->data, args->len);
}

static int rp1_pio_write_hw(struct rp1_pio_client *client, void *param)
{
	struct rp1_pio_device *pio = client->pio;
	struct rp1_access_hw_args *args = param;
	uint32_t write_buf[32 + 1];
	int len;

	len = min(args->len, sizeof(write_buf) - 4);
	write_buf[0] = args->addr;
	if (copy_from_user(&write_buf[1], args->data, len))
		return -EFAULT;
	return rp1_firmware_message(pio->fw, pio->fw_pio_base + WRITE_HW,
				    write_buf, 4 + len, NULL, 0);
}

static int rp1_pio_find_program(struct rp1_pio_device *pio,
				struct rp1_pio_add_program_args *prog)
{
	uint start, end, prog_size;
	uint32_t used_mask;
	uint i;

	start = (prog->origin != RP1_PIO_ORIGIN_ANY) ? prog->origin : 0;
	end = (prog->origin != RP1_PIO_ORIGIN_ANY) ? prog->origin :
			(RP1_PIO_INSTRUCTION_COUNT - prog->num_instrs);
	prog_size = sizeof(prog->instrs[0]) * prog->num_instrs;
	used_mask = (uint32_t)(~0) >> (32 - prog->num_instrs);

	/* Find the best match */
	for (i = start; i <= end; i++) {
		uint32_t mask = used_mask << i;

		if ((pio->used_instrs & mask) != mask)
			continue;
		if (!memcmp(pio->instrs + i, prog->instrs, prog_size))
			return i;
	}

	return -1;
}

int rp1_pio_can_add_program(struct rp1_pio_client *client, void *param)
{
	struct rp1_pio_add_program_args *args = param;
	struct rp1_pio_device *pio = client->pio;
	int offset;

	if (args->num_instrs > RP1_PIO_INSTR_COUNT ||
		((args->origin != RP1_PIO_ORIGIN_ANY) &&
		 (args->origin >= RP1_PIO_INSTR_COUNT ||
		  ((args->origin + args->num_instrs) > RP1_PIO_INSTR_COUNT))))
		return -EINVAL;

	mutex_lock(&pio->instr_mutex);
	offset = rp1_pio_find_program(pio, args);
	mutex_unlock(&pio->instr_mutex);
	if (offset >= 0)
		return offset;

	/* Don't send the instructions, just the header */
	return rp1_pio_message(pio, PIO_CAN_ADD_PROGRAM, args,
			       offsetof(struct rp1_pio_add_program_args, instrs));
}
EXPORT_SYMBOL_GPL(rp1_pio_can_add_program);

int rp1_pio_add_program(struct rp1_pio_client *client, void *param)
{
	struct rp1_pio_add_program_args *args = param;
	struct rp1_pio_device *pio = client->pio;
	int offset;
	uint i;

	if (args->num_instrs > RP1_PIO_INSTR_COUNT ||
		((args->origin != RP1_PIO_ORIGIN_ANY) &&
		 (args->origin >= RP1_PIO_INSTR_COUNT ||
		  ((args->origin + args->num_instrs) > RP1_PIO_INSTR_COUNT))))
		return -EINVAL;

	mutex_lock(&pio->instr_mutex);
	offset = rp1_pio_find_program(pio, args);
	if (offset < 0)
		offset = rp1_pio_message(client->pio, PIO_ADD_PROGRAM, args, sizeof(*args));

	if (offset >= 0) {
		uint32_t used_mask;
		uint prog_size;

		used_mask = ((uint32_t)(~0) >> (-args->num_instrs & 0x1f)) << offset;
		prog_size = sizeof(args->instrs[0]) * args->num_instrs;

		if ((pio->used_instrs & used_mask) != used_mask) {
			pio->used_instrs |= used_mask;
			memcpy(pio->instrs + offset, args->instrs, prog_size);
		}
		client->claimed_instrs |= used_mask;
		for (i = 0; i < args->num_instrs; i++)
			pio->instr_refcounts[offset + i]++;
	}
	mutex_unlock(&pio->instr_mutex);
	return offset;
}
EXPORT_SYMBOL_GPL(rp1_pio_add_program);

static void rp1_pio_remove_instrs(struct rp1_pio_device *pio, uint32_t mask)
{
	struct rp1_pio_remove_program_args args;
	uint i;

	mutex_lock(&pio->instr_mutex);
	args.num_instrs = 0;
	for (i = 0; ; i++, mask >>= 1) {
		if ((mask & 1) && pio->instr_refcounts[i] && !--pio->instr_refcounts[i]) {
			pio->used_instrs &= ~(1 << i);
			args.num_instrs++;
		} else if (args.num_instrs) {
			args.origin = i - args.num_instrs;
			rp1_pio_message(pio, PIO_REMOVE_PROGRAM, &args, sizeof(args));
			args.num_instrs = 0;
		}
		if (!mask)
			break;
	}
	mutex_unlock(&pio->instr_mutex);
}

int rp1_pio_remove_program(struct rp1_pio_client *client, void *param)
{
	struct rp1_pio_remove_program_args *args = param;
	uint32_t used_mask;
	int ret = -ENOENT;

	if (args->num_instrs > RP1_PIO_INSTR_COUNT ||
		args->origin >= RP1_PIO_INSTR_COUNT ||
		(args->origin + args->num_instrs) > RP1_PIO_INSTR_COUNT)
		return -EINVAL;

	used_mask = ((uint32_t)(~0) >> (32 - args->num_instrs)) << args->origin;
	if ((client->claimed_instrs & used_mask) == used_mask) {
		client->claimed_instrs &= ~used_mask;
		rp1_pio_remove_instrs(client->pio, used_mask);
		ret = 0;
	}
	return ret;
}
EXPORT_SYMBOL_GPL(rp1_pio_remove_program);

int rp1_pio_clear_instr_mem(struct rp1_pio_client *client, void *param)
{
	struct rp1_pio_device *pio = client->pio;

	mutex_lock(&pio->instr_mutex);
	(void)rp1_pio_message(client->pio, PIO_CLEAR_INSTR_MEM, NULL, 0);
	memset(pio->instr_refcounts, 0, sizeof(pio->instr_refcounts));
	pio->used_instrs = 0;
	client->claimed_instrs = 0;
	mutex_unlock(&pio->instr_mutex);
	return 0;
}
EXPORT_SYMBOL_GPL(rp1_pio_clear_instr_mem);

int rp1_pio_sm_claim(struct rp1_pio_client *client, void *param)
{
	struct rp1_pio_sm_claim_args *args = param;
	struct rp1_pio_device *pio = client->pio;
	int ret;

	mutex_lock(&pio->instr_mutex);
	ret = rp1_pio_message(client->pio, PIO_SM_CLAIM, args, sizeof(*args));
	if (ret >= 0) {
		if (args->mask)
			client->claimed_sms |= args->mask;
		else
			client->claimed_sms |= (1 << ret);
		pio->claimed_sms |= client->claimed_sms;
	}
	mutex_unlock(&pio->instr_mutex);
	return ret;
}
EXPORT_SYMBOL_GPL(rp1_pio_sm_claim);

int rp1_pio_sm_unclaim(struct rp1_pio_client *client, void *param)
{
	struct rp1_pio_sm_claim_args *args = param;
	struct rp1_pio_device *pio = client->pio;

	mutex_lock(&pio->instr_mutex);
	(void)rp1_pio_message(client->pio, PIO_SM_UNCLAIM, args, sizeof(*args));
	client->claimed_sms &= ~args->mask;
	pio->claimed_sms &= ~args->mask;
	mutex_unlock(&pio->instr_mutex);
	return 0;
}
EXPORT_SYMBOL_GPL(rp1_pio_sm_unclaim);

int rp1_pio_sm_is_claimed(struct rp1_pio_client *client, void *param)
{
	struct rp1_pio_sm_claim_args *args = param;

	return rp1_pio_message(client->pio, PIO_SM_IS_CLAIMED, args, sizeof(*args));
}
EXPORT_SYMBOL_GPL(rp1_pio_sm_is_claimed);

int rp1_pio_sm_init(struct rp1_pio_client *client, void *param)
{
	struct rp1_pio_sm_init_args *args = param;

	return rp1_pio_message(client->pio, PIO_SM_INIT, args, sizeof(*args));
}
EXPORT_SYMBOL_GPL(rp1_pio_sm_init);

int rp1_pio_sm_set_config(struct rp1_pio_client *client, void *param)
{
	struct rp1_pio_sm_set_config_args *args = param;

	return rp1_pio_message(client->pio, PIO_SM_SET_CONFIG, args, sizeof(*args));
}
EXPORT_SYMBOL_GPL(rp1_pio_sm_set_config);

int rp1_pio_sm_exec(struct rp1_pio_client *client, void *param)
{
	struct rp1_pio_sm_exec_args *args = param;

	return rp1_pio_message(client->pio, PIO_SM_EXEC, args, sizeof(*args));
}
EXPORT_SYMBOL_GPL(rp1_pio_sm_exec);

int rp1_pio_sm_clear_fifos(struct rp1_pio_client *client, void *param)
{
	struct rp1_pio_sm_clear_fifos_args *args = param;

	return rp1_pio_message(client->pio, PIO_SM_CLEAR_FIFOS, args, sizeof(*args));
}
EXPORT_SYMBOL_GPL(rp1_pio_sm_clear_fifos);

int rp1_pio_sm_set_clkdiv(struct rp1_pio_client *client, void *param)
{
	struct rp1_pio_sm_set_clkdiv_args *args = param;

	return rp1_pio_message(client->pio, PIO_SM_SET_CLKDIV, args, sizeof(*args));
}
EXPORT_SYMBOL_GPL(rp1_pio_sm_set_clkdiv);

int rp1_pio_sm_set_pins(struct rp1_pio_client *client, void *param)
{
	struct rp1_pio_sm_set_pins_args *args = param;

	return rp1_pio_message(client->pio, PIO_SM_SET_PINS, args, sizeof(*args));
}
EXPORT_SYMBOL_GPL(rp1_pio_sm_set_pins);

int rp1_pio_sm_set_pindirs(struct rp1_pio_client *client, void *param)
{
	struct rp1_pio_sm_set_pindirs_args *args = param;

	return rp1_pio_message(client->pio, PIO_SM_SET_PINDIRS, args, sizeof(*args));
}
EXPORT_SYMBOL_GPL(rp1_pio_sm_set_pindirs);

int rp1_pio_sm_set_enabled(struct rp1_pio_client *client, void *param)
{
	struct rp1_pio_sm_set_enabled_args *args = param;

	return rp1_pio_message(client->pio, PIO_SM_SET_ENABLED, args, sizeof(*args));
}
EXPORT_SYMBOL_GPL(rp1_pio_sm_set_enabled);

int rp1_pio_sm_restart(struct rp1_pio_client *client, void *param)
{
	struct rp1_pio_sm_restart_args *args = param;

	return rp1_pio_message(client->pio, PIO_SM_RESTART, args, sizeof(*args));
}
EXPORT_SYMBOL_GPL(rp1_pio_sm_restart);

int rp1_pio_sm_clkdiv_restart(struct rp1_pio_client *client, void *param)
{
	struct rp1_pio_sm_restart_args *args = param;

	return rp1_pio_message(client->pio, PIO_SM_CLKDIV_RESTART, args, sizeof(*args));
}
EXPORT_SYMBOL_GPL(rp1_pio_sm_clkdiv_restart);

int rp1_pio_sm_enable_sync(struct rp1_pio_client *client, void *param)
{
	struct rp1_pio_sm_enable_sync_args *args = param;

	return rp1_pio_message(client->pio, PIO_SM_ENABLE_SYNC, args, sizeof(*args));
}
EXPORT_SYMBOL_GPL(rp1_pio_sm_enable_sync);

int rp1_pio_sm_put(struct rp1_pio_client *client, void *param)
{
	struct rp1_pio_sm_put_args *args = param;

	return rp1_pio_message(client->pio, PIO_SM_PUT, args, sizeof(*args));
}
EXPORT_SYMBOL_GPL(rp1_pio_sm_put);

int rp1_pio_sm_get(struct rp1_pio_client *client, void *param)
{
	struct rp1_pio_sm_get_args *args = param;
	int ret;

	ret = rp1_pio_message_resp(client->pio, PIO_SM_GET, args, sizeof(*args),
				   &args->data, NULL, sizeof(args->data));
	if (ret >= 0)
		return offsetof(struct rp1_pio_sm_get_args, data) + ret;
	return ret;
}
EXPORT_SYMBOL_GPL(rp1_pio_sm_get);

int rp1_pio_sm_set_dmactrl(struct rp1_pio_client *client, void *param)
{
	struct rp1_pio_sm_set_dmactrl_args *args = param;

	return rp1_pio_message(client->pio, PIO_SM_SET_DMACTRL, args, sizeof(*args));
}
EXPORT_SYMBOL_GPL(rp1_pio_sm_set_dmactrl);

int rp1_pio_sm_fifo_state(struct rp1_pio_client *client, void *param)
{
	struct rp1_pio_sm_fifo_state_args *args = param;
	const int level_offset = offsetof(struct rp1_pio_sm_fifo_state_args, level);
	int ret;

	ret = rp1_pio_message_resp(client->pio, PIO_SM_FIFO_STATE, args, sizeof(*args),
				   &args->level, NULL, sizeof(*args) - level_offset);
	if (ret >= 0)
		return level_offset + ret;
	return ret;
}
EXPORT_SYMBOL_GPL(rp1_pio_sm_fifo_state);

int rp1_pio_sm_drain_tx(struct rp1_pio_client *client, void *param)
{
	struct rp1_pio_sm_clear_fifos_args *args = param;

	return rp1_pio_message(client->pio, PIO_SM_DRAIN_TX, args, sizeof(*args));
}
EXPORT_SYMBOL_GPL(rp1_pio_sm_drain_tx);

int rp1_pio_gpio_init(struct rp1_pio_client *client, void *param)
{
	struct rp1_gpio_init_args *args = param;

	return rp1_pio_message(client->pio, GPIO_INIT, args, sizeof(*args));
}
EXPORT_SYMBOL_GPL(rp1_pio_gpio_init);

int rp1_pio_gpio_set_function(struct rp1_pio_client *client, void *param)
{
	struct rp1_gpio_set_function_args *args = param;

	return rp1_pio_message(client->pio, GPIO_SET_FUNCTION, args, sizeof(*args));
}
EXPORT_SYMBOL_GPL(rp1_pio_gpio_set_function);

int rp1_pio_gpio_set_pulls(struct rp1_pio_client *client, void *param)
{
	struct rp1_gpio_set_pulls_args *args = param;

	return rp1_pio_message(client->pio, GPIO_SET_PULLS, args, sizeof(*args));
}
EXPORT_SYMBOL_GPL(rp1_pio_gpio_set_pulls);

int rp1_pio_gpio_set_outover(struct rp1_pio_client *client, void *param)
{
	struct rp1_gpio_set_args *args = param;

	return rp1_pio_message(client->pio, GPIO_SET_OUTOVER, args, sizeof(*args));
}
EXPORT_SYMBOL_GPL(rp1_pio_gpio_set_outover);

int rp1_pio_gpio_set_inover(struct rp1_pio_client *client, void *param)
{
	struct rp1_gpio_set_args *args = param;

	return rp1_pio_message(client->pio, GPIO_SET_INOVER, args, sizeof(*args));
}
EXPORT_SYMBOL_GPL(rp1_pio_gpio_set_inover);

int rp1_pio_gpio_set_oeover(struct rp1_pio_client *client, void *param)
{
	struct rp1_gpio_set_args *args = param;

	return rp1_pio_message(client->pio, GPIO_SET_OEOVER, args, sizeof(*args));
}
EXPORT_SYMBOL_GPL(rp1_pio_gpio_set_oeover);

int rp1_pio_gpio_set_input_enabled(struct rp1_pio_client *client, void *param)
{
	struct rp1_gpio_set_args *args = param;

	return rp1_pio_message(client->pio, GPIO_SET_INPUT_ENABLED, args, sizeof(*args));
}
EXPORT_SYMBOL_GPL(rp1_pio_gpio_set_input_enabled);

int rp1_pio_gpio_set_drive_strength(struct rp1_pio_client *client, void *param)
{
	struct rp1_gpio_set_args *args = param;

	return rp1_pio_message(client->pio, GPIO_SET_DRIVE_STRENGTH, args, sizeof(*args));
}
EXPORT_SYMBOL_GPL(rp1_pio_gpio_set_drive_strength);

static void rp1_pio_sm_dma_callback(void *param)
{
	struct dma_info *dma = param;

	up(&dma->buf_sem);
}

static void rp1_pio_sm_kernel_dma_callback(void *param)
{
	struct dma_xfer_state *dxs = param;

	dxs->dma->tail_idx++;
	up(&dxs->dma->buf_sem);

	dxs->callback(dxs->callback_param);

	kfree(dxs);
}

static void rp1_pio_sm_dma_free(struct device *dev, struct dma_info *dma)
{
	dmaengine_terminate_all(dma->chan);
	while (dma->buf_count > 0) {
		dma->buf_count--;
		dma_free_coherent(dev, ROUND_UP(dma->buf_size, PAGE_SIZE),
				  dma->bufs[dma->buf_count].buf,
				  dma->bufs[dma->buf_count].dma_addr);
	}

	dma_release_channel(dma->chan);
}

static int rp1_pio_sm_config_xfer_internal(struct rp1_pio_client *client, uint sm, uint dir,
					   uint buf_size, uint buf_count)
{
	struct rp1_pio_sm_set_dmactrl_args set_dmactrl_args;
	struct rp1_pio_device *pio = client->pio;
	struct platform_device *pdev = pio->pdev;
	struct device *dev = &pdev->dev;
	struct dma_slave_config config = {};
	phys_addr_t fifo_addr;
	struct dma_info *dma;
	uint32_t dma_mask;
	char chan_name[4];
	int ret = 0;

	if (sm >= RP1_PIO_SMS_COUNT || dir >= RP1_PIO_DIR_COUNT)
		return -EINVAL;
	if ((buf_count || buf_size) &&
	    (!buf_size || (buf_size & 3) ||
	     !buf_count || buf_count > DMA_BOUNCE_BUFFER_COUNT))
		return -EINVAL;

	dma_mask = 1 << (sm * 2 + dir);

	dma = &pio->dma_configs[sm][dir];

	spin_lock(&pio->lock);
	if (pio->claimed_dmas & dma_mask)
		rp1_pio_sm_dma_free(dev, dma);
	pio->claimed_dmas |= dma_mask;
	client->claimed_dmas |= dma_mask;
	spin_unlock(&pio->lock);

	dma->buf_size = buf_size;
	/* Round up the allocations */
	buf_size = ROUND_UP(buf_size, PAGE_SIZE);
	sema_init(&dma->buf_sem, 0);

	/* Allocate and configure a DMA channel */
	/* Careful - each SM FIFO has its own DREQ value */
	chan_name[0] = (dir == RP1_PIO_DIR_TO_SM) ? 't' : 'r';
	chan_name[1] = 'x';
	chan_name[2] = '0' + sm;
	chan_name[3] = '\0';

	dma->chan = dma_request_chan(dev, chan_name);
	if (IS_ERR(dma->chan))
		return PTR_ERR(dma->chan);

	/* Alloc and map bounce buffers */
	for (dma->buf_count = 0; dma->buf_count < buf_count; dma->buf_count++) {
		struct dma_buf_info *dbi = &dma->bufs[dma->buf_count];

		dbi->buf = dma_alloc_coherent(dma->chan->device->dev, buf_size,
					      &dbi->dma_addr, GFP_KERNEL);
		if (!dbi->buf) {
			ret = -ENOMEM;
			goto err_dma_free;
		}
		sg_init_table(&dbi->sgl, 1);
		sg_dma_address(&dbi->sgl) = dbi->dma_addr;
	}

	fifo_addr = pio->phys_addr;
	fifo_addr += sm * (RP1_PIO_FIFO_TX1 - RP1_PIO_FIFO_TX0);
	fifo_addr += (dir == RP1_PIO_DIR_TO_SM) ? RP1_PIO_FIFO_TX0 : RP1_PIO_FIFO_RX0;

	config.src_addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES;
	config.dst_addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES;
	config.src_addr = fifo_addr;
	config.dst_addr = fifo_addr;
	config.direction = (dir == RP1_PIO_DIR_TO_SM) ? DMA_MEM_TO_DEV : DMA_DEV_TO_MEM;

	ret = dmaengine_slave_config(dma->chan, &config);
	if (ret)
		goto err_dma_free;

	set_dmactrl_args.sm = sm;
	set_dmactrl_args.is_tx = (dir == RP1_PIO_DIR_TO_SM);
	set_dmactrl_args.ctrl = RP1_PIO_DMACTRL_DEFAULT;
	if (dir == RP1_PIO_DIR_FROM_SM)
		set_dmactrl_args.ctrl = (RP1_PIO_DMACTRL_DEFAULT & ~0x1f) | 1;

	ret = rp1_pio_sm_set_dmactrl(client, &set_dmactrl_args);
	if (ret)
		goto err_dma_free;

	return 0;

err_dma_free:
	rp1_pio_sm_dma_free(dev, dma);

	spin_lock(&pio->lock);
	client->claimed_dmas &= ~dma_mask;
	pio->claimed_dmas &= ~dma_mask;
	spin_unlock(&pio->lock);

	return ret;
}

static int rp1_pio_sm_config_xfer_user(struct rp1_pio_client *client, void *param)
{
	struct rp1_pio_sm_config_xfer_args *args = param;

	return rp1_pio_sm_config_xfer_internal(client, args->sm, args->dir,
					       args->buf_size, args->buf_count);
}

static int rp1_pio_sm_config_xfer32_user(struct rp1_pio_client *client, void *param)
{
	struct rp1_pio_sm_config_xfer32_args *args = param;

	return rp1_pio_sm_config_xfer_internal(client, args->sm, args->dir,
					       args->buf_size, args->buf_count);
}

static int rp1_pio_sm_tx_user(struct rp1_pio_device *pio, struct dma_info *dma,
				  const void __user *userbuf, size_t bytes)
{
	struct platform_device *pdev = pio->pdev;
	struct dma_async_tx_descriptor *desc;
	struct device *dev = &pdev->dev;
	int ret = 0;

	/* Clean the slate - we're running synchronously */
	dma->head_idx = 0;
	dma->tail_idx = 0;

	while (bytes > 0) {
		size_t copy_bytes = min(bytes, dma->buf_size);
		struct dma_buf_info *dbi;

		/* grab the next free buffer, waiting if they're all full */
		if (dma->head_idx - dma->tail_idx == dma->buf_count) {
			if (down_timeout(&dma->buf_sem,
				msecs_to_jiffies(1000))) {
				dev_err(dev, "DMA bounce timed out\n");
				break;
			}
			dma->tail_idx++;
		}

		dbi = &dma->bufs[dma->head_idx % dma->buf_count];

		sg_dma_len(&dbi->sgl) = copy_bytes;

		ret = copy_from_user(dbi->buf, userbuf, copy_bytes);
		if (ret < 0)
			break;

		userbuf += copy_bytes;

		desc = dmaengine_prep_slave_sg(dma->chan, &dbi->sgl, 1,
					       DMA_MEM_TO_DEV,
					       DMA_PREP_INTERRUPT | DMA_CTRL_ACK |
					       DMA_PREP_FENCE);
		if (!desc) {
			dev_err(dev, "DMA preparation failed\n");
			ret = -EIO;
			break;
		}

		desc->callback = rp1_pio_sm_dma_callback;
		desc->callback_param = dma;

		/* Submit the buffer - the callback will kick the semaphore */
		ret = dmaengine_submit(desc);
		if (ret < 0)
			break;
		ret = 0;

		dma_async_issue_pending(dma->chan);

		dma->head_idx++;
		bytes -= copy_bytes;
	}

	/* Block for completion */
	while (dma->tail_idx != dma->head_idx) {
		if (down_timeout(&dma->buf_sem, msecs_to_jiffies(1000))) {
			dev_err(dev, "DMA wait timed out\n");
			ret = -ETIMEDOUT;
			break;
		}
		dma->tail_idx++;
	}

	return ret;
}

static int rp1_pio_sm_rx_user(struct rp1_pio_device *pio, struct dma_info *dma,
				  void __user *userbuf, size_t bytes)
{
	struct platform_device *pdev = pio->pdev;
	struct dma_async_tx_descriptor *desc;
	struct device *dev = &pdev->dev;
	int ret = 0;

	/* Clean the slate - we're running synchronously */
	dma->head_idx = 0;
	dma->tail_idx = 0;

	while (bytes || dma->tail_idx != dma->head_idx) {
		size_t copy_bytes = min(bytes, dma->buf_size);
		struct dma_buf_info *dbi;

		/*
		 * wait for the next RX to complete if all the buffers are
		 * outstanding or we're finishing up.
		 */
		if (!bytes || dma->head_idx - dma->tail_idx == dma->buf_count) {
			if (down_timeout(&dma->buf_sem,
				msecs_to_jiffies(1000))) {
				dev_err(dev, "DMA wait timed out\n");
				ret = -ETIMEDOUT;
				break;
			}

			dbi = &dma->bufs[dma->tail_idx++ % dma->buf_count];
			ret = copy_to_user(userbuf, dbi->buf, sg_dma_len(&dbi->sgl));
			if (ret < 0)
				break;
			userbuf += sg_dma_len(&dbi->sgl);

			if (!bytes)
				continue;
		}

		dbi = &dma->bufs[dma->head_idx % dma->buf_count];
		sg_dma_len(&dbi->sgl) = copy_bytes;
		desc = dmaengine_prep_slave_sg(dma->chan, &dbi->sgl, 1,
					       DMA_DEV_TO_MEM,
					       DMA_PREP_INTERRUPT | DMA_CTRL_ACK |
					       DMA_PREP_FENCE);
		if (!desc) {
			dev_err(dev, "DMA preparation failed\n");
			ret = -EIO;
			break;
		}

		desc->callback = rp1_pio_sm_dma_callback;
		desc->callback_param = dma;

		/* Submit the buffer - the callback will kick the semaphore */
		ret = dmaengine_submit(desc);
		if (ret < 0)
			break;

		dma->head_idx++;
		dma_async_issue_pending(dma->chan);

		bytes -= copy_bytes;
	}

	return ret;
}

static int rp1_pio_sm_xfer_data32_user(struct rp1_pio_client *client, void *param)
{
	struct rp1_pio_sm_xfer_data32_args *args = param;
	struct rp1_pio_device *pio = client->pio;
	struct dma_info *dma;

	if (args->sm >= RP1_PIO_SMS_COUNT || args->dir >= RP1_PIO_DIR_COUNT ||
	    !args->data_bytes || !args->data)
		return -EINVAL;

	dma = &pio->dma_configs[args->sm][args->dir];

	if (args->dir == RP1_PIO_DIR_TO_SM)
		return rp1_pio_sm_tx_user(pio, dma, args->data, args->data_bytes);
	else
		return rp1_pio_sm_rx_user(pio, dma, args->data, args->data_bytes);
}

static int rp1_pio_sm_xfer_data_user(struct rp1_pio_client *client, void *param)
{
	struct rp1_pio_sm_xfer_data_args *args = param;
	struct rp1_pio_sm_xfer_data32_args args32;

	args32.sm = args->sm;
	args32.dir = args->dir;
	args32.data_bytes = args->data_bytes;
	args32.data = args->data;

	return rp1_pio_sm_xfer_data32_user(client, &args32);
}

int rp1_pio_sm_config_xfer(struct rp1_pio_client *client, uint sm, uint dir,
			      uint buf_size, uint buf_count)
{
	return rp1_pio_sm_config_xfer_internal(client, sm, dir, buf_size, buf_count);
}
EXPORT_SYMBOL_GPL(rp1_pio_sm_config_xfer);

int rp1_pio_sm_xfer_data(struct rp1_pio_client *client, uint sm, uint dir,
				    uint data_bytes, void *data, dma_addr_t dma_addr,
				    void (*callback)(void *param), void *param)
{
	struct rp1_pio_device *pio = client->pio;
	struct platform_device *pdev = pio->pdev;
	struct dma_async_tx_descriptor *desc;
	struct dma_xfer_state *dxs = NULL;
	struct device *dev = &pdev->dev;
	struct dma_buf_info *dbi = NULL;
	struct scatterlist sg;
	struct dma_info *dma;
	int ret = 0;

	if (sm >= RP1_PIO_SMS_COUNT || dir >= RP1_PIO_DIR_COUNT)
		return -EINVAL;

	dma = &pio->dma_configs[sm][dir];

	if (!dma_addr) {
		dxs = kmalloc(sizeof(*dxs), GFP_KERNEL);
		dxs->dma = dma;
		dxs->callback = callback;
		dxs->callback_param = param;
		callback = rp1_pio_sm_kernel_dma_callback;
		param = dxs;

		if (!dma->buf_count || data_bytes > dma->buf_size)
			return -EINVAL;

		/* Grab a dma buffer */
		if (dma->head_idx - dma->tail_idx == dma->buf_count) {
			if (down_timeout(&dma->buf_sem, msecs_to_jiffies(1000))) {
				dev_err(dev, "DMA wait timed out\n");
				return -ETIMEDOUT;
			}
		}

		dbi = &dma->bufs[dma->head_idx % dma->buf_count];
		dma_addr = dbi->dma_addr;

		if (dir == PIO_DIR_TO_SM)
			memcpy(dbi->buf, data, data_bytes);
	}

	sg_init_table(&sg, 1);
	sg_dma_address(&sg) = dma_addr;
	sg_dma_len(&sg) = data_bytes;

	desc = dmaengine_prep_slave_sg(dma->chan, &sg, 1,
					   (dir == PIO_DIR_TO_SM) ? DMA_MEM_TO_DEV : DMA_DEV_TO_MEM,
					   DMA_PREP_INTERRUPT | DMA_CTRL_ACK |
					   DMA_PREP_FENCE);
	if (!desc) {
		dev_err(dev, "DMA preparation failed\n");
		return -EIO;
	}

	desc->callback = callback;
	desc->callback_param = param;

	ret = dmaengine_submit(desc);
	if (ret < 0) {
		dev_err(dev, "dmaengine_submit failed (%d)\n", ret);
		return ret;
	}

	dma->head_idx++;
	dma_async_issue_pending(dma->chan);

	return 0;
}
EXPORT_SYMBOL_GPL(rp1_pio_sm_xfer_data);

struct handler_info {
	const char *name;
	int (*func)(struct rp1_pio_client *client, void *param);
	int argsize;
} ioctl_handlers[] = {
	HANDLER(SM_CONFIG_XFER, sm_config_xfer_user),
	HANDLER(SM_XFER_DATA, sm_xfer_data_user),
	HANDLER(SM_XFER_DATA32, sm_xfer_data32_user),
	HANDLER(SM_CONFIG_XFER32, sm_config_xfer32_user),

	HANDLER(CAN_ADD_PROGRAM, can_add_program),
	HANDLER(ADD_PROGRAM, add_program),
	HANDLER(REMOVE_PROGRAM, remove_program),
	HANDLER(CLEAR_INSTR_MEM, clear_instr_mem),

	HANDLER(SM_CLAIM, sm_claim),
	HANDLER(SM_UNCLAIM, sm_unclaim),
	HANDLER(SM_IS_CLAIMED, sm_is_claimed),

	HANDLER(SM_INIT, sm_init),
	HANDLER(SM_SET_CONFIG, sm_set_config),
	HANDLER(SM_EXEC, sm_exec),
	HANDLER(SM_CLEAR_FIFOS, sm_clear_fifos),
	HANDLER(SM_SET_CLKDIV, sm_set_clkdiv),
	HANDLER(SM_SET_PINS, sm_set_pins),
	HANDLER(SM_SET_PINDIRS, sm_set_pindirs),
	HANDLER(SM_SET_ENABLED, sm_set_enabled),
	HANDLER(SM_RESTART, sm_restart),
	HANDLER(SM_CLKDIV_RESTART, sm_clkdiv_restart),
	HANDLER(SM_ENABLE_SYNC, sm_enable_sync),
	HANDLER(SM_PUT, sm_put),
	HANDLER(SM_GET, sm_get),
	HANDLER(SM_SET_DMACTRL, sm_set_dmactrl),
	HANDLER(SM_FIFO_STATE, sm_fifo_state),
	HANDLER(SM_DRAIN_TX, sm_drain_tx),

	HANDLER(GPIO_INIT, gpio_init),
	HANDLER(GPIO_SET_FUNCTION, gpio_set_function),
	HANDLER(GPIO_SET_PULLS, gpio_set_pulls),
	HANDLER(GPIO_SET_OUTOVER, gpio_set_outover),
	HANDLER(GPIO_SET_INOVER, gpio_set_inover),
	HANDLER(GPIO_SET_OEOVER, gpio_set_oeover),
	HANDLER(GPIO_SET_INPUT_ENABLED, gpio_set_input_enabled),
	HANDLER(GPIO_SET_DRIVE_STRENGTH, gpio_set_drive_strength),

	HANDLER(READ_HW, read_hw),
	HANDLER(WRITE_HW, write_hw),
};

struct rp1_pio_client *rp1_pio_open(void)
{
	struct rp1_pio_client *client;

	if (!g_pio)
		return ERR_PTR(-ENOENT);

	client = kzalloc(sizeof(*client), GFP_KERNEL);
	if (!client)
		return ERR_PTR(-ENOMEM);

	client->pio = g_pio;

	return client;
}
EXPORT_SYMBOL_GPL(rp1_pio_open);

void rp1_pio_close(struct rp1_pio_client *client)
{
	struct rp1_pio_device *pio = client->pio;
	uint claimed_dmas = client->claimed_dmas;
	int i;

	/* Free any allocated resources */

	for (i = 0; claimed_dmas; i++) {
		uint mask = (1 << i);

		if (claimed_dmas & mask) {
			struct dma_info *dma = &pio->dma_configs[i >> 1][i & 1];

			claimed_dmas &= ~mask;
			rp1_pio_sm_dma_free(&pio->pdev->dev, dma);
		}
	}

	spin_lock(&pio->lock);
	pio->claimed_dmas &= ~client->claimed_dmas;
	spin_unlock(&pio->lock);

	if (client->claimed_sms) {
		struct rp1_pio_sm_set_enabled_args se_args = {
			.mask = client->claimed_sms, .enable = 0
		};
		struct rp1_pio_sm_claim_args uc_args = {
			.mask = client->claimed_sms
		};

		rp1_pio_sm_set_enabled(client, &se_args);
		rp1_pio_sm_unclaim(client, &uc_args);
	}

	if (client->claimed_instrs)
		rp1_pio_remove_instrs(pio, client->claimed_instrs);

	/* Reinitialise the SM? */

	kfree(client);
}
EXPORT_SYMBOL_GPL(rp1_pio_close);

void rp1_pio_set_error(struct rp1_pio_client *client, int err)
{
	client->error = err;
}
EXPORT_SYMBOL_GPL(rp1_pio_set_error);

int rp1_pio_get_error(const struct rp1_pio_client *client)
{
	return client->error;
}
EXPORT_SYMBOL_GPL(rp1_pio_get_error);

void rp1_pio_clear_error(struct rp1_pio_client *client)
{
	client->error = 0;
}
EXPORT_SYMBOL_GPL(rp1_pio_clear_error);

static int rp1_pio_file_open(struct inode *inode, struct file *filp)
{
	struct rp1_pio_client *client;

	client = rp1_pio_open();
	if (IS_ERR(client))
		return PTR_ERR(client);

	filp->private_data = client;

	return 0;
}

static int rp1_pio_file_release(struct inode *inode, struct file *filp)
{
	struct rp1_pio_client *client = filp->private_data;

	rp1_pio_close(client);

	return 0;
}

static long rp1_pio_ioctl(struct file *filp, unsigned int ioctl_num,
			  unsigned long ioctl_param)
{
	struct rp1_pio_client *client = filp->private_data;
	struct device *dev = &client->pio->pdev->dev;
	void __user *argp = (void __user *)ioctl_param;
	int nr = _IOC_NR(ioctl_num);
	int sz = _IOC_SIZE(ioctl_num);
	struct handler_info *hdlr = &ioctl_handlers[nr];
	uint32_t argbuf[MAX_ARG_SIZE/sizeof(uint32_t)];
	int ret;

	if (nr >= ARRAY_SIZE(ioctl_handlers) || !hdlr->func) {
		dev_err(dev, "unknown ioctl: %x\n", ioctl_num);
		return -EOPNOTSUPP;
	}

	if (sz != hdlr->argsize) {
		dev_err(dev, "wrong %s argsize (expected %d, got %d)\n",
			hdlr->name, hdlr->argsize, sz);
		return -EINVAL;
	}

	if (copy_from_user(argbuf, argp, sz))
		return -EFAULT;

	ret = (hdlr->func)(client, argbuf);
	dev_dbg(dev, "%s: %s -> %d\n", __func__, hdlr->name, ret);
	if (ret > 0) {
		if (copy_to_user(argp, argbuf, ret))
			ret = -EFAULT;
	}

	return ret;
}

#ifdef CONFIG_COMPAT

struct rp1_pio_sm_xfer_data_args_compat {
	uint16_t sm;
	uint16_t dir;
	uint16_t data_bytes;
	compat_uptr_t data;
};

struct rp1_pio_sm_xfer_data32_args_compat {
	uint16_t sm;
	uint16_t dir;
	uint32_t data_bytes;
	compat_uptr_t data;
};

struct rp1_access_hw_args_compat {
	uint32_t addr;
	uint32_t len;
	compat_uptr_t data;
};

#define PIO_IOC_SM_XFER_DATA_COMPAT \
	_IOW(PIO_IOC_MAGIC, 1, struct rp1_pio_sm_xfer_data_args_compat)
#define PIO_IOC_SM_XFER_DATA32_COMPAT \
	_IOW(PIO_IOC_MAGIC, 2, struct rp1_pio_sm_xfer_data32_args_compat)
#define PIO_IOC_READ_HW_COMPAT _IOW(PIO_IOC_MAGIC, 8, struct rp1_access_hw_args_compat)
#define PIO_IOC_WRITE_HW_COMPAT _IOW(PIO_IOC_MAGIC, 9, struct rp1_access_hw_args_compat)

static long rp1_pio_compat_ioctl(struct file *filp, unsigned int ioctl_num,
				 unsigned long ioctl_param)
{
	struct rp1_pio_client *client = filp->private_data;

	switch (ioctl_num) {
	case PIO_IOC_SM_XFER_DATA_COMPAT:
	{
		struct rp1_pio_sm_xfer_data_args_compat compat_param;
		struct rp1_pio_sm_xfer_data_args param;

		if (copy_from_user(&compat_param, compat_ptr(ioctl_param), sizeof(compat_param)))
			return -EFAULT;
		param.sm = compat_param.sm;
		param.dir = compat_param.dir;
		param.data_bytes = compat_param.data_bytes;
		param.data = compat_ptr(compat_param.data);
		return rp1_pio_sm_xfer_data_user(client, &param);
	}
	case PIO_IOC_SM_XFER_DATA32_COMPAT:
	{
		struct rp1_pio_sm_xfer_data32_args_compat compat_param;
		struct rp1_pio_sm_xfer_data32_args param;

		if (copy_from_user(&compat_param, compat_ptr(ioctl_param), sizeof(compat_param)))
			return -EFAULT;
		param.sm = compat_param.sm;
		param.dir = compat_param.dir;
		param.data_bytes = compat_param.data_bytes;
		param.data = compat_ptr(compat_param.data);
		return rp1_pio_sm_xfer_data32_user(client, &param);
	}

	case PIO_IOC_READ_HW_COMPAT:
	case PIO_IOC_WRITE_HW_COMPAT:
	{
		struct rp1_access_hw_args_compat compat_param;
		struct rp1_access_hw_args param;

		if (copy_from_user(&compat_param, compat_ptr(ioctl_param), sizeof(compat_param)))
			return -EFAULT;
		param.addr = compat_param.addr;
		param.len = compat_param.len;
		param.data = compat_ptr(compat_param.data);
		if (ioctl_num == PIO_IOC_READ_HW_COMPAT)
			return rp1_pio_read_hw(client, &param);
		else
			return rp1_pio_write_hw(client, &param);
	}
	default:
		return rp1_pio_ioctl(filp, ioctl_num, ioctl_param);
	}
}
#else
#define rp1_pio_compat_ioctl NULL
#endif

const struct file_operations rp1_pio_fops = {
	.owner =	THIS_MODULE,
	.open =		rp1_pio_file_open,
	.release =	rp1_pio_file_release,
	.unlocked_ioctl = rp1_pio_ioctl,
	.compat_ioctl = rp1_pio_compat_ioctl,
};

static int rp1_pio_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct resource *ioresource;
	struct rp1_pio_device *pio;
	struct rp1_firmware *fw;
	uint32_t op_count = 0;
	uint32_t op_base = 0;
	struct device *cdev;
	char dev_name[16];
	void *p;
	int ret;
	int i;

	/* Run-time check for a build-time misconfiguration */
	for (i = 0; i < ARRAY_SIZE(ioctl_handlers); i++) {
		struct handler_info *hdlr = &ioctl_handlers[i];

		if (WARN_ON(hdlr->argsize > MAX_ARG_SIZE))
			return -EINVAL;
	}

	pdev->id = of_alias_get_id(pdev->dev.of_node, "pio");
	if (pdev->id < 0)
		return dev_err_probe(dev, pdev->id, "alias is missing\n");

	fw = devm_rp1_firmware_get(dev, dev->of_node);
	if (!fw)
		return dev_err_probe(dev, -EPROBE_DEFER, "failed to find RP1 firmware driver\n");
	if (IS_ERR(fw)) {
		dev_warn(dev, "failed to contact RP1 firmware\n");
		return PTR_ERR(fw);
	}
	ret = rp1_firmware_get_feature(fw, FOURCC_PIO, &op_base, &op_count);
	if (ret < 0)
		return ret;

	pio = devm_kzalloc(&pdev->dev, sizeof(*pio), GFP_KERNEL);
	if (!pio)
		return -ENOMEM;

	platform_set_drvdata(pdev, pio);
	pio->fw_pio_base = op_base;
	pio->fw_pio_count = op_count;
	pio->pdev = pdev;
	pio->fw = fw;
	spin_lock_init(&pio->lock);
	mutex_init(&pio->instr_mutex);

	p = devm_platform_get_and_ioremap_resource(pdev, 0, &ioresource);
	if (IS_ERR(p))
		return PTR_ERR(p);

	pio->phys_addr = ioresource->start;

	ret = alloc_chrdev_region(&pio->dev_num, 0, 1, DRIVER_NAME);
	if (ret < 0) {
		dev_err(dev, "alloc_chrdev_region failed (rc=%d)\n", ret);
		goto out_err;
	}

	pio->dev_class = class_create(DRIVER_NAME);
	if (IS_ERR(pio->dev_class)) {
		ret = PTR_ERR(pio->dev_class);
		dev_err(dev, "class_create failed (err %d)\n", ret);
		goto out_unregister;
	}

	cdev_init(&pio->cdev, &rp1_pio_fops);
	ret = cdev_add(&pio->cdev, pio->dev_num, 1);
	if (ret) {
		dev_err(dev, "cdev_add failed (err %d)\n", ret);
		goto out_class_destroy;
	}

	sprintf(dev_name, "pio%d", pdev->id);
	cdev = device_create(pio->dev_class, NULL, pio->dev_num, NULL, dev_name);
	if (IS_ERR(cdev)) {
		ret = PTR_ERR(cdev);
		dev_err(dev, "%s: device_create failed (err %d)\n", __func__, ret);
		goto out_cdev_del;
	}

	g_pio = pio;

	dev_info(dev, "Created instance as %s\n", dev_name);
	return 0;

out_cdev_del:
	cdev_del(&pio->cdev);

out_class_destroy:
	class_destroy(pio->dev_class);

out_unregister:
	unregister_chrdev_region(pio->dev_num, 1);

out_err:
	return ret;
}

static void rp1_pio_remove(struct platform_device *pdev)
{
	struct rp1_pio_device *pio = platform_get_drvdata(pdev);

	/* There should be no clients */

	if (g_pio == pio)
		g_pio = NULL;

	device_destroy(pio->dev_class, pio->dev_num);
	cdev_del(&pio->cdev);
	class_destroy(pio->dev_class);
	unregister_chrdev_region(pio->dev_num, 1);
}

static const struct of_device_id rp1_pio_ids[] = {
	{ .compatible = "raspberrypi,rp1-pio" },
	{ }
};
MODULE_DEVICE_TABLE(of, rp1_pio_ids);

static struct platform_driver rp1_pio_driver = {
	.driver	= {
		.name		= "rp1-pio",
		.of_match_table	= of_match_ptr(rp1_pio_ids),
	},
	.probe		= rp1_pio_probe,
	.remove_new	= rp1_pio_remove,
	.shutdown	= rp1_pio_remove,
};

module_platform_driver(rp1_pio_driver);

MODULE_DESCRIPTION("PIO controller driver for Raspberry Pi RP1");
MODULE_AUTHOR("Phil Elwell");
MODULE_LICENSE("GPL");
