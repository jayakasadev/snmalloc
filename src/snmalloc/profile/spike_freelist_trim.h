#pragma once

#include "../ds/sizeclasstable.h"
#include "../ds_core/defines.h"
#include "sampler.h"
#ifdef SNMALLOC_PROFILE_SECONDARY_STORAGE
#  include "spike_secondary.h"
#endif

#include <climits>
#include <cstddef>
#include <cstdint>

namespace snmalloc::profile::spike
{
  class RefillSampler
  {
    Sampler small_samplers_[NUM_SMALL_SIZECLASSES]{};
    Sampler large_sampler_{};
    int64_t small_countdowns_[NUM_SMALL_SIZECLASSES]{};
    int64_t large_countdown_{0};
    SampledAlloc* pending_main_sample_{nullptr};

    SampledAlloc* retain_sample(SampledAlloc* node) noexcept
    {
      if (node == nullptr)
        return nullptr;
#ifdef SNMALLOC_PROFILE_SECONDARY_STORAGE
      ProfileSecondaryAllocator::cancel_pending();
      node->alloc_ts_ns = static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
      pending_secondary_sample.node = node;
#else
      SNMALLOC_ASSERT(pending_main_sample_ == nullptr);
      pending_main_sample_ = node;
#endif
      return node;
    }

    static void seed(Sampler& sampler, int64_t& countdown) noexcept
    {
      if (sampler.debug_initialized())
        return;

      const size_t rate = Sampler::get_sampling_rate();
      if (rate == 0)
      {
        countdown = INT64_MAX / 2;
        return;
      }

      if (countdown == 0 || countdown == INT64_MAX / 2)
      {
        countdown = 0;
        (void)sampler.record_alloc_from_namespace_tls(
          0, 0, 0, countdown, false);
      }
    }

  public:
    [[nodiscard]] static size_t ordinary_allocations_before_sample(
      int64_t countdown, size_t object_size) noexcept
    {
      if (countdown <= 0 || object_size == 0)
        return 0;
      return static_cast<size_t>(
        (countdown - 1) / static_cast<int64_t>(object_size));
    }

    [[nodiscard]] size_t
    transfer_limit(smallsizeclass_t sizeclass, size_t object_size) noexcept
    {
      int64_t& countdown = small_countdowns_[sizeclass];
      seed(small_samplers_[sizeclass], countdown);
      if (countdown <= 0 && sampler_reentered())
        return 1;
      return ordinary_allocations_before_sample(countdown, object_size);
    }

    void debit(
      smallsizeclass_t sizeclass,
      size_t object_size,
      size_t transferred) noexcept
    {
      small_countdowns_[sizeclass] -=
        static_cast<int64_t>(object_size * transferred);
    }

    SampledAlloc* prepare_small_sample(
      smallsizeclass_t sizeclass, size_t object_size) noexcept
    {
#ifndef SNMALLOC_PROFILE_SECONDARY_STORAGE
      if (pending_main_sample_ != nullptr)
        return pending_main_sample_;
#endif
      int64_t& countdown = small_countdowns_[sizeclass];
      Sampler& sampler = small_samplers_[sizeclass];
      seed(sampler, countdown);
      if (countdown > static_cast<int64_t>(object_size))
        return nullptr;
      countdown -= static_cast<int64_t>(object_size);
      const bool fired = sampler.record_alloc_from_namespace_tls(
        0, object_size, object_size, countdown, false);
      return fired ? retain_sample(sampler.last_sample()) : nullptr;
    }

    SampledAlloc*
    prepare_large_sample(size_t requested, size_t allocated) noexcept
    {
#ifndef SNMALLOC_PROFILE_SECONDARY_STORAGE
      if (pending_main_sample_ != nullptr)
        return pending_main_sample_;
#endif
      seed(large_sampler_, large_countdown_);
      large_countdown_ -= static_cast<int64_t>(requested);
      if (large_countdown_ > 0)
        return nullptr;
      const bool fired = large_sampler_.record_alloc_from_namespace_tls(
        0, requested, allocated, large_countdown_, false);
      if (fired)
      {
        SampledAlloc* node = large_sampler_.last_sample();
        if (node != nullptr)
          node->requested_size.store(requested, std::memory_order_relaxed);
        return retain_sample(node);
      }
      return nullptr;
    }

    SampledAlloc* take_main_sample() noexcept
    {
      return stl::exchange(pending_main_sample_, nullptr);
    }

    void debug_set_small_countdown(
      smallsizeclass_t sizeclass, int64_t countdown) noexcept
    {
      small_countdowns_[sizeclass] = countdown;
    }

    void debug_seed_small(smallsizeclass_t sizeclass) noexcept
    {
      seed(small_samplers_[sizeclass], small_countdowns_[sizeclass]);
    }

    [[nodiscard]] int64_t
    debug_small_countdown(smallsizeclass_t sizeclass) const noexcept
    {
      return small_countdowns_[sizeclass];
    }
  };

  inline thread_local RefillSampler refill_sampler;
} // namespace snmalloc::profile::spike
