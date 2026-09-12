#pragma once

#include "../ds/sizeclasstable.h"
#include "../ds_core/defines.h"
#include "allocation_sample_list.h"
#include "lifetime_histogram.h"
#include "reentrancy_guard.h"
#include "sampler.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <stddef.h>

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <sys/mman.h>
#  include <unistd.h>
#endif

namespace snmalloc::profile::spike
{
  struct PendingSecondarySample
  {
    SampledAlloc* node{nullptr};
  };

  inline thread_local PendingSecondarySample pending_secondary_sample;

  inline void
  prepare_current_countdown_sample(size_t requested, size_t allocated) noexcept
  {
    if (pending_secondary_sample.node != nullptr)
    {
      SamplerGlobals::pool().release(pending_secondary_sample.node);
      pending_secondary_sample.node = nullptr;
    }
    if (tl_record_alloc(0, requested, allocated, false))
    {
      auto* node = tl_sampler.last_sample();
      if (node != nullptr)
      {
        node->alloc_ts_ns = static_cast<uint64_t>(
          std::chrono::steady_clock::now().time_since_epoch().count());
        pending_secondary_sample.node = node;
      }
    }
  }

  class ProfileSecondaryAllocator
  {
    struct Block
    {
      Block* next;
      void* user;
      size_t capacity;
      size_t mapping_size;
      SampledAlloc* sample;
      bool active;
    };

    static inline std::atomic_flag lock_ = ATOMIC_FLAG_INIT;
    static inline Block* blocks_ = nullptr;

    class Guard
    {
      bool held_{true};

    public:
      Guard() noexcept
      {
        while (lock_.test_and_set(std::memory_order_acquire))
        {
        }
      }

      ~Guard() noexcept
      {
        if (held_)
          lock_.clear(std::memory_order_release);
      }

      void release() noexcept
      {
        lock_.clear(std::memory_order_release);
        held_ = false;
      }
    };

    static size_t page_size() noexcept
    {
#if defined(_WIN32)
      SYSTEM_INFO info;
      ::GetSystemInfo(&info);
      return info.dwPageSize;
#else
      long result = ::sysconf(_SC_PAGESIZE);
      return result > 0 ? static_cast<size_t>(result) : 4096;
#endif
    }

    static void* map(size_t bytes) noexcept
    {
#if defined(_WIN32)
      return ::VirtualAlloc(
        nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#else
      void* p = ::mmap(
        nullptr,
        bytes,
        PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS,
        -1,
        0);
      return p == MAP_FAILED ? nullptr : p;
#endif
    }

    static Block* find(const void* pointer, bool active_only) noexcept
    {
      for (Block* b = blocks_; b != nullptr; b = b->next)
      {
        if (b->user == pointer && (!active_only || b->active))
          return b;
      }
      return nullptr;
    }

  public:
    static constexpr inline bool pass_through = false;

    static void initialize() noexcept {}

    static void cancel_pending() noexcept
    {
      SampledAlloc* sample =
        stl::exchange(pending_secondary_sample.node, nullptr);
      SamplerGlobals::pool().release(sample);
    }

    [[nodiscard]] static bool has_pending() noexcept
    {
      return pending_secondary_sample.node != nullptr;
    }

    template<class SizeAlign>
    SNMALLOC_FAST_PATH static void* allocate(SizeAlign&& getter)
    {
      SampledAlloc* sample = pending_secondary_sample.node;
      if (sample == nullptr)
        return nullptr;
      auto [size, align] = getter();
      if (size > (SIZE_MAX / 2) || align == 0 || (align & (align - 1)) != 0)
      {
        cancel_pending();
        return nullptr;
      }
      const size_t capacity = round_size(size);
      if (capacity < size)
      {
        cancel_pending();
        return nullptr;
      }
      if (align < alignof(void*))
        align = alignof(void*);

      Guard guard;
      Block* block = nullptr;
      for (Block* b = blocks_; b != nullptr; b = b->next)
      {
        if (
          !b->active && b->capacity >= capacity &&
          (reinterpret_cast<uintptr_t>(b->user) & (align - 1)) == 0)
        {
          block = b;
          break;
        }
      }

      if (block == nullptr)
      {
        const size_t page = page_size();
        const size_t align_slop = align - 1;
        if (
          sizeof(Block) > SIZE_MAX - align_slop ||
          capacity > SIZE_MAX - sizeof(Block) - align_slop)
        {
          cancel_pending();
          return nullptr;
        }
        const size_t needed = sizeof(Block) + align_slop + capacity;
        if (needed > SIZE_MAX - (page - 1))
        {
          cancel_pending();
          return nullptr;
        }
        const size_t mapping_size = (needed + page - 1) & ~(page - 1);
        void* mapping = map(mapping_size);
        if (mapping == nullptr)
        {
          cancel_pending();
          return nullptr;
        }
        block = new (mapping) Block{};
        uintptr_t user = reinterpret_cast<uintptr_t>(block + 1);
        user = (user + align - 1) & ~(align - 1);
        block->user = reinterpret_cast<void*>(user);
        block->capacity = capacity;
        block->mapping_size = mapping_size;
        block->next = blocks_;
        blocks_ = block;
      }

      block->active = true;
      block->sample = sample;
      sample->alloc_addr = reinterpret_cast<uintptr_t>(block->user);
      sample->requested_size.store(size, std::memory_order_relaxed);
      sample->allocated_size.store(block->capacity, std::memory_order_relaxed);
      pending_secondary_sample.node = nullptr;
      guard.release();
      SamplerGlobals::list().push(sample);
      {
        ReentrancyGuard broadcast_guard;
        AllocationSampleList::global().broadcast(*sample);
      }
      return block->user;
    }

    SNMALLOC_FAST_PATH
    static void deallocate(void* pointer)
    {
      Guard guard;
      Block* block = find(pointer, true);
      snmalloc_check_client(
        mitigations(sanity_checks),
        block != nullptr,
        "Not allocated by profiling secondary allocator");
      if (block == nullptr)
        return;
      SampledAlloc* sample = block->sample;
      block->sample = nullptr;
      block->active = false;
      if (sample != nullptr)
      {
        const uint64_t now = static_cast<uint64_t>(
          std::chrono::steady_clock::now().time_since_epoch().count());
        const uint64_t lifetime =
          now > sample->alloc_ts_ns ? now - sample->alloc_ts_ns : 1;
        LifetimeHistogram::get().record_lifetime_ns(lifetime);
        SamplerGlobals::list().remove(sample);
        SamplerGlobals::pool().release(sample);
      }
    }

    SNMALLOC_FAST_PATH
    static size_t alloc_size(const void* pointer)
    {
      Guard guard;
      Block* block = find(pointer, true);
      return block == nullptr ? 0 : block->capacity;
    }

    SNMALLOC_FAST_PATH
    static bool owns(const void* pointer)
    {
      Guard guard;
      return find(pointer, true) != nullptr;
    }
  };
} // namespace snmalloc::profile::spike
