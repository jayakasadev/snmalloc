// SPDX-License-Identifier: MIT
//
// Heap profiler -- concurrent list of currently-sampled allocations.
//
// An intrusive Treiber stack. Liveness and the next link share one word
// (`SampledAlloc::next`, tombstone in the low bit), so a reader gets both
// from a single atomic load.
//
// Mutations are rare (only sampled allocations) and serialize briefly so
// removal always unlinks a node. Snapshots traverse without the mutation
// lock; `NodePool::release` defers reuse while a reader is active.

#pragma once

#include "../ds_core/defines.h"
#include "node_pool.h"
#include "sampled_alloc.h"

#include <atomic>
#include <cstdint>

namespace snmalloc::profile
{
  /**
   * Concurrent intrusive list of SampledAlloc nodes.
   *
   * Invariants:
   *   - A node counts as on the list from the `push` that linked it until a
   *     tombstone CAS succeeds on its `next` field.
   *   - Readers tolerate concurrent push and remove: a push may or may not
   *     be seen by a snapshot already in flight, while a tombstone is seen
   *     by any snapshot that acquire-loads `next` after it.
   */
  class SampledList
  {
  public:
    static constexpr uintptr_t kTombstoneBit = 1;

    [[nodiscard]] static SampledAlloc* untag(uintptr_t p) noexcept
    {
      return reinterpret_cast<SampledAlloc*>(p & ~kTombstoneBit);
    }

    [[nodiscard]] static bool is_tombstoned(uintptr_t p) noexcept
    {
      return (p & kTombstoneBit) != 0;
    }

    [[nodiscard]] static uintptr_t tag(SampledAlloc* p, bool tomb) noexcept
    {
      return reinterpret_cast<uintptr_t>(p) | (tomb ? kTombstoneBit : 0);
    }

    SampledList() noexcept = default;
    SampledList(const SampledList&) = delete;
    SampledList& operator=(const SampledList&) = delete;

    /**
     * Publish a freshly-acquired node. On return, any snapshot that
     * acquire-loads the head sees the node with its payload fully written.
     */
    void push(SampledAlloc* node) noexcept
    {
      lock_mutations();
      SampledAlloc* old_head = head_.load(std::memory_order_relaxed);
      node->next.store(tag(old_head, false), std::memory_order_relaxed);
      head_.store(node, std::memory_order_release);
      unlock_mutations();
    }

    /**
     * Mark a node as removed. Safe from any thread, including one that did
     * not push it, as happens on a cross-thread free.
     *
     * Returns true if this call was the one that tombstoned the node.
     */
    bool remove(SampledAlloc* node) noexcept
    {
      if (node == nullptr)
        return false;

      lock_mutations();
      uintptr_t cur = node->next.load(std::memory_order_relaxed);
      if (is_tombstoned(cur))
      {
        unlock_mutations();
        return false;
      }
      node->next.store(cur | kTombstoneBit, std::memory_order_release);
      unlink_locked(node, untag(cur));
      unlock_mutations();
      return true;
    }

    /**
     * Call `fn(node)` for every live node and return how many there were.
     *
     * Runs safely alongside concurrent push and remove. `fn` must not call
     * `remove`: a snapshot is read-only.
     */
    template<typename F>
    size_t snapshot(F&& fn) const noexcept
    {
      // Serialize reader registration with unlink. A reader either protects
      // an old node before it is removed, or starts from the post-unlink head.
      lock_mutations();
      snapshot_readers.fetch_add(1, std::memory_order_acq_rel);
      size_t live = 0;
      SampledAlloc* cur = head_.load(std::memory_order_acquire);
      unlock_mutations();
      while (cur != nullptr)
      {
        uintptr_t n = cur->next.load(std::memory_order_acquire);
        if (!is_tombstoned(n))
        {
          fn(cur);
          ++live;
        }
        cur = untag(n);
      }
      if (snapshot_readers.fetch_sub(1, std::memory_order_acq_rel) == 1)
        NodePool<>::flush_if_idle();
      return live;
    }

    /// Test-only: count of live nodes.
    [[nodiscard]] size_t debug_count() const noexcept
    {
      return snapshot([](SampledAlloc*) {});
    }

    /// Test-only: empty the list, handing every node, live or tombstoned, to
    /// `fn` so the caller can return it to the pool. Not safe to run
    /// alongside push, remove or snapshot.
    template<typename F>
    void debug_drain(F&& fn) noexcept
    {
      SampledAlloc* cur = head_.exchange(nullptr, std::memory_order_acq_rel);
      while (cur != nullptr)
      {
        SampledAlloc* next = untag(cur->next.load(std::memory_order_relaxed));
        cur->next.store(0, std::memory_order_relaxed);
        fn(cur);
        cur = next;
      }
    }

  private:
    void lock_mutations() const noexcept
    {
      while (mutation_lock_.test_and_set(std::memory_order_acquire))
      {}
    }

    void unlock_mutations() const noexcept
    {
      mutation_lock_.clear(std::memory_order_release);
    }

    void unlink_locked(SampledAlloc* node, SampledAlloc* succ) noexcept
    {
      SampledAlloc* h = head_.load(std::memory_order_relaxed);
      if (h == node)
      {
        head_.store(succ, std::memory_order_release);
        return;
      }

      SampledAlloc* prev = h;
      while (prev != nullptr)
      {
        uintptr_t pn = prev->next.load(std::memory_order_relaxed);
        SampledAlloc* nxt = untag(pn);
        if (nxt == node)
        {
          prev->next.store(tag(succ, false), std::memory_order_release);
          return;
        }
        prev = nxt;
      }
    }

    alignas(kCacheLineSize) std::atomic<SampledAlloc*> head_{nullptr};
    mutable std::atomic_flag mutation_lock_ = ATOMIC_FLAG_INIT;
  };
} // namespace snmalloc::profile
