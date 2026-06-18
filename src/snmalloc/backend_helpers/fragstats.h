#pragma once

// SPDX-License-Identifier: MIT
//
// Backend fragmentation counters.
//
// Exposes OS-level memory-accounting figures surfaced across the
// C / Rust FFI boundary:
//
//   bytes_committed           -- bytes currently in the "in use" state
//                                from the PAL's perspective; on POSIX
//                                that means pages handed to the OS via
//                                `notify_using` and not yet released via
//                                `notify_not_using`.
//
//   bytes_decommitted_to_os   -- cumulative bytes handed back to the OS
//                                via `PAL::notify_not_using` since
//                                process start.  Strictly monotone.
//
// `commitrange.h` is the only writer, incrementing the atomics from its
// `notify_using` / `notify_not_using` branches.  The backend path is not
// the hot path (commit calls hit the PAL, which already issues a syscall
// on most platforms), so the relaxed atomics introduce negligible
// overhead.
//
// Inline `static` data members keep the symbols header-only; the linker
// collapses the multiple TU definitions to one shared instance.

#include "largebuddyrange.h"
#include "snmalloc/stl/atomic.h"

#include <stddef.h>
#include <stdint.h>

namespace snmalloc
{
  /**
   * POD snapshot of the backend fragmentation counters.  Returned by
   * `get_backend_frag_stats()`; populated by the FullAllocStats getter
   * in `src/snmalloc/override/stats_export.cc`.
   *
   * All fields are u64 to match the wire format of
   * `struct snmalloc_full_stats`; the underlying atomics are
   * `size_t`-typed but the cast is safe on every platform snmalloc
   * supports (size_t is at most 64 bits).
   *
   * The `free_chunk_count_by_log_size` buckets correspond to chunk
   * sizes from `MIN_CHUNK_SIZE` (typically 16 KiB) up to
   * `MIN_CHUNK_SIZE << 15`, log2-spaced.
   */
  struct BackendFragStats
  {
    /** Bytes the allocator currently has committed via the PAL. */
    uint64_t bytes_committed;
    /** Cumulative bytes returned to the OS via `notify_not_using`. */
    uint64_t bytes_decommitted_to_os;
    /**
     * Log2-bucketed free-chunk histogram aggregated across every live
     * `LargeBuddyRange` Buddy in the process.
     * `free_chunk_count_by_log_size[i]` is the live count of free chunks
     * of size `1 << (MIN_CHUNK_BITS + i)` bytes.
     */
    uint64_t
      free_chunk_count_by_log_size[LargeBuddyFreeChunkHistogram::NUM_BUCKETS];
  };

  /**
   * Process-global counter storage for the backend fragmentation
   * accounting.  The struct itself is never instantiated; the static
   * inline members let the counters live in a single linkage unit
   * regardless of how many `CommitRange<PAL>` template instantiations
   * the build emits.
   *
   * `commitrange.h` is the only writer; this header is the only
   * reader.  Atomic updates use `memory_order_relaxed` -- the counters
   * are not used for synchronisation, only for reporting.
   */
  struct BackendFragCounters
  {
    // alignas(64) places each atomic on its own cache line to avoid
    // false sharing.  Without padding the two counters land in adjacent
    // 8-byte slots in the same line; a thread bumping `bytes_committed`
    // would then contend with a concurrent `bytes_decommitted_to_os`
    // increment, costing inter-core invalidations.
    alignas(64) static inline stl::Atomic<size_t> bytes_committed{0};
    alignas(64) static inline stl::Atomic<size_t> bytes_decommitted_to_os{0};

    /**
     * Record a successful `notify_using` of `size` bytes.  Called from
     * `CommitRange<PAL>::alloc_range` after the PAL hands the pages
     * back as in-use.
     *
     * Compiles to a no-op when SNMALLOC_STATS_BASIC is off, so backend
     * ranges pay zero atomic overhead in that configuration.
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
     * Record a `notify_not_using` of `size` bytes.  Called from
     * `CommitRange<PAL>::dealloc_range` after the PAL has been told to
     * release the pages.  Decreases the live `bytes_committed` figure
     * (clamped at zero to stay defensive against any future caller
     * that double-frees) and bumps the cumulative
     * `bytes_decommitted_to_os` counter.
     *
     * Compiles to a no-op when SNMALLOC_STATS_BASIC is off, matching
     * `on_commit`.
     */
    static void on_decommit(size_t size)
    {
#ifdef SNMALLOC_STATS_BASIC
      // Defensive clamped subtract.  `fetch_sub` of `size` would
      // underflow if `bytes_committed < size`; under normal operation
      // that cannot happen (every dealloc matches a prior alloc), but
      // we treat the underflow path as a no-op rather than corrupting
      // the counter.
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
   * Read a coherent (per-counter) snapshot of the backend
   * fragmentation accounting.
   *
   * The two atomics are loaded with `memory_order_relaxed` and the
   * snapshot is NOT transactional: a concurrent commit/decommit may
   * cause the returned `bytes_committed` to lag `bytes_decommitted_to_os`
   * by one operation.  Callers that need a strict invariant should
   * sample twice and reconcile, but for telemetry purposes the
   * single-snapshot read is sufficient.
   */
  inline BackendFragStats get_backend_frag_stats()
  {
    BackendFragStats out{};
    out.bytes_committed = static_cast<uint64_t>(
      BackendFragCounters::bytes_committed.load(stl::memory_order_relaxed));
    out.bytes_decommitted_to_os =
      static_cast<uint64_t>(BackendFragCounters::bytes_decommitted_to_os.load(
        stl::memory_order_relaxed));
    // Snapshot the process-global LargeBuddyRange free-chunk histogram.
    // It is owned by `LargeBuddyFreeChunkHistogram` and updated from
    // `Buddy::add_block` / `Buddy::remove_block` whenever a chunk enters
    // or leaves the free list.  Reading has no template-state dependency,
    // so a direct static snapshot suffices.
    LargeBuddyFreeChunkHistogram::snapshot(out.free_chunk_count_by_log_size);
    return out;
  }
} // namespace snmalloc
