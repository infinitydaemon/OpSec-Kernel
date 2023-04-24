#ifndef __RPI4_ESC_PROTECT_H
#define __RPI4_ESC_PROTECT_H

#include <linux/sched.h>
#include <linux/ptrace.h>

/* Prevent process from attaching to other processes with ptrace */
#define disallow_ptrace() prctl(PR_SET_DUMPABLE, 0)

/* Prevent process from forking */
#define disallow_fork() current->flags |= PF_EXITING

/* Prevent process from executing code in data sections */
#define disallow_exec_data() \
    current->mm->def_flags &= ~(VM_READ | VM_EXEC | VM_WRITE)

/* Prevent process from accessing kernel memory */
#define disallow_kernel_access() \
    current->mm->def_flags &= ~(VM_READ | VM_WRITE | VM_EXEC)

/* Disable the use of setuid and setgid bits */
#define disallow_setuid_gid() \
    current->cred->euid = current->cred->uid; \
    current->cred->egid = current->cred->gid; \
    current->cred->suid = current->cred->uid; \
    current->cred->sgid = current->cred->gid; \
    current->cred->fsuid = current->cred->uid; \
    current->cred->fsgid = current->cred->gid

#endif /* __RPI4_ESC_PROTECT_H */
