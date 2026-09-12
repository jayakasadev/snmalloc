#include <cstddef>
#include <iostream>
#include <snmalloc/mem/secondary/chain.h>

namespace
{
  struct FirstSuccess
  {
    static void initialize() noexcept {}

    template<typename Getter>
    static void* allocate(Getter&&)
    {
      return reinterpret_cast<void*>(0x1000);
    }

    static bool owns(const void* p)
    {
      return p == reinterpret_cast<void*>(0x1000);
    }

    static void deallocate(void*) {}

    static size_t alloc_size(const void*)
    {
      return 16;
    }
  };

  struct CancelSecond
  {
    static inline size_t cancellations = 0;

    static void initialize() noexcept {}

    static void cancel_pending() noexcept
    {
      cancellations++;
    }

    template<typename Getter>
    static void* allocate(Getter&&)
    {
      return nullptr;
    }

    static bool owns(const void*)
    {
      return false;
    }

    static void deallocate(void*) {}

    static size_t alloc_size(const void*)
    {
      return 0;
    }
  };

  struct NoCancelSecond : CancelSecond
  {
    static void cancel_pending() = delete;
  };
}

int main()
{
  using CancelChain =
    snmalloc::SecondaryAllocatorChain<FirstSuccess, CancelSecond>;
  void* p = CancelChain::allocate([] { return 0; });
  if (p != reinterpret_cast<void*>(0x1000) || CancelSecond::cancellations != 1)
  {
    std::cerr << "first-success did not cancel second pending state\n";
    return 1;
  }

  using PlainChain =
    snmalloc::SecondaryAllocatorChain<FirstSuccess, NoCancelSecond>;
  if (PlainChain::allocate([] { return 0; }) != reinterpret_cast<void*>(0x1000))
  {
    std::cerr << "chain without cancellation hook failed\n";
    return 1;
  }
  return 0;
}
