/*
 * Copyright 2022 Google LLC
 * SPDX-License-Identifier: MIT
 */

#ifndef CPU_TRACE_H
#define CPU_TRACE_H

#include "u_perfetto.h"
#include "u_gpuvis.h"

#include "util/detect_os.h"
#include "util/macros.h"

#if defined(HAVE_PERFETTO)

/* note that util_perfetto_is_tracing_enabled always returns false util
 * util_perfetto_init is called
 */
#define _MESA_TRACE_BEGIN(name)                                              \
   do {                                                                      \
      if (unlikely(util_perfetto_is_tracing_enabled()))                      \
         util_perfetto_trace_begin(name);                                    \
   } while (0)

#define _MESA_TRACE_FLOW_BEGIN(name, id)                                     \
   do {                                                                      \
      if (unlikely(util_perfetto_is_tracing_enabled()))                      \
         util_perfetto_trace_begin_flow(name, id);                           \
   } while (0)

#define _MESA_TRACE_END()                                                    \
   do {                                                                      \
      if (unlikely(util_perfetto_is_tracing_enabled()))                      \
         util_perfetto_trace_end();                                          \
   } while (0)

#define _MESA_TRACE_SET_COUNTER(name, value)                                 \
   do {                                                                      \
      if (unlikely(util_perfetto_is_tracing_enabled()))                      \
         util_perfetto_counter_set(name, value);                             \
   } while (0)

#define _MESA_TRACE_TIMESTAMP_BEGIN(name, track_id, flow_id, timestamp)      \
   do {                                                                      \
      if (unlikely(util_perfetto_is_tracing_enabled()))                      \
         util_perfetto_trace_full_begin(name, track_id, flow_id, timestamp); \
   } while (0)

#define _MESA_TRACE_TIMESTAMP_END(name, track_id, timestamp)                 \
   do {                                                                      \
      if (unlikely(util_perfetto_is_tracing_enabled()))                      \
         util_perfetto_trace_full_end(name, track_id, timestamp);            \
   } while (0)

/* NOTE: for now disable atrace for C++ to workaround a ndk bug with ordering
 * between stdatomic.h and atomic.h.  See:
 *
 *   https://github.com/android/ndk/issues/1178
 */
#elif DETECT_OS_ANDROID && !defined(__cplusplus)

#include <cutils/trace.h>

#define _MESA_TRACE_BEGIN(name)                                              \
   atrace_begin(ATRACE_TAG_GRAPHICS, name)
#define _MESA_TRACE_END() atrace_end(ATRACE_TAG_GRAPHICS)
#define _MESA_TRACE_FLOW_BEGIN(name, id)                                     \
   atrace_begin(ATRACE_TAG_GRAPHICS, name)
#define _MESA_TRACE_SET_COUNTER(name, value)
#define _MESA_TRACE_TIMESTAMP_BEGIN(name, track_id, flow_id, timestamp)
#define _MESA_TRACE_TIMESTAMP_END(name, track_id, timestamp)
#else

#define _MESA_TRACE_BEGIN(name)
#define _MESA_TRACE_END()
#define _MESA_TRACE_FLOW_BEGIN(name, id)
#define _MESA_TRACE_SET_COUNTER(name, value)
#define _MESA_TRACE_TIMESTAMP_BEGIN(name, track_id, flow_id, timestamp)
#define _MESA_TRACE_TIMESTAMP_END(name, track_id, timestamp)

#endif /* HAVE_PERFETTO */

#if defined(HAVE_GPUVIS)

#define _MESA_GPUVIS_TRACE_BEGIN(name) util_gpuvis_begin(name)
#define _MESA_GPUVIS_TRACE_END() util_gpuvis_end()

#else

#define _MESA_GPUVIS_TRACE_BEGIN(name)
#define _MESA_GPUVIS_TRACE_END()

#endif /* HAVE_GPUVIS */

#if __has_attribute(cleanup) && __has_attribute(unused)

#define _MESA_TRACE_SCOPE_VAR_CONCAT(name, suffix) name##suffix
#define _MESA_TRACE_SCOPE_VAR(suffix)                                        \
   _MESA_TRACE_SCOPE_VAR_CONCAT(_mesa_trace_scope_, suffix)

/* This must expand to a single non-scoped statement for
 *
 *    if (cond)
 *       _MESA_TRACE_SCOPE(...)
 *
 * to work.
 */
#define _MESA_TRACE_SCOPE(name)                                              \
   int _MESA_TRACE_SCOPE_VAR(__LINE__)                                       \
      __attribute__((cleanup(_mesa_trace_scope_end), unused)) =              \
         _mesa_trace_scope_begin(name)

#define _MESA_TRACE_SCOPE_FLOW(name, id)                                     \
   int _MESA_TRACE_SCOPE_VAR(__LINE__)                                       \
      __attribute__((cleanup(_mesa_trace_scope_end), unused)) =              \
         _mesa_trace_scope_flow_begin(name, id)

static inline int
_mesa_trace_scope_begin(const char *name)
{
   _MESA_TRACE_BEGIN(name);
   _MESA_GPUVIS_TRACE_BEGIN(name);
   return 0;
}

static inline int
_mesa_trace_scope_flow_begin(const char *name, uint64_t *id)
{
   if (*id == 0)
      *id = util_perfetto_next_id();
   _MESA_TRACE_FLOW_BEGIN(name, *id);
   _MESA_GPUVIS_TRACE_BEGIN(name);
   return 0;
}

static inline void
_mesa_trace_scope_end(UNUSED int *scope)
{
   _MESA_GPUVIS_TRACE_END();
   _MESA_TRACE_END();
}

#else

#define _MESA_TRACE_SCOPE(name)

#endif /* __has_attribute(cleanup) && __has_attribute(unused) */

#define MESA_TRACE_SCOPE(name) _MESA_TRACE_SCOPE(name)
#define MESA_TRACE_SCOPE_FLOW(name, id) _MESA_TRACE_SCOPE_FLOW(name, id)
#define MESA_TRACE_FUNC() _MESA_TRACE_SCOPE(__func__)
#define MESA_TRACE_FUNC_FLOW(id) _MESA_TRACE_SCOPE_FLOW(__func__, id)
#define MESA_TRACE_SET_COUNTER(name, value) _MESA_TRACE_SET_COUNTER(name, value)
#define MESA_TRACE_TIMESTAMP_BEGIN(name, track_id, flow_id, timestamp) \
   _MESA_TRACE_TIMESTAMP_BEGIN(name, track_id, flow_id, timestamp)
#define MESA_TRACE_TIMESTAMP_END(name, track_id, timestamp) \
   _MESA_TRACE_TIMESTAMP_END(name, track_id, timestamp)

static inline void
util_cpu_trace_init()
{
   util_perfetto_init();
   util_gpuvis_init();
}

#endif /* CPU_TRACE_H */
