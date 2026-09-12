use snmalloc_rs::SnMalloc;
use std::alloc::{GlobalAlloc, Layout};

#[test]
fn profiling_supported_matches_feature() {
    let a = SnMalloc::new();
    let supported = a.profiling_supported();
    if cfg!(feature = "profiling") {
        assert!(
            supported,
            "feature on must imply C-side SNMALLOC_PROFILE=ON"
        );
    } else {
        assert!(
            !supported,
            "feature off must imply C-side SNMALLOC_PROFILE undefined; \
             got profiling_supported() == true"
        );
    }
}

#[test]
fn snapshot_returns_owned_profile() {
    let a = SnMalloc::new();
    let snap = a.snapshot();
    assert_eq!(snap.is_empty(), snap.len() == 0);
    let _ = snap.total_allocated_bytes();
    let _ = snap.total_requested_bytes();
    assert_eq!(snap.samples().len(), snap.len());
}

#[test]
fn feature_off_is_quiescent() {
    if cfg!(feature = "profiling") {
        return;
    }
    let a = SnMalloc::new();
    assert!(!a.profiling_supported());
    assert_eq!(a.sampling_rate(), 0);
    a.set_sampling_rate(8192);
    assert_eq!(a.sampling_rate(), 0);
    let snap = a.snapshot();
    assert!(snap.is_empty());
    assert_eq!(snap.total_allocated_bytes(), 0u128);
    assert_eq!(snap.total_requested_bytes(), 0u128);
}

#[test]
fn sampling_rate_roundtrips_when_supported() {
    let a = SnMalloc::new();
    if !a.profiling_supported() {
        return;
    }
    let saved = a.sampling_rate();
    a.set_sampling_rate(4096);
    assert_eq!(a.sampling_rate(), 4096);
    a.set_sampling_rate(1);
    assert_eq!(a.sampling_rate(), 1);
    a.set_sampling_rate(saved);
}

#[test]
fn live_sampling_run() {
    let a = SnMalloc::new();
    if !a.profiling_supported() {
        return;
    }

    const RATE: usize = 4096;
    const N: usize = 100_000;
    const SIZE: usize = 64;

    a.set_sampling_rate(RATE);

    let layout = Layout::from_size_align(SIZE, 8).unwrap();
    let mut ptrs: Vec<*mut u8> = Vec::with_capacity(N);
    for _ in 0..N {
        let p = unsafe { a.alloc(layout) };
        assert!(!p.is_null());
        ptrs.push(p);
    }

    let snap_live = a.snapshot();
    let observed = snap_live.len();
    let expected = (N * SIZE) as f64 / RATE as f64;
    let sigma = expected.sqrt();
    let low = expected - 6.0 * sigma;
    let high = expected + 6.0 * sigma;
    assert!(
        observed > 0,
        "expected at least one live sample after {N} x {SIZE}B allocs at \
         rate {RATE}; got 0 -- profile slot is probably not wired into \
         the rust shim's Config"
    );
    assert!(
        (observed as f64) >= low && (observed as f64) <= high,
        "observed {observed} samples, expected {expected:.1} +/- 6 sigma \
         ({sigma:.1}); window = [{low:.1}, {high:.1}]"
    );

    for p in ptrs {
        unsafe { a.dealloc(p, layout) };
    }

    let snap_drained = a.snapshot();
    let remaining = snap_drained.len();
    assert!(
        remaining < observed,
        "expected sample count to drop after freeing all allocations; \
         was {observed}, still {remaining}"
    );
}
