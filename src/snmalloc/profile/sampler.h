// SPDX-License-Identifier: MIT
//
// Heap profiler -- per-thread Poisson sampler.
//
// One sample fires per ~`sampling_rate` bytes of requested memory. Each
// thread keeps a byte countdown: the fast path subtracts the request size and
// branches, and the slow path draws the next interval from Exp(rate), claims
// a node, captures a stack, and publishes the sample.

#pragma once

#include "../ds_core/defines.h"
#include "../pal/pal_stack_walker.h"
#include "node_pool.h"
#include "reentrancy_guard.h"
#include "sampled_alloc.h"
#include "sampled_list.h"

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>

#if defined(__x86_64__) || defined(_M_X64)
#  if defined(_MSC_VER)
#    include <intrin.h>
#  else
#    include <x86intrin.h>
#  endif
#endif

// Alignment for the per-thread counter so it does not false-share with
// neighbouring data. Apple Silicon uses 128-byte lines, elsewhere 64.
#ifndef SNMALLOC_CACHE_LINE_SIZE
#  if defined(__APPLE__) && defined(__aarch64__)
#    define SNMALLOC_CACHE_LINE_SIZE 128
#  else
#    define SNMALLOC_CACHE_LINE_SIZE 64
#  endif
#endif

namespace snmalloc::profile
{
  /**
   * Per-thread countdown, in bytes of request, driving the alloc fast path.
   *
   * `0` means "not yet seeded": the fast path's `<= 0` branch sends the first
   * allocation on a thread into the slow path, which draws an Exp(rate)
   * interval and seeds the counter.
   */
  inline thread_local int64_t bytes_until_sample = 0;

  /**
   * State shared by every thread's sampler. `inline` so all translation units
   * see one copy.
   */
  struct SamplerGlobals
  {
    /// Mean sampling interval, in bytes.
    static constexpr size_t kDefaultSamplingRate = 512 * 1024;

    static std::atomic<size_t>& sampling_rate() noexcept
    {
      static std::atomic<size_t> rate{kDefaultSamplingRate};
      return rate;
    }

    /// One node pool per process.
    static NodePool<>& pool() noexcept
    {
      static NodePool<> p;
      return p;
    }

    /// One list of live samples per process.
    static SampledList& list() noexcept
    {
      static SampledList l;
      return l;
    }

    /// Mixed into each thread's PRNG seed so threads diverge.
    static std::atomic<uint64_t>& thread_salt() noexcept
    {
      static std::atomic<uint64_t> salt{0xDEADBEEFCAFEBABEULL};
      return salt;
    }
  };

  /**
   * Per-thread Poisson sampler. One instance per thread; the counter and PRNG
   * state are not shared.
   */
  class Sampler
  {
  public:
    Sampler() noexcept = default;
    Sampler(const Sampler&) = delete;
    Sampler& operator=(const Sampler&) = delete;

    /**
     * Returns true if this allocation was sampled, in which case
     * `last_sample()` gives the published node.
     *
     * The caller does not own that node: it stays on the global SampledList
     * until the matching dealloc hook removes it.
     */
    SNMALLOC_FAST_PATH_INLINE bool record_alloc(
      uintptr_t alloc_addr,
      size_t requested_size,
      size_t allocated_size) noexcept
    {
      // Re-entrancy is checked in the slow path, not here. Under re-entry the
      // countdown is allowed to run negative; the slow path then bails
      // without resetting it, so the next sample fires as soon as the outer
      // slow path finishes. The weight maths accounts for the overshoot.
      hot_.bytes_until_sample -= static_cast<int64_t>(requested_size);
      if (SNMALLOC_LIKELY(hot_.bytes_until_sample > 0))
      {
        last_sample_ = nullptr;
        return false;
      }
      return record_alloc_slow(alloc_addr, requested_size, allocated_size);
    }

    SNMALLOC_FAST_PATH_INLINE bool record_alloc(size_t requested_size) noexcept
    {
      return record_alloc(0, requested_size, requested_size);
    }

    /**
     * Slow path for the namespace-scope counter used by `tl_record_alloc`.
     *
     * The caller must already have subtracted `requested_size` from
     * `counter_inout` and seen it go non-positive. The next interval is
     * written back through `counter_inout` so the fast path can resume.
     */
    SNMALLOC_SLOW_PATH bool record_alloc_from_namespace_tls(
      uintptr_t alloc_addr,
      size_t requested_size,
      size_t allocated_size,
      int64_t& counter_inout) noexcept
    {
      hot_.bytes_until_sample = counter_inout;
      const bool fired =
        record_alloc_slow(alloc_addr, requested_size, allocated_size);
      counter_inout = hot_.bytes_until_sample;
      return fired;
    }

    /**
     * Weight of the most recent sample, in bytes of request. Only valid
     * immediately after `record_alloc` returned true.
     */
    [[nodiscard]] uint64_t last_weight() const noexcept
    {
      return weight_;
    }

