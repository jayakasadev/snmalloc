// SPDX-License-Identifier: MIT
//
// Heap profiler -- log2 histogram of sampled-allocation lifetimes.
//
// A lifetime of `n` nanoseconds, measured from sample fire to free, bumps
// bucket `floor(log2(n))`. Bucket 0 covers 1-2ns and the last bucket
// saturates, covering ~2.1s and above.
//
// Every bump is a relaxed `fetch_add`, so a reader can see buckets that do
// not sum to a consistent total. That is acceptable for a histogram.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace snmalloc::profile
{
  /// Must match `SNMALLOC_FULL_STATS_LIFETIME_BUCKETS` in
  /// `global/stats_export.h`, which copies the histogram across the C ABI.
  inline constexpr size_t kLifetimeBuckets = 32;

  /**
   * One histogram per process, in static storage so the buckets survive
   * profiling being paused and resumed.
   */
  class LifetimeHistogram
  {
  public:
    LifetimeHistogram() noexcept = default;
    LifetimeHistogram(const LifetimeHistogram&) = delete;
    LifetimeHistogram& operator=(const LifetimeHistogram&) = delete;

    /// Built on first call and trivially destructible, so shutdown order
    /// does not matter.
    static LifetimeHistogram& get() noexcept
    {
      static LifetimeHistogram instance;
      return instance;
    }

    /**
     * Count a lifetime of `ns` nanoseconds.
     */
    void record_lifetime_ns(uint64_t ns) noexcept
    {
      const size_t bucket = bucket_for(ns);
      buckets_[bucket].fetch_add(1, std::memory_order_relaxed);
    }

    /// Count in bucket `i`, which must be less than `kLifetimeBuckets`.
    [[nodiscard]] uint64_t bucket(size_t i) const noexcept
    {
      return buckets_[i].load(std::memory_order_relaxed);
    }

    /**
     * Bucket for a lifetime of `ns` nanoseconds: `floor(log2(ns))`, with 0
     * and 1 both in bucket 0 and anything from 2^31 up in the last bucket.
     */
    [[nodiscard]] static size_t bucket_for(uint64_t ns) noexcept
    {
      if (ns <= 1)
        return 0;
        // floor(log2(ns)) as 63 - clz; ns is at least 2 here, and clz is
        // undefined for 0.
#if defined(_MSC_VER)
      unsigned long index = 0;
      _BitScanReverse64(&index, ns);
      const size_t b = static_cast<size_t>(index);
#else
      const size_t b = static_cast<size_t>(63 - __builtin_clzll(ns));
#endif
      return b >= kLifetimeBuckets ? (kLifetimeBuckets - 1) : b;
    }

  private:
    std::atomic<uint64_t> buckets_[kLifetimeBuckets]{};
  };
} // namespace snmalloc::profile
