#pragma once

// Telemetry counters owned by an `Allocator`, plus the hooks its hot paths
// call to update them.
//
// Two build flags gate what exists here:
//   SNMALLOC_STATS_BASIC  per-thread frontend counters.
//   SNMALLOC_STATS_FULL   adds the per-size-class histogram; implies BASIC.
// With both off, `AllocStats` is an empty struct with empty hooks.
//
// Only the thread owning the allocator writes these counters, so they are
// plain non-atomic `uint64_t`.  Other threads read them through
// `snmalloc_get_full_stats`, which walks the allocator pool and then adds the
// process-global aggregators below.  Those globals hold the counters of
// allocators already returned to the pool at thread exit, which the pool walk
// no longer visits.

#include "../ds/sizeclasstable.h"
#include "../ds_core/defines.h"

#ifdef SNMALLOC_STATS_BASIC
#  include "snmalloc/stl/atomic.h"
#endif

#include <cstdint>

namespace snmalloc
{
#ifdef SNMALLOC_STATS_BASIC
  /**
   * Per-thread frontend counters, embedded in every `Allocator`.
   *
   * Given its own cache line so counter writes do not false-share with
   * neighbouring `Allocator` fields.
   */
  struct alignas(CACHELINE_SIZE) FrontendStats
  {
    /// Allocations credited at slab refill (fast + slow).
    uint64_t total_allocs{0};
    /// Slab-refill calls (one per `on_small_refill`).
    uint64_t slow_path_allocs{0};

    /// Fast-path alloc count, i.e. `total_allocs - slow_path_allocs`.
    [[nodiscard]] uint64_t fast_path_allocs() const noexcept
    {
      return total_allocs - slow_path_allocs;
    }

    /**
     * Deallocations of objects this allocator owns.
     *
     * Credited in advance at slab refill (one per object handed to the fast
     * free list), not per dealloc.  It therefore over-counts by however many
     * of those objects another thread ends up freeing; those frees bump
     * `remote_deallocs` instead.
     */
    uint64_t fast_path_deallocs{0};
    /// Deallocations of objects owned by another allocator, routed through the
    /// remote dealloc cache.
    uint64_t remote_deallocs{0};
    /// Number of times this thread drained its incoming message queue.
    uint64_t message_queue_drains{0};
    /// Cross-thread messages dequeued by this thread.
    uint64_t cross_thread_messages_received{0};

    /// Add another block's counters into this one.
    void accumulate(const FrontendStats& other) noexcept
    {
      total_allocs += other.total_allocs;
      slow_path_allocs += other.slow_path_allocs;
      fast_path_deallocs += other.fast_path_deallocs;
      remote_deallocs += other.remote_deallocs;
      message_queue_drains += other.message_queue_drains;
      cross_thread_messages_received += other.cross_thread_messages_received;
    }
  };

  /**
   * Process-global aggregator that a thread drains its `FrontendStats` into at
   * teardown.  Exited threads no longer appear in `AllocPool::iterate()`, so
   * without this their counters would vanish from the snapshot.
   *
   * Atomic because a thread may be draining while another thread is reading
   * the snapshot.  Relaxed ordering is enough: these counters order nothing
   * else.
   */
  struct FrontendStatsGlobal
  {
    stl::Atomic<uint64_t> total_allocs{0};
    stl::Atomic<uint64_t> slow_path_allocs{0};
    stl::Atomic<uint64_t> fast_path_deallocs{0};
    stl::Atomic<uint64_t> remote_deallocs{0};
    stl::Atomic<uint64_t> message_queue_drains{0};
    stl::Atomic<uint64_t> cross_thread_messages_received{0};

    void drain_from(const FrontendStats& s) noexcept
    {
      total_allocs.fetch_add(s.total_allocs, stl::memory_order_relaxed);
      slow_path_allocs.fetch_add(
        s.slow_path_allocs, stl::memory_order_relaxed);
      fast_path_deallocs.fetch_add(
        s.fast_path_deallocs, stl::memory_order_relaxed);
      remote_deallocs.fetch_add(s.remote_deallocs, stl::memory_order_relaxed);
      message_queue_drains.fetch_add(
        s.message_queue_drains, stl::memory_order_relaxed);
      cross_thread_messages_received.fetch_add(
        s.cross_thread_messages_received, stl::memory_order_relaxed);
    }

    void snapshot_into(FrontendStats& out) noexcept
    {
      out.total_allocs += total_allocs.load(stl::memory_order_relaxed);
      out.slow_path_allocs +=
        slow_path_allocs.load(stl::memory_order_relaxed);
      out.fast_path_deallocs +=
        fast_path_deallocs.load(stl::memory_order_relaxed);
      out.remote_deallocs += remote_deallocs.load(stl::memory_order_relaxed);
      out.message_queue_drains +=
        message_queue_drains.load(stl::memory_order_relaxed);
      out.cross_thread_messages_received +=
        cross_thread_messages_received.load(stl::memory_order_relaxed);
    }
  };

