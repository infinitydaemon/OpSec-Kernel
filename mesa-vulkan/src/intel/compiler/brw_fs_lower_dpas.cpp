/*
 * Copyright 2023 Intel Corporation
 * SPDX-License-Identifier: MIT
 */

#include "brw_fs.h"
#include "brw_fs_builder.h"

using namespace brw;

static void
f16_using_mac(const fs_builder &bld, fs_inst *inst)
{
   /* We only intend to support configurations where the destination and
    * accumulator have the same type.
    */
   if (!inst->src[0].is_null())
      assert(inst->dst.type == inst->src[0].type);

   assert(inst->src[1].type == BRW_TYPE_HF);
   assert(inst->src[2].type == BRW_TYPE_HF);

   const brw_reg_type src0_type = inst->dst.type;
   const brw_reg_type src1_type = BRW_TYPE_HF;
   const brw_reg_type src2_type = BRW_TYPE_HF;

   const brw_reg dest = inst->dst;
   brw_reg src0 = inst->src[0];
   const brw_reg src1 = retype(inst->src[1], src1_type);
   const brw_reg src2 = retype(inst->src[2], src2_type);

   const unsigned dest_stride =
      dest.type == BRW_TYPE_HF ? REG_SIZE / 2 : REG_SIZE;

   for (unsigned r = 0; r < inst->rcount; r++) {
      brw_reg temp = bld.vgrf(BRW_TYPE_HF);

      for (unsigned subword = 0; subword < 2; subword++) {
         for (unsigned s = 0; s < inst->sdepth; s++) {
            /* The first multiply of the dot-product operation has to
             * explicitly write the accumulator register. The successive MAC
             * instructions will implicitly read *and* write the
             * accumulator. Those MAC instructions can also optionally
             * explicitly write some other register.
             *
             * FINISHME: The accumulator can actually hold 16 HF values. On
             * Gfx12 there are two accumulators. It should be possible to do
             * this in SIMD16 or even SIMD32. I was unable to get this to work
             * properly.
             */
            if (s == 0 && subword == 0) {
               const unsigned acc_width = 8;
               brw_reg acc = suboffset(retype(brw_acc_reg(inst->exec_size), BRW_TYPE_UD),
                                      inst->group % acc_width);

               if (bld.shader->devinfo->verx10 >= 125) {
                  acc = subscript(acc, BRW_TYPE_HF, subword);
               } else {
                  acc = retype(acc, BRW_TYPE_HF);
               }

               bld.MUL(acc,
                       subscript(retype(byte_offset(src1, s * REG_SIZE),
                                        BRW_TYPE_UD),
                                 BRW_TYPE_HF, subword),
                       component(retype(byte_offset(src2, r * REG_SIZE),
                                        BRW_TYPE_HF),
                                 s * 2 + subword))
                  ->writes_accumulator = true;

            } else {
               brw_reg result;

               /* As mentioned above, the MAC had an optional, explicit
                * destination register. Various optimization passes are not
                * clever enough to understand the intricacies of this
                * instruction, so only write the result register on the final
                * MAC in the sequence.
                */
               if ((s + 1) == inst->sdepth && subword == 1)
                  result = temp;
               else
                  result = retype(bld.null_reg_ud(), BRW_TYPE_HF);

               bld.MAC(result,
                       subscript(retype(byte_offset(src1, s * REG_SIZE),
                                        BRW_TYPE_UD),
                                 BRW_TYPE_HF, subword),
                       component(retype(byte_offset(src2, r * REG_SIZE),
                                        BRW_TYPE_HF),
                                 s * 2 + subword))
                  ->writes_accumulator = true;
            }
         }
      }

      if (!src0.is_null()) {
         if (src0_type != BRW_TYPE_HF) {
            brw_reg temp2 = bld.vgrf(src0_type);

            bld.MOV(temp2, temp);

            bld.ADD(byte_offset(dest, r * dest_stride),
                    temp2,
                    byte_offset(src0, r * dest_stride));
         } else {
            bld.ADD(byte_offset(dest, r * dest_stride),
                    temp,
                    byte_offset(src0, r * dest_stride));
         }
      } else {
         bld.MOV(byte_offset(dest, r * dest_stride), temp);
      }
   }
}

static void
int8_using_dp4a(const fs_builder &bld, fs_inst *inst)
{
   /* We only intend to support configurations where the destination and
    * accumulator have the same type.
    */
   if (!inst->src[0].is_null())
      assert(inst->dst.type == inst->src[0].type);

   assert(inst->src[1].type == BRW_TYPE_B ||
          inst->src[1].type == BRW_TYPE_UB);
   assert(inst->src[2].type == BRW_TYPE_B ||
          inst->src[2].type == BRW_TYPE_UB);

   const brw_reg_type src1_type = inst->src[1].type == BRW_TYPE_UB
      ? BRW_TYPE_UD : BRW_TYPE_D;

   const brw_reg_type src2_type = inst->src[2].type == BRW_TYPE_UB
      ? BRW_TYPE_UD : BRW_TYPE_D;

   brw_reg dest = inst->dst;
   brw_reg src0 = inst->src[0];
   const brw_reg src1 = retype(inst->src[1], src1_type);
   const brw_reg src2 = retype(inst->src[2], src2_type);

   const unsigned dest_stride = reg_unit(bld.shader->devinfo) * REG_SIZE;

   for (unsigned r = 0; r < inst->rcount; r++) {
      if (!src0.is_null()) {
         bld.MOV(dest, src0);
         src0 = byte_offset(src0, dest_stride);
      } else {
         bld.MOV(dest, retype(brw_imm_d(0), dest.type));
      }

      for (unsigned s = 0; s < inst->sdepth; s++) {
         bld.DP4A(dest,
                  dest,
                  byte_offset(src1, s * inst->exec_size * 4),
                  component(byte_offset(src2, r * inst->sdepth * 4), s))
            ->saturate = inst->saturate;
      }

      dest = byte_offset(dest, dest_stride);
   }
}

