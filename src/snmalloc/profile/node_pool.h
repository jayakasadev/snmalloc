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

    NodePool() noexcept
    {
      singleton().store(this, std::memory_order_release);
    }
    NodePool(const NodePool&) = delete;
    NodePool& operator=(const NodePool&) = delete;

    ~NodePool() noexcept
    {
      NodePool* expected = this;
      singleton().compare_exchange_strong(
        expected, nullptr, std::memory_order_acq_rel);
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
        nodes_[i].pool_next.store(
          (i + 1 == Capacity) ? nullptr : &nodes_[i + 1],
          std::memory_order_relaxed);
      }

      head_.store(pack_head(0, 0), std::memory_order_release);
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

      flush_limbo();
      uint64_t cur = head_.load(std::memory_order_acquire);
      for (;;)
      {
        const Head h = unpack_head(cur);
        if (h.idx == kNullIdx)
        {
          drops_.fetch_add(1, std::memory_order_relaxed);
          return nullptr;
        }
        SampledAlloc* top = &nodes_[h.idx];
        SampledAlloc* nxt =
          top->pool_next.load(std::memory_order_relaxed);
        const uint32_t next_idx =
          (nxt == nullptr) ? kNullIdx : static_cast<uint32_t>(nxt - nodes_);
        if (head_.compare_exchange_weak(
              cur,
              pack_head(next_idx, h.tag + 1),
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
     *
     * If a snapshot is walking the list, the node sits on a limbo stack
     * until that walk ends so a reader cannot observe a reused payload.
     */
    SNMALLOC_FAST_PATH void release(SampledAlloc* n) noexcept
    {
      if (n == nullptr || nodes_ == nullptr)
        return;
      n->state.store(
        static_cast<uint8_t>(NodeState::Free), std::memory_order_release);
      push_limbo(n);
      if (snapshot_readers.load(std::memory_order_acquire) == 0)
        flush_limbo();
    }

    static void flush_if_idle() noexcept
    {
      if (NodePool* p = singleton().load(std::memory_order_acquire))
        p->flush_limbo();
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
    /// Free-list head decoded from a node index plus an ABA tag.
    struct Head
    {
      uint32_t idx;
      uint32_t tag;
    };

    static constexpr uint64_t pack_head(uint32_t idx, uint32_t tag) noexcept
    {
      return static_cast<uint64_t>(idx) | (static_cast<uint64_t>(tag) << 32);
    }

    static constexpr Head unpack_head(uint64_t raw) noexcept
    {
      return {
        static_cast<uint32_t>(raw),
        static_cast<uint32_t>(raw >> 32)};
    }

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
      head_.store(pack_head(kNullIdx, 0), std::memory_order_release);
    }

    static std::atomic<NodePool*>& singleton() noexcept
    {
      static std::atomic<NodePool*> p{nullptr};
      return p;
    }

    void push_limbo(SampledAlloc* n) noexcept
    {
      SampledAlloc* old = limbo_.load(std::memory_order_relaxed);
      do
      {
        n->pool_next.store(old, std::memory_order_relaxed);
      } while (!limbo_.compare_exchange_weak(
        old, n, std::memory_order_release, std::memory_order_relaxed));
    }

    void push_freelist(SampledAlloc* n) noexcept
    {
      const uint32_t idx = static_cast<uint32_t>(n - nodes_);
      uint64_t cur = head_.load(std::memory_order_acquire);
      for (;;)
      {
        const Head h = unpack_head(cur);
        n->pool_next.store(
          (h.idx == kNullIdx) ? nullptr : &nodes_[h.idx],
          std::memory_order_relaxed);
        if (head_.compare_exchange_weak(
              cur,
              pack_head(idx, h.tag + 1),
              std::memory_order_release,
              std::memory_order_acquire))
          return;
      }
    }

    void flush_limbo() noexcept
    {
      if (snapshot_readers.load(std::memory_order_acquire) != 0)
        return;
      if (reclaim_lock_.test_and_set(std::memory_order_acquire))
        return;
      SampledAlloc* batch = limbo_.exchange(nullptr, std::memory_order_acq_rel);
      if (snapshot_readers.load(std::memory_order_acquire) != 0)
      {
        while (batch != nullptr)
        {
          SampledAlloc* next =
            batch->pool_next.load(std::memory_order_relaxed);
          push_limbo(batch);
          batch = next;
        }
        reclaim_lock_.clear(std::memory_order_release);
        return;
      }
      while (batch != nullptr)
      {
        SampledAlloc* next =
          batch->pool_next.load(std::memory_order_relaxed);
        push_freelist(batch);
        batch = next;
      }
      reclaim_lock_.clear(std::memory_order_release);
    }

    SampledAlloc* nodes_{nullptr};
    alignas(kCacheLineSize) std::atomic<uint64_t> head_{0};
    alignas(kCacheLineSize) std::atomic<uint64_t> drops_{0};
    alignas(kCacheLineSize) std::atomic<SampledAlloc*> limbo_{nullptr};
    std::atomic_flag reclaim_lock_ = ATOMIC_FLAG_INIT;
    std::atomic<uint64_t> seq_{0};
    std::atomic<bool> initialized_{false};
    std::atomic<bool> initializing_{false};
  };
} // namespace snmalloc::profile
