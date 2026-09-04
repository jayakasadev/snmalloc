#pragma once

#include "../ds/ds.h"
#include "../global/runtime_config.h"
#include "../mem/mem.h"
#include "../pal/pal.h"
#include "buddy.h"
#include "empty_range.h"
#include "range_helpers.h"
#include "snmalloc/stl/array.h"
#include "snmalloc/stl/atomic.h"

namespace snmalloc
{
  /**
   * Counts of free chunks by log2 size, summed across every
   * `LargeBuddyRange` in the process (the shared `GlobalR` plus each
   * thread's local cache).  Bucket index is
   * `log2(block_size) - MIN_CHUNK_BITS`.
   *
   * Only the first `NUM_BUCKETS` sizes are tracked, matching the space
   * available in `FullAllocStats.reserved[]`.  Larger chunks are simply not
   * counted.
   *
   * Updates are relaxed and unsynchronised with respect to each other, so a
   * reader can catch a consolidation halfway through and see one bucket low
   * while its neighbour reads high.
   */
  struct LargeBuddyFreeChunkHistogram
  {
    /** Number of log2 buckets exposed through the FFI struct. */
    static constexpr size_t NUM_BUCKETS = 16;

    /** Per-bucket free-block count. */
    static inline stl::Atomic<size_t> counts[NUM_BUCKETS]{};

    /**
     * Count one free block arriving at absolute log2 size `size_bits`.  Sizes
     * outside the tracked window are ignored.
     *
     * A no-op unless SNMALLOC_STATS_BASIC is defined.
     */
    static void on_add(size_t size_bits)
    {
#ifdef SNMALLOC_STATS_BASIC
      auto rel = size_bits - MIN_CHUNK_BITS;
      if (rel < NUM_BUCKETS)
      {
        counts[rel].fetch_add(1, stl::memory_order_relaxed);
      }
#else
      (void)size_bits;
#endif
    }

    /**
     * Count one free block leaving absolute log2 size `size_bits`.  Sizes
     * outside the tracked window are ignored.
     *
     * A no-op unless SNMALLOC_STATS_BASIC is defined.
     */
    static void on_remove(size_t size_bits)
    {
#ifdef SNMALLOC_STATS_BASIC
      auto rel = size_bits - MIN_CHUNK_BITS;
      if (rel < NUM_BUCKETS)
      {
        // Floor the subtraction at zero: a block can be added outside the
        // tracked window and removed from inside it, so removals are not
        // guaranteed to be matched by an earlier add.
        auto prev = counts[rel].load(stl::memory_order_relaxed);
        while (true)
        {
          auto next = (prev > 0) ? (prev - 1) : 0;
          if (counts[rel].compare_exchange_weak(
                prev, next, stl::memory_order_relaxed))
          {
            break;
          }
        }
      }
#else
      (void)size_bits;
#endif
    }

    /**
     * Copy the counts into `out`.  Each bucket is loaded separately, so the
     * result is not a consistent point-in-time view.
     */
    static void snapshot(uint64_t (&out)[NUM_BUCKETS])
    {
      for (size_t i = 0; i < NUM_BUCKETS; ++i)
      {
        out[i] =
          static_cast<uint64_t>(counts[i].load(stl::memory_order_relaxed));
      }
    }
  };

  /**
   * Class for using the pagemap entries for the buddy allocator.
   */
  template<SNMALLOC_CONCEPT(IsWritablePagemap) Pagemap>
  class BuddyChunkRep
  {
  public:
    /*
     * The values we store in our rbtree are the addresses of (combined spans
     * of) chunks of the address space; as such, bits in (MIN_CHUNK_SIZE - 1)
     * are unused and so the RED_BIT is packed therein.  However, in practice,
     * these are not "just any" uintptr_t-s, but specifically the uintptr_t-s
     * inside the Pagemap's BackendAllocator::Entry structures.
     *
     * The BackendAllocator::Entry provides us with helpers that guarantee that
     * we use only the bits that we are allowed to.
     * @{
     */
    using Handle = MetaEntryBase::BackendStateWordRef;
    using Contents = uintptr_t;
    ///@}

    /**
     * The bit that we will use to mark an entry as red.
     * This has constraints in two directions, it must not be one of the
     * reserved bits from the perspective of the meta entry and it must not be
     * a bit that is a valid part of the address of a chunk.
     * @{
     */
    static constexpr address_t RED_BIT = 1 << 8;