  inline FrontendStatsGlobal& frontend_stats_global() noexcept
  {
    static FrontendStatsGlobal g;
    return g;
  }
#endif // SNMALLOC_STATS_BASIC

#ifdef SNMALLOC_STATS_FULL
  /**
   * Per-thread histogram over small sizeclasses, embedded in every `Allocator`
   * alongside `FrontendStats`.  Arrays are indexed by `smallsizeclass_t` and
   * written only by the owning thread.
   *
   * A cross-thread free is split across two blocks: the freeing thread bumps
   * its own `cumulative_dealloc[sc]`, and the owning thread applies the
   * matching `live_count[sc]` decrement.  The two only balance once the whole
   * pool is summed.
   *
   * Given its own cache line for the same reason as `FrontendStats`.
   */
  struct alignas(CACHELINE_SIZE) SizeClassStats
  {
    /// Live bytes per small sizeclass.  Producers leave this at zero;
    /// stats_export.cc derives it from `live_count`.  Kept so the snapshot
    /// layout does not change.
    uint64_t live_bytes[NUM_SMALL_SIZECLASSES] = {};
    /// Live object count per small sizeclass on this thread.
    uint64_t live_count[NUM_SMALL_SIZECLASSES] = {};
    /// Cumulative allocations per small sizeclass.  Producers leave this at
    /// zero; stats_export.cc derives it from
    /// `live_count + cumulative_dealloc`, which holds once summed over the
    /// whole pool.
    uint64_t cumulative_alloc[NUM_SMALL_SIZECLASSES] = {};
    /// Cumulative deallocations per small sizeclass; only ever increases.
    /// Bumped by the freeing thread, which need not be the owning thread.
    uint64_t cumulative_dealloc[NUM_SMALL_SIZECLASSES] = {};

    /// Add another block's per-class counters into this one.
    void accumulate(const SizeClassStats& other) noexcept
    {
      for (size_t i = 0; i < NUM_SMALL_SIZECLASSES; i++)
      {
        live_bytes[i] += other.live_bytes[i];
        live_count[i] += other.live_count[i];
        cumulative_alloc[i] += other.cumulative_alloc[i];
        cumulative_dealloc[i] += other.cumulative_dealloc[i];
      }
    }
  };

  /**
   * Process-global per-size-class aggregator.  Same purpose and same
   * relaxed-atomic reasoning as `FrontendStatsGlobal`.
   */
  struct SizeClassStatsGlobal
  {
    stl::Atomic<uint64_t> live_bytes[NUM_SMALL_SIZECLASSES]{};
    stl::Atomic<uint64_t> live_count[NUM_SMALL_SIZECLASSES]{};
    stl::Atomic<uint64_t> cumulative_alloc[NUM_SMALL_SIZECLASSES]{};
    stl::Atomic<uint64_t> cumulative_dealloc[NUM_SMALL_SIZECLASSES]{};

    void drain_from(const SizeClassStats& s) noexcept
    {
      for (size_t i = 0; i < NUM_SMALL_SIZECLASSES; i++)
      {
        live_bytes[i].fetch_add(s.live_bytes[i], stl::memory_order_relaxed);
        live_count[i].fetch_add(s.live_count[i], stl::memory_order_relaxed);
        cumulative_alloc[i].fetch_add(
          s.cumulative_alloc[i], stl::memory_order_relaxed);
        cumulative_dealloc[i].fetch_add(
          s.cumulative_dealloc[i], stl::memory_order_relaxed);
      }
    }

    void snapshot_into(SizeClassStats& out) noexcept
    {
      for (size_t i = 0; i < NUM_SMALL_SIZECLASSES; i++)
      {
        out.live_bytes[i] += live_bytes[i].load(stl::memory_order_relaxed);
        out.live_count[i] += live_count[i].load(stl::memory_order_relaxed);
        out.cumulative_alloc[i] +=
          cumulative_alloc[i].load(stl::memory_order_relaxed);
        out.cumulative_dealloc[i] +=
          cumulative_dealloc[i].load(stl::memory_order_relaxed);
      }
    }
  };

  inline SizeClassStatsGlobal& size_class_stats_global() noexcept
  {
    static SizeClassStatsGlobal g;
    return g;
  }
#endif // SNMALLOC_STATS_FULL

  /**
   * An allocator's telemetry state, plus the hooks its hot paths call.
   *
   * The allocator only ever updates counters through these `on_*` methods.
   * When neither stats tier is enabled this is an empty struct with empty
   * methods, so the member costs no bytes and the calls compile away.
   */
  struct AllocStats
  {
#ifdef SNMALLOC_STATS_BASIC
    /// Frontend counters for this thread.
    FrontendStats frontend{};
#endif
#ifdef SNMALLOC_STATS_FULL
    /// Per-size-class histogram for this thread.
    SizeClassStats sizeclass{};
#endif

