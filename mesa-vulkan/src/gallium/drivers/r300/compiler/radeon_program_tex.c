/*
 * Copyright 2010 Corbin Simpson
 * Copyright 2010 Marek Olšák <maraeo@gmail.com>
 * SPDX-License-Identifier: MIT
 */

#include "radeon_program_tex.h"

#include "radeon_compiler_util.h"

/* Series of transformations to be done on textures. */

static void
scale_texcoords(struct r300_fragment_program_compiler *compiler, struct rc_instruction *inst,
                unsigned state_constant)
{
   struct rc_instruction *inst_mov;

   unsigned temp = rc_find_free_temporary(&compiler->Base);

   inst_mov = rc_insert_new_instruction(&compiler->Base, inst->Prev);

   inst_mov->U.I.Opcode = RC_OPCODE_MUL;
   inst_mov->U.I.DstReg.File = RC_FILE_TEMPORARY;
   inst_mov->U.I.DstReg.Index = temp;
   inst_mov->U.I.SrcReg[0] = inst->U.I.SrcReg[0];
   inst_mov->U.I.SrcReg[1].File = RC_FILE_CONSTANT;
   inst_mov->U.I.SrcReg[1].Index = rc_constants_add_state(&compiler->Base.Program.Constants,
                                                          state_constant, inst->U.I.TexSrcUnit);

   reset_srcreg(&inst->U.I.SrcReg[0]);
   inst->U.I.SrcReg[0].File = RC_FILE_TEMPORARY;
   inst->U.I.SrcReg[0].Index = temp;
}

static void
projective_divide(struct r300_fragment_program_compiler *compiler, struct rc_instruction *inst)
{
   struct rc_instruction *inst_mul, *inst_rcp;

   /* Make sure there is no temp reusing, some later passes depend on the SSA-like form. */
   unsigned temp_rcp = rc_find_free_temporary(&compiler->Base);
   unsigned temp_mul = rc_find_free_temporary(&compiler->Base);

   inst_rcp = rc_insert_new_instruction(&compiler->Base, inst->Prev);
   inst_rcp->U.I.Opcode = RC_OPCODE_RCP;
   inst_rcp->U.I.DstReg.File = RC_FILE_TEMPORARY;
   inst_rcp->U.I.DstReg.Index = temp_rcp;
   inst_rcp->U.I.DstReg.WriteMask = RC_MASK_W;
   inst_rcp->U.I.SrcReg[0] = inst->U.I.SrcReg[0];
   /* Because the input can be arbitrarily swizzled,
    * read the component mapped to W. */
   inst_rcp->U.I.SrcReg[0].Swizzle = RC_MAKE_SWIZZLE_SMEAR(GET_SWZ(inst->U.I.SrcReg[0].Swizzle, 3));

   inst_mul = rc_insert_new_instruction(&compiler->Base, inst->Prev);
   inst_mul->U.I.Opcode = RC_OPCODE_MUL;
   inst_mul->U.I.DstReg.File = RC_FILE_TEMPORARY;
   inst_mul->U.I.DstReg.Index = temp_mul;
   inst_mul->U.I.SrcReg[0] = inst->U.I.SrcReg[0];
   inst_mul->U.I.SrcReg[1].File = RC_FILE_TEMPORARY;
   inst_mul->U.I.SrcReg[1].Index = temp_rcp;
   inst_mul->U.I.SrcReg[1].Swizzle = RC_SWIZZLE_WWWW;

   reset_srcreg(&inst->U.I.SrcReg[0]);
   inst->U.I.Opcode = RC_OPCODE_TEX;
   inst->U.I.SrcReg[0].File = RC_FILE_TEMPORARY;
   inst->U.I.SrcReg[0].Index = temp_mul;
}

/**
 * Transform TEX, TXP, TXB, and KIL instructions in the following ways:
 *  - implement texture compare (shadow extensions)
 *  - extract non-native source / destination operands
 *  - premultiply texture coordinates for RECT
 *  - extract operand swizzles
 *  - introduce a temporary register when write masks are needed
 */
