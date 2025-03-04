/*
 * Copyright © 2010 Intel Corporation
 * SPDX-License-Identifier: MIT
 */

#include "brw_shader.h"
#include "brw_eu.h"
#include "brw_shader.h"
#include "brw_cfg.h"
#include "brw_compiler.h"
#include "brw_inst.h"
#include "brw_isa_info.h"

static void
initialize_sources(brw_inst *inst, const brw_reg src[], uint8_t num_sources);

void
brw_inst::init(enum opcode opcode, uint8_t exec_size, const brw_reg &dst,
              const brw_reg *src, unsigned sources)
{
   memset((void*)this, 0, sizeof(*this));

   initialize_sources(this, src, sources);

   for (unsigned i = 0; i < sources; i++)
      this->src[i] = src[i];

   this->opcode = opcode;
   this->dst = dst;
   this->exec_size = exec_size;

   assert(dst.file != IMM && dst.file != UNIFORM);

   assert(this->exec_size != 0);

   this->conditional_mod = BRW_CONDITIONAL_NONE;

   /* This will be the case for almost all instructions. */
   switch (dst.file) {
   case VGRF:
   case ADDRESS:
   case ARF:
   case FIXED_GRF:
   case ATTR:
      this->size_written = dst.component_size(exec_size);
      break;
   case BAD_FILE:
      this->size_written = 0;
      break;
   case IMM:
   case UNIFORM:
      unreachable("Invalid destination register file");
   }

   this->writes_accumulator = false;
}

brw_inst::brw_inst()
{
   init(BRW_OPCODE_NOP, 8, dst, NULL, 0);
}

brw_inst::brw_inst(enum opcode opcode, uint8_t exec_size)
{
   init(opcode, exec_size, reg_undef, NULL, 0);
}

brw_inst::brw_inst(enum opcode opcode, uint8_t exec_size, const brw_reg &dst)
{
   init(opcode, exec_size, dst, NULL, 0);
}

brw_inst::brw_inst(enum opcode opcode, uint8_t exec_size, const brw_reg &dst,
                 const brw_reg &src0)
{
   const brw_reg src[1] = { src0 };
   init(opcode, exec_size, dst, src, 1);
}

brw_inst::brw_inst(enum opcode opcode, uint8_t exec_size, const brw_reg &dst,
                 const brw_reg &src0, const brw_reg &src1)
{
   const brw_reg src[2] = { src0, src1 };
   init(opcode, exec_size, dst, src, 2);
}

brw_inst::brw_inst(enum opcode opcode, uint8_t exec_size, const brw_reg &dst,
                 const brw_reg &src0, const brw_reg &src1, const brw_reg &src2)
{
   const brw_reg src[3] = { src0, src1, src2 };
   init(opcode, exec_size, dst, src, 3);
}

brw_inst::brw_inst(enum opcode opcode, uint8_t exec_width, const brw_reg &dst,
                 const brw_reg src[], unsigned sources)
{
   init(opcode, exec_width, dst, src, sources);
}

brw_inst::brw_inst(const brw_inst &that)
{
   memcpy((void*)this, &that, sizeof(that));
   initialize_sources(this, that.src, that.sources);
}

brw_inst::~brw_inst()
{
   if (this->src != this->builtin_src)
      delete[] this->src;
}

static void
initialize_sources(brw_inst *inst, const brw_reg src[], uint8_t num_sources)
{
   if (num_sources > ARRAY_SIZE(inst->builtin_src))
      inst->src = new brw_reg[num_sources];
   else
      inst->src = inst->builtin_src;

   for (unsigned i = 0; i < num_sources; i++)
      inst->src[i] = src[i];

   inst->sources = num_sources;
}

void
brw_inst::resize_sources(uint8_t num_sources)
{
   if (this->sources == num_sources)
      return;

   brw_reg *old_src = this->src;
   brw_reg *new_src;

   const unsigned builtin_size = ARRAY_SIZE(this->builtin_src);

   if (old_src == this->builtin_src) {
      if (num_sources > builtin_size) {
         new_src = new brw_reg[num_sources];
         for (unsigned i = 0; i < this->sources; i++)
            new_src[i] = old_src[i];

      } else {
         new_src = old_src;
      }
   } else {
      if (num_sources <= builtin_size) {
         new_src = this->builtin_src;
         assert(this->sources > num_sources);
         for (unsigned i = 0; i < num_sources; i++)
            new_src[i] = old_src[i];

      } else if (num_sources < this->sources) {
         new_src = old_src;

      } else {
         new_src = new brw_reg[num_sources];
         for (unsigned i = 0; i < this->sources; i++)
            new_src[i] = old_src[i];
      }

      if (old_src != new_src)
         delete[] old_src;
   }

   this->sources = num_sources;
   this->src = new_src;
}

bool
brw_inst::is_send_from_grf() const
{
   switch (opcode) {
   case SHADER_OPCODE_SEND:
   case SHADER_OPCODE_SEND_GATHER:
   case FS_OPCODE_INTERPOLATE_AT_SAMPLE:
   case FS_OPCODE_INTERPOLATE_AT_SHARED_OFFSET:
   case FS_OPCODE_INTERPOLATE_AT_PER_SLOT_OFFSET:
   case SHADER_OPCODE_INTERLOCK:
   case SHADER_OPCODE_MEMORY_FENCE:
   case SHADER_OPCODE_BARRIER:
      return true;
   case FS_OPCODE_UNIFORM_PULL_CONSTANT_LOAD:
      return src[1].file == VGRF;
   default:
      return false;
   }
}

