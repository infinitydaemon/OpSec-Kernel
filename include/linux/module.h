/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Dynamic loading of modules into the kernel.
 *
 * Rewritten by Richard Henderson <rth@tamu.edu> Dec 1996
 * Rewritten again by Rusty Russell, 2002
 */

#ifndef _LINUX_MODULE_H
#define _LINUX_MODULE_H

#include <linux/list.h>
#include <linux/stat.h>
#include <linux/buildid.h>
#include <linux/compiler.h>
#include <linux/cache.h>
#include <linux/kmod.h>
#include <linux/init.h>
#include <linux/elf.h>
#include <linux/stringify.h>
#include <linux/kobject.h>
#include <linux/moduleparam.h>
#include <linux/jump_label.h>
#include <linux/export.h>
#include <linux/rbtree_latch.h>
#include <linux/error-injection.h>
#include <linux/tracepoint-defs.h>
#include <linux/srcu.h>
#include <linux/static_call_types.h>
#include <linux/dynamic_debug.h>

#include <linux/percpu.h>
#include <asm/module.h>

#define MODULE_NAME_LEN MAX_PARAM_PREFIX_LEN

struct modversion_info {
	unsigned long crc;
	char name[MODULE_NAME_LEN];
};

struct module;
struct exception_table_entry;

struct module_kobject {
	struct kobject kobj;
	struct module *mod;
	struct kobject *drivers_dir;
	struct module_param_attrs *mp;
	struct completion *kobj_completion;
} __randomize_layout;

struct module_attribute {
	struct attribute attr;
	ssize_t (*show)(struct module_attribute *, struct module_kobject *,
			char *);
	ssize_t (*store)(struct module_attribute *, struct module_kobject *,
			 const char *, size_t count);
	void (*setup)(struct module *, const char *);
	int (*test)(struct module *);
	void (*free)(struct module *);
};

struct module_version_attribute {
	struct module_attribute mattr;
	const char *module_name;
	const char *version;
};

extern ssize_t __modver_version_show(struct module_attribute *,
				     struct module_kobject *, char *);

extern struct module_attribute module_uevent;

/* These are either module local, or the kernel's dummy ones. */
extern int init_module(void);
extern void cleanup_module(void);

#ifndef MODULE
/**
 * module_init() - driver initialization entry point
 * @x: function to be run at kernel boot time or module insertion
 *
 * module_init() will either be called during do_initcalls() (if
 * builtin) or at module insertion time (if a module).  There can only
 * be one per module.
 */
#define module_init(x)	__initcall(x);

/**
 * module_exit() - driver exit entry point
 * @x: function to be run when driver is removed
 *
 * module_exit() will wrap the driver clean-up code
 * with cleanup_module() when used with rmmod when
 * the driver is a module.  If the driver is statically
 * compiled into the kernel, module_exit() has no effect.
 * There can only be one per module.
 */
#define module_exit(x)	__exitcall(x);

#else /* MODULE */

/*
 * In most cases loadable modules do not need custom
 * initcall levels. There are still some valid cases where
 * a driver may be needed early if built in, and does not
 * matter when built as a loadable module. Like bus
 * snooping debug drivers.
 */
#define early_initcall(fn)		module_init(fn)
#define core_initcall(fn)		module_init(fn)
#define core_initcall_sync(fn)		module_init(fn)
#define postcore_initcall(fn)		module_init(fn)
#define postcore_initcall_sync(fn)	module_init(fn)
#define arch_initcall(fn)		module_init(fn)
#define subsys_initcall(fn)		module_init(fn)
#define subsys_initcall_sync(fn)	module_init(fn)
#define fs_initcall(fn)			module_init(fn)
#define fs_initcall_sync(fn)		module_init(fn)
#define rootfs_initcall(fn)		module_init(fn)
#define device_initcall(fn)		module_init(fn)
#define device_initcall_sync(fn)	module_init(fn)
#define late_initcall(fn)		module_init(fn)
#define late_initcall_sync(fn)		module_init(fn)

