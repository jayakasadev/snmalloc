#pragma once

#include "../ds/ds.h"

namespace snmalloc
{
  /**
   * Default no-op histogram hook for `Buddy`.  Whenever a free block is
   * inserted into or removed from the buddy allocator's per-bucket
   * cache/tree, the buddy invokes `Histogram::on_add(size_bits)` /
   * `Histogram::on_remove(size_bits)`.  The default specialisation is
   * empty so callers (e.g. `SmallBuddyRange`) that do not want to track
   * a histogram pay zero overhead -- the inlined no-op compiles away.
   */
  struct BuddyNoHistogram
  {
    static void on_add(size_t /*size_bits*/) {}

    static void on_remove(size_t /*size_bits*/) {}
  };

  /**
   * Class representing a buddy allocator
   *
   * Underlying node `Rep` representation is passed in.
   *
   * The allocator can handle blocks between inclusive MIN_SIZE_BITS and
   * exclusive MAX_SIZE_BITS.
   *
   * `Histogram` is a free-chunk-count callback hook with two static
   * methods (`on_add(size_bits)` / `on_remove(size_bits)`) invoked
   * whenever the per-bucket cache/tree population changes by one.  The
   * default `BuddyNoHistogram` is a pair of no-ops; `LargeBuddyRange`
   * substitutes a process-global atomic histogram so callers can report
   * a log2-bucketed view of free chunks.
   */
  template<
    typename Rep,
    size_t MIN_SIZE_BITS,
    size_t MAX_SIZE_BITS,
    typename Histogram = BuddyNoHistogram>
  class Buddy
  {
    static_assert(MAX_SIZE_BITS > MIN_SIZE_BITS);

    struct Entry
    {
      typename Rep::Contents cache[3];
      RBTree<Rep> tree{};
    };

    stl::Array<Entry, MAX_SIZE_BITS - MIN_SIZE_BITS> entries{};
    // All RBtrees at or above this index should be empty.
    size_t empty_at_or_above{0};

    size_t to_index(size_t size)
    {
      SNMALLOC_ASSERT(size != 0);
      SNMALLOC_ASSERT(bits::is_pow2(size));
      auto log = snmalloc::bits::next_pow2_bits(size);
      SNMALLOC_ASSERT_MSG(
        log >= MIN_SIZE_BITS, "Size too big: {} log {}.", size, log);
      SNMALLOC_ASSERT_MSG(
        log < MAX_SIZE_BITS, "Size too small: {} log {}.", size, log);

      return log - MIN_SIZE_BITS;
    }

    void validate_block(typename Rep::Contents addr, size_t size)
    {
      SNMALLOC_ASSERT(bits::is_pow2(size));
      SNMALLOC_ASSERT(addr == Rep::align_down(addr, size));
      UNUSED(addr, size);
    }

    void invariant()
    {
#ifndef NDEBUG
      for (size_t i = empty_at_or_above; i < entries.size(); i++)
      {
        SNMALLOC_ASSERT(entries[i].tree.is_empty());
        // TODO check cache is empty
      }
#endif
    }

    bool remove_buddy(typename Rep::Contents addr, size_t size)
    {
      auto idx = to_index(size);

      // Empty at this range.
      if (idx >= empty_at_or_above)
        return false;

      auto buddy = Rep::buddy(addr, size);

      // Check local cache first
      for (auto& e : entries[idx].cache)
      {
        if (Rep::equal(buddy, e))
        {
          if (!Rep::can_consolidate(addr, size))
            return false;

          // The buddy is about to be merged with `addr` into one bigger
          // block; give the representation a chance to reconcile any
          // policy-specific per-node state before that happens.  In
          // particular, `BuddyChunkRep` uses this to recommit a
          // previously-decommitted buddy: the merged block is a single
          // node going forward and has no way to represent "half of my
          // backing memory is decommitted, half is not", so any such
          // asymmetry must be resolved (by recommitting) before the two
          // halves become inseparable.
          Rep::on_consolidate(buddy, size);

          e = entries[idx].tree.remove_min();
          // One free block leaves the system at this bucket: either the
          // matched cache slot is overwritten with the tree's minimum
          // (so the tree shrinks by one) or, if the tree was already
          // empty, `remove_min` returns `Rep::null` and the slot
          // becomes null.  Both branches net to -1 entry at `idx`.
          Histogram::on_remove(MIN_SIZE_BITS + idx);
          return true;
        }
      }

      auto path = entries[idx].tree.get_root_path();
      bool contains_buddy = entries[idx].tree.find(path, buddy);

      if (!contains_buddy)
        return false;

      // Only check if we can consolidate after we know the buddy is in
      // the buddy allocator.  This is required to prevent possible segfaults
      // from looking at the buddies meta-data, which we only know exists
      // once we have found it in the red-black tree.
      if (!Rep::can_consolidate(addr, size))
        return false;

      // See the matching comment on the cache-slot branch above.
      Rep::on_consolidate(buddy, size);

      entries[idx].tree.remove_path(path);
      Histogram::on_remove(MIN_SIZE_BITS + idx);
      return true;
    }

