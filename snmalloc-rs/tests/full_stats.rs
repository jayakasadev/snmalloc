#![cfg(feature = "stats-basic")]

use snmalloc_rs::{SnMalloc, SNMALLOC_FULL_STATS_VERSION};
use std::alloc::{GlobalAlloc, Layout};

#[global_allocator]
static ALLOC: SnMalloc = SnMalloc;

#[test]
fn full_stats_version_is_populated() {
    let stats = SnMalloc::full_stats();
    assert_eq!(
        stats.version, SNMALLOC_FULL_STATS_VERSION,
        "version must match SNMALLOC_FULL_STATS_VERSION"
    );
}

#[test]
fn full_stats_bytes_in_use_grows_with_live_allocation() {
    let alloc = SnMalloc::new();
    let before = SnMalloc::full_stats();

    let layout = Layout::from_size_align(1 << 20, 64).unwrap();
    let ptr = unsafe { alloc.alloc(layout) };
    assert!(!ptr.is_null(), "1 MiB allocation must not return null");

    let during = SnMalloc::full_stats();

    assert!(
        during.bytes_in_use > 0,
        "bytes_in_use must be non-zero with a 1 MiB live allocation, \
         got {}",
        during.bytes_in_use
    );
    assert!(
        during.bytes_in_use >= before.bytes_in_use,
        "bytes_in_use must not regress after a fresh allocation \
         (before = {}, during = {})",
        before.bytes_in_use,
        during.bytes_in_use
    );
    assert!(
        during.peak_bytes_in_use >= during.bytes_in_use,
        "peak_bytes_in_use ({}) must be >= bytes_in_use ({})",
        during.peak_bytes_in_use,
        during.bytes_in_use
    );

    unsafe { alloc.dealloc(ptr, layout) };
}

#[test]
fn full_stats_backend_frag_invariants() {
    let alloc = SnMalloc::new();

    let layout = Layout::from_size_align(1 << 20, 64).unwrap();
    let p1 = unsafe { alloc.alloc(layout) };
    let p2 = unsafe { alloc.alloc(layout) };
    assert!(!p1.is_null() && !p2.is_null());

    let snap = SnMalloc::full_stats();

    assert!(
        snap.bytes_committed > 0,
        "bytes_committed must be > 0 after live allocations; got {}",
        snap.bytes_committed
    );

    assert!(
        snap.bytes_committed <= snap.bytes_mapped,
        "bytes_committed ({}) must be <= bytes_mapped ({})",
        snap.bytes_committed,
        snap.bytes_mapped
    );

    unsafe { alloc.dealloc(p1, layout) };
    unsafe { alloc.dealloc(p2, layout) };

    let after = SnMalloc::full_stats();
    assert!(
        after.bytes_decommitted_to_os >= snap.bytes_decommitted_to_os,
        "bytes_decommitted_to_os must be monotone non-decreasing \
         (snap = {}, after = {})",
        snap.bytes_decommitted_to_os,
        after.bytes_decommitted_to_os
    );
    assert_eq!(after.version, SNMALLOC_FULL_STATS_VERSION);
}

#[test]
fn full_stats_freechunk_histogram_populates() {
    let alloc = SnMalloc::new();

    let layout = Layout::from_size_align(1 << 20, 64).unwrap();
    const N: usize = 10;
    let mut ptrs: [*mut u8; N] = [core::ptr::null_mut(); N];
    for slot in ptrs.iter_mut() {
        let p = unsafe { alloc.alloc(layout) };
        assert!(!p.is_null(), "1 MiB allocation must not return null");
        *slot = p;
    }
    for slot in ptrs.iter().copied() {
        unsafe { alloc.dealloc(slot, layout) };
    }

    let snap = SnMalloc::full_stats();
    assert_eq!(snap.version, SNMALLOC_FULL_STATS_VERSION);

    let hist = snap.free_chunk_histogram();
    assert_eq!(
        hist.len(),
        snmalloc_rs::SNMALLOC_FULL_STATS_FREECHUNK_BUCKETS,
        "free_chunk_histogram length must match the FFI bucket count"
    );

    let nonzero = hist.iter().filter(|&&c| c != 0).count();
    assert!(
        nonzero > 0,
        "expected at least one non-zero free-chunk bucket after \
         {} x 1 MiB alloc+free; got histogram {:?}",
        N,
        hist
    );

    for i in 0..snmalloc_rs::SNMALLOC_FULL_STATS_FREECHUNK_BUCKETS {
        assert_eq!(
            hist[i], snap.reserved[i],
            "free_chunk_histogram[{}] ({}) must equal reserved[{}] ({})",
            i, hist[i], i, snap.reserved[i]
        );
    }
}

#[test]
fn full_stats_peak_is_monotone_after_dealloc() {
    let alloc = SnMalloc::new();
    let before = SnMalloc::full_stats();

    let layout = Layout::from_size_align(1 << 20, 64).unwrap();
    let ptr = unsafe { alloc.alloc(layout) };
    assert!(!ptr.is_null());
    unsafe { alloc.dealloc(ptr, layout) };

    let after = SnMalloc::full_stats();
    assert!(
        after.peak_bytes_in_use >= before.peak_bytes_in_use,
        "peak_bytes_in_use must be monotone non-decreasing across a \
         dealloc (before = {}, after = {})",
        before.peak_bytes_in_use,
        after.peak_bytes_in_use
    );
}
