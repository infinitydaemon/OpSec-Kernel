/*
 * Copyright © 2015 Intel Corporation
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
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include "util/u_printf.h"
#include <sys/stat.h>

#include "anv_private.h"
#include "vk_enum_to_str.h"

void
__anv_perf_warn(struct anv_device *device,
                const struct vk_object_base *object,
                const char *file, int line, const char *format, ...)
{
   va_list ap;
   char buffer[256];

   va_start(ap, format);
   vsnprintf(buffer, sizeof(buffer), format, ap);
   va_end(ap);

   if (object) {
      __vk_log(VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT,
               VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
               VK_LOG_OBJS(object), file, line,
               "PERF: %s", buffer);
   } else {
      __vk_log(VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT,
               VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
               VK_LOG_NO_OBJS(device->physical->instance), file, line,
               "PERF: %s", buffer);
   }
}

void
anv_cmd_buffer_pending_pipe_debug(struct anv_cmd_buffer *cmd_buffer,
                                  enum anv_pipe_bits bits,
                                  const char* reason)
{
   if (bits == 0)
      return;

   fprintf(stdout, "acc: ");

   fprintf(stdout, "bits: ");
   anv_dump_pipe_bits(bits, stdout);
   fprintf(stdout, "reason: %s", reason);

   if (cmd_buffer->batch.pc_reasons_count < ARRAY_SIZE(cmd_buffer->batch.pc_reasons))
      cmd_buffer->batch.pc_reasons[cmd_buffer->batch.pc_reasons_count++] = reason;
   fprintf(stdout, "\n");
}

void
anv_dump_pipe_bits(enum anv_pipe_bits bits, FILE *f)
{
   if (bits & ANV_PIPE_DEPTH_CACHE_FLUSH_BIT)
      fputs("+depth_flush ", f);
   if (bits & ANV_PIPE_DATA_CACHE_FLUSH_BIT)
      fputs("+dc_flush ", f);
   if (bits & ANV_PIPE_HDC_PIPELINE_FLUSH_BIT)
      fputs("+hdc_flush ", f);
   if (bits & ANV_PIPE_RENDER_TARGET_CACHE_FLUSH_BIT)
      fputs("+rt_flush ", f);
   if (bits & ANV_PIPE_TILE_CACHE_FLUSH_BIT)
      fputs("+tile_flush ", f);
   if (bits & ANV_PIPE_L3_FABRIC_FLUSH_BIT)
      fputs("+l3_fabric_flush ", f);
   if (bits & ANV_PIPE_STATE_CACHE_INVALIDATE_BIT)
      fputs("+state_inval ", f);
   if (bits & ANV_PIPE_CONSTANT_CACHE_INVALIDATE_BIT)
      fputs("+const_inval ", f);
   if (bits & ANV_PIPE_VF_CACHE_INVALIDATE_BIT)
      fputs("+vf_inval ", f);
   if (bits & ANV_PIPE_TEXTURE_CACHE_INVALIDATE_BIT)
      fputs("+tex_inval ", f);
   if (bits & ANV_PIPE_INSTRUCTION_CACHE_INVALIDATE_BIT)
      fputs("+ic_inval ", f);
   if (bits & ANV_PIPE_STALL_AT_SCOREBOARD_BIT)
      fputs("+pb_stall ", f);
   if (bits & ANV_PIPE_PSS_STALL_SYNC_BIT)
      fputs("+pss_stall ", f);
   if (bits & ANV_PIPE_DEPTH_STALL_BIT)
      fputs("+depth_stall ", f);
   if (bits & ANV_PIPE_CS_STALL_BIT ||
       bits & ANV_PIPE_END_OF_PIPE_SYNC_BIT)
      fputs("+cs_stall ", f);
   if (bits & ANV_PIPE_UNTYPED_DATAPORT_CACHE_FLUSH_BIT)
      fputs("+utdp_flush ", f);
   if (bits & ANV_PIPE_CCS_CACHE_FLUSH_BIT)
      fputs("+ccs_flush ", f);
}

const char *
anv_gfx_state_bit_to_str(enum anv_gfx_state_bits state)
{
#define NAME(name) case ANV_GFX_STATE_##name: return #name;
   switch (state) {
      NAME(URB);
      NAME(VF_STATISTICS);
      NAME(VF_SGVS);
      NAME(VF_SGVS_2);
      NAME(VF_SGVS_INSTANCING);
      NAME(PRIMITIVE_REPLICATION);
      NAME(MULTISAMPLE);
      NAME(SBE);
      NAME(SBE_SWIZ);
      NAME(SO_DECL_LIST);
      NAME(VS);
      NAME(HS);
      NAME(DS);
      NAME(GS);
      NAME(PS);
      NAME(PS_EXTRA);
      NAME(SBE_MESH);
      NAME(CLIP_MESH);
      NAME(MESH_CONTROL);
      NAME(MESH_SHADER);
      NAME(MESH_DISTRIB);
      NAME(TASK_CONTROL);
      NAME(TASK_SHADER);
      NAME(TASK_REDISTRIB);
      NAME(BLEND_STATE_PTR);
      NAME(CLIP);
      NAME(CC_STATE);
      NAME(CC_STATE_PTR);
      NAME(CPS);
      NAME(DEPTH_BOUNDS);
      NAME(INDEX_BUFFER);
      NAME(LINE_STIPPLE);
      NAME(PS_BLEND);
      NAME(RASTER);
      NAME(SAMPLE_MASK);
      NAME(SAMPLE_PATTERN);
      NAME(SCISSOR);
      NAME(SF);
      NAME(STREAMOUT);
      NAME(TE);
      NAME(VERTEX_INPUT);
      NAME(VF);
      NAME(VF_TOPOLOGY);
      NAME(VFG);
      NAME(VIEWPORT_CC);
      NAME(VIEWPORT_CC_PTR);
      NAME(VIEWPORT_SF_CLIP);
      NAME(WM);
      NAME(WM_DEPTH_STENCIL);
      NAME(PMA_FIX);
      NAME(WA_18019816803);
      NAME(TBIMR_TILE_PASS_INFO);
   default: unreachable("invalid state");
   }
}

VkResult
anv_device_print_init(struct anv_device *device)
{
   struct anv_bo *bo;
   VkResult result =
      anv_device_alloc_bo(device, "printf",
                          anv_printf_buffer_size(),
                          ANV_BO_ALLOC_CAPTURE |
                          ANV_BO_ALLOC_MAPPED |
                          ANV_BO_ALLOC_HOST_COHERENT |
                          ANV_BO_ALLOC_NO_LOCAL_MEM,
                          0 /* explicit_address */,
                          &bo);
   if (result != VK_SUCCESS)
      return result;

   u_printf_init(&device->printf, bo, (uint32_t*)bo->map);
   return VK_SUCCESS;
}

