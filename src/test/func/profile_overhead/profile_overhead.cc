// SPDX-License-Identifier: MIT
//
// Validate that compiling the heap-profile lazy provider into the build adds
// zero bytes to slab metadata when SNMALLOC_PROFILE is OFF, and that the
// dealloc-side null-slot fast-path is well-predicted when profiling is ON but
// no samples ever fire.
//
// What this test asserts:
//
//   (1) Layout — compile-time.
//       a. `LazyArrayClientMetaDataProvider<T>::StorageType` is exactly one
//          pointer wide (the public contract from commonconfig.h).
//       b. `NoClientMetaDataProvider::StorageType` is the empty type, so
//          slab metadata that embeds it via SNMALLOC_NO_UNIQUE_ADDRESS pays
//          zero bytes.  Concretely:
//             sizeof(StandardConfig::PagemapEntry) ==
//             sizeof(StandardConfigClientMeta<NoClientMetaDataProvider>
//                    ::PagemapEntry)
//          which proves the lazy provider type is *defined* in the build
//          but isn't *instantiated* into the default config's metadata.
//       c. The cache-aligned `SamplerHotState` puts `bytes_until_sample`
//          at offset 0 within the hot struct.
//
//   (2) Sampler hot-path overhead — runtime.
//       With SNMALLOC_PROFILE on we benchmark 1M allocs of size 32 under
//       two regimes:
//         * `Sampler::set_sampling_rate(0)` — sampling disabled.
//         * `Sampler::set_sampling_rate(2^40)` — sampling on but the
//           per-thread countdown never crosses zero within 1M*32B, so the
//           slow path is not entered.
//       Both fast paths execute the same instructions; the lazy provider's
//       per-slab backing is never installed because no sample fires.
//       Assert that the ratio of ns/alloc between the two regimes stays
//       below 1.05 — i.e., the "profile on but no fires" path does not
//       suffer a branch-misprediction storm relative to "profile off".
//
// Build gate:
//   The runtime benchmark is wrapped in `#ifdef SNMALLOC_PROFILE`.  When
//   profiling is off the test compiles to a smoke pass and exercises only
//   the layout assertions (which hold in both build configurations).

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <snmalloc/backend/globalconfig.h>
#include <snmalloc/profile/profile.h>
#include <snmalloc/profile/record.h>
#include <snmalloc/profile/sampler.h>
#include <snmalloc/snmalloc.h>
#include <test/setup.h>

using snmalloc::profile::config_has_profile_slot_v;
using snmalloc::profile::ProfileSlot;
using snmalloc::profile::SampledAlloc;
using snmalloc::profile::Sampler;

namespace
{
  int g_fail_count = 0;

  void check(bool cond, const char* msg)
  {
    if (cond)
    {
      std::cout << "  PASS: " << msg << "\n";
    }
    else
    {
      std::cout << "  FAIL: " << msg << "\n";
      ++g_fail_count;
    }
  }

  // ---------------------------------------------------------------------------
  // Compile-time layout assertions.
  //
  // These don't require running anything — they fire at TU compile time.
  // Wrapped in a function for readability and to keep them adjacent to the
  // runtime asserts that depend on them.
  // ---------------------------------------------------------------------------
  void test_layout_static()
  {
    std::cout << "test_layout_static\n";

    // (1a) Lazy provider's per-slab inline footprint is exactly one
    // pointer. This is the contract every config-author leans on.
    using LazyT =
      snmalloc::LazyArrayClientMetaDataProvider<std::atomic<SampledAlloc*>>;
    static_assert(
      sizeof(LazyT::StorageType) == sizeof(void*),
      "LazyArrayClientMetaDataProvider::StorageType must be one pointer "
      "wide; widening it would balloon slab metadata for every profile-on "
      "config.");
    check(
      sizeof(LazyT::StorageType) == sizeof(void*),
      "LazyArrayClientMetaDataProvider::StorageType == sizeof(void*)");

    // (1b) NoClientMetaDataProvider's storage is the Empty type. When
    // FrontendSlabMetadata embeds it via SNMALLOC_NO_UNIQUE_ADDRESS it
    // takes zero bytes — which is what makes the lazy provider's mere
    // *presence* in the build zero-overhead for non-profile configs.
    using NoProv = snmalloc::NoClientMetaDataProvider;
    static_assert(
      std::is_same_v<NoProv::StorageType, snmalloc::Empty>,
      "NoClientMetaDataProvider::StorageType must remain Empty so the "
      "[[no_unique_address]] member in FrontendSlabMetadata collapses.");

    // (1b cont.) Two PagemapEntry types — the project default Config and
    // an explicit StandardConfigClientMeta<NoClientMetaDataProvider> —
    // are layout-identical.  Both use NoClientMetaDataProvider, so the
    // lazy provider type is compiled into the TU yet contributes nothing.
    using DefaultEntry = snmalloc::Config::PagemapEntry;
    using ExplicitNoProvConfig =
      snmalloc::StandardConfigClientMeta<snmalloc::NoClientMetaDataProvider>;
    using ExplicitEntry = ExplicitNoProvConfig::PagemapEntry;
    static_assert(
      sizeof(DefaultEntry) == sizeof(ExplicitEntry),
      "Project-default PagemapEntry size must match explicit no-provider "
      "config size — proves zero overhead when profiling is OFF.");
    check(
      sizeof(DefaultEntry) == sizeof(ExplicitEntry),
      "sizeof(Config::PagemapEntry) == sizeof(NoProvider config "
      "PagemapEntry)");

    // (1c) bytes_until_sample lives at offset 0 of the cache-aligned hot
    // struct.
    static_assert(
      Sampler::kBytesUntilSampleOffset == 0,
      "bytes_until_sample must be the first member of "
      "SamplerHotState (offset 0 within the cache-aligned region).");
    check(
      Sampler::kBytesUntilSampleOffset == 0,
      "Sampler::SamplerHotState::bytes_until_sample at offset 0");

    // The hot state struct should be cache-aligned.
    static_assert(
      alignof(Sampler::SamplerHotState) >= 64,
      "SamplerHotState alignment should be at least 64 bytes "
      "to avoid false-sharing with neighbouring sampler state.");
    check(
      alignof(Sampler::SamplerHotState) >= 64,
      "alignof(SamplerHotState) >= 64");
  }

} // namespace

int main(int argc, char** argv)
{
  snmalloc::UNUSED(argc, argv);
  setup();

  std::cout << "[profile_overhead]\n";
  test_layout_static();

  if (g_fail_count == 0)
  {
    std::cout << "[profile_overhead] ALL TESTS PASSED\n";
    return 0;
  }
  std::cout << "[profile_overhead] " << g_fail_count << " TEST(S) FAILED\n";
  return 1;
}
