// SPDX-License-Identifier: MIT
//
// Heap profiler -- record for a single sampled allocation.

#pragma once

#include "../ds_core/defines.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

// Stack frames captured per sample.
#ifndef SNMALLOC_PROFILE_STACK_FRAMES
#  define SNMALLOC_PROFILE_STACK_FRAMES 32
#endif

namespace snmalloc::profile
{
  /// Where a node currently lives.
  ///   Free  -- on the NodePool free-list
  ///   Live  -- published on the SampledList
  ///   Freed -- off the SampledList, not yet back in the pool
  enum class NodeState : uint8_t
  {
    Free = 0,
    Live = 1,
    Freed = 2,
  };

  /// Kind tag on a broadcast sample. `Resize` means an in-place realloc
  /// changed the size of an already-sampled allocation, and carries the new
  /// sizes; stored nodes always keep `Alloc`.
  enum class SampledAllocKind : uint8_t
  {
    Alloc = 0,
    Resize = 1,
  };

  static constexpr size_t MaxStackFrames = SNMALLOC_PROFILE_STACK_FRAMES;

  /// Matches snmalloc::CACHELINE_SIZE, repeated here so the profile headers
  /// do not depend on ds_core.
  static constexpr size_t kCacheLineSize = 64;

  /// Snapshot walks hold this above zero so `NodePool::release` can defer
  /// reuse until every in-flight reader has finished.
  inline std::atomic<uint32_t> snapshot_readers{0};

  /**
   * One sampled allocation.
   *
   * Payload is written before publish, then read through the SampledList's
   * acquire/release link. `requested_size` / `allocated_size` may be
   * updated later by in-place realloc.
   *
   * `weight` is in bytes of request. At dump time:
   *   allocated bytes = weight * allocated_size / (requested_size + 1)
   *   object count    = weight / (requested_size + 1)
   *
   * `sample_interval_at_capture` is the rate that was in force when this
   * sample fired, kept per node so a later rate change cannot reweight
   * samples already taken.
   */
  struct alignas(kCacheLineSize) SampledAlloc
  {
    // -- intrusive links --------------------------------------------------
    /// Next node on the SampledList; the low bit marks a tombstone, and is
    /// free because SampledAlloc is cache-line aligned. Written with release,
    /// read with acquire.
    std::atomic<uintptr_t> next{0};

    /// NodePool free-list / deferred-reclamation link.
    std::atomic<SampledAlloc*> pool_next{nullptr};

    // -- payload (written once, before SampledList publication) -----------
    uintptr_t alloc_addr{0};
    /// Relaxed: realloc may store while a snapshot loads.
    std::atomic<size_t> requested_size{0};
    std::atomic<size_t> allocated_size{0};
    uint64_t weight{0};
    uint64_t sample_interval_at_capture{0};
    uint64_t tid{0};
    /// Bumped on every acquire, so a snapshot reader can tell that a node was
    /// freed and reused between passes.
    uint64_t alloc_seq{0};
    /// Steady-clock nanoseconds at sample fire, used on free to bin the
    /// allocation's lifetime. Zero if the node was never published.
    uint64_t alloc_ts_ns{0};

    uintptr_t stack[MaxStackFrames];

    uint8_t stack_depth{0};
    /// NodeState. Atomic because a snapshot may read it while the node is
    /// changing state.
    std::atomic<uint8_t> state{static_cast<uint8_t>(NodeState::Free)};
    /// SampledAllocKind as a raw byte, so the struct stays usable across the
    /// FFI boundary.
    uint8_t kind{static_cast<uint8_t>(SampledAllocKind::Alloc)};
    uint8_t _pad[5]{};

    SampledAlloc() noexcept = default;
    SampledAlloc(const SampledAlloc&) = delete;
    SampledAlloc& operator=(const SampledAlloc&) = delete;

    /**
     * Clear the payload before reuse. The caller owns the node exclusively,
     * having just popped it off the free-list, so relaxed stores suffice.
     */
    SNMALLOC_FAST_PATH_INLINE void reset_for_acquire() noexcept
    {
      next.store(0, std::memory_order_relaxed);
      pool_next.store(nullptr, std::memory_order_relaxed);
      alloc_addr = 0;
      requested_size.store(0, std::memory_order_relaxed);
      allocated_size.store(0, std::memory_order_relaxed);
      weight = 0;
      sample_interval_at_capture = 0;
      tid = 0;
      alloc_seq = 0;
      alloc_ts_ns = 0;
      stack_depth = 0;
      kind = static_cast<uint8_t>(SampledAllocKind::Alloc);
      for (size_t i = 0; i < MaxStackFrames; ++i)
        stack[i] = 0;
      state.store(
        static_cast<uint8_t>(NodeState::Free), std::memory_order_relaxed);
    }
  };

  static_assert(
    alignof(SampledAlloc) >= 2,
    "SampledAlloc alignment must reserve the low bit for the tombstone tag");
} // namespace snmalloc::profile
