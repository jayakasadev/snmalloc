// SPDX-License-Identifier: MIT
//
// Heap profiler -- lock-free list of currently-sampled allocations.
//
// An intrusive Treiber stack. Liveness and the next link share one word
// (`SampledAlloc::next`, tombstone in the low bit), so a reader gets both
// from a single atomic load.
//
// Removal has two steps: CAS the tombstone bit in, which is the point at
// which the node counts as gone, then try to unlink it physically. Losing
// that second race is harmless -- readers skip tombstoned nodes and a later
// pass reaps them. Node memory belongs to the NodePool, so the list needs no
// deferred reclamation.

#pragma once

#include "../ds_core/defines.h"
#include "sampled_alloc.h"

#include <atomic>
#include <cstdint>

namespace snmalloc::profile
{
  /**
   * Lock-free intrusive list of SampledAlloc nodes.
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
      SampledAlloc* old_head = head_.load(std::memory_order_relaxed);
      for (;;)
      {
        node->next.store(tag(old_head, false), std::memory_order_relaxed);
        if (head_.compare_exchange_weak(
              old_head,
              node,
              std::memory_order_release,
              std::memory_order_relaxed))
        {
          return;
        }
      }
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

      // The node counts as removed once this CAS lands.
      uintptr_t cur = node->next.load(std::memory_order_relaxed);
      for (;;)
      {
        if (is_tombstoned(cur))
          return false;
        if (node->next.compare_exchange_weak(
              cur,
              cur | kTombstoneBit,
              std::memory_order_release,
              std::memory_order_relaxed))
          break;
      }

      // Best-effort: readers skip tombstoned nodes if this fails.
      try_unlink(node);
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
      size_t live = 0;
      SampledAlloc* cur = head_.load(std::memory_order_acquire);
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
    /**
     * Point `node`'s predecessor past it. Best-effort: if a race is lost the
     * node stays tombstoned and a later walk reaps it.
     */
    void try_unlink(SampledAlloc* node) noexcept
    {
      // Strip node's own tombstone bit to get its successor.
      uintptr_t node_next = node->next.load(std::memory_order_acquire);
      SampledAlloc* succ = untag(node_next);

      // The node may itself be the head.
      SampledAlloc* h = head_.load(std::memory_order_acquire);
      if (h == node)
      {
        if (head_.compare_exchange_strong(
              h, succ, std::memory_order_release, std::memory_order_relaxed))
          return;
        // Lost the race; fall through to the scan.
      }

      SampledAlloc* prev = head_.load(std::memory_order_acquire);
      while (prev != nullptr)
      {
        if (prev == node)
          return; // Back at the head; leave it to a later pass.
        uintptr_t pn = prev->next.load(std::memory_order_acquire);
        if (is_tombstoned(pn))
        {
          // Unlinking a tombstoned predecessor will splice past whatever is
          // attached to it, so skip it here.
          prev = untag(pn);
          continue;
        }
        SampledAlloc* nxt = untag(pn);
        if (nxt == node)
        {
          // Both values are untagged: the tombstone bit in `prev->next`
          // belongs to `prev`, not to the node being unlinked.
          uintptr_t expected = tag(node, false);
          uintptr_t desired = tag(succ, false);
          prev->next.compare_exchange_strong(
            expected,
            desired,
            std::memory_order_release,
            std::memory_order_relaxed);
          return;
        }
        prev = nxt;
      }
    }

    alignas(kCacheLineSize) std::atomic<SampledAlloc*> head_{nullptr};
  };
} // namespace snmalloc::profile
