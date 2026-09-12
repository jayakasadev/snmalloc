#pragma once

#include "snmalloc/ds_core/defines.h"

namespace snmalloc
{
  template<typename First, typename Second>
  class SecondaryAllocatorChain
  {
    template<typename T>
    static auto cancel_pending(int) noexcept
      -> decltype(T::cancel_pending(), void())
    {
      T::cancel_pending();
    }

    template<typename>
    static void cancel_pending(long) noexcept
    {}

  public:
    static constexpr inline bool pass_through = false;

    static void initialize() noexcept
    {
      First::initialize();
      Second::initialize();
    }

    template<class SizeAlign>
    SNMALLOC_FAST_PATH static void* allocate(SizeAlign&& getter)
    {
      if (void* p = First::allocate(getter))
      {
        cancel_pending<Second>(0);
        return p;
      }
      return Second::allocate(getter);
    }

    SNMALLOC_FAST_PATH static bool owns(const void* pointer)
    {
      return First::owns(pointer) || Second::owns(pointer);
    }

    SNMALLOC_FAST_PATH static void deallocate(void* pointer)
    {
      if (First::owns(pointer))
        First::deallocate(pointer);
      else
        Second::deallocate(pointer);
    }

    SNMALLOC_FAST_PATH static size_t alloc_size(const void* pointer)
    {
      if (First::owns(pointer))
        return First::alloc_size(pointer);
      return Second::alloc_size(pointer);
    }
  };
} // namespace snmalloc
