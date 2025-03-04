/*
 * Copyright © 2018 Intel Corporation
 * SPDX-License-Identifier: MIT
 */

#include "brw_asm.h"
#include "brw_asm_internal.h"
#include "brw_disasm_info.h"

/* TODO: Check if we can use bison/flex without globals. */

extern FILE *yyin;
struct list_head instr_labels;
struct list_head target_labels;

struct brw_codegen *p;
const char *input_filename;
int errors;
bool compaction_warning_given;

static bool
i965_postprocess_labels()
{
   void *store = p->store;

   struct target_label *tlabel;
   struct instr_label *ilabel, *s;

   const unsigned to_bytes_scale = brw_jump_scale(p->devinfo);

   LIST_FOR_EACH_ENTRY(tlabel, &target_labels, link) {
      LIST_FOR_EACH_ENTRY_SAFE(ilabel, s, &instr_labels, link) {
         if (!strcmp(tlabel->name, ilabel->name)) {
            brw_eu_inst *inst = store + ilabel->offset;

            int relative_offset = (tlabel->offset - ilabel->offset) / sizeof(brw_eu_inst);
            relative_offset *= to_bytes_scale;

            unsigned opcode = brw_eu_inst_opcode(p->isa, inst);

            if (ilabel->type == INSTR_LABEL_JIP) {
               switch (opcode) {
               case BRW_OPCODE_IF:
               case BRW_OPCODE_ELSE:
               case BRW_OPCODE_ENDIF:
               case BRW_OPCODE_WHILE:
                  brw_eu_inst_set_jip(p->devinfo, inst, relative_offset);
                  break;
               case BRW_OPCODE_BREAK:
               case BRW_OPCODE_HALT:
               case BRW_OPCODE_CONTINUE:
                  brw_eu_inst_set_jip(p->devinfo, inst, relative_offset);
                  break;
               default:
                  fprintf(stderr, "Unknown opcode %d with JIP label\n", opcode);
                  return false;
               }
            } else {
               switch (opcode) {
               case BRW_OPCODE_IF:
               case BRW_OPCODE_ELSE:
                  brw_eu_inst_set_uip(p->devinfo, inst, relative_offset);
                  break;
               case BRW_OPCODE_WHILE:
               case BRW_OPCODE_ENDIF:
                  fprintf(stderr, "WHILE/ENDIF cannot have UIP offset\n");
                  return false;
               case BRW_OPCODE_BREAK:
               case BRW_OPCODE_CONTINUE:
               case BRW_OPCODE_HALT:
                  brw_eu_inst_set_uip(p->devinfo, inst, relative_offset);
                  break;
               default:
                  fprintf(stderr, "Unknown opcode %d with UIP label\n", opcode);
                  return false;
               }
            }

            list_del(&ilabel->link);
         }
      }
   }

   LIST_FOR_EACH_ENTRY(ilabel, &instr_labels, link) {
      fprintf(stderr, "Unknown label '%s'\n", ilabel->name);
   }

   return list_is_empty(&instr_labels);
}

/* TODO: Would be nice to make this operate on string instead on a FILE. */

brw_assemble_result
brw_assemble(void *mem_ctx, const struct intel_device_info *devinfo,
             FILE *f, const char *filename, brw_assemble_flags flags)
{
   brw_assemble_result result = {0};

   list_inithead(&instr_labels);
   list_inithead(&target_labels);

   struct brw_isa_info isa;
   brw_init_isa_info(&isa, devinfo);

   p = rzalloc(mem_ctx, struct brw_codegen);
   brw_init_codegen(&isa, p, p);

   yyin = f;
   input_filename = filename;

   compaction_warning_given = false;
   int err = yyparse();
   if (err || errors)
      goto end;

   if (!i965_postprocess_labels())
      goto end;

   struct disasm_info *disasm_info = disasm_initialize(p->isa, NULL);
   if (!disasm_info) {
      ralloc_free(disasm_info);
      fprintf(stderr, "Unable to initialize disasm_info struct instance\n");
      goto end;
   }

   /* Add "inst groups" so validation errors can be recorded. */
   for (int i = 0; i <= p->next_insn_offset; i += 16)
      disasm_new_inst_group(disasm_info, i);

   if (!brw_validate_instructions(p->isa, p->store, 0,
                                  p->next_insn_offset, disasm_info)) {
      dump_assembly(p->store, 0, p->next_insn_offset, disasm_info, NULL);
      ralloc_free(disasm_info);
      fprintf(stderr, "Invalid instructions.\n");
      goto end;
   }

   if ((flags & BRW_ASSEMBLE_COMPACT) != 0)
      brw_compact_instructions(p, 0, disasm_info);

   result.bin = p->store;
   result.bin_size = p->next_insn_offset;

   if ((flags & BRW_ASSEMBLE_DUMP) != 0)
      dump_assembly(p->store, 0, p->next_insn_offset, disasm_info, NULL);

   ralloc_free(disasm_info);

end:
   /* Reset internal state. */
   yyin = NULL;
   input_filename = NULL;
   p = NULL;
   list_inithead(&instr_labels);
   list_inithead(&target_labels);

   return result;
}

