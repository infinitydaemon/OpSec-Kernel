/*
 * Copyright 2024 Valve Corporation
 * SPDX-License-Identifier: MIT
 */

#include "compiler/nir/nir.h"
#include "compiler/nir/nir_builder.h"
#include "util/bitscan.h"
#include "util/macros.h"
#include "agx_compile.h"
#include "agx_helpers.h"
#include "agx_pack.h"
#include "agx_uvs.h"
#include "nir_builder_opcodes.h"
#include "nir_intrinsics.h"
#include "nir_intrinsics_indices.h"
#include "shader_enums.h"

struct ctx {
   nir_def *layer, *viewport;
   nir_cursor after_layer_viewport;
   struct agx_unlinked_uvs_layout *layout;
};

static enum uvs_group
group_for_varying(gl_varying_slot loc)
{
   switch (loc) {
   case VARYING_SLOT_POS:
      return UVS_POSITION;
   case VARYING_SLOT_PSIZ:
      return UVS_PSIZ;
   default:
      return UVS_VARYINGS;
   }
}

static bool
lower(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   struct ctx *ctx = data;
   if (intr->intrinsic != nir_intrinsic_store_output)
      return false;

   b->cursor = nir_instr_remove(&intr->instr);

   nir_io_semantics sem = nir_intrinsic_io_semantics(intr);
   unsigned component = nir_intrinsic_component(intr);

   nir_def *value = intr->src[0].ssa;
   nir_def *offset = intr->src[1].ssa;

   /* If there is only 1 user varying, it is at the base of the varying section.
    * This saves us an indirection on simple separate shaders.
    */
   bool single_vary = util_is_power_of_two_nonzero64(ctx->layout->written);
   enum uvs_group group = group_for_varying(sem.location);

   nir_def *base;
   if ((group == UVS_VARYINGS) && !single_vary)
      base = nir_load_uvs_index_agx(b, .io_semantics = sem);
   else
      base = nir_imm_intN_t(b, ctx->layout->group_offs[group], 16);

   nir_def *index = nir_iadd(b, nir_iadd_imm(b, base, component),
                             nir_imul_imm(b, nir_u2u16(b, offset), 4));

   if (sem.location != VARYING_SLOT_LAYER)
      nir_store_uvs_agx(b, value, index);

   /* Insert clip distance sysval writes, and gather layer/viewport writes so we
    * can accumulate their system value. These are still lowered like normal to
    * write them for the varying FS input.
    */
   if (sem.location == VARYING_SLOT_LAYER) {
      assert(ctx->layer == NULL && "only written once");
      ctx->layer = value;
      ctx->after_layer_viewport = nir_after_instr(index->parent_instr);
   } else if (sem.location == VARYING_SLOT_VIEWPORT) {
      assert(ctx->viewport == NULL && "only written once");
      ctx->viewport = value;
      ctx->after_layer_viewport = nir_after_instr(index->parent_instr);
   } else if (sem.location == VARYING_SLOT_CLIP_DIST0 ||
              sem.location == VARYING_SLOT_CLIP_DIST1) {

      unsigned clip_base = ctx->layout->group_offs[UVS_CLIP_DIST];
      unsigned c = 4 * (sem.location - VARYING_SLOT_CLIP_DIST0) + component;

      if (c < b->shader->info.clip_distance_array_size) {
         nir_def *index = nir_iadd_imm(
            b, nir_imul_imm(b, nir_u2u16(b, offset), 4), clip_base + c);

         nir_store_uvs_agx(b, value, index);
      }
   }

   /* Combined clip/cull used */
   assert(sem.location != VARYING_SLOT_CULL_DIST0);
   assert(sem.location != VARYING_SLOT_CULL_DIST1);

   return true;
}

static void
write_layer_viewport_sysval(struct ctx *ctx)
{
   nir_builder b = nir_builder_at(ctx->after_layer_viewport);

   nir_def *zero = nir_imm_intN_t(&b, 0, 16);
   nir_def *layer = ctx->layer ? nir_u2u16(&b, ctx->layer) : zero;
   nir_def *viewport = ctx->viewport ? nir_u2u16(&b, ctx->viewport) : zero;

   nir_store_uvs_agx(
      &b, nir_pack_32_2x16_split(&b, layer, viewport),
      nir_imm_int(&b, ctx->layout->group_offs[UVS_LAYER_VIEWPORT]));
}

