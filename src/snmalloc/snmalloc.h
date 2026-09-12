#pragma once

// Core implementation of snmalloc independent of the configuration mode
#include "snmalloc_core.h"

// Provides the global configuration for the snmalloc implementation.
#include "backend/globalconfig.h"

#ifdef SNMALLOC_ENABLE_GWP_ASAN_INTEGRATION
#  include "snmalloc/mem/secondary/gwp_asan.h"
#  ifdef SNMALLOC_PROFILE_REFILL_SAMPLING
#    include "snmalloc/mem/secondary/chain.h"
#    include "snmalloc/mem/secondary/default.h"
#  endif
#endif
#ifdef SNMALLOC_PROFILE_SECONDARY_STORAGE
#  include "snmalloc/profile/spike_secondary.h"
#  ifdef SNMALLOC_ENABLE_GWP_ASAN_INTEGRATION
#    include "snmalloc/mem/secondary/chain.h"
#  endif
#endif
#if defined(SNMALLOC_PROFILE_REFILL_SAMPLING) && \
  !defined(SNMALLOC_PROFILE_SECONDARY_STORAGE)
#  include "snmalloc/profile/sampled_alloc.h"

#  include <atomic>
#endif

namespace snmalloc
{
// If you define SNMALLOC_PROVIDE_OWN_CONFIG then you must provide your own
// definition of `snmalloc::Alloc` before including any files that include
// `snmalloc.h` or consume the global allocation APIs.
#ifndef SNMALLOC_PROVIDE_OWN_CONFIG
#  if defined(SNMALLOC_PROFILE_SECONDARY_STORAGE) && \
    defined(SNMALLOC_ENABLE_GWP_ASAN_INTEGRATION)
  using ProfileSecondaryChain = SecondaryAllocatorChain<
    GwpAsanSecondaryAllocator,
    profile::spike::ProfileSecondaryAllocator>;
  using Config = snmalloc::
    StandardConfigClientMeta<NoClientMetaDataProvider, ProfileSecondaryChain>;
#  elif defined(SNMALLOC_PROFILE_SECONDARY_STORAGE)
  using Config = snmalloc::StandardConfigClientMeta<
    NoClientMetaDataProvider,
    profile::spike::ProfileSecondaryAllocator>;
#  elif defined(SNMALLOC_PROFILE_REFILL_SAMPLING) && \
    defined(SNMALLOC_ENABLE_GWP_ASAN_INTEGRATION)
  using RefillGwpSecondaryChain = SecondaryAllocatorChain<
    GwpAsanSecondaryAllocator,
    DefaultSecondaryAllocator>;
  using Config = snmalloc::StandardConfigClientMeta<
    LazyArrayClientMetaDataProvider<std::atomic<profile::SampledAlloc*>>,
    RefillGwpSecondaryChain>;
#  elif defined(SNMALLOC_ENABLE_GWP_ASAN_INTEGRATION)
  using Config = snmalloc::StandardConfigClientMeta<
    NoClientMetaDataProvider,
    GwpAsanSecondaryAllocator>;
#  elif defined(SNMALLOC_PROFILE_REFILL_SAMPLING)
  using Config = snmalloc::StandardConfigClientMeta<
    LazyArrayClientMetaDataProvider<std::atomic<profile::SampledAlloc*>>>;
#  else
  using Config = snmalloc::StandardConfigClientMeta<NoClientMetaDataProvider>;
#  endif
#endif
  /**
   * Create allocator type for this configuration.
   */
  using Alloc = snmalloc::Allocator<Config>;
} // namespace snmalloc

// User facing API surface, needs to know what `Alloc` is.
#include "snmalloc_front.h"
