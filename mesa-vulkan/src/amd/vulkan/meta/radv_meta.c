/*
 * Copyright © 2016 Red Hat
 * based on intel anv code:
 * Copyright © 2015 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 */

#include "radv_meta.h"
#include "radv_printf.h"

#include "vk_common_entrypoints.h"
#include "vk_pipeline_cache.h"
#include "vk_util.h"

static void
radv_suspend_queries(struct radv_meta_saved_state *state, struct radv_cmd_buffer *cmd_buffer)
{
   const uint32_t num_pipeline_stat_queries = radv_get_num_pipeline_stat_queries(cmd_buffer);

   if (num_pipeline_stat_queries > 0) {
      cmd_buffer->state.flush_bits &= ~RADV_CMD_FLAG_START_PIPELINE_STATS;
      cmd_buffer->state.flush_bits |= RADV_CMD_FLAG_STOP_PIPELINE_STATS;
   }

   /* Pipeline statistics queries. */
   if (cmd_buffer->state.active_pipeline_queries > 0) {
      state->active_emulated_pipeline_queries = cmd_buffer->state.active_emulated_pipeline_queries;
      cmd_buffer->state.active_emulated_pipeline_queries = 0;
      cmd_buffer->state.dirty |= RADV_CMD_DIRTY_SHADER_QUERY;
   }

   /* Occlusion queries. */
   if (cmd_buffer->state.active_occlusion_queries) {
      state->active_occlusion_queries = cmd_buffer->state.active_occlusion_queries;
      cmd_buffer->state.active_occlusion_queries = 0;
      cmd_buffer->state.dirty |= RADV_CMD_DIRTY_OCCLUSION_QUERY;
   }

   /* Primitives generated queries (legacy). */
   if (cmd_buffer->state.active_prims_gen_queries) {
      cmd_buffer->state.suspend_streamout = true;
      cmd_buffer->state.dirty |= RADV_CMD_DIRTY_STREAMOUT_ENABLE;
   }

   /* Primitives generated queries (NGG). */
   if (cmd_buffer->state.active_emulated_prims_gen_queries) {
      state->active_emulated_prims_gen_queries = cmd_buffer->state.active_emulated_prims_gen_queries;
      cmd_buffer->state.active_emulated_prims_gen_queries = 0;
      cmd_buffer->state.dirty |= RADV_CMD_DIRTY_SHADER_QUERY;
   }

   /* Transform feedback queries (NGG). */
   if (cmd_buffer->state.active_emulated_prims_xfb_queries) {
      state->active_emulated_prims_xfb_queries = cmd_buffer->state.active_emulated_prims_xfb_queries;
      cmd_buffer->state.active_emulated_prims_xfb_queries = 0;
      cmd_buffer->state.dirty |= RADV_CMD_DIRTY_SHADER_QUERY;
   }
}

static void
radv_resume_queries(const struct radv_meta_saved_state *state, struct radv_cmd_buffer *cmd_buffer)
{
   const uint32_t num_pipeline_stat_queries = radv_get_num_pipeline_stat_queries(cmd_buffer);

   if (num_pipeline_stat_queries > 0) {
      cmd_buffer->state.flush_bits &= ~RADV_CMD_FLAG_STOP_PIPELINE_STATS;
      cmd_buffer->state.flush_bits |= RADV_CMD_FLAG_START_PIPELINE_STATS;
   }

   /* Pipeline statistics queries. */
   if (cmd_buffer->state.active_pipeline_queries > 0) {
      cmd_buffer->state.active_emulated_pipeline_queries = state->active_emulated_pipeline_queries;
      cmd_buffer->state.dirty |= RADV_CMD_DIRTY_SHADER_QUERY;
   }

   /* Occlusion queries. */
   if (state->active_occlusion_queries) {
      cmd_buffer->state.active_occlusion_queries = state->active_occlusion_queries;
      cmd_buffer->state.dirty |= RADV_CMD_DIRTY_OCCLUSION_QUERY;
   }

   /* Primitives generated queries (legacy). */
   if (cmd_buffer->state.active_prims_gen_queries) {
      cmd_buffer->state.suspend_streamout = false;
      cmd_buffer->state.dirty |= RADV_CMD_DIRTY_STREAMOUT_ENABLE;
   }

   /* Primitives generated queries (NGG). */
   if (state->active_emulated_prims_gen_queries) {
      cmd_buffer->state.active_emulated_prims_gen_queries = state->active_emulated_prims_gen_queries;
      cmd_buffer->state.dirty |= RADV_CMD_DIRTY_SHADER_QUERY;
   }

   /* Transform feedback queries (NGG). */
   if (state->active_emulated_prims_xfb_queries) {
      cmd_buffer->state.active_emulated_prims_xfb_queries = state->active_emulated_prims_xfb_queries;
      cmd_buffer->state.dirty |= RADV_CMD_DIRTY_SHADER_QUERY;
   }
}

