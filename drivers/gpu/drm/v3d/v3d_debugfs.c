// SPDX-License-Identifier: GPL-2.0+
/* Copyright (C) 2014-2018 Broadcom */

#include <linux/circ_buf.h>
#include <linux/ctype.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/string_helpers.h>
#include <linux/sched/clock.h>

#include <drm/drm_debugfs.h>

#include "v3d_drv.h"
#include "v3d_regs.h"

#define REGDEF(min_ver, max_ver, reg) { min_ver, max_ver, reg, #reg }
struct v3d_reg_def {
	u32 min_ver;
	u32 max_ver;
	u32 reg;
	const char *name;
};

static const struct v3d_reg_def v3d_hub_reg_defs[] = {
	REGDEF(33, 42, V3D_HUB_AXICFG),
	REGDEF(33, 71, V3D_HUB_UIFCFG),
	REGDEF(33, 71, V3D_HUB_IDENT0),
	REGDEF(33, 71, V3D_HUB_IDENT1),
	REGDEF(33, 71, V3D_HUB_IDENT2),
	REGDEF(33, 71, V3D_HUB_IDENT3),
	REGDEF(33, 71, V3D_HUB_INT_STS),
	REGDEF(33, 71, V3D_HUB_INT_MSK_STS),

	REGDEF(33, 71, V3D_MMU_CTL),
	REGDEF(33, 71, V3D_MMU_VIO_ADDR),
	REGDEF(33, 71, V3D_MMU_VIO_ID),
	REGDEF(33, 71, V3D_MMU_DEBUG_INFO),

	REGDEF(71, 71, V3D_V7_GMP_STATUS),
	REGDEF(71, 71, V3D_V7_GMP_CFG),
	REGDEF(71, 71, V3D_V7_GMP_VIO_ADDR),
};

static const struct v3d_reg_def v3d_gca_reg_defs[] = {
	REGDEF(33, 33, V3D_GCA_SAFE_SHUTDOWN),
	REGDEF(33, 33, V3D_GCA_SAFE_SHUTDOWN_ACK),
};

static const struct v3d_reg_def v3d_core_reg_defs[] = {
	REGDEF(33, 71, V3D_CTL_IDENT0),
	REGDEF(33, 71, V3D_CTL_IDENT1),
	REGDEF(33, 71, V3D_CTL_IDENT2),
	REGDEF(33, 71, V3D_CTL_MISCCFG),
	REGDEF(33, 71, V3D_CTL_INT_STS),
	REGDEF(33, 71, V3D_CTL_INT_MSK_STS),
	REGDEF(33, 71, V3D_CLE_CT0CS),
	REGDEF(33, 71, V3D_CLE_CT0CA),
	REGDEF(33, 71, V3D_CLE_CT0EA),
	REGDEF(33, 71, V3D_CLE_CT1CS),
	REGDEF(33, 71, V3D_CLE_CT1CA),
	REGDEF(33, 71, V3D_CLE_CT1EA),

	REGDEF(33, 71, V3D_PTB_BPCA),
	REGDEF(33, 71, V3D_PTB_BPCS),

	REGDEF(33, 41, V3D_GMP_STATUS),
	REGDEF(33, 41, V3D_GMP_CFG),
	REGDEF(33, 41, V3D_GMP_VIO_ADDR),

	REGDEF(33, 71, V3D_ERR_FDBGO),
	REGDEF(33, 71, V3D_ERR_FDBGB),
	REGDEF(33, 71, V3D_ERR_FDBGS),
	REGDEF(33, 71, V3D_ERR_STAT),
};

static const struct v3d_reg_def v3d_csd_reg_defs[] = {
	REGDEF(41, 71, V3D_CSD_STATUS),
	REGDEF(41, 41, V3D_CSD_CURRENT_CFG0),
	REGDEF(41, 41, V3D_CSD_CURRENT_CFG1),
	REGDEF(41, 41, V3D_CSD_CURRENT_CFG2),
	REGDEF(41, 41, V3D_CSD_CURRENT_CFG3),
	REGDEF(41, 41, V3D_CSD_CURRENT_CFG4),
	REGDEF(41, 41, V3D_CSD_CURRENT_CFG5),
	REGDEF(41, 41, V3D_CSD_CURRENT_CFG6),
	REGDEF(71, 71, V3D_V7_CSD_CURRENT_CFG0),
	REGDEF(71, 71, V3D_V7_CSD_CURRENT_CFG1),
	REGDEF(71, 71, V3D_V7_CSD_CURRENT_CFG2),
	REGDEF(71, 71, V3D_V7_CSD_CURRENT_CFG3),
	REGDEF(71, 71, V3D_V7_CSD_CURRENT_CFG4),
	REGDEF(71, 71, V3D_V7_CSD_CURRENT_CFG5),
	REGDEF(71, 71, V3D_V7_CSD_CURRENT_CFG6),
	REGDEF(71, 71, V3D_V7_CSD_CURRENT_CFG7),
};

