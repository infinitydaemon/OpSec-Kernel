// SPDX-License-Identifier: GPL-2.0
/*
 * Handling of different ABIs (personalities).
 *
 * We group personalities into execution domains which have their
 * own handlers for kernel entry points, signal mapping, etc...
 *
 * 2001-05-06	Complete rewrite,  Christoph Hellwig (hch@infradead.org)
 * 2023			CWD Systems
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kmod.h>
#include <linux/module.h>
#include <linux/personality.h>
#include <linux/proc_fs.h>
#include <linux/sched.h>
#include <linux/seq_file.h>
#include <linux/syscalls.h>
#include <linux/sysctl.h>
#include <linux/types.h>
#include <linux/fs.h>
#include <linux/unistd.h>

#ifdef CONFIG_PROC_FS

typedef int (*proc_show_func_t)(struct seq_file *seq, void *offset);

static int execdomains_proc_show(struct seq_file *seq, void *offset)
{
	seq_puts(seq, "0-0\tLinux\t[kernel]\n");
	return 0;
}

static int __init proc_execdomains_init(void)
{
	proc_show_func_t show_func = execdomains_proc_show;
	proc_create_single("execdomains", 0, NULL, show_func);
	return 0;
}
module_init(proc_execdomains_init);
#endif

typedef unsigned int (*personality_func_t)(unsigned int personality);

SYSCALL_DEFINE1(personality, unsigned int, personality)
{
	unsigned int old_personality = current->personality;

	if (personality != 0xffffffff) {
		personality_func_t set_personality_func = set_personality;
		set_personality_func(personality);
	}

	return old_personality;
}