bool
brw_inst::is_control_source(unsigned arg) const
{
   switch (opcode) {
   case FS_OPCODE_UNIFORM_PULL_CONSTANT_LOAD:
      return arg == 0;

   case SHADER_OPCODE_BROADCAST:
   case SHADER_OPCODE_SHUFFLE:
   case SHADER_OPCODE_QUAD_SWIZZLE:
      return arg == 1;

   case FS_OPCODE_INTERPOLATE_AT_SAMPLE:
   case FS_OPCODE_INTERPOLATE_AT_SHARED_OFFSET:
   case FS_OPCODE_INTERPOLATE_AT_PER_SLOT_OFFSET:
      return arg == INTERP_SRC_MSG_DESC || arg == INTERP_SRC_NOPERSPECTIVE;

   case SHADER_OPCODE_MOV_INDIRECT:
   case SHADER_OPCODE_CLUSTER_BROADCAST:
      return arg == 1 || arg == 2;

   case SHADER_OPCODE_SEND:
   case SHADER_OPCODE_SEND_GATHER:
      return arg == 0 || arg == 1;

   case SHADER_OPCODE_MEMORY_LOAD_LOGICAL:
   case SHADER_OPCODE_MEMORY_STORE_LOGICAL:
   case SHADER_OPCODE_MEMORY_ATOMIC_LOGICAL:
      return arg != MEMORY_LOGICAL_BINDING &&
             arg != MEMORY_LOGICAL_ADDRESS &&
             arg != MEMORY_LOGICAL_DATA0 &&
             arg != MEMORY_LOGICAL_DATA1;

   case SHADER_OPCODE_QUAD_SWAP:
   case SHADER_OPCODE_INCLUSIVE_SCAN:
   case SHADER_OPCODE_EXCLUSIVE_SCAN:
   case SHADER_OPCODE_VOTE_ANY:
   case SHADER_OPCODE_VOTE_ALL:
   case SHADER_OPCODE_REDUCE:
      return arg != 0;

   default:
      return false;
   }
}

bool
brw_inst::is_payload(unsigned arg) const
{
   switch (opcode) {
   case FS_OPCODE_INTERPOLATE_AT_PER_SLOT_OFFSET:
   case FS_OPCODE_INTERPOLATE_AT_SAMPLE:
   case FS_OPCODE_INTERPOLATE_AT_SHARED_OFFSET:
   case SHADER_OPCODE_INTERLOCK:
   case SHADER_OPCODE_MEMORY_FENCE:
   case SHADER_OPCODE_BARRIER:
      return arg == 0;

   case SHADER_OPCODE_SEND:
      return arg == 2 || arg == 3;

   case SHADER_OPCODE_SEND_GATHER:
      return arg >= 2;

   default:
      return false;
   }
}

bool
brw_inst::can_do_source_mods(const struct intel_device_info *devinfo) const
{
   if (is_send_from_grf())
      return false;

   /* From TGL PRM Vol 2a Pg. 1053 and Pg. 1069 MAD and MUL Instructions:
    *
    * "When multiplying a DW and any lower precision integer, source modifier
    *  is not supported."
    */
   if (devinfo->ver >= 12 && (opcode == BRW_OPCODE_MUL ||
                              opcode == BRW_OPCODE_MAD)) {
      const brw_reg_type exec_type = get_exec_type(this);
      const unsigned min_brw_type_size_bytes = opcode == BRW_OPCODE_MAD ?
         MIN2(brw_type_size_bytes(src[1].type), brw_type_size_bytes(src[2].type)) :
         MIN2(brw_type_size_bytes(src[0].type), brw_type_size_bytes(src[1].type));

      if (brw_type_is_int(exec_type) &&
          brw_type_size_bytes(exec_type) >= 4 &&
          brw_type_size_bytes(exec_type) != min_brw_type_size_bytes)
         return false;
   }

   switch (opcode) {
   case BRW_OPCODE_ADDC:
   case BRW_OPCODE_BFE:
   case BRW_OPCODE_BFI1:
   case BRW_OPCODE_BFI2:
   case BRW_OPCODE_BFREV:
   case BRW_OPCODE_CBIT:
   case BRW_OPCODE_FBH:
   case BRW_OPCODE_FBL:
   case BRW_OPCODE_ROL:
   case BRW_OPCODE_ROR:
   case BRW_OPCODE_SUBB:
   case BRW_OPCODE_DP4A:
   case BRW_OPCODE_DPAS:
   case SHADER_OPCODE_BROADCAST:
   case SHADER_OPCODE_CLUSTER_BROADCAST:
   case SHADER_OPCODE_MOV_INDIRECT:
   case SHADER_OPCODE_SHUFFLE:
   case SHADER_OPCODE_INT_QUOTIENT:
   case SHADER_OPCODE_INT_REMAINDER:
   case SHADER_OPCODE_REDUCE:
   case SHADER_OPCODE_INCLUSIVE_SCAN:
   case SHADER_OPCODE_EXCLUSIVE_SCAN:
   case SHADER_OPCODE_VOTE_ANY:
   case SHADER_OPCODE_VOTE_ALL:
   case SHADER_OPCODE_VOTE_EQUAL:
   case SHADER_OPCODE_BALLOT:
   case SHADER_OPCODE_QUAD_SWAP:
   case SHADER_OPCODE_READ_FROM_LIVE_CHANNEL:
   case SHADER_OPCODE_READ_FROM_CHANNEL:
      return false;
   default:
      return true;
   }
}

