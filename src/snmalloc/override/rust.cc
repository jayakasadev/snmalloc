#define SNMALLOC_NAME_MANGLE(a) sn_##a

#include "rust_config.h"

#ifdef SNMALLOC_PROFILE
#  include <snmalloc/profile/addr_lookup.h>
#  include <snmalloc/profile/profile.h>
#  include <snmalloc/profile/record.h>
#endif

// The libc API provided by malloc.cc will always be mangled per above.
#ifdef SNMALLOC_RUST_LIBC_API
#  include "malloc.cc"
#else
#  include "snmalloc/snmalloc.h"
#endif

#include "rust.h"
#include "rust_profile.h"

#include <stdlib.h>
#include <string.h>

#ifndef SNMALLOC_EXPORT
#  define SNMALLOC_EXPORT
#endif

using namespace snmalloc;

extern "C" SNMALLOC_EXPORT void*
SNMALLOC_NAME_MANGLE(rust_alloc)(size_t alignment, size_t size)
{
  return alloc(aligned_size(alignment, size));
}

extern "C" SNMALLOC_EXPORT void*
SNMALLOC_NAME_MANGLE(rust_alloc_zeroed)(size_t alignment, size_t size)
{
  return alloc<Zero>(aligned_size(alignment, size));
}

extern "C" SNMALLOC_EXPORT void
SNMALLOC_NAME_MANGLE(rust_dealloc)(void* ptr, size_t alignment, size_t size)
{
  dealloc(ptr, aligned_size(alignment, size));
}

extern "C" SNMALLOC_EXPORT void* SNMALLOC_NAME_MANGLE(rust_realloc)(
  void* ptr, size_t alignment, size_t old_size, size_t new_size)
{
  size_t aligned_old_size = aligned_size(alignment, old_size),
         aligned_new_size = aligned_size(alignment, new_size);
  if (
    size_to_sizeclass_full(aligned_old_size).raw() ==
    size_to_sizeclass_full(aligned_new_size).raw())
  {
#ifdef SNMALLOC_PROFILE
    // The allocation is staying put, so if it was sampled its recorded sizes
    // need updating and a Resize event broadcasting.  The out-of-place path
    // below needs no hook: its `alloc` and `dealloc` calls already report the
    // new and old pointers.
    snmalloc::profile::record_realloc<snmalloc::Config>(
      ptr, new_size, aligned_new_size);
#endif
    return ptr;
  }
  void* p = alloc(aligned_new_size);
  if (p)
  {
    memcpy(p, ptr, old_size < new_size ? old_size : new_size);
    dealloc(ptr, aligned_old_size);
  }
  return p;
}

extern "C" SNMALLOC_EXPORT void SNMALLOC_NAME_MANGLE(rust_statistics)(
  size_t* current_memory_usage, size_t* peak_memory_usage)
{
  *current_memory_usage = Alloc::Config::Backend::get_current_usage();
  *peak_memory_usage = Alloc::Config::Backend::get_peak_usage();
}

extern "C" SNMALLOC_EXPORT size_t
SNMALLOC_NAME_MANGLE(rust_usable_size)(const void* ptr)
{
  return alloc_size(ptr);
}

// ---------------------------------------------------------------------------
// Heap profiling C ABI.
//
// Every symbol here exists in every build, so the Rust FFI links either way.
// Without SNMALLOC_PROFILE they are all stubs returning zero, false or null,
// except `sn_rust_profile_supported`, which returns false so callers can
// detect the situation.  The Rust crate's own `profiling` feature is
// independent of the C++ flag.
// ---------------------------------------------------------------------------

#ifdef SNMALLOC_PROFILE

namespace
{
  /**
   * A snapshot, handed to callers as an opaque handle and released by
   * `sn_rust_profile_snapshot_end`.
   *
   * Samples are copied into a flat array of PODs so the caller can iterate
   * without holding any reference into live profile state.
   *
   * Backing storage comes from `malloc` / `free`.  That is safe here because
   * snapshots are taken off the allocation path, with no sampler guard held.
   */
  struct RustProfileSnapshot
  {
    SnRustProfileRawSample* samples;
    size_t count;
  };
} // namespace

