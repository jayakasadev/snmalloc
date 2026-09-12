// SPDX-License-Identifier: MIT
//
// Human-readable text dump of allocator telemetry.
//
// Formats a `snmalloc_get_full_stats` snapshot; collects nothing itself and
// mutates no allocator state.  Three entry points:
//
//   snmalloc_dump_stats_to_buffer   writes into a caller-supplied buffer
//                                   following the snprintf contract.  This is
//                                   the C ABI form, and the other two are
//                                   wrappers around it.
//   snmalloc::dump_stats            writes to a FILE stream.
//   snmalloc::dump_stats_to_string  writes into a std::string.

#pragma once

#include <stddef.h>
#include <stdio.h>

#ifndef SNMALLOC_EXPORT
#  define SNMALLOC_EXPORT
#endif

#ifdef __cplusplus
#  include <string>
#endif

#ifdef __cplusplus
extern "C"
{
#endif

  /**
   * Format a fresh telemetry snapshot into `buf`, following the `snprintf`
   * rules for truncation:
   *   * with enough room, the full text plus a NUL terminator is written;
   *   * otherwise as many bytes as fit are written, still NUL-terminated when
   *     `buf_len > 0`;
   *   * a NULL `buf` or zero `buf_len` writes nothing.
   *
   * Returns the length the text would have, excluding the NUL.  To size a
   * buffer exactly, call once with `(NULL, 0)`, allocate that plus one, then
   * call again.
   *
   * Safe to call from any thread at any time.
   */
  SNMALLOC_EXPORT size_t
  snmalloc_dump_stats_to_buffer(char* buf, size_t buf_len);

#ifdef __cplusplus
} // extern "C"
#endif

#ifdef __cplusplus
namespace snmalloc
{
  /**
   * Write a telemetry snapshot to the writable stream `out`, sizing the
   * temporary buffer internally.  Does nothing when `out` is null.
   */
  SNMALLOC_EXPORT void dump_stats(FILE* out);

  /**
   * Write a telemetry snapshot into `out`, which is cleared first and then
   * resized to the exact text length.
   */
  SNMALLOC_EXPORT void dump_stats_to_string(std::string& out);
} // namespace snmalloc
#endif // __cplusplus
