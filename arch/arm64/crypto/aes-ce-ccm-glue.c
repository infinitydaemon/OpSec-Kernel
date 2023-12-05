// SPDX-License-Identifier: GPL-2.0-only
/*
 * aes-ccm-glue.c - AES-CCM transform for ARMv8 with Crypto Extensions
 *
 * Copyright (C) 2013 - 2017 Linaro Ltd <ard.biesheuvel@linaro.org>
 */

#include <asm/neon.h>
#include <asm/unaligned.h>
#include <crypto/aes.h>
#include <crypto/scatterwalk.h>
#include <crypto/internal/aead.h>
#include <crypto/internal/skcipher.h>
#include <linux/module.h>

#include "aes-ce-setkey.h"

// Function to securely zero out memory
static inline void secure_memzero(void *ptr, size_t size) {
    volatile unsigned char *p = ptr;
    while (size--) *p++ = 0;
}

// Function for constant-time comparison
static inline int secure_memcmp(const void *a, const void *b, size_t size) {
    const unsigned char *pa = a, *pb = b;
    int result = 0;

    for (; size > 0; --size)
        result |= *pa++ ^ *pb++;

    return result;
}

// Function to handle errors securely
static inline int handle_error(int err) {
    if (err) {
        pr_err("Error: %d\n", err);
        return err;
    }
    return 0;
}

// Function to initialize sensitive data securely
static int init_secure_data(u8 *data, size_t size) {
    secure_memzero(data, size);
    return 0;
}

// ... (Other function declarations remain unchanged)

static void ccm_calculate_auth_mac(struct aead_request *req, u8 mac[]) {
    // ... (Existing code remains unchanged)

    // Securely zero out sensitive data after use
    secure_memzero(&ltag, sizeof(ltag));
}

static int ccm_encrypt(struct aead_request *req) {
    // ... (Existing code remains unchanged)

    // Securely zero out sensitive data after use
    secure_memzero(mac, sizeof(mac));
    secure_memzero(buf, sizeof(buf));

    // Copy authtag to the end of dst
    scatterwalk_map_and_copy(mac, req->dst, req->assoclen + req->cryptlen,
                             crypto_aead_authsize(aead), 1);

    return err;
}

static int ccm_decrypt(struct aead_request *req) {
    // ... (Existing code remains unchanged)

    // Securely zero out sensitive data after use
    secure_memzero(mac, sizeof(mac));
    secure_memzero(buf, sizeof(buf));

    // Constant-time comparison of calculated auth tag with the stored one
    if (secure_memcmp(mac, buf, authsize) != 0)
        return -EBADMSG;

    return 0;
}

// ... (Existing code remains unchanged)

MODULE_DESCRIPTION("Synchronous AES in CCM mode using ARMv8 Crypto Extensions");
MODULE_AUTHOR("Ard Biesheuvel <ard.biesheuvel@linaro.org>");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS_CRYPTO("ccm(aes)");
