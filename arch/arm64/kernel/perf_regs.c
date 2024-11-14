// SPDX-License-Identifier: GPL-2.0
#include <linux/compat.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/perf_event.h>
#include <linux/bug.h>
#include <linux/sched/task_stack.h>

#include <asm/perf_regs.h>
#include <asm/ptrace.h>

#define PERF_IDX_SP_32BIT 15
#define REG_RESERVED (~((1ULL << PERF_REG_ARM64_MAX) - 1))

static u64 perf_ext_regs_value(int idx)
{
	switch (idx) {
	case PERF_REG_ARM64_VG:
		if (WARN_ON_ONCE(!system_supports_sve()))
			return 0;

		/* Vector granule is current length in bits of SVE registers divided by 64 */
		return (task_get_sve_vl(current) * 8) / 64;
	default:
		WARN_ON_ONCE(true);
		return 0;
	}
}

static inline u64 get_compat_register_value(struct pt_regs *regs, int idx)
{
	switch (idx) {
	case PERF_REG_ARM64_SP:
		return regs->compat_sp;
	case PERF_REG_ARM64_LR:
		return regs->compat_lr;
	case PERF_IDX_SP_32BIT:
		return regs->pc;
	default:
		return 0;
	}
}

u64 perf_reg_value(struct pt_regs *regs, int idx)
{
	if (WARN_ON_ONCE((u32)idx >= PERF_REG_ARM64_EXTENDED_MAX))
		return 0;

	if (compat_user_mode(regs)) {
		return get_compat_register_value(regs, idx);
	}

	switch (idx) {
	case PERF_REG_ARM64_SP:
		return regs->sp;
	case PERF_REG_ARM64_PC:
		return regs->pc;
	default:
		if ((u32)idx >= PERF_REG_ARM64_MAX)
			return perf_ext_regs_value(idx);
		return regs->regs[idx];
	}
}

int perf_reg_validate(u64 mask)
{
	u64 reserved_mask = REG_RESERVED;

	if (system_supports_sve())
		reserved_mask &= ~(1ULL << PERF_REG_ARM64_VG);

	return (!mask || mask & reserved_mask) ? -EINVAL : 0;
}

u64 perf_reg_abi(struct task_struct *task)
{
	return is_compat_thread(task_thread_info(task)) ? PERF_SAMPLE_REGS_ABI_32 : PERF_SAMPLE_REGS_ABI_64;
}

void perf_get_regs_user(struct perf_regs *regs_user, struct pt_regs *regs)
{
	regs_user->regs = task_pt_regs(current);
	regs_user->abi = perf_reg_abi(current);
}
