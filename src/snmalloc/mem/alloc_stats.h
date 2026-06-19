#pragma once

// Allocator-side telemetry counters and their per-Allocator hook surface.
//
// All of the stats state and every counter mutation lives here rather than
// in corealloc.h.  The allocator hot paths call the `AllocStats` member hooks
// (`on_small_refill`, `on_local_dealloc`, ...) by name; when the stats build
// flags are off those hooks are empty and `AllocStats` is an empty type, so
// the calls inline away to nothing and the member adds no per-Allocator bytes
// (it is declared `[[no_unique_address]]`).
//
// Two independent build tiers gate the cost:
//   SNMALLOC_STATS_BASIC  cheap per-thread frontend cache counters.
//   SNMALLOC_STATS_FULL   adds the per-size-class live/cumulative histogram.
//                         FULL implies BASIC (enforced in CMakeLists.txt).
//
// Counters are mutated only on the owning thread, so they are plain
// non-atomic `uint64_t` loads/stores on the hot path.  Cross-thread reads go
// through `snmalloc_get_full_stats`, which walks the live allocator pool and
// adds in the process-global aggregators below (those collect the counters of
// allocators that have already been returned to the pool at thread teardown,
// which no longer appear in the pool walk).

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
   * Per-thread frontend cache counters, embedded in every `Allocator`.
   *
   * Aligned to its own cache line so the hot-path counter store never dirties
   * a line shared with adjacent `Allocator` members (false sharing there cost
   * an extra cache-line transition per allocation).
   */
  struct alignas(CACHELINE_SIZE) FrontendStats
  {
    /**
     * Combined alloc counter: the cumulative-alloc total (low 48 bits) and
     * the slow-path call count (high 16 bits) packed into one 64-bit word, so
     * the `small_refill` slow path credits both with a single store rather
     * than two adjacent load-modify-stores.
     *
     * Decoded back into the public `fast_path_allocs` / `slow_path_allocs`
     * fields at snapshot time in stats_export.cc, so the `FullAllocStats` ABI
     * is unchanged.
     *
     * The two lanes occupy disjoint bit ranges, so a plain `+=` accumulates
     * each independently as long as neither overflows its sub-field width.
     * The 16-bit slow lane saturates at 65535 refills (~16M allocs for the
     * smallest sizeclasses) -- effectively unbounded for any real workload,
     * and well clear of the 48-bit total lane so the packed `+=` never carries
     * from the low lane into the high one.
     */
    uint64_t packed_allocs{0};

    /// Bit position of the slow-call lane within `packed_allocs`.
    static constexpr uint64_t PACKED_ALLOCS_SLOW_SHIFT = 48;
    /// Mask covering the low (total-alloc) lane of `packed_allocs`.
    static constexpr uint64_t PACKED_ALLOCS_TOTAL_MASK =
      (uint64_t{1} << PACKED_ALLOCS_SLOW_SHIFT) - 1;
    /// Pre-packed `+1` in the slow-call lane; added to `refill_count` at the
    /// refill site so one 64-bit add updates both lanes.
    static constexpr uint64_t PACKED_ALLOCS_SLOW_INC = uint64_t{1}
      << PACKED_ALLOCS_SLOW_SHIFT;

    /// Slow-path call count decoded from `packed_allocs`.
    [[nodiscard]] uint64_t slow_path_allocs() const noexcept
    {
      return packed_allocs >> PACKED_ALLOCS_SLOW_SHIFT;
    }

    /// Cumulative-alloc total (fast + slow) decoded from `packed_allocs`.
    [[nodiscard]] uint64_t total_allocs() const noexcept
    {
      return packed_allocs & PACKED_ALLOCS_TOTAL_MASK;
    }

    /// Fast-path alloc count, i.e. `total_allocs() - slow_path_allocs()`.
    [[nodiscard]] uint64_t fast_path_allocs() const noexcept
    {
      return total_allocs() - slow_path_allocs();
    }

    /**
     * Deallocations whose pagemap entry pointed at this allocator (the "local"
     * branch of `Allocator::dealloc`).
     *
     * Pre-credited at slab refill rather than bumped per-dealloc, mirroring
     * the batched alloc counter: every object handed onto a thread's fast free
     * list is assumed to be freed locally, so the credit fires at the same
     * site as the `packed_allocs` refill credit.  Cross-thread frees instead
     * bump `remote_deallocs`, so this counter over-credits by the
     * cross-thread-freed portion; the drift is bounded by program behaviour
     * and acceptable for an observability surface.
     */
    uint64_t fast_path_deallocs{0};
    /// Deallocations whose pagemap entry pointed at a remote allocator; routed
    /// through the remote dealloc cache.
    uint64_t remote_deallocs{0};
    /// Number of times this thread drained its incoming message queue.
    uint64_t message_queue_drains{0};
    /// Cross-thread messages dequeued by this thread.
    uint64_t cross_thread_messages_received{0};

    /// Add another snapshot's counters into this one.  Used by the snapshot
    /// aggregator and by the thread-exit drain.
    void accumulate(const FrontendStats& other) noexcept
    {
      // The high 16 bits (slow-call count) and low 48 bits (cumulative total)
      // live in disjoint bit ranges, so a plain `+=` accumulates each lane
      // independently provided neither overflows its sub-field width.
      packed_allocs += other.packed_allocs;
      fast_path_deallocs += other.fast_path_deallocs;
      remote_deallocs += other.remote_deallocs;
      message_queue_drains += other.message_queue_drains;
      cross_thread_messages_received += other.cross_thread_messages_received;
    }
  };

  /**
   * Process-global aggregator that collects a thread's `FrontendStats` at
   * teardown.  Exited threads no longer appear in `AllocPool::iterate()`, so
   * without this drain their counters would vanish from the snapshot.  The
   * counters are `Atomic` so the producer-side `fetch_add` at teardown is safe
   * against the consumer-side read in `snmalloc_get_full_stats`; relaxed
   * ordering suffices because the snapshot does not participate in any
   * happens-before chain with allocator state.
   */
  struct FrontendStatsGlobal
  {
    stl::Atomic<uint64_t> packed_allocs{0};
    stl::Atomic<uint64_t> fast_path_deallocs{0};
    stl::Atomic<uint64_t> remote_deallocs{0};
    stl::Atomic<uint64_t> message_queue_drains{0};
    stl::Atomic<uint64_t> cross_thread_messages_received{0};

    void drain_from(const FrontendStats& s) noexcept
    {
      packed_allocs.fetch_add(s.packed_allocs, stl::memory_order_relaxed);
      fast_path_deallocs.fetch_add(
        s.fast_path_deallocs, stl::memory_order_relaxed);
      remote_deallocs.fetch_add(s.remote_deallocs, stl::memory_order_relaxed);
      message_queue_drains.fetch_add(
        s.message_queue_drains, stl::memory_order_relaxed);
      cross_thread_messages_received.fetch_add(
        s.cross_thread_messages_received, stl::memory_order_relaxed);
    }

    void snapshot_into(FrontendStats& out) const noexcept
    {
      out.packed_allocs += packed_allocs.load(stl::memory_order_relaxed);
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
   * Per-thread per-small-sizeclass histogram, embedded in every `Allocator`
   * alongside `FrontendStats`.  Arrays are indexed by `smallsizeclass_t` and
   * mutated only on the owning thread.
   *
   * Byte / count deltas are tracked so cross-thread frees net out when summed
   * across the pool: the freeing thread bumps `cumulative_dealloc[sc]` on its
   * own block, while the owning thread's `live_*[sc]` decrement happens on the
   * block that recorded the alloc (slab-local fast dealloc, or message-queue
   * drain).
   *
   * `cumulative_alloc[sc]` is not maintained on the hot path: it is derived at
   * snapshot time from `live_count[sc] + cumulative_dealloc[sc]`, which holds
   * because each alloc/dealloc pair conserves
   * `cumulative_alloc - cumulative_dealloc = live_count` per class once summed
   * across the pool.  The field is retained for output stability and left at
   * zero by the producer paths.
   *
   * Aligned to its own cache line for the same false-sharing reason as
   * `FrontendStats`.
   */
  struct alignas(CACHELINE_SIZE) SizeClassStats
  {
    /// Live byte total per small sizeclass.  Not maintained on the hot path;
    /// derived at snapshot time from `live_count[sc] * sizeclass_to_size(sc)`
    /// (every object in a small sizeclass has the same size).  Retained for
    /// output-layout stability; left at zero by the producer paths.
    uint64_t live_bytes[NUM_SMALL_SIZECLASSES] = {};
    /// Live object count per small sizeclass on this thread.  The only
    /// per-class quantity maintained on the alloc/dealloc hot path.
    uint64_t live_count[NUM_SMALL_SIZECLASSES] = {};
    /// Derived at snapshot time (see struct doc); left at zero by producers.
    uint64_t cumulative_alloc[NUM_SMALL_SIZECLASSES] = {};
    /// Cumulative deallocations per small sizeclass (monotone).  Bumped on the
    /// freeing thread, which may or may not be the owning thread.
    uint64_t cumulative_dealloc[NUM_SMALL_SIZECLASSES] = {};

    /// Add another snapshot's per-class counters into this one.
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
   * Process-global per-size-class aggregator, symmetric to
   * `FrontendStatsGlobal`.  Same atomic / relaxed-ordering rationale.
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

    void snapshot_into(SizeClassStats& out) const noexcept
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
   * Per-Allocator telemetry state plus the hook surface the allocator hot
   * paths call into.
   *
   * Each `on_*` method is the single point where the corresponding counters
   * are mutated; the allocator calls them by name and never touches the
   * counter fields directly.  When neither stats tier is enabled this is an
   * empty struct with empty (inlined-away) methods, so an
   * `[[no_unique_address]]` `AllocStats` member costs zero bytes and the
   * hot-path calls compile to nothing.
   */
  struct AllocStats
  {
#ifdef SNMALLOC_STATS_BASIC
    /// Frontend cache counters for this thread.
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

    /// Fast-path small allocation served from an existing free list.  The
    /// alloc-count credit is batched at refill time; only the per-class live
    /// counters move here.
    SNMALLOC_FAST_PATH_INLINE void
    on_small_alloc_fast(smallsizeclass_t sizeclass_idx) noexcept
    {
#ifdef SNMALLOC_STATS_FULL
      sizeclass.live_count[sizeclass_idx]++;
#else
      UNUSED(sizeclass_idx);
#endif
    }

    /// Slow-path slab refill that handed `refill_count` objects onto the fast
    /// free list (one returned to the caller, the rest credited ahead of their
    /// future fast-path allocs).  One packed store credits both the cumulative
    /// total and the slow-path call count; `fast_path_deallocs` is pre-credited
    /// symmetrically.
    SNMALLOC_FAST_PATH_INLINE void on_small_refill(
      smallsizeclass_t sizeclass_idx, uint16_t refill_count) noexcept
    {
#ifdef SNMALLOC_STATS_BASIC
      frontend.packed_allocs += static_cast<uint64_t>(refill_count) +
        FrontendStats::PACKED_ALLOCS_SLOW_INC;
      frontend.fast_path_deallocs += refill_count;
#  ifdef SNMALLOC_STATS_FULL
      sizeclass.live_count[sizeclass_idx]++;
#  else
      UNUSED(sizeclass_idx);
#  endif
#else
      UNUSED(sizeclass_idx, refill_count);
#endif
    }

    /// Local-owner dealloc fast path: the freed object's slab is owned by this
    /// thread, so both the cumulative-free and live counters move here.
    SNMALLOC_FAST_PATH_INLINE void
    on_local_dealloc(sizeclass_t sc_full) noexcept
    {
#ifdef SNMALLOC_STATS_FULL
      // Only small sizeclasses are tracked in the histogram.  live_count
      // cannot underflow: every local-fast-path dealloc pairs with a prior
      // alloc on this same per-thread block (cross-thread frees take the
      // remote path below).
      if (sc_full.is_small())
      {
        smallsizeclass_t sc = sc_full.as_small();
        sizeclass.cumulative_dealloc[sc]++;
        sizeclass.live_count[sc]--;
      }
#else
      UNUSED(sc_full);
#endif
    }

    /// Cross-allocator dealloc: the freed object is owned by another thread,
    /// so it is routed through this thread's remote dealloc cache.  The
    /// cumulative-free count is credited on this (freeing) thread; the paired
    /// live decrement happens on the owning thread in `on_remote_ingest`.
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

    /// Owning-thread ingest of a batch of cross-thread frees from the message
    /// queue.  `delta_bytes` is the byte total this message returned; the
    /// object count is recovered by dividing by the class object size.  This
    /// applies the live decrement that pairs with the `on_remote_dealloc`
    /// cumulative bump the freeing thread made.
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

    /// Drain this thread's counters into the process-global aggregators and
    /// reset the local block.  Called from `ThreadAlloc::teardown` after the
    /// allocator is about to be released back to the pool, so the next thread
    /// to acquire it starts clean.  Deliberately not called from `flush()`,
    /// which also runs on live threads and would erase counters mid-lifetime.
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