bool
brw_inst::can_do_cmod() const
{
   switch (opcode) {
   case BRW_OPCODE_ADD:
   case BRW_OPCODE_ADD3:
   case BRW_OPCODE_ADDC:
   case BRW_OPCODE_AND:
   case BRW_OPCODE_ASR:
   case BRW_OPCODE_AVG:
   case BRW_OPCODE_CMP:
   case BRW_OPCODE_CMPN:
   case BRW_OPCODE_DP2:
   case BRW_OPCODE_DP3:
   case BRW_OPCODE_DP4:
   case BRW_OPCODE_DPH:
   case BRW_OPCODE_FRC:
   case BRW_OPCODE_LINE:
   case BRW_OPCODE_LRP:
   case BRW_OPCODE_LZD:
   case BRW_OPCODE_MAC:
   case BRW_OPCODE_MACH:
   case BRW_OPCODE_MAD:
   case BRW_OPCODE_MOV:
   case BRW_OPCODE_MUL:
   case BRW_OPCODE_NOT:
   case BRW_OPCODE_OR:
   case BRW_OPCODE_PLN:
   case BRW_OPCODE_RNDD:
   case BRW_OPCODE_RNDE:
   case BRW_OPCODE_RNDU:
   case BRW_OPCODE_RNDZ:
   case BRW_OPCODE_SHL:
   case BRW_OPCODE_SHR:
   case BRW_OPCODE_SUBB:
   case BRW_OPCODE_XOR:
      break;
   default:
      return false;
   }

   /* The accumulator result appears to get used for the conditional modifier
    * generation.  When negating a UD value, there is a 33rd bit generated for
    * the sign in the accumulator value, so now you can't check, for example,
    * equality with a 32-bit value.  See piglit fs-op-neg-uvec4.
    */
   for (unsigned i = 0; i < sources; i++) {
      if (brw_type_is_uint(src[i].type) && src[i].negate)
         return false;
   }

   if (dst.file == ARF && dst.nr == BRW_ARF_SCALAR && src[0].file == IMM)
      return false;

   return true;
}

bool
brw_inst::can_change_types() const
{
   return dst.type == src[0].type &&
          !src[0].abs && !src[0].negate && !saturate && src[0].file != ATTR &&
          (opcode == BRW_OPCODE_MOV ||
           (opcode == SHADER_OPCODE_LOAD_PAYLOAD && sources == 1) ||
           (opcode == BRW_OPCODE_SEL &&
            dst.type == src[1].type &&
            predicate != BRW_PREDICATE_NONE &&
            !src[1].abs && !src[1].negate && src[1].file != ATTR));
}

/**
 * Returns true if the instruction has a flag that means it won't
 * update an entire destination register.
 *
 * For example, dead code elimination and live variable analysis want to know
 * when a write to a variable screens off any preceding values that were in
 * it.
 */
bool
brw_inst::is_partial_write() const
{
   if (this->predicate && !this->predicate_trivial &&
       this->opcode != BRW_OPCODE_SEL)
      return true;

   if (!this->dst.is_contiguous())
      return true;

   if (this->dst.offset % REG_SIZE != 0)
      return true;

   return this->size_written % REG_SIZE != 0;
}

unsigned
brw_inst::components_read(unsigned i) const
{
   /* Return zero if the source is not present. */
   if (src[i].file == BAD_FILE)
      return 0;

   switch (opcode) {
   case BRW_OPCODE_PLN:
      return i == 0 ? 1 : 2;

   case FS_OPCODE_PIXEL_X:
   case FS_OPCODE_PIXEL_Y:
      assert(i < 2);
      if (i == 0)
         return 2;
      else
         return 1;

   case FS_OPCODE_FB_WRITE_LOGICAL:
      assert(src[FB_WRITE_LOGICAL_SRC_COMPONENTS].file == IMM);
      /* First/second FB write color. */
      if (i < 2)
         return src[FB_WRITE_LOGICAL_SRC_COMPONENTS].ud;
      else
         return 1;

   case SHADER_OPCODE_TEX_LOGICAL:
   case SHADER_OPCODE_TXD_LOGICAL:
   case SHADER_OPCODE_TXF_LOGICAL:
   case SHADER_OPCODE_TXL_LOGICAL:
   case SHADER_OPCODE_TXS_LOGICAL:
   case SHADER_OPCODE_IMAGE_SIZE_LOGICAL:
   case FS_OPCODE_TXB_LOGICAL:
   case SHADER_OPCODE_TXF_CMS_W_LOGICAL:
   case SHADER_OPCODE_TXF_CMS_W_GFX12_LOGICAL:
   case SHADER_OPCODE_TXF_MCS_LOGICAL:
   case SHADER_OPCODE_LOD_LOGICAL:
   case SHADER_OPCODE_TG4_LOGICAL:
   case SHADER_OPCODE_TG4_OFFSET_LOGICAL:
   case SHADER_OPCODE_TG4_BIAS_LOGICAL:
   case SHADER_OPCODE_TG4_EXPLICIT_LOD_LOGICAL:
   case SHADER_OPCODE_TG4_IMPLICIT_LOD_LOGICAL:
   case SHADER_OPCODE_TG4_OFFSET_LOD_LOGICAL:
   case SHADER_OPCODE_TG4_OFFSET_BIAS_LOGICAL:
   case SHADER_OPCODE_SAMPLEINFO_LOGICAL:
      assert(src[TEX_LOGICAL_SRC_COORD_COMPONENTS].file == IMM &&
             src[TEX_LOGICAL_SRC_GRAD_COMPONENTS].file == IMM &&
             src[TEX_LOGICAL_SRC_RESIDENCY].file == IMM);
      /* Texture coordinates. */
      if (i == TEX_LOGICAL_SRC_COORDINATE)
         return src[TEX_LOGICAL_SRC_COORD_COMPONENTS].ud;
      /* Texture derivatives. */
      else if ((i == TEX_LOGICAL_SRC_LOD || i == TEX_LOGICAL_SRC_LOD2) &&
               opcode == SHADER_OPCODE_TXD_LOGICAL)
         return src[TEX_LOGICAL_SRC_GRAD_COMPONENTS].ud;
      /* Texture offset. */
      else if (i == TEX_LOGICAL_SRC_TG4_OFFSET)
         return 2;
      /* MCS */
      else if (i == TEX_LOGICAL_SRC_MCS) {
         if (opcode == SHADER_OPCODE_TXF_CMS_W_LOGICAL)
            return 2;
         else if (opcode == SHADER_OPCODE_TXF_CMS_W_GFX12_LOGICAL)
            return 4;
         else
            return 1;
      } else
         return 1;

   case SHADER_OPCODE_MEMORY_LOAD_LOGICAL:
      if (i == MEMORY_LOGICAL_DATA0)
         return 0;
      /* fallthrough */
   case SHADER_OPCODE_MEMORY_STORE_LOGICAL:
      if (i == MEMORY_LOGICAL_DATA1)
         return 0;
      /* fallthrough */
   case SHADER_OPCODE_MEMORY_ATOMIC_LOGICAL:
      if (i == MEMORY_LOGICAL_DATA0 || i == MEMORY_LOGICAL_DATA1)
         return src[MEMORY_LOGICAL_COMPONENTS].ud;
      else if (i == MEMORY_LOGICAL_ADDRESS)
         return src[MEMORY_LOGICAL_COORD_COMPONENTS].ud;
      else
         return 1;

   case FS_OPCODE_INTERPOLATE_AT_PER_SLOT_OFFSET:
      return (i == 0 ? 2 : 1);

   case SHADER_OPCODE_URB_WRITE_LOGICAL:
      assert(src[URB_LOGICAL_SRC_COMPONENTS].file == IMM);

      if (i == URB_LOGICAL_SRC_DATA)
         return src[URB_LOGICAL_SRC_COMPONENTS].ud;
      else
         return 1;

   case BRW_OPCODE_DPAS:
      unreachable("Do not use components_read() for DPAS.");

   default:
      return 1;
   }
}

