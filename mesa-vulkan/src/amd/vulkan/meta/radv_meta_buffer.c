#include "nir/nir_builder.h"
#include "radv_cp_dma.h"
#include "radv_debug.h"
#include "radv_meta.h"
#include "radv_sdma.h"

#include "radv_cs.h"
#include "sid.h"
#include "vk_common_entrypoints.h"

static nir_shader *
build_buffer_fill_shader(struct radv_device *dev)
{
   nir_builder b = radv_meta_init_shader(dev, MESA_SHADER_COMPUTE, "meta_buffer_fill");
   b.shader->info.workgroup_size[0] = 64;

   nir_def *pconst = nir_load_push_constant(&b, 4, 32, nir_imm_int(&b, 0), .range = 16);
   nir_def *buffer_addr = nir_pack_64_2x32(&b, nir_channels(&b, pconst, 0b0011));
   nir_def *max_offset = nir_channel(&b, pconst, 2);
   nir_def *data = nir_swizzle(&b, nir_channel(&b, pconst, 3), (unsigned[]){0, 0, 0, 0}, 4);

   nir_def *global_id =
      nir_iadd(&b, nir_imul_imm(&b, nir_channel(&b, nir_load_workgroup_id(&b), 0), b.shader->info.workgroup_size[0]),
               nir_load_local_invocation_index(&b));

   nir_def *offset = nir_imin(&b, nir_imul_imm(&b, global_id, 16), max_offset);
   nir_def *dst_addr = nir_iadd(&b, buffer_addr, nir_u2u64(&b, offset));
   nir_build_store_global(&b, data, dst_addr, .align_mul = 4);

   return b.shader;
}

struct fill_constants {
   uint64_t addr;
   uint32_t max_offset;
   uint32_t data;
};

static VkResult
create_fill_pipeline(struct radv_device *device)
{
   VkResult result;

   const VkPushConstantRange pc_range = {
      .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
      .size = sizeof(struct fill_constants),
   };

   result = radv_meta_create_pipeline_layout(device, NULL, 1, &pc_range, &device->meta_state.buffer.fill_p_layout);
   if (result != VK_SUCCESS)
      return result;

   nir_shader *cs = build_buffer_fill_shader(device);

   result = radv_meta_create_compute_pipeline(device, cs, device->meta_state.buffer.fill_p_layout,
                                              &device->meta_state.buffer.fill_pipeline);

   ralloc_free(cs);
   return result;
}

static VkResult
get_fill_pipeline(struct radv_device *device, VkPipeline *pipeline_out)
{
   struct radv_meta_state *state = &device->meta_state;
   VkResult result = VK_SUCCESS;

   mtx_lock(&state->mtx);
   if (!state->buffer.fill_pipeline) {
      result = create_fill_pipeline(device);
      if (result != VK_SUCCESS)
         goto fail;
   }

   *pipeline_out = state->buffer.fill_pipeline;

fail:
   mtx_unlock(&state->mtx);
   return result;
}

static nir_shader *
build_buffer_copy_shader(struct radv_device *dev)
{
   nir_builder b = radv_meta_init_shader(dev, MESA_SHADER_COMPUTE, "meta_buffer_copy");
   b.shader->info.workgroup_size[0] = 64;

   nir_def *pconst = nir_load_push_constant(&b, 4, 32, nir_imm_int(&b, 0), .range = 16);
   nir_def *max_offset = nir_load_push_constant(&b, 1, 32, nir_imm_int(&b, 0), .base = 16, .range = 4);
   nir_def *src_addr = nir_pack_64_2x32(&b, nir_channels(&b, pconst, 0b0011));
   nir_def *dst_addr = nir_pack_64_2x32(&b, nir_channels(&b, pconst, 0b1100));

   nir_def *global_id =
      nir_iadd(&b, nir_imul_imm(&b, nir_channel(&b, nir_load_workgroup_id(&b), 0), b.shader->info.workgroup_size[0]),
               nir_load_local_invocation_index(&b));

   nir_def *offset = nir_u2u64(&b, nir_imin(&b, nir_imul_imm(&b, global_id, 16), max_offset));

   nir_def *data = nir_build_load_global(&b, 4, 32, nir_iadd(&b, src_addr, offset), .align_mul = 4);
   nir_build_store_global(&b, data, nir_iadd(&b, dst_addr, offset), .align_mul = 4);

   return b.shader;
}