static int v3d_v3d_debugfs_regs(struct seq_file *m, void *unused)
{
	struct drm_info_node *node = (struct drm_info_node *)m->private;
	struct drm_device *dev = node->minor->dev;
	struct v3d_dev *v3d = to_v3d_dev(dev);
	int i, core;

	for (i = 0; i < ARRAY_SIZE(v3d_hub_reg_defs); i++) {
		const struct v3d_reg_def *def = &v3d_hub_reg_defs[i];

		if (v3d->ver >= def->min_ver && v3d->ver <= def->max_ver) {
			seq_printf(m, "%s (0x%04x): 0x%08x\n",
				   def->name, def->reg, V3D_READ(def->reg));
		}
	}

	for (i = 0; i < ARRAY_SIZE(v3d_gca_reg_defs); i++) {
		const struct v3d_reg_def *def = &v3d_gca_reg_defs[i];

		if (v3d->ver >= def->min_ver && v3d->ver <= def->max_ver) {
			seq_printf(m, "%s (0x%04x): 0x%08x\n",
				   def->name, def->reg, V3D_GCA_READ(def->reg));
		}
	}

	for (core = 0; core < v3d->cores; core++) {
		for (i = 0; i < ARRAY_SIZE(v3d_core_reg_defs); i++) {
			const struct v3d_reg_def *def = &v3d_core_reg_defs[i];

			if (v3d->ver >= def->min_ver && v3d->ver <= def->max_ver) {
				seq_printf(m, "core %d %s (0x%04x): 0x%08x\n",
					   core, def->name, def->reg,
					   V3D_CORE_READ(core, def->reg));
			}
		}

		for (i = 0; i < ARRAY_SIZE(v3d_csd_reg_defs); i++) {
			const struct v3d_reg_def *def = &v3d_csd_reg_defs[i];

			if (v3d->ver >= def->min_ver && v3d->ver <= def->max_ver) {
				seq_printf(m, "core %d %s (0x%04x): 0x%08x\n",
					   core, def->name, def->reg,
					   V3D_CORE_READ(core, def->reg));
			}
		}
	}

	return 0;
}

