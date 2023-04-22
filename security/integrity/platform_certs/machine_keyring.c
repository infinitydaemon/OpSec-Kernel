// SPDX-License-Identifier: GPL-2.0
/*
 * Machine keyring routines.
 *
 * Copyright (c) 2021, Oracle and/or its affiliates.
 */

#include <linux/efi.h>
#include "../integrity.h"

#include "machine_keyring.h"

static bool trust_mok;

int machine_keyring_init(void)
{
    int rc;

    rc = integrity_init_keyring(INTEGRITY_KEYRING_MACHINE);
    if (rc)
        return rc;

    pr_notice("Machine keyring initialized\n");
    return 0;
}

void add_to_machine_keyring(const char *source, const void *data, size_t len)
{
    key_perm_t perm;
    int rc;

    perm = (KEY_POS_ALL & ~KEY_POS_SETATTR) | KEY_USR_VIEW;
    rc = integrity_load_cert(INTEGRITY_KEYRING_MACHINE, source, data, len, perm);

    /*
     * Some MOKList keys may not pass the machine keyring restrictions.
     * If the restriction check does not pass and the platform keyring
     * is configured, try to add it into that keyring instead.
     */
    if (rc && IS_ENABLED(CONFIG_INTEGRITY_PLATFORM_KEYRING))
        rc = integrity_load_cert(INTEGRITY_KEYRING_PLATFORM, source,
                                 data, len, perm);

    if (rc)
        pr_info("Error adding keys to machine keyring %s\n", source);
}

bool trust_moklist(void)
{
    static bool initialized;

    if (!initialized) {
        initialized = true;

        struct efi_mokvar_table_entry *mokvar_entry;
        mokvar_entry = efi_mokvar_entry_find("MokListTrustedRT");

        if (mokvar_entry)
            trust_mok = true;
    }

    return trust_mok;
}

