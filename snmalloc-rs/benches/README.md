# `snmalloc-rs` benchmarks

Criterion benchmarks that measure the per-allocation latency added by
heap profiling (`SNMALLOC_PROFILE` on the C++ side, the `profiling`
Cargo feature on the Rust side).

## Running

```bash
# Baseline -- profile-off (single variant per group).
cargo bench --bench profile_bench

# Profiling-on -- three variants per group:
#   profile-off          (always-off branch, control)
#   profile-on-inactive  (countdown active, sample rate = usize::MAX)
#   profile-on-active    (countdown active, sample rate = 512 KiB default)
cargo bench --bench profile_bench --features profiling
```

A full sweep takes a few minutes. Criterion writes HTML pages and JSON
estimates under `target/criterion/`, and the bench prints a short
summary to stderr pointing at the key files.

## What to look at

The main number is **`ratio_idle`**, computed per benchmark group:

```
ratio_idle = mean(profile-on-inactive) / mean(profile-off)
```

This is what you pay for compiling profiling support in and never
turning it on. The bench targets `ratio_idle <= 1.05`, so anything above
that in any group is worth investigating.

`profile-on-active` measures the cost of actually taking the sampling
path, so it is larger. At the default 512 KiB rate the sampler fires
about once per 16K small allocations, and capturing the stack dominates
that column. Compare it against your previous run, not against
`profile-off`.

## Absolute numbers

Nanoseconds per allocation depend on the host, the C++ build flags
(`debug` vs release, `check`, and so on), and the OS. Use this suite for
**relative** comparisons: variant against variant in one run, or run
against run on the same machine. Compare ratios, not raw numbers, across
machines.