    static_assert(RED_BIT < MIN_CHUNK_SIZE);
    static_assert(MetaEntryBase::is_backend_allowed_value(
      MetaEntryBase::Word::One, RED_BIT));
    static_assert(MetaEntryBase::is_backend_allowed_value(
      MetaEntryBase::Word::Two, RED_BIT));
    ///@}

    /**
     * The bit marking a free chunk whose memory has already been handed back
     * to the OS with `PAL::notify_not_using`, so a later decommit pass can
     * skip it.
     *
     * Stored in the same low-bit region as `RED_BIT` and subject to the same
     * two constraints: it must not overlap a valid chunk address bit, and it
     * must be a bit the meta entry lets the backend use.
     * @{
     */
    static constexpr address_t DECOMMITTED_BIT = 1 << 9;

    static_assert(DECOMMITTED_BIT < MIN_CHUNK_SIZE);
    static_assert(DECOMMITTED_BIT != RED_BIT);
    static_assert(MetaEntryBase::is_backend_allowed_value(
      MetaEntryBase::Word::One, DECOMMITTED_BIT));
    static_assert(MetaEntryBase::is_backend_allowed_value(
      MetaEntryBase::Word::Two, DECOMMITTED_BIT));
    ///@}

    /// Both flag bits.  `set` preserves exactly these bits; `get` strips
    /// exactly these bits.
    static constexpr address_t FLAG_BITS = RED_BIT | DECOMMITTED_BIT;

    /// The value of a null node, as returned by `get`
    static constexpr Contents null = 0;
    /// The value of a null node, as stored in a `uintptr_t`.
    static constexpr Contents root = 0;

    /**
     * Set the value.  Preserve the red/black colour and decommitted flag.
     */
    static void set(Handle ptr, Contents r)
    {
      ptr = r | (static_cast<address_t>(ptr.get()) & FLAG_BITS);
    }

    /**
     * Returns the value, stripping out the red/black colour and
     * decommitted flag.
     */
    static Contents get(const Handle ptr)
    {
      return ptr.get() & ~FLAG_BITS;
    }

    /**
     * Returns a pointer to the tree node for the specified address.
     */
    static Handle ref(bool direction, Contents k)
    {
      // Special case for accessing the null entry.  We want to make sure
      // that this is never modified by the back end, so we make it point to
      // a constant entry and use the MMU to trap even in release modes.
      static const Contents null_entry = 0;
      if (SNMALLOC_UNLIKELY(address_cast(k) == 0))
      {
        return {const_cast<Contents*>(&null_entry)};
      }
      auto& entry = Pagemap::template get_metaentry_mut<false>(address_cast(k));
      if (direction)
        return entry.get_backend_word(Pagemap::Entry::Word::One);

      return entry.get_backend_word(Pagemap::Entry::Word::Two);
    }

    static bool is_red(Contents k)
    {
      return (ref(true, k).get() & RED_BIT) == RED_BIT;
    }

    static void set_red(Contents k, bool new_is_red)
    {
      if (new_is_red != is_red(k))
      {
        auto v = ref(true, k);
        v = v.get() ^ RED_BIT;
      }
      SNMALLOC_ASSERT(is_red(k) == new_is_red);
    }

    /**
     * Has this chunk's memory already been returned to the OS?
     */
    static bool is_decommitted(Contents k)
    {
      return (ref(true, k).get() & DECOMMITTED_BIT) == DECOMMITTED_BIT;
    }

    /**
     * Set or clear the decommitted flag for this chunk.
     */
    static void set_decommitted(Contents k, bool new_is_decommitted)
    {
      if (new_is_decommitted != is_decommitted(k))
      {
        auto v = ref(true, k);
        v = v.get() ^ DECOMMITTED_BIT;
      }
      SNMALLOC_ASSERT(is_decommitted(k) == new_is_decommitted);
    }

    /**
     * Called by `Buddy::remove_buddy` just before chunk `k` is merged with its
     * buddy into one bigger block.  Recommits `k` and clears its flag.
     *
     * This maintains the invariant every user of `DECOMMITTED_BIT` relies on:
     * a chunk is either entirely committed or entirely decommitted, never a
     * mix.  A merged block has only one flag, so if a decommitted chunk merged
     * with a committed buddy the result would claim to be committed while half
     * its memory was not, and a later full-size `notify_not_using` would
     * decommit memory that was already released.
     *
     * `size` is the pre-merge size of `k`, so this recommits only `k`'s own
     * memory and not the larger merged block.
     */
    static void on_consolidate(Contents k, size_t size)
    {
      if (is_decommitted(k))
      {
        DefaultPal::template notify_using<NoZero>(
          reinterpret_cast<void*>(k), size);
        set_decommitted(k, false);
      }
    }

