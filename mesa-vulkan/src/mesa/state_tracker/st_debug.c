/**************************************************************************
 * 
 * Copyright 2007 VMware, Inc.
 * All Rights Reserved.
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 * 
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL VMWARE AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 * 
 **************************************************************************/


#include "main/context.h"
#include "main/debug_output.h"
#include "program/prog_print.h"

#include "pipe/p_state.h"
#include "pipe/p_shader_tokens.h"

#include "cso_cache/cso_cache.h"

#include "st_context.h"
#include "st_debug.h"
#include "st_program.h"



int ST_DEBUG = 0;

static const struct debug_named_value st_debug_flags[] = {
   { "mesa",     DEBUG_MESA, NULL },
   { "tgsi",     DEBUG_PRINT_IR, NULL },
   { "nir",      DEBUG_PRINT_IR, NULL },
   { "fallback", DEBUG_FALLBACK, NULL },
   { "buffer",   DEBUG_BUFFER, NULL },
   { "wf",       DEBUG_WIREFRAME, NULL },
   { "gremedy",  DEBUG_GREMEDY, "Enable GREMEDY debug extensions" },
   { "noreadpixcache", DEBUG_NOREADPIXCACHE, NULL },
   { "xfb",      DEBUG_PRINT_XFB, NULL },
   DEBUG_NAMED_VALUE_END
};

DEBUG_GET_ONCE_FLAGS_OPTION(st_debug, "ST_DEBUG", st_debug_flags, 0)


void
st_debug_init(void)
{
   ST_DEBUG = debug_get_option_st_debug();
}
