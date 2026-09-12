// SPDX-License-Identifier: MIT
//
// Heap profiler -- address to alloc-site lookup.
//
// Given a heap address, return the alloc-time stack of the allocation
// containing it, provided that allocation was sampled and is still live.
// Interior addresses match: anything in [base, base + allocated_size).
//
// Each call sorts a fresh index built from one SampledList snapshot, so a
// lookup is O(N log N) in the number of live samples and a query within it
// is O(log N). Lookups are driven by hardware samples or offline
// inspection, never by the allocator, so that cost does not matter.
//
// Concurrent allocations and frees during the walk are safe; this code never
// modifies the SampledList.

#pragma once

#include "../ds_core/defines.h"
#include "sampled_alloc.h"
#include "sampler.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace snmalloc::profile
{
  /**
   * Result of `lookup_alloc_site`. Entries past `depth` are unspecified.
   */
  struct LookupFrames
  {
    /// Captured return addresses, innermost first.
    std::array<uintptr_t, MaxStackFrames> frames{};
    /// How many entries of `frames` are valid, at most `MaxStackFrames`.
    size_t depth{0};
    /// Start of the matched allocation, so a caller given an interior
    /// address can see how far into the object it pointed.
    uintptr_t base_addr{0};
    /// Sizeclass-rounded size of the match, in bytes.
    size_t allocated_size{0};
  };

  /**
   * Look up `addr` among the live samples.
   *
   * Returns the allocation's captured stack if it was sampled, is still
   * live, and `addr` falls in `[base, base + allocated_size)`. Otherwise
   * returns `std::nullopt`, which is the usual answer given that most
   * allocations are never sampled.
   *
   * A sample that fires or is freed during the call may or may not be
   * observed; either answer is correct here.
   */
  [[nodiscard]] inline std::optional<LookupFrames>
  lookup_alloc_site(uintptr_t addr) noexcept
  {
    // Holding the node pointer lets us copy the frames only for the entry
    // the search actually picks.
    struct Entry
    {
      uintptr_t base;
      size_t size;
      std::array<uintptr_t, MaxStackFrames> frames{};
      uint8_t depth{0};
    };

    std::vector<Entry> entries;

    SamplerGlobals::list().snapshot([&](SampledAlloc* node) noexcept {
      const size_t sz =
        node->allocated_size.load(std::memory_order_relaxed);
      if (sz == 0)
        return;
      Entry e{node->alloc_addr, sz, {}, 0};
      const size_t depth = node->stack_depth <= MaxStackFrames ?
        node->stack_depth :
        MaxStackFrames;
      e.depth = static_cast<uint8_t>(depth);
      for (size_t i = 0; i < depth; ++i)
        e.frames[i] = node->stack[i];
      entries.push_back(e);
    });

    if (entries.empty())
      return std::nullopt;

    // Sort by base address. Live ranges never overlap, so an address belongs
    // to at most one entry and the order among equals does not matter.
    std::sort(
      entries.begin(),
      entries.end(),
      [](const Entry& a, const Entry& b) noexcept { return a.base < b.base; });

    // `upper_bound` gives the first base above `addr`, so its predecessor is
    // the only entry whose range can contain `addr`.
    auto it = std::upper_bound(
      entries.begin(),
      entries.end(),
      addr,
      [](uintptr_t needle, const Entry& e) noexcept {
        return needle < e.base;
      });

    if (it == entries.begin())
      return std::nullopt; // Below every live sample.

    --it;
    const Entry& cand = *it;
    if (addr >= cand.base + cand.size)
      return std::nullopt; // In a gap between samples.

    // Clamp the depth so a corrupt `stack_depth` cannot read out of bounds.
    LookupFrames out;
    const size_t depth = cand.depth <= MaxStackFrames ? cand.depth :
                                                       MaxStackFrames;
    out.depth = depth;
    out.base_addr = cand.base;
    out.allocated_size = cand.size;
    for (size_t i = 0; i < depth; ++i)
      out.frames[i] = cand.frames[i];
    return out;
  }
} // namespace snmalloc::profile