  public:
    constexpr Buddy() = default;

    /**
     * Add a block to the buddy allocator.
     *
     * Blocks needs to be power of two size and aligned to the same power of
     * two.
     *
     * Returns null, if the block is successfully added. Otherwise, returns the
     * consolidated block that is MAX_SIZE_BITS big, and hence too large for
     * this allocator.
     *
     * If `landed_size_bits_out` is non-null and the block is successfully
     * added (i.e. this returns `Rep::null`), `*landed_size_bits_out` is set
     * to the absolute log2 size-bucket (`MIN_SIZE_BITS + idx`) that the
     * block actually came to rest in.  Because a block may consolidate with
     * its buddy any number of times before finding an empty bucket, this
     * can differ from `next_pow2_bits(size)` -- callers that need to know
     * exactly which bucket's population changed (e.g. to update per-bucket
     * "last touched" bookkeeping outside this class) should read it back
     * through this out-parameter rather than recomputing it from `size`.
     * Left unmodified on the "too large for this allocator" path, since
     * the block leaves this `Buddy` instance entirely in that case and
     * there is no bucket here to record.  Defaults to `nullptr`, which
     * costs nothing extra: the compiler elides the unused stores.
     */
    typename Rep::Contents add_block(
      typename Rep::Contents addr,
      size_t size,
      size_t* landed_size_bits_out = nullptr)
    {
      validate_block(addr, size);

      if (remove_buddy(addr, size))
      {
        // `remove_buddy` already reconciled the *other* half's (the
        // buddy's) decommitted state via `Rep::on_consolidate` before
        // removing it.  `addr` itself -- the block passed in here -- is
        // the other half of this same merge, and needs exactly the same
        // reconciliation: it may carry its own stale `DECOMMITTED_BIT`
        // from whatever it was doing before this call, and the merged
        // node that results (`align_down(addr, size * 2)` below, which
        // is `addr`'s own value whenever `addr` happens to be the lower
        // of the two addresses) inherits whichever of the two flags
        // ends up attached to that address, unless it is normalised
        // here first.  Must run at the *original* `size` -- that is the
        // granularity `addr`'s flag (if any) was actually set at.
        Rep::on_consolidate(addr, size);

        // Add to next level cache
        size *= 2;
        addr = Rep::align_down(addr, size);
        if (size == bits::one_at_bit(MAX_SIZE_BITS))
        {
          // Invariant should be checked on all non-tail return paths.
          // Holds trivially here with current design.
          invariant();
          // Too big for this buddy allocator.
          return addr;
        }
        return add_block(addr, size, landed_size_bits_out);
      }

      auto idx = to_index(size);
      empty_at_or_above = bits::max(empty_at_or_above, idx + 1);

      for (auto& e : entries[idx].cache)
      {
        if (Rep::equal(Rep::null, e))
        {
          e = addr;
          // One new free block enters the system at this bucket via
          // the inline cache.
          Histogram::on_add(MIN_SIZE_BITS + idx);
          if (landed_size_bits_out != nullptr)
            *landed_size_bits_out = MIN_SIZE_BITS + idx;
          return Rep::null;
        }
      }

      auto path = entries[idx].tree.get_root_path();
      entries[idx].tree.find(path, addr);
      entries[idx].tree.insert_path(path, addr);
      // One new free block enters the system at this bucket via the
      // red-black tree (cache slots were all full).
      Histogram::on_add(MIN_SIZE_BITS + idx);
      if (landed_size_bits_out != nullptr)
        *landed_size_bits_out = MIN_SIZE_BITS + idx;
      invariant();
      return Rep::null;
    }