void
anv_device_print_fini(struct anv_device *device)
{
   anv_device_release_bo(device, device->printf.bo);
   u_printf_destroy(&device->printf);
}

static void
create_directory(const char *dir, const char *sub_dir)
{
   char full_path[PATH_MAX];
   snprintf(full_path, sizeof(full_path), "%s/%s", dir, sub_dir);

   if (mkdir(dir, 0777) == -1 && errno != EEXIST) {
      perror("Error creating directory");
      return;
   }

   if (mkdir(full_path, 0777) == -1 && errno != EEXIST) {
      perror("Error creating sub directory");
      return;
   }
}

static void
create_bvh_dump_file(struct anv_bvh_dump *bvh)
{
   if (bvh == NULL) {
      fprintf(stderr, "Error: BVH DUMP structure is NULL\n");
      return;
   }

   char file_name[256];
   const char *dump_directory = "bvh_dump";
   const char *dump_sub_directory = NULL;

   switch (bvh->dump_type) {
   case BVH_ANV:
      dump_sub_directory = "BVH_ANV";
      break;
   case BVH_IR_HDR:
      dump_sub_directory = "BVH_IR_HDR";
      break;
   case BVH_IR_AS:
      dump_sub_directory = "BVH_IR_AS";
      break;
   default:
      unreachable("invalid dump type");
   }

   create_directory(dump_directory, dump_sub_directory);

   snprintf(file_name, sizeof(file_name),
            bvh->geometry_type == VK_GEOMETRY_TYPE_INSTANCES_KHR
               ? "%s/%s/tlas_%d.txt"
               : "%s/%s/blas_%d.txt",
            dump_directory, dump_sub_directory, bvh->bvh_id);

   FILE *file = fopen(file_name, "w");
   if (file == NULL) {
      perror("Error creating file");
      return;
   }

   fprintf(stderr, "BVH Dump File created: %s\n", file_name);

   uint8_t *addr = (uint8_t *)(bvh->bo->map);
   /* Dump every bytes like this: B0 B1 B2 B3 ... B15 */
   for (uint64_t i = 0; i < bvh->dump_size; i++) {
      uint8_t result = *(volatile uint8_t *)((uint8_t *)addr + i);
      fprintf(file, "%02" PRIx8 " ", result);
      if ((i + 1) % 16 == 0) {
         fprintf(file, "\n");
      }
   }

   fclose(file);
}

void anv_dump_bvh_to_files(struct anv_device *device)
{
   /* device->mutex is acquired in anv_queue_submit, so no need to lock here. */
   list_for_each_entry_safe(struct anv_bvh_dump, bvh_dump, &device->bvh_dumps,
                            link) {
      create_bvh_dump_file(bvh_dump);

      anv_device_release_bo(device, bvh_dump->bo);
      list_del(&bvh_dump->link);
      free(bvh_dump);
   }
}