    /**
     * Sampling interval in bytes that was in force when the last sample
     * fired.
     */
    [[nodiscard]] uint64_t last_interval() const noexcept
    {
      return interval_at_capture_;
    }

    /**
     * The node just published, or nullptr if the last `record_alloc` did not
     * sample or the pool was empty.
     */
    [[nodiscard]] SampledAlloc* last_sample() const noexcept
    {
      return last_sample_;
    }

    /// Test-only: current value of this sampler's countdown.
    [[nodiscard]] int64_t debug_bytes_until_sample() const noexcept
    {
      return hot_.bytes_until_sample;
    }

    /// Test-only: true once the slow path has seeded this sampler.
    [[nodiscard]] bool debug_initialized() const noexcept
    {
      return interval_at_capture_ != 0;
    }

    /**
     * Set the global mean sampling interval in bytes; 0 disables sampling.
     * Per-thread countdowns are not redrawn, so a change takes effect at each
     * thread's next slow-path entry.
     */
    static void set_sampling_rate(size_t bytes) noexcept
    {
      SamplerGlobals::sampling_rate().store(bytes, std::memory_order_relaxed);
    }

    [[nodiscard]] static size_t get_sampling_rate() noexcept
    {
      return SamplerGlobals::sampling_rate().load(std::memory_order_relaxed);
    }

  private:
    SNMALLOC_SLOW_PATH bool record_alloc_slow(
      uintptr_t alloc_addr,
      size_t requested_size,
      size_t allocated_size) noexcept
    {
      // Bail if this thread is already inside the sampler, e.g. because the
      // stack walker allocated on first use. The counter stays negative, so
      // the next allocation comes back here; re-entry is bounded by the
      // outer slow-path frame.
      if (SNMALLOC_UNLIKELY(sampler_reentered()))
      {
        last_sample_ = nullptr;
        return false;
      }

      const uint64_t rate =
        SamplerGlobals::sampling_rate().load(std::memory_order_relaxed);
      if (SNMALLOC_UNLIKELY(rate == 0))
      {
        // Sampling disabled: park the counter far in the future so the fast
        // path stops coming here. `interval_at_capture_` is left alone, so
        // re-enabling sampling still seeds via the branch below.
        hot_.bytes_until_sample = INT64_MAX / 2;
        last_sample_ = nullptr;
        return false;
      }

      // A zero `interval_at_capture_` means this sampler has never been
      // seeded; `rate` is non-zero here, so setting it below also marks the
      // sampler as seeded.
      if (SNMALLOC_UNLIKELY(interval_at_capture_ == 0))
      {
        // Draw the first countdown from Exp(rate) rather than sampling the
        // first allocation outright, which would bias the estimator.
        seed_prng_if_needed();
        hot_.bytes_until_sample = draw_exponential(rate, prng_step()) -
          static_cast<int64_t>(requested_size);
        interval_at_capture_ = rate;
        if (hot_.bytes_until_sample > 0)
        {
          last_sample_ = nullptr;
          return false;
        }
        // This first request is big enough to cross the threshold on its own,
        // so fall through and fire a sample.
      }

      // Weight in bytes of request, counting the overshoot: the counter is
      // <= 0 here, so this is rate + requested_size + (-counter).
      weight_ = static_cast<uint64_t>(
        static_cast<int64_t>(rate) -
        static_cast<int64_t>(hot_.bytes_until_sample) +
        static_cast<int64_t>(requested_size));
      interval_at_capture_ = rate;

      // `+=`, not `=`, so the overshoot carries into the next interval.
      hot_.bytes_until_sample += draw_exponential(rate, prng_step());

      // Guard the work below: anything it allocates re-enters this function,
      // sees the flag, and bails.
      ReentrancyGuard guard;

      SampledAlloc* node = SamplerGlobals::pool().acquire();
      if (SNMALLOC_UNLIKELY(node == nullptr))
      {
        // Pool empty; the pool itself counts the drop.
        last_sample_ = nullptr;
        return true; // sample fired, just not recorded
      }

      node->alloc_addr = alloc_addr;
      node->requested_size.store(requested_size, std::memory_order_relaxed);
      node->allocated_size.store(allocated_size, std::memory_order_relaxed);
      node->weight = weight_;
      node->sample_interval_at_capture = interval_at_capture_;
      node->tid = current_tid();

      // Skip one frame so this function does not appear in the trace.
      node->stack_depth = static_cast<uint8_t>(
        snmalloc::profile::stack_walk(node->stack, MaxStackFrames, 1));

      SamplerGlobals::list().push(node);
      last_sample_ = node;
      return true;
    }

    // xoshiro256** step.
    SNMALLOC_FAST_PATH_INLINE uint64_t prng_step() noexcept
    {
      const uint64_t result = rotl(s_[1] * 5, 7) * 9;
      const uint64_t t = s_[1] << 17;
      s_[2] ^= s_[0];
      s_[3] ^= s_[1];
      s_[1] ^= s_[2];
      s_[0] ^= s_[3];
      s_[2] ^= t;
      s_[3] = rotl(s_[3], 45);
      // Never return 0: `draw_exponential` requires a non-zero draw.
      return result | 1;
    }

