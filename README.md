# snmalloc

snmalloc is a high-performance allocator. 
snmalloc can be used directly in a project as a header-only C++ library, 
it can be `LD_PRELOAD`ed on Elf platforms (e.g. Linux, BSD),
and there is a [crate](https://crates.io/crates/snmalloc-rs) to use it from Rust.

Its key design features are:

* Memory that is freed by the same thread that allocated it does not require any
  synchronising operations.
* Freeing memory in a different thread to initially allocated it, does not take
  any locks and instead uses a novel message passing scheme to return the
  memory to the original allocator, where it is recycled.  This enables 1000s of remote 
  deallocations to be performed with only a single atomic operation enabling great
  scaling with core count. 
* The allocator uses large ranges of pages to reduce the amount of meta-data
  required.
* The fast paths are highly optimised with just two branches on the fast path 
  for malloc (On Linux compiled with Clang).
* The platform dependencies are abstracted away to enable porting to other platforms. 

snmalloc's design is particular well suited to the following two difficult 
scenarios that can be problematic for other allocators:

  * Allocations on one thread are freed by a different thread
  * Deallocations occur in large batches

Both of these can cause massive reductions in performance of other allocators, but 
do not for snmalloc.

The implementation of snmalloc has evolved significantly since the [initial paper](snmalloc.pdf).
The mechanism for returning memory to remote threads has remained, but most of the meta-data layout has changed.
We recommend you read [docs/security](./docs/security/README.md) to find out about the current design, and 
if you want to dive into the code [docs/AddressSpace.md](./docs/AddressSpace.md) provides a good overview of the allocation and deallocation paths.

[![snmalloc CI](https://github.com/microsoft/snmalloc/actions/workflows/main.yml/badge.svg)](https://github.com/microsoft/snmalloc/actions/workflows/main.yml)

# Hardening

There is a hardened version of snmalloc, it contains

*  Randomisation of the allocations' relative locations,
*  Most meta-data is stored separately from allocations, and is protected with guard pages,
*  All in-band meta-data is protected with a novel encoding that can detect corruption, and
*  Provides a `memcpy` that automatically checks the bounds relative to the underlying malloc.

A more comprehensive write up is in [docs/security](./docs/security/README.md).

# Further documentation

 - [Instructions for building snmalloc](docs/BUILDING.md)
 - [Instructions for porting snmalloc](docs/PORTING.md)

## Heap Profiling

snmalloc has an opt-in **statistical heap profiler**. When enabled at
build time, it records a random sample of allocations together with
their call stacks, for offline analysis with the usual flamegraph and
pprof tools.

### Enabling at build time

The profiler is off by default. Turn it on with one CMake option:

```sh
cmake -B build -DSNMALLOC_PROFILE=ON
cmake --build build
```

With `SNMALLOC_PROFILE=OFF` every profiling code path is compiled out.

### What it samples

Each allocation may be recorded, with a probability set by the *mean
sampling interval* in bytes. The default is 524288 bytes (512 KiB), so
roughly one allocation per 512 KiB requested is recorded. Sample weights
are unbiased Poisson estimators: summing `weight` across a snapshot
estimates total bytes requested. Scale by
`allocated_size / requested_size` to estimate bytes the allocator
actually handed back.

You can change the interval at runtime. A smaller interval (say 64 KiB)
gives more detail and costs more; a larger one (say 1 MiB) gives less
detail and costs less. Measure your own workload to pick a value.

### C ABI for embedding

The C++ build exposes a few `extern "C"` symbols so you can drive the
profiler from a non-Rust host:

| Symbol | Purpose |
| ------ | ------- |
| `sn_rust_profile_supported` | Returns `true` iff built with `SNMALLOC_PROFILE=ON`. |
| `sn_rust_profile_set_sampling_rate` | Set the mean sampling interval in bytes. `0` disables. |
| `sn_rust_profile_get_sampling_rate` | Read the current sampling interval. |
| `sn_rust_profile_snapshot_begin` / `_count` / `_get` / `_end` | RAII-style enumeration of currently-live sampled allocations. |
| `sn_rust_profile_streaming_start` / `_stop` | Register a `void(*)(const SnRustProfileRawSample*)` callback that receives every sample as it occurs. |

Every `SnRustProfileRawSample` has a `kind` byte:

- `SN_RUST_PROFILE_KIND_ALLOC` — a newly sampled allocation.
- `SN_RUST_PROFILE_KIND_RESIZE` — an in-place `realloc` changed the size
  of an already-sampled allocation. The event carries the new
  `requested_size` / `allocated_size` and keeps the original stack and
  weight.

An out-of-place `realloc` may emit a normal allocation event for the
replacement; deallocation events are not streamed. Snapshots always report
`kind == ALLOC`.

The Rust crate calls these same exports. See
`src/snmalloc/override/rust.cc` and `src/snmalloc/override/rust.h`.

### Rust crate

The [`snmalloc-rs`](snmalloc-rs/README.md) crate wraps the C ABI safely:
a snapshot type ([`HeapProfile`](snmalloc-rs/src/profile.rs)), a
streaming session ([`ProfilingSession`](snmalloc-rs/src/streaming.rs)),
and an environment-variable initializer
([`SnMalloc::init_profiling_from_env`](snmalloc-rs/src/config.rs)) for
turning profiling on without recompiling. See
[snmalloc-rs/README.md](snmalloc-rs/README.md#heap-profiling) for the
API and examples.

### Output formats

The Rust crate writes two formats:

- **Folded (collapsed) stacks** — one line per unique stack with summed
  weights. Read by Brendan Gregg's
  [`flamegraph.pl`](https://github.com/brendangregg/FlameGraph),
  [`inferno-flamegraph`](https://github.com/jonhoo/inferno), and
  [Speedscope](https://www.speedscope.app/).
- **Google `pprof` protobuf** — read by `go tool pprof`,
  [Pyroscope](https://pyroscope.io/), [Polar Signals
  Cloud](https://www.polarsignals.com/), and
  [Parca](https://www.parca.dev/). Emitted with live
  `inuse_objects` / `inuse_space` axes.

### Measuring overhead

To measure the profiler's cost on your machine, run the criterion suite
in
[`snmalloc-rs/benches/profile_bench.rs`](snmalloc-rs/benches/profile_bench.rs).
It compares three configurations: `profile-off`, `profile-on-inactive`,
and `profile-on-active`.

### Further reading

- [PMU profiling](docs/profiling-pmu.md) — attributing cache misses,
  false sharing, and branch-hint misses using `perf` on Linux and
  Instruments on macOS.

# Contributing

This project welcomes contributions and suggestions.  Most contributions require you to agree to a
Contributor License Agreement (CLA) declaring that you have the right to, and actually do, grant us
the rights to use your contribution. For details, visit https://cla.microsoft.com.

When you submit a pull request, a CLA-bot will automatically determine whether you need to provide
a CLA and decorate the PR appropriately (e.g., label, comment). Simply follow the instructions
provided by the bot. You will only need to do this once across all repos using our CLA.

This project has adopted the [Microsoft Open Source Code of Conduct](https://opensource.microsoft.com/codeofconduct/).
For more information see the [Code of Conduct FAQ](https://opensource.microsoft.com/codeofconduct/faq/) or
contact [opencode@microsoft.com](mailto:opencode@microsoft.com) with any additional questions or comments.