struct copy_constants {
   uint64_t src_addr;
   uint64_t dst_addr;
   uint32_t max_offset;
};

static VkResult
create_copy_pipeline(struct radv_device *device)
{
   VkResult result;

   const VkPushConstantRange pc_range = {
      .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
      .size = sizeof(struct copy_constants),
   };

   result = radv_meta_create_pipeline_layout(device, NULL, 1, &pc_range, &device->meta_state.buffer.copy_p_layout);
   if (result != VK_SUCCESS)
      return result;

   nir_shader *cs = build_buffer_copy_shader(device);

   result = radv_meta_create_compute_pipeline(device, cs, device->meta_state.buffer.copy_p_layout,
                                              &device->meta_state.buffer.copy_pipeline);

   ralloc_free(cs);
   return result;
}

static VkResult
get_copy_pipeline(struct radv_device *device, VkPipeline *pipeline_out)
{
   struct radv_meta_state *state = &device->meta_state;
   VkResult result = VK_SUCCESS;

   mtx_lock(&state->mtx);
   if (!state->buffer.copy_pipeline) {
      result = create_copy_pipeline(device);
      if (result != VK_SUCCESS)
         goto fail;
   }

   *pipeline_out = state->buffer.copy_pipeline;

fail:
   mtx_unlock(&state->mtx);
   return result;
}

VkResult
radv_device_init_meta_buffer_state(struct radv_device *device, bool on_demand)
{
   VkResult result;

   if (on_demand)
      return VK_SUCCESS;

   result = create_fill_pipeline(device);
   if (result != VK_SUCCESS)
      return result;

   result = create_copy_pipeline(device);
   if (result != VK_SUCCESS)
      return result;

   return result;
}

void
radv_device_finish_meta_buffer_state(struct radv_device *device)
{
   struct radv_meta_state *state = &device->meta_state;

   radv_DestroyPipeline(radv_device_to_handle(device), state->buffer.copy_pipeline, &state->alloc);
   radv_DestroyPipeline(radv_device_to_handle(device), state->buffer.fill_pipeline, &state->alloc);
   radv_DestroyPipelineLayout(radv_device_to_handle(device), state->buffer.copy_p_layout, &state->alloc);
   radv_DestroyPipelineLayout(radv_device_to_handle(device), state->buffer.fill_p_layout, &state->alloc);
}

static void
fill_buffer_shader(struct radv_cmd_buffer *cmd_buffer, uint64_t va, uint64_t size, uint32_t data)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   struct radv_meta_saved_state saved_state;
   VkPipeline pipeline;
   VkResult result;

   result = get_fill_pipeline(device, &pipeline);
   if (result != VK_SUCCESS) {
      vk_command_buffer_set_error(&cmd_buffer->vk, result);
      return;
   }

   radv_meta_save(&saved_state, cmd_buffer, RADV_META_SAVE_COMPUTE_PIPELINE | RADV_META_SAVE_CONSTANTS);

   radv_CmdBindPipeline(radv_cmd_buffer_to_handle(cmd_buffer), VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);

   assert(size >= 16 && size <= UINT32_MAX);

   struct fill_constants fill_consts = {
      .addr = va,
      .max_offset = size - 16,
      .data = data,
   };

   vk_common_CmdPushConstants(radv_cmd_buffer_to_handle(cmd_buffer), device->meta_state.buffer.fill_p_layout,
                              VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(fill_consts), &fill_consts);

   radv_unaligned_dispatch(cmd_buffer, DIV_ROUND_UP(size, 16), 1, 1);

   radv_meta_restore(&saved_state, cmd_buffer);
}

