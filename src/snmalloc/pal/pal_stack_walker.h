#pragma once

/**
 * Stack walker used by the heap profiler.
 *
 * Frame-pointer walker on x86_64 / aarch64 with Linux or macOS; elsewhere a
 * stub that returns no frames.
 *
 * The frame-pointer walker validates every frame, so a corrupt or absent
 * chain just yields a shorter trace. First use queries pthread stack bounds
 * and is therefore not async-signal-safe.
 *
 * Define SNMALLOC_PROFILE_STACK_WALKER_FP or
 * SNMALLOC_PROFILE_STACK_WALKER_NULL before including this header to force
 * one walker; otherwise it is chosen from the target arch and OS.
 */

#include "../ds_core/defines.h"
#include "pal_consts.h"

#include <stddef.h>
#include <stdint.h>

#if !defined(SNMALLOC_PROFILE_STACK_WALKER_FP) && \
  !defined(SNMALLOC_PROFILE_STACK_WALKER_NULL)
#  if (defined(__x86_64__) || defined(__aarch64__)) && \
    (defined(__linux__) || defined(__APPLE__)) && \
    !defined(__CHERI_PURE_CAPABILITY__)
#    define SNMALLOC_PROFILE_STACK_WALKER_FP 1
#  else
#    define SNMALLOC_PROFILE_STACK_WALKER_NULL 1
#  endif
#endif

#if defined(SNMALLOC_PROFILE_STACK_WALKER_FP)
#  if defined(__linux__) || defined(__APPLE__)
#    include <pthread.h>
#  endif
#  if defined(__APPLE__) && __has_include(<ptrauth.h>)
#    include <ptrauth.h>
#  endif
#endif

namespace snmalloc
{
  /**
   * Which walker this build selected, so callers can opt out gracefully when
   * it is the stub.
   */
  enum class StackWalkerKind : uint8_t
  {
    Null = 0,
    FramePointer = 1,
  };

  namespace profile
  {
#if defined(SNMALLOC_PROFILE_STACK_WALKER_FP)

    // Clear Pointer Authentication bits from an aarch64 saved link register;
    // with them set the value is not a usable code address. Whether they are
    // present depends on runtime state, so the strip is unconditional.
    // Identity on x86_64.
    SNMALLOC_FAST_PATH_INLINE uintptr_t strip_pac(uintptr_t lr) noexcept
    {
#  if defined(__aarch64__)
#    if defined(__APPLE__) && __has_include(<ptrauth.h>)
      return reinterpret_cast<uintptr_t>(
        ptrauth_strip(reinterpret_cast<void*>(lr), ptrauth_key_return_address));
#    elif defined(__GNUC__) || defined(__clang__)
      // `xpaclri` strips the bits on ARMv8.3+ and decodes to NOP elsewhere.
      register uintptr_t x30 __asm__("x30") = lr;
      __asm__("hint #7" /* xpaclri */ : "+r"(x30));
      return x30;
#    else
      // Clear the top byte and PAC region; already zero without PAC.
      return lr & ((uintptr_t{1} << 56) - 1);
#    endif
#  else
      return lr;
#  endif
    }

    // Cached bounds of the current thread's stack, held in a plain
    // thread_local so first access runs no constructor and cannot malloc:
    // dynamically-initialised TLS could re-enter the allocator.
    struct StackBounds
    {
      uintptr_t lo;
      uintptr_t hi;
      bool valid;
    };

    namespace detail
    {
      inline thread_local StackBounds tls_bounds = {0, 0, false};

      inline void populate_bounds(StackBounds& b) noexcept
      {
#  if defined(__APPLE__)
        // Darwin gives the high end of the stack.
        void* hi = pthread_get_stackaddr_np(pthread_self());
        size_t sz = pthread_get_stacksize_np(pthread_self());
        const uintptr_t hi_u = reinterpret_cast<uintptr_t>(hi);
        if (hi != nullptr && sz != 0 && hi_u >= sz)
        {
          b.hi = hi_u;
          b.lo = hi_u - sz;
          b.valid = true;
        }
#  elif defined(__linux__)
        pthread_attr_t attr;
        if (pthread_getattr_np(pthread_self(), &attr) == 0)
        {
          void* lo = nullptr;
          size_t sz = 0;
          if (pthread_attr_getstack(&attr, &lo, &sz) == 0 && lo != nullptr)
          {
            const uintptr_t lo_u = reinterpret_cast<uintptr_t>(lo);
            if (sz != 0 && lo_u <= UINTPTR_MAX - sz)
            {
              b.lo = lo_u;
              b.hi = lo_u + sz;
              b.valid = true;
            }
          }
          pthread_attr_destroy(&attr);
        }
#  else
        b.valid = false;
#  endif
      }
    } // namespace detail

