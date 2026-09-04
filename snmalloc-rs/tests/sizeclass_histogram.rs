#![cfg(feature = "stats-full")]

use snmalloc_rs::SnMalloc;
use std::alloc::{GlobalAlloc, Layout};

#[global_allocator]
static ALLOC: SnMalloc = SnMalloc;

const N: usize = 100;

const ALLOC_SIZE: usize = 32;

fn dominant_class(before: &[u64], after: &[u64]) -> Option<(usize, u64)> {
    let mut best: Option<(usize, u64)> = None;
    for (i, (b, a)) in before.iter().zip(after.iter()).enumerate() {
        let delta = a.saturating_sub(*b);
        if delta == 0 {
            continue;
        }
        match best {
            None => best = Some((i, delta)),
            Some((_, d)) if delta > d => best = Some((i, delta)),
            _ => {}
        }
    }
    best
}

#[test]
fn cumulative_alloc_per_class_rises() {
    let alloc = SnMalloc::new();
    let before = SnMalloc::full_stats();

    let layout = Layout::from_size_align(ALLOC_SIZE, 16).unwrap();
    let mut ptrs = Vec::with_capacity(N);
    for _ in 0..N {
        let p = unsafe { alloc.alloc(layout) };
        assert!(!p.is_null(), "alloc must succeed");
        ptrs.push(p);
    }

    let after = SnMalloc::full_stats();

    let (sc, alloc_delta) = dominant_class(
        &before.cumulative_alloc_by_class,
        &after.cumulative_alloc_by_class,
    )
    .expect(
        "at least one cumulative_alloc_by_class slot must rise after \
         100 same-size allocations",
    );

    assert!(
        alloc_delta >= N as u64,
        "cumulative_alloc_by_class[{}] delta (={}) must rise by at \
         least N={} after {} allocations of size {}",
        sc,
        alloc_delta,
        N,
        N,
        ALLOC_SIZE,
    );

    let live_count_delta =
        after.total_live_count_by_class[sc] - before.total_live_count_by_class[sc];
    assert!(
        live_count_delta >= N as u64,
        "total_live_count_by_class[{}] delta (={}) must rise by at \
         least N={} after {} allocations (no frees yet)",
        sc,
        live_count_delta,
        N,
        N,
    );

    let live_bytes_delta =
        after.total_live_bytes_by_class[sc] - before.total_live_bytes_by_class[sc];
    assert!(
        live_bytes_delta >= (live_count_delta) * ALLOC_SIZE as u64,
        "total_live_bytes_by_class[{}] delta (={}) must be >= \
         live_count_delta ({}) * ALLOC_SIZE ({})",
        sc,
        live_bytes_delta,
        live_count_delta,
        ALLOC_SIZE,
    );

    for p in ptrs.drain(..) {
        unsafe { alloc.dealloc(p, layout) };
    }

    let post_free = SnMalloc::full_stats();

    assert!(
        post_free.cumulative_alloc_by_class[sc] >= after.cumulative_alloc_by_class[sc],
        "cumulative_alloc_by_class[{}] is monotone (after={}, \
         post_free={})",
        sc,
        after.cumulative_alloc_by_class[sc],
        post_free.cumulative_alloc_by_class[sc],
    );

    let dealloc_delta =
        post_free.cumulative_dealloc_by_class[sc] - before.cumulative_dealloc_by_class[sc];
    assert!(
        dealloc_delta >= N as u64,
        "cumulative_dealloc_by_class[{}] delta (={}) must rise by \
         at least N={} after {} frees on the same thread",
        sc,
        dealloc_delta,
        N,
        N,
    );

    assert!(
        post_free.total_live_count_by_class[sc] <= after.total_live_count_by_class[sc],
        "total_live_count_by_class[{}] must not rise after frees \
         (after={}, post_free={})",
        sc,
        after.total_live_count_by_class[sc],
        post_free.total_live_count_by_class[sc],
    );

    let live_drop = after.total_live_count_by_class[sc] - post_free.total_live_count_by_class[sc];
    assert!(
        live_drop >= N as u64,
        "total_live_count_by_class[{}] must drop by at least N={} \
         after {} same-thread frees (after={}, post_free={})",
        sc,
        N,
        N,
        after.total_live_count_by_class[sc],
        post_free.total_live_count_by_class[sc],
    );
}

#[test]
fn cumulative_monotone_invariant_holds() {
    let alloc = SnMalloc::new();
    let layout = Layout::from_size_align(48, 16).unwrap();
    let mut ptrs = Vec::with_capacity(16);
    for _ in 0..16 {
        let p = unsafe { alloc.alloc(layout) };
        assert!(!p.is_null());
        ptrs.push(p);
    }
    for p in ptrs.drain(..8) {
        unsafe { alloc.dealloc(p, layout) };
    }

    let snap = SnMalloc::full_stats();

    for i in 0..snap.cumulative_alloc_by_class.len() {
        let a = snap.cumulative_alloc_by_class[i];
        let d = snap.cumulative_dealloc_by_class[i];

        assert!(
            a >= d,
            "class {}: cumulative_alloc ({}) must be >= \
             cumulative_dealloc ({})",
            i,
            a,
            d,
        );
    }

    for p in ptrs.drain(..) {
        unsafe { alloc.dealloc(p, layout) };
    }
}