unsigned
brw_inst::size_read(const struct intel_device_info *devinfo, int arg) const
{
   switch (opcode) {
   case SHADER_OPCODE_SEND:
      if (arg == 2) {
         return mlen * REG_SIZE;
      } else if (arg == 3) {
         return ex_mlen * REG_SIZE;
      }
      break;

   case SHADER_OPCODE_SEND_GATHER:
      if (arg >= 3) {
         /* SEND_GATHER is Xe3+, so no need to pass devinfo around. */
         const unsigned reg_unit = 2;
         return REG_SIZE * reg_unit;
      }
      break;

   case BRW_OPCODE_PLN:
      if (arg == 0)
         return 16;
      break;

   case SHADER_OPCODE_LOAD_PAYLOAD:
      if (arg < this->header_size)
         return retype(src[arg], BRW_TYPE_UD).component_size(8);
      break;

   case SHADER_OPCODE_BARRIER:
      return REG_SIZE;

   case SHADER_OPCODE_MOV_INDIRECT:
      if (arg == 0) {
         assert(src[2].file == IMM);
         return src[2].ud;
      }
      break;

   case BRW_OPCODE_DPAS: {
      /* This is a little bit sketchy. There's no way to get at devinfo from
       * here, so the regular reg_unit() cannot be used. However, on
       * reg_unit() == 1 platforms, DPAS exec_size must be 8, and on known
       * reg_unit() == 2 platforms, DPAS exec_size must be 16. This is not a
       * coincidence, so this isn't so bad.
       */
      const unsigned reg_unit = this->exec_size / 8;

      switch (arg) {
      case 0:
         if (src[0].type == BRW_TYPE_HF) {
            return rcount * reg_unit * REG_SIZE / 2;
         } else {
            return rcount * reg_unit * REG_SIZE;
         }
      case 1:
         return sdepth * reg_unit * REG_SIZE;
      case 2:
         /* This is simpler than the formula described in the Bspec, but it
          * covers all of the cases that we support. Each inner sdepth
          * iteration of the DPAS consumes a single dword for int8, uint8, or
          * float16 types. These are the one source types currently
          * supportable through Vulkan. This is independent of reg_unit.
          */
         return rcount * sdepth * 4;
      default:
         unreachable("Invalid source number.");
      }
      break;
   }

   default:
      break;
   }

   switch (src[arg].file) {
   case UNIFORM:
   case IMM:
      return components_read(arg) * brw_type_size_bytes(src[arg].type);
   case BAD_FILE:
   case ADDRESS:
   case ARF:
   case FIXED_GRF:
   case VGRF:
   case ATTR:
      /* Regardless of exec_size, values marked as scalar are SIMD8. */
      return components_read(arg) *
             src[arg].component_size(src[arg].is_scalar ? 8 * reg_unit(devinfo) : exec_size);
   }
   return 0;
}