int
radeonTransformTEX(struct radeon_compiler *c, struct rc_instruction *inst, void *data)
{
   struct r300_fragment_program_compiler *compiler = (struct r300_fragment_program_compiler *)data;
   rc_wrap_mode wrapmode = compiler->state.unit[inst->U.I.TexSrcUnit].wrap_mode;
   int is_rect = inst->U.I.TexSrcTarget == RC_TEXTURE_RECT;

   if (inst->U.I.Opcode != RC_OPCODE_TEX && inst->U.I.Opcode != RC_OPCODE_TXB &&
       inst->U.I.Opcode != RC_OPCODE_TXP && inst->U.I.Opcode != RC_OPCODE_TXD &&
       inst->U.I.Opcode != RC_OPCODE_TXL && inst->U.I.Opcode != RC_OPCODE_KIL)
      return 0;

   /* R300 cannot sample from rectangles and the wrap mode fallback needs
    * normalized coordinates anyway. */
   if (inst->U.I.Opcode != RC_OPCODE_KIL && is_rect && (!c->is_r500 || wrapmode != RC_WRAP_NONE)) {
      scale_texcoords(compiler, inst, RC_STATE_R300_TEXRECT_FACTOR);
      inst->U.I.TexSrcTarget = RC_TEXTURE_2D;
   }

   /* Divide by W if needed. */
   if (inst->U.I.Opcode == RC_OPCODE_TXP &&
       (wrapmode == RC_WRAP_REPEAT || wrapmode == RC_WRAP_MIRRORED_REPEAT ||
        compiler->state.unit[inst->U.I.TexSrcUnit].clamp_and_scale_before_fetch)) {
      projective_divide(compiler, inst);
   }

   /* Texture wrap modes don't work on NPOT textures.
    *
    * Non-wrapped/clamped texcoords with NPOT are free in HW. Repeat and
    * mirroring are not. If we need to repeat, we do:
    *
    * MUL temp, texcoord, <scaling factor constant>
    * FRC temp, temp ; Discard integer portion of coords
    *
    * This gives us coords in [0, 1].
    *
    * Mirroring is trickier. We're going to start out like repeat:
    *
    * MUL temp, texcoord, <scaling factor constant> ; De-mirror across axes
    * MUL temp, temp, 0.5 ; Pattern repeats in [0, 2]
    *                            ; so scale to [0, 1]
    * FRC temp, temp ; Make the pattern repeat
    * MAD temp, temp, 2, -1 ; Move the pattern to [-1, 1]
    * ADD temp, 1, -abs(temp) ; Now comes a neat trick: use abs to mirror the pattern.
    *				; The pattern is backwards, so reverse it (1-x).
    *
    * This gives us coords in [0, 1].
    *
    * ~ C & M. ;)
    */
   if (inst->U.I.Opcode != RC_OPCODE_KIL && wrapmode != RC_WRAP_NONE) {
      struct rc_instruction *inst_mov;
      unsigned temp = rc_find_free_temporary(c);

      if (wrapmode == RC_WRAP_REPEAT) {
         /* Both instructions will be paired up. */
         struct rc_instruction *inst_frc = rc_insert_new_instruction(c, inst->Prev);

         inst_frc->U.I.Opcode = RC_OPCODE_FRC;
         inst_frc->U.I.DstReg.File = RC_FILE_TEMPORARY;
         inst_frc->U.I.DstReg.Index = temp;
         inst_frc->U.I.DstReg.WriteMask = RC_MASK_XYZ;
         inst_frc->U.I.SrcReg[0] = inst->U.I.SrcReg[0];
      } else if (wrapmode == RC_WRAP_MIRRORED_REPEAT) {
         /*
          * Function:
          *   f(v) = 1 - abs(frac(v * 0.5) * 2 - 1)
          *
          * Code:
          *   MUL temp, src0, 0.5
          *   FRC temp, temp
          *   MAD temp, temp, 2, -1
          *   ADD temp, 1, -abs(temp)
          */

         struct rc_instruction *inst_mul, *inst_frc, *inst_mad, *inst_add;
         unsigned two, two_swizzle;

         inst_mul = rc_insert_new_instruction(c, inst->Prev);
         unsigned temp_mul = rc_find_free_temporary(c);

         inst_mul->U.I.Opcode = RC_OPCODE_MUL;
         inst_mul->U.I.DstReg.File = RC_FILE_TEMPORARY;
         inst_mul->U.I.DstReg.Index = temp_mul;
         inst_mul->U.I.DstReg.WriteMask = RC_MASK_XYZ;
         inst_mul->U.I.SrcReg[0] = inst->U.I.SrcReg[0];
         inst_mul->U.I.SrcReg[1].Swizzle = RC_SWIZZLE_HHHH;

         inst_frc = rc_insert_new_instruction(c, inst->Prev);
         unsigned temp_frc = rc_find_free_temporary(c);

         inst_frc->U.I.Opcode = RC_OPCODE_FRC;
         inst_frc->U.I.DstReg.File = RC_FILE_TEMPORARY;
         inst_frc->U.I.DstReg.Index = temp_frc;
         inst_frc->U.I.DstReg.WriteMask = RC_MASK_XYZ;
         inst_frc->U.I.SrcReg[0].File = RC_FILE_TEMPORARY;
         inst_frc->U.I.SrcReg[0].Index = temp_mul;
         inst_frc->U.I.SrcReg[0].Swizzle = RC_SWIZZLE_XYZ0;

         two = rc_constants_add_immediate_scalar(&c->Program.Constants, 2, &two_swizzle);
         inst_mad = rc_insert_new_instruction(c, inst->Prev);
         unsigned temp_mad = rc_find_free_temporary(c);

         inst_mad->U.I.Opcode = RC_OPCODE_MAD;
         inst_mad->U.I.DstReg.File = RC_FILE_TEMPORARY;
         inst_mad->U.I.DstReg.Index = temp_mad;
         inst_mad->U.I.DstReg.WriteMask = RC_MASK_XYZ;
         inst_mad->U.I.SrcReg[0].File = RC_FILE_TEMPORARY;
         inst_mad->U.I.SrcReg[0].Index = temp_frc;
         inst_mad->U.I.SrcReg[0].Swizzle = RC_SWIZZLE_XYZ0;
         inst_mad->U.I.SrcReg[1].File = RC_FILE_CONSTANT;
         inst_mad->U.I.SrcReg[1].Index = two;
         inst_mad->U.I.SrcReg[1].Swizzle = two_swizzle;
         inst_mad->U.I.SrcReg[2].Swizzle = RC_SWIZZLE_1111;
         inst_mad->U.I.SrcReg[2].Negate = RC_MASK_XYZ;

         inst_add = rc_insert_new_instruction(c, inst->Prev);

         inst_add->U.I.Opcode = RC_OPCODE_ADD;
         inst_add->U.I.DstReg.File = RC_FILE_TEMPORARY;
         inst_add->U.I.DstReg.Index = temp;
         inst_add->U.I.DstReg.WriteMask = RC_MASK_XYZ;
         inst_add->U.I.SrcReg[0].Swizzle = RC_SWIZZLE_1111;
         inst_add->U.I.SrcReg[1].File = RC_FILE_TEMPORARY;
         inst_add->U.I.SrcReg[1].Index = temp_mad;
         inst_add->U.I.SrcReg[1].Swizzle = RC_SWIZZLE_XYZ0;
         inst_add->U.I.SrcReg[1].Abs = 1;
         inst_add->U.I.SrcReg[1].Negate = RC_MASK_XYZ;
      } else if (wrapmode == RC_WRAP_MIRRORED_CLAMP) {
         /*
          * Mirrored clamp modes are bloody simple, we just use abs
          * to mirror [0, 1] into [-1, 0]. This works for
          * all modes i.e. CLAMP, CLAMP_TO_EDGE, and CLAMP_TO_BORDER.
          */
         struct rc_instruction *inst_mov;

         inst_mov = rc_insert_new_instruction(c, inst->Prev);

         inst_mov->U.I.Opcode = RC_OPCODE_MOV;
         inst_mov->U.I.DstReg.File = RC_FILE_TEMPORARY;
         inst_mov->U.I.DstReg.Index = temp;
         inst_mov->U.I.DstReg.WriteMask = RC_MASK_XYZ;
         inst_mov->U.I.SrcReg[0] = inst->U.I.SrcReg[0];
         inst_mov->U.I.SrcReg[0].Abs = 1;
      }

      /* Preserve W for TXP/TXB. */
      inst_mov = rc_insert_new_instruction(c, inst->Prev);

      inst_mov->U.I.Opcode = RC_OPCODE_MOV;
      inst_mov->U.I.DstReg.File = RC_FILE_TEMPORARY;
      inst_mov->U.I.DstReg.Index = temp;
      inst_mov->U.I.DstReg.WriteMask = RC_MASK_W;
      inst_mov->U.I.SrcReg[0] = inst->U.I.SrcReg[0];

      reset_srcreg(&inst->U.I.SrcReg[0]);
      inst->U.I.SrcReg[0].File = RC_FILE_TEMPORARY;
      inst->U.I.SrcReg[0].Index = temp;
   }

   /* NPOT -> POT conversion for 3D textures. */
   if (inst->U.I.Opcode != RC_OPCODE_KIL &&
       compiler->state.unit[inst->U.I.TexSrcUnit].clamp_and_scale_before_fetch) {
      struct rc_instruction *inst_mov;
      unsigned temp = rc_find_free_temporary(c);

      /* Saturate XYZ. */
      inst_mov = rc_insert_new_instruction(c, inst->Prev);
      inst_mov->U.I.Opcode = RC_OPCODE_MOV;
      inst_mov->U.I.SaturateMode = RC_SATURATE_ZERO_ONE;
      inst_mov->U.I.DstReg.File = RC_FILE_TEMPORARY;
      inst_mov->U.I.DstReg.Index = temp;
      inst_mov->U.I.DstReg.WriteMask = RC_MASK_XYZ;
      inst_mov->U.I.SrcReg[0] = inst->U.I.SrcReg[0];

      /* Copy W. */
      inst_mov = rc_insert_new_instruction(c, inst->Prev);
      inst_mov->U.I.Opcode = RC_OPCODE_MOV;
      inst_mov->U.I.DstReg.File = RC_FILE_TEMPORARY;
      inst_mov->U.I.DstReg.Index = temp;
      inst_mov->U.I.DstReg.WriteMask = RC_MASK_W;
      inst_mov->U.I.SrcReg[0] = inst->U.I.SrcReg[0];

      reset_srcreg(&inst->U.I.SrcReg[0]);
      inst->U.I.SrcReg[0].File = RC_FILE_TEMPORARY;
      inst->U.I.SrcReg[0].Index = temp;

      scale_texcoords(compiler, inst, RC_STATE_R300_TEXSCALE_FACTOR);
   }

   /* Cannot write texture to output registers or with saturate (all chips),
    * or with masks (non-r500). */
   if (inst->U.I.Opcode != RC_OPCODE_KIL) {
      /* We should not be getting saturates on TEX, but assert just to be sure. */
      assert(!inst->U.I.SaturateMode);

      if (inst->U.I.DstReg.File != RC_FILE_TEMPORARY || inst->U.I.SaturateMode ||
          (!c->is_r500 && inst->U.I.DstReg.WriteMask != RC_MASK_XYZW)) {
         struct rc_instruction *inst_mov = rc_insert_new_instruction(c, inst);

         inst_mov->U.I.Opcode = RC_OPCODE_MOV;
         inst_mov->U.I.SaturateMode = inst->U.I.SaturateMode;
         inst_mov->U.I.DstReg = inst->U.I.DstReg;
         inst_mov->U.I.SrcReg[0].File = RC_FILE_TEMPORARY;
         inst_mov->U.I.SrcReg[0].Index = rc_find_free_temporary(c);

         inst->U.I.DstReg.File = RC_FILE_TEMPORARY;
         inst->U.I.DstReg.Index = inst_mov->U.I.SrcReg[0].Index;
         inst->U.I.DstReg.WriteMask = RC_MASK_XYZW;
      }
   }

   return 1;
}
