/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * aesce-ccm-core.S - AES-CCM transform for ARMv8 with Crypto Extensions
 *
 * Copyright (C) 2013 - 2017 Linaro Ltd <ard.biesheuvel@linaro.org>
 */

#include <linux/linkage.h>
#include <asm/assembler.h>

.text
.arch	armv8-a+crypto

// AES-CCM Authentication Data
SYM_FUNC_START(ce_aes_ccm_auth_data)
    ld1 {v0.16b}, [x0]               // Load MAC
    cbz w3, 1f
    sub w3, w3, #16
    eor v1.16b, v1.16b, v1.16b

0:  ldrb w7, [x1], #1                // Get 1 byte of input
    subs w2, w2, #1
    add w3, w3, #1
    ins v1.b[0], w7
    ext v1.16b, v1.16b, v1.16b, #1   // Rotate in the input bytes
    beq 8f                           // Out of input?
    cbnz w3, 0b
    eor v0.16b, v0.16b, v1.16b

1:  ld1 {v3.4s}, [x4]                 // Load first round key
    prfm pldl1strm, [x1]
    cmp w5, #12                       // Which key size?
    add x6, x4, #16
    sub w7, w5, #2                     // Modified # of rounds
    bmi 2f
    bne 5f
    mov v5.16b, v3.16b
    b 4f

2:  mov v4.16b, v3.16b
    ld1 {v5.4s}, [x6], #16            // Load 2nd round key

3:  aese v0.16b, v4.16b
    aesmc v0.16b, v0.16b

4:  ld1 {v3.4s}, [x6], #16            // Load next round key
    aese v0.16b, v5.16b
    aesmc v0.16b, v0.16b

5:  ld1 {v4.4s}, [x6], #16            // Load next round key
    subs w7, w7, #3
    aese v0.16b, v3.16b
    aesmc v0.16b, v0.16b
    ld1 {v5.4s}, [x6], #16            // Load next round key
    bpl 3b

6:  aese v0.16b, v4.16b
    subs w2, w2, #16                   // Last data?
    eor v0.16b, v0.16b, v5.16b        // Final round
    bmi 6f
    ld1 {v1.16b}, [x1], #16           // Load next input block
    eor v0.16b, v0.16b, v1.16b        // XOR with MAC
    bne 1b

6:  st1 {v0.16b}, [x0]                // Store MAC
    beq 10f
    adds w2, w2, #16
    beq 10f
    mov w3, w2

7:  ldrb w7, [x1], #1
    umov w6, v0.b[0]
    eor w6, w6, w7
    strb w6, [x0], #1
    subs w2, w2, #1
    beq 10f
    ext v0.16b, v0.16b, v0.16b, #1    // Rotate out the MAC bytes
    b 7b

8:  cbz w3, 91f
    mov w7, w3
    add w3, w3, #16

9:  ext v1.16b, v1.16b, v1.16b, #1
    adds w7, w7, #1
    bne 9b

91: eor v0.16b, v0.16b, v1.16b
    st1 {v0.16b}, [x0]

10: mov w0, w3
    ret
SYM_FUNC_END(ce_aes_ccm_auth_data)

// AES-CCM Final
SYM_FUNC_START(ce_aes_ccm_final)
    ld1 {v3.4s}, [x2], #16            // Load first round key
    ld1 {v0.16b}, [x0]                // Load MAC
    cmp w3, #12                       // Which key size?
    sub w3, w3, #2                     // Modified # of rounds
    ld1 {v1.16b}, [x1]                 // Load 1st ctriv
    bmi 0f
    bne 3f
    mov v5.16b, v3.16b
    b 2f

0:  mov v4.16b, v3.16b
1:  ld1 {v5.4s}, [x2], #16            //
