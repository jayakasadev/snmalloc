#pragma once

// Heap-profiler dealloc hook entry points, declared unconditionally so the
// allocator can call them by name without an `#ifdef` at the call site.
//
// When SNMALLOC_PROFILE is on, these are forward declarations; the real,
// re-entrancy-guarded definitions live in profile/record.h (pulled in by
// backend_helpers.h, which is the point of template instantiation).  When
// SNMALLOC_PROFILE is off, they are empty inline no-ops, so the allocator's
// unconditional calls compile to nothing.

#include "../ds_core/defines.h"

namespace snmalloc::profile
{
#ifdef SNMALLOC_PROFILE
  /// H1/H2/H3/H4 hook: clear any per-object profile slot for `p` and detach it
  /// from the live-sample list.  Defined in profile/record.h.
  template<typename Config>
  SNMALLOC_FAST_PATH_INLINE void record_dealloc(void* p) noexcept;

  /// Peek-only fast path for the H1 site: probe the slab-metadata profile slot
  /// inline and report whether the dealloc is done (no sample to clear).
  /// Returns false to fall through to the full `record_dealloc`.  Defined in
  /// profile/record.h.
  template<typename Config>
  SNMALLOC_FAST_PATH_INLINE bool record_dealloc_peek(void* p) noexcept;
#else
  template<typename Config>
  SNMALLOC_FAST_PATH_INLINE void record_dealloc(void* p) noexcept
  {
    UNUSED(p);
  }

  template<typename Config>
  SNMALLOC_FAST_PATH_INLINE bool record_dealloc_peek(void* p) noexcept
  {
    UNUSED(p);
    return false;
  }
#endif
} // namespace snmalloc::profile
