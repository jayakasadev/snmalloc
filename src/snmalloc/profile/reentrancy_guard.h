// SPDX-License-Identifier: MIT
//
// Heap profiler -- per-thread re-entrancy guard for the sampler slow path.
//
// Firing a sample walks the stack, claims a node and publishes it, and those
// steps can allocate (glibc's backtrace() mallocs a buffer on first use).
// Without this flag the sampler would recurse into itself or corrupt its own
// per-thread state.
//
// The flag is a plain thread_local byte, so first access runs no constructor
// and cannot malloc.

#pragma once

#include "../ds_core/defines.h"

#include <cstdint>

namespace snmalloc::profile
{
  /// Set while this thread is inside the sampler slow path.
  inline thread_local uint8_t profile_in_progress = 0;

  /// True if this thread is already inside the sampler.
  SNMALLOC_FAST_PATH_INLINE bool sampler_reentered() noexcept
  {
    return profile_in_progress != 0;
  }

  /**
   * Sets the flag for its scope.
   *
   * Callers must check `sampler_reentered()` first: the guard does not save
   * and restore the previous value.
   */
  class ReentrancyGuard
  {
  public:
    SNMALLOC_FAST_PATH_INLINE ReentrancyGuard() noexcept
    {
      SNMALLOC_ASSERT(profile_in_progress == 0);
      profile_in_progress = 1;
    }

    SNMALLOC_FAST_PATH_INLINE ~ReentrancyGuard() noexcept
    {
      profile_in_progress = 0;
    }

    ReentrancyGuard(const ReentrancyGuard&) = delete;
    ReentrancyGuard& operator=(const ReentrancyGuard&) = delete;
    ReentrancyGuard(ReentrancyGuard&&) = delete;
    ReentrancyGuard& operator=(ReentrancyGuard&&) = delete;
  };
} // namespace snmalloc::profile
