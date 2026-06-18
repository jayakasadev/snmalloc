#pragma once

// Heap-profiler dealloc hook entry points, declared unconditionally so the
// allocator can call them by name without an `#ifdef` at the call site.
//
// These are thin wrappers over `profile::record_dealloc` /
// `record_dealloc_peek` (defined in profile/record.h).  When SNMALLOC_PROFILE
// is on they are declared here and defined in record.h (the point of template
// instantiation); when off they are empty inline no-ops, so the allocator's
// unconditional calls compile to nothing and the profile subsystem headers are
// not pulled in.
//
// A distinct name from `record_dealloc` is deliberate: `record_dealloc` is
// always defined in record.h (self-disabling per config so the profiler tests
// can call it directly in any build), whereas `on_dealloc` is the build-gated
// entry the allocator uses.

#include "../ds_core/defines.h"

namespace snmalloc::profile
{
#ifdef SNMALLOC_PROFILE
  /// Defined in profile/record.h; forwards to `record_dealloc`.
  template<typename Config>
  SNMALLOC_FAST_PATH_INLINE void on_dealloc(void* p) noexcept;

  /// Defined in profile/record.h; forwards to `record_dealloc_peek`.
  template<typename Config>
  SNMALLOC_FAST_PATH_INLINE bool on_dealloc_peek(void* p) noexcept;
#else
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
