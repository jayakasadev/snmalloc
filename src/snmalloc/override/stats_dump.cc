// SPDX-License-Identifier: MIT
//
// Implementation of the text dump declared in
// `src/snmalloc/global/stats_dump.h`.  Formats a `snmalloc_get_full_stats`
// snapshot as:
//
//   ------------------------------------------------
//   MALLOC:    ....... (   ..  MiB) Bytes in use by application
//   ... (one MALLOC: line per figure)
//   ------------------------------------------------
//   Class   Size       Live  TotalAllocs  TotalDeallocs
//      0      16        230         5012           4782
//   ... (one row per non-empty size class)
//   ------------------------------------------------
//   Lifetime histogram (log2 ns buckets):
//      bucket   range              count
//          0   [1 ns - 2 ns)        ....
//   ... (one row per non-empty bucket)
//   ------------------------------------------------
//
// The two optional tables are omitted when they hold no data, so a build
// without the stats or profile tiers still produces a readable dump.

#include "snmalloc/global/stats_dump.h"

#include "../snmalloc.h"
#include "snmalloc/global/stats_export.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <string>

#ifndef SNMALLOC_EXPORT
#  define SNMALLOC_EXPORT
#endif

namespace
{
  /// State of an in-progress write into a caller's buffer.
  ///
  /// `written` counts the bytes actually placed in `buf`, excluding the NUL.
  /// `total` counts what the text would take with an unbounded buffer, and is
  /// still accumulated when `buf` is NULL, which is how the size query works.
  struct WriteCursor
  {
    char* buf;
    size_t cap;
    size_t written;
    size_t total;
  };

  /// Append `fmt`-formatted text to `*cursor`, truncating rather than
  /// overflowing.  Leaves `buf` NUL-terminated whenever `cap > 0`.
  static void cursor_printf(WriteCursor* cursor, const char* fmt, ...)
  {
    va_list args;
    va_start(args, fmt);
    // `vsnprintf`'s size argument includes room for the terminator.
    size_t remaining =
      (cursor->buf != nullptr && cursor->cap > cursor->written) ?
      (cursor->cap - cursor->written) :
      0;
    int n = vsnprintf(
      cursor->buf != nullptr ? cursor->buf + cursor->written : nullptr,
      remaining,
      fmt,
      args);
    va_end(args);

    if (n < 0)
    {
      // Encoding error.  Append nothing and advance neither counter.
      return;
    }

    size_t emitted = static_cast<size_t>(n);
    cursor->total += emitted;
    if (cursor->buf != nullptr && remaining > 0)
    {
      // `vsnprintf` kept one byte back for the terminator, so at most
      // `remaining - 1` bytes reached the buffer.
      size_t actually_written =
        emitted < (remaining - 1) ? emitted : (remaining - 1);
      cursor->written += actually_written;
    }
  }

  /// Render `bytes` as a B / KiB / MiB / GiB string.  `out` must hold at least
  /// 32 bytes.
  static void bytes_to_human(uint64_t bytes, char* out, size_t out_cap)
  {
    constexpr double kKiB = 1024.0;
    constexpr double kMiB = kKiB * 1024.0;
    constexpr double kGiB = kMiB * 1024.0;
    double b = static_cast<double>(bytes);
    if (b >= kGiB)
      snprintf(out, out_cap, "%6.1f GiB", b / kGiB);
    else if (b >= kMiB)
      snprintf(out, out_cap, "%6.1f MiB", b / kMiB);
    else if (b >= kKiB)
      snprintf(out, out_cap, "%6.1f KiB", b / kKiB);
    else
      snprintf(out, out_cap, "%6.0f   B", b);
  }

