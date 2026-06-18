// SPDX-License-Identifier: MIT
//
// Heap profiler -- umbrella header for the snmalloc heap-profile subsystem.
//
// Pulling in this header only makes the subsystem's types visible; it does
// not enable profiling on any allocator path.  Sampling is wired in at the
// alloc/dealloc hooks in corealloc.h, gated on SNMALLOC_PROFILE.
//
// Components:
//   sampler.h           -- per-thread Poisson sampler
//   sampled_alloc.h     -- one record per sampled allocation
//   node_pool.h         -- pre-allocated lock-free pool of records
//   sampled_list.h      -- lock-free intrusive list of live samples
//   reentrancy_guard.h  -- per-thread guard against sampler recursion
//
// record.h (the H1/A1 hook bodies in profile/record.h) is deliberately
// NOT pulled in via this umbrella header: it has a hard dependency on
// the slab-metadata + Config types declared by mem/corealloc.h, and
// including it here would create a header cycle through commonconfig.h.
// Consumers of the hook (just corealloc.h itself) include record.h
// directly behind their own SNMALLOC_PROFILE gate.

#pragma once

#include "node_pool.h"
#include "reentrancy_guard.h"
#include "sampled_alloc.h"
#include "sampled_list.h"
#include "sampler.h"