static bool
gather_components(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   struct agx_unlinked_uvs_layout *layout = data;
   if (intr->intrinsic != nir_intrinsic_store_output)
      return false;

   nir_io_semantics sem = nir_intrinsic_io_semantics(intr);
   unsigned component = nir_intrinsic_component(intr);

   if (nir_src_is_const(intr->src[1])) {
      unsigned loc = sem.location + nir_src_as_uint(intr->src[1]);
      layout->components[loc] = MAX2(layout->components[loc], component + 1);
   } else {
      for (unsigned i = 0; i < sem.num_slots; ++i) {
         layout->components[sem.location + i] = 4;
      }
   }

   return false;
}

bool
agx_nir_lower_uvs(nir_shader *s, struct agx_unlinked_uvs_layout *layout)
{
   bool progress = false;

   /* Scalarize up front so we can ignore vectors later */
   NIR_PASS(progress, s, nir_lower_io_to_scalar, nir_var_shader_out, NULL,
            NULL);

   /* Determine the unlinked UVS layout */
   NIR_PASS(progress, s, nir_shader_intrinsics_pass, gather_components,
            nir_metadata_control_flow, layout);

   unsigned sizes[UVS_NUM_GROUP] = {
      [UVS_POSITION] = 4,
      [UVS_PSIZ] = !!(s->info.outputs_written & VARYING_BIT_PSIZ),
      [UVS_LAYER_VIEWPORT] = !!(s->info.outputs_written &
                                (VARYING_BIT_LAYER | VARYING_BIT_VIEWPORT)),
      [UVS_CLIP_DIST] = s->info.clip_distance_array_size,
   };

   for (unsigned i = 0; i < ARRAY_SIZE(layout->components); ++i) {
      if (i != VARYING_SLOT_POS && i != VARYING_SLOT_PSIZ &&
          i != VARYING_SLOT_LAYER && layout->components[i]) {

         layout->written |= BITFIELD64_BIT(i);
         sizes[UVS_VARYINGS] += layout->components[i];
      }
   }

   unsigned offs = 0;
   for (enum uvs_group g = 0; g < UVS_NUM_GROUP; ++g) {
      layout->group_offs[g] = offs;
      offs += sizes[g];
   }

   layout->size = offs;
   layout->user_size = sizes[UVS_VARYINGS];

   /* Now lower in terms of the unlinked layout */
   struct ctx ctx = {.layout = layout};
   NIR_PASS(progress, s, nir_shader_intrinsics_pass, lower,
            nir_metadata_control_flow, &ctx);

   if (ctx.layer || ctx.viewport) {
      write_layer_viewport_sysval(&ctx);
   }

   /* Finally, pack what we can. It's much cheaper to do this at compile-time
    * than draw-time.
    */
   agx_pack(&layout->osel, OUTPUT_SELECT, cfg) {
      cfg.point_size = sizes[UVS_PSIZ];
      cfg.viewport_target = sizes[UVS_LAYER_VIEWPORT];
      cfg.render_target = cfg.viewport_target;

      cfg.clip_distance_plane_0 = sizes[UVS_CLIP_DIST] > 0;
      cfg.clip_distance_plane_1 = sizes[UVS_CLIP_DIST] > 1;
      cfg.clip_distance_plane_2 = sizes[UVS_CLIP_DIST] > 2;
      cfg.clip_distance_plane_3 = sizes[UVS_CLIP_DIST] > 3;
      cfg.clip_distance_plane_4 = sizes[UVS_CLIP_DIST] > 4;
      cfg.clip_distance_plane_5 = sizes[UVS_CLIP_DIST] > 5;
      cfg.clip_distance_plane_6 = sizes[UVS_CLIP_DIST] > 6;
      cfg.clip_distance_plane_7 = sizes[UVS_CLIP_DIST] > 7;
   }

   agx_pack(&layout->vdm, VDM_STATE_VERTEX_OUTPUTS, cfg) {
      cfg.output_count_1 = offs;
      cfg.output_count_2 = offs;
   }

   return progress;
}