  /// Render bucket `bucket`'s range, `[2^bucket, 2^(bucket+1))` nanoseconds,
  /// into `out`, choosing whichever time unit reads best.
  static void
  lifetime_range_to_human(unsigned bucket, char* out, size_t out_cap)
  {
    // Bounds in nanoseconds, capped at `1 << 63` so the shifts stay in range.
    uint64_t lo =
      (bucket >= 63u) ? (uint64_t{1} << 63) : (uint64_t{1} << bucket);
    uint64_t hi =
      (bucket >= 62u) ? (uint64_t{1} << 63) : (uint64_t{1} << (bucket + 1u));

    auto fmt_one = [](uint64_t ns, char* dst, size_t cap) {
      if (ns >= 3'600'000'000'000ull)
        snprintf(
          dst,
          cap,
          "%llu hr",
          static_cast<unsigned long long>(ns / 3'600'000'000'000ull));
      else if (ns >= 1'000'000'000ull)
        snprintf(
          dst,
          cap,
          "%llu s",
          static_cast<unsigned long long>(ns / 1'000'000'000ull));
      else if (ns >= 1'000'000ull)
        snprintf(
          dst,
          cap,
          "%llu ms",
          static_cast<unsigned long long>(ns / 1'000'000ull));
      else if (ns >= 1'000ull)
        snprintf(
          dst, cap, "%llu us", static_cast<unsigned long long>(ns / 1'000ull));
      else
        snprintf(dst, cap, "%llu ns", static_cast<unsigned long long>(ns));
    };

    char lo_str[24];
    char hi_str[24];
    fmt_one(lo, lo_str, sizeof(lo_str));
    fmt_one(hi, hi_str, sizeof(hi_str));
    snprintf(out, out_cap, "[%s - %s)", lo_str, hi_str);
  }

  /// Object size in bytes for size-class slot `slot`.  Returns 0 for slots
  /// with no corresponding class in this configuration.
  static uint64_t sizeclass_slot_to_bytes(unsigned slot)
  {
    if (slot >= snmalloc::NUM_SMALL_SIZECLASSES)
      return 0;
    return static_cast<uint64_t>(snmalloc::sizeclass_to_size(
      static_cast<snmalloc::smallsizeclass_t>(slot)));
  }

  /// Write the whole dump of snapshot `s` into `cursor`.
  static void format_dump(WriteCursor* cursor, const snmalloc_full_stats* s)
  {
    char human[32];

    cursor_printf(cursor, "------------------------------------------------\n");

    bytes_to_human(s->bytes_in_use, human, sizeof(human));
    cursor_printf(
      cursor,
      "MALLOC:   %12llu (%s) Bytes in use by application\n",
      static_cast<unsigned long long>(s->bytes_in_use),
      human);

    bytes_to_human(s->peak_bytes_in_use, human, sizeof(human));
    cursor_printf(
      cursor,
      "MALLOC: + %12llu (%s) Peak bytes in use\n",
      static_cast<unsigned long long>(s->peak_bytes_in_use),
      human);

    bytes_to_human(s->bytes_committed, human, sizeof(human));
    cursor_printf(
      cursor,
      "MALLOC: + %12llu (%s) Bytes committed to OS\n",
      static_cast<unsigned long long>(s->bytes_committed),
      human);

    bytes_to_human(s->bytes_decommitted_to_os, human, sizeof(human));
    cursor_printf(
      cursor,
      "MALLOC: + %12llu (%s) Bytes decommitted (returned to OS)\n",
      static_cast<unsigned long long>(s->bytes_decommitted_to_os),
      human);

    cursor_printf(
      cursor,
      "MALLOC:   %12llu              Fast-path allocations\n",
      static_cast<unsigned long long>(s->fast_path_allocs));

    cursor_printf(
      cursor,
      "MALLOC:   %12llu              Slow-path allocations\n",
      static_cast<unsigned long long>(s->slow_path_allocs));

    cursor_printf(
      cursor,
      "MALLOC:   %12llu              Fast-path deallocations\n",
      static_cast<unsigned long long>(s->fast_path_deallocs));

    cursor_printf(
      cursor,
      "MALLOC:   %12llu              Cross-thread deallocations\n",
      static_cast<unsigned long long>(s->remote_deallocs));

    cursor_printf(
      cursor,
      "MALLOC:   %12llu              Message-queue drains\n",
      static_cast<unsigned long long>(s->message_queue_drains));

    cursor_printf(
      cursor,
      "MALLOC:   %12llu              Cross-thread messages received\n",
      static_cast<unsigned long long>(s->cross_thread_messages_received));

    // Per-size-class table: one row per class with any non-zero counter.  The
    // whole section is skipped when every class is empty, which is the case in
    // a build without the per-class instrumentation.
    bool any_class = false;
    for (unsigned i = 0; i < SNMALLOC_FULL_STATS_SIZECLASS_SLOTS; ++i)
    {
      if (
        s->total_live_count_by_class[i] != 0 ||
        s->cumulative_alloc_by_class[i] != 0 ||
        s->cumulative_dealloc_by_class[i] != 0)
      {
        any_class = true;
        break;
      }
    }
    if (any_class)
    {
      cursor_printf(
        cursor, "------------------------------------------------\n");
      cursor_printf(
        cursor, "Class   Size         Live    TotalAllocs    TotalDeallocs\n");
      for (unsigned i = 0; i < SNMALLOC_FULL_STATS_SIZECLASS_SLOTS; ++i)
      {
        if (
          s->total_live_count_by_class[i] == 0 &&
          s->cumulative_alloc_by_class[i] == 0 &&
          s->cumulative_dealloc_by_class[i] == 0)
          continue;
        uint64_t bytes = sizeclass_slot_to_bytes(i);
        cursor_printf(
          cursor,
          "%5u  %5llu  %11llu  %13llu  %15llu\n",
          i,
          static_cast<unsigned long long>(bytes),
          static_cast<unsigned long long>(s->total_live_count_by_class[i]),
          static_cast<unsigned long long>(s->cumulative_alloc_by_class[i]),
          static_cast<unsigned long long>(s->cumulative_dealloc_by_class[i]));
      }
    }

    // Lifetime histogram: one row per non-zero bucket.  Skipped entirely when
    // every bucket is zero, as in a build without profiling or before any
    // sampled allocation has been freed.
    bool any_bucket = false;
    for (unsigned i = 0; i < SNMALLOC_FULL_STATS_LIFETIME_BUCKETS; ++i)
    {
      if (s->lifetime_buckets_ns[i] != 0)
      {
        any_bucket = true;
        break;
      }
    }
    if (any_bucket)
    {
      cursor_printf(
        cursor, "------------------------------------------------\n");
      cursor_printf(cursor, "Lifetime histogram (log2 ns buckets):\n");
      cursor_printf(cursor, "  bucket  range                       count\n");
      // Enough for `[%s - %s)` with two 23-byte bounds, the framing
      // characters, and the terminator.
      char range[64];
      for (unsigned i = 0; i < SNMALLOC_FULL_STATS_LIFETIME_BUCKETS; ++i)
      {
        if (s->lifetime_buckets_ns[i] == 0)
          continue;
        lifetime_range_to_human(i, range, sizeof(range));
        cursor_printf(
          cursor,
          "  %6u  %-26s %12llu\n",
          i,
          range,
          static_cast<unsigned long long>(s->lifetime_buckets_ns[i]));
      }
    }

    cursor_printf(cursor, "------------------------------------------------\n");
  }
} // namespace

extern "C" SNMALLOC_EXPORT size_t
snmalloc_dump_stats_to_buffer(char* buf, size_t buf_len)
{
  snmalloc_full_stats snap;
  // Left uninitialised: `snmalloc_get_full_stats` zeroes it first.
  snmalloc_get_full_stats(&snap);

  WriteCursor cursor{buf, buf_len, 0, 0};
  format_dump(&cursor, &snap);

  // `cursor_printf` terminates on every append, but terminate again here so
  // the buffer is still valid if nothing was appended at all.
  if (buf != nullptr && buf_len > 0)
  {
    size_t term_idx = cursor.written < buf_len ? cursor.written : buf_len - 1;
    buf[term_idx] = '\0';
  }

  return cursor.total;
}

namespace snmalloc
{
  SNMALLOC_EXPORT void dump_stats(FILE* out)
  {
    if (out == nullptr)
      return;
    size_t needed = snmalloc_dump_stats_to_buffer(nullptr, 0);
    // A std::string as the buffer, so it is freed on every return path.  The
    // fill below passes `needed + 1` to leave room for the terminator, which
    // std::string keeps past its own end.
    std::string buf;
    buf.resize(needed);
    if (needed > 0)
    {
      snmalloc_dump_stats_to_buffer(&buf[0], needed + 1);
    }
    if (!buf.empty())
    {
      fwrite(buf.data(), 1, buf.size(), out);
    }
  }

  SNMALLOC_EXPORT void dump_stats_to_string(std::string& out)
  {
    size_t needed = snmalloc_dump_stats_to_buffer(nullptr, 0);
    out.clear();
    out.resize(needed);
    if (needed > 0)
    {
      snmalloc_dump_stats_to_buffer(&out[0], needed + 1);
    }
  }
} // namespace snmalloc
