/*
 * Copyright (c) 2015 Etnaviv Project
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sub license,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *    Christian Gmeiner <christian.gmeiner@gmail.com>
 */

#include <sys/stat.h>

#include "util/u_screen.h"

#include "etnaviv/etnaviv_screen.h"
#include "etnaviv_drm_public.h"

#include <stdio.h>


static struct pipe_screen *
screen_create(int gpu_fd, const struct pipe_screen_config *config, struct renderonly *ro)
{
   struct etna_device *dev;
   struct etna_gpu *gpu = NULL;
   struct etna_gpu *npu = NULL;
   int i;

   dev = etna_device_new_dup(gpu_fd);
   if (!dev) {
      fprintf(stderr, "Error creating device\n");
      return NULL;
   }

   for (i = 0; !gpu || !npu; i++) {
      struct etna_core_info *info;
      struct etna_gpu *core = etna_gpu_new(dev, i);

      if (!core)
         break;

      info = etna_gpu_get_core_info(core);
      switch (info->type) {
      case ETNA_CORE_GPU:
         /* Look for a 3D capable GPU */
         if (!gpu && etna_core_has_feature(info, ETNA_FEATURE_PIPE_3D)) {
            gpu = core;
            continue;
         }
         break;
      case ETNA_CORE_NPU:
         if (!npu) {
            npu = core;
            continue;
         }
         break;
      default:
         unreachable("invalid core type");
      }

      etna_gpu_del(core);
   }

   if (!gpu && !npu) {
      fprintf(stderr, "Error creating gpu or npu\n");
      return NULL;
   }

   return etna_screen_create(dev, gpu, npu, ro);
}

struct pipe_screen *
etna_drm_screen_create_renderonly(int fd, struct renderonly *ro,
                                  const struct pipe_screen_config *config)
{
   return u_pipe_screen_lookup_or_create(fd, config, ro, screen_create);
}

struct pipe_screen *
etna_drm_screen_create(int fd)
{
   return u_pipe_screen_lookup_or_create(fd, NULL, NULL, screen_create);
}
