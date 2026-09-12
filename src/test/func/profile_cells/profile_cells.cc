#include <array>
#include <iostream>
#include <snmalloc/override/rust_config.h>
#include <snmalloc/profile/profile.h>
#include <snmalloc/snmalloc.h>
#include <test/setup.h>

int main()
{
#ifdef SNMALLOC_PROFILE
  constexpr size_t count = 2048;
  std::array<void*, count> allocations{};
  auto& samples = snmalloc::profile::SamplerGlobals::list();
  const size_t baseline = samples.debug_count();

#  ifdef SNMALLOC_PROFILE_REFILL_SAMPLING
  using RefillSampler = snmalloc::profile::spike::RefillSampler;
  snmalloc::smallsizeclass_t exact_class{};
  RefillSampler exact;

  snmalloc::profile::Sampler::set_sampling_rate(64);
  exact.debug_seed_small(exact_class);
  exact.debug_set_small_countdown(exact_class, 64);
  exact.debit(exact_class, 64, 1);
  const size_t exact_limit = exact.transfer_limit(exact_class, 64);
  if (exact_limit != 0 || exact.debug_small_countdown(exact_class) != 0)
  {
    std::cerr << "exact-zero debit was redrawn before a real allocation\n";
    return 1;
  }

  snmalloc::profile::SampledAlloc* exact_sample =
    exact.prepare_small_sample(exact_class, 64);
  if (exact_sample == nullptr)
  {
    std::cerr << "exact-zero sample was not retained\n";
    return 1;
  }

#    ifdef SNMALLOC_PROFILE_SECONDARY_STORAGE
  void* exact_pointer = snmalloc::Config::SecondaryAllocator::allocate(
    [] { return snmalloc::stl::Pair<size_t, size_t>{64, 64}; });
  if (exact_pointer == nullptr || samples.debug_count() != baseline + 1)
  {
    std::cerr << "exact-zero secondary sample was not published\n";
    return 1;
  }
  snmalloc::Config::SecondaryAllocator::deallocate(exact_pointer);
#    else
  snmalloc::profile::Sampler::set_sampling_rate(0);
  void* exact_pointer = snmalloc::alloc(64);
  snmalloc::profile::finalize_prepared_alloc<snmalloc::Config>(
    exact_pointer, 64, 64, exact.take_main_sample());
  if (samples.debug_count() != baseline + 1)
  {
    std::cerr << "exact-zero main sample was not published\n";
    return 1;
  }
  snmalloc::dealloc(exact_pointer);

  snmalloc::profile::Sampler::set_sampling_rate(64);
  exact.debug_set_small_countdown(exact_class, 64);
  exact.debit(exact_class, 64, 1);
  snmalloc::profile::SampledAlloc* failed_sample =
    exact.prepare_small_sample(exact_class, 64);
  snmalloc::profile::finalize_prepared_alloc<snmalloc::Config>(
    nullptr, 64, 64, exact.take_main_sample());
  if (
    failed_sample == nullptr || exact.take_main_sample() != nullptr ||
    failed_sample->state.load(std::memory_order_relaxed) !=
      static_cast<uint8_t>(snmalloc::profile::NodeState::Free) ||
    samples.debug_count() != baseline)
  {
    std::cerr << "failed main allocation left stale pending state\n";
    return 1;
  }
#    endif

  if (
    samples.debug_count() != baseline ||
    exact_sample->state.load(std::memory_order_relaxed) !=
      static_cast<uint8_t>(snmalloc::profile::NodeState::Free))
  {
    std::cerr << "exact-zero sample did not drain\n";
    return 1;
  }

  RefillSampler disabled;
  snmalloc::profile::Sampler::set_sampling_rate(0);
  disabled.debug_seed_small(exact_class);
  if (disabled.debug_small_countdown(exact_class) != INT64_MAX / 2)
  {
    std::cerr << "disabled sampler was not parked\n";
    return 1;
  }
  snmalloc::profile::Sampler::set_sampling_rate(64);
  disabled.debug_seed_small(exact_class);
  if (disabled.debug_small_countdown(exact_class) == INT64_MAX / 2)
  {
    std::cerr << "re-enabled sampler stayed parked\n";
    return 1;
  }
#  endif

  snmalloc::profile::Sampler::set_sampling_rate(1);
  for (size_t i = 0; i < 20000; i++)
  {
    void* p = snmalloc::alloc(16 + ((i % 7) * 16));
    if (p == nullptr)
    {
      std::cerr << "immediate allocation failed\n";
      return 1;
    }
    snmalloc::dealloc(p);
  }
  if (samples.debug_count() != baseline)
  {
    std::cerr << "immediate alloc/free did not drain samples\n";
    return 1;
  }

  for (size_t i = 0; i < count; i++)
  {
    allocations[i] = snmalloc::alloc(32 + ((i & 1) * 32));
    if (allocations[i] == nullptr)
    {
      std::cerr << "allocation failed\n";
      return 1;
    }
  }

  if (samples.debug_count() == baseline)
  {
    std::cerr << "profile cell produced no samples\n";
    return 1;
  }
  for (void* p : allocations)
    snmalloc::dealloc(p);

  if (samples.debug_count() != baseline)
  {
    std::cerr << "profile cell did not drain samples: baseline=" << baseline
              << " remaining=" << samples.debug_count() << "\n";
    samples.snapshot([](snmalloc::profile::SampledAlloc* sample) {
      std::cerr << "remaining sample="
                << reinterpret_cast<void*>(sample->alloc_addr) << "\n";
    });
    return 1;
  }

  void* large = snmalloc::alloc(1024 * 1024);
  if (large == nullptr || samples.debug_count() == baseline)
  {
    std::cerr << "profile cell produced no large sample\n";
    return 1;
  }
  snmalloc::dealloc(large);
  if (samples.debug_count() != baseline)
  {
    std::cerr << "profile cell did not drain large sample\n";
    return 1;
  }
#endif
  return 0;
}