void
agx_assign_uvs(struct agx_varyings_vs *varyings,
               struct agx_unlinked_uvs_layout *layout, uint64_t flat_mask,
               uint64_t linear_mask)
{
   *varyings = (struct agx_varyings_vs){0};

   /* These are always flat-shaded from the FS perspective */
   flat_mask |= VARYING_BIT_LAYER | VARYING_BIT_VIEWPORT;

   /* The internal cull distance slots are always linearly-interpolated */
   linear_mask |= BITFIELD64_RANGE(VARYING_SLOT_CULL_PRIMITIVE, 2);

   assert(!(flat_mask & linear_mask));

   /* TODO: Link FP16 varyings */
   unsigned num_32_smooth = 0, num_32_flat = 0, num_32_linear = 0;
   struct {
      uint32_t *num;
      uint64_t mask;
   } parts[] = {
      {&num_32_smooth, ~flat_mask & ~linear_mask},
      {&num_32_flat, flat_mask},
      {&num_32_linear, linear_mask},
   };

   unsigned base = layout->group_offs[UVS_VARYINGS];

   for (unsigned p = 0; p < ARRAY_SIZE(parts); ++p) {
      u_foreach_bit64(loc, parts[p].mask & layout->written) {
         assert(loc < ARRAY_SIZE(varyings->slots));
         varyings->slots[loc] = base;

         base += layout->components[loc];
         (*parts[p].num) += layout->components[loc];
      }
   }

   agx_pack(&varyings->counts_32, VARYING_COUNTS, cfg) {
      cfg.smooth = num_32_smooth;
      cfg.flat = num_32_flat;
      cfg.linear = num_32_linear;
   }

   agx_pack(&varyings->counts_16, VARYING_COUNTS, cfg) {
      cfg.smooth = 0;
      cfg.flat = 0;
      cfg.linear = 0;
   }
}

static inline enum agx_shade_model
translate_flat_shade_model(unsigned provoking_vertex)
{
   static_assert(AGX_SHADE_MODEL_FLAT_VERTEX_0 == 0, "hw");
   static_assert(AGX_SHADE_MODEL_FLAT_VERTEX_2 == 2, "hw");

   assert(provoking_vertex <= 2);

   if (provoking_vertex == 1)
      return AGX_SHADE_MODEL_FLAT_VERTEX_1;
   else
      return (enum agx_shade_model)provoking_vertex;
}

void
agx_link_varyings_vs_fs(void *out, struct agx_varyings_vs *vs,
                        unsigned nr_user_indices, struct agx_varyings_fs *fs,
                        unsigned provoking_vertex, uint8_t sprite_coord_enable,
                        bool *generate_primitive_id)
{
   assert(fs->nr_bindings > 0);

   *generate_primitive_id = false;

   struct agx_cf_binding_header_packed *header = out;
   struct agx_cf_binding_packed *bindings = (void *)(header + 1);

   unsigned user_base = 1 + (fs->reads_z ? 1 : 0);
   unsigned nr_slots = user_base + nr_user_indices;

   agx_pack(header, CF_BINDING_HEADER, cfg) {
      cfg.number_of_32_bit_slots = nr_slots;
      cfg.number_of_coefficient_registers = fs->nr_cf;
   }

   for (unsigned i = 0; i < fs->nr_bindings; ++i) {
      struct agx_cf_binding b = fs->bindings[i];

      agx_pack(bindings + i, CF_BINDING, cfg) {
         cfg.base_coefficient_register = b.cf_base;
         cfg.components = b.count;

         if (b.smooth) {
            cfg.shade_model = b.perspective ? AGX_SHADE_MODEL_PERSPECTIVE
                                            : AGX_SHADE_MODEL_LINEAR;
         } else {
            cfg.shade_model = translate_flat_shade_model(provoking_vertex);
         }

         if (b.slot == VARYING_SLOT_PNTC ||
             (b.slot >= VARYING_SLOT_TEX0 && b.slot <= VARYING_SLOT_TEX7 &&
              (sprite_coord_enable &
               BITFIELD_BIT(b.slot - VARYING_SLOT_TEX0)))) {

            assert(b.offset == 0);
            cfg.source = AGX_COEFFICIENT_SOURCE_POINT_COORD;
         } else if (b.slot == VARYING_SLOT_PRIMITIVE_ID &&
                    !vs->slots[VARYING_SLOT_PRIMITIVE_ID]) {
            cfg.source = AGX_COEFFICIENT_SOURCE_PRIMITIVE_ID;
            *generate_primitive_id = true;
         } else if (b.slot == VARYING_SLOT_POS) {
            assert(b.offset >= 2 && "gl_Position.xy are not varyings");
            assert(fs->reads_z || b.offset != 2);

            if (b.offset == 2) {
               cfg.source = AGX_COEFFICIENT_SOURCE_FRAGCOORD_Z;
               cfg.base_slot = 1;
            } else {
               assert(!b.perspective && "W must not be perspective divided");
            }
         } else {
            unsigned vs_index = vs->slots[b.slot];
            assert(b.offset < 4);

            /* Varyings not written by vertex shader are undefined but we can't
             * crash */
            if (vs_index) {
               assert(vs_index >= 4 &&
                      "gl_Position should have been the first 4 slots");

               cfg.base_slot = user_base + (vs_index - 4) + b.offset;
            }
         }

         assert(cfg.base_coefficient_register + cfg.components <= fs->nr_cf &&
                "overflowed coefficient registers");
      }
   }
}