static int v3d_v3d_debugfs_ident(struct seq_file *m, void *unused)
{
	struct drm_info_node *node = (struct drm_info_node *)m->private;
	struct drm_device *dev = node->minor->dev;
	struct v3d_dev *v3d = to_v3d_dev(dev);
	u32 ident0, ident1, ident2, ident3, cores;
	int core;

	ident0 = V3D_READ(V3D_HUB_IDENT0);
	ident1 = V3D_READ(V3D_HUB_IDENT1);
	ident2 = V3D_READ(V3D_HUB_IDENT2);
	ident3 = V3D_READ(V3D_HUB_IDENT3);
	cores = V3D_GET_FIELD(ident1, V3D_HUB_IDENT1_NCORES);

	seq_printf(m, "Revision:   %d.%d.%d.%d\n",
		   V3D_GET_FIELD(ident1, V3D_HUB_IDENT1_TVER),
		   V3D_GET_FIELD(ident1, V3D_HUB_IDENT1_REV),
		   V3D_GET_FIELD(ident3, V3D_HUB_IDENT3_IPREV),
		   V3D_GET_FIELD(ident3, V3D_HUB_IDENT3_IPIDX));
	seq_printf(m, "MMU:        %s\n",
		   str_yes_no(ident2 & V3D_HUB_IDENT2_WITH_MMU));
	seq_printf(m, "TFU:        %s\n",
		   str_yes_no(ident1 & V3D_HUB_IDENT1_WITH_TFU));
	if (v3d->ver <= 42) {
		seq_printf(m, "TSY:        %s\n",
			   str_yes_no(ident1 & V3D_HUB_IDENT1_WITH_TSY));
	}
	seq_printf(m, "MSO:        %s\n",
		   str_yes_no(ident1 & V3D_HUB_IDENT1_WITH_MSO));
	seq_printf(m, "L3C:        %s (%dkb)\n",
		   str_yes_no(ident1 & V3D_HUB_IDENT1_WITH_L3C),
		   V3D_GET_FIELD(ident2, V3D_HUB_IDENT2_L3C_NKB));

	for (core = 0; core < cores; core++) {
		u32 misccfg;
		u32 nslc, ntmu, qups;

		ident0 = V3D_CORE_READ(core, V3D_CTL_IDENT0);
		ident1 = V3D_CORE_READ(core, V3D_CTL_IDENT1);
		ident2 = V3D_CORE_READ(core, V3D_CTL_IDENT2);
		misccfg = V3D_CORE_READ(core, V3D_CTL_MISCCFG);

		nslc = V3D_GET_FIELD(ident1, V3D_IDENT1_NSLC);
		ntmu = V3D_GET_FIELD(ident1, V3D_IDENT1_NTMU);
		qups = V3D_GET_FIELD(ident1, V3D_IDENT1_QUPS);

		seq_printf(m, "Core %d:\n", core);
		seq_printf(m, "  Revision:     %d.%d\n",
			   V3D_GET_FIELD(ident0, V3D_IDENT0_VER),
			   V3D_GET_FIELD(ident1, V3D_IDENT1_REV));
		seq_printf(m, "  Slices:       %d\n", nslc);
		seq_printf(m, "  TMUs:         %d\n", nslc * ntmu);
		seq_printf(m, "  QPUs:         %d\n", nslc * qups);
		seq_printf(m, "  Semaphores:   %d\n",
			   V3D_GET_FIELD(ident1, V3D_IDENT1_NSEM));
		if (v3d->ver <= 42) {
			seq_printf(m, "  BCG int:      %d\n",
				   (ident2 & V3D_IDENT2_BCG_INT) != 0);
		}
		if (v3d->ver < 40) {
			seq_printf(m, "  Override TMU: %d\n",
				   (misccfg & V3D_MISCCFG_OVRTMUOUT) != 0);
		}
	}

	return 0;
}

static int v3d_debugfs_bo_stats(struct seq_file *m, void *unused)
{
	struct drm_info_node *node = (struct drm_info_node *)m->private;
	struct drm_device *dev = node->minor->dev;
	struct v3d_dev *v3d = to_v3d_dev(dev);

	mutex_lock(&v3d->bo_lock);
	seq_printf(m, "allocated bos:          %d\n",
		   v3d->bo_stats.num_allocated);
	seq_printf(m, "allocated bo size (kb): %ld\n",
		   (long)v3d->bo_stats.pages_allocated << (PAGE_SHIFT - 10));
	mutex_unlock(&v3d->bo_lock);

	return 0;
}

static int v3d_debugfs_gpu_usage(struct seq_file *m, void *unused)
{
	struct drm_info_node *node = (struct drm_info_node *)m->private;
	struct drm_device *dev = node->minor->dev;
	struct v3d_dev *v3d = to_v3d_dev(dev);
	struct v3d_queue_stats *queue_stats;
	enum v3d_queue queue;
	u64 timestamp = local_clock();
	u64 active_runtime;

	seq_printf(m, "timestamp;%llu;\n", local_clock());
	seq_printf(m, "\"QUEUE\";\"JOBS\";\"RUNTIME\";\"ACTIVE\";\n");
	for (queue = 0; queue < V3D_MAX_QUEUES; queue++) {
		if (!v3d->queue[queue].sched.ready)
			continue;

		queue_stats = &v3d->gpu_queue_stats[queue];
		mutex_lock(&queue_stats->lock);
		v3d_sched_stats_update(queue_stats);
		if (queue_stats->last_pid)
			active_runtime = timestamp - queue_stats->last_exec_start;
		else
			active_runtime = 0;

		seq_printf(m, "%s;%d;%llu;%c;\n",
			   v3d_queue_to_string(queue),
			   queue_stats->jobs_sent,
			   queue_stats->runtime + active_runtime,
			   queue_stats->last_pid?'1':'0');
		mutex_unlock(&queue_stats->lock);
	}

	return 0;
}

