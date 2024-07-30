/*
 * Copyright © 2021 Valve Corporation
 *
 * SPDX-License-Identifier: MIT
 */

#define AC_SURFACE_INCLUDE_NIR
#include "ac_surface.h"

#include "radv_meta.h"
#include "vk_common_entrypoints.h"
#include "vk_format.h"

void
radv_device_finish_meta_copy_vrs_htile_state(struct radv_device *device)
{
   struct radv_meta_state *state = &device->meta_state;

   radv_DestroyPipeline(radv_device_to_handle(device), state->copy_vrs_htile_pipeline, &state->alloc);
   radv_DestroyPipelineLayout(radv_device_to_handle(device), state->copy_vrs_htile_p_layout, &state->alloc);
   device->vk.dispatch_table.DestroyDescriptorSetLayout(radv_device_to_handle(device), state->copy_vrs_htile_ds_layout,
                                                        &state->alloc);
}

static nir_shader *
build_copy_vrs_htile_shader(struct radv_device *device, struct radeon_surf *surf)
{
   const struct radv_physical_device *pdev = radv_device_physical(device);
   nir_builder b = radv_meta_init_shader(device, MESA_SHADER_COMPUTE, "meta_copy_vrs_htile");
   b.shader->info.workgroup_size[0] = 8;
   b.shader->info.workgroup_size[1] = 8;

   /* Get coordinates. */
   nir_def *global_id = get_global_ids(&b, 2);

   nir_def *offset = nir_load_push_constant(&b, 2, 32, nir_imm_int(&b, 0), .range = 8);

   /* Multiply the coordinates by the HTILE block size. */
   nir_def *coord = nir_iadd(&b, nir_imul_imm(&b, global_id, 8), offset);

   /* Load constants. */
   nir_def *constants = nir_load_push_constant(&b, 3, 32, nir_imm_int(&b, 8), .range = 20);
   nir_def *htile_pitch = nir_channel(&b, constants, 0);
   nir_def *htile_slice_size = nir_channel(&b, constants, 1);
   nir_def *read_htile_value = nir_channel(&b, constants, 2);

   /* Get the HTILE addr from coordinates. */
   nir_def *zero = nir_imm_int(&b, 0);
   nir_def *htile_addr =
      ac_nir_htile_addr_from_coord(&b, &pdev->info, &surf->u.gfx9.zs.htile_equation, htile_pitch, htile_slice_size,
                                   nir_channel(&b, coord, 0), nir_channel(&b, coord, 1), zero, zero);

   /* Set up the input VRS image descriptor. */
   const struct glsl_type *vrs_sampler_type = glsl_sampler_type(GLSL_SAMPLER_DIM_2D, false, false, GLSL_TYPE_FLOAT);
   nir_variable *input_vrs_img = nir_variable_create(b.shader, nir_var_uniform, vrs_sampler_type, "input_vrs_image");
   input_vrs_img->data.descriptor_set = 0;
   input_vrs_img->data.binding = 0;

   /* Load the VRS rates from the 2D image. */
   nir_def *value = nir_txf_deref(&b, nir_build_deref_var(&b, input_vrs_img), global_id, NULL);

   /* Extract the X/Y rates and clamp them because the maximum supported VRS rate is 2x2 (1x1 in
    * hardware).
    *
    * VRS rate X = min(value >> 2, 1)
    * VRS rate Y = min(value & 3, 1)
    */
   nir_def *x_rate = nir_ushr_imm(&b, nir_channel(&b, value, 0), 2);
   x_rate = nir_umin(&b, x_rate, nir_imm_int(&b, 1));

   nir_def *y_rate = nir_iand_imm(&b, nir_channel(&b, value, 0), 3);
   y_rate = nir_umin(&b, y_rate, nir_imm_int(&b, 1));

   /* Compute the final VRS rate. */
   nir_def *vrs_rates = nir_ior(&b, nir_ishl_imm(&b, y_rate, 10), nir_ishl_imm(&b, x_rate, 6));

   /* Load the HTILE buffer descriptor. */
   nir_def *htile_buf = radv_meta_load_descriptor(&b, 0, 1);

   /* Load the HTILE value if requested, otherwise use the default value. */
   nir_variable *htile_value = nir_local_variable_create(b.impl, glsl_int_type(), "htile_value");

   nir_push_if(&b, nir_ieq_imm(&b, read_htile_value, 1));
   {
      /* Load the existing HTILE 32-bit value for this 8x8 pixels area. */
      nir_def *input_value = nir_load_ssbo(&b, 1, 32, htile_buf, htile_addr);

      /* Clear the 4-bit VRS rates. */
      nir_store_var(&b, htile_value, nir_iand_imm(&b, input_value, 0xfffff33f), 0x1);
   }
   nir_push_else(&b, NULL);
   {
      nir_store_var(&b, htile_value, nir_imm_int(&b, 0xfffff33f), 0x1);
   }
   nir_pop_if(&b, NULL);

   /* Set the VRS rates loaded from the image. */
   nir_def *output_value = nir_ior(&b, nir_load_var(&b, htile_value), vrs_rates);

   /* Store the updated HTILE 32-bit which contains the VRS rates. */
   nir_store_ssbo(&b, output_value, htile_buf, htile_addr, .access = ACCESS_NON_READABLE);

   return b.shader;
}

