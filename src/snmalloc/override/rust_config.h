// SPDX-License-Identifier: MIT

#pragma once

#ifdef SNMALLOC_PROFILE
#  include <atomic>
#  include <snmalloc/backend/globalconfig.h>
#  include <snmalloc/profile/sampled_alloc.h>
#  include <snmalloc/snmalloc_core.h>

namespace snmalloc
{
  using Config = StandardConfigClientMeta<
    LazyArrayClientMetaDataProvider<std::atomic<profile::SampledAlloc*>>>;
}

#  define SNMALLOC_PROVIDE_OWN_CONFIG
#endif
