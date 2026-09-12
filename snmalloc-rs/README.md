# snmalloc-rs

[![snmalloc-rs CI](https://github.com/microsoft/snmalloc/actions/workflows/rust.yml/badge.svg)](https://github.com/microsoft/snmalloc/actions/workflows/rust.yml)

`snmalloc-rs` provides a wrapper for [`microsoft/snmalloc`](https://github.com/microsoft/snmalloc) to make it usable as
a global allocator for rust. snmalloc is a research allocator. Its key design features are:

- Memory that is freed by the same thread that allocated it does not require any synchronising operations.
- Freeing memory in a different thread to initially allocated it, does not take any locks and instead uses a novel
  message passing scheme to return the memory to the original allocator, where it is recycled.
- The allocator uses large ranges of pages to reduce the amount of meta-data required.

Some old benchmark results are available in
the [`snmalloc` paper](https://github.com/microsoft/snmalloc/blob/master/snmalloc.pdf). 

There are the following features defined in this crate:

- `debug`: Enable the `Debug` mode in `snmalloc`. This is also automatically enabled if Cargo's `DEBUG` environment variable is set to `true`.
- `native-cpu`: Optimize `snmalloc` for the native CPU of the host machine. (this is not a default behavior
  since `0.2.14`)
- `qemu`: Workaround `madvise` problem of QEMU environment
- `local_dynamic_tls`: Workaround cannot allocate memory in static tls block
- `build_cc`: Use of cc crate instead of cmake (cmake still default) as builder (more platform agnostic)
- `usecxx17`: Use C++17 standard
- `check`: Enable extra checks to improve security, see upstream [security docs](https://github.com/microsoft/snmalloc/tree/main/docs/security).
  Note that the `memcpy` protection is not enabled in Rust.
- `win8compat`: Improve compatibility for old Windows platforms (removing usages of `VirtualAlloc2` and other new APIs)
- `lto`: Links with InterProceduralOptimization/LinkTimeOptimization
- `notls`: Enables to be loaded dynamically, thus disable tls.
- `stats`: Enables allocation statistics.
- `libc-api`: Enables libc API backed by snmalloc.
- `usewait-on-address`: Enable `WaitOnAddress` support on Windows (enabled by default).
- `tracing`: Enable structured tracing/logging.
- `fuzzing`: Enable fuzzing support.
- `vendored-stl`: Use self-vendored STL.
- `check-loads`: Enable check loads feature.
- `pageid`: Enable page ID feature.
- `gwp-asan`: Enable GWP-ASan integration. Requires `SNMALLOC_GWP_ASAN_INCLUDE_PATH` and `SNMALLOC_GWP_ASAN_LIBRARY_PATH`.
- `profiling`: Enable the statistical heap profiler. Activates the C-side `SNMALLOC_PROFILE=ON` build and exposes the `HeapProfile` / `ProfilingSession` APIs documented below.
- `symbolicate`: Resolve raw frame addresses captured by the profiler into function/file/line via the [`backtrace`](https://crates.io/crates/backtrace) crate. Compose with `profiling`.
- `criterion-integration`: Expose `snmalloc_rs::criterion::bench_with_profile` / `bench_with_profile_batched`, which run a `criterion` bench under a single `ProfilingSession` and write a folded-stack flamegraph afterwards. Compose with `profiling`. See [Bench profiling](#bench-profiling) below.

## Heap Profiling

Using Bazel? See [`docs/bazel.md`](docs/bazel.md).

The `profiling` Cargo feature turns on a statistical heap profiler in
the underlying snmalloc build. Each allocation may be recorded with its
call stack; summing the per-sample weights gives an unbiased estimate of
total bytes allocated. The default sampling interval is 524288 bytes
(512 KiB). To measure the profiler's cost on your machine, run
`benches/profile_bench.rs`.

Enable in `Cargo.toml`:

```toml
[dependencies]
snmalloc-rs = { version = "0.7.4", features = ["profiling"] }
# Optional: resolve raw frame addresses to function/file/line.
# snmalloc-rs = { version = "0.7.4", features = ["profiling", "symbolicate"] }
```

### Quick start: snapshot + flamegraph

`SnMalloc::snapshot()` returns a [`HeapProfile`] holding every sampled
allocation that is currently live. Write it in Brendan Gregg's
folded-stack format, which
[`inferno-flamegraph`](https://github.com/jonhoo/inferno) and
[Speedscope](https://www.speedscope.app/) can read:

```rust
use snmalloc_rs::SnMalloc;
use std::fs::File;

#[global_allocator]
static ALLOC: SnMalloc = SnMalloc;

fn main() -> std::io::Result<()> {
    // 256 KiB mean sampling interval. Set to 0 to disable.
    ALLOC.set_sampling_rate(256 * 1024);

    // ... run your workload ...

    let profile = ALLOC.snapshot();
    let mut out = File::create("heap.folded")?;
    profile.write_flamegraph(&mut out)?;
    Ok(())
}
```

Then render to SVG:

```sh
inferno-flamegraph < heap.folded > heap.svg
```

### When to use snapshot vs streaming

The two modes answer different questions:

| | `SnMalloc::snapshot()` | `ProfilingSession::start` (streaming) |
| - | - | - |
| **What it captures** | Sampled allocations live at the moment you call it. | Sampled allocation and in-place resize events. |
| **Best for** | "What is holding memory right now?" — leak triage, before/after comparisons. | "Which call site allocates fastest?" — hot-path work, churn analysis. |
| **Bias** | Misses short-lived allocations, which are freed before the snapshot. | None, but you pay to store and aggregate every event. |
| **Output** | A `HeapProfile`; write it with `write_pprof` / `write_flamegraph`. | A callback; your application decides how to store events (often a JSON-Lines file). |
| **Tooling** | `snmalloc-tools profile-top` | `snmalloc-tools rate-report` |

**Rule of thumb.** Ask "where is my live heap?" → snapshot. Ask "which
call site is hottest?" → streaming.

`rate-report` reads the log as a stream, so multi-million-event traces
are fine. See [`snmalloc-tools/README.md`](../snmalloc-tools/README.md)
for the log format.

### Streaming mode

For long-running services, `ProfilingSession::start` registers a closure
that receives a [`StreamSample`] for every sampled allocation as it
happens, so you never have to poll `snapshot()`. Dropping the session
unregisters the callback and cleans up.

```rust
use snmalloc_rs::{ProfilingSession, SnMalloc};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::Arc;

let bytes_seen = Arc::new(AtomicU64::new(0));
let counter = Arc::clone(&bytes_seen);

let _session = ProfilingSession::start(move |sample| {
    counter.fetch_add(sample.weight(), Ordering::Relaxed);
})
.expect("no other session active");

// ... run workload ...
// Session is unregistered automatically when `_session` is dropped.
```

The closure must be `Fn + Send + Sync + 'static`; samples can arrive on
any thread. Only one session can be active per process.

#### Realloc / Resize events

Each `StreamSample` has an `EventKind`:

- `EventKind::Alloc` — a newly sampled allocation.
- `EventKind::Resize` — an in-place `realloc` changed the size of an
  already-sampled allocation. It carries the new `requested_size` /
  `allocated_size` and keeps the original stack and weight.

An out-of-place `realloc` may produce a normal allocation event for the
replacement. Deallocation events are not streamed.

```rust
use snmalloc_rs::streaming::EventKind;

let _session = ProfilingSession::start(|sample| {
    match sample.kind() {
        EventKind::Alloc => { /* a fresh sampled allocation */ }
        EventKind::Resize => { /* an in-place realloc grew/shrank it */ }
    }
});
```

### Runtime configuration via env vars

`SnMalloc::init_profiling_from_env()` reads `SNMALLOC_PROFILE_ENABLE`
and `SNMALLOC_PROFILE_RATE` and applies the sampling rate they describe.
Use it to ship a binary that can be switched into profiling mode without
a rebuild:

```rust
use snmalloc_rs::SnMalloc;

#[global_allocator]
static ALLOC: SnMalloc = SnMalloc;

fn main() {
    // Honour SNMALLOC_PROFILE_ENABLE=1 / SNMALLOC_PROFILE_RATE=<bytes>.
    let _ = ALLOC.init_profiling_from_env();

    // ... your app ...
}
```

Resolution order:

1. If `SNMALLOC_PROFILE_RATE` is a parseable non-negative integer, it
   wins (including `0`, which explicitly disables).
2. Otherwise, a truthy `SNMALLOC_PROFILE_ENABLE` (`1` / `true` / `yes`,
   case-insensitive) enables sampling at the default 512 KiB rate.
3. Otherwise the call is a no-op — the sampling rate is unchanged.

Operators can then control profiling without rebuilding:

```sh
SNMALLOC_PROFILE_ENABLE=1 ./my-app                 # default 512 KiB
SNMALLOC_PROFILE_RATE=65536 ./my-app               # 64 KiB high-res
SNMALLOC_PROFILE_RATE=0 ./my-app                   # explicitly off
```

To set the same options in code instead, use `ProfileConfig` with
`SnMalloc::configure_profiling`.

### Typed configuration

```rust
use snmalloc_rs::{ProfileConfig, SnMalloc};

let cfg = ProfileConfig::with_sampling_rate(128 * 1024);
SnMalloc.configure_profiling(cfg);
```

### Google pprof output

`HeapProfile::write_pprof` writes the snapshot in Google's
[`pprof`](https://github.com/google/pprof) protobuf format, which
`go tool pprof`, Pyroscope, Polar Signals, and Parca can read:

```rust
use snmalloc_rs::{SnMalloc, Weight};
use std::fs::File;

let profile = SnMalloc.snapshot();
let mut out = File::create("heap.pb")?;
profile.write_pprof(&mut out, Weight::Allocated)?;
# Ok::<(), std::io::Error>(())
```

Then inspect with the standard pprof tooling:

```sh
go tool pprof -http=:8080 heap.pb
```

Two live-heap axes are written: `inuse_objects` and `inuse_space`.
Cumulative `alloc_*` axes are omitted because a snapshot contains currently
live allocations, not allocation history. `go tool pprof` defaults to
`inuse_space`, which estimates the bytes the allocator handed back,
including sizeclass slack. The profile period records the sampling interval
at snapshot time.

The `Weight` argument remains for API compatibility; pprof HEAP output
always uses the allocated-space projection. Use the `Weight` choices with
flamegraph output and aggregate helpers.

### Symbolicated output

Add the `symbolicate` feature and the profiler resolves frame addresses
to function names, source files, and line numbers using the `backtrace`
crate. `write_flamegraph` then emits resolved frames with no API change:

```rust
# #[cfg(feature = "symbolicate")] {
use snmalloc_rs::SnMalloc;
use std::fs::File;

let profile = SnMalloc.snapshot();
let mut out = File::create("heap.folded")?;
profile.write_flamegraph(&mut out)?;
# }
# Ok::<(), std::io::Error>(())
```

Frames that cannot be resolved fall back to `0x` plus 16 hex digits, the
same rendering used without the feature.

Call `write_flamegraph_raw` if you always want raw addresses — for
example to symbolicate them with an external tool. Both methods are
always available.

### Bench profiling

The `criterion-integration` feature (compose with `profiling`) adds two
helpers in `snmalloc_rs::criterion` that wrap a
[`criterion`](https://docs.rs/criterion) bench function in one
[`ProfilingSession`]. The session opens once per bench function, and the
samples are written as a folded-stack flamegraph after `bencher.iter`
returns, so the profiled window matches the measured window exactly.

Enable in `Cargo.toml`:

```toml
[dev-dependencies]
snmalloc-rs = { version = "0.8", features = ["profiling", "criterion-integration"] }
criterion = { version = "0.5", default-features = false }
```

`bench_with_profile` covers `criterion::Bencher::iter`:

```rust,no_run
use criterion::{black_box, Bencher, Criterion};
use snmalloc_rs::{criterion::bench_with_profile, SnMalloc};
use std::path::Path;

fn bench_my_workload(c: &mut Criterion) {
    SnMalloc.set_sampling_rate(65_536); // 64 KiB for higher-res bench profiles
    c.bench_function("my_workload", |b: &mut Bencher| {
        bench_with_profile(b, Path::new("target/criterion/my_workload.folded"), || {
            let v: Vec<u64> = (0..1024).collect();
            black_box(v);
        });
    });
}
```

`bench_with_profile_batched` covers `Bencher::iter_batched`. Pass
`setup`, `routine`, and `BatchSize` through unchanged:

```rust,no_run
use criterion::{black_box, BatchSize, Bencher, Criterion};
use snmalloc_rs::{criterion::bench_with_profile_batched, SnMalloc};
use std::path::Path;

fn bench_with_setup(c: &mut Criterion) {
    SnMalloc.set_sampling_rate(65_536);
    c.bench_function("with_setup", |b: &mut Bencher| {
        bench_with_profile_batched(
            b,
            Path::new("target/criterion/with_setup.folded"),
            || (0..1024u64).rev().collect::<Vec<u64>>(), // setup, not measured
            |mut v| { v.sort(); black_box(v); },         // routine, measured + profiled
            BatchSize::SmallInput,
        );
    });
}
```

A runnable example lives at `benches/criterion_profile_example.rs`.

#### Tuning tips

- **Session setup and teardown cost is fixed per session.** It does not
  amortise well across very short bench bodies. If your body finishes in
  tens of nanoseconds, wrap a body that loops instead of a single
  iteration.
- **Sampling rate.** All samples go through one dispatch path, which a
  high sampling rate plus a heavily-allocating body can saturate. Tune
  [`SnMalloc::set_sampling_rate`](https://docs.rs/snmalloc-rs) —
  `65_536` for a one-off high-resolution profile, `524_288` for a
  production-shaped rate — and consider
  [`SnMalloc::set_max_local_cache`](https://docs.rs/snmalloc-rs), which
  caps the per-thread cache and so bounds the sample rate.

### Feature-off behaviour

With the `profiling` feature **off**, every API above still compiles and
runs:

- `SnMalloc::profiling_supported()` returns `false`.
- `SnMalloc::set_sampling_rate(...)` does nothing; `sampling_rate()`
  returns `0`.
- `SnMalloc::snapshot()` returns an empty `HeapProfile`.
- `write_flamegraph` / `write_pprof` succeed and write valid empty
  output.

So you can call the profiling API unconditionally and switch it on or
off with the Cargo feature alone.

## Build Configuration

The build script ensures architectural alignment between the Rust profile and the underlying `snmalloc` allocator:

### Environment Variables
The following environment variables are automatically detected and propagated:
- `DEBUG`: Synchronizes the `snmalloc` build type with the Cargo profile. If `true`, `snmalloc` is built in `Debug` mode.
- `OPT_LEVEL`: Propagated to the C++ compiler to ensure optimization parity between Rust and C++ components.

### Windows CRT Consistency
On Windows, the build script enforces static CRT linking (`/MT` or `/MTd`) across both `cc` and `cmake` builders. This prevents linker errors and ensures consistency when `snmalloc` is used as a global allocator.

**To get the crates compiled, you need to choose either `1mib` or `16mib` to determine the chunk configuration**

To use `snmalloc-rs` add it as a dependency:

```toml
# Cargo.toml
[dependencies]
snmalloc-rs = "0.7.5"
```

To set `SnMalloc` as the global allocator add this to your project:

```rust
#[global_allocator]
static ALLOC: snmalloc_rs::SnMalloc = snmalloc_rs::SnMalloc;
```

## For Android Cross-Compilation

- `ANDROID_NDK` must be provided as an environment variable
- `ANDROID_PLATFORM` can be passed as an optional environment variable
- `ANDROID_ABI` used by CMake is detected automatically
- feature `android-lld` can be used to set the linker of `snmalloc` to `lld`
- ~~feature `android-shared-std` can be used to set the STL library of `snmalloc` to `c++_shared` (it uses `c++_static` by
  default)~~ (`libstdc++` is no longer a dependency)

## Changelog

### 0.7.5

- Tracking upstream to match version 0.7.5.

### 0.7.4

- Tracking upstream to match version 0.7.4.
- SnMalloc has been moved to upstream repository. Future releases will track upstream release directly.

### 0.3.8

- Tracking upstream to match version 0.7.1
- Recommended to upgrade from 0.3.7 to get an important bug fix.

### 0.3.7

- Tracking upstream to match version 0.7

### 0.3.4
- Tracking upstream to version 0.6.2.

### 0.3.3
- Tracking upstream to fix Linux PAL typo.

### 0.3.2

- Tracking upstream to enable old Linux variants.

### 0.3.1 

- Fixes `build_cc` feature (broken in 0.3.0 release).
- Fixes `native-cpu` feature (broken in 0.3.0 release).

### 0.3.0

- Release to support snmalloc 0.6.0.

### 0.3.0-beta.1

- Beta release to support snmalloc ~~2~~ 0.6.0
