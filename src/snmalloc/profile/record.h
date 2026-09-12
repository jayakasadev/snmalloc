// SPDX-License-Identifier: MIT
//
// Heap profiler -- the alloc and dealloc hook bodies.
//
// Every `record_*` function below compiles to nothing unless `Config` carries
// a LazyArrayClientMetaDataProvider<ProfileSlot>, so the bodies need no build
// gating and cannot drift between translation units.
//
// Re-entrancy: everything the profiler allocates comes from the platform
// layer (`Pal::reserve`), never from snmalloc itself, so no hook can recurse
// into the allocator. Where that is not enough the hook takes the per-thread
// ReentrancyGuard.

#pragma once

// `snmalloc_core.h` makes this header self-sufficient. There is no include
// cycle: mem/corealloc.h never includes it.
#include "../ds_core/defines.h"
#include "../snmalloc_core.h"
#include "allocation_sample_list.h"
#include "lifetime_histogram.h"
#include "node_pool.h"
#include "reentrancy_guard.h"
#include "sampled_alloc.h"
#include "sampled_list.h"
#include "sampler.h"
#ifdef SNMALLOC_PROFILE_SECONDARY_STORAGE
#  include "spike_secondary.h"
#endif

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace snmalloc::profile
{
  /**
   * One slot per object, holding the sample for that object if it was
   * sampled. Atomic so double-free and cross-thread free race through a
   * single CAS.
   */
  using ProfileSlot = std::atomic<SampledAlloc*>;

  /**
   * Nanoseconds from a steady clock, so a wall-clock step cannot produce a
   * negative lifetime. Allocates nothing.
   */
  SNMALLOC_FAST_PATH_INLINE uint64_t lifetime_now_ns() noexcept
  {
    return static_cast<uint64_t>(
      std::chrono::steady_clock::now().time_since_epoch().count());
  }

  /**
   * Does `Config` carry per-object profile slots? When false, every
   * `record_*` call below compiles away.
   */
  template<typename Config>
  inline constexpr bool config_has_profile_slot_v = std::is_same_v<
    typename Config::ClientMeta,
    LazyArrayClientMetaDataProvider<ProfileSlot>>;

  /**
   * Find the profile slot for `p`, or nullptr if there cannot be one: the
   * page is not frontend-owned, the slab metadata is missing, or nothing on
   * this slab has ever been sampled so the backing array is absent.
   *
   * Never installs the backing array. A dealloc must not force the
   * profiler's metadata into existence.
   */
  template<typename Config>
  SNMALLOC_FAST_PATH_INLINE ProfileSlot* find_profile_slot_from_entry(
    void* p, const typename Config::PagemapEntry& entry) noexcept
  {
    static_assert(
      config_has_profile_slot_v<Config>,
      "find_profile_slot requires a LazyArrayClientMetaDataProvider<"
      "ProfileSlot> config; gate callers on config_has_profile_slot_v");

    using ClientMeta = typename Config::ClientMeta;
    using Storage = typename ClientMeta::StorageType;

    if (SNMALLOC_UNLIKELY(!entry.is_owned()))
      return nullptr;
    if (SNMALLOC_UNLIKELY(entry.is_backend_owned()))
      return nullptr;

    auto* meta = entry.get_slab_metadata();
    if (SNMALLOC_UNLIKELY(meta == nullptr))
      return nullptr;

    // A large allocation owns its slab and uses slot 0; small allocations
    // index by position within the slab.
    auto sc = entry.get_sizeclass();
    size_t index = sc.is_small() ? slab_index(sc, address_cast(p)) : 0;

    // Read the lazy pointer directly. `ClientMeta::get` would reserve memory
    // from the platform layer, which a dealloc must not do.
    Storage* storage = &meta->client_meta_;
    ProfileSlot* backing = storage->backing.load(std::memory_order_acquire);
    if (backing == nullptr)
      return nullptr;

    return &backing[index];
  }

  template<typename Config>
  SNMALLOC_FAST_PATH_INLINE ProfileSlot* find_profile_slot(void* p) noexcept
  {
    const auto& entry =
      Config::Backend::template get_metaentry<true>(address_cast(p));
    return find_profile_slot_from_entry<Config>(p, entry);
  }

  /**
   * Clear a slot and recycle its sample. Safe to call on a null or
   * already-cleared slot.
   *
   * Returns the node this call cleared, or nullptr if there was nothing to
   * clear or another thread won the race.
   */
  SNMALLOC_FAST_PATH_INLINE SampledAlloc*
  clear_profile_slot(ProfileSlot* slot) noexcept
  {
    if (slot == nullptr)
      return nullptr;

    // Acquire on success so we see the payload written by the thread that
    // published the sample.
    SampledAlloc* expected = slot->load(std::memory_order_relaxed);
    if (expected == nullptr)
      return nullptr;

    // No retry: there is at most one legitimate clearer per sample, so a
    // failed CAS means another free got there first.
    if (!slot->compare_exchange_strong(
          expected,
          nullptr,
          std::memory_order_acquire,
          std::memory_order_relaxed))
    {
      return nullptr;
    }

    // The CAS above succeeded, so exactly one thread records this sample's
    // lifetime. A zero timestamp means the sample was published without one,
    // and binning `now - 0` would land in the largest bucket.
    const uint64_t alloc_ts = expected->alloc_ts_ns;
    if (alloc_ts != 0)
    {
      const uint64_t now_ns = lifetime_now_ns();
      // An alloc and free within one clock tick give equal timestamps; count
      // those in the lowest bucket rather than dropping them.
      const uint64_t lifetime_ns =
        (now_ns > alloc_ts) ? (now_ns - alloc_ts) : 1;
      LifetimeHistogram::get().record_lifetime_ns(lifetime_ns);
    }

    // Must leave the list before going back to the pool.
    SamplerGlobals::list().remove(expected);
    SamplerGlobals::pool().release(expected);
    return expected;
  }

  /**
   * Dealloc hook. If `p` was sampled, clears its slot, takes the sample off
   * the global list, and returns the node to the pool. The CAS inside
   * `clear_profile_slot` makes double-free and cross-thread free safe.
   */
  template<typename Config>
  SNMALLOC_FAST_PATH_INLINE void record_dealloc(
    void* p, const typename Config::PagemapEntry& entry) noexcept
  {
    if constexpr (!config_has_profile_slot_v<Config>)
    {
      (void)p;
      (void)entry;
      return;
    }
    else
    {
      if (SNMALLOC_UNLIKELY(p == nullptr))
        return;

      // Probe the slot before any re-entrancy bookkeeping: it is a plain
      // load on metadata the free path reads anyway, whereas the guard costs
      // a thread-local write even when there is no slot.
      ProfileSlot* slot = find_profile_slot_from_entry<Config>(p, entry);
      if (SNMALLOC_LIKELY(slot == nullptr))
        return;

      // Slab has slots but this object was never sampled.
      if (SNMALLOC_LIKELY(slot->load(std::memory_order_relaxed) == nullptr))
        return;

      // The profiler can free memory during its own cleanup; do not recurse.
      if (SNMALLOC_UNLIKELY(sampler_reentered()))
        return;

      ReentrancyGuard guard;

      // Re-checks with a CAS: another thread may have cleared the slot since
      // the load above.
      (void)clear_profile_slot(slot);
    }
  }

  template<typename Config>
  SNMALLOC_FAST_PATH_INLINE void record_dealloc(void* p) noexcept
  {
    if constexpr (!config_has_profile_slot_v<Config>)
    {
      (void)p;
    }
    else
    {
      if (SNMALLOC_UNLIKELY(p == nullptr))
        return;
      const auto& entry =
        Config::Backend::template get_metaentry<true>(address_cast(p));
      record_dealloc<Config>(p, entry);
    }
  }

  /**
   * Alloc-side slot lookup, installing the slab's backing array on first
   * use. This is the only place allowed to install it.
   *
   * Returns nullptr if `p` has no frontend slab metadata, or if the install
   * failed. The install goes through the platform layer, not the host
   * allocator, so it cannot re-enter `snmalloc::alloc`.
   */
  template<typename Config>
  SNMALLOC_FAST_PATH_INLINE ProfileSlot*
  find_or_install_profile_slot(void* p) noexcept
  {
    static_assert(
      config_has_profile_slot_v<Config>,
      "find_or_install_profile_slot requires a "
      "LazyArrayClientMetaDataProvider<ProfileSlot> config; gate callers "
      "on config_has_profile_slot_v");

    using ClientMeta = typename Config::ClientMeta;
    using Storage = typename ClientMeta::StorageType;

    const auto& entry =
      Config::Backend::template get_metaentry<true>(address_cast(p));

    if (SNMALLOC_UNLIKELY(!entry.is_owned()))
      return nullptr;
    if (SNMALLOC_UNLIKELY(entry.is_backend_owned()))
      return nullptr;

    auto* meta = entry.get_slab_metadata();
    if (SNMALLOC_UNLIKELY(meta == nullptr))
      return nullptr;

    auto sc = entry.get_sizeclass();
    const bool is_small = sc.is_small();
    const size_t index = is_small ? slab_index(sc, address_cast(p)) : 0;
    // The backing array needs one slot per object on the slab; a large
    // allocation has the slab to itself.
    const size_t slab_object_count =
      is_small ? sizeclass_to_slab_object_count(sc.as_small()) : 1;

    Storage* storage = &meta->client_meta_;
    ProfileSlot* backing = storage->backing.load(std::memory_order_acquire);
    if (SNMALLOC_UNLIKELY(backing == nullptr))
    {
      // Install failure (out of address space) is treated like a dropped
      // sample.
      backing = ClientMeta::install(storage, slab_object_count);
      if (SNMALLOC_UNLIKELY(backing == nullptr))
        return nullptr;
    }
    return &backing[index];
  }

  SNMALLOC_FAST_PATH_INLINE void
  cancel_prepared_alloc(SampledAlloc* sample) noexcept
  {
    if (sample == nullptr)
      return;
    SamplerGlobals::pool().release(sample);
  }

  /**
   * Finish publishing a sample whose Poisson decision was made before the
   * main-heap allocation. This does not touch a sampler countdown.
   */
  template<typename Config>
  SNMALLOC_FAST_PATH_INLINE void finalize_prepared_alloc(
    void* p, size_t requested, size_t allocated, SampledAlloc* sample) noexcept
  {
    if (sample == nullptr)
      return;
    if (p == nullptr)
    {
      cancel_prepared_alloc(sample);
      return;
    }

    sample->alloc_addr = reinterpret_cast<uintptr_t>(p);
    sample->requested_size.store(requested, std::memory_order_relaxed);
    sample->allocated_size.store(allocated, std::memory_order_relaxed);
    sample->alloc_ts_ns = lifetime_now_ns();

    ProfileSlot* slot = find_or_install_profile_slot<Config>(p);
    if (slot == nullptr)
    {
      cancel_prepared_alloc(sample);
      return;
    }

    SampledAlloc* expected = nullptr;
    if (!slot->compare_exchange_strong(
          expected,
          sample,
          std::memory_order_release,
          std::memory_order_relaxed))
    {
      cancel_prepared_alloc(sample);
      return;
    }

    SamplerGlobals::list().push(sample);
    ReentrancyGuard broadcast_guard;
    AllocationSampleList::global().broadcast(*sample);
  }

  /**
   * Alloc hook, called for every successful allocation. Ticks the sampler
   * and, when a sample fires, stores the node in `p`'s profile slot so the
   * dealloc hook can find it again.
   */
  template<typename Config>
  SNMALLOC_FAST_PATH_INLINE void
  record_alloc(void* p, size_t requested, size_t allocated) noexcept
  {
    if constexpr (!config_has_profile_slot_v<Config>)
    {
      (void)p;
      (void)requested;
      (void)allocated;
      return;
    }
    else
    {
      if (SNMALLOC_UNLIKELY(p == nullptr))
        return;

      // The sampler's slow path checks re-entrancy and takes its own guard
      // before doing any work, so no outer guard is needed here.
      const uintptr_t addr = reinterpret_cast<uintptr_t>(p);
      const bool fired = tl_record_alloc(addr, requested, allocated);
      if (SNMALLOC_LIKELY(!fired))
        return;

      SampledAlloc* node = tl_sampler.last_sample();
      if (node == nullptr)
      {
        // Sample fired but the pool was empty, or the sampler was
        // re-entered. Nothing to install.
        return;
      }

      // Stamp the timestamp before the node is reachable from the dealloc
      // hook. A plain store is enough: the slot CAS below is the release
      // that publishes this write to the freeing thread.
      node->alloc_ts_ns = lifetime_now_ns();

      ProfileSlot* slot = find_or_install_profile_slot<Config>(p);
      if (SNMALLOC_UNLIKELY(slot == nullptr))
      {
        // The dealloc side could never reach this sample, so recycle it now
        // instead of leaking a pool node.
        SamplerGlobals::list().remove(node);
        SamplerGlobals::pool().release(node);
        return;
      }

      // `p` has not been returned to the caller yet, so nothing should be
      // able to lose this CAS; the failure branch is defensive only.
      SampledAlloc* expected = nullptr;
      if (SNMALLOC_UNLIKELY(!slot->compare_exchange_strong(
            expected,
            node,
            std::memory_order_release,
            std::memory_order_relaxed)))
      {
        // Lost the race: tombstone and recycle.
        SamplerGlobals::list().remove(node);
        SamplerGlobals::pool().release(node);
        return;
      }

      // The node is now fully published, so tell any streaming subscribers.
      // Alloc events only, so a subscriber sees exactly one event per
      // sampled allocation. The guard makes a handler that allocates
      // short-circuit instead of recursing back through here.
      {
        ReentrancyGuard broadcast_guard;
        AllocationSampleList::global().broadcast(*node);
      }
    }
  }

  /**
   * In-place resize hook, called from `snmalloc::libc::realloc` when the
   * pointer is unchanged and the new size stays in the same sizeclass.
   *
   * Only allocations that were already sampled are updated: re-rolling the
   * sampler on a resize would double-count and bias the estimator. The
   * stored `weight` and `sample_interval_at_capture` stay tied to the
   * original allocation, while snapshots see the newest size.
   *
   * Out-of-place realloc is not hooked. It is an alloc, a memcpy and a
   * dealloc, which the alloc and dealloc hooks already describe.
   */
  template<typename Config>
  SNMALLOC_FAST_PATH_INLINE void record_realloc(
    void* p, size_t new_requested_size, size_t new_allocated_size) noexcept
  {
    if constexpr (!config_has_profile_slot_v<Config>)
    {
      (void)p;
      (void)new_requested_size;
      (void)new_allocated_size;
      return;
    }
    else
    {
      if (SNMALLOC_UNLIKELY(p == nullptr))
        return;

      // A streaming handler that reallocs would land back here; bail rather
      // than recurse.
      if (sampler_reentered())
        return;

      ReentrancyGuard guard;

      // No lazy install here: if the original allocation was not sampled
      // there is nothing to update.
      ProfileSlot* slot = find_profile_slot<Config>(p);
      if (slot == nullptr)
        return;

      SampledAlloc* node = slot->load(std::memory_order_acquire);
      if (node == nullptr)
      {
        // The slab has slots but this object was not sampled.
        return;
      }

      node->requested_size.store(new_requested_size, std::memory_order_relaxed);
      node->allocated_size.store(new_allocated_size, std::memory_order_relaxed);

      // Broadcast from a local copy tagged `Resize`; the stored node stays
      // `Alloc` because only its size changed. Copy the payload only: the
      // list links and state belong to the live node.
      SampledAlloc resize_event;
      resize_event.alloc_addr = node->alloc_addr;
      resize_event.requested_size = new_requested_size;
      resize_event.allocated_size = new_allocated_size;
      resize_event.weight = node->weight;
      resize_event.sample_interval_at_capture =
        node->sample_interval_at_capture;
      resize_event.tid = node->tid;
      resize_event.alloc_seq = node->alloc_seq;
      resize_event.stack_depth = node->stack_depth;
      for (size_t i = 0; i < MaxStackFrames; ++i)
        resize_event.stack[i] = node->stack[i];
      resize_event.kind = static_cast<uint8_t>(SampledAllocKind::Resize);

      AllocationSampleList::global().broadcast(resize_event);
    }
  }

#ifdef SNMALLOC_PROFILE
  // Definitions of the entry points declared in profile/hooks.h, which is
  // also where the non-profile build's no-op versions live.
  template<typename Config>
  SNMALLOC_FAST_PATH_INLINE void
  prepare_alloc(size_t requested, size_t allocated) noexcept
  {
#  if defined(SNMALLOC_PROFILE_SECONDARY_STORAGE) && \
    !defined(SNMALLOC_PROFILE_REFILL_SAMPLING)
    spike::prepare_current_countdown_sample(requested, allocated);
#  else
    UNUSED(requested, allocated);
#  endif
  }

  template<typename Config>
  SNMALLOC_FAST_PATH_INLINE void
  on_alloc(void* p, size_t requested, size_t allocated) noexcept
  {
#  if defined(SNMALLOC_PROFILE_REFILL_SAMPLING) || \
    defined(SNMALLOC_PROFILE_SECONDARY_STORAGE)
    UNUSED(p, requested, allocated);
#  else
    record_alloc<Config>(p, requested, allocated);
#  endif
  }

  template<typename Config>
  SNMALLOC_FAST_PATH_INLINE void
  on_realloc(void* p, size_t requested, size_t allocated) noexcept
  {
#  if defined(SNMALLOC_PROFILE_SECONDARY_STORAGE)
    UNUSED(p, requested, allocated);
#  else
    record_realloc<Config>(p, requested, allocated);
#  endif
  }

  template<typename Config>
  SNMALLOC_FAST_PATH_INLINE void
  on_dealloc(void* p, const typename Config::PagemapEntry& entry) noexcept
  {
#  if defined(SNMALLOC_PROFILE_SECONDARY_STORAGE)
    UNUSED(p);
    UNUSED(entry);
#  else
    record_dealloc<Config>(p, entry);
#  endif
  }
#endif
} // namespace snmalloc::profile
