// SPDX-License-Identifier: MIT
//
// Public C ABI for aggregated allocator telemetry: the layout of
// `struct snmalloc_full_stats` and the `snmalloc_get_full_stats` getter,
// defined in `src/snmalloc/override/stats_export.cc`.
//
// Only POD types and fixed-width integers appear here, so the layout is stable
// for the Rust binding in `snmalloc-sys` and for C++ callers that do not want
// to depend on the Config templates.
//
// Which fields carry data depends on how snmalloc was built; see the field
// documentation below.  Fields that a build does not populate read as zero.

#pragma once

#include <stdint.h>

#ifndef SNMALLOC_EXPORT
#  define SNMALLOC_EXPORT
#endif

/**
 * Wire-format version for `struct snmalloc_full_stats`.
 *
 * Bumped whenever a slot from `reserved[]` starts carrying real data.  The
 * offsets of already-defined fields never change, so a consumer that reads
 * this field first can treat a higher-than-expected version as "extra fields I
 * do not know about" and ignore them.
 *
 * Version 2 added the free-chunk histogram in `reserved[0..15]`.
 */
#define SNMALLOC_FULL_STATS_VERSION 2u

/**
 * Number of `reserved[]` slots used by the free-chunk histogram.  Slot `i`
 * counts free chunks of size `1 << (MIN_CHUNK_BITS + i)` bytes.
 */
#define SNMALLOC_FULL_STATS_FREECHUNK_BUCKETS 16u

/**
 * Number of slots in each per-size-class array.  Wide enough for snmalloc's 64
 * small-object size classes.
 */
#define SNMALLOC_FULL_STATS_SIZECLASS_SLOTS 64u

/**
 * Number of buckets in the allocation-lifetime histogram.
 */
#define SNMALLOC_FULL_STATS_LIFETIME_BUCKETS 32u

/**
 * Size of the trailing `reserved[]` pool.  Later revisions take new fields
 * from here; `SNMALLOC_FULL_STATS_VERSION` says how many are in use.
 */
#define SNMALLOC_FULL_STATS_RESERVED_SLOTS 64u

#ifdef __cplusplus
extern "C"
{
#endif

  /**
   * Aggregated allocator telemetry snapshot.  Layout is identical on both
   * sides of the C / Rust FFI boundary.
   *
   * Field semantics:
   *
   *   `version`
   *     `SNMALLOC_FULL_STATS_VERSION` as of the producer's build.
   *
   *   `bytes_in_use` / `peak_bytes_in_use`
   *     Bytes reserved from the OS, at range granularity -- not a total of
   *     live allocation sizes.  Same numbers the Rust
   *     `SnMalloc::memory_stats()` getter reports.
   *
   *   `bytes_mapped` / `bytes_committed` / `bytes_decommitted_to_os`
   *     Byte counts for mapped, committed, and released-to-OS memory.
   *
   *   `fast_path_allocs` / `slow_path_allocs` / `fast_path_deallocs` /
   *   `remote_deallocs` / `message_queue_drains` /
   *   `cross_thread_messages_received`
   *     Frontend operation counters, summed over all threads.  Zero unless
   *     built with SNMALLOC_STATS_BASIC.
   *
   *   `total_live_bytes_by_class[]` / `total_live_count_by_class[]` /
   *   `cumulative_alloc_by_class[]` / `cumulative_dealloc_by_class[]`
   *     Per-size-class totals, indexed by small-object size class.  Zero
   *     unless built with SNMALLOC_STATS_FULL.
   *
   *   `lifetime_buckets_ns[]`
   *     Allocation lifetimes bucketed by log2 nanoseconds.  Zero unless built
   *     with both SNMALLOC_PROFILE and SNMALLOC_STATS_FULL.
   *
   *   `reserved[]`
   *     Slots for future fields.  As of version 2, `reserved[i]` for `i` in
   *     `[0, 15]` counts free chunks of size `1 << (MIN_CHUNK_BITS + i)` bytes
   *     across the `LargeBuddyRange` caches.  The rest read as zero.
   */
  struct snmalloc_full_stats
  {
    uint32_t version;
    /* Explicit padding so the uint64_t fields below are naturally aligned on
     * every compiler and platform.  Any change to this struct must keep the
     * offsets of the fields already defined here. */
    uint32_t _pad0;

    uint64_t bytes_in_use;
    uint64_t peak_bytes_in_use;

    uint64_t bytes_mapped;
    uint64_t bytes_committed;
    uint64_t bytes_decommitted_to_os;

    uint64_t fast_path_allocs;
    uint64_t slow_path_allocs;
    uint64_t fast_path_deallocs;
    uint64_t remote_deallocs;
    uint64_t message_queue_drains;
    uint64_t cross_thread_messages_received;

    uint64_t total_live_bytes_by_class[SNMALLOC_FULL_STATS_SIZECLASS_SLOTS];
    uint64_t total_live_count_by_class[SNMALLOC_FULL_STATS_SIZECLASS_SLOTS];
    uint64_t cumulative_alloc_by_class[SNMALLOC_FULL_STATS_SIZECLASS_SLOTS];
    uint64_t cumulative_dealloc_by_class[SNMALLOC_FULL_STATS_SIZECLASS_SLOTS];

    uint64_t lifetime_buckets_ns[SNMALLOC_FULL_STATS_LIFETIME_BUCKETS];

    uint64_t reserved[SNMALLOC_FULL_STATS_RESERVED_SLOTS];
  };

  /**
   * Fill in `*out` with a telemetry snapshot.  `out` must be non-NULL.
   *
   * `*out` is zeroed first, so fields this build does not populate read as
   * zero.  Read-only and safe to call from any thread at any time.  The
   * counters are read one at a time, so the result is not a consistent
   * point-in-time view of a running process.
   */
  SNMALLOC_EXPORT void snmalloc_get_full_stats(struct snmalloc_full_stats* out);

#ifdef __cplusplus
} // extern "C"
#endif
