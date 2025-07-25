/* SPDX-License-Identifier: GPL-2.0 */

/* Firmware attributes class helper module */

#ifndef FW_ATTR_CLASS_H
#define FW_ATTR_CLASS_H

#include <linux/device/class.h>

extern const struct class firmware_attributes_class;
int fw_attributes_class_get(const struct class **fw_attr_class);
int fw_attributes_class_put(void);

#endif /* FW_ATTR_CLASS_H */
