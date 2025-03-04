/*
 * Copyright © 2021 Google
 *
 * SPDX-License-Identifier: MIT
 */

#include <assert.h>
#include <stdbool.h>

#include "nir/nir_builder.h"
#include "radv_meta.h"
#include "sid.h"
#include "vk_common_entrypoints.h"
#include "vk_format.h"

static VkPipeline
radv_get_etc_decode_pipeline(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   struct radv_meta_state *state = &device->meta_state;
   VkResult ret;

   ret = vk_texcompress_etc2_late_init(&device->vk, &state->etc_decode);
   if (ret != VK_SUCCESS) {
      vk_command_buffer_set_error(&cmd_buffer->vk, ret);
      return VK_NULL_HANDLE;
   }

   return state->etc_decode.pipeline;
}

static void
decode_etc(struct radv_cmd_buffer *cmd_buffer, struct radv_image_view *src_iview, struct radv_image_view *dst_iview,
           const VkOffset3D *offset, const VkExtent3D *extent)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   VkPipeline pipeline = radv_get_etc_decode_pipeline(cmd_buffer);

   radv_meta_bind_descriptors(cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, device->meta_state.etc_decode.pipeline_layout,
                              2,
                              (VkDescriptorGetInfoEXT[]){{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT,
                                                          .type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                                          .data.pSampledImage =
                                                             (VkDescriptorImageInfo[]){
                                                                {.sampler = VK_NULL_HANDLE,
                                                                 .imageView = radv_image_view_to_handle(src_iview),
                                                                 .imageLayout = VK_IMAGE_LAYOUT_GENERAL},
                                                             }},
                                                         {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT,
                                                          .type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                                          .data.pStorageImage = (VkDescriptorImageInfo[]){
                                                             {
                                                                .sampler = VK_NULL_HANDLE,
                                                                .imageView = radv_image_view_to_handle(dst_iview),
                                                                .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
                                                             },
                                                          }}});

   radv_CmdBindPipeline(radv_cmd_buffer_to_handle(cmd_buffer), VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);

   unsigned push_constants[5] = {
      offset->x, offset->y, offset->z, src_iview->image->vk.format, src_iview->image->vk.image_type,
   };

   vk_common_CmdPushConstants(radv_cmd_buffer_to_handle(cmd_buffer), device->meta_state.etc_decode.pipeline_layout,
                              VK_SHADER_STAGE_COMPUTE_BIT, 0, 20, push_constants);
   radv_unaligned_dispatch(cmd_buffer, extent->width, extent->height, extent->depth);
}

void
radv_meta_decode_etc(struct radv_cmd_buffer *cmd_buffer, struct radv_image *image, VkImageLayout layout,
                     const VkImageSubresourceLayers *subresource, VkOffset3D offset, VkExtent3D extent)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   struct radv_meta_saved_state saved_state;
   radv_meta_save(&saved_state, cmd_buffer,
                  RADV_META_SAVE_COMPUTE_PIPELINE | RADV_META_SAVE_CONSTANTS | RADV_META_SAVE_DESCRIPTORS |
                     RADV_META_SUSPEND_PREDICATING);

   const bool is_3d = image->vk.image_type == VK_IMAGE_TYPE_3D;
   const uint32_t base_slice = is_3d ? offset.z : subresource->baseArrayLayer;
   const uint32_t slice_count = is_3d ? extent.depth : vk_image_subresource_layer_count(&image->vk, subresource);

   extent = vk_image_sanitize_extent(&image->vk, extent);
   offset = vk_image_sanitize_offset(&image->vk, offset);

   VkFormat load_format = vk_texcompress_etc2_load_format(image->vk.format);
   struct radv_image_view src_iview;
   radv_image_view_init(
      &src_iview, device,
      &(VkImageViewCreateInfo){
         .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
         .image = radv_image_to_handle(image),
         .viewType = vk_texcompress_etc2_image_view_type(image->vk.image_type),
         .format = load_format,
         .subresourceRange =
            {
               .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
               .baseMipLevel = subresource->mipLevel,
               .levelCount = 1,
               .baseArrayLayer = 0,
               .layerCount = subresource->baseArrayLayer + vk_image_subresource_layer_count(&image->vk, subresource),
            },
      },
      NULL);

   VkFormat store_format = vk_texcompress_etc2_store_format(image->vk.format);
   struct radv_image_view dst_iview;
   radv_image_view_init(
      &dst_iview, device,
      &(VkImageViewCreateInfo){
         .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
         .image = radv_image_to_handle(image),
         .viewType = vk_texcompress_etc2_image_view_type(image->vk.image_type),
         .format = store_format,
         .subresourceRange =
            {
               .aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT,
               .baseMipLevel = subresource->mipLevel,
               .levelCount = 1,
               .baseArrayLayer = 0,
               .layerCount = subresource->baseArrayLayer + vk_image_subresource_layer_count(&image->vk, subresource),
            },
      },
      NULL);

   decode_etc(cmd_buffer, &src_iview, &dst_iview, &(VkOffset3D){offset.x, offset.y, base_slice},
              &(VkExtent3D){extent.width, extent.height, slice_count});

   radv_image_view_finish(&src_iview);
   radv_image_view_finish(&dst_iview);

   radv_meta_restore(&saved_state, cmd_buffer);
}
