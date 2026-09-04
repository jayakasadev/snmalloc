// SPDX-License-Identifier: MIT
//
// Heap profiler -- fixed-capacity lock-free pool of SampledAlloc nodes.
//
// Storage is one contiguous region taken straight from the OS (mmap or
// VirtualAlloc). It must never come from snmalloc itself: the profiler runs
// inside the allocator's own alloc path.
//
// The free-list is a Treiber stack whose 64-bit head packs a node index and
// an ABA tag. When the pool is empty `acquire()` returns nullptr and counts a
// drop, and the caller skips the sample.

#pragma once

#include "../ds_core/defines.h"
#include "sampled_alloc.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <sys/mman.h>
#  include <unistd.h>
#endif

#ifndef SNMALLOC_PROFILE_POOL_CAPACITY
#  define SNMALLOC_PROFILE_POOL_CAPACITY 16384
#endif

namespace snmalloc::profile
{
  /**
   * Lock-free pool of `Capacity` SampledAlloc nodes.
   *
   * Thread-safe, and safe to call from inside the allocator: every method
   * touches only the pool's own memory and never the host allocator.
   */
  template<size_t Capacity = SNMALLOC_PROFILE_POOL_CAPACITY>
  class NodePool
  {
    static_assert(
      Capacity > 0 && Capacity < (1u << 31),
      "Capacity must fit in 31 bits (one bit reserved as null sentinel)");

  public:
    static constexpr uint32_t kNullIdx = 0xFFFFFFFFu;

    NodePool() noexcept = default;
    NodePool(const NodePool&) = delete;
    NodePool& operator=(const NodePool&) = delete;

    ~NodePool() noexcept
    {
      release_storage();
    }

    /**
     * Reserve storage and build the free-list. Idempotent and thread-safe.
     */
    void init() noexcept
    {
      if (SNMALLOC_LIKELY(initialized_.load(std::memory_order_acquire)))
        return;

      // One thread wins the right to initialise; the rest wait.
      bool expected = false;
      if (!initializing_.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel))
      {
        while (!initialized_.load(std::memory_order_acquire))
        {
          // Spin: init is O(Capacity) and happens once per process.
        }
        return;
      }

      const size_t bytes = Capacity * sizeof(SampledAlloc);
      void* base = os_reserve(bytes);
      if (base == nullptr)
      {
        // Publish as initialised with `nodes_` still null: the pool is
        // unusable and every acquire counts a drop, but nothing blocks and
        // the process keeps running.
        initialized_.store(true, std::memory_order_release);
        return;
      }
      nodes_ = static_cast<SampledAlloc*>(base);

      // Construct the nodes and chain them into the free-list.
      for (uint32_t i = 0; i < Capacity; ++i)
      {
        new (&nodes_[i]) SampledAlloc();
        nodes_[i].pool_next = (i + 1 == Capacity) ? nullptr : &nodes_[i + 1];
      }

      Head h{};
      h.parts.idx = 0;
      h.parts.tag = 0;
      head_.store(h.raw, std::memory_order_release);
      initialized_.store(true, std::memory_order_release);
    }

    /**
     * Pop a node, or nullptr if the pool is empty.
     *
     * The caller owns the node exclusively. It arrives reset and marked
     * Live, ready for the caller to fill in the payload and publish it on a
     * SampledList.
     */
    SNMALLOC_FAST_PATH SampledAlloc* acquire() noexcept
    {
      if (SNMALLOC_UNLIKELY(!initialized_.load(std::memory_order_acquire)))
      {
        init();
        if (SNMALLOC_UNLIKELY(nodes_ == nullptr))
        {
          drops_.fetch_add(1, std::memory_order_relaxed);
          return nullptr;
        }
      }

      uint64_t cur = head_.load(std::memory_order_acquire);
      for (;;)
      {
        Head h{};
        h.raw = cur;
        if (h.parts.idx == kNullIdx)
        {
          drops_.fetch_add(1, std::memory_order_relaxed);
          return nullptr;
        }
        SampledAlloc* top = &nodes_[h.parts.idx];
        SampledAlloc* nxt = top->pool_next;
        Head nh{};
        nh.parts.idx =
          (nxt == nullptr) ? kNullIdx : static_cast<uint32_t>(nxt - nodes_);
        nh.parts.tag = h.parts.tag + 1;
        if (head_.compare_exchange_weak(
              cur,
              nh.raw,
              std::memory_order_acquire,
              std::memory_order_acquire))
        {
          top->reset_for_acquire();
          top->alloc_seq = seq_.fetch_add(1, std::memory_order_relaxed) + 1;
          top->state.store(
            static_cast<uint8_t>(NodeState::Live), std::memory_order_relaxed);
          return top;
        }
      }
    }