static int v3d_debugfs_gpu_pid_usage(struct seq_file *m, void *unused)
{
	struct drm_info_node *node = (struct drm_info_node *)m->private;
	struct drm_device *dev = node->minor->dev;
	struct v3d_dev *v3d = to_v3d_dev(dev);
	struct v3d_queue_stats *queue_stats;
	struct v3d_queue_pid_stats *cur;
	enum v3d_queue queue;
	u64 active_runtime;
	u64 timestamp = local_clock();

	seq_printf(m, "timestamp;%llu;\n", timestamp);
	seq_printf(m, "\"QUEUE\";\"PID\",\"JOBS\";\"RUNTIME\";\"ACTIVE\";\n");
	for (queue = 0; queue < V3D_MAX_QUEUES; queue++) {

		if (!v3d->queue[queue].sched.ready)
			continue;

		queue_stats = &v3d->gpu_queue_stats[queue];
		mutex_lock(&queue_stats->lock);
		queue_stats->gpu_pid_stats_timeout = jiffies + V3D_QUEUE_STATS_TIMEOUT;
		v3d_sched_stats_update(queue_stats);
		list_for_each_entry(cur, &queue_stats->pid_stats_list, list) {

			if (cur->pid == queue_stats->last_pid)
				active_runtime = timestamp - queue_stats->last_exec_start;
			else
				active_runtime = 0;

			seq_printf(m, "%s;%d;%d;%llu;%c;\n",
				   v3d_queue_to_string(queue),
				   cur->pid, cur->jobs_sent,
				   cur->runtime + active_runtime,
				   cur->pid == queue_stats->last_pid ? '1' : '0');
		}
		mutex_unlock(&queue_stats->lock);
	}

	return 0;
}

static int v3d_measure_clock(struct seq_file *m, void *unused)
{
	struct drm_info_node *node = (struct drm_info_node *)m->private;
	struct drm_device *dev = node->minor->dev;
	struct v3d_dev *v3d = to_v3d_dev(dev);
	uint32_t cycles;
	int core = 0;
	int measure_ms = 1000;

	if (v3d->ver >= 40) {
		int cycle_count_reg = v3d->ver < 71 ?
			V3D_PCTR_CYCLE_COUNT : V3D_V7_PCTR_CYCLE_COUNT;
		V3D_CORE_WRITE(core, V3D_V4_PCTR_0_SRC_0_3,
			       V3D_SET_FIELD(cycle_count_reg,
					     V3D_PCTR_S0));
		V3D_CORE_WRITE(core, V3D_V4_PCTR_0_CLR, 1);
		V3D_CORE_WRITE(core, V3D_V4_PCTR_0_EN, 1);
	} else {
		V3D_CORE_WRITE(core, V3D_V3_PCTR_0_PCTRS0,
			       V3D_PCTR_CYCLE_COUNT);
		V3D_CORE_WRITE(core, V3D_V3_PCTR_0_CLR, 1);
		V3D_CORE_WRITE(core, V3D_V3_PCTR_0_EN,
			       V3D_V3_PCTR_0_EN_ENABLE |
			       1);
	}
	msleep(measure_ms);
	cycles = V3D_CORE_READ(core, V3D_PCTR_0_PCTR0);

	seq_printf(m, "cycles: %d (%d.%d Mhz)\n",
		   cycles,
		   cycles / (measure_ms * 1000),
		   (cycles / (measure_ms * 100)) % 10);

	return 0;
}

static const struct drm_info_list v3d_debugfs_list[] = {
	{"v3d_ident", v3d_v3d_debugfs_ident, 0},
	{"v3d_regs", v3d_v3d_debugfs_regs, 0},
	{"measure_clock", v3d_measure_clock, 0},
	{"bo_stats", v3d_debugfs_bo_stats, 0},
	{"gpu_usage", v3d_debugfs_gpu_usage, 0},
	{"gpu_pid_usage", v3d_debugfs_gpu_pid_usage, 0},
};

void
v3d_debugfs_init(struct drm_minor *minor)
{
	drm_debugfs_create_files(v3d_debugfs_list,
				 ARRAY_SIZE(v3d_debugfs_list),
				 minor->debugfs_root, minor);
}