    inline const StackBounds& get_thread_stack_bounds() noexcept
    {
      if (SNMALLOC_LIKELY(detail::tls_bounds.valid))
        return detail::tls_bounds;
      detail::populate_bounds(detail::tls_bounds);
      return detail::tls_bounds;
    }

    /**
     * Drop the cached bounds for this thread. Call after swapping the thread
     * onto a different stack (fibres, ucontext_t). Idempotent.
     */
    inline void invalidate_thread_stack_bounds() noexcept
    {
      detail::tls_bounds.valid = false;
    }

    // Frame-pointer walker.
    //
    // `capture` writes up to `max_depth` return addresses into `out`, which
    // must have room for that many, and returns how many it wrote. Frame 0
    // is `capture`'s caller; `skip` discards that many frames first.
    struct FramePointerWalker
    {
      static constexpr StackWalkerKind kind = StackWalkerKind::FramePointer;

      static constexpr const char* name() noexcept
      {
        return "fp";
      }

      static SNMALLOC_FAST_PATH_INLINE size_t
      capture(uintptr_t* out, size_t max_depth, size_t skip = 0) noexcept
      {
        if (SNMALLOC_UNLIKELY(max_depth == 0))
          return 0;

        const StackBounds& bounds = get_thread_stack_bounds();
        if (SNMALLOC_UNLIKELY(!bounds.valid))
          return 0;

        auto* fp = static_cast<void**>(__builtin_frame_address(0));
        if (SNMALLOC_UNLIKELY(fp == nullptr))
          return 0;

        uintptr_t prev_fp = 0;
        size_t depth = 0;
        size_t skipped = 0;

        // Cap the iterations so a corrupt chain cannot loop forever.
        const size_t max_iters = max_depth + skip + 1;
        for (size_t iter = 0; iter < max_iters; ++iter)
        {
          const auto fp_u = reinterpret_cast<uintptr_t>(fp);

          // The two-word frame at `fp` must lie inside the thread's stack, be
          // pointer-aligned, and sit strictly above the previous frame: the
          // chain climbs on a grows-down stack, so anything else is a cycle
          // or corruption.
          if (SNMALLOC_UNLIKELY(
                fp_u < bounds.lo || bounds.hi < 2 * sizeof(void*) ||
                fp_u > bounds.hi - 2 * sizeof(void*) || fp_u <= prev_fp ||
                (fp_u & (sizeof(void*) - 1)) != 0))
            break;

          void* next_fp_raw = fp[0];
          void* ret_addr = fp[1];

          if (SNMALLOC_UNLIKELY(ret_addr == nullptr))
            break;

          uintptr_t pc = strip_pac(reinterpret_cast<uintptr_t>(ret_addr));

          if (skipped < skip)
          {
            ++skipped;
          }
          else
          {
            out[depth++] = pc;
            if (depth >= max_depth)
              break;
          }

          prev_fp = fp_u;
          fp = static_cast<void**>(next_fp_raw);

          // Thread entry code zeroes the saved frame pointer to end the
          // chain.
          if (fp == nullptr)
            break;
        }

        return depth;
      }
    };

    using DefaultStackWalker = FramePointerWalker;

#else // SNMALLOC_PROFILE_STACK_WALKER_NULL

    /**
     * Stub for platforms with no walker implementation. Captures no frames.
     */
    struct NullStackWalker
    {
      static constexpr StackWalkerKind kind = StackWalkerKind::Null;

      static constexpr const char* name() noexcept
      {
        return "null";
      }

      static SNMALLOC_FAST_PATH_INLINE size_t
      capture(uintptr_t* out, size_t max_depth, size_t skip = 0) noexcept
      {
        (void)out;
        (void)max_depth;
        (void)skip;
        return 0;
      }
    };

    inline void invalidate_thread_stack_bounds() noexcept {}

    using DefaultStackWalker = NullStackWalker;

#endif

    /**
     * Capture a stack with whichever walker this build selected.
     */
    SNMALLOC_FAST_PATH_INLINE size_t
    stack_walk(uintptr_t* out, size_t max_depth, size_t skip = 0) noexcept
    {
      return DefaultStackWalker::capture(out, max_depth, skip);
    }

  } // namespace profile
} // namespace snmalloc