    /**
     * Return a node to the free-list. The caller must already have removed
     * it from any SampledList.
     */
    SNMALLOC_FAST_PATH void release(SampledAlloc* n) noexcept
    {
      if (n == nullptr || nodes_ == nullptr)
        return;
      // Release so a snapshot in flight sees the node go Free before
      // `pool_next` is overwritten.
      n->state.store(
        static_cast<uint8_t>(NodeState::Free), std::memory_order_release);
      n->next.store(0, std::memory_order_relaxed);

      const uint32_t idx = static_cast<uint32_t>(n - nodes_);
      uint64_t cur = head_.load(std::memory_order_acquire);
      for (;;)
      {
        Head h{};
        h.raw = cur;
        n->pool_next =
          (h.parts.idx == kNullIdx) ? nullptr : &nodes_[h.parts.idx];
        Head nh{};
        nh.parts.idx = idx;
        nh.parts.tag = h.parts.tag + 1;
        if (head_.compare_exchange_weak(
              cur,
              nh.raw,
              std::memory_order_release,
              std::memory_order_acquire))
          return;
      }
    }

    [[nodiscard]] uint64_t drop_count() const noexcept
    {
      return drops_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] static constexpr size_t capacity() noexcept
    {
      return Capacity;
    }

    [[nodiscard]] SampledAlloc* base() noexcept
    {
      return nodes_;
    }

    /// Test-only.
    void debug_reset_drops() noexcept
    {
      drops_.store(0, std::memory_order_relaxed);
    }

  private:
    /// Free-list head: a node index plus an ABA tag in one 64-bit word.
    union Head
    {
      struct
      {
        uint32_t idx;
        uint32_t tag;
      } parts;

      uint64_t raw;
    };

    static_assert(sizeof(Head) == 8, "Head must pack into one 64-bit word");

    static void* os_reserve(size_t bytes) noexcept
    {
#if defined(_WIN32)
      return ::VirtualAlloc(
        nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#else
      void* p = ::mmap(
        nullptr,
        bytes,
        PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS,
        -1,
        0);
      if (p == MAP_FAILED)
        return nullptr;
      return p;
#endif
    }

    static void os_release(void* base, size_t bytes) noexcept
    {
#if defined(_WIN32)
      (void)bytes;
      ::VirtualFree(base, 0, MEM_RELEASE);
#else
      ::munmap(base, bytes);
#endif
    }

    void release_storage() noexcept
    {
      if (nodes_ == nullptr)
        return;
      for (uint32_t i = 0; i < Capacity; ++i)
        nodes_[i].~SampledAlloc();
      os_release(nodes_, Capacity * sizeof(SampledAlloc));
      nodes_ = nullptr;
      initialized_.store(false, std::memory_order_release);
      initializing_.store(false, std::memory_order_release);
      Head h{};
      h.parts.idx = kNullIdx;
      h.parts.tag = 0;
      head_.store(h.raw, std::memory_order_release);
    }

    SampledAlloc* nodes_{nullptr};
    alignas(kCacheLineSize) std::atomic<uint64_t> head_{0};
    alignas(kCacheLineSize) std::atomic<uint64_t> drops_{0};
    std::atomic<uint64_t> seq_{0};
    std::atomic<bool> initialized_{false};
    std::atomic<bool> initializing_{false};
  };
} // namespace snmalloc::profile
