//! ```text
//! # Baseline, profile-off
//! cargo bench --bench profile_bench
//!
//! # profile-on-inactive and profile-on-active (selected at runtime)
//! cargo bench --bench profile_bench --features profiling
//! ```

use std::alloc::{alloc, dealloc, Layout};
use std::time::Duration;

use criterion::{black_box, criterion_group, BenchmarkId, Criterion, Throughput};

use snmalloc_rs::SnMalloc;

const BATCH: usize = 64;

fn mixed_sizes() -> Vec<usize> {
    let mut state: u64 = 0x9E37_79B9_7F4A_7C15;
    (0..BATCH)
        .map(|_| {
            state = state
                .wrapping_mul(6364136223846793005)
                .wrapping_add(1442695040888963407);
            16 + ((state >> 33) as usize % (16384 - 16))
        })
        .collect()
}

#[derive(Copy, Clone, Debug, Eq, PartialEq)]
enum Variant {
    ProfileOff,
    ProfileOnInactive,
    ProfileOnActive,
}

impl Variant {
    fn label(self) -> &'static str {
        match self {
            Variant::ProfileOff => "profile-off",
            Variant::ProfileOnInactive => "profile-on-inactive",
            Variant::ProfileOnActive => "profile-on-active",
        }
    }
}

fn apply_variant(v: Variant) {
    let a = SnMalloc::new();
    match v {
        Variant::ProfileOff => {
            a.set_sampling_rate(0);
        }
        Variant::ProfileOnInactive => {
            a.set_sampling_rate(usize::MAX);
        }
        Variant::ProfileOnActive => {
            a.set_sampling_rate(524_288);
        }
    }
}

fn variants() -> &'static [Variant] {
    if cfg!(feature = "profiling") {
        &[
            Variant::ProfileOff,
            Variant::ProfileOnInactive,
            Variant::ProfileOnActive,
        ]
    } else {
        &[Variant::ProfileOff]
    }
}

#[inline(always)]
fn alloc_batch(size: usize) {
    let layout = Layout::from_size_align(size, 8).expect("valid layout");
    let mut ptrs: [*mut u8; BATCH] = [core::ptr::null_mut(); BATCH];
    for p in ptrs.iter_mut() {
        // SAFETY: `layout` has size > 0; `alloc` is the documented
        // global-allocator entry point.
        *p = unsafe { alloc(layout) };
        black_box(*p);
    }
    for p in ptrs.iter() {
        // SAFETY: each pointer was produced by `alloc(layout)` above.
        unsafe { dealloc(*p, layout) };
    }
}

#[inline(always)]
fn alloc_batch_mixed(sizes: &[usize]) {
    let mut ptrs: [*mut u8; BATCH] = [core::ptr::null_mut(); BATCH];
    let mut layouts: [Layout; BATCH] =
        [Layout::from_size_align(8, 8).expect("valid layout"); BATCH];
    for i in 0..BATCH {
        layouts[i] = Layout::from_size_align(sizes[i], 8).expect("valid layout");
        // SAFETY: size > 0 by construction in `mixed_sizes`.
        ptrs[i] = unsafe { alloc(layouts[i]) };
        black_box(ptrs[i]);
    }
    for i in 0..BATCH {
        // SAFETY: pointer paired with its allocating layout.
        unsafe { dealloc(ptrs[i], layouts[i]) };
    }
}

fn bench_small(c: &mut Criterion) {
    let mut group = c.benchmark_group("small_allocs");
    group.throughput(Throughput::Elements(BATCH as u64));
    for &v in variants() {
        apply_variant(v);
        group.bench_with_input(BenchmarkId::from_parameter(v.label()), &v, |b, _| {
            b.iter(|| alloc_batch(32));
        });
    }
    group.finish();
}

fn bench_medium(c: &mut Criterion) {
    let mut group = c.benchmark_group("medium_allocs");
    group.throughput(Throughput::Elements(BATCH as u64));
    group.sample_size(200);
    group.measurement_time(Duration::from_secs(20));
    for &v in variants() {
        apply_variant(v);
        group.bench_with_input(BenchmarkId::from_parameter(v.label()), &v, |b, _| {
            b.iter(|| alloc_batch(4096));
        });
    }
    group.finish();
}

fn bench_mixed(c: &mut Criterion) {
    let mut group = c.benchmark_group("mixed");
    group.throughput(Throughput::Elements(BATCH as u64));
    group.sample_size(200);
    group.measurement_time(Duration::from_secs(20));
    let sizes = mixed_sizes();
    for &v in variants() {
        apply_variant(v);
        group.bench_with_input(BenchmarkId::from_parameter(v.label()), &v, |b, _| {
            b.iter(|| alloc_batch_mixed(&sizes));
        });
    }
    group.finish();
}

fn print_report() {
    eprintln!();
    eprintln!("==== profile_bench summary ====");
    eprintln!("Detailed numbers (mean ns / element, with confidence intervals)");
    eprintln!("are in target/criterion/*/new/estimates.json.");
    eprintln!("Key ratio to inspect:");
    eprintln!("  ratio_idle = mean(profile-on-inactive) / mean(profile-off)");
    eprintln!("              (per group: small_allocs, medium_allocs, mixed)");
    eprintln!("Target: ratio_idle <= 1.05 (i.e. <=5% idle overhead).");
    eprintln!("===============================");
}

fn configure() -> Criterion {
    Criterion::default()
        .warm_up_time(Duration::from_secs(3))
        .measurement_time(Duration::from_secs(5))
        .sample_size(50)
}

criterion_group! {
    name = profile_benches;
    config = configure();
    targets = bench_small, bench_medium, bench_mixed
}

fn main() {
    profile_benches();
    Criterion::default().configure_from_args().final_summary();
    print_report();
}
