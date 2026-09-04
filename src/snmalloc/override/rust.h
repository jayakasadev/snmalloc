// SPDX-License-Identifier: MIT
//
// Declarations of the core `sn_rust_*` symbols defined in `rust.cc`.  Kept in
// a header so that `rust.cc` can include it and have the compiler check the
// definitions against it, and so Rust bindgen can generate the FFI bindings
// from C headers alone rather than parsing C++.
//
// `rust_profile.h` declares the heap-profiling half of the same ABI.

#pragma once

#include <stddef.h>

#ifndef SNMALLOC_EXPORT
#  define SNMALLOC_EXPORT
#endif

#ifdef __cplusplus
extern "C"
{
#endif

  /**
   * Allocate `size` bytes aligned to `alignment`, which must be a power of two
   * greater than zero.  Returns NULL on out-of-memory.
   */
  SNMALLOC_EXPORT void* sn_rust_alloc(size_t alignment, size_t size);

  /**
   * Like `sn_rust_alloc` but zero-initialises the returned region.
   */
  SNMALLOC_EXPORT void* sn_rust_alloc_zeroed(size_t alignment, size_t size);

  /**
   * Deallocate the region previously returned by `sn_rust_alloc` /
   * `sn_rust_alloc_zeroed` / `sn_rust_realloc`.  `alignment` and `size`
   * must match the values used at allocation time.
   */
  SNMALLOC_EXPORT void
  sn_rust_dealloc(void* ptr, size_t alignment, size_t size);

  /**
   * Resize the allocation at `ptr` from `old_size` to `new_size` bytes
   * (both with the same `alignment`).  Returns NULL on failure, in which
   * case the original allocation is left intact.
   */
  SNMALLOC_EXPORT void* sn_rust_realloc(
    void* ptr, size_t alignment, size_t old_size, size_t new_size);

  /**
   * Write the current and peak bytes reserved from the OS into the two
   * outputs.  Both must be non-NULL.
   */
  SNMALLOC_EXPORT void
  sn_rust_statistics(size_t* current_memory_usage, size_t* peak_memory_usage);

  /**
   * Return the usable size of the allocation at `ptr` in bytes, i.e. the size
   * of the sizeclass it was rounded up to.  Returns 0 for NULL.
   */
  SNMALLOC_EXPORT size_t sn_rust_usable_size(const void* ptr);

#ifdef __cplusplus
}
#endif