static void
copy_buffer_shader(struct radv_cmd_buffer *cmd_buffer, uint64_t src_va, uint64_t dst_va, uint64_t size)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   struct radv_meta_saved_state saved_state;
   VkPipeline pipeline;
   VkResult result;

   result = get_copy_pipeline(device, &pipeline);
   if (result != VK_SUCCESS) {
      vk_command_buffer_set_error(&cmd_buffer->vk, result);
      return;
   }

   radv_meta_save(&saved_state, cmd_buffer, RADV_META_SAVE_COMPUTE_PIPELINE | RADV_META_SAVE_CONSTANTS);

   radv_CmdBindPipeline(radv_cmd_buffer_to_handle(cmd_buffer), VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);

   assert(size >= 16 && size <= UINT32_MAX);

   struct copy_constants copy_consts = {
      .src_addr = src_va,
      .dst_addr = dst_va,
      .max_offset = size - 16,
   };

   vk_common_CmdPushConstants(radv_cmd_buffer_to_handle(cmd_buffer), device->meta_state.buffer.copy_p_layout,
                              VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(copy_consts), &copy_consts);

   radv_unaligned_dispatch(cmd_buffer, DIV_ROUND_UP(size, 16), 1, 1);

   radv_meta_restore(&saved_state, cmd_buffer);
}

static bool
radv_prefer_compute_dma(const struct radv_device *device, uint64_t size, struct radeon_winsys_bo *src_bo,
                        struct radeon_winsys_bo *dst_bo)
{
   const struct radv_physical_device *pdev = radv_device_physical(device);
   bool use_compute = size >= RADV_BUFFER_OPS_CS_THRESHOLD;

   if (pdev->info.gfx_level >= GFX10 && pdev->info.has_dedicated_vram) {
      if ((src_bo && !(src_bo->initial_domain & RADEON_DOMAIN_VRAM)) ||
          (dst_bo && !(dst_bo->initial_domain & RADEON_DOMAIN_VRAM))) {
         /* Prefer CP DMA for GTT on dGPUS due to slow PCIe. */
         use_compute = false;
      }
   }

   return use_compute;
}

uint32_t
radv_fill_buffer(struct radv_cmd_buffer *cmd_buffer, const struct radv_image *image, struct radeon_winsys_bo *bo,
                 uint64_t va, uint64_t size, uint32_t value)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   bool use_compute = radv_prefer_compute_dma(device, size, NULL, bo);
   uint32_t flush_bits = 0;

   assert(!(va & 3));
   assert(!(size & 3));

   if (bo)
      radv_cs_add_buffer(device->ws, cmd_buffer->cs, bo);

   if (cmd_buffer->qf == RADV_QUEUE_TRANSFER) {
      radv_sdma_fill_buffer(device, cmd_buffer->cs, va, size, value);
   } else if (use_compute) {
      fill_buffer_shader(cmd_buffer, va, size, value);

      flush_bits =
         RADV_CMD_FLAG_CS_PARTIAL_FLUSH | RADV_CMD_FLAG_INV_VCACHE |
         radv_src_access_flush(cmd_buffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT, image);
   } else if (size)
      radv_cp_dma_clear_buffer(cmd_buffer, va, size, value);

   return flush_bits;
}

void
radv_copy_buffer(struct radv_cmd_buffer *cmd_buffer, struct radeon_winsys_bo *src_bo, struct radeon_winsys_bo *dst_bo,
                 uint64_t src_offset, uint64_t dst_offset, uint64_t size)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   bool use_compute =
      !(size & 3) && !(src_offset & 3) && !(dst_offset & 3) && radv_prefer_compute_dma(device, size, src_bo, dst_bo);

   uint64_t src_va = radv_buffer_get_va(src_bo) + src_offset;
   uint64_t dst_va = radv_buffer_get_va(dst_bo) + dst_offset;

   radv_cs_add_buffer(device->ws, cmd_buffer->cs, src_bo);
   radv_cs_add_buffer(device->ws, cmd_buffer->cs, dst_bo);

   if (cmd_buffer->qf == RADV_QUEUE_TRANSFER)
      radv_sdma_copy_buffer(device, cmd_buffer->cs, src_va, dst_va, size);
   else if (use_compute)
      copy_buffer_shader(cmd_buffer, src_va, dst_va, size);
   else if (size)
      radv_cp_dma_buffer_copy(cmd_buffer, src_va, dst_va, size);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdFillBuffer(VkCommandBuffer commandBuffer, VkBuffer dstBuffer, VkDeviceSize dstOffset, VkDeviceSize fillSize,
                   uint32_t data)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   VK_FROM_HANDLE(radv_buffer, dst_buffer, dstBuffer);

   fillSize = vk_buffer_range(&dst_buffer->vk, dstOffset, fillSize) & ~3ull;

   radv_fill_buffer(cmd_buffer, NULL, dst_buffer->bo,
                    radv_buffer_get_va(dst_buffer->bo) + dst_buffer->offset + dstOffset, fillSize, data);
}