namespace {
   unsigned
   predicate_width(const intel_device_info *devinfo, brw_predicate predicate)
   {
      if (devinfo->ver >= 20) {
         return 1;
      } else {
         switch (predicate) {
         case BRW_PREDICATE_NONE:            return 1;
         case BRW_PREDICATE_NORMAL:          return 1;
         case BRW_PREDICATE_ALIGN1_ANY2H:    return 2;
         case BRW_PREDICATE_ALIGN1_ALL2H:    return 2;
         case BRW_PREDICATE_ALIGN1_ANY4H:    return 4;
         case BRW_PREDICATE_ALIGN1_ALL4H:    return 4;
         case BRW_PREDICATE_ALIGN1_ANY8H:    return 8;
         case BRW_PREDICATE_ALIGN1_ALL8H:    return 8;
         case BRW_PREDICATE_ALIGN1_ANY16H:   return 16;
         case BRW_PREDICATE_ALIGN1_ALL16H:   return 16;
         case BRW_PREDICATE_ALIGN1_ANY32H:   return 32;
         case BRW_PREDICATE_ALIGN1_ALL32H:   return 32;
         default: unreachable("Unsupported predicate");
         }
      }
   }
}

unsigned
brw_inst::flags_read(const intel_device_info *devinfo) const
{
   if (devinfo->ver < 20 && (predicate == BRW_PREDICATE_ALIGN1_ANYV ||
                             predicate == BRW_PREDICATE_ALIGN1_ALLV)) {
      /* The vertical predication modes combine corresponding bits from
       * f0.0 and f1.0 on Gfx7+.
       */
      const unsigned shift = 4;
      return brw_flag_mask(this, 1) << shift | brw_flag_mask(this, 1);
   } else if (predicate) {
      return brw_flag_mask(this, predicate_width(devinfo, predicate));
   } else {
      unsigned mask = 0;
      for (int i = 0; i < sources; i++) {
         mask |= brw_flag_mask(src[i], size_read(devinfo, i));
      }
      return mask;
   }
}

unsigned
brw_inst::flags_written(const intel_device_info *devinfo) const
{
   if (conditional_mod && (opcode != BRW_OPCODE_SEL &&
                           opcode != BRW_OPCODE_CSEL &&
                           opcode != BRW_OPCODE_IF &&
                           opcode != BRW_OPCODE_WHILE)) {
      return brw_flag_mask(this, 1);
   } else if (opcode == FS_OPCODE_LOAD_LIVE_CHANNELS ||
              opcode == SHADER_OPCODE_BALLOT ||
              opcode == SHADER_OPCODE_VOTE_ANY ||
              opcode == SHADER_OPCODE_VOTE_ALL ||
              opcode == SHADER_OPCODE_VOTE_EQUAL) {
      return brw_flag_mask(this, 32);
   } else {
      return brw_flag_mask(dst, size_written);
   }
}

bool
brw_inst::has_sampler_residency() const
{
   switch (opcode) {
   case SHADER_OPCODE_TEX_LOGICAL:
   case FS_OPCODE_TXB_LOGICAL:
   case SHADER_OPCODE_TXL_LOGICAL:
   case SHADER_OPCODE_TXD_LOGICAL:
   case SHADER_OPCODE_TXF_LOGICAL:
   case SHADER_OPCODE_TXF_CMS_W_GFX12_LOGICAL:
   case SHADER_OPCODE_TXF_CMS_W_LOGICAL:
   case SHADER_OPCODE_TXS_LOGICAL:
   case SHADER_OPCODE_TG4_OFFSET_LOGICAL:
   case SHADER_OPCODE_TG4_LOGICAL:
   case SHADER_OPCODE_TG4_BIAS_LOGICAL:
   case SHADER_OPCODE_TG4_EXPLICIT_LOD_LOGICAL:
   case SHADER_OPCODE_TG4_IMPLICIT_LOD_LOGICAL:
   case SHADER_OPCODE_TG4_OFFSET_LOD_LOGICAL:
   case SHADER_OPCODE_TG4_OFFSET_BIAS_LOGICAL:
      assert(src[TEX_LOGICAL_SRC_RESIDENCY].file == IMM);
      return src[TEX_LOGICAL_SRC_RESIDENCY].ud != 0;
   default:
      return false;
   }
}

/* \sa inst_is_raw_move in brw_eu_validate. */
bool
brw_inst::is_raw_move() const
{
   if (opcode != BRW_OPCODE_MOV)
      return false;

   if (src[0].file == IMM) {
      if (brw_type_is_vector_imm(src[0].type))
         return false;
   } else if (src[0].negate || src[0].abs) {
      return false;
   }

   if (saturate)
      return false;

   return src[0].type == dst.type ||
          (brw_type_is_int(src[0].type) &&
           brw_type_is_int(dst.type) &&
           brw_type_size_bits(src[0].type) == brw_type_size_bits(dst.type));
}

bool
brw_inst::uses_address_register_implicitly() const
{
   switch (opcode) {
   case SHADER_OPCODE_BROADCAST:
   case SHADER_OPCODE_SHUFFLE:
   case SHADER_OPCODE_MOV_INDIRECT:
      return true;
   default:
      return false;
   }
}

bool
brw_inst::is_commutative() const
{
   switch (opcode) {
   case BRW_OPCODE_AND:
   case BRW_OPCODE_OR:
   case BRW_OPCODE_XOR:
   case BRW_OPCODE_ADD:
   case BRW_OPCODE_ADD3:
   case SHADER_OPCODE_MULH:
      return true;

   case BRW_OPCODE_MUL:
      /* Integer multiplication of dword and word sources is not actually
       * commutative. The DW source must be first.
       */
      return !brw_type_is_int(src[0].type) ||
             brw_type_size_bits(src[0].type) == brw_type_size_bits(src[1].type);

   case BRW_OPCODE_SEL:
      /* MIN and MAX are commutative. */
      if (conditional_mod == BRW_CONDITIONAL_GE ||
          conditional_mod == BRW_CONDITIONAL_L) {
         return true;
      }
      FALLTHROUGH;
   default:
      return false;
   }
}

