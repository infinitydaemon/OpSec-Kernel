/*
 * Copyright 2022 Alyssa Rosenzweig
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "asahi/genxml/agx_pack.h"
#include "agx_bo.h"

/* Opaque structure representing a PPP update */
struct agx_ppp_update {
   uint8_t *head;
   uint64_t gpu_base;
   size_t total_size;

#ifndef NDEBUG
   uint8_t *cpu_base;
#endif
};

ALWAYS_INLINE static size_t
agx_ppp_update_size(struct AGX_PPP_HEADER *present)
{
   size_t size = AGX_PPP_HEADER_LENGTH;

#define PPP_CASE(x, y)                                                         \
   if (present->x)                                                             \
      size += AGX_##y##_LENGTH;
   PPP_CASE(fragment_control, FRAGMENT_CONTROL);
   PPP_CASE(fragment_control_2, FRAGMENT_CONTROL);
   PPP_CASE(fragment_front_face, FRAGMENT_FACE);
   PPP_CASE(fragment_front_face_2, FRAGMENT_FACE_2);
   PPP_CASE(fragment_front_stencil, FRAGMENT_STENCIL);
   PPP_CASE(fragment_back_face, FRAGMENT_FACE);
   PPP_CASE(fragment_back_face_2, FRAGMENT_FACE_2);
   PPP_CASE(fragment_back_stencil, FRAGMENT_STENCIL);
   PPP_CASE(depth_bias_scissor, DEPTH_BIAS_SCISSOR);

   if (present->region_clip)
      size += present->viewport_count * AGX_REGION_CLIP_LENGTH;

   if (present->viewport) {
      size += AGX_VIEWPORT_CONTROL_LENGTH +
              (present->viewport_count * AGX_VIEWPORT_LENGTH);
   }

   PPP_CASE(w_clamp, W_CLAMP);
   PPP_CASE(output_select, OUTPUT_SELECT);
   PPP_CASE(varying_counts_32, VARYING_COUNTS);
   PPP_CASE(varying_counts_16, VARYING_COUNTS);
   PPP_CASE(cull, CULL);
   PPP_CASE(cull_2, CULL_2);

   if (present->fragment_shader) {
      size +=
         AGX_FRAGMENT_SHADER_WORD_0_LENGTH + AGX_FRAGMENT_SHADER_WORD_1_LENGTH +
         AGX_FRAGMENT_SHADER_WORD_2_LENGTH + AGX_FRAGMENT_SHADER_WORD_3_LENGTH;
   }

   PPP_CASE(occlusion_query, FRAGMENT_OCCLUSION_QUERY);
   PPP_CASE(occlusion_query_2, FRAGMENT_OCCLUSION_QUERY_2);
   PPP_CASE(output_unknown, OUTPUT_UNKNOWN);
   PPP_CASE(output_size, OUTPUT_SIZE);
   PPP_CASE(varying_word_2, VARYING_2);
#undef PPP_CASE

   assert((size % 4) == 0 && "PPP updates are aligned");
   return size;
}

static inline bool
agx_ppp_validate(struct agx_ppp_update *ppp, size_t size)
{
#ifndef NDEBUG
   /* Assert that we don't overflow. Ideally we'd assert that types match too
    * but that's harder to do at the moment.
    */
   assert(((ppp->head - ppp->cpu_base) + size) <= ppp->total_size);
#endif

   return true;
}

#define agx_ppp_push(ppp, T, name)                                             \
   for (bool it = agx_ppp_validate((ppp), AGX_##T##_LENGTH); it;               \
        it = false, (ppp)->head += AGX_##T##_LENGTH)                           \
      agx_pack((ppp)->head, T, name)

#define agx_ppp_push_packed(ppp, src, T)                                       \
   do {                                                                        \
      agx_ppp_validate((ppp), AGX_##T##_LENGTH);                               \
      memcpy((ppp)->head, src, AGX_##T##_LENGTH);                              \
      (ppp)->head += AGX_##T##_LENGTH;                                         \
   } while (0)

#define agx_ppp_push_merged(ppp, T, name, merge)                               \
   for (uint8_t _tmp[AGX_##T##_LENGTH], it = 1; it;                            \
        it = 0, agx_ppp_push_merged_blobs(ppp, AGX_##T##_LENGTH,               \
                                          (uint32_t *)_tmp,                    \
                                          (uint32_t *)&merge))                 \
      agx_pack(_tmp, T, name)

ALWAYS_INLINE static struct agx_ppp_update
agx_new_ppp_update(struct agx_ptr out, size_t size,
                   struct AGX_PPP_HEADER *present)
{
   struct agx_ppp_update ppp = {
      .head = out.cpu,
      .gpu_base = out.gpu,
      .total_size = size,
#ifndef NDEBUG
      .cpu_base = out.cpu,
#endif
   };

   agx_ppp_push(&ppp, PPP_HEADER, cfg) {
      cfg = *present;
   }

   return ppp;
}

static inline void
agx_ppp_fini(uint8_t **out, struct agx_ppp_update *ppp)
{
   size_t size = ppp->total_size;
   assert((size % 4) == 0);
   size_t size_words = size / 4;

#ifndef NDEBUG
   assert(size == (ppp->head - ppp->cpu_base) && "mismatched ppp size");
#endif

   assert(ppp->gpu_base < (1ull << 40));
   assert(size_words < (1ull << 24));

   agx_pack(*out, PPP_STATE, cfg) {
      cfg.pointer_hi = (ppp->gpu_base >> 32);
      cfg.pointer_lo = (uint32_t)ppp->gpu_base;
      cfg.size_words = size_words;
   };

   *out += AGX_PPP_STATE_LENGTH;
}

static void
agx_ppp_push_merged_blobs(struct agx_ppp_update *ppp, size_t length,
                          void *src1_, void *src2_)
{
   assert((length & 3) == 0);
   assert(((uintptr_t)src1_ & 3) == 0);
   assert(((uintptr_t)src2_ & 3) == 0);

   uint32_t *dst = (uint32_t *)ppp->head;
   uint32_t *src1 = (uint32_t *)src1_;
   uint32_t *src2 = (uint32_t *)src2_;

   for (unsigned i = 0; i < (length / 4); ++i) {
      dst[i] = src1[i] | src2[i];
   }

   ppp->head += length;
}