static void
copy_buffer(struct radv_cmd_buffer *cmd_buffer, struct radv_buffer *src_buffer, struct radv_buffer *dst_buffer,
            const VkBufferCopy2 *region)
{
   bool old_predicating;

   /* VK_EXT_conditional_rendering says that copy commands should not be
    * affected by conditional rendering.
    */
   old_predicating = cmd_buffer->state.predicating;
   cmd_buffer->state.predicating = false;

   radv_copy_buffer(cmd_buffer, src_buffer->bo, dst_buffer->bo, src_buffer->offset + region->srcOffset,
                    dst_buffer->offset + region->dstOffset, region->size);

   /* Restore conditional rendering. */
   cmd_buffer->state.predicating = old_predicating;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdCopyBuffer2(VkCommandBuffer commandBuffer, const VkCopyBufferInfo2 *pCopyBufferInfo)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   VK_FROM_HANDLE(radv_buffer, src_buffer, pCopyBufferInfo->srcBuffer);
   VK_FROM_HANDLE(radv_buffer, dst_buffer, pCopyBufferInfo->dstBuffer);

   for (unsigned r = 0; r < pCopyBufferInfo->regionCount; r++) {
      copy_buffer(cmd_buffer, src_buffer, dst_buffer, &pCopyBufferInfo->pRegions[r]);
   }
}

void
radv_update_buffer_cp(struct radv_cmd_buffer *cmd_buffer, uint64_t va, const void *data, uint64_t size)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   uint64_t words = size / 4;
   bool mec = radv_cmd_buffer_uses_mec(cmd_buffer);

   assert(size < RADV_BUFFER_UPDATE_THRESHOLD);

   radv_emit_cache_flush(cmd_buffer);
   radeon_check_space(device->ws, cmd_buffer->cs, words + 4);

   radeon_emit(cmd_buffer->cs, PKT3(PKT3_WRITE_DATA, 2 + words, 0));
   radeon_emit(cmd_buffer->cs,
               S_370_DST_SEL(mec ? V_370_MEM : V_370_MEM_GRBM) | S_370_WR_CONFIRM(1) | S_370_ENGINE_SEL(V_370_ME));
   radeon_emit(cmd_buffer->cs, va);
   radeon_emit(cmd_buffer->cs, va >> 32);
   radeon_emit_array(cmd_buffer->cs, data, words);

   if (radv_device_fault_detection_enabled(device))
      radv_cmd_buffer_trace_emit(cmd_buffer);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdUpdateBuffer(VkCommandBuffer commandBuffer, VkBuffer dstBuffer, VkDeviceSize dstOffset, VkDeviceSize dataSize,
                     const void *pData)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   VK_FROM_HANDLE(radv_buffer, dst_buffer, dstBuffer);
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   uint64_t va = radv_buffer_get_va(dst_buffer->bo);
   va += dstOffset + dst_buffer->offset;

   assert(!(dataSize & 3));
   assert(!(va & 3));

   if (!dataSize)
      return;

   if (dataSize < RADV_BUFFER_UPDATE_THRESHOLD && cmd_buffer->qf != RADV_QUEUE_TRANSFER) {
      radv_cs_add_buffer(device->ws, cmd_buffer->cs, dst_buffer->bo);
      radv_update_buffer_cp(cmd_buffer, va, pData, dataSize);
   } else {
      uint32_t buf_offset;
      radv_cmd_buffer_upload_data(cmd_buffer, dataSize, pData, &buf_offset);
      radv_copy_buffer(cmd_buffer, cmd_buffer->upload.upload_bo, dst_buffer->bo, buf_offset,
                       dstOffset + dst_buffer->offset, dataSize);
   }
}
