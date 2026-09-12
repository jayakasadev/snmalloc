// SPDX-License-Identifier: MIT

#pragma once

#ifdef SNMALLOC_PROFILE
#  include <snmalloc/backend/globalconfig.h>
#  include <snmalloc/snmalloc_core.h>

#  ifdef SNMALLOC_PROFILE_SECONDARY_STORAGE
#    include <snmalloc/profile/spike_secondary.h>
#    ifdef SNMALLOC_ENABLE_GWP_ASAN_INTEGRATION
#      include <snmalloc/mem/secondary/chain.h>
#      include <snmalloc/mem/secondary/gwp_asan.h>
#    endif
#  else
#    include <atomic>
#    include <snmalloc/profile/sampled_alloc.h>
#    if defined(SNMALLOC_PROFILE_REFILL_SAMPLING) && \
      defined(SNMALLOC_ENABLE_GWP_ASAN_INTEGRATION)
#      include <snmalloc/mem/secondary/chain.h>
#      include <snmalloc/mem/secondary/default.h>
#      include <snmalloc/mem/secondary/gwp_asan.h>
#    endif
#  endif

namespace snmalloc
{
#  ifdef SNMALLOC_PROFILE_SECONDARY_STORAGE
#    ifdef SNMALLOC_ENABLE_GWP_ASAN_INTEGRATION
  using ProfileSecondaryChain = SecondaryAllocatorChain<
    GwpAsanSecondaryAllocator,
    profile::spike::ProfileSecondaryAllocator>;
  using Config =
    StandardConfigClientMeta<NoClientMetaDataProvider, ProfileSecondaryChain>;
#    else
  using Config = StandardConfigClientMeta<
    NoClientMetaDataProvider,
    profile::spike::ProfileSecondaryAllocator>;
#    endif
#  else
#    if defined(SNMALLOC_PROFILE_REFILL_SAMPLING) && \
      defined(SNMALLOC_ENABLE_GWP_ASAN_INTEGRATION)
  using RefillGwpSecondaryChain = SecondaryAllocatorChain<
    GwpAsanSecondaryAllocator,
    DefaultSecondaryAllocator>;
  using Config = StandardConfigClientMeta<
    LazyArrayClientMetaDataProvider<std::atomic<profile::SampledAlloc*>>,
    RefillGwpSecondaryChain>;
#    else
  using Config = StandardConfigClientMeta<
    LazyArrayClientMetaDataProvider<std::atomic<profile::SampledAlloc*>>>;
#    endif
#  endif
}

#  define SNMALLOC_PROVIDE_OWN_CONFIG
#endif
