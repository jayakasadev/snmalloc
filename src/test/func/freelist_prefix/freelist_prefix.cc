#include <array>
#include <iostream>
#include <snmalloc/snmalloc.h>
#ifdef SNMALLOC_PROFILE_REFILL_SAMPLING
#  include <snmalloc/profile/spike_freelist_trim.h>
#endif
#include <test/setup.h>

template<bool Random>
bool check_prefix()
{
  constexpr size_t count = 12;
  alignas(64) std::array<std::array<unsigned char, 64>, count> storage{};
  snmalloc::LocalEntropy entropy;
  entropy.init<snmalloc::DefaultPal>();

  snmalloc::freelist::Builder<Random, true> builder;
  const snmalloc::FreeListKey key{3, 5, 7};
  builder.init(reinterpret_cast<snmalloc::address_t>(storage.data()), key, 11);
  for (auto& object : storage)
  {
    auto p = snmalloc::capptr::Alloc<void>::unsafe_from(object.data());
    builder.add(
      snmalloc::freelist::Object::make<snmalloc::capptr::bounds::AllocWild>(p),
      key,
      11,
      entropy);
  }

  snmalloc::freelist::Iter<> prefix;
  auto domesticate = [](snmalloc::freelist::QueuePtr p) {
    return snmalloc::freelist::HeadPtr::unsafe_from(p.unsafe_ptr());
  };
  uint16_t remaining = builder.close_prefix(prefix, key, 11, 3, domesticate);
  size_t taken = 0;
  std::array<snmalloc::freelist::HeadPtr, 3> initial_detached{};
  while (!prefix.empty())
  {
    initial_detached[taken] = prefix.take(key, domesticate);
    taken++;
  }
  if (taken != 3 || remaining != count - 3 || builder.count() != count - 3)
    return false;

  for (size_t i = 0; i < 3; i++)
    builder.add(initial_detached[i], key, 11, entropy);
  if (builder.count() != count)
    return false;

  std::array<snmalloc::freelist::HeadPtr, count> detached{};
  for (size_t round = 0; round < 1000; round++)
  {
    snmalloc::freelist::Iter<> list;
    const uint16_t before = builder.count();
    const uint16_t left = builder.close_prefix(
      list, key, 11, static_cast<uint16_t>(1 + (round % 5)), domesticate);
    size_t detached_count = 0;
    while (!list.empty())
      detached[detached_count++] = list.take(key, domesticate);

    if (
      detached_count != static_cast<size_t>(before - left) ||
      builder.count() != left)
      return false;

    for (size_t i = 0; i < detached_count; i++)
      builder.add(detached[i], key, 11, entropy);
    if (builder.count() != count)
      return false;
  }

  size_t drained = 0;
  while (!builder.empty())
  {
    snmalloc::freelist::Iter<> list;
    builder.close(list, key, 11);
    while (!list.empty())
    {
      list.take(key, domesticate);
      drained++;
    }
  }
  return drained == count && builder.count() == 0;
}

int main()
{
  if (!check_prefix<false>() || !check_prefix<true>())
  {
    std::cerr << "freelist prefix close invariant failed\n";
    return 1;
  }
#ifdef SNMALLOC_PROFILE_REFILL_SAMPLING
  using RefillSampler = snmalloc::profile::spike::RefillSampler;
  if (
    RefillSampler::ordinary_allocations_before_sample(192, 64) != 2 ||
    RefillSampler::ordinary_allocations_before_sample(193, 64) != 3)
  {
    std::cerr << "allocated-byte refill exhaustion math failed\n";
    return 1;
  }

  RefillSampler sampler;
  snmalloc::smallsizeclass_t first{};
  snmalloc::smallsizeclass_t second{1};
  sampler.debug_set_small_countdown(first, 193);
  sampler.debug_set_small_countdown(second, 513);
  sampler.debit(first, 64, 3);
  if (
    sampler.debug_small_countdown(first) != 1 ||
    sampler.debug_small_countdown(second) != 513)
  {
    std::cerr << "sizeclass countdowns are not independent\n";
    return 1;
  }
#endif
  return 0;
}
