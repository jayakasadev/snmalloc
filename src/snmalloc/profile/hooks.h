#pragma once

// Heap-profiler alloc/dealloc hook entry points, declared unconditionally so
// the allocator can call them by name without an `#ifdef` at the call site.
//
// These are thin wrappers over the `profile::record_*` functions (defined in
// profile/record.h).  When SNMALLOC_PROFILE is on they are declared here and
// defined in record.h (the point of template instantiation); when off they are
// empty inline no-ops, so the allocator's unconditional calls compile to
// nothing and the profile subsystem headers are not pulled in.
//
// A distinct name from `record_*` is deliberate: the `record_*` functions are
// always defined in record.h (self-disabling per config so the profiler tests
// can call them directly in any build), whereas the `on_*` entry points here
// are the build-gated ones the allocator uses.

#include "../ds_core/defines.h"

namespace snmalloc::profile
{
#ifdef SNMALLOC_PROFILE
  /// Alloc chokepoint hook; forwards to `record_alloc`.  Defined in record.h.
  template<typename Config>
  SNMALLOC_FAST_PATH_INLINE void
  on_alloc(void* p, size_t requested, size_t allocated) noexcept;

  /// In-place realloc hook; forwards to `record_realloc`.  Defined in record.h.
  template<typename Config>
  SNMALLOC_FAST_PATH_INLINE void
  on_realloc(void* p, size_t requested, size_t allocated) noexcept;

  /// Dealloc waist hook; forwards to `record_dealloc`.  Defined in record.h.
  template<typename Config>
  SNMALLOC_FAST_PATH_INLINE void on_dealloc(void* p) noexcept;

  /// Force-inlined peek for the dealloc fast path; forwards to
  /// `record_dealloc_peek`.  Defined in record.h.
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