void
radv_meta_save(struct radv_meta_saved_state *state, struct radv_cmd_buffer *cmd_buffer, uint32_t flags)
{
   VkPipelineBindPoint bind_point =
      flags & RADV_META_SAVE_GRAPHICS_PIPELINE ? VK_PIPELINE_BIND_POINT_GRAPHICS : VK_PIPELINE_BIND_POINT_COMPUTE;
   struct radv_descriptor_state *descriptors_state = radv_get_descriptors_state(cmd_buffer, bind_point);

   assert(flags & (RADV_META_SAVE_GRAPHICS_PIPELINE | RADV_META_SAVE_COMPUTE_PIPELINE));

   state->flags = flags;
   state->active_occlusion_queries = 0;
   state->active_emulated_prims_gen_queries = 0;
   state->active_emulated_prims_xfb_queries = 0;

   if (state->flags & RADV_META_SAVE_GRAPHICS_PIPELINE) {
      assert(!(state->flags & RADV_META_SAVE_COMPUTE_PIPELINE));

      state->old_graphics_pipeline = cmd_buffer->state.graphics_pipeline;

      /* Save all dynamic states. */
      state->dynamic = cmd_buffer->state.dynamic;
   }

   if (state->flags & RADV_META_SAVE_COMPUTE_PIPELINE) {
      assert(!(state->flags & RADV_META_SAVE_GRAPHICS_PIPELINE));

      state->old_compute_pipeline = cmd_buffer->state.compute_pipeline;
   }

   for (unsigned i = 0; i <= MESA_SHADER_MESH; i++) {
      state->old_shader_objs[i] = cmd_buffer->state.shader_objs[i];
   }

   if (state->flags & RADV_META_SAVE_DESCRIPTORS) {
      state->old_descriptor_set0 = descriptors_state->sets[0];
      state->old_descriptor_set0_valid = !!(descriptors_state->valid & 0x1);
      state->old_descriptor_buffer_addr0 = cmd_buffer->descriptor_buffers[0];
      state->old_descriptor_buffer0 = descriptors_state->descriptor_buffers[0];
   }

   if (state->flags & RADV_META_SAVE_CONSTANTS) {
      memcpy(state->push_constants, cmd_buffer->push_constants, MAX_PUSH_CONSTANTS_SIZE);
   }

   if (state->flags & RADV_META_SAVE_RENDER) {
      state->render = cmd_buffer->state.render;
      radv_cmd_buffer_reset_rendering(cmd_buffer);
   }

   if (state->flags & RADV_META_SUSPEND_PREDICATING) {
      state->predicating = cmd_buffer->state.predicating;
      cmd_buffer->state.predicating = false;
   }

   radv_suspend_queries(state, cmd_buffer);
}

void
radv_meta_restore(const struct radv_meta_saved_state *state, struct radv_cmd_buffer *cmd_buffer)
{
   VkPipelineBindPoint bind_point = state->flags & RADV_META_SAVE_GRAPHICS_PIPELINE ? VK_PIPELINE_BIND_POINT_GRAPHICS
                                                                                    : VK_PIPELINE_BIND_POINT_COMPUTE;
   struct radv_descriptor_state *descriptors_state = radv_get_descriptors_state(cmd_buffer, bind_point);

   if (state->flags & RADV_META_SAVE_GRAPHICS_PIPELINE) {
      if (state->old_graphics_pipeline) {
         radv_CmdBindPipeline(radv_cmd_buffer_to_handle(cmd_buffer), VK_PIPELINE_BIND_POINT_GRAPHICS,
                              radv_pipeline_to_handle(&state->old_graphics_pipeline->base));
      }

      /* Restore all dynamic states. */
      cmd_buffer->state.dynamic = state->dynamic;
      cmd_buffer->state.dirty_dynamic |= RADV_DYNAMIC_ALL;

      /* Re-emit the guardband state because meta operations changed dynamic states. */
      cmd_buffer->state.dirty |= RADV_CMD_DIRTY_GUARDBAND;
   }

   if (state->flags & RADV_META_SAVE_COMPUTE_PIPELINE) {
      if (state->old_compute_pipeline) {
         radv_CmdBindPipeline(radv_cmd_buffer_to_handle(cmd_buffer), VK_PIPELINE_BIND_POINT_COMPUTE,
                              radv_pipeline_to_handle(&state->old_compute_pipeline->base));
      }
   }

   VkShaderEXT shaders[MESA_SHADER_MESH + 1];
   VkShaderStageFlagBits stages[MESA_SHADER_MESH + 1];
   uint32_t stage_count = 0;

   for (unsigned i = 0; i <= MESA_SHADER_MESH; i++) {
      if (state->old_shader_objs[i]) {
         stages[stage_count] = mesa_to_vk_shader_stage(i);
         shaders[stage_count] = radv_shader_object_to_handle(state->old_shader_objs[i]);
         stage_count++;
      }
   }

   if (stage_count > 0) {
      radv_CmdBindShadersEXT(radv_cmd_buffer_to_handle(cmd_buffer), stage_count, stages, shaders);
   }

   if (state->flags & RADV_META_SAVE_DESCRIPTORS) {
      if (state->old_descriptor_set0_valid)
         radv_set_descriptor_set(cmd_buffer, bind_point, state->old_descriptor_set0, 0);
      cmd_buffer->descriptor_buffers[0] = state->old_descriptor_buffer_addr0;
      descriptors_state->descriptor_buffers[0] = state->old_descriptor_buffer0;
   }

   if (state->flags & RADV_META_SAVE_CONSTANTS) {
      VkShaderStageFlags stage_flags = VK_SHADER_STAGE_COMPUTE_BIT;

      if (state->flags & RADV_META_SAVE_GRAPHICS_PIPELINE)
         stage_flags |= VK_SHADER_STAGE_ALL_GRAPHICS;

      vk_common_CmdPushConstants(radv_cmd_buffer_to_handle(cmd_buffer), VK_NULL_HANDLE, stage_flags, 0,
                                 MAX_PUSH_CONSTANTS_SIZE, state->push_constants);
   }

   if (state->flags & RADV_META_SAVE_RENDER) {
      cmd_buffer->state.render = state->render;
      cmd_buffer->state.dirty |= RADV_CMD_DIRTY_FRAMEBUFFER;
   }

   if (state->flags & RADV_META_SUSPEND_PREDICATING)
      cmd_buffer->state.predicating = state->predicating;

   radv_resume_queries(state, cmd_buffer);
}

