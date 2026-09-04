#pragma once

// Heap-profiler hook entry points, declared unconditionally so the allocator
// can call them without an `#ifdef` at each call site.
//
// Under SNMALLOC_PROFILE these forward to the `record_*` functions defined in
// profile/record.h; otherwise they are empty inline no-ops, so a non-profile
// build compiles them away and never pulls in the profiler headers.

#include "../ds_core/defines.h"

namespace snmalloc::profile
{
#ifdef SNMALLOC_PROFILE
  template<typename Config>
  SNMALLOC_FAST_PATH_INLINE void
  on_alloc(void* p, size_t requested, size_t allocated) noexcept;

  /// In-place realloc only; see record.h.
  template<typename Config>
  SNMALLOC_FAST_PATH_INLINE void
  on_realloc(void* p, size_t requested, size_t allocated) noexcept;

  template<typename Config>
  SNMALLOC_FAST_PATH_INLINE void on_dealloc(void* p) noexcept;

  /// True when `p` was never sampled and `on_dealloc` can be skipped.
  template<typename Config>
  SNMALLOC_FAST_PATH_INLINE bool on_dealloc_peek(void* p) noexcept;
#else
  template<typename Config>
  SNMALLOC_FAST_PATH_INLINE void
  on_alloc(void* p, size_t requested, size_t allocated) noexcept
  {
    UNUSED(p, requested, allocated);
  }

  template<typename Config>
  SNMALLOC_FAST_PATH_INLINE void
  on_realloc(void* p, size_t requested, size_t allocated) noexcept
  {
    UNUSED(p, requested, allocated);
  }

  template<typename Config>
  SNMALLOC_FAST_PATH_INLINE void on_dealloc(void* p) noexcept
  {
    UNUSED(p);
  }

  template<typename Config>
  SNMALLOC_FAST_PATH_INLINE bool on_dealloc_peek(void* p) noexcept
  {
    UNUSED(p);
    return false;
  }
#endif
} // namespace snmalloc::profile
