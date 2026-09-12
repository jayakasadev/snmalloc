// SPDX-License-Identifier: MIT
//
// C ABI for the runtime tunables in `src/snmalloc/global/runtime_config.h`.
// Each function is a passthrough to `snmalloc::RuntimeConfig`.
//
// These symbols are exported in every build configuration, whatever the
// SNMALLOC_PROFILE and SNMALLOC_STATS settings, so callers do not have to
// probe for them.  Safe to call from any thread at any time.

#include "snmalloc/global/runtime_config.h"

#include "../snmalloc.h"

#ifdef SNMALLOC_PROFILE
#  include "../profile/sampler.h"
#endif

#include <stdint.h>

#ifndef SNMALLOC_EXPORT
#  define SNMALLOC_EXPORT
#endif

using snmalloc::RuntimeConfig;

extern "C" SNMALLOC_EXPORT void snmalloc_set_sample_interval(uint64_t bytes)
{
#ifdef SNMALLOC_PROFILE
  snmalloc::profile::Sampler::set_sampling_rate(static_cast<size_t>(bytes));
  bytes = snmalloc::profile::Sampler::get_sampling_rate();
#endif
  RuntimeConfig::set_sample_interval_bytes(bytes);
}

extern "C" SNMALLOC_EXPORT void snmalloc_set_decay_rate(uint32_t milliseconds)
{
  RuntimeConfig::set_decay_rate_ms(milliseconds);
}

extern "C" SNMALLOC_EXPORT void snmalloc_set_max_local_cache(uint64_t bytes)
{
  RuntimeConfig::set_max_local_cache_bytes(bytes);
}

extern "C" SNMALLOC_EXPORT uint64_t snmalloc_get_sample_interval(void)
{
  return RuntimeConfig::sample_interval_bytes();
}

extern "C" SNMALLOC_EXPORT uint32_t snmalloc_get_decay_rate(void)
{
  return RuntimeConfig::decay_rate_ms();
}

extern "C" SNMALLOC_EXPORT uint64_t snmalloc_get_max_local_cache(void)
{
  return RuntimeConfig::max_local_cache_bytes();
}