extern "C" SNMALLOC_EXPORT bool sn_rust_profile_supported(void)
{
  return true;
}

extern "C" SNMALLOC_EXPORT void sn_rust_profile_set_sampling_rate(size_t bytes)
{
  snmalloc::profile::Sampler::set_sampling_rate(bytes);
}

extern "C" SNMALLOC_EXPORT size_t sn_rust_profile_get_sampling_rate(void)
{
  return snmalloc::profile::Sampler::get_sampling_rate();
}

extern "C" SNMALLOC_EXPORT void* sn_rust_profile_snapshot_begin(void)
{
  // Count first so we know how much to allocate.
  size_t live = snmalloc::profile::SamplerGlobals::list().debug_count();

  auto* snap =
    static_cast<RustProfileSnapshot*>(::malloc(sizeof(RustProfileSnapshot)));
  if (snap == nullptr)
    return nullptr;

  snap->samples = nullptr;
  snap->count = 0;

  if (live == 0)
    return snap;

  // Other threads may add samples between the count above and the copy below,
  // so over-allocate a little and bound the copy by the capacity.  Samples
  // arriving after the snapshot starts may or may not be included, which is
  // the usual contract for a heap profiler.
  const size_t cap = live + 16;
  snap->samples = static_cast<SnRustProfileRawSample*>(
    ::malloc(cap * sizeof(SnRustProfileRawSample)));
  if (snap->samples == nullptr)
  {
    ::free(snap);
    return nullptr;
  }

  size_t idx = 0;
  snmalloc::profile::SamplerGlobals::list().snapshot(
    [&](snmalloc::profile::SampledAlloc* node) noexcept {
      if (idx >= cap)
        return;
      SnRustProfileRawSample& out = snap->samples[idx];
      out.alloc_ptr = reinterpret_cast<void*>(node->alloc_addr);
      out.requested_size =
        node->requested_size.load(std::memory_order_relaxed);
      out.allocated_size =
        node->allocated_size.load(std::memory_order_relaxed);
      out.weight = static_cast<size_t>(node->weight);
      const size_t depth = node->stack_depth <= SNMALLOC_PROFILE_STACK_FRAMES ?
        node->stack_depth :
        SNMALLOC_PROFILE_STACK_FRAMES;
      out.stack_depth = static_cast<uint32_t>(depth);
      for (size_t i = 0; i < depth; ++i)
        out.stack[i] = reinterpret_cast<void*>(node->stack[i]);
      for (size_t i = depth; i < SNMALLOC_PROFILE_STACK_FRAMES; ++i)
        out.stack[i] = nullptr;
      // Always `Alloc` here: the stored record is never tagged `Resize`, only
      // the copy the streaming path broadcasts.
      out.kind = node->kind;
      ++idx;
    });

  snap->count = idx;
  return snap;
}

extern "C" SNMALLOC_EXPORT size_t sn_rust_profile_snapshot_count(void* handle)
{
  if (handle == nullptr)
    return 0;
  return static_cast<RustProfileSnapshot*>(handle)->count;
}

extern "C" SNMALLOC_EXPORT bool sn_rust_profile_snapshot_get(
  void* handle, size_t idx, SnRustProfileRawSample* out)
{
  if (handle == nullptr || out == nullptr)
    return false;
  auto* snap = static_cast<RustProfileSnapshot*>(handle);
  if (idx >= snap->count)
    return false;
  *out = snap->samples[idx];
  return true;
}

extern "C" SNMALLOC_EXPORT void sn_rust_profile_snapshot_end(void* handle)
{
  if (handle == nullptr)
    return;
  auto* snap = static_cast<RustProfileSnapshot*>(handle);
  ::free(snap->samples);
  ::free(snap);
}