#define console_initcall(fn)		module_init(fn)

/* Each module must use one module_init(). */
#define module_init(initfn)					\
	static inline initcall_t __maybe_unused __inittest(void)		\
	{ return initfn; }					\
	int init_module(void) __copy(initfn)			\
		__attribute__((alias(#initfn)));		\
	___ADDRESSABLE(init_module, __initdata);

/* This is only required if you want to be unloadable. */
#define module_exit(exitfn)					\
	static inline exitcall_t __maybe_unused __exittest(void)		\
	{ return exitfn; }					\
	void cleanup_module(void) __copy(exitfn)		\
		__attribute__((alias(#exitfn)));		\
	___ADDRESSABLE(cleanup_module, __exitdata);

#endif

/* This means "can be init if no module support, otherwise module load
   may call it." */
#ifdef CONFIG_MODULES
#define __init_or_module
#define __initdata_or_module
#define __initconst_or_module
#define __INIT_OR_MODULE	.text
#define __INITDATA_OR_MODULE	.data
#define __INITRODATA_OR_MODULE	.section ".rodata","a",%progbits
#else
#define __init_or_module __init
#define __initdata_or_module __initdata
#define __initconst_or_module __initconst
#define __INIT_OR_MODULE __INIT
#define __INITDATA_OR_MODULE __INITDATA
#define __INITRODATA_OR_MODULE __INITRODATA
#endif /*CONFIG_MODULES*/

struct module_kobject *lookup_or_create_module_kobject(const char *name);

/* Generic info of form tag = "info" */
#define MODULE_INFO(tag, info) __MODULE_INFO(tag, tag, info)

/* For userspace: you can also call me... */
#define MODULE_ALIAS(_alias) MODULE_INFO(alias, _alias)

/* Soft module dependencies. See man modprobe.d for details.
 * Example: MODULE_SOFTDEP("pre: module-foo module-bar post: module-baz")
 */
#define MODULE_SOFTDEP(_softdep) MODULE_INFO(softdep, _softdep)

/*
 * Weak module dependencies. See man modprobe.d for details.
 * Example: MODULE_WEAKDEP("module-foo")
 */
#define MODULE_WEAKDEP(_weakdep) MODULE_INFO(weakdep, _weakdep)

/*
 * MODULE_FILE is used for generating modules.builtin
 * So, make it no-op when this is being built as a module
 */
#ifdef MODULE
#define MODULE_FILE
#else
#define MODULE_FILE	MODULE_INFO(file, KBUILD_MODFILE);
#endif

/*
 * The following license idents are currently accepted as indicating free
 * software modules
 *
 *	"GPL"				[GNU Public License v2]
 *	"GPL v2"			[GNU Public License v2]
 *	"GPL and additional rights"	[GNU Public License v2 rights and more]
 *	"Dual BSD/GPL"			[GNU Public License v2
 *					 or BSD license choice]
 *	"Dual MIT/GPL"			[GNU Public License v2
 *					 or MIT license choice]
 *	"Dual MPL/GPL"			[GNU Public License v2
 *					 or Mozilla license choice]
 *
 * The following other idents are available
 *
 *	"Proprietary"			[Non free products]
 *
 * Both "GPL v2" and "GPL" (the latter also in dual licensed strings) are
 * merely stating that the module is licensed under the GPL v2, but are not
 * telling whether "GPL v2 only" or "GPL v2 or later". The reason why there
 * are two variants is a historic and failed attempt to convey more
 * information in the MODULE_LICENSE string. For module loading the
 * "only/or later" distinction is completely irrelevant and does neither
 * replace the proper license identifiers in the corresponding source file
 * nor amends them in any way. The sole purpose is to make the
 * 'Proprietary' flagging work and to refuse to bind symbols which are
 * exported with EXPORT_SYMBOL_GPL when a non free module is loaded.
 *
 * In the same way "BSD" is not a clear license information. It merely
 * states, that the module is licensed under one of the compatible BSD
 * license variants. The detailed and correct license information is again
 * to be found in the corresponding source files.
 *
 * There are dual licensed components, but when running with Linux it is the
 * GPL that is relevant so this is a non issue. Similarly LGPL linked with GPL
 * is a GPL combined work.
 *
 * This exists for several reasons
 * 1.	So modinfo can show license info for users wanting to vet their setup
 *	is free
 * 2.	So the community can ignore bug reports including proprietary modules
 * 3.	So vendors can do likewise based on their own policies
 */
#define MODULE_LICENSE(_license) MODULE_FILE MODULE_INFO(license, _license)

/*
 * Author(s), use "Name <email>" or just "Name", for multiple
 * authors use multiple MODULE_AUTHOR() statements/lines.
 */
#define MODULE_AUTHOR(_author) MODULE_INFO(author, _author)

/* What your module does. */
#define MODULE_DESCRIPTION(_description) MODULE_INFO(description, _description)

#ifdef MODULE
/* Creates an alias so file2alias.c can find device table. */
#define MODULE_DEVICE_TABLE(type, name)					\
extern typeof(name) __mod_##type##__##name##_device_table		\
  __attribute__ ((unused, alias(__stringify(name))))
#else  /* !MODULE */
#define MODULE_DEVICE_TABLE(type, name)
#endif

/* Version of form [<epoch>:]<version>[-<extra-version>].
 * Or for CVS/RCS ID version, everything but the number is stripped.
 * <epoch>: A (small) unsigned integer which allows you to start versions
 * anew. If not mentioned, it's zero.  eg. "2:1.0" is after
 * "1:2.0".

 * <version>: The <version> may contain only alphanumerics and the
 * character `.'.  Ordered by numeric sort for numeric parts,
 * ascii sort for ascii parts (as per RPM or DEB algorithm).

 * <extraversion>: Like <version>, but inserted for local
 * customizations, eg "rh3" or "rusty1".

 * Using this automatically adds a checksum of the .c files and the
 * local headers in "srcversion".
 */

#if defined(MODULE) || !defined(CONFIG_SYSFS)
#define MODULE_VERSION(_version) MODULE_INFO(version, _version)
#else
#define MODULE_VERSION(_version)					\
	MODULE_INFO(version, _version);					\
	static struct module_version_attribute __modver_attr		\
		__used __section("__modver")				\
		__aligned(__alignof__(struct module_version_attribute)) \
		= {							\
			.mattr	= {					\
				.attr	= {				\
					.name	= "version",		\
					.mode	= S_IRUGO,		\
				},					\
				.show	= __modver_version_show,	\
			},						\
			.module_name	= KBUILD_MODNAME,		\
			.version	= _version,			\
		}
#endif

/* Optional firmware file (or files) needed by the module
 * format is simply firmware file name.  Multiple firmware
 * files require multiple MODULE_FIRMWARE() specifiers */
#define MODULE_FIRMWARE(_firmware) MODULE_INFO(firmware, _firmware)

#define MODULE_IMPORT_NS(ns)	MODULE_INFO(import_ns, __stringify(ns))

struct notifier_block;

#ifdef CONFIG_MODULES

extern int modules_disabled; /* for sysctl */
/* Get/put a kernel symbol (calls must be symmetric) */
void *__symbol_get(const char *symbol);
void *__symbol_get_gpl(const char *symbol);
#define symbol_get(x) ((typeof(&x))(__symbol_get(__stringify(x))))

/* modules using other modules: kdb wants to see this. */
struct module_use {
	struct list_head source_list;
	struct list_head target_list;
	struct module *source, *target;
};

enum module_state {
	MODULE_STATE_LIVE,	/* Normal state. */
	MODULE_STATE_COMING,	/* Full formed, running module_init. */
	MODULE_STATE_GOING,	/* Going away. */
	MODULE_STATE_UNFORMED,	/* Still setting it up. */
};

struct mod_tree_node {
	struct module *mod;
	struct latch_tree_node node;
};

enum mod_mem_type {
	MOD_TEXT = 0,
	MOD_DATA,
	MOD_RODATA,
	MOD_RO_AFTER_INIT,
	MOD_INIT_TEXT,
	MOD_INIT_DATA,
	MOD_INIT_RODATA,

	MOD_MEM_NUM_TYPES,
	MOD_INVALID = -1,
};

#define mod_mem_type_is_init(type)	\
	((type) == MOD_INIT_TEXT ||	\
	 (type) == MOD_INIT_DATA ||	\
	 (type) == MOD_INIT_RODATA)

#define mod_mem_type_is_core(type) (!mod_mem_type_is_init(type))

#define mod_mem_type_is_text(type)	\
	 ((type) == MOD_TEXT ||		\
	  (type) == MOD_INIT_TEXT)

#define mod_mem_type_is_data(type) (!mod_mem_type_is_text(type))

#define mod_mem_type_is_core_data(type)	\
	(mod_mem_type_is_core(type) &&	\
	 mod_mem_type_is_data(type))

#define for_each_mod_mem_type(type)			\
	for (enum mod_mem_type (type) = 0;		\
	     (type) < MOD_MEM_NUM_TYPES; (type)++)

#define for_class_mod_mem_type(type, class)		\
	for_each_mod_mem_type(type)			\
		if (mod_mem_type_is_##class(type))

struct module_memory {
	void *base;
	unsigned int size;

#ifdef CONFIG_MODULES_TREE_LOOKUP
	struct mod_tree_node mtn;
#endif
};

#ifdef CONFIG_MODULES_TREE_LOOKUP
/* Only touch one cacheline for common rbtree-for-core-layout case. */
#define __module_memory_align ____cacheline_aligned
#else
#define __module_memory_align
#endif

struct mod_kallsyms {
	Elf_Sym *symtab;
	unsigned int num_symtab;
	char *strtab;
	char *typetab;
};

#ifdef CONFIG_LIVEPATCH
/**
 * struct klp_modinfo - ELF information preserved from the livepatch module
 *
 * @hdr: ELF header
 * @sechdrs: Section header table
 * @secstrings: String table for the section headers
 * @symndx: The symbol table section index
 */
struct klp_modinfo {
	Elf_Ehdr hdr;
	Elf_Shdr *sechdrs;
	char *secstrings;
	unsigned int symndx;
};
#endif

struct module {
	enum module_state state;

	/* Member of list of modules */
	struct list_head list;

	/* Unique handle for this module */
	char name[MODULE_NAME_LEN];

#ifdef CONFIG_STACKTRACE_BUILD_ID
	/* Module build ID */
	unsigned char build_id[BUILD_ID_SIZE_MAX];
#endif

	/* Sysfs stuff. */
	struct module_kobject mkobj;
	struct module_attribute *modinfo_attrs;
	const char *version;
	const char *srcversion;
	struct kobject *holders_dir;

	/* Exported symbols */
	const struct kernel_symbol *syms;
	const s32 *crcs;
	unsigned int num_syms;

#ifdef CONFIG_ARCH_USES_CFI_TRAPS
	s32 *kcfi_traps;
	s32 *kcfi_traps_end;
#endif

	/* Kernel parameters. */
#ifdef CONFIG_SYSFS
	struct mutex param_lock;
#endif
	struct kernel_param *kp;
	unsigned int num_kp;

	/* GPL-only exported symbols. */
	unsigned int num_gpl_syms;
	const struct kernel_symbol *gpl_syms;
	const s32 *gpl_crcs;
	bool using_gplonly_symbols;

#ifdef CONFIG_MODULE_SIG
	/* Signature was verified. */
	bool sig_ok;
#endif

	bool async_probe_requested;

	/* Exception table */
	unsigned int num_exentries;
	struct exception_table_entry *extable;

	/* Startup function. */
	int (*init)(void);

	struct module_memory mem[MOD_MEM_NUM_TYPES] __module_memory_align;

	/* Arch-specific module values */
	struct mod_arch_specific arch;

	unsigned long taints;	/* same bits as kernel:taint_flags */

#ifdef CONFIG_GENERIC_BUG
	/* Support for BUG */
	unsigned num_bugs;
	struct list_head bug_list;
	struct bug_entry *bug_table;
#endif

#ifdef CONFIG_KALLSYMS
	/* Protected by RCU and/or module_mutex: use rcu_dereference() */
	struct mod_kallsyms __rcu *kallsyms;
	struct mod_kallsyms core_kallsyms;

	/* Section attributes */
	struct module_sect_attrs *sect_attrs;

	/* Notes attributes */
	struct module_notes_attrs *notes_attrs;
#endif

	/* The command line arguments (may be mangled).  People like
	   keeping pointers to this stuff */
	char *args;

#ifdef CONFIG_SMP
	/* Per-cpu data. */
	void __percpu *percpu;
	unsigned int percpu_size;
#endif
	void *noinstr_text_start;
	unsigned int noinstr_text_size;

#ifdef CONFIG_TRACEPOINTS
	unsigned int num_tracepoints;
	tracepoint_ptr_t *tracepoints_ptrs;
#endif
#ifdef CONFIG_TREE_SRCU
	unsigned int num_srcu_structs;
	struct srcu_struct **srcu_struct_ptrs;
#endif
#ifdef CONFIG_BPF_EVENTS
	unsigned int num_bpf_raw_events;
	struct bpf_raw_event_map *bpf_raw_events;
#endif
#if 1
	unsigned int btf_data_size;
	unsigned int btf_base_data_size;
	void *btf_data;
	void *btf_base_data;
#endif
#ifdef CONFIG_JUMP_LABEL
	struct jump_entry *jump_entries;
	unsigned int num_jump_entries;
#endif
#ifdef CONFIG_TRACING
	unsigned int num_trace_bprintk_fmt;
	const char **trace_bprintk_fmt_start;
#endif
#ifdef CONFIG_EVENT_TRACING
	struct trace_event_call **trace_events;
	unsigned int num_trace_events;
	struct trace_eval_map **trace_evals;
	unsigned int num_trace_evals;
#endif
#ifdef CONFIG_FTRACE_MCOUNT_RECORD
	unsigned int num_ftrace_callsites;
	unsigned long *ftrace_callsites;
#endif
#ifdef CONFIG_KPROBES
	void *kprobes_text_start;
	unsigned int kprobes_text_size;
	unsigned long *kprobe_blacklist;
	unsigned int num_kprobe_blacklist;
#endif
#ifdef CONFIG_HAVE_STATIC_CALL_INLINE
	int num_static_call_sites;
	struct static_call_site *static_call_sites;
#endif
#if IS_ENABLED(CONFIG_KUNIT)
	int num_kunit_init_suites;
	struct kunit_suite **kunit_init_suites;
	int num_kunit_suites;
	struct kunit_suite **kunit_suites;
#endif


#ifdef CONFIG_LIVEPATCH
	bool klp; /* Is this a livepatch module? */
	bool klp_alive;

	/* ELF information */
	struct klp_modinfo *klp_info;
#endif

#ifdef CONFIG_PRINTK_INDEX
	unsigned int printk_index_size;
	struct pi_entry **printk_index_start;
#endif

#ifdef CONFIG_MODULE_UNLOAD
	/* What modules depend on me? */
	struct list_head source_list;
	/* What modules do I depend on? */
	struct list_head target_list;

	/* Destruction function. */
	void (*exit)(void);

	atomic_t refcnt;
#endif

#ifdef CONFIG_MITIGATION_ITS
	int its_num_pages;
	void **its_page_array;
#endif

#ifdef CONFIG_CONSTRUCTORS
	/* Constructor functions. */
	ctor_fn_t *ctors;
	unsigned int num_ctors;
#endif

#ifdef CONFIG_FUNCTION_ERROR_INJECTION
	struct error_injection_entry *ei_funcs;
	unsigned int num_ei_funcs;
#endif
#ifdef CONFIG_DYNAMIC_DEBUG_CORE
	struct _ddebug_info dyndbg_info;
#endif
} ____cacheline_aligned __randomize_layout;
#ifndef MODULE_ARCH_INIT
#define MODULE_ARCH_INIT {}
#endif

#ifndef HAVE_ARCH_KALLSYMS_SYMBOL_VALUE
static inline unsigned long kallsyms_symbol_value(const Elf_Sym *sym)
{
	return sym->st_value;
}
#endif

/* FIXME: It'd be nice to isolate modules during init, too, so they
   aren't used before they (may) fail.  But presently too much code
   (IDE & SCSI) require entry into the module during init.*/
static inline bool module_is_live(struct module *mod)
{
	return mod->state != MODULE_STATE_GOING;
}

static inline bool module_is_coming(struct module *mod)
{
        return mod->state == MODULE_STATE_COMING;
}

struct module *__module_text_address(unsigned long addr);
struct module *__module_address(unsigned long addr);
bool is_module_address(unsigned long addr);
bool __is_module_percpu_address(unsigned long addr, unsigned long *can_addr);
bool is_module_percpu_address(unsigned long addr);
bool is_module_text_address(unsigned long addr);

static inline bool within_module_mem_type(unsigned long addr,
					  const struct module *mod,
					  enum mod_mem_type type)
{
	unsigned long base, size;

	base = (unsigned long)mod->mem[type].base;
	size = mod->mem[type].size;
	return addr - base < size;
}

static inline bool within_module_core(unsigned long addr,
				      const struct module *mod)
{
	for_class_mod_mem_type(type, core) {
		if (within_module_mem_type(addr, mod, type))
			return true;
	}
	return false;
}

static inline bool within_module_init(unsigned long addr,
				      const struct module *mod)
{
	for_class_mod_mem_type(type, init) {
		if (within_module_mem_type(addr, mod, type))
			return true;
	}
	return false;
}

static inline bool within_module(unsigned long addr, const struct module *mod)
{
	return within_module_init(addr, mod) || within_module_core(addr, mod);
}

/* Search for module by name: must be in a RCU-sched critical section. */
struct module *find_module(const char *name);

extern void __noreturn __module_put_and_kthread_exit(struct module *mod,
			long code);
#define module_put_and_kthread_exit(code) __module_put_and_kthread_exit(THIS_MODULE, code)

#ifdef CONFIG_MODULE_UNLOAD
int module_refcount(struct module *mod);
void __symbol_put(const char *symbol);
#define symbol_put(x) __symbol_put(__stringify(x))
void symbol_put_addr(void *addr);

/* Sometimes we know we already have a refcount, and it's easier not
   to handle the error case (which only happens with rmmod --wait). */
extern void __module_get(struct module *module);

/**
 * try_module_get() - take module refcount unless module is being removed
 * @module: the module we should check for
 *
 * Only try to get a module reference count if the module is not being removed.
 * This call will fail if the module is in the process of being removed.
 *
 * Care must also be taken to ensure the module exists and is alive prior to
 * usage of this call. This can be gauranteed through two means:
 *
 * 1) Direct protection: you know an earlier caller must have increased the
 *    module reference through __module_get(). This can typically be achieved
 *    by having another entity other than the module itself increment the
 *    module reference count.
 *
 * 2) Implied protection: there is an implied protection against module
 *    removal. An example of this is the implied protection used by kernfs /
 *    sysfs. The sysfs store / read file operations are guaranteed to exist
 *    through the use of kernfs's active reference (see kernfs_active()) and a
 *    sysfs / kernfs file removal cannot happen unless the same file is not
 *    active. Therefore, if a sysfs file is being read or written to the module
 *    which created it must still exist. It is therefore safe to use
 *    try_module_get() on module sysfs store / read ops.
 *
 * One of the real values to try_module_get() is the module_is_live() check
 * which ensures that the caller of try_module_get() can yield to userspace
 * module removal requests and gracefully fail if the module is on its way out.
 *
 * Returns true if the reference count was successfully incremented.
 */
extern bool try_module_get(struct module *module);

/**
 * module_put() - release a reference count to a module
 * @module: the module we should release a reference count for
 *
 * If you successfully bump a reference count to a module with try_module_get(),
 * when you are finished you must call module_put() to release that reference
 * count.
 */
extern void module_put(struct module *module);

#else /*!CONFIG_MODULE_UNLOAD*/
static inline bool try_module_get(struct module *module)
{
	return !module || module_is_live(module);
}
static inline void module_put(struct module *module)
{
}
static inline void __module_get(struct module *module)
{
}
#define symbol_put(x) do { } while (0)
#define symbol_put_addr(p) do { } while (0)

#endif /* CONFIG_MODULE_UNLOAD */

/* This is a #define so the string doesn't get put in every .o file */
#define module_name(mod)			\
({						\
	struct module *__mod = (mod);		\
	__mod ? __mod->name : "kernel";		\
})

/* Dereference module function descriptor */
void *dereference_module_function_descriptor(struct module *mod, void *ptr);

int register_module_notifier(struct notifier_block *nb);
int unregister_module_notifier(struct notifier_block *nb);

extern void print_modules(void);

static inline bool module_requested_async_probing(struct module *module)
{
	return module && module->async_probe_requested;
}

static inline bool is_livepatch_module(struct module *mod)
{
#ifdef CONFIG_LIVEPATCH
	return mod->klp;
#else
	return false;
#endif
}

void set_module_sig_enforced(void);

#else /* !CONFIG_MODULES... */

static inline struct module *__module_address(unsigned long addr)
{
	return NULL;
}

static inline struct module *__module_text_address(unsigned long addr)
{
	return NULL;
}

static inline bool is_module_address(unsigned long addr)
{
	return false;
}

static inline bool is_module_percpu_address(unsigned long addr)
{
	return false;
}

static inline bool __is_module_percpu_address(unsigned long addr, unsigned long *can_addr)
{
	return false;
}

static inline bool is_module_text_address(unsigned long addr)
{
	return false;
}

static inline bool within_module_core(unsigned long addr,
				      const struct module *mod)
{
	return false;
}

static inline bool within_module_init(unsigned long addr,
				      const struct module *mod)
{
	return false;
}

static inline bool within_module(unsigned long addr, const struct module *mod)
{
	return false;
}

/* Get/put a kernel symbol (calls should be symmetric) */
#define symbol_get(x) ({ extern typeof(x) x __attribute__((weak,visibility("hidden"))); &(x); })
#define symbol_put(x) do { } while (0)
#define symbol_put_addr(x) do { } while (0)

static inline void __module_get(struct module *module)
{
}

static inline bool try_module_get(struct module *module)
{
	return true;
}

static inline void module_put(struct module *module)
{
}

#define module_name(mod) "kernel"

static inline int register_module_notifier(struct notifier_block *nb)
{
	/* no events will happen anyway, so this can always succeed */
	return 0;
}

static inline int unregister_module_notifier(struct notifier_block *nb)
{
	return 0;
}

#define module_put_and_kthread_exit(code) kthread_exit(code)

static inline void print_modules(void)
{
}

static inline bool module_requested_async_probing(struct module *module)
{
	return false;
}


static inline void set_module_sig_enforced(void)
{
}

/* Dereference module function descriptor */
static inline
void *dereference_module_function_descriptor(struct module *mod, void *ptr)
{
	return ptr;
}

static inline bool module_is_coming(struct module *mod)
{
	return false;
}
#endif /* CONFIG_MODULES */

#ifdef CONFIG_SYSFS
extern struct kset *module_kset;
extern const struct kobj_type module_ktype;
#endif /* CONFIG_SYSFS */

#define symbol_request(x) try_then_request_module(symbol_get(x), "symbol:" #x)

/* BELOW HERE ALL THESE ARE OBSOLETE AND WILL VANISH */

#define __MODULE_STRING(x) __stringify(x)

#ifdef CONFIG_GENERIC_BUG
void module_bug_finalize(const Elf_Ehdr *, const Elf_Shdr *,
			 struct module *);
void module_bug_cleanup(struct module *);

#else	/* !CONFIG_GENERIC_BUG */

static inline void module_bug_finalize(const Elf_Ehdr *hdr,
					const Elf_Shdr *sechdrs,
					struct module *mod)
{
}
static inline void module_bug_cleanup(struct module *mod) {}
#endif	/* CONFIG_GENERIC_BUG */

#ifdef CONFIG_MITIGATION_RETPOLINE
extern bool retpoline_module_ok(bool has_retpoline);
#else
static inline bool retpoline_module_ok(bool has_retpoline)
{
	return true;
}
#endif

#ifdef CONFIG_MODULE_SIG
bool is_module_sig_enforced(void);

static inline bool module_sig_ok(struct module *module)
{
	return module->sig_ok;
}
#else	/* !CONFIG_MODULE_SIG */
static inline bool is_module_sig_enforced(void)
{
	return false;
}

static inline bool module_sig_ok(struct module *module)
{
	return true;
}
#endif	/* CONFIG_MODULE_SIG */

#if defined(CONFIG_MODULES) && defined(CONFIG_KALLSYMS)
int module_kallsyms_on_each_symbol(const char *modname,
				   int (*fn)(void *, const char *, unsigned long),
				   void *data);

/* For kallsyms to ask for address resolution.  namebuf should be at
 * least KSYM_NAME_LEN long: a pointer to namebuf is returned if
 * found, otherwise NULL.
 */
int module_address_lookup(unsigned long addr,
			  unsigned long *symbolsize,
			  unsigned long *offset,
			  char **modname, const unsigned char **modbuildid,
			  char *namebuf);
int lookup_module_symbol_name(unsigned long addr, char *symname);
int lookup_module_symbol_attrs(unsigned long addr,
			       unsigned long *size,
			       unsigned long *offset,
			       char *modname,
			       char *name);

/* Returns 0 and fills in value, defined and namebuf, or -ERANGE if
 * symnum out of range.
 */
int module_get_kallsym(unsigned int symnum, unsigned long *value, char *type,
		       char *name, char *module_name, int *exported);

/* Look for this name: can be of form module:name. */
unsigned long module_kallsyms_lookup_name(const char *name);

unsigned long find_kallsyms_symbol_value(struct module *mod, const char *name);

#else	/* CONFIG_MODULES && CONFIG_KALLSYMS */

static inline int module_kallsyms_on_each_symbol(const char *modname,
						 int (*fn)(void *, const char *, unsigned long),
						 void *data)
{
	return -EOPNOTSUPP;
}

/* For kallsyms to ask for address resolution.  NULL means not found. */
static inline int module_address_lookup(unsigned long addr,
						unsigned long *symbolsize,
						unsigned long *offset,
						char **modname,
						const unsigned char **modbuildid,
						char *namebuf)
{
	return 0;
}

static inline int lookup_module_symbol_name(unsigned long addr, char *symname)
{
	return -ERANGE;
}

static inline int module_get_kallsym(unsigned int symnum, unsigned long *value,
				     char *type, char *name,
				     char *module_name, int *exported)
{
	return -ERANGE;
}

static inline unsigned long module_kallsyms_lookup_name(const char *name)
{
	return 0;
}

static inline unsigned long find_kallsyms_symbol_value(struct module *mod,
						       const char *name)
{
	return 0;
}

#endif  /* CONFIG_MODULES && CONFIG_KALLSYMS */

#endif /* _LINUX_MODULE_H */