    /**
     * Called by `Buddy::remove_block` just before `whole` is split into two
     * separately-tracked halves: `whole` shrunk to `size`, and `second`.
     *
     * Copies `whole`'s decommitted flag onto `second`, which shares the same
     * physical state but has its own, stale, flag bit.  `whole` keeps the flag
     * it already had.
     */
    static void on_split(Contents whole, Contents second, size_t size)
    {
      UNUSED(size);
      set_decommitted(second, is_decommitted(whole));
    }

    static Contents offset(Contents k, size_t size)
    {
      return k + size;
    }

    static Contents buddy(Contents k, size_t size)
    {
      return k ^ size;
    }

    static Contents align_down(Contents k, size_t size)
    {
      return k & ~(size - 1);
    }

    static bool compare(Contents k1, Contents k2)
    {
      return k1 > k2;
    }

    static bool equal(Contents k1, Contents k2)
    {
      return k1 == k2;
    }

    static uintptr_t printable(Contents k)
    {
      return k;
    }

    /**
     * Convert the pointer wrapper into something that the snmalloc debug
     * printing code can print.
     */
    static address_t printable(Handle k)
    {
      return k.printable_address();
    }

    /**
     * Returns the name for use in debugging traces.  Not used in normal builds
     * (release or debug), only when tracing is enabled.
     */
    static const char* name()
    {
      return "BuddyChunkRep";
    }

    static bool can_consolidate(Contents k, size_t size)
    {
      // Need to know both entries exist in the pagemap.
      // This must only be called if that has already been
      // ascertained.
      // The buddy could be in a part of the pagemap that has
      // not been registered and thus could segfault on access.
      auto larger = bits::max(k, buddy(k, size));
      auto& entry =
        Pagemap::template get_metaentry_mut<false>(address_cast(larger));
      return !entry.is_boundary();
    }
  };

  /**
   * Used to represent a consolidating range of memory.  Uses a buddy allocator
   * to consolidate adjacent blocks.
   *
   * ParentRange - Represents the range to get memory from to fill this range.
   *
   * REFILL_SIZE_BITS - Maximum size of a refill, may ask for less during warm
   * up phase.
   *
   * MAX_SIZE_BITS - Maximum size that this range will store.
   *
   * Pagemap - How to access the pagemap, which is used to store the red black
   * tree nodes for the buddy allocators.
   *
   * MIN_REFILL_SIZE_BITS - The minimum size that the ParentRange can be asked
   * for
   *
   * MANAGES_COMMITTED_MEMORY - Whether chunks held in this instance's cache
   * have been committed (passed to `PAL::notify_using`) before arriving here.
   * True for caches downstream of a `CommitRange`, such as the per-thread
   * local caches.  False for caches of reserved-but-never-committed address
   * space, such as `GlobalR`.
   *
   * This must be correct: the decay policies below call
   * `PAL::notify_not_using` / `notify_using` on cached chunks, which is only
   * valid on committed memory.  When false, both decay paths are compiled out
   * with `if constexpr` and nothing is ever decommitted.
   */
  template<
    size_t REFILL_SIZE_BITS,
    size_t MAX_SIZE_BITS,
    SNMALLOC_CONCEPT(IsWritablePagemap) Pagemap,
    size_t MIN_REFILL_SIZE_BITS = 0,
    bool MANAGES_COMMITTED_MEMORY = true>
  class LargeBuddyRange
  {
    static_assert(
      REFILL_SIZE_BITS <= MAX_SIZE_BITS, "REFILL_SIZE_BITS > MAX_SIZE_BITS");
    static_assert(
      MIN_REFILL_SIZE_BITS <= REFILL_SIZE_BITS,
      "MIN_REFILL_SIZE_BITS > REFILL_SIZE_BITS");

    /**
     * Maximum size of a refill
     */
    static constexpr size_t REFILL_SIZE = bits::one_at_bit(REFILL_SIZE_BITS);

    /**
     * Minimum size of a refill
     */
    static constexpr size_t MIN_REFILL_SIZE =
      bits::one_at_bit(MIN_REFILL_SIZE_BITS);

