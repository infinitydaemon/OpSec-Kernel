/*
 * Copyright (c) 2024 Intel Corporation
 * SPDX-License-Identifier: MIT
 */

#include <gtest/gtest.h>
#include "brw_shader.h"
#include "brw_builder.h"
#include "brw_cfg.h"

struct FSCombineConstantsTest : public ::testing::Test {
   FSCombineConstantsTest() {
      mem_ctx = ralloc_context(NULL);

      devinfo = {};
      devinfo.ver = 9;
      devinfo.verx10 = 90;

      compiler = {};
      compiler.devinfo = &devinfo;
      brw_init_isa_info(&compiler.isa, &devinfo);

      params = {};
      params.mem_ctx = mem_ctx;

      prog_data = {};
      nir_shader *nir =
         nir_shader_create(mem_ctx, MESA_SHADER_COMPUTE, NULL, NULL);

      shader = new brw_shader(&compiler, &params, NULL,
                              &prog_data.base, nir, 8, false, false);
   }

   ~FSCombineConstantsTest() override {
      delete shader;
      ralloc_free(mem_ctx);
      mem_ctx = NULL;
   }

   void *mem_ctx;
   brw_compiler compiler;
   brw_compile_params params;
   intel_device_info devinfo;
   struct brw_wm_prog_data prog_data;
   struct gl_shader_program *shader_prog;

   brw_shader *shader;

   bool opt_combine_constants(brw_shader *s) {
      const bool print = getenv("TEST_DEBUG");

      if (print) {
         fprintf(stderr, "= Before =\n");
         s->cfg->dump();
      }

      bool ret = brw_opt_combine_constants(*s);

      if (print) {
         fprintf(stderr, "\n= After =\n");
         s->cfg->dump();
      }

      return ret;
   }
};

static brw_builder
make_builder(brw_shader *s)
{
   return brw_builder(s, s->dispatch_width).at_end();
}

TEST_F(FSCombineConstantsTest, Simple)
{
   brw_builder bld = make_builder(shader);

   brw_reg r = brw_vec8_grf(1, 0);
   brw_reg imm_a = brw_imm_ud(1);
   brw_reg imm_b = brw_imm_ud(2);

   bld.SEL(r, imm_a, imm_b);
   brw_calculate_cfg(*shader);

   bool progress = opt_combine_constants(shader);
   ASSERT_TRUE(progress);

   ASSERT_EQ(shader->cfg->num_blocks, 1);
   bblock_t *block = cfg_first_block(shader->cfg);
   ASSERT_NE(block, nullptr);

   /* We can do better but for now sanity check that
    * there's a MOV and a SEL.
    */
   ASSERT_EQ(bblock_start(block)->opcode, BRW_OPCODE_MOV);
   ASSERT_EQ(bblock_end(block)->opcode, BRW_OPCODE_SEL);
}

TEST_F(FSCombineConstantsTest, DoContainingDo)
{
   brw_builder bld = make_builder(shader);

   brw_reg r1 = brw_vec8_grf(1, 0);
   brw_reg r2 = brw_vec8_grf(2, 0);
   brw_reg imm_a = brw_imm_ud(1);
   brw_reg imm_b = brw_imm_ud(2);

   bld.DO();
   bld.DO();
   bld.SEL(r1, imm_a, imm_b);
   bld.WHILE();
   bld.WHILE();
   bld.SEL(r2, imm_a, imm_b);
   brw_calculate_cfg(*shader);

   unsigned original_num_blocks = shader->cfg->num_blocks;

   bool progress = opt_combine_constants(shader);
   ASSERT_TRUE(progress);

   /* We can do better but for now sanity check there's
    * enough blocks, since the original issue motivating this
    * test is that the shader would be empty.
    */
   ASSERT_GE(shader->cfg->num_blocks, original_num_blocks);
   brw_validate(*shader);
}

