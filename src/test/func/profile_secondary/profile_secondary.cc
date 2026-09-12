#include <iostream>
#include <limits>
#include <snmalloc/mem/secondary/chain.h>
#include <snmalloc/snmalloc.h>
#include <test/setup.h>

namespace
{
  int first_object;
  int second_object;
  int first_frees;
  int second_frees;

  struct FirstSecondary
  {
    static void initialize() noexcept {}

    template<typename Getter>
    static void* allocate(Getter&&)
    {
      return nullptr;
    }

    static bool owns(const void* p)
    {
      return p == &first_object;
    }

    static void deallocate(void*)
    {
      first_frees++;
    }

    static size_t alloc_size(const void*)
    {
      return 11;
    }
  };

  struct SecondSecondary
  {
    static void initialize() noexcept {}

    template<typename Getter>
    static void* allocate(Getter&&)
    {
      return nullptr;
    }

    static bool owns(const void* p)
    {
      return p == &second_object;
    }

    static void deallocate(void*)
    {
      second_frees++;
    }

    static size_t alloc_size(const void*)
    {
      return 22;
    }
  };
}

int main()
{
  using Chain =
    snmalloc::SecondaryAllocatorChain<FirstSecondary, SecondSecondary>;
  if (
    !Chain::owns(&first_object) || !Chain::owns(&second_object) ||
    Chain::alloc_size(&first_object) != 11 ||
    Chain::alloc_size(&second_object) != 22)
  {
    std::cerr << "secondary chain ownership dispatch failed\n";
    return 1;
  }
  Chain::deallocate(&first_object);
  Chain::deallocate(&second_object);
  if (first_frees != 1 || second_frees != 1)
  {
    std::cerr << "secondary chain free dispatch failed\n";
    return 1;
  }

#if defined(SNMALLOC_PROFILE_SECONDARY_STORAGE)
  using Secondary = snmalloc::Config::SecondaryAllocator;
  snmalloc::profile::Sampler::set_sampling_rate(1);

  const size_t baseline =
    snmalloc::profile::SamplerGlobals::list().debug_count();
  for (size_t i = 0; i < 1024 && !Secondary::has_pending(); i++)
    snmalloc::profile::spike::prepare_current_countdown_sample(1, 1);
  if (!Secondary::has_pending())
  {
    std::cerr << "could not prepare overflow sample\n";
    return 1;
  }
  void* overflow = Secondary::allocate([] {
    return snmalloc::stl::Pair<size_t, size_t>{
      std::numeric_limits<size_t>::max(), alignof(std::max_align_t)};
  });
  if (
    overflow != nullptr || Secondary::has_pending() ||
    snmalloc::profile::SamplerGlobals::list().debug_count() != baseline)
  {
    std::cerr << "overflow did not cancel pending sample cleanly\n";
    return 1;
  }

  void* sampled = nullptr;
  for (size_t i = 0; i < 128 && sampled == nullptr; i++)
  {
    void* p = snmalloc::alloc(32 + (i & 1) * 32);
    if (Secondary::owns(p))
      sampled = p;
    else
      snmalloc::dealloc(p);
  }

  if (sampled == nullptr)
  {
    std::cerr << "no secondary sample\n";
    return 1;
  }
  if (snmalloc::alloc_size(sampled) < 32)
  {
    std::cerr << "secondary alloc_size too small\n";
    return 1;
  }
  if (
    (reinterpret_cast<uintptr_t>(sampled) & (alignof(std::max_align_t) - 1)) !=
    0)
  {
    std::cerr << "secondary alignment is invalid\n";
    return 1;
  }

  snmalloc::dealloc(sampled);
  if (Secondary::owns(sampled))
  {
    std::cerr << "freed secondary pointer is still owned\n";
    return 1;
  }
#endif
  return 0;
}