    static constexpr uint64_t rotl(uint64_t x, int k) noexcept
    {
      return (x << k) | (x >> (64 - k));
    }

    void seed_prng_if_needed() noexcept
    {
      if (SNMALLOC_LIKELY((s_[0] | s_[1] | s_[2] | s_[3]) != 0))
        return;
      const uint64_t a = read_cycle_counter();
      const uint64_t b = reinterpret_cast<uintptr_t>(&a);
      const uint64_t c = SamplerGlobals::thread_salt().fetch_add(
        0x9E3779B97F4A7C15ULL, std::memory_order_relaxed);
      // SplitMix64 expansion to four words.
      uint64_t z = a ^ b ^ c;
      // A zero seed would make every mix collapse to zero.
      if (z == 0)
        z = 0x9E3779B97F4A7C15ULL;
      for (int i = 0; i < 4; ++i)
      {
        z += 0x9E3779B97F4A7C15ULL;
        uint64_t y = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        y = (y ^ (y >> 27)) * 0x94D049BB133111EBULL;
        s_[i] = y ^ (y >> 31);
      }
      if ((s_[0] | s_[1] | s_[2] | s_[3]) == 0)
        s_[0] = 1;
    }

    static uint64_t read_cycle_counter() noexcept
    {
#if defined(__x86_64__) || defined(_M_X64)
      return static_cast<uint64_t>(__rdtsc());
#elif defined(__aarch64__)
      uint64_t v;
      __asm__ volatile("mrs %0, cntvct_el0" : "=r"(v));
      return v;
#else
      // No cycle counter here: use the address of a thread-local, which only
      // has to differ between threads.
      thread_local uint64_t entropy = 0;
      return reinterpret_cast<uintptr_t>(&entropy);
#endif
    }

    /**
     * Draw X ~ Exp(mean) in bytes from a uniform `r != 0`, using
     * X = -mean * ln(U) with U = (r >> 11) * 2^-53 in (0, 1].
     *
     * Taking the top 53 bits as the mantissa avoids double-rounding, and
     * forcing the low bit keeps U strictly positive so `log` cannot return
     * -inf. `std::log` is a leaf call: no allocation, no global state.
     */
    SNMALLOC_FAST_PATH_INLINE static int64_t
    draw_exponential(uint64_t mean, uint64_t r) noexcept
    {
      const uint64_t bits = (r >> 11) | 1; // 53-bit mantissa, non-zero
      const double u =
        static_cast<double>(bits) * (1.0 / static_cast<double>(1ULL << 53));
      const double x = -std::log(u); // in (0, 36.7]
      const double bytes = static_cast<double>(mean) * x;
      // +1 keeps the countdown moving when `bytes` rounds to zero.
      return static_cast<int64_t>(bytes) + 1;
    }

    static uint64_t current_tid() noexcept
    {
      // The address of a thread-local is a stable per-thread id and needs no
      // syscall. Readers only use it to tell threads apart.
      thread_local int tid_anchor = 0;
      return reinterpret_cast<uintptr_t>(&tid_anchor);
    }

  public:
    // Cache-line aligned so the hot counter does not false-share with colder
    // state or with other threads' data. Public so the offset can be
    // asserted below.
    struct alignas(SNMALLOC_CACHE_LINE_SIZE) SamplerHotState
    {
      int64_t bytes_until_sample{0};
    };

    static constexpr size_t kBytesUntilSampleOffset =
      offsetof(SamplerHotState, bytes_until_sample);
    static_assert(
      kBytesUntilSampleOffset == 0,
      "bytes_until_sample must be the first member of "
      "SamplerHotState so it sits at offset 0 of the cache-aligned region");

  private:
    // `hot_` must stay the first member so the counter gets its own cache
    // line, separate from the colder state below it.
    SamplerHotState hot_{};
    uint64_t s_[4]{0, 0, 0, 0};
    uint64_t weight_{0};
    uint64_t interval_at_capture_{0};
    SampledAlloc* last_sample_{nullptr};
  };

  /// The sampler for this thread. Trivially destructible.
  inline thread_local Sampler tl_sampler;

  /**
   * Alloc-side entry point used by `record_alloc` in record.h.
   *
   * Returns true if this allocation was sampled, in which case
   * `tl_sampler.last_sample()` holds the published node.
   */
  SNMALLOC_FAST_PATH_INLINE bool tl_record_alloc(
    uintptr_t alloc_addr, size_t requested_size, size_t allocated_size) noexcept
  {
    bytes_until_sample -= static_cast<int64_t>(requested_size);
    if (SNMALLOC_LIKELY(bytes_until_sample > 0))
      return false;

    // The sampler updates the counter through this reference, so the fast
    // path picks up the next interval.
    return tl_sampler.record_alloc_from_namespace_tls(
      alloc_addr, requested_size, allocated_size, bytes_until_sample);
  }
} // namespace snmalloc::profile
