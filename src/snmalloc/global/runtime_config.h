// SPDX-License-Identifier: MIT
//
// Process-wide runtime tunables:
//
//   sample_interval_bytes   mean Poisson interval, in bytes, for the heap
//                           profiler.  Setting it also updates the sampler's
//                           own global via `Sampler::set_sampling_rate`.
//
//   decay_rate_ms           how long, in milliseconds, an unused chunk may sit
//                           in a backend cache before being returned to the
//                           OS.  Read by `LargeBuddyRange`.
//
//   max_local_cache_bytes   per-thread local-cache cap, in bytes.
//
// Each value lives in a function-local `std::atomic`, so it is constructed on
// first use and does not depend on global initialisation order.  Reads and
// writes are lock-free and safe from any thread at any time, including before
// the first allocation.
//
// `override/runtime_config.cc` exposes these as a C ABI for non-C++ callers.

#pragma once

#include <atomic>
#include <cstdint>

namespace snmalloc
{
  /**
   * Runtime-settable allocator tunables.  All methods are static; see the file
   * header for the contract.
   */
  class RuntimeConfig
  {
  public:
    /// Default mean sampling interval, in bytes.  Must stay equal to
    /// `snmalloc::profile::SamplerGlobals::kDefaultSamplingRate`, so that
    /// reading this tunable before anyone sets it reports what the sampler is
    /// really using.
    static constexpr uint64_t kDefaultSampleIntervalBytes =
      static_cast<uint64_t>(512) * 1024;

    /// Default decay window, in milliseconds.
    static constexpr uint32_t kDefaultDecayRateMs = 50u;

    /// Default per-thread local-cache cap, in bytes.
    static constexpr uint64_t kDefaultMaxLocalCacheBytes =
      static_cast<uint64_t>(1) * 1024 * 1024;

    /**
     * Get the mean sampling interval, in bytes.  Zero means sampling is
     * disabled.
     */
    [[nodiscard]] static uint64_t sample_interval_bytes() noexcept
    {
      return sample_interval_storage().load(std::memory_order_acquire);
    }

    /**
     * Set the mean sampling interval, in bytes.  Zero disables sampling.
     */
    static void set_sample_interval_bytes(uint64_t bytes) noexcept
    {
      sample_interval_storage().store(bytes, std::memory_order_release);
    }

    /**
     * Get the chunk decay window, in milliseconds.  Zero is valid and means
     * the backend returns chunks to the OS immediately.
     */
    [[nodiscard]] static uint32_t decay_rate_ms() noexcept
    {
      return decay_rate_storage().load(std::memory_order_acquire);
    }

    /**
     * Set the chunk decay window, in milliseconds.
     */
    static void set_decay_rate_ms(uint32_t milliseconds) noexcept
    {
      decay_rate_storage().store(milliseconds, std::memory_order_release);
    }

    /**
     * Get the per-thread local-cache cap, in bytes.
     */
    [[nodiscard]] static uint64_t max_local_cache_bytes() noexcept
    {
      return max_local_cache_storage().load(std::memory_order_acquire);
    }

    /**
     * Set the per-thread local-cache cap, in bytes.
     */
    static void set_max_local_cache_bytes(uint64_t bytes) noexcept
    {
      max_local_cache_storage().store(bytes, std::memory_order_release);
    }

  private:
    // Each atomic is a function-local static, so it is created by whichever
    // thread first reaches its accessor.  C++ guarantees that is thread-safe,
    // and it means there is no global construction order to get wrong.
    static std::atomic<uint64_t>& sample_interval_storage() noexcept
    {
      static std::atomic<uint64_t> v{kDefaultSampleIntervalBytes};
      return v;
    }

    static std::atomic<uint32_t>& decay_rate_storage() noexcept
    {
      static std::atomic<uint32_t> v{kDefaultDecayRateMs};
      return v;
    }

    static std::atomic<uint64_t>& max_local_cache_storage() noexcept
    {
      static std::atomic<uint64_t> v{kDefaultMaxLocalCacheBytes};
      return v;
    }
  };
} // namespace snmalloc