bool
brw_inst::is_3src(const struct brw_compiler *compiler) const
{
   return ::is_3src(&compiler->isa, opcode);
}

bool
brw_inst::is_math() const
{
   return (opcode == SHADER_OPCODE_RCP ||
           opcode == SHADER_OPCODE_RSQ ||
           opcode == SHADER_OPCODE_SQRT ||
           opcode == SHADER_OPCODE_EXP2 ||
           opcode == SHADER_OPCODE_LOG2 ||
           opcode == SHADER_OPCODE_SIN ||
           opcode == SHADER_OPCODE_COS ||
           opcode == SHADER_OPCODE_INT_QUOTIENT ||
           opcode == SHADER_OPCODE_INT_REMAINDER ||
           opcode == SHADER_OPCODE_POW);
}

bool
brw_inst::is_control_flow_begin() const
{
   switch (opcode) {
   case BRW_OPCODE_DO:
   case BRW_OPCODE_IF:
   case BRW_OPCODE_ELSE:
      return true;
   default:
      return false;
   }
}

bool
brw_inst::is_control_flow_end() const
{
   switch (opcode) {
   case BRW_OPCODE_ELSE:
   case BRW_OPCODE_WHILE:
   case BRW_OPCODE_ENDIF:
   case SHADER_OPCODE_FLOW:
      return true;
   default:
      return false;
   }
}

bool
brw_inst::is_control_flow() const
{
   switch (opcode) {
   case BRW_OPCODE_DO:
   case BRW_OPCODE_WHILE:
   case BRW_OPCODE_IF:
   case BRW_OPCODE_ELSE:
   case BRW_OPCODE_ENDIF:
   case BRW_OPCODE_BREAK:
   case BRW_OPCODE_CONTINUE:
   case BRW_OPCODE_JMPI:
   case BRW_OPCODE_BRD:
   case BRW_OPCODE_BRC:
   case BRW_OPCODE_HALT:
   case BRW_OPCODE_CALLA:
   case BRW_OPCODE_CALL:
   case BRW_OPCODE_GOTO:
   case BRW_OPCODE_RET:
   case SHADER_OPCODE_FLOW:
      return true;
   case BRW_OPCODE_MOV:
   case BRW_OPCODE_ADD:
      return dst.is_ip();
   default:
      return false;
   }
}

bool
brw_inst::uses_indirect_addressing() const
{
   switch (opcode) {
   case SHADER_OPCODE_BROADCAST:
   case SHADER_OPCODE_CLUSTER_BROADCAST:
   case SHADER_OPCODE_MOV_INDIRECT:
      return true;
   default:
      return false;
   }
}

bool
brw_inst::can_do_saturate() const
{
   switch (opcode) {
   case BRW_OPCODE_ADD:
   case BRW_OPCODE_ADD3:
   case BRW_OPCODE_ASR:
   case BRW_OPCODE_AVG:
   case BRW_OPCODE_CSEL:
   case BRW_OPCODE_DP2:
   case BRW_OPCODE_DP3:
   case BRW_OPCODE_DP4:
   case BRW_OPCODE_DPH:
   case BRW_OPCODE_DP4A:
   case BRW_OPCODE_LINE:
   case BRW_OPCODE_LRP:
   case BRW_OPCODE_MAC:
   case BRW_OPCODE_MAD:
   case BRW_OPCODE_MATH:
   case BRW_OPCODE_MOV:
   case BRW_OPCODE_MUL:
   case SHADER_OPCODE_MULH:
   case BRW_OPCODE_PLN:
   case BRW_OPCODE_RNDD:
   case BRW_OPCODE_RNDE:
   case BRW_OPCODE_RNDU:
   case BRW_OPCODE_RNDZ:
   case BRW_OPCODE_SEL:
   case BRW_OPCODE_SHL:
   case BRW_OPCODE_SHR:
   case SHADER_OPCODE_COS:
   case SHADER_OPCODE_EXP2:
   case SHADER_OPCODE_LOG2:
   case SHADER_OPCODE_POW:
   case SHADER_OPCODE_RCP:
   case SHADER_OPCODE_RSQ:
   case SHADER_OPCODE_SIN:
   case SHADER_OPCODE_SQRT:
      return true;
   default:
      return false;
   }
}

bool
brw_inst::reads_accumulator_implicitly() const
{
   switch (opcode) {
   case BRW_OPCODE_MAC:
   case BRW_OPCODE_MACH:
      return true;
   default:
      return false;
   }
}

bool
brw_inst::writes_accumulator_implicitly(const struct intel_device_info *devinfo) const
{
   return writes_accumulator ||
          (eot && intel_needs_workaround(devinfo, 14010017096));
}

bool
brw_inst::has_side_effects() const
{
   switch (opcode) {
   case SHADER_OPCODE_SEND:
   case SHADER_OPCODE_SEND_GATHER:
      return send_has_side_effects;

   case BRW_OPCODE_SYNC:
   case SHADER_OPCODE_MEMORY_STORE_LOGICAL:
   case SHADER_OPCODE_MEMORY_ATOMIC_LOGICAL:
   case SHADER_OPCODE_MEMORY_FENCE:
   case SHADER_OPCODE_INTERLOCK:
   case SHADER_OPCODE_URB_WRITE_LOGICAL:
   case FS_OPCODE_FB_WRITE_LOGICAL:
   case SHADER_OPCODE_BARRIER:
   case SHADER_OPCODE_RND_MODE:
   case SHADER_OPCODE_FLOAT_CONTROL_MODE:
   case FS_OPCODE_SCHEDULING_FENCE:
   case SHADER_OPCODE_BTD_SPAWN_LOGICAL:
   case SHADER_OPCODE_BTD_RETIRE_LOGICAL:
   case RT_OPCODE_TRACE_RAY_LOGICAL:
      return true;
   default:
      return eot;
   }
}