// ---------------------------------------------------------------------------
// Streaming-mode FFI.
//
// One registered C callback receives an event per sampled allocation.  The
// underlying `AllocationSampleList` supports several subscribers, but this FFI
// allows only one at a time so callers have no slot index to track; a second
// registration returns -1.  Register directly in C++ if you need more.
//
// The shim below converts each `SampledAlloc` into the FFI-stable
// `SnRustProfileRawSample`, so the C++ type never reaches the caller.  It must
// stay `noexcept` and allocation-free, per the handler contract.
// ---------------------------------------------------------------------------

namespace
{
  /// The registered user callback.  Atomic because allocating threads read it
  /// while another thread may be registering or unregistering.
  std::atomic<void (*)(const SnRustProfileRawSample*)> g_streaming_user_cb{
    nullptr};

  /**
   * Registered with `AllocationSampleList::global()`; converts the sample and
   * calls the user callback.  Runs on the allocating thread.
   */
  void
  streaming_broadcast_shim(const snmalloc::profile::SampledAlloc& node) noexcept
  {
    auto user_cb = g_streaming_user_cb.load(std::memory_order_acquire);
    if (user_cb == nullptr)
      return;

    // Kept on the stack: this must not allocate, or it would re-enter the
    // allocator it is reporting on.
    SnRustProfileRawSample out{};
    out.alloc_ptr = reinterpret_cast<void*>(node.alloc_addr);
    out.requested_size =
      node.requested_size.load(std::memory_order_relaxed);
    out.allocated_size =
      node.allocated_size.load(std::memory_order_relaxed);
    out.weight = static_cast<size_t>(node.weight);
    const size_t depth = node.stack_depth <= SNMALLOC_PROFILE_STACK_FRAMES ?
      node.stack_depth :
      SNMALLOC_PROFILE_STACK_FRAMES;
    out.stack_depth = static_cast<uint32_t>(depth);
    for (size_t i = 0; i < depth; ++i)
      out.stack[i] = reinterpret_cast<void*>(node.stack[i]);
    for (size_t i = depth; i < SNMALLOC_PROFILE_STACK_FRAMES; ++i)
      out.stack[i] = nullptr;
    // `Alloc` from `record_alloc`, `Resize` from `record_realloc`.
    out.kind = node.kind;

    user_cb(&out);
  }
} // namespace

extern "C" SNMALLOC_EXPORT int
sn_rust_profile_streaming_start(void (*cb)(const SnRustProfileRawSample*))
{
  if (cb == nullptr)
    return -1;

  // Claim the single callback slot.  Failure means one is already registered.
  void (*expected)(const SnRustProfileRawSample*) = nullptr;
  if (!g_streaming_user_cb.compare_exchange_strong(
        expected, cb, std::memory_order_acq_rel, std::memory_order_relaxed))
  {
    return -1;
  }

  const int rc =
    snmalloc::profile::AllocationSampleList::global().register_handler(
      streaming_broadcast_shim);
  if (rc != snmalloc::profile::AllocationSampleList::kOk)
  {
    // The broadcast list had no free slot.  Release the callback slot again so
    // a later start() can retry.
    g_streaming_user_cb.store(nullptr, std::memory_order_release);
    return -1;
  }
  return 0;
}

extern "C" SNMALLOC_EXPORT int sn_rust_profile_streaming_stop(void)
{
  // Unregister the shim before clearing the callback pointer.  Broadcasts run
  // without a lock, so a broadcast already in flight must still find a valid
  // callback to call.
  const int rc =
    snmalloc::profile::AllocationSampleList::global().unregister_handler(
      streaming_broadcast_shim);

  auto prev = g_streaming_user_cb.exchange(nullptr, std::memory_order_acq_rel);

  if (rc != snmalloc::profile::AllocationSampleList::kOk || prev == nullptr)
    return -1;
  return 0;
}