    /// One entry into the message-queue slow path (one drain attempt).
    SNMALLOC_FAST_PATH_INLINE void on_message_queue_drain() noexcept
    {
#ifdef SNMALLOC_STATS_BASIC
      frontend.message_queue_drains++;
#endif
    }

    /// One cross-thread message dequeued by this (destination) thread.
    SNMALLOC_FAST_PATH_INLINE void on_message_received() noexcept
    {
#ifdef SNMALLOC_STATS_BASIC
      frontend.cross_thread_messages_received++;
#endif
    }

    /// Small allocation served from an existing free list.
    SNMALLOC_FAST_PATH_INLINE void
    on_small_alloc_fast(smallsizeclass_t sizeclass_idx) noexcept
    {
#ifdef SNMALLOC_STATS_BASIC
      frontend.total_allocs++;
#  ifdef SNMALLOC_STATS_FULL
      sizeclass.live_count[sizeclass_idx]++;
#  else
      UNUSED(sizeclass_idx);
#  endif
#else
      UNUSED(sizeclass_idx);
#endif
    }

    /// Slab refill: one slow allocation.
    SNMALLOC_FAST_PATH_INLINE void
    on_small_refill(smallsizeclass_t sizeclass_idx) noexcept
    {
#ifdef SNMALLOC_STATS_BASIC
      frontend.total_allocs++;
      frontend.slow_path_allocs++;
#  ifdef SNMALLOC_STATS_FULL
      sizeclass.live_count[sizeclass_idx]++;
#  else
      UNUSED(sizeclass_idx);
#  endif
#else
      UNUSED(sizeclass_idx);
#endif
    }

    /// Dealloc of an object this thread owns, so both counters move here.
    SNMALLOC_FAST_PATH_INLINE void
    on_local_dealloc(sizeclass_t sc_full) noexcept
    {
#ifdef SNMALLOC_STATS_BASIC
      frontend.fast_path_deallocs++;
#  ifdef SNMALLOC_STATS_FULL
      // Only small sizeclasses are in the histogram.  `live_count` cannot
      // underflow here: this path only runs for objects allocated on this same
      // block, and cross-thread frees go to `on_remote_dealloc` instead.
      if (sc_full.is_small())
      {
        smallsizeclass_t sc = sc_full.as_small();
        sizeclass.cumulative_dealloc[sc]++;
        sizeclass.live_count[sc]--;
      }
#  else
      UNUSED(sc_full);
#  endif
#else
      UNUSED(sc_full);
#endif
    }

    /// Dealloc of an object owned by another allocator, routed through this
    /// thread's remote dealloc cache.  Only the dealloc count moves here; the
    /// owning thread applies the `live_count` decrement in `on_remote_ingest`.
    SNMALLOC_FAST_PATH_INLINE void
    on_remote_dealloc(sizeclass_t sc_full) noexcept
    {
#ifdef SNMALLOC_STATS_BASIC
      frontend.remote_deallocs++;
#  ifdef SNMALLOC_STATS_FULL
      if (sc_full.is_small())
      {
        sizeclass.cumulative_dealloc[sc_full.as_small()]++;
      }
#  else
      UNUSED(sc_full);
#  endif
#else
      UNUSED(sc_full);
#endif
    }

    /// Owning thread receiving a batch of cross-thread frees off the message
    /// queue.  `delta_bytes` is a byte total; the object count is that divided
    /// by the sizeclass object size.  Applies the `live_count` decrement that
    /// pairs with the freeing thread's `on_remote_dealloc`.
    SNMALLOC_FAST_PATH_INLINE void
    on_remote_ingest(sizeclass_t sc_full, size_t delta_bytes) noexcept
    {
#ifdef SNMALLOC_STATS_FULL
      if (sc_full.is_small())
      {
        smallsizeclass_t sc = sc_full.as_small();
        size_t objsize = sizeclass_full_to_size(sc_full);
        size_t length = delta_bytes / objsize;
        sizeclass.live_count[sc] -= length;
      }
#else
      UNUSED(sc_full, delta_bytes);
#endif
    }

    /// Move this thread's counters into the process-global aggregators and
    /// clear the local block, so the next thread to acquire this allocator
    /// starts from zero.
    ///
    /// Must only be called from `ThreadAlloc::teardown`, just before the
    /// allocator returns to the pool.  Calling it from `flush()` would zero
    /// the counters of a still-running thread.
    void drain_to_global() noexcept
    {
#ifdef SNMALLOC_STATS_BASIC
      frontend_stats_global().drain_from(frontend);
      frontend = FrontendStats{};
#  ifdef SNMALLOC_STATS_FULL
      size_class_stats_global().drain_from(sizeclass);
      sizeclass = SizeClassStats{};
#  endif
#endif
    }
  };
} // namespace snmalloc