static void
int8_using_mul_add(const fs_builder &bld, fs_inst *inst)
{
   /* We only intend to support configurations where the destination and
    * accumulator have the same type.
    */
   if (!inst->src[0].is_null())
      assert(inst->dst.type == inst->src[0].type);

   assert(inst->src[1].type == BRW_TYPE_B ||
          inst->src[1].type == BRW_TYPE_UB);
   assert(inst->src[2].type == BRW_TYPE_B ||
          inst->src[2].type == BRW_TYPE_UB);

   const brw_reg_type src0_type = inst->dst.type;

   const brw_reg_type src1_type = inst->src[1].type == BRW_TYPE_UB
      ? BRW_TYPE_UD : BRW_TYPE_D;

   const brw_reg_type src2_type = inst->src[2].type == BRW_TYPE_UB
      ? BRW_TYPE_UD : BRW_TYPE_D;

   brw_reg dest = inst->dst;
   brw_reg src0 = inst->src[0];
   const brw_reg src1 = retype(inst->src[1], src1_type);
   const brw_reg src2 = retype(inst->src[2], src2_type);

   const unsigned dest_stride = REG_SIZE;

   for (unsigned r = 0; r < inst->rcount; r++) {
      if (!src0.is_null()) {
         bld.MOV(dest, src0);
         src0 = byte_offset(src0, dest_stride);
      } else {
         bld.MOV(dest, retype(brw_imm_d(0), dest.type));
      }

      for (unsigned s = 0; s < inst->sdepth; s++) {
         brw_reg temp1 = bld.vgrf(BRW_TYPE_UD);
         brw_reg temp2 = bld.vgrf(BRW_TYPE_UD);
         brw_reg temp3 = bld.vgrf(BRW_TYPE_UD, 2);
         const brw_reg_type temp_type =
            (inst->src[1].type == BRW_TYPE_B ||
             inst->src[2].type == BRW_TYPE_B)
            ? BRW_TYPE_W : BRW_TYPE_UW;

         /* Expand 8 dwords of packed bytes into 16 dwords of packed
          * words.
          *
          * FINISHME: Gfx9 should not need this work around. Gfx11
          * may be able to use integer MAD. Both platforms may be
          * able to use MAC.
          */
         bld.group(32, 0).MOV(retype(temp3, temp_type),
                              retype(byte_offset(src2, r * REG_SIZE),
                                     inst->src[2].type));

         bld.MUL(subscript(temp1, temp_type, 0),
                 subscript(retype(byte_offset(src1, s * REG_SIZE),
                                  BRW_TYPE_UD),
                           inst->src[1].type, 0),
                 subscript(component(retype(temp3, BRW_TYPE_UD),
                                     s * 2),
                           temp_type, 0));

         bld.MUL(subscript(temp1, temp_type, 1),
                 subscript(retype(byte_offset(src1, s * REG_SIZE),
                                  BRW_TYPE_UD),
                           inst->src[1].type, 1),
                 subscript(component(retype(temp3, BRW_TYPE_UD),
                                     s * 2),
                           temp_type, 1));

         bld.MUL(subscript(temp2, temp_type, 0),
                 subscript(retype(byte_offset(src1, s * REG_SIZE),
                                  BRW_TYPE_UD),
                           inst->src[1].type, 2),
                 subscript(component(retype(temp3, BRW_TYPE_UD),
                                     s * 2 + 1),
                           temp_type, 0));

         bld.MUL(subscript(temp2, temp_type, 1),
                 subscript(retype(byte_offset(src1, s * REG_SIZE),
                                  BRW_TYPE_UD),
                           inst->src[1].type, 3),
                 subscript(component(retype(temp3, BRW_TYPE_UD),
                                     s * 2 + 1),
                           temp_type, 1));

         bld.ADD(subscript(temp1, src0_type, 0),
                 subscript(temp1, temp_type, 0),
                 subscript(temp1, temp_type, 1));

         bld.ADD(subscript(temp2, src0_type, 0),
                 subscript(temp2, temp_type, 0),
                 subscript(temp2, temp_type, 1));

         bld.ADD(retype(temp1, src0_type),
                 retype(temp1, src0_type),
                 retype(temp2, src0_type));

         bld.ADD(dest, dest, retype(temp1, src0_type))
            ->saturate = inst->saturate;
      }

      dest = byte_offset(dest, dest_stride);
   }
}

bool
brw_fs_lower_dpas(fs_visitor &v)
{
   bool progress = false;

   foreach_block_and_inst_safe(block, fs_inst, inst, v.cfg) {
      if (inst->opcode != BRW_OPCODE_DPAS)
         continue;

      const unsigned exec_size = v.devinfo->ver >= 20 ? 16 : 8;
      const fs_builder bld = fs_builder(&v, block, inst).group(exec_size, 0).exec_all();

      if (brw_type_is_float(inst->dst.type)) {
         f16_using_mac(bld, inst);
      } else {
         if (v.devinfo->ver >= 12) {
            int8_using_dp4a(bld, inst);
         } else {
            int8_using_mul_add(bld, inst);
         }
      }

      inst->remove(block);
      progress = true;
   }

   if (progress)
      v.invalidate_analysis(DEPENDENCY_INSTRUCTIONS);

   return progress;
}