  public:
    template<typename ParentRange = EmptyRange<>>
    class Type : public ContainsParent<ParentRange>
    {
      using ContainsParent<ParentRange>::parent;

      /**
       * The size of memory requested so far.
       *
       * This is used to determine the refill size.
       */
      size_t requested_total = 0;

      /**
       * Node representation for `buddy_large`.
       */
      using Rep = BuddyChunkRep<Pagemap>;

      /**
       * Buddy allocator used to represent this range of memory.
       *
       * The fourth template argument makes every insertion and removal update
       * `LargeBuddyFreeChunkHistogram`.
       */
      Buddy<Rep, MIN_CHUNK_BITS, MAX_SIZE_BITS, LargeBuddyFreeChunkHistogram>
        buddy_large;

      /**
       * State for the timed decay sweep, used when `decay_rate_ms() > 0`.
       * (`decay_rate_ms() == 0` decommits immediately in `dealloc_range` and
       * ignores all of this.)
       *
       * This state belongs to one `Type` instance and describes only its own
       * `buddy_large`.  Several instances are live at once -- the shared
       * `GlobalR` plus one local cache per thread -- and their free chunks are
       * unrelated, so their idle clocks must be too.
       *
       * These fields are only touched from `alloc_range` / `dealloc_range`, so
       * they inherit `buddy_large`'s synchronisation: the shared instance is
       * inside the `LockRange` that `GlobalRange` wraps it in, and a
       * per-thread cache is only ever touched by its owning thread.
       * @{
       */

      /**
       * `PAL::time_in_ms()` at which a chunk was last added to each bucket of
       * `buddy_large`, indexed as `Buddy::to_index` indexes its own entries,
       * so `last_touched_ms[idx]` covers chunks of size
       * `1 << (MIN_CHUNK_BITS + idx)`.
       *
       * Zero means "never touched", which is indistinguishable from "touched
       * at time zero".  That only matters for the first `decay_rate_ms()` of
       * process life, when sweeping an empty bucket does nothing.
       */
      stl::Array<uint64_t, decltype(buddy_large)::bucket_count()>
        last_touched_ms{};

      /**
       * `PAL::time_in_ms()` at which the next sweep of `last_touched_ms` is
       * due.  Compared on every `alloc_range` / `dealloc_range` call, so the
       * not-yet-due case must stay a single comparison.
       */
      uint64_t next_sweep_due_ms = 0;

      /**
       * Record that a chunk was just added to bucket `idx`.  `idx` is a bucket
       * index relative to `MIN_CHUNK_BITS`, not the absolute log2 size that
       * `Buddy::add_block` reports through `landed_size_bits_out`.
       */
      void touch_bucket(size_t idx)
      {
        if constexpr (MANAGES_COMMITTED_MEMORY && pal_supports<Time, DefaultPal>)
        {
          last_touched_ms[idx] = DefaultPal::time_in_ms();
        }
        else
        {
          UNUSED(idx);
        }
      }

      /**
       * If a sweep is due, decommit every chunk in every bucket that has been
       * idle for at least `decay_rate_ms()`, then schedule the next sweep.
       *
       * There is no background thread: this is called at the end of both
       * `alloc_range` and `dealloc_range`, and returns after a few comparisons
       * when no sweep is due.  Compiled out when `MANAGES_COMMITTED_MEMORY` is
       * false.
       */
      void maybe_sweep_decay()
      {
        if constexpr (
          MANAGES_COMMITTED_MEMORY && pal_supports<Time, DefaultPal>)
        {
          auto decay_ms = RuntimeConfig::decay_rate_ms();
          if (decay_ms == 0)
          {
            // `dealloc_range` already decommitted eagerly.
            return;
          }

          auto now = DefaultPal::time_in_ms();
          if (now < next_sweep_due_ms)
            return;

          for (size_t idx = 0; idx < last_touched_ms.size(); idx++)
          {
            // Written as a subtraction rather than
            // `last_touched_ms[idx] + decay_ms < now` so the addition cannot
            // overflow.
            if (now - last_touched_ms[idx] < decay_ms)
              continue;

            buddy_large.for_each_in_bucket(
              idx, [idx](typename Rep::Contents addr) {
                if (Rep::is_decommitted(addr))
                  return;

                auto size = bits::one_at_bit(MIN_CHUNK_BITS + idx);
                DefaultPal::notify_not_using(
                  reinterpret_cast<void*>(addr), size);
                Rep::set_decommitted(addr, true);
              });
          }

          next_sweep_due_ms = now + decay_ms;
        }
      }
      ///@}

