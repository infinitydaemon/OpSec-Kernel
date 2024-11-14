// SPDX-License-Identifier: GPL-2.0

#include <linux/compiler.h>
#include <linux/context_tracking.h>
#include <linux/errno.h>
#include <linux/nospec.h>
#include <linux/ptrace.h>
#include <linux/randomize_kstack.h>
#include <linux/syscalls.h>

#include <asm/debug-monitors.h>
#include <asm/exception.h>
#include <asm/fpsimd.h>
#include <asm/syscall.h>
#include <asm/thread_info.h>
#include <asm/unistd.h>

long compat_arm_syscall(struct pt_regs *regs, int scno);
long sys_ni_syscall(void);

static long do_ni_syscall(struct pt_regs *regs, int scno)
{
#ifdef CONFIG_COMPAT
	if (is_compat_task()) {
		long ret = compat_arm_syscall(regs, scno);
		if (ret != -ENOSYS)
			return ret;
	}
#endif
	return sys_ni_syscall();
}

static long __invoke_syscall(struct pt_regs *regs, syscall_fn_t syscall_fn)
{
	return syscall_fn(regs);
}

static inline void set_kstack_offset(void)
{
	choose_random_kstack_offset(get_random_u16());
	add_random_kstack_offset();
}

static inline bool check_and_handle_mte_fault(unsigned long flags, struct pt_regs *regs)
{
	if (flags & _TIF_MTE_ASYNC_FAULT) {
		syscall_set_return_value(current, regs, -ERESTARTNOINTR, 0);
		return true;
	}
	return false;
}

static void invoke_syscall(struct pt_regs *regs, unsigned int scno,
			   unsigned int sc_nr, const syscall_fn_t syscall_table[])
{
	long ret;
	set_kstack_offset();

	if (scno < sc_nr) {
		syscall_fn_t syscall_fn = syscall_table[array_index_nospec(scno, sc_nr)];
		ret = __invoke_syscall(regs, syscall_fn);
	} else {
		ret = do_ni_syscall(regs, scno);
	}

	syscall_set_return_value(current, regs, 0, ret);
}

static inline bool has_syscall_work(unsigned long flags)
{
	return unlikely(flags & _TIF_SYSCALL_WORK);
}

static void handle_tracing_and_exit(struct pt_regs *regs, int *scno)
{
	*scno = syscall_trace_enter(regs);
	if (*scno == NO_SYSCALL)
		syscall_set_return_value(current, regs, -ENOSYS, 0);
}

static void el0_svc_common(struct pt_regs *regs, int scno, int sc_nr,
			   const syscall_fn_t syscall_table[])
{
	unsigned long flags = read_thread_flags();

	regs->orig_x0 = regs->regs[0];
	regs->syscallno = scno;

	if (check_and_handle_mte_fault(flags, regs))
		return;

	if (has_syscall_work(flags)) {
		handle_tracing_and_exit(regs, &scno);
		if (scno == NO_SYSCALL)
			goto trace_exit;
	}

	invoke_syscall(regs, scno, sc_nr, syscall_table);

	/* Exit tracing */
	if (!has_syscall_work(flags) && !IS_ENABLED(CONFIG_DEBUG_RSEQ)) {
		flags = read_thread_flags();
		if (!has_syscall_work(flags) && !(flags & _TIF_SINGLESTEP))
			return;
	}

trace_exit:
	syscall_trace_exit(regs);
}

void do_el0_svc(struct pt_regs *regs)
{
	el0_svc_common(regs, regs->regs[8], __NR_syscalls, sys_call_table);
}

#ifdef CONFIG_COMPAT
void do_el0_svc_compat(struct pt_regs *regs)
{
	el0_svc_common(regs, regs->regs[7], __NR_compat_syscalls, compat_sys_call_table);
}
#endif
