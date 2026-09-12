#pragma once

#include "../mem/mem.h"

namespace snmalloc
{
  /**
   * Options for a specific snmalloc configuration.  Every globals object must
   * have one `constexpr` instance of this class called `Options`.  This should
   * be constructed to explicitly override any of the defaults.  A
   * configuration that does not need to override anything would simply declare
   * this as a field of the global object:
   *
   * ```c++
   * static constexpr snmalloc::Flags Options{};
   * ```
   *
   * A global configuration that wished to use out-of-line message queues but
   * accept the defaults for everything else would instead do this:
   *
   * ```c++
   *     static constexpr snmalloc::Flags Options{.IsQueueInline = false};
   * ```
   *
   * To maintain backwards source compatibility in future versions, any new
   * option added here should have its default set to be whatever snmalloc was
   * doing before the new option was added.
   */
  struct Flags
  {
    /**
     * Should allocators have inline message queues?  If this is true then
     * the `Allocator` is responsible for allocating the
     * `RemoteAllocator` that contains its message queue.  If this is false
     * then the `RemoteAllocator` must be separately allocated and provided
     * to the `Allocator` before it is used.
     */
    bool IsQueueInline = true;

    /**
     * Does the `Allocator` own a `Backend::LocalState` object?  If this is
     * true then the `Allocator` is responsible for allocating and
     * deallocating a local state object, otherwise the surrounding code is
     * responsible for creating it.
     */
    bool AllocOwnsLocalState = true;

    /**
     * Are `Allocator` allocated by the pool allocator?  If not then the
     * code embedding this snmalloc configuration is responsible for allocating
     * `Allocator` instances.
     */
    bool AllocIsPoolAllocated = true;

    /**
     * Are the front and back pointers to the message queue in a RemoteAllocator
     * considered to be capptr_bounds::Wildness::Tame (as opposed to Wild)?
     * That is, is it presumed that clients or other potentialadversaries cannot
     * access the front and back pointers themselves, even if they can access
     * the queue nodes themselves (which are always considered Wild)?
     */
    bool QueueHeadsAreTame = true;

    /**
     * Does the backend provide a capptr_domesticate function to sanity check
     * pointers? If so it will be called when untrusted pointers are consumed
     * (on dealloc and in freelists) otherwise a no-op version is provided.
     */
    bool HasDomesticate = false;
  };

  struct NoClientMetaDataProvider
  {
    using StorageType = Empty;
    using DataRef = Empty&;

    static size_t required_count(size_t)
    {
      return 1;
    }

    static DataRef get(StorageType* base, size_t)
    {
      return *base;
    }
  };

  template<typename T>
  struct ArrayClientMetaDataProvider
  {
    using StorageType = T;
    using DataRef = T&;

    static size_t required_count(size_t max_count)
    {
      return max_count;
    }

    static DataRef get(StorageType* base, size_t index)
    {
      return base[index];
    }
  };

  /**
   * Lazy variant of `ArrayClientMetaDataProvider<T>`.
   *
   * Every slab pays one `stl::Atomic<T*>`; the `slab_object_count *
   * sizeof(T)` array behind it is only allocated the first time that slab is
   * touched. Intended for metadata that most slabs never need, such as
   * heap-profiling samples.
   *
   * Installation goes straight to the platform layer, never through the
   * frontend allocator, so it cannot recurse into user `malloc`. Two threads
   * touching a slab at once are resolved by compare-and-swap, with the loser
   * decommitting its mapping. There is no portable `Pal::release`, so the
   * virtual reservation is held for the life of the slab.
   */
  template<typename T>
  struct LazyArrayClientMetaDataProvider
  {
    /**
     * Per-slab storage: a pointer to the backing array, null until it is
     * installed. The object count is not cached here; it comes from the
     * pagemap sizeclass and is passed to `get`.
     */
    struct StorageType
    {
      stl::Atomic<T*> backing{nullptr};
    };

    static_assert(
      sizeof(StorageType) == sizeof(void*),
      "LazyArrayClientMetaDataProvider::StorageType must be exactly one "
      "pointer wide");

    using DataRef = T&;

    /**
     * One pointer per slab, whatever the slab's object count.
     */
    static constexpr size_t required_count(size_t /*max_count*/)
    {
      return 1;
    }

    /**
     * `notify_using` needs a page-aligned base and length when zeroing. Both
     * it and the matching decommit use this rounded size, so the two stay
     * balanced.
     */
    static constexpr size_t round_to_page(size_t bytes)
    {
      return bits::align_up(bytes, DefaultPal::page_size);
    }

    /**
     * Install a zeroed backing array for this slab and publish it, or
     * return the pointer of whichever thread got there first. Returns
     * nullptr if the platform could not give us the memory.
     */
    SNMALLOC_SLOW_PATH static T*
    install(StorageType* base, size_t slab_object_count)
    {
      const size_t raw_bytes = slab_object_count * sizeof(T);
      const size_t alloc_bytes = round_to_page(raw_bytes);

      void* p = DefaultPal::reserve(alloc_bytes);
      if (SNMALLOC_UNLIKELY(p == nullptr))
        return nullptr;

      // YesZero so every slot reads as zero; on Windows this also commits
      // the pages.
      if (SNMALLOC_UNLIKELY(
            !DefaultPal::template notify_using<YesZero>(p, alloc_bytes)))
        return nullptr;

      auto* fresh = static_cast<T*>(p);
      T* expected = nullptr;
      if (base->backing.compare_exchange_strong(
            expected,
            fresh,
            stl::memory_order_acq_rel,
            stl::memory_order_acquire))
      {
        return fresh;
      }

      // Lost the race: hand our pages back and use the winner's array. The
      // virtual reservation itself is leaked.
      DefaultPal::notify_not_using(p, alloc_bytes);
      return expected;
    }

    /**
     * `slab_object_count` sizes the backing array on first touch; callers
     * get it from the pagemap sizeclass. The extra argument means this
     * signature is not interchangeable with the other providers' `get`.
     */
    static DataRef
    get(StorageType* base, size_t index, size_t slab_object_count)
    {
      T* buf = base->backing.load(stl::memory_order_acquire);
      if (SNMALLOC_UNLIKELY(buf == nullptr))
        buf = install(base, slab_object_count);
      return buf[index];
    }
  };

  /**
   * Class containing definitions that are likely to be used by all except for
   * the most unusual back-end implementations.  This can be subclassed as a
   * convenience for back-end implementers, but is not required.
   */
  class CommonConfig
  {
  public:
    /**
     * Special remote that should never be used as a real remote.
     * This is used to initialise allocators that should always hit the
     * remote path for deallocation. Hence moving a branch off the critical
     * path.
     */
    SNMALLOC_REQUIRE_CONSTINIT
    inline static RemoteAllocator unused_remote;
  };

  template<typename PAL>
  static constexpr size_t MinBaseSizeBits()
  {
    if constexpr (pal_supports<AlignedAllocation, PAL>)
    {
      return bits::next_pow2_bits_const(PAL::minimum_alloc_size);
    }
    else
    {
      return MIN_CHUNK_BITS;
    }
  }
} // namespace snmalloc

#include "../mem/remotecache.h"
