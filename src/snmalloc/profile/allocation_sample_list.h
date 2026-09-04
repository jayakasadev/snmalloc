// SPDX-License-Identifier: MIT
//
// Heap profiler -- streaming broadcast to sample subscribers.
//
// Not to be confused with `sampled_list.h`, which holds the live samples
// themselves. This is a notification registry: every sampled allocation is
// handed to each registered handler as it happens, instead of consumers
// having to poll snapshots.
//
// Subscribers live in a fixed array of atomic function pointers, so nothing
// here allocates. Registering fails once all slots are taken.
//
// Requirements on a handler:
//   - Must be `noexcept`; an escaping exception is undefined behaviour.
//   - Must not allocate through snmalloc, which would re-enter the alloc
//     path. This is not enforced, but the call site in `record.h` holds the
//     sampler's ReentrancyGuard, so an allocating handler short-circuits
//     instead of looping forever.
//   - Must return promptly: it runs on the allocating thread, inline with
//     the allocation.
//
// Registering and unregistering are lock-free and may run concurrently with
// a broadcast, which re-reads each slot as it goes. A handler registered
// during a broadcast may or may not be called by it.

#pragma once

#include "../ds_core/defines.h"
#include "sampled_alloc.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace snmalloc::profile
{
  /**
   * A streaming subscriber. Called once per sampled allocation, on the
   * allocating thread; see the file header for what it may do.
   */
  using AllocationSampleCallback = void (*)(const SampledAlloc&) noexcept;

  /**
   * Registry of streaming subscribers.
   */
  class AllocationSampleList
  {
  public:
    /// Subscriber limit. Every slot is read on each broadcast, and real
    /// consumers number zero or one.
    static constexpr size_t kMaxSubscribers = 4;

    static constexpr int kOk = 0;
    static constexpr int kNoFreeSlot = -1;
    static constexpr int kNotRegistered = -1;

    AllocationSampleList() noexcept = default;
    AllocationSampleList(const AllocationSampleList&) = delete;
    AllocationSampleList& operator=(const AllocationSampleList&) = delete;

    /**
     * The one registry per process, shared by the alloc hook and the FFI
     * that registers handlers.
     */
    static AllocationSampleList& global() noexcept
    {
      static AllocationSampleList g;
      return g;
    }

    /**
     * Add `cb` as a subscriber. Returns `kOk`, or `kNoFreeSlot` if every
     * slot is in use. `nullptr` is rejected: an empty slot holds null.
     */
    int register_handler(AllocationSampleCallback cb) noexcept
    {
      if (cb == nullptr)
        return kNoFreeSlot;

      for (size_t i = 0; i < kMaxSubscribers; ++i)
      {
        AllocationSampleCallback expected = nullptr;
        if (slots_[i].compare_exchange_strong(
              expected,
              cb,
              std::memory_order_acq_rel,
              std::memory_order_relaxed))
        {
          return kOk;
        }
      }
      return kNoFreeSlot;
    }

    /**
     * Remove `cb`. Returns `kOk`, or `kNotRegistered` if it was not
     * subscribed.
     */
    int unregister_handler(AllocationSampleCallback cb) noexcept
    {
      if (cb == nullptr)
        return kNotRegistered;

      for (size_t i = 0; i < kMaxSubscribers; ++i)
      {
        AllocationSampleCallback expected = cb;
        if (slots_[i].compare_exchange_strong(
              expected,
              nullptr,
              std::memory_order_acq_rel,
              std::memory_order_relaxed))
        {
          return kOk;
        }
      }
      return kNotRegistered;
    }

    /**
     * Hand `sample` to every subscriber, once each, in unspecified order. A
     * slot cleared by a concurrent unregister is skipped.
     */
    void broadcast(const SampledAlloc& sample) const noexcept
    {
      for (size_t i = 0; i < kMaxSubscribers; ++i)
      {
        AllocationSampleCallback cb = slots_[i].load(std::memory_order_acquire);
        if (cb != nullptr)
        {
          cb(sample);
        }
      }
    }

    /**
     * Number of subscribers. For diagnostics, not for branching on.
     */
    [[nodiscard]] size_t subscriber_count() const noexcept
    {
      size_t n = 0;
      for (size_t i = 0; i < kMaxSubscribers; ++i)
      {
        if (slots_[i].load(std::memory_order_relaxed) != nullptr)
          ++n;
      }
      return n;
    }

    /**
     * Test-only: drop every subscriber. Not safe to run alongside
     * broadcast, register or unregister.
     */
    void clear_all() noexcept
    {
      for (size_t i = 0; i < kMaxSubscribers; ++i)
      {
        slots_[i].store(nullptr, std::memory_order_release);
      }
    }

  private:
    alignas(kCacheLineSize)
      std::atomic<AllocationSampleCallback> slots_[kMaxSubscribers]{};
  };
} // namespace snmalloc::profile