VkImageViewType
radv_meta_get_view_type(const struct radv_image *image)
{
   switch (image->vk.image_type) {
   case VK_IMAGE_TYPE_1D:
      return VK_IMAGE_VIEW_TYPE_1D;
   case VK_IMAGE_TYPE_2D:
      return VK_IMAGE_VIEW_TYPE_2D;
   case VK_IMAGE_TYPE_3D:
      return VK_IMAGE_VIEW_TYPE_3D;
   default:
      unreachable("bad VkImageViewType");
   }
}

static VKAPI_ATTR void *VKAPI_CALL
meta_alloc(void *_device, size_t size, size_t alignment, VkSystemAllocationScope allocationScope)
{
   struct radv_device *device = _device;
   return device->vk.alloc.pfnAllocation(device->vk.alloc.pUserData, size, alignment,
                                         VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
}

static VKAPI_ATTR void *VKAPI_CALL
meta_realloc(void *_device, void *original, size_t size, size_t alignment, VkSystemAllocationScope allocationScope)
{
   struct radv_device *device = _device;
   return device->vk.alloc.pfnReallocation(device->vk.alloc.pUserData, original, size, alignment,
                                           VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
}

static VKAPI_ATTR void VKAPI_CALL
meta_free(void *_device, void *data)
{
   struct radv_device *device = _device;
   device->vk.alloc.pfnFree(device->vk.alloc.pUserData, data);
}

static void
radv_init_meta_cache(struct radv_device *device)
{
   const struct radv_physical_device *pdev = radv_device_physical(device);
   struct vk_pipeline_cache *cache;

   VkPipelineCacheCreateInfo create_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
   };

   struct vk_pipeline_cache_create_info info = {
      .pCreateInfo = &create_info,
      .disk_cache = pdev->disk_cache_meta,
   };

   cache = vk_pipeline_cache_create(&device->vk, &info, NULL);
   if (cache)
      device->meta_state.cache = vk_pipeline_cache_to_handle(cache);
}

VkResult
radv_device_init_meta(struct radv_device *device)
{
   const struct radv_physical_device *pdev = radv_device_physical(device);
   VkResult result;

   memset(&device->meta_state, 0, sizeof(device->meta_state));

   device->meta_state.alloc = (VkAllocationCallbacks){
      .pUserData = device,
      .pfnAllocation = meta_alloc,
      .pfnReallocation = meta_realloc,
      .pfnFree = meta_free,
   };

   radv_init_meta_cache(device);

   result = vk_meta_device_init(&device->vk, &device->meta_state.device);
   if (result != VK_SUCCESS)
      return result;

   device->meta_state.device.pipeline_cache = device->meta_state.cache;

   mtx_init(&device->meta_state.mtx, mtx_plain);

   if (pdev->emulate_etc2) {
      device->meta_state.etc_decode.allocator = &device->meta_state.alloc;
      device->meta_state.etc_decode.nir_options = &pdev->nir_options[MESA_SHADER_COMPUTE];
      device->meta_state.etc_decode.pipeline_cache = device->meta_state.cache;

      vk_texcompress_etc2_init(&device->vk, &device->meta_state.etc_decode);
   }

   if (pdev->emulate_astc) {
      result = vk_texcompress_astc_init(&device->vk, &device->meta_state.alloc, device->meta_state.cache,
                                        &device->meta_state.astc_decode);
      if (result != VK_SUCCESS)
         return result;
   }

   if (device->vk.enabled_features.nullDescriptor) {
      result = radv_device_init_null_accel_struct(device);
      if (result != VK_SUCCESS)
         return result;
   }

   return VK_SUCCESS;
}

void
radv_device_finish_meta(struct radv_device *device)
{
   const struct radv_physical_device *pdev = radv_device_physical(device);

   if (pdev->emulate_etc2)
      vk_texcompress_etc2_finish(&device->vk, &device->meta_state.etc_decode);

   if (pdev->emulate_astc) {
      if (device->meta_state.astc_decode)
         vk_texcompress_astc_finish(&device->vk, &device->meta_state.alloc, device->meta_state.astc_decode);
   }

   radv_device_finish_accel_struct_build_state(device);

   vk_common_DestroyPipelineCache(radv_device_to_handle(device), device->meta_state.cache, NULL);
   mtx_destroy(&device->meta_state.mtx);

   if (device->meta_state.device.cache)
      vk_meta_device_finish(&device->vk, &device->meta_state.device);
}

VkResult
radv_meta_get_noop_pipeline_layout(struct radv_device *device, VkPipelineLayout *layout_out)
{
   enum radv_meta_object_key_type key = RADV_META_OBJECT_KEY_NOOP;

   return vk_meta_get_pipeline_layout(&device->vk, &device->meta_state.device, NULL, NULL, &key, sizeof(key),
                                      layout_out);
}

void
radv_meta_bind_descriptors(struct radv_cmd_buffer *cmd_buffer, VkPipelineBindPoint bind_point, VkPipelineLayout _layout,
                           uint32_t num_descriptors, const VkDescriptorGetInfoEXT *descriptors)
{
   VK_FROM_HANDLE(radv_pipeline_layout, layout, _layout);
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   struct radv_descriptor_set_layout *set_layout = layout->set[0].layout;
   uint32_t upload_offset;
   uint8_t *ptr;

   assert(layout->num_sets == 1);

   if (!radv_cmd_buffer_upload_alloc(cmd_buffer, set_layout->size, &upload_offset, (void *)&ptr))
      return;

   for (uint32_t i = 0; i < num_descriptors; i++) {
      const VkDescriptorGetInfoEXT *descriptor = &descriptors[i];
      const uint32_t binding_offset = set_layout->binding[i].offset;

      radv_GetDescriptorEXT(radv_device_to_handle(device), descriptor, 0, ptr + binding_offset);

      VkImageView image_view = VK_NULL_HANDLE;
      if (descriptor->type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {
         image_view = descriptor->data.pCombinedImageSampler->imageView;
      } else if (descriptor->type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE) {
         image_view = descriptor->data.pSampledImage->imageView;
      } else if (descriptor->type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) {
         image_view = descriptor->data.pStorageImage->imageView;
      }

      /* Buffer descriptors use BDA and they should be added to the list before. */
      if (image_view) {
         VK_FROM_HANDLE(radv_image_view, iview, image_view);
         for (uint32_t b = 0; b < ARRAY_SIZE(iview->image->bindings); b++) {
            if (iview->image->bindings[b].bo)
               radv_cs_add_buffer(device->ws, cmd_buffer->cs, iview->image->bindings[b].bo);
         }
      }
   }

   const VkDescriptorBufferBindingInfoEXT descriptor_buffer_binding = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_BUFFER_BINDING_INFO_EXT,
      .address = radv_buffer_get_va(cmd_buffer->upload.upload_bo) + upload_offset,
      .usage = VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT,
   };

   radv_CmdBindDescriptorBuffersEXT(radv_cmd_buffer_to_handle(cmd_buffer), 1, &descriptor_buffer_binding);

   const VkSetDescriptorBufferOffsetsInfoEXT descriptor_buffer_offsets = {
      .sType = VK_STRUCTURE_TYPE_SET_DESCRIPTOR_BUFFER_OFFSETS_INFO_EXT,
      .stageFlags = vk_shader_stages_from_bind_point(bind_point),
      .layout = radv_pipeline_layout_to_handle(layout),
      .firstSet = 0,
      .setCount = 1,
      .pBufferIndices = (uint32_t[]){0},
      .pOffsets = (uint64_t[]){0},
   };

   radv_CmdSetDescriptorBufferOffsets2EXT(radv_cmd_buffer_to_handle(cmd_buffer), &descriptor_buffer_offsets);
}
