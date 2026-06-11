// SPDX-License-Identifier: MIT
//
// Bindgen entry point for the snmalloc Rust shim FFI surface.
//
// A single bindgen invocation against this header produces the
// extern decls for both the core C ABI surface (`sn_rust_alloc`,
// `sn_rust_dealloc`, `sn_rust_realloc`, `sn_rust_statistics`,
// `sn_rust_usable_size`, `sn_rust_alloc_zeroed`) and the heap-
// profiling surface (`sn_rust_profile_*` plus the
// `SnRustProfileRawSample` struct).
//
// Consumers use allowlist flags to scope which symbols land in the
// resulting Rust module.  The split between core and profile is
// expressed at the bindgen-flag level, not at the header level: both
// the Cargo `build.rs` and the Bazel `rust_bindgen_library` rules
// point at this same header and pass different `--allowlist-*`
// arguments depending on whether the consumer wants the profiling
// surface or not.

#pragma once

#include "snmalloc/override/rust.h"
#include "snmalloc/override/rust_profile.h"
