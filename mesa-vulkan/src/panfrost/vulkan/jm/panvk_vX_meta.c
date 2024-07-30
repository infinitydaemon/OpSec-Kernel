/*
 * Copyright © 2021 Collabora Ltd.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "genxml/gen_macros.h"

#include "nir/nir_builder.h"
#include "pan_encoder.h"
#include "pan_shader.h"

#include "panvk_device.h"

#include "vk_format.h"

mali_ptr
panvk_per_arch(meta_emit_viewport)(struct pan_pool *pool, uint16_t minx,
                                   uint16_t miny, uint16_t maxx, uint16_t maxy)
{
   struct panfrost_ptr vp = pan_pool_alloc_desc(pool, VIEWPORT);

   pan_pack(vp.cpu, VIEWPORT, cfg) {
      cfg.scissor_minimum_x = minx;
      cfg.scissor_minimum_y = miny;
      cfg.scissor_maximum_x = maxx;
      cfg.scissor_maximum_y = maxy;
   }

   return vp.gpu;
}

void
panvk_per_arch(meta_init)(struct panvk_device *dev)
{
   struct panvk_pool_properties bin_pool_props = {
      .create_flags = PAN_KMOD_BO_FLAG_EXECUTABLE,
      .slab_size = 16 * 1024,
      .label = "panvk_meta binary pool",
      .prealloc = false,
      .owns_bos = true,
      .needs_locking = false,
   };
   struct panvk_pool_properties desc_pool_props = {
      .create_flags = 0,
      .slab_size = 16 * 1024,
      .label = "panvk_meta descriptor pool",
      .prealloc = false,
      .owns_bos = true,
      .needs_locking = false,
   };

   panvk_pool_init(&dev->meta.bin_pool, dev, NULL, &bin_pool_props);
   panvk_pool_init(&dev->meta.desc_pool, dev, NULL, &desc_pool_props);
   panvk_per_arch(meta_blit_init)(dev);
   panvk_per_arch(meta_copy_init)(dev);
   panvk_per_arch(meta_clear_init)(dev);
   panvk_per_arch(meta_desc_copy_init)(dev);
}

void
panvk_per_arch(meta_cleanup)(struct panvk_device *dev)
{
   panvk_per_arch(meta_blit_cleanup)(dev);
   panvk_pool_cleanup(&dev->meta.desc_pool);
   panvk_pool_cleanup(&dev->meta.bin_pool);
}
