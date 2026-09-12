// SPDX-License-Identifier: MIT
//
// Implementation of `snmalloc_get_full_stats`, declared in
// `src/snmalloc/global/stats_export.h`.  Read-only: mutates no allocator
// state.

#include "snmalloc/global/stats_export.h"

#include "rust_config.h"
#include "../snmalloc.h"

// SNMALLOC_PROFILE produces the lifetime histogram; SNMALLOC_STATS_FULL is
// what exposes it here.  Both are required.
#if defined(SNMALLOC_PROFILE) && defined(SNMALLOC_STATS_FULL)
#  include "snmalloc/profile/lifetime_histogram.h"
#endif

#include <string.h>

using namespace snmalloc;

extern "C" SNMALLOC_EXPORT void
snmalloc_get_full_stats(struct snmalloc_full_stats* out)
{
  if (out == nullptr)
    return;

  // Zero everything first so unpopulated fields, including unused
  // `reserved[]` slots, read as zero.
  memset(out, 0, sizeof(*out));

  out->version = SNMALLOC_FULL_STATS_VERSION;

  // Same `StatsRange` counters that `sn_rust_statistics` and
  // `get_malloc_info_v1` report.
  out->bytes_in_use =
    static_cast<uint64_t>(Alloc::Config::Backend::get_current_usage());
  out->peak_bytes_in_use =
    static_cast<uint64_t>(Alloc::Config::Backend::get_peak_usage());

  // `bytes_mapped` equals `bytes_in_use` because snmalloc only maps memory it
  // also holds a backend reservation for.  The commit figures come from
  // `BackendFragCounters`, which `CommitRange<PAL>` maintains.
  out->bytes_mapped = out->bytes_in_use;
  {
    auto frag = snmalloc::get_backend_frag_stats();
    out->bytes_committed = frag.bytes_committed;
    out->bytes_decommitted_to_os = frag.bytes_decommitted_to_os;

    // The free-chunk histogram occupies the first
    // `SNMALLOC_FULL_STATS_FREECHUNK_BUCKETS` slots of `reserved[]`.
    static_assert(
      SNMALLOC_FULL_STATS_FREECHUNK_BUCKETS <=
        SNMALLOC_FULL_STATS_RESERVED_SLOTS,
      "Free-chunk histogram must fit in reserved[] slot pool.");
    static_assert(
      static_cast<size_t>(SNMALLOC_FULL_STATS_FREECHUNK_BUCKETS) ==
        snmalloc::LargeBuddyFreeChunkHistogram::NUM_BUCKETS,
      "Free-chunk histogram bucket count must match the C ABI macro.");
    for (size_t i = 0; i < SNMALLOC_FULL_STATS_FREECHUNK_BUCKETS; ++i)
    {
      out->reserved[i] = frag.free_chunk_count_by_log_size[i];
    }
  }

  // Lifetime histogram, filled in on the dealloc path of a sampled
  // allocation.  Guarded so a non-profile build does not have to link the
  // singleton accessor at all; those builds leave the field zero.
#if defined(SNMALLOC_PROFILE) && defined(SNMALLOC_STATS_FULL)
  {
    auto& hist = snmalloc::profile::LifetimeHistogram::get();
    static_assert(
      snmalloc::profile::kLifetimeBuckets ==
        SNMALLOC_FULL_STATS_LIFETIME_BUCKETS,
      "LifetimeHistogram bucket count must match "
      "SNMALLOC_FULL_STATS_LIFETIME_BUCKETS");
    for (size_t i = 0; i < SNMALLOC_FULL_STATS_LIFETIME_BUCKETS; ++i)
      out->lifetime_buckets_ns[i] = hist.bucket(i);
  }
#endif

#ifdef SNMALLOC_STATS_BASIC
  // Sum the counters of every allocator still in the pool, then add the
  // process-global aggregators, which hold the counters of threads that have
  // already exited.
  //
  // The per-class arrays only exist in a FULL build, so their aggregation is
  // nested under the FULL guard below.
  {
    FrontendStats agg{};
#  ifdef SNMALLOC_STATS_FULL
    SizeClassStats sc_agg{};
#  endif
    using AllocT = Allocator<Alloc::Config>;
    for (AllocT* a = AllocPool<Alloc::Config>::iterate(); a != nullptr;
         a = AllocPool<Alloc::Config>::iterate(a))
    {
      agg.accumulate(a->alloc_stats.frontend);
#  ifdef SNMALLOC_STATS_FULL
      sc_agg.accumulate(a->alloc_stats.sizeclass);
#  endif
    }
    frontend_stats_global().snapshot_into(agg);
#  ifdef SNMALLOC_STATS_FULL
    size_class_stats_global().snapshot_into(sc_agg);
#  endif

    out->fast_path_allocs = agg.fast_path_allocs();
    out->slow_path_allocs =
      agg.slow_path_allocs.load(stl::memory_order_relaxed);
    out->fast_path_deallocs =
      agg.fast_path_deallocs.load(stl::memory_order_relaxed);
    out->remote_deallocs =
      agg.remote_deallocs.load(stl::memory_order_relaxed);
    out->message_queue_drains =
      agg.message_queue_drains.load(stl::memory_order_relaxed);
    out->cross_thread_messages_received =
      agg.cross_thread_messages_received.load(stl::memory_order_relaxed);

#  ifdef SNMALLOC_STATS_FULL
    // Copy the per-class arrays out.  Slots past `NUM_SMALL_SIZECLASSES` are
    // left zero by the `memset` above.
    static_assert(
      NUM_SMALL_SIZECLASSES <= SNMALLOC_FULL_STATS_SIZECLASS_SLOTS,
      "Per-class histogram has fewer FFI slots than snmalloc's "
      "small-class count; bump SNMALLOC_FULL_STATS_SIZECLASS_SLOTS "
      "to keep the FullAllocStats wire format wide enough.");
    for (size_t i = 0; i < NUM_SMALL_SIZECLASSES; i++)
    {
      // Live bytes are not tracked on the hot path.  Every object in a small
      // sizeclass has the same size, so the product below is exact.
      const uint64_t live =
        sc_agg.live_count[i].load(stl::memory_order_relaxed);
      const uint64_t dealloc =
        sc_agg.cumulative_dealloc[i].load(stl::memory_order_relaxed);
      out->total_live_bytes_by_class[i] = live *
        sizeclass_to_size(static_cast<smallsizeclass_t>(i));
      out->total_live_count_by_class[i] = live;
      // Cumulative allocations are not tracked on the hot path either; they
      // follow from `live_count + cumulative_dealloc`.  A concurrent producer
      // can leave the two summands momentarily out of step, so the derived
      // total can be off by a few.
      out->cumulative_alloc_by_class[i] =
        live + dealloc;
      out->cumulative_dealloc_by_class[i] = dealloc;
    }
#  endif // SNMALLOC_STATS_FULL
  }
#endif // SNMALLOC_STATS_BASIC
}
