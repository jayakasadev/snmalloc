// SPDX-License-Identifier: MIT
//
// Heap profiler C ABI, for Rust and any other FFI caller.  Declarations only.
//
// Every symbol here is exported whether or not the C++ build set
// SNMALLOC_PROFILE.  Without it they are all no-ops returning 0, false or
// null, except `sn_rust_profile_supported`, which returns false.  That keeps
// the ABI stable enough for one snmalloc-sys crate to build against either
// flavour without `#[cfg]` on its extern blocks.
//
// SNMALLOC_PROFILE_STACK_FRAMES must equal the C++ side's value in
// src/snmalloc/profile/sampled_alloc.h, because it sets the size of the
// `stack` array below.  Changing it there means rebuilding snmalloc-sys.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef SNMALLOC_PROFILE_STACK_FRAMES
#  define SNMALLOC_PROFILE_STACK_FRAMES 32
#endif

#ifndef SNMALLOC_EXPORT
#  define SNMALLOC_EXPORT
#endif

#ifdef __cplusplus
extern "C"
{
#endif

/**
 * Event kind for a sampled allocation, matching
 * `snmalloc::profile::SampledAllocKind`:
 *   0 = Alloc   a newly sampled allocation.
 *   1 = Resize  an in-place realloc changed a sample's size.  Only streaming
 *               consumers see this; snapshots always report Alloc.
 */
#define SN_RUST_PROFILE_KIND_ALLOC ((uint8_t)0)
#define SN_RUST_PROFILE_KIND_RESIZE ((uint8_t)1)

  /**
   * One sampled allocation.  A plain C struct, so Rust can mirror it with
   * `#[repr(C)]`.
   *
   * There is no runtime versioning: a consumer must be compiled against the
   * same version of this header as the shim it runs against.
   *
   * Fields:
   *   alloc_ptr        Pointer the original alloc returned.  May be null if
   *                    the alloc-side hook could not record one.
   *   requested_size   Bytes the caller asked for.  Post-resize value on a
   *                    Resize event.
   *   allocated_size   Bytes snmalloc actually provided, rounded up to a
   *                    sizeclass.  Post-resize value on a Resize event.
   *   weight           Bytes this sample stands for, as the Poisson sampler's
   *                    unbiased estimate.  A Resize does not change it: the
   *                    sampler is not re-rolled.
   *   stack_depth      Valid entries in `stack`, at most
   *                    SNMALLOC_PROFILE_STACK_FRAMES.
   *   stack            Return addresses, innermost first; entries past
   *                    `stack_depth` are unspecified.  A Resize does not
   *                    change it, so this stays the original alloc site.
   *   kind             SN_RUST_PROFILE_KIND_ALLOC or
   *                    SN_RUST_PROFILE_KIND_RESIZE.
   */
  struct SnRustProfileRawSample
  {
    void* alloc_ptr;
    size_t requested_size;
    size_t allocated_size;
    size_t weight;
    uint32_t stack_depth;
    void* stack[SNMALLOC_PROFILE_STACK_FRAMES];
    uint8_t kind;
  };

  /**
   * Was this snmalloc built with SNMALLOC_PROFILE=ON?  When false, every other
   * sn_rust_profile_* call does nothing.
   */
  SNMALLOC_EXPORT bool sn_rust_profile_supported(void);

  /**
   * Set the mean sampling interval, in bytes.  0 disables sampling.
   *
   * When SNMALLOC_PROFILE=OFF this is a no-op.
   */
  SNMALLOC_EXPORT void sn_rust_profile_set_sampling_rate(size_t bytes);

  /**
   * Get the current mean sampling interval, in bytes.
   *
   * When SNMALLOC_PROFILE=OFF returns 0.
   */
  SNMALLOC_EXPORT size_t sn_rust_profile_get_sampling_rate(void);

  /**
   * Take a snapshot of the currently-live sampled allocations, returning an
   * opaque handle for sn_rust_profile_snapshot_count and
   * sn_rust_profile_snapshot_get.
   *
   * The caller MUST pass the handle to sn_rust_profile_snapshot_end to free
   * it.  A null return means profiling is off or the snapshot could not be
   * allocated; treat both as "no samples".
   *
   * Other threads may allocate and free while the snapshot is taken.  A sample
   * that starts or ends during the call may or may not be included.
   */
  SNMALLOC_EXPORT void* sn_rust_profile_snapshot_begin(void);

  /**
   * Number of samples in the snapshot identified by `handle`.  Returns 0
   * for a null handle or when SNMALLOC_PROFILE=OFF.
   */
  SNMALLOC_EXPORT size_t sn_rust_profile_snapshot_count(void* handle);

  /**
   * Copy sample at index `idx` into `*out`.  Returns true on success,
   * false when:
   *   - SNMALLOC_PROFILE=OFF (no samples to copy)
   *   - handle is null
   *   - out is null
   *   - idx is out of range
   */
  SNMALLOC_EXPORT bool sn_rust_profile_snapshot_get(
    void* handle, size_t idx, struct SnRustProfileRawSample* out);

  /**
   * Release the snapshot allocated by sn_rust_profile_snapshot_begin.
   * Safe to call with a null handle (no-op).
   */
  SNMALLOC_EXPORT void sn_rust_profile_snapshot_end(void* handle);

  // ---------------------------------------------------------------------------
  // Streaming mode.
  //
  // Instead of polling for live samples, register a callback that is invoked
  // once per sampled allocation, as it happens, on the allocating thread.
  // Only one callback may be registered at a time.  Frees are not reported.
  //
  // Streaming and snapshots can be used together.
  //
  // The callback must:
  //   - be `noexcept`; letting an exception escape is undefined behaviour.
  //   - not allocate from the snmalloc heap, which would re-enter the
  //     allocator that is calling it.
  //   - return promptly.  It runs inline with the allocation, so treat it like
  //     a signal handler.
  //   - copy out anything it needs: the `SnRustProfileRawSample` pointer is
  //     only valid for the duration of the call.
  // ---------------------------------------------------------------------------

  /**
   * Register the streaming callback.  Returns 0 on success, or -1 if one is
   * already registered, `cb` is null, or profiling is off.
   */
  SNMALLOC_EXPORT int sn_rust_profile_streaming_start(
    void (*cb)(const struct SnRustProfileRawSample*));

  /**
   * Unregister the streaming callback.  Returns 0 on success, or -1 if none is
   * registered or profiling is off.
   */
  SNMALLOC_EXPORT int sn_rust_profile_streaming_stop(void);

  // ---------------------------------------------------------------------------
  // Address -> alloc-site reverse lookup.
  //
  // Find the still-live sampled allocation containing `addr` -- typically an
  // address harvested from a PMU sample -- and copy its alloc-time call stack
  // into `out_frames`.  `addr` may point anywhere inside the allocation, not
  // just at its start.
  //
  // Parameters:
  //   addr               Address to look up.
  //   out_frames         Caller-owned buffer receiving return addresses,
  //                      innermost first.  May be null only when `max_frames`
  //                      is zero, i.e. when only the outputs below are wanted.
  //   max_frames         Capacity of `out_frames`.  A deeper stack is
  //                      truncated to fit, which the caller sees as a return
  //                      value equal to `max_frames`.  A buffer of
  //                      SNMALLOC_PROFILE_STACK_FRAMES can never truncate.
  //   out_base_addr      Optional; receives the allocation's base address.
  //   out_allocated_size Optional; receives its size in bytes, rounded up to a
  //                      sizeclass.
  //
  // Returns the number of frames written, or -1 if no live sampled allocation
  // contains `addr`, if `out_frames` is null with `max_frames > 0`, or if
  // profiling is off.  An address in a non-sampled or already-freed allocation
  // is a normal miss.
  //
  // Read-only, and safe to call while other threads allocate and free.
  // ---------------------------------------------------------------------------
  SNMALLOC_EXPORT intptr_t sn_rust_profile_lookup_alloc_site(
    uintptr_t addr,
    uintptr_t* out_frames,
    size_t max_frames,
    uintptr_t* out_base_addr,
    size_t* out_allocated_size);

// ---------------------------------------------------------------------------
// Allocation-lifetime histogram.
//
// Lifetimes of sampled allocations in nanoseconds, bucketed by
// `floor(log2(lifetime_ns))`.  The last bucket absorbs everything above its
// range.  Counts are process-wide and are not reset by taking a snapshot.
// ---------------------------------------------------------------------------

/// Number of lifetime histogram buckets.  Must match
/// `SNMALLOC_FULL_STATS_LIFETIME_BUCKETS` and
/// `snmalloc::profile::kLifetimeBuckets`.
#define SN_RUST_PROFILE_LIFETIME_BUCKETS ((size_t)32)

  /**
   * Copy up to `len` buckets into `out_buckets`, in index order, and return
   * how many were written.  Returns 0 and writes nothing if `out_buckets` is
   * NULL, `len` is zero, or profiling is off.
   *
   * Read-only, and safe to call while lifetimes are being recorded.
   */
  SNMALLOC_EXPORT size_t
  sn_rust_profile_lifetime_histogram(uint64_t* out_buckets, size_t len);

#ifdef __cplusplus
}
#endif