bool
brw_inst::is_volatile() const
{
   return opcode == SHADER_OPCODE_MEMORY_LOAD_LOGICAL ||
          ((opcode == SHADER_OPCODE_SEND ||
            opcode == SHADER_OPCODE_SEND_GATHER) && send_is_volatile);
}

#ifndef NDEBUG
static bool
inst_is_in_block(const bblock_t *block, const brw_inst *inst)
{
   const exec_node *n = inst;

   /* Find the tail sentinel. If the tail sentinel is the sentinel from the
    * list header in the bblock_t, then this instruction is in that basic
    * block.
    */
   while (!n->is_tail_sentinel())
      n = n->get_next();

   return n == &block->instructions.tail_sentinel;
}
#endif

static void
adjust_later_block_ips(bblock_t *start_block, int ip_adjustment)
{
   for (bblock_t *block_iter = start_block->next();
        block_iter;
        block_iter = block_iter->next()) {
      block_iter->start_ip += ip_adjustment;
      block_iter->end_ip += ip_adjustment;
   }
}

void
brw_inst::insert_after(bblock_t *block, brw_inst *inst)
{
   assert(this != inst);
   assert(block->end_ip_delta == 0);

   if (!this->is_head_sentinel())
      assert(inst_is_in_block(block, this) || !"Instruction not in block");

   block->end_ip++;

   adjust_later_block_ips(block, 1);

   exec_node::insert_after(inst);
}

void
brw_inst::insert_before(bblock_t *block, brw_inst *inst)
{
   assert(this != inst);
   assert(block->end_ip_delta == 0);

   if (!this->is_tail_sentinel())
      assert(inst_is_in_block(block, this) || !"Instruction not in block");

   block->end_ip++;

   adjust_later_block_ips(block, 1);

   exec_node::insert_before(inst);
}

void
brw_inst::remove(bblock_t *block, bool defer_later_block_ip_updates)
{
   assert(inst_is_in_block(block, this) || !"Instruction not in block");

   if (exec_list_is_singular(&block->instructions)) {
      this->opcode = BRW_OPCODE_NOP;
      this->resize_sources(0);
      this->dst = brw_reg();
      this->size_written = 0;
      return;
   }

   if (defer_later_block_ip_updates) {
      block->end_ip_delta--;
   } else {
      assert(block->end_ip_delta == 0);
      adjust_later_block_ips(block, -1);
   }

   if (block->start_ip == block->end_ip) {
      if (block->end_ip_delta != 0) {
         adjust_later_block_ips(block, block->end_ip_delta);
         block->end_ip_delta = 0;
      }

      block->cfg->remove_block(block);
   } else {
      block->end_ip--;
   }

   exec_node::remove();
}

enum brw_reg_type
get_exec_type(const brw_inst *inst)
{
   brw_reg_type exec_type = BRW_TYPE_B;

   for (int i = 0; i < inst->sources; i++) {
      if (inst->src[i].file != BAD_FILE &&
          !inst->is_control_source(i)) {
         const brw_reg_type t = get_exec_type(inst->src[i].type);
         if (brw_type_size_bytes(t) > brw_type_size_bytes(exec_type))
            exec_type = t;
         else if (brw_type_size_bytes(t) == brw_type_size_bytes(exec_type) &&
                  brw_type_is_float(t))
            exec_type = t;
      }
   }

   if (exec_type == BRW_TYPE_B)
      exec_type = inst->dst.type;

   assert(exec_type != BRW_TYPE_B);

   /* Promotion of the execution type to 32-bit for conversions from or to
    * half-float seems to be consistent with the following text from the
    * Cherryview PRM Vol. 7, "Execution Data Type":
    *
    * "When single precision and half precision floats are mixed between
    *  source operands or between source and destination operand [..] single
    *  precision float is the execution datatype."
    *
    * and from "Register Region Restrictions":
    *
    * "Conversion between Integer and HF (Half Float) must be DWord aligned
    *  and strided by a DWord on the destination."
    */
   if (brw_type_size_bytes(exec_type) == 2 &&
       inst->dst.type != exec_type) {
      if (exec_type == BRW_TYPE_HF)
         exec_type = BRW_TYPE_F;
      else if (inst->dst.type == BRW_TYPE_HF)
         exec_type = BRW_TYPE_D;
   }

   return exec_type;
}

/**
 * Return whether the following regioning restriction applies to the specified
 * instruction.  From the Cherryview PRM Vol 7. "Register Region
 * Restrictions":
 *
 * "When source or destination datatype is 64b or operation is integer DWord
 *  multiply, regioning in Align1 must follow these rules:
 *
 *  1. Source and Destination horizontal stride must be aligned to the same qword.
 *  2. Regioning must ensure Src.Vstride = Src.Width * Src.Hstride.
 *  3. Source and Destination offset must be the same, except the case of
 *     scalar source."
 */