      /**
       * The parent might not support deallocation if this buddy allocator
       * covers the whole range.  Uses template insanity to make this work.
       */
      template<bool exists = MAX_SIZE_BITS != (bits::BITS - 1)>
      stl::enable_if_t<exists>
      parent_dealloc_range(capptr::Arena<void> base, size_t size)
      {
        static_assert(
          MAX_SIZE_BITS != (bits::BITS - 1), "Don't set SFINAE parameter");
        parent.dealloc_range(base, size);
      }

      void dealloc_overflow(capptr::Arena<void> overflow)
      {
        if constexpr (MAX_SIZE_BITS != (bits::BITS - 1))
        {
          if (overflow != nullptr)
          {
            parent.dealloc_range(overflow, bits::one_at_bit(MAX_SIZE_BITS));
          }
        }
        else
        {
          if (overflow != nullptr)
            abort();
        }
      }

      /**
       * Add a range of memory to the address space.
       * Divides blocks into power of two sizes with natural alignment
       */
      void add_range(capptr::Arena<void> base, size_t length)
      {
        range_to_pow_2_blocks<MIN_CHUNK_BITS>(
          base, length, [this](capptr::Arena<void> base, size_t align, bool) {
            size_t landed_size_bits = 0;
            auto overflow = capptr::Arena<void>::unsafe_from(
              reinterpret_cast<void*>(buddy_large.add_block(
                base.unsafe_uintptr(), align, &landed_size_bits)));

            if (overflow == nullptr)
            {
              // The chunk stayed in this buddy's cache, so restart its
              // bucket's idle clock.
              touch_bucket(landed_size_bits - MIN_CHUNK_BITS);
            }

            dealloc_overflow(overflow);
          });
      }

      capptr::Arena<void> refill(size_t size)
      {
        if (ParentRange::Aligned)
        {
          // Use amount currently requested to determine refill size.
          // This will gradually increase the usage of the parent range.
          // So small examples can grow local caches slowly, and larger
          // examples will grow them by the refill size.
          //
          // The heuristic is designed to allocate the following sequence for
          // 16KiB requests 16KiB, 16KiB, 32Kib, 64KiB, ..., REFILL_SIZE/2,
          // REFILL_SIZE, REFILL_SIZE, ... Hence if this if they are coming from
          // a contiguous aligned range, then they could be consolidated.  This
          // depends on the ParentRange behaviour.
          size_t refill_size = bits::min(REFILL_SIZE, requested_total);
          refill_size = bits::max(refill_size, MIN_REFILL_SIZE);
          refill_size = bits::max(refill_size, size);
          refill_size = bits::next_pow2(refill_size);

          auto refill_range = parent.alloc_range(refill_size);
          if (refill_range != nullptr)
          {
            requested_total += refill_size;
            add_range(pointer_offset(refill_range, size), refill_size - size);
          }
          return refill_range;
        }

        // Note the unaligned parent path does not use
        // requested_total in the heuristic for the initial size
        // this is because the request needs to introduce alignment.
        // Currently the unaligned variant is not used as a local cache.
        // So the gradual growing of refill_size is not needed.

        // Need to overallocate to get the alignment right.
        bool overflow = false;
        size_t needed_size = bits::umul(size, 2, overflow);
        if (overflow)
        {
          return nullptr;
        }

        auto refill_size = bits::max(needed_size, REFILL_SIZE);
        while (needed_size <= refill_size)
        {
          auto refill = parent.alloc_range(refill_size);

          if (refill != nullptr)
          {
            requested_total += refill_size;
            add_range(refill, refill_size);

            SNMALLOC_ASSERT(refill_size < bits::one_at_bit(MAX_SIZE_BITS));
            static_assert(
              (REFILL_SIZE < bits::one_at_bit(MAX_SIZE_BITS)) ||
                ParentRange::Aligned,
              "Required to prevent overflow.");

            return alloc_range(size);
          }

          refill_size >>= 1;
        }

        return nullptr;
      }

    public:
      static constexpr bool Aligned = true;

      static constexpr bool ConcurrencySafe = false;

      /* The large buddy allocator always deals in Arena-bounded pointers. */
      using ChunkBounds = capptr::bounds::Arena;
      static_assert(
        stl::is_same_v<typename ParentRange::ChunkBounds, ChunkBounds>);