static VkResult
create_pipeline(struct radv_device *device, struct radeon_surf *surf)
{
   struct radv_meta_state *state = &device->meta_state;
   VkResult result;

   const VkDescriptorSetLayoutBinding bindings[] = {
      {
         .binding = 0,
         .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
         .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
      },
      {
         .binding = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
      },
   };

   result = radv_meta_create_descriptor_set_layout(device, 2, bindings, &state->copy_vrs_htile_ds_layout);
   if (result != VK_SUCCESS)
      return result;

   const VkPushConstantRange pc_range = {
      .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
      .size = 20,
   };

   result = radv_meta_create_pipeline_layout(device, &state->copy_vrs_htile_ds_layout, 1, &pc_range,
                                             &state->copy_vrs_htile_p_layout);
   if (result != VK_SUCCESS)
      return result;

   nir_shader *cs = build_copy_vrs_htile_shader(device, surf);

   result =
      radv_meta_create_compute_pipeline(device, cs, state->copy_vrs_htile_p_layout, &state->copy_vrs_htile_pipeline);

   ralloc_free(cs);
   return result;
}

static VkResult
get_pipeline(struct radv_device *device, struct radv_image *image, VkPipeline *pipeline_out)
{
   struct radv_meta_state *state = &device->meta_state;
   VkResult result = VK_SUCCESS;

   mtx_lock(&state->mtx);
   if (!state->copy_vrs_htile_pipeline) {
      result = create_pipeline(device, &image->planes[0].surface);
      if (result != VK_SUCCESS)
         goto fail;
   }

   *pipeline_out = state->copy_vrs_htile_pipeline;

fail:
   mtx_unlock(&state->mtx);
   return result;
}

void
radv_copy_vrs_htile(struct radv_cmd_buffer *cmd_buffer, struct radv_image_view *vrs_iview, const VkRect2D *rect,
                    struct radv_image *dst_image, struct radv_buffer *htile_buffer, bool read_htile_value)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   struct radv_meta_state *state = &device->meta_state;
   struct radv_meta_saved_state saved_state;
   VkPipeline pipeline;
   VkResult result;

   assert(radv_image_has_htile(dst_image));

   result = get_pipeline(device, dst_image, &pipeline);
   if (result != VK_SUCCESS) {
      vk_command_buffer_set_error(&cmd_buffer->vk, result);
      return;
   }

   cmd_buffer->state.flush_bits |=
      radv_src_access_flush(cmd_buffer, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, NULL) |
      radv_dst_access_flush(cmd_buffer, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT, NULL);

   radv_meta_save(&saved_state, cmd_buffer,
                  RADV_META_SAVE_COMPUTE_PIPELINE | RADV_META_SAVE_CONSTANTS | RADV_META_SAVE_DESCRIPTORS);

   radv_CmdBindPipeline(radv_cmd_buffer_to_handle(cmd_buffer), VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);

   radv_meta_push_descriptor_set(
      cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, state->copy_vrs_htile_p_layout, 0, 2,
      (VkWriteDescriptorSet[]){{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                .dstBinding = 0,
                                .dstArrayElement = 0,
                                .descriptorCount = 1,
                                .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                .pImageInfo =
                                   (VkDescriptorImageInfo[]){
                                      {
                                         .sampler = VK_NULL_HANDLE,
                                         .imageView = radv_image_view_to_handle(vrs_iview),
                                         .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
                                      },
                                   }},
                               {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                .dstBinding = 1,
                                .dstArrayElement = 0,
                                .descriptorCount = 1,
                                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                .pBufferInfo = &(VkDescriptorBufferInfo){.buffer = radv_buffer_to_handle(htile_buffer),
                                                                         .offset = 0,
                                                                         .range = htile_buffer->vk.size}}});

   const unsigned constants[5] = {
      rect->offset.x,
      rect->offset.y,
      dst_image->planes[0].surface.meta_pitch,
      dst_image->planes[0].surface.meta_slice_size,
      read_htile_value,
   };

   vk_common_CmdPushConstants(radv_cmd_buffer_to_handle(cmd_buffer), state->copy_vrs_htile_p_layout,
                              VK_SHADER_STAGE_COMPUTE_BIT, 0, 20, constants);

   uint32_t width = DIV_ROUND_UP(rect->extent.width, 8);
   uint32_t height = DIV_ROUND_UP(rect->extent.height, 8);

   radv_unaligned_dispatch(cmd_buffer, width, height, 1);

   radv_meta_restore(&saved_state, cmd_buffer);

   cmd_buffer->state.flush_bits |=
      RADV_CMD_FLAG_CS_PARTIAL_FLUSH | RADV_CMD_FLAG_INV_VCACHE |
      radv_src_access_flush(cmd_buffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT, NULL);
}
