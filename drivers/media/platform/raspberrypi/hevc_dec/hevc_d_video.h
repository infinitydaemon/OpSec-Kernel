/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Raspberry Pi HEVC driver
 *
 * Copyright (C) 2024 Raspberry Pi Ltd
 *
 * Based on the Cedrus VPU driver, that is:
 *
 * Copyright (C) 2016 Florent Revest <florent.revest@free-electrons.com>
 * Copyright (C) 2018 Paul Kocialkowski <paul.kocialkowski@bootlin.com>
 * Copyright (C) 2018 Bootlin
 */

#ifndef _HEVC_D_VIDEO_H_
#define _HEVC_D_VIDEO_H_

struct hevc_d_format {
	u32		pixelformat;
	u32		directions;
	unsigned int	capabilities;
};

static inline int is_sps_set(const struct v4l2_ctrl_hevc_sps * const sps)
{
	return sps && sps->pic_width_in_luma_samples;
}

extern const struct v4l2_ioctl_ops hevc_d_ioctl_ops;

int hevc_d_queue_init(void *priv, struct vb2_queue *src_vq,
		      struct vb2_queue *dst_vq);

size_t hevc_d_bit_buf_size(unsigned int w, unsigned int h, unsigned int bits_minus8);
size_t hevc_d_round_up_size(const size_t x);

void hevc_d_prepare_src_format(struct v4l2_pix_format_mplane *pix_fmt);

#endif
