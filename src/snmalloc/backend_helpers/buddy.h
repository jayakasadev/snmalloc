#pragma once

#include "../ds/ds.h"

namespace snmalloc
{
  /**
   * Default `Histogram` hook for `Buddy`: does nothing, so callers that do not
   * want free-chunk counts (e.g. `SmallBuddyRange`) pay nothing.
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
   * `Histogram` is notified whenever the number of free blocks in a bucket
   * changes by one, via `on_add(size_bits)` / `on_remove(size_bits)`.
   * `size_bits` is an absolute log2 size, not a bucket index.
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

          // `buddy` is about to disappear into a merged block, so let the
          // representation reconcile its per-node state first.  Must happen at
          // the pre-merge `size`.
          Rep::on_consolidate(buddy, size);

          e = entries[idx].tree.remove_min();
          // Net effect on this bucket is -1 block either way: the cache slot
          // is refilled from the tree, or the tree was empty and the slot
          // becomes null.
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

      // As on the cache-slot branch above.
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
     * If `landed_size_bits_out` is non-null and the block was added (i.e. this
     * returned `Rep::null`), it receives the absolute log2 size the block came
     * to rest at.  That can be larger than `next_pow2_bits(size)`, because the
     * block may consolidate with its buddy several times on the way in.  It is
     * left untouched on the "too large" path.
     */
    typename Rep::Contents add_block(
      typename Rep::Contents addr,
      size_t size,
      size_t* landed_size_bits_out = nullptr)
    {
      validate_block(addr, size);

      if (remove_buddy(addr, size))
      {
        // `remove_buddy` reconciled the other half of this merge; `addr` is
        // this half and needs the same treatment, because the merged node
        // takes its per-node state from whichever address it ends up at.  Must
        // run at the original `size`, which is the granularity `addr`'s own
        // state was recorded at.
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
          Histogram::on_add(MIN_SIZE_BITS + idx);
          if (landed_size_bits_out != nullptr)
            *landed_size_bits_out = MIN_SIZE_BITS + idx;
          return Rep::null;
        }
      }

      auto path = entries[idx].tree.get_root_path();
      entries[idx].tree.find(path, addr);
      entries[idx].tree.insert_path(path, addr);
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

      // `bigger` is about to become two separately-tracked nodes: `bigger` at
      // half its former size, and `second`.  Let the representation copy any
      // per-node state onto `second`, whose own state is otherwise left over
      // from the last time that address was a node and so is stale.
      Rep::on_split(bigger, second, size);

      // Split large block
      add_block(second, size);
      return bigger;
    }

    /**
     * Number of size buckets.  Callers keeping their own per-bucket array can
     * use this to size it, and index it the same way `to_index` does.
     */
    static constexpr size_t bucket_count()
    {
      return MAX_SIZE_BITS - MIN_SIZE_BITS;
    }

    /**
     * Call `f(addr)` once for every free block in bucket `idx`, covering both
     * the cache slots and the tree.  `idx` is `size_bits - MIN_SIZE_BITS`.
     *
     * `f` must not add or remove blocks; this walk assumes the bucket is not
     * modified while it runs.
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