    /**
     * Removes a block of size from the buddy allocator.
     *
     * Return Rep::null if this cannot be satisfied.
     */
    typename Rep::Contents remove_block(size_t size)
    {
      invariant();
      auto idx = to_index(size);
      if (idx >= empty_at_or_above)
        return Rep::null;

      auto addr = entries[idx].tree.remove_min();
      for (auto& e : entries[idx].cache)
      {
        if (Rep::equal(Rep::null, addr) || Rep::compare(e, addr))
        {
          addr = stl::exchange(e, addr);
        }
      }

      if (addr != Rep::null)
      {
        validate_block(addr, size);
        // One free block leaves the system at this bucket -- either
        // popped directly from the tree (when `tree.remove_min` was
        // non-null) or selected from a cache slot via the swap loop
        // above.  Either way, the net population at `idx` falls by 1.
        Histogram::on_remove(MIN_SIZE_BITS + idx);
        return addr;
      }

      if (size * 2 == bits::one_at_bit(MAX_SIZE_BITS))
        // Too big for this buddy allocator
        return Rep::null;

      auto bigger = remove_block(size * 2);
      if (bigger == Rep::null)
      {
        empty_at_or_above = idx;
        invariant();
        return Rep::null;
      }

      auto second = Rep::offset(bigger, size);

      // `bigger` is about to be split into two independently-tracked
      // nodes (`bigger`, now half its former size, and `second`).  Give
      // the representation a chance to propagate any policy-specific
      // per-node state from the whole onto the new half before the split
      // takes effect.  In particular, `BuddyChunkRep` uses this to copy
      // `bigger`'s decommitted flag onto `second`: the two halves were
      // physically one committed-or-decommitted unit an instant ago, but
      // `second` is a distinct pagemap entry whose own flag bits are
      // otherwise whatever was left over from whenever that address was
      // last independently a tree/cache node -- almost certainly stale
      // and unrelated to `bigger`'s actual current state.  Without this,
      // `second` could sit in the cache reporting the wrong commit state
      // entirely (in either direction): falsely "committed" while
      // actually still decommitted memory (a later sweep would then skip
      // ever decommitting it, silently leaking RSS), or falsely
      // "decommitted" while actually fully resident (a later sweep would
      // then skip calling `notify_not_using` on already-decommitted
      // memory that was never actually released, again just skipping
      // work -- but the first, unnoticed direction is the one that
      // matters: a *second* real decommit call from a future sweep
      // landing on memory a stale flag incorrectly reported as
      // "not yet decommitted", when it had genuinely already been
      // released by an earlier pass, which is exactly the scenario that
      // produced a `SIGBUS`/`SIGSEGV` crash during development of the
      // decay sweep this hook exists for).
      Rep::on_split(bigger, second, size);

      // Split large block
      add_block(second, size);
      return bigger;
    }

    /**
     * Number of size buckets this allocator manages, i.e. the number of
     * entries in the `entries` array.  Exposed so callers that keep
     * parallel per-bucket bookkeeping (indexed the same way `to_index`
     * indexes `entries`) can size their own array to match without
     * duplicating `MAX_SIZE_BITS - MIN_SIZE_BITS` at the call site.
     */
    static constexpr size_t bucket_count()
    {
      return MAX_SIZE_BITS - MIN_SIZE_BITS;
    }

    /**
     * Visit every block currently cached at bucket `idx` (both the inline
     * `cache` slots and the red-black tree), calling `f(addr)` once per
     * block.  Read-only: does not remove, insert or otherwise alter the
     * bucket's contents or the tree's shape.  `idx` uses the same indexing
     * as `to_index` -- i.e. `idx = size_bits - MIN_SIZE_BITS`.
     *
     * Intended for callers that need to inspect (not mutate) the free set
     * at a given bucket, e.g. a time-based decommit sweep that only wants
     * to touch buckets whose bookkeeping says they have gone idle.
     */
    template<typename F>
    void for_each_in_bucket(size_t idx, F f)
    {
      SNMALLOC_ASSERT(idx < entries.size());
      for (auto& e : entries[idx].cache)
      {
        if (!Rep::equal(Rep::null, e))
          f(e);
      }
      entries[idx].tree.for_each(f);
    }
  };
} // namespace snmalloc
