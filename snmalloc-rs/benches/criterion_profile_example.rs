//! Profiles [`bench_with_profile`] and [`bench_with_profile_batched`].
//!
//! ```text
//! cargo build -p snmalloc-rs --features profiling,criterion-integration \
//!     --benches
//! ```
//! ```text
//! cargo bench -p snmalloc-rs --features profiling,criterion-integration \
//!     --bench criterion_profile_example
//! ```
//! ```text
//! inferno-flamegraph < target/criterion/example_iter.folded \
//!     > target/criterion/example_iter.svg
//! ```

#![cfg_attr(
    not(all(feature = "profiling", feature = "criterion-integration")),
    allow(unused_imports, dead_code)
)]

#[cfg(all(feature = "profiling", feature = "criterion-integration"))]
mod inner {
    use std::path::Path;
    use std::time::Duration;

    use criterion::{black_box, BatchSize, Criterion};

    use snmalloc_rs::criterion::{bench_with_profile, bench_with_profile_batched};
    use snmalloc_rs::SnMalloc;

    pub fn example_iter(c: &mut Criterion) {
        SnMalloc.set_sampling_rate(65_536);

        c.bench_function("example_iter", |b| {
            bench_with_profile(b, Path::new("target/criterion/example_iter.folded"), || {
                let v: Vec<u64> = (0..1024).collect();
                black_box(v);
            });
        });
    }

    pub fn example_iter_batched(c: &mut Criterion) {
        SnMalloc.set_sampling_rate(65_536);

        c.bench_function("example_iter_batched", |b| {
            bench_with_profile_batched(
                b,
                Path::new("target/criterion/example_iter_batched.folded"),
                || (0..1024u64).rev().collect::<Vec<u64>>(),
                |mut v| {
                    v.sort();
                    black_box(v);
                },
                BatchSize::SmallInput,
            );
        });
    }

    pub fn configure() -> Criterion {
        Criterion::default()
            .warm_up_time(Duration::from_secs(1))
            .measurement_time(Duration::from_secs(2))
            .sample_size(20)
    }
}

#[cfg(all(feature = "profiling", feature = "criterion-integration"))]
criterion::criterion_group! {
    name = profile_helper_benches;
    config = inner::configure();
    targets = inner::example_iter, inner::example_iter_batched
}

#[cfg(all(feature = "profiling", feature = "criterion-integration"))]
criterion::criterion_main!(profile_helper_benches);

#[cfg(not(all(feature = "profiling", feature = "criterion-integration")))]
fn main() {}
