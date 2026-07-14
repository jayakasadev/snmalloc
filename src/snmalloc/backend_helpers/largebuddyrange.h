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
   * Process-global log2-bucketed histogram of free chunks held inside
   * `LargeBuddyRange` instances.
   *
   * snmalloc has several `LargeBuddyRange` instantiations active at
   * runtime: the process-singleton `GlobalR` (lifted via
   * `GlobalRange`/`StaticRange`) and one per-thread `LargeObjectRange`
   * local cache.  This struct aggregates the free-chunk population
   * across every live `Buddy<BuddyChunkRep<...>>` instance into one
   * shared array of atomics, keyed by `log2(block_size) - MIN_CHUNK_BITS`.
   *
   * The histogram occupies the first 16 slots of
   * `FullAllocStats.reserved[]`, covering chunk sizes from
   * `MIN_CHUNK_SIZE` up to `MIN_CHUNK_SIZE << 15`.  That range is
   * sufficient for the configurations snmalloc ships -- the largest
   * cacheable size on x86-64 is `bits::BITS - 1 = 62 bits`, which
   * exceeds 16 buckets, but free chunks above `MIN_CHUNK_BITS + 15`
   * are exceedingly rare and not particularly useful for the
   * fragmentation diagnostics this histogram targets.  Buckets that
   * fall outside the 16-slot window are silently dropped (the
   * counters never decrement below zero either, matching
   * `BackendFragCounters` semantics).
   *
   * Updates are `memory_order_relaxed`: the counters are not used for
   * synchronisation, only for observability.  Both `Buddy` mutators
   * and the reader run while holding their respective locks, but the
   * histogram itself is unsynchronised; a concurrent reader may observe
   * a transient inconsistency at the moment a block consolidates from
   * bucket `idx` to `idx+1` (one bucket may read low while the other
   * reads high), which we accept for a telemetry-grade snapshot.
   */
  struct LargeBuddyFreeChunkHistogram
  {
    /** Number of log2 buckets exposed through the FFI struct. */
    static constexpr size_t NUM_BUCKETS = 16;

    /** Per-bucket free-block count. */
    static inline stl::Atomic<size_t> counts[NUM_BUCKETS]{};

    /**
     * Record one new free block entering the buddy allocator at the
     * given log-size (in absolute bits, e.g. log2 of MIN_CHUNK_SIZE
     * for the smallest chunk).  Out-of-window updates are silently
     * dropped.
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
      // Compiles to a no-op when BASIC stats are off so Buddy
      // insertion pays zero atomic overhead.
      (void)size_bits;
#endif
    }

    /**
     * Record one free block leaving the buddy allocator at the given
     * log-size.  Uses a clamped-subtract compare-exchange loop so
     * that an out-of-order observation (e.g. a buddy that consolidated
     * across a bucket the reader never saw) cannot underflow the
     * counter.
     */
    static void on_remove(size_t size_bits)
    {
#ifdef SNMALLOC_STATS_BASIC
      auto rel = size_bits - MIN_CHUNK_BITS;
      if (rel < NUM_BUCKETS)
      {
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
      // No-op when BASIC stats are off.
      (void)size_bits;
#endif
    }

    /**
     * Snapshot the histogram into `out[0..NUM_BUCKETS-1]`.  Each load
     * is independent (`memory_order_relaxed`), so the snapshot is not
     * transactional.  Suitable for fragmentation diagnostics; not
     * suitable for invariants that require an exact total.
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
     * The bit used to mark a cached chunk as having already been
     * decommitted (`PAL::notify_not_using`'d) by the time-windowed decay
     * sweep while it sits idle in the buddy allocator's own cache/tree.
     * Lets the sweep skip a chunk it already decommitted on an earlier
     * pass without re-issuing `notify_not_using` on it.  Packed into the
     * same low-bit region as `RED_BIT`, one bit further up -- subject to
     * the same two constraints (must not collide with a valid chunk
     * address bit, must be one of the bits the meta entry's back-end
     * word actually allows us to use).
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

    /// Both flag bits packed into the low bits of the stored address;
    /// `set`/`get` preserve exactly these bits and nothing else.
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
     * Has this chunk already been decommitted (`notify_not_using`'d) by
     * the time-windowed decay sweep while it sits in the buddy cache?
     * Mirrors `is_red` exactly -- same `ref(true, k)` access pattern,
     * since both flag bits live in the same backend word.
     */
    static bool is_decommitted(Contents k)
    {
      return (ref(true, k).get() & DECOMMITTED_BIT) == DECOMMITTED_BIT;
    }

    /**
     * Set or clear the decommitted flag for this chunk.  Mirrors
     * `set_red` exactly.
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
     * Called by `Buddy::remove_buddy` immediately before `k` (a chunk
     * currently sitting in the buddy cache/tree) is merged with its
     * buddy into one bigger block.
     *
     * This is the fix for a real bug found while implementing the
     * time-windowed decay sweep (see `LargeBuddyRange::maybe_sweep_decay`
     * in this file): once two chunks merge, the result is a single node
     * with a single `DECOMMITTED_BIT` -- there is no way to represent
     * "the lower half of my backing memory was `notify_not_using`'d, the
     * upper half was not".  If a decommitted chunk were allowed to merge
     * with a still-committed buddy without reconciling that asymmetry
     * first, the resulting merged node would report `is_decommitted() ==
     * false` while actually being half-unmapped/half-protected, so a
     * later full-size `notify_not_using` on that merged block would
     * double-decommit already-released memory -- exactly what produced
     * a `SIGBUS`/`SIGSEGV` crash (observed as a `memset` fault inside
     * `PALApple::notify_not_using`, and would equally corrupt accounting
     * on other PALs) during testing of this feature.
     *
     * The fix: before a decommitted chunk can be merged away, eagerly
     * recommit it (`notify_using`) and clear the flag, exactly as if it
     * had just been reused via `alloc_range`.  This keeps the invariant
     * that every chunk actually resident in the cache/tree at any given
     * moment is either fully committed (flag clear) or fully decommitted
     * (flag set) -- never a mix -- which the merge step depends on.
     * `k` is passed by the caller pre-merge, i.e. still at its original
     * `size`, so this only ever recommits exactly the memory that was
     * (possibly) decommitted, not the larger merged region.
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
     * Called by `Buddy::remove_block` immediately before a `whole` block
     * (about to be handed to a caller) is split into two independently
     * tracked halves: `whole` itself, shrunk down to `size`, and
     * `second`, the new node for the other half.
     *
     * Copies `whole`'s decommitted flag onto `second`, so both halves
     * agree with physical reality immediately after the split.  See
     * `on_consolidate` above for why leaving this flag unreconciled is
     * dangerous.
     *
     * Deliberately does not touch `whole`'s own flag -- `whole` keeps
     * whatever state it already had; only `second` needs to be brought
     * into line with it.
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
   * MANAGES_COMMITTED_MEMORY - Whether chunks sitting in this instance's own
   * buddy cache/tree are backed by memory that has already been committed
   * (i.e. `PAL::notify_using` has been called on it) before landing here.
   * Defaults to `true`, matching the per-thread local caches, which sit
   * downstream of a `CommitRange`; the process-global raw-address-space
   * caches (`GlobalR` and friends) manage reserved-but-never-committed
   * space instead and must set this to `false`.
   *
   * It matters because the decay policies below call
   * `PAL::notify_not_using`/`notify_using` directly on cached chunks, which
   * is only safe on memory that is actually committed; doing so on
   * never-committed address space is undefined behaviour and was observed
   * to crash (`SIGBUS`/`SIGSEGV`) during development. When this parameter
   * is `false`, both decay paths compile away entirely (`if constexpr`) --
   * no cost, and no decommit ever happens for that cache.
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
       * Node representation for `buddy_large`, named so the decay-sweep
       * machinery below can refer to `Rep::Contents`/`Rep::is_decommitted`
       * /`Rep::set_decommitted` without repeating the `BuddyChunkRep<Pagemap>`
       * spelling at every use.
       */
      using Rep = BuddyChunkRep<Pagemap>;

      /**
       * Buddy allocator used to represent this range of memory.
       *
       * The fourth template argument plugs in the free-chunk histogram
       * hook: every insertion/removal into the buddy cache or red-black
       * tree bumps the matching log-size bucket of
       * `LargeBuddyFreeChunkHistogram`, which
       * `get_free_chunk_count_by_log_size` then reads.
       */
      Buddy<Rep, MIN_CHUNK_BITS, MAX_SIZE_BITS, LargeBuddyFreeChunkHistogram>
        buddy_large;

      /**
       * Time-windowed decay policy state (`decay_rate_ms() > 0` case; the
       * `== 0` "decay immediately" case is handled inline in
       * `dealloc_range` and does not touch any of this).
       *
       * This state is per-`Type`-instance, deliberately -- unlike
       * `LargeBuddyFreeChunkHistogram` (a process-global singleton keyed
       * only by absolute log-size), idle-time tracking must be scoped to
       * the one `buddy_large` it describes.  There can be several live
       * `LargeBuddyRange::Type` instances at once (the process-singleton
       * `GlobalR` plus one per-thread local cache); a chunk landing in one
       * instance's bucket must not reset another instance's idle clock
       * for what is, from the sweep's point of view, a completely
       * unrelated set of free chunks.
       *
       * Both fields are touched only from `alloc_range`/`dealloc_range`,
       * i.e. only where `buddy_large` itself is touched, so they share its
       * synchronisation story exactly: for the process-global instance
       * (`GlobalR` in `standard_range.h`/`meta_protected_range.h`) that is
       * the `LockRange` a `GlobalRange` wraps it in (see
       * `backend_helpers/globalrange.h` + `lockrange.h`); a per-thread
       * `LargeObjectRange`/`MetaRange` local cache is only ever touched by
       * its owning thread and needs no lock at all.  Neither case leaves
       * these fields racing.
       * @{
       */

      /**
       * Last time (per `PAL::time_in_ms()`) a chunk was deposited into
       * each bucket of `buddy_large`, indexed the same way
       * `Buddy::to_index` indexes its own `entries` (i.e.
       * `last_touched_ms[idx]` corresponds to chunks of size
       * `1 << (MIN_CHUNK_BITS + idx)`).  Zero-initialised, meaning
       * "never touched" -- which is indistinguishable from "touched at
       * PAL startup time 0", but that only matters in the first
       * `decay_rate_ms()` milliseconds of process life, during which
       * sweeping a still-empty bucket is a harmless no-op anyway.
       */
      stl::Array<uint64_t, decltype(buddy_large)::bucket_count()>
        last_touched_ms{};

      /**
       * The next time (per `PAL::time_in_ms()`) at which it is worth
       * walking `last_touched_ms` looking for stale buckets.  Checked on
       * every `alloc_range`/`dealloc_range` call, so it must stay a cheap
       * single comparison in the common case where it has not yet
       * elapsed; the actual sweep only runs once per elapsed window, not
       * once per call.
       */
      uint64_t next_sweep_due_ms = 0;

      /**
       * Record that a chunk was just deposited into bucket `idx` of
       * `buddy_large` (`idx` in the same indexing as `Buddy::to_index`,
       * i.e. relative to `MIN_CHUNK_BITS`, NOT the absolute log-size
       * `add_block`'s `landed_size_bits_out` reports).
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
       * Time-windowed decay sweep.  Called from the tail of both
       * `alloc_range` and `dealloc_range` -- there is no dedicated
       * background thread. The fast path (immediate-decay active, the
       * PAL can't report time, or the sweep isn't due yet) is just a
       * few comparisons.  When due, it walks each stale bucket and
       * decommits any chunk not already flagged `is_decommitted`, then
       * reschedules the next sweep one window out.  No-ops entirely
       * when `MANAGES_COMMITTED_MEMORY` is `false` -- see that template
       * parameter's own documentation on `LargeBuddyRange`.
       */
      void maybe_sweep_decay()
      {
        if constexpr (
          MANAGES_COMMITTED_MEMORY && pal_supports<Time, DefaultPal>)
        {
          auto decay_ms = RuntimeConfig::decay_rate_ms();
          if (decay_ms == 0)
          {
            // Immediate-decay policy owns this case entirely (handled
            // inline in dealloc_range); nothing for the windowed sweep
            // to do.
            return;
          }

          auto now = DefaultPal::time_in_ms();
          if (now < next_sweep_due_ms)
            return;

          for (size_t idx = 0; idx < last_touched_ms.size(); idx++)
          {
            // `now - last_touched_ms[idx]` rather than
            // `last_touched_ms[idx] + decay_ms < now` to sidestep any
            // (extremely unlikely on a 64-bit ms counter, but free to
            // avoid) overflow of the addition; unsigned wraparound on
            // the subtraction gives the right answer even if `now` and
            // `last_touched_ms[idx]` are far apart.
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

          // Next sweep in one more decay window.  A full `decay_ms`
          // period (rather than some finer-grained fraction) keeps the
          // amortised cost of the walk low -- the sweep is O(number of
          // buckets), not O(number of cached chunks), so a shorter
          // period would not meaningfully improve reclaim latency for
          // any individual chunk (that is still bounded by
          // `decay_ms` from when it was last touched) while making the
          // per-call fast-path check succeed proportionally more often.
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
              // The chunk came to rest somewhere in this buddy's own
              // cache/tree; record that its bucket just gained a fresh
              // (i.e. definitely-not-idle) entry, so the decay sweep
              // gives it a full window before considering it stale.
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
            // This chunk came from the buddy allocator's own cache/tree
            // rather than fresh memory from the parent range.  If a decay
            // policy is active (immediate, i.e. `decay_rate_ms() == 0`; or
            // the time-windowed sweep below, for `decay_rate_ms() > 0`),
            // the chunk may have been decommitted while it sat in the
            // buddy cache, so unconditionally notify the PAL that we are
            // about to use it again.  This is idempotent and cheap on
            // POSIX platforms when the chunk was never actually
            // decommitted.
            //
            // Only meaningful when this instance manages committed
            // memory in the first place -- see `MANAGES_COMMITTED_MEMORY`
            // on `LargeBuddyRange`.  For a raw-address-space instance
            // (`GlobalR` and friends) neither decay path below ever runs,
            // so this call would be pure unnecessary syscall overhead on
            // every cache hit.
            DefaultPal::template notify_using<NoZero>(
              result.unsafe_ptr(), size);

            // Clear the decommitted flag, if it was set.  Correctness of
            // the *current* use of this chunk does not depend on this --
            // the unconditional `notify_using` above already made the
            // memory valid to use regardless of the flag's state.  This
            // matters for a *future* free/idle cycle of this exact chunk:
            // without clearing it here, a chunk that the sweep decommitted
            // once would permanently read as `is_decommitted() == true`
            // even after being reused and freed again as fully-resident
            // memory, causing a later sweep pass to wrongly skip
            // re-decommitting it (see `is_decommitted` and
            // `maybe_sweep_decay`'s skip check) -- silently leaking RSS
            // that should have been reclaimed.  `Rep::is_decommitted` is a
            // stateless pagemap-entry read, so checking before writing is
            // essentially free and keeps this a no-op on the common case
            // where the flag was never set.
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
            // Both decay policies below only make sense -- and are only
            // safe -- when this instance's cache/tree holds memory that
            // has actually been committed at some point (see
            // `MANAGES_COMMITTED_MEMORY` on `LargeBuddyRange`).  For a
            // raw-address-space instance (`GlobalR` and friends) this
            // whole block compiles away: the chunk was never committed,
            // so there is nothing to eagerly decommit and no bucket
            // timestamp worth tracking for a sweep that will never run
            // against it.
            if (RuntimeConfig::decay_rate_ms() == 0)
            {
              // The chunk was absorbed into this buddy allocator's own
              // cache/tree (it was not consolidated all the way up to a
              // block big enough to hand back to the parent range).  With
              // immediate decay requested, return the physical pages to
              // the OS right away rather than letting them sit here fully
              // committed indefinitely.  The chunk remains a live,
              // addressable node in the buddy cache/tree -- its RB-tree
              // bookkeeping lives in the pagemap metadata entry, not in
              // the chunk's own memory -- so it is safe to decommit while
              // still linked in.
              //
              // Must decommit at the block's *actual landed* address and
              // size (`landed_size_bits`, via `align_down`), not the
              // original `base`/`size` that was passed in: `add_block`
              // may have consolidated `base` with one or more buddies
              // before coming to rest, in which case `base` is no longer
              // the address of the live tree/cache node at all (it could
              // now be the upper half of a bigger, differently-based
              // block) and `size` badly undercounts how much memory the
              // node actually spans.  Decommitting only the original
              // `size` bytes at `base` would physically release just a
              // sub-slice of a bigger node whose *own* tracked address is
              // elsewhere, leaving that node's flag out of sync with
              // reality.  Operating on the landed block's own address/
              // size instead keeps this consistent with the invariant
              // `on_consolidate`/`on_split` maintain (see there for the
              // crash this prevents).
              auto landed_size = bits::one_at_bit(landed_size_bits);
              auto landed_base =
                Rep::align_down(base.unsafe_uintptr(), landed_size);
              DefaultPal::notify_not_using(
                reinterpret_cast<void*>(landed_base), landed_size);

              // Mark the flag so that any future merge
              // (`Buddy::add_block`'s `on_consolidate` calls) or split
              // (`Buddy::remove_block`'s `on_split` call) knows this
              // chunk's memory is not currently committed and reconciles
              // correctly, and so `alloc_range` recommits it on reuse.
              // Without this, `is_decommitted` would always read `false`
              // for chunks decommitted via *this* branch (as opposed to
              // the windowed sweep, which does set it).
              Rep::set_decommitted(landed_base, true);
            }
            else
            {
              // Time-windowed decay policy: record that this bucket just
              // gained a fresh entry so the sweep gives it a full
              // `decay_rate_ms()` window before treating it as stale. Not
              // done in the immediate-decay branch above -- that chunk is
              // decommitted right away and `last_touched_ms` is only ever
              // consulted by the windowed sweep, which is a no-op
              // whenever `decay_rate_ms() == 0`.
              touch_bucket(landed_size_bits - MIN_CHUNK_BITS);
            }
          }
        }

        dealloc_overflow(overflow);
        maybe_sweep_decay();
      }

      /**
       * Snapshot the process-global log2-bucketed free-chunk histogram
       * for `LargeBuddyRange` instances.
       *
       * The histogram aggregates free-chunk populations across EVERY
       * live `LargeBuddyRange` Buddy in the process -- the
       * single-instance `GlobalR` plus every per-thread local cache --
       * so the snapshot does not vary across `Type` instantiations.
       * The method is provided as an instance accessor on `Type` to
       * match the rest of the range API surface and to give callers a
       * uniform call shape regardless of which range they are querying.
       *
       * `out[i]` corresponds to chunks of size
       * `1 << (MIN_CHUNK_BITS + i)` bytes for `i` in
       * `[0, NUM_BUCKETS - 1]`.  Block sizes beyond
       * `MIN_CHUNK_BITS + 15` are not tracked; the histogram is
       * deliberately sized to fit the first 16 slots of
       * `FullAllocStats.reserved[]`.
       *
       * Marked `const` -- only atomic reads happen.  Safe to call
       * from any thread at any point in the process lifetime.
       */
      void get_free_chunk_count_by_log_size(
        uint64_t (&out)[LargeBuddyFreeChunkHistogram::NUM_BUCKETS]) const
      {
        LargeBuddyFreeChunkHistogram::snapshot(out);
      }
    };
  };
} // namespace snmalloc