bool
has_dst_aligned_region_restriction(const intel_device_info *devinfo,
                                   const brw_inst *inst,
                                   brw_reg_type dst_type)
{
   const brw_reg_type exec_type = get_exec_type(inst);
   /* Even though the hardware spec claims that "integer DWord multiply"
    * operations are restricted, empirical evidence and the behavior of the
    * simulator suggest that only 32x32-bit integer multiplication is
    * restricted.
    */
   const bool is_dword_multiply = !brw_type_is_float(exec_type) &&
      ((inst->opcode == BRW_OPCODE_MUL &&
        MIN2(brw_type_size_bytes(inst->src[0].type), brw_type_size_bytes(inst->src[1].type)) >= 4) ||
       (inst->opcode == BRW_OPCODE_MAD &&
        MIN2(brw_type_size_bytes(inst->src[1].type), brw_type_size_bytes(inst->src[2].type)) >= 4));

   if (brw_type_size_bytes(dst_type) > 4 || brw_type_size_bytes(exec_type) > 4 ||
       (brw_type_size_bytes(exec_type) == 4 && is_dword_multiply))
      return intel_device_info_is_9lp(devinfo) || devinfo->verx10 >= 125;

   else if (brw_type_is_float(dst_type))
      return devinfo->verx10 >= 125;

   else
      return false;
}

/**
 * Return true if the instruction can be potentially affected by the Xe2+
 * regioning restrictions that apply to integer types smaller than a dword.
 * The restriction isn't quoted here due to its length, see BSpec #56640 for
 * details.
 */
bool
has_subdword_integer_region_restriction(const intel_device_info *devinfo,
                                        const brw_inst *inst,
                                        const brw_reg *srcs, unsigned num_srcs)
{
   if (devinfo->ver >= 20 &&
       brw_type_is_int(inst->dst.type) &&
       MAX2(byte_stride(inst->dst),
            brw_type_size_bytes(inst->dst.type)) < 4) {
      for (unsigned i = 0; i < num_srcs; i++) {
         if (brw_type_is_int(srcs[i].type) &&
             ((brw_type_size_bytes(srcs[i].type) < 4 &&
               byte_stride(srcs[i]) >= 4) ||
              (MAX2(byte_stride(inst->dst),
                   brw_type_size_bytes(inst->dst.type)) == 1 &&
               brw_type_size_bytes(srcs[i].type) == 1 &&
               byte_stride(srcs[i]) >= 2)))
            return true;
      }
   }

   return false;
}

/**
 * Return whether the LOAD_PAYLOAD instruction is a plain copy of bits from
 * the specified register file into a VGRF.
 *
 * This implies identity register regions without any source-destination
 * overlap, but otherwise has no implications on the location of sources and
 * destination in the register file: Gathering any number of portions from
 * multiple virtual registers in any order is allowed.
 */
static bool
is_copy_payload(const struct intel_device_info *devinfo,
                brw_reg_file file, const brw_inst *inst)
{
   if (inst->opcode != SHADER_OPCODE_LOAD_PAYLOAD ||
       inst->is_partial_write() || inst->saturate ||
       inst->dst.file != VGRF)
      return false;

   for (unsigned i = 0; i < inst->sources; i++) {
      if (inst->src[i].file != file ||
          inst->src[i].abs || inst->src[i].negate)
         return false;

      if (!inst->src[i].is_contiguous())
         return false;

      if (regions_overlap(inst->dst, inst->size_written,
                          inst->src[i], inst->size_read(devinfo, i)))
         return false;
   }

   return true;
}

/**
 * Like is_copy_payload(), but the instruction is required to copy a single
 * contiguous block of registers from the given register file into the
 * destination without any reordering.
 */
bool
is_identity_payload(const struct intel_device_info *devinfo,
                    brw_reg_file file, const brw_inst *inst)
{
   if (is_copy_payload(devinfo, file, inst)) {
      brw_reg reg = inst->src[0];

      for (unsigned i = 0; i < inst->sources; i++) {
         reg.type = inst->src[i].type;
         if (!inst->src[i].equals(reg))
            return false;

         reg = byte_offset(reg, inst->size_read(devinfo, i));
      }

      return true;
   } else {
      return false;
   }
}

/**
 * Like is_copy_payload(), but the instruction is required to source data from
 * at least two disjoint VGRFs.
 *
 * This doesn't necessarily rule out the elimination of this instruction
 * through register coalescing, but due to limitations of the register
 * coalesce pass it might be impossible to do so directly until a later stage,
 * when the LOAD_PAYLOAD instruction is unrolled into a sequence of MOV
 * instructions.
 */
bool
is_multi_copy_payload(const struct intel_device_info *devinfo,
                      const brw_inst *inst)
{
   if (is_copy_payload(devinfo, VGRF, inst)) {
      for (unsigned i = 0; i < inst->sources; i++) {
            if (inst->src[i].nr != inst->src[0].nr)
               return true;
      }
   }

   return false;
}

/**
 * Like is_identity_payload(), but the instruction is required to copy the
 * whole contents of a single VGRF into the destination.
 *
 * This means that there is a good chance that the instruction will be
 * eliminated through register coalescing, but it's neither a necessary nor a
 * sufficient condition for that to happen -- E.g. consider the case where
 * source and destination registers diverge due to other instructions in the
 * program overwriting part of their contents, which isn't something we can
 * predict up front based on a cheap strictly local test of the copy
 * instruction.
 */
bool
is_coalescing_payload(const brw_shader &s, const brw_inst *inst)
{
   return is_identity_payload(s.devinfo, VGRF, inst) &&
          inst->src[0].offset == 0 &&
          s.alloc.sizes[inst->src[0].nr] * REG_SIZE == inst->size_written;
}