// ---------------------------------------------------------------------------
// Address -> alloc-site reverse lookup.  See `rust_profile.h` for the full
// contract.
// ---------------------------------------------------------------------------

extern "C" SNMALLOC_EXPORT intptr_t sn_rust_profile_lookup_alloc_site(
  uintptr_t addr,
  uintptr_t* out_frames,
  size_t max_frames,
  uintptr_t* out_base_addr,
  size_t* out_allocated_size)
{
  if (out_frames == nullptr && max_frames > 0)
    return -1;

  auto result = snmalloc::profile::lookup_alloc_site(addr);
  if (!result.has_value())
    return -1;

  const auto& f = *result;
  if (out_base_addr != nullptr)
    *out_base_addr = f.base_addr;
  if (out_allocated_size != nullptr)
    *out_allocated_size = f.allocated_size;

  // Truncate to the caller's buffer rather than overflowing it; the caller
  // detects truncation by seeing `max_frames` returned.
  const size_t to_copy = f.depth < max_frames ? f.depth : max_frames;
  for (size_t i = 0; i < to_copy; ++i)
    out_frames[i] = f.frames[i];
  return static_cast<intptr_t>(to_copy);
}

// ---------------------------------------------------------------------------
// Allocation-lifetime histogram.  See `rust_profile.h` for the full contract.
// ---------------------------------------------------------------------------
extern "C" SNMALLOC_EXPORT size_t
sn_rust_profile_lifetime_histogram(uint64_t* out_buckets, size_t len)
{
  if (out_buckets == nullptr || len == 0)
    return 0;
  const size_t to_copy = len < snmalloc::profile::kLifetimeBuckets ?
    len :
    snmalloc::profile::kLifetimeBuckets;
  auto& hist = snmalloc::profile::LifetimeHistogram::get();
  for (size_t i = 0; i < to_copy; ++i)
    out_buckets[i] = hist.bucket(i);
  return to_copy;
}

#else // !SNMALLOC_PROFILE

// Stubs, so the FFI stays linkable when profiling is compiled out.

extern "C" SNMALLOC_EXPORT bool sn_rust_profile_supported(void)
{
  return false;
}

extern "C" SNMALLOC_EXPORT void
sn_rust_profile_set_sampling_rate(size_t /*bytes*/)
{}

extern "C" SNMALLOC_EXPORT size_t sn_rust_profile_get_sampling_rate(void)
{
  return 0;
}

extern "C" SNMALLOC_EXPORT void* sn_rust_profile_snapshot_begin(void)
{
  return nullptr;
}

extern "C" SNMALLOC_EXPORT size_t sn_rust_profile_snapshot_count(void* /*h*/)
{
  return 0;
}

extern "C" SNMALLOC_EXPORT bool sn_rust_profile_snapshot_get(
  void* /*handle*/, size_t /*idx*/, SnRustProfileRawSample* /*out*/)
{
  return false;
}

extern "C" SNMALLOC_EXPORT void sn_rust_profile_snapshot_end(void* /*h*/) {}

extern "C" SNMALLOC_EXPORT int
sn_rust_profile_streaming_start(void (*)(const SnRustProfileRawSample*))
{
  return -1;
}

extern "C" SNMALLOC_EXPORT int sn_rust_profile_streaming_stop(void)
{
  return -1;
}

extern "C" SNMALLOC_EXPORT intptr_t sn_rust_profile_lookup_alloc_site(
  uintptr_t /*addr*/,
  uintptr_t* /*out_frames*/,
  size_t /*max_frames*/,
  uintptr_t* /*out_base_addr*/,
  size_t* /*out_allocated_size*/)
{
  return -1;
}

extern "C" SNMALLOC_EXPORT size_t
sn_rust_profile_lifetime_histogram(uint64_t* /*out_buckets*/, size_t /*len*/)
{
  return 0;
}

#endif // SNMALLOC_PROFILE