      constexpr Type() = default;

      capptr::Arena<void> alloc_range(size_t size)
      {
        SNMALLOC_ASSERT(size >= MIN_CHUNK_SIZE);
        SNMALLOC_ASSERT(bits::is_pow2(size));

        if (size >= bits::mask_bits(MAX_SIZE_BITS))
        {
          if (ParentRange::Aligned)
            return parent.alloc_range(size);

          return nullptr;
        }

        auto result = capptr::Arena<void>::unsafe_from(
          reinterpret_cast<void*>(buddy_large.remove_block(size)));

        if (result != nullptr)
        {
          if constexpr (MANAGES_COMMITTED_MEMORY)
          {
            // This chunk came out of the buddy cache, where either decay
            // policy may have decommitted it, so recommit before handing it
            // out.  Called unconditionally because it is idempotent and
            // cheaper than always tracking which chunks need it.
            DefaultPal::template notify_using<NoZero>(
              result.unsafe_ptr(), size);

            // Clear the flag to match.  The current use is already safe
            // thanks to the `notify_using` above; this is for the next time
            // the chunk is freed and goes idle.  A stale set flag would make
            // a later sweep skip decommitting memory that is actually
            // resident, holding on to RSS indefinitely.
            if (Rep::is_decommitted(result.unsafe_uintptr()))
              Rep::set_decommitted(result.unsafe_uintptr(), false);

            maybe_sweep_decay();
          }
          return result;
        }

        result = refill(size);
        maybe_sweep_decay();
        return result;
      }

      void dealloc_range(capptr::Arena<void> base, size_t size)
      {
        SNMALLOC_ASSERT(size >= MIN_CHUNK_SIZE);
        SNMALLOC_ASSERT(bits::is_pow2(size));

        if constexpr (MAX_SIZE_BITS != (bits::BITS - 1))
        {
          if (size >= bits::mask_bits(MAX_SIZE_BITS))
          {
            parent_dealloc_range(base, size);
            return;
          }
        }

        size_t landed_size_bits = 0;
        auto overflow =
          capptr::Arena<void>::unsafe_from(reinterpret_cast<void*>(
            buddy_large.add_block(
              base.unsafe_uintptr(), size, &landed_size_bits)));

        if (overflow == nullptr)
        {
          if constexpr (MANAGES_COMMITTED_MEMORY)
          {
            if (RuntimeConfig::decay_rate_ms() == 0)
            {
              // Immediate decay: hand the pages back now rather than leaving
              // them committed in the cache.  Safe to do while the chunk is
              // still linked in, because the buddy allocator keeps its
              // tree/cache bookkeeping in the pagemap rather than in the
              // chunk's own memory.
              //
              // Must use the address and size the block landed at, not the
              // `base` and `size` passed in.  `add_block` may have merged
              // `base` into a bigger block, in which case the live node is a
              // different, lower address spanning more memory, and
              // decommitting `size` bytes at `base` would release part of
              // that node while leaving its flag clear -- breaking the
              // all-or-nothing invariant `on_consolidate` depends on.
              auto landed_size = bits::one_at_bit(landed_size_bits);
              auto landed_base =
                Rep::align_down(base.unsafe_uintptr(), landed_size);
              DefaultPal::notify_not_using(
                reinterpret_cast<void*>(landed_base), landed_size);

              // Record the state so a later merge or split reconciles it, and
              // so `alloc_range` recommits the chunk when it is reused.
              Rep::set_decommitted(landed_base, true);
            }
            else
            {
              // Timed decay: start this bucket's idle clock.  Not needed on
              // the branch above, where the chunk is already decommitted.
              touch_bucket(landed_size_bits - MIN_CHUNK_BITS);
            }
          }
        }

        dealloc_overflow(overflow);
        maybe_sweep_decay();
      }

      /**
       * Copy the free-chunk counts into `out`, where `out[i]` counts free
       * chunks of size `1 << (MIN_CHUNK_BITS + i)` bytes.  Larger sizes are
       * not tracked.
       *
       * The counts are process-global, so the result is the same regardless of
       * which instance is asked.  Read-only and safe from any thread.
       */
      void get_free_chunk_count_by_log_size(
        uint64_t (&out)[LargeBuddyFreeChunkHistogram::NUM_BUCKETS]) const
      {
        LargeBuddyFreeChunkHistogram::snapshot(out);
      }
    };
  };
} // namespace snmalloc
