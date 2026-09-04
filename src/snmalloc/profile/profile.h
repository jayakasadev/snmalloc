// SPDX-License-Identifier: MIT
//
// Heap profiler -- umbrella header.
//
// Including this only makes the profiler's types visible; it does not enable
// sampling. That happens at the alloc/dealloc hooks, under SNMALLOC_PROFILE.
//
// `record.h` is deliberately not included here: it needs types from
// mem/corealloc.h and including it would create a header cycle through
// commonconfig.h. Its callers include it directly.

#pragma once

#include "node_pool.h"
#include "reentrancy_guard.h"
#include "sampled_alloc.h"
#include "sampled_list.h"
#include "sampler.h"
