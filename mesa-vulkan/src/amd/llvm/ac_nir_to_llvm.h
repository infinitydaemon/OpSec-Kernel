/*
 * Copyright © 2016 Bas Nieuwenhuizen
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AC_NIR_TO_LLVM_H
#define AC_NIR_TO_LLVM_H

#include "amd_family.h"
#include "compiler/shader_enums.h"
#include <llvm-c/Core.h>
#include "llvm-c/TargetMachine.h"

#include <stdbool.h>

struct nir_shader;
struct ac_llvm_context;
struct ac_shader_abi;
struct ac_shader_args;

static inline unsigned ac_llvm_reg_index_soa(unsigned index, unsigned chan)
{
   return (index * 4) + chan;
}

bool ac_nir_translate(struct ac_llvm_context *ac, struct ac_shader_abi *abi,
                      const struct ac_shader_args *args, struct nir_shader *nir);

#endif /* AC_NIR_TO_LLVM_H */
