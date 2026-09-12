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
  struct SampledAlloc;

  template<typename Config>
  SNMALLOC_FAST_PATH_INLINE void
  prepare_alloc(size_t requested, size_t allocated) noexcept;

#ifdef SNMALLOC_PROFILE
  template<typename Config>
  SNMALLOC_FAST_PATH_INLINE void
  on_alloc(void* p, size_t requested, size_t allocated) noexcept;

  /// In-place realloc only; see record.h.
  template<typename Config>
  SNMALLOC_FAST_PATH_INLINE void
  on_realloc(void* p, size_t requested, size_t allocated) noexcept;

  template<typename Config>
  SNMALLOC_FAST_PATH_INLINE void on_dealloc(
    void* p, const typename Config::PagemapEntry& entry) noexcept;

  template<typename Config>
  SNMALLOC_FAST_PATH_INLINE void finalize_prepared_alloc(
    void* p, size_t requested, size_t allocated, SampledAlloc* sample) noexcept;

  SNMALLOC_FAST_PATH_INLINE void
  cancel_prepared_alloc(SampledAlloc* sample) noexcept;
#else
  template<typename Config>
  SNMALLOC_FAST_PATH_INLINE void
  prepare_alloc(size_t requested, size_t allocated) noexcept
  {
    UNUSED(requested, allocated);
  }

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
  SNMALLOC_FAST_PATH_INLINE void
  on_dealloc(void* p, const typename Config::PagemapEntry& entry) noexcept
  {
    UNUSED(p);
    UNUSED(entry);
  }
#endif
} // namespace snmalloc::profile
