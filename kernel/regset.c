// SPDX-License-Identifier: GPL-2.0-only
#include <linux/export.h>
#include <linux/slab.h>
#include <linux/regset.h>

static int __regset_get(struct task_struct *target,
                        const struct user_regset *regset,
                        unsigned int size,
                        void **data)
{
    int res = 0;
    void *to_free = NULL, *p = NULL;

    if (!regset->regset_get)
        return -EOPNOTSUPP;

    if (size > regset->n * regset->size)
        size = regset->n * regset->size;

    if (!*data) {
        to_free = *data = kzalloc(size, GFP_KERNEL);
        if (!*data)
            return -ENOMEM;
    }
    p = *data;

    struct membuf mem_buf = {
        .p = p,
        .left = size
    };
    res = regset->regset_get(target, regset, mem_buf);

    if (res < 0) {
        kfree(to_free);
        *data = NULL;
        return res;
    }

    return size - res;
}

int regset_get(struct task_struct *target,
               const struct user_regset *regset,
               unsigned int size,
               void *data)
{
    return __regset_get(target, regset, size, &data);
}
EXPORT_SYMBOL(regset_get);

int regset_get_alloc(struct task_struct *target,
                     const struct user_regset *regset,
                     unsigned int size,
                     void **data)
{
    *data = NULL;
    return __regset_get(target, regset, size, data);
}
EXPORT_SYMBOL(regset_get_alloc);

int copy_regset_to_user(struct task_struct *target,
                        const struct user_regset_view *view,
                        unsigned int setno,
                        unsigned int offset,
                        unsigned int size,
                        void __user *data)
{
    const struct user_regset *regset = &view->regsets[setno];
    void *buf = NULL;
    int ret = 0;

    ret = regset_get_alloc(target, regset, size, &buf);
    if (ret > 0) {
        if (copy_to_user(data, buf, ret)) {
            ret = -EFAULT;
        }
    }
    kfree(buf);
    return ret;
}
