use snmalloc_rs::{HeapProfile, SnMalloc};
use std::alloc::{GlobalAlloc, Layout};
use std::thread;
use std::time::Duration;

#[global_allocator]
static ALLOC: SnMalloc = SnMalloc;

const N_BUCKETS: usize = snmalloc_sys::SN_RUST_PROFILE_LIFETIME_BUCKETS;

#[test]
fn lifetime_histogram_api_smoke() {
    let buckets = HeapProfile::lifetime_histogram();
    assert_eq!(buckets.len(), N_BUCKETS, "fixed-size histogram length");

    let a = SnMalloc::new();
    if !a.profiling_supported() {
        assert!(
            buckets.iter().all(|&b| b == 0),
            "feature-off build must report an all-zero histogram"
        );
    }
}

fn bucket_for(ns: u64) -> usize {
    if ns <= 1 {
        return 0;
    }
    let b = 63 - (ns.leading_zeros() as usize);
    if b >= N_BUCKETS {
        N_BUCKETS - 1
    } else {
        b
    }
}

#[test]
fn lifetime_histogram_observes_sleep_window() {
    let a = SnMalloc::new();
    if !a.profiling_supported() {
        return;
    }

    let saved_rate = a.sampling_rate();
    a.set_sampling_rate(1);

    let before = HeapProfile::lifetime_histogram();

    const N_BUFS: usize = 16;
    let layout = Layout::from_size_align(1 << 20, 64).unwrap();
    let mut ptrs: Vec<*mut u8> = Vec::with_capacity(N_BUFS);
    for _ in 0..N_BUFS {
        let ptr = unsafe { a.alloc(layout) };
        assert!(!ptr.is_null(), "1 MiB alloc must succeed");
        ptrs.push(ptr);
    }

    thread::sleep(Duration::from_millis(50));

    for ptr in ptrs {
        unsafe { a.dealloc(ptr, layout) };
    }

    let after = HeapProfile::lifetime_histogram();
    a.set_sampling_rate(saved_rate);

    let mut delta = [0u64; N_BUCKETS];
    for i in 0..N_BUCKETS {
        delta[i] = after[i].saturating_sub(before[i]);
    }
    let total: u64 = delta.iter().sum();

    assert!(
        total >= 1,
        "expected at least one lifetime bump across the 50ms window \
         from {N_BUFS} sampled allocs; got per-bucket delta {:?}",
        delta
    );

    let min_expected_bucket = bucket_for(50_000_000);
    let max_bucket_with_count = (0..N_BUCKETS)
        .rev()
        .find(|&i| delta[i] > 0)
        .expect("at least one bucket must have a non-zero delta");
    assert!(
        max_bucket_with_count >= min_expected_bucket,
        "expected a bump in bucket >= {} (>= 50 ms); highest observed = {} \
         (delta = {:?})",
        min_expected_bucket,
        max_bucket_with_count,
        delta
    );
}

#[test]
fn bucket_for_matches_log2() {
    assert_eq!(bucket_for(0), 0);
    assert_eq!(bucket_for(1), 0);
    assert_eq!(bucket_for(2), 1);
    assert_eq!(bucket_for(3), 1);
    assert_eq!(bucket_for(4), 2);
    assert_eq!(bucket_for(8), 3);
    assert_eq!(bucket_for(1024), 10);
    assert_eq!(bucket_for(u64::MAX), N_BUCKETS - 1);
    assert_eq!(bucket_for(1u64 << 31), N_BUCKETS - 1);
    assert_eq!(bucket_for(1u64 << 62), N_BUCKETS - 1);
}
