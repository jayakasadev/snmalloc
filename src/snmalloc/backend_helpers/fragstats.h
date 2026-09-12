#pragma once

// SPDX-License-Identifier: MIT
//
// Backend commit/decommit byte counters, reported across the C / Rust FFI
// boundary:
//
//   bytes_committed          bytes committed and not yet released.
//
//   bytes_decommitted_to_os  bytes released since process start; only ever
//                            increases.
//
// `commitrange.h` is the only writer, so these count only the commits and
// decommits that pass through a `CommitRange`.  Other callers of
// `PAL::notify_using` / `notify_not_using`, such as `LargeBuddyRange`'s decay
// sweep, are not reflected here.
//
// The counters are `static inline`, so every translation unit and every
// `CommitRange<PAL>` instantiation shares one instance.

#include "largebuddyrange.h"
#include "snmalloc/stl/atomic.h"

#include <stddef.h>
#include <stdint.h>

namespace snmalloc
{
  /**
   * Snapshot of the backend commit counters, returned by
   * `get_backend_frag_stats()`.
   *
   * All fields are `uint64_t` to match the wire format of
   * `struct snmalloc_full_stats`.
   */
  struct BackendFragStats
  {
    /** Bytes currently committed via the PAL. */
    uint64_t bytes_committed;
    /** Cumulative bytes returned to the OS via `notify_not_using`. */
    uint64_t bytes_decommitted_to_os;
    /**
     * Free-chunk counts summed over every live `LargeBuddyRange` in the
     * process.  Entry `i` counts free chunks of size
     * `1 << (MIN_CHUNK_BITS + i)` bytes.
     */
    uint64_t
      free_chunk_count_by_log_size[LargeBuddyFreeChunkHistogram::NUM_BUCKETS];
  };

  /**
   * Process-global storage for the commit counters.  Never instantiated; the
   * members are `static inline` so all users share one copy.
   *
   * `commitrange.h` is the only writer, this header the only reader.  Updates
   * are relaxed: the counters are for reporting only and order nothing else.
   */
  struct BackendFragCounters
  {
    // One cache line each, so concurrent commit and decommit on different
    // threads do not false-share.
    alignas(64) static inline stl::Atomic<size_t> bytes_committed{0};
    alignas(64) static inline stl::Atomic<size_t> bytes_decommitted_to_os{0};

    /**
     * Record `size` bytes committed.  Called by
     * `CommitRange<PAL>::alloc_range` once `notify_using` has succeeded.
     *
     * A no-op unless SNMALLOC_STATS_BASIC is defined.
     */
    static void on_commit(size_t size)
    {
#ifdef SNMALLOC_STATS_BASIC
      bytes_committed.fetch_add(size, stl::memory_order_relaxed);
#else
      (void)size;
#endif
    }

    /**
     * Record `size` bytes decommitted.  Called by
     * `CommitRange<PAL>::dealloc_range` once the PAL has been told to release
     * the pages.  Subtracts from `bytes_committed` and adds to
     * `bytes_decommitted_to_os`.
     *
     * A no-op unless SNMALLOC_STATS_BASIC is defined.
     */
    static void on_decommit(size_t size)
    {
#ifdef SNMALLOC_STATS_BASIC
      // Subtract with a floor of zero rather than `fetch_sub`, which would
      // wrap if `bytes_committed < size`.  That should not happen (every
      // decommit follows a commit), but a wrapped counter is much worse than
      // a clamped one.
      auto prev = bytes_committed.load(stl::memory_order_relaxed);
      while (true)
      {
        auto next = (prev >= size) ? (prev - size) : 0;
        if (bytes_committed.compare_exchange_weak(
              prev, next, stl::memory_order_relaxed))
        {
          break;
        }
      }
      bytes_decommitted_to_os.fetch_add(size, stl::memory_order_relaxed);
#else
      (void)size;
#endif
    }
  };

  /**
   * Read the backend commit counters.
   *
   * Each field is loaded separately, so the result is not a consistent
   * point-in-time view: a concurrent commit or decommit can leave the fields
   * disagreeing by one operation.
   */
  inline BackendFragStats get_backend_frag_stats()
  {
    BackendFragStats out{};
    out.bytes_committed = static_cast<uint64_t>(
      BackendFragCounters::bytes_committed.load(stl::memory_order_relaxed));
    out.bytes_decommitted_to_os =
      static_cast<uint64_t>(BackendFragCounters::bytes_decommitted_to_os.load(
        stl::memory_order_relaxed));
    // The free-chunk histogram is process-global and maintained by
    // `Buddy::add_block` / `Buddy::remove_block`, so it can be read without
    // reference to any particular range instance.
    LargeBuddyFreeChunkHistogram::snapshot(out.free_chunk_count_by_log_size);
    return out;
  }
} // namespace snmalloc
