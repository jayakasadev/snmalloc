use snmalloc_rs::{BtSample, HeapProfile, HotSpotKey, SnMalloc};
use std::alloc::{GlobalAlloc, Layout};

fn make_sample(stack: Vec<usize>, weight: usize) -> BtSample {
    BtSample {
        alloc_ptr: core::ptr::null(),
        requested_size: 64,
        allocated_size: 64,
        weight,
        stack: stack.into_iter().map(|u| u as *const u8).collect(),
    }
}

#[test]
fn top_sites_n_zero_returns_empty() {
    let p = HeapProfile::from_samples(vec![make_sample(vec![0xaaaa, 0xbbbb], 4096)]);
    assert!(p.top_sites(0, HotSpotKey::LeafFrame).is_empty());
    assert!(p.top_sites(0, HotSpotKey::FullStack).is_empty());
    assert!(p.top_sites(0, HotSpotKey::CallSite).is_empty());
}

#[test]
fn top_sites_empty_profile() {
    let p = HeapProfile::default();
    assert!(p.top_sites(10, HotSpotKey::LeafFrame).is_empty());
    assert!(p.top_sites(10, HotSpotKey::FullStack).is_empty());
    assert!(p.top_sites(10, HotSpotKey::CallSite).is_empty());
}

#[test]
fn top_sites_leaf_frame_collapses_callers() {
    let p = HeapProfile::from_samples(vec![
        make_sample(vec![0xaaaa, 0xbbbb], 4096),
        make_sample(vec![0xaaaa, 0xcccc], 8192),
        make_sample(vec![0xdddd, 0xbbbb], 1024),
    ]);
    let sites = p.top_sites(10, HotSpotKey::LeafFrame);
    assert_eq!(sites.len(), 2);

    assert_eq!(sites[0].leaf_frame as usize, 0xaaaa);
    assert_eq!(sites[0].inclusive_bytes, 12288u128);
    assert_eq!(sites[0].sample_count, 2);

    assert_eq!(sites[1].leaf_frame as usize, 0xdddd);
    assert_eq!(sites[1].inclusive_bytes, 1024u128);
    assert_eq!(sites[1].sample_count, 1);
}

#[test]
fn top_sites_full_stack_keeps_callers_separate() {
    let p = HeapProfile::from_samples(vec![
        make_sample(vec![0xaaaa, 0xbbbb], 4096),
        make_sample(vec![0xaaaa, 0xcccc], 8192),
    ]);
    let sites = p.top_sites(10, HotSpotKey::FullStack);
    assert_eq!(sites.len(), 2);
    assert_eq!(sites[0].inclusive_bytes, 8192u128);
    assert_eq!(sites[1].inclusive_bytes, 4096u128);
    assert_eq!(sites[0].leaf_frame as usize, 0xaaaa);
    assert_eq!(sites[1].leaf_frame as usize, 0xaaaa);
    assert_eq!(sites[0].stack.len(), 2);
    assert_eq!(sites[1].stack.len(), 2);
}

#[test]
fn top_sites_truncates_to_n() {
    let p = HeapProfile::from_samples(vec![
        make_sample(vec![0x1], 1000),
        make_sample(vec![0x2], 2000),
        make_sample(vec![0x3], 3000),
        make_sample(vec![0x4], 4000),
        make_sample(vec![0x5], 5000),
    ]);
    let sites = p.top_sites(3, HotSpotKey::LeafFrame);
    assert_eq!(sites.len(), 3);
    assert_eq!(sites[0].leaf_frame as usize, 0x5);
    assert_eq!(sites[1].leaf_frame as usize, 0x4);
    assert_eq!(sites[2].leaf_frame as usize, 0x3);
    let sum: u128 = sites.iter().map(|s| s.inclusive_bytes).sum();
    assert_eq!(sum, 12000u128);
}

#[test]
fn top_sites_handles_empty_stacks() {
    let p = HeapProfile::from_samples(vec![
        make_sample(vec![], 1000),
        make_sample(vec![], 2000),
        make_sample(vec![0xfeed], 4000),
    ]);
    let sites = p.top_sites(10, HotSpotKey::LeafFrame);
    assert_eq!(sites.len(), 2);
    assert_eq!(sites[0].leaf_frame as usize, 0xfeed);
    assert_eq!(sites[0].inclusive_bytes, 4000u128);
    assert_eq!(sites[1].leaf_frame as usize, 0);
    assert_eq!(sites[1].inclusive_bytes, 3000u128);
    assert_eq!(sites[1].sample_count, 2);
}

#[test]
fn top_sites_call_site_degrades_to_leaf() {
    let p = HeapProfile::from_samples(vec![
        make_sample(vec![0xaaaa, 0xbbbb], 4096),
        make_sample(vec![0xaaaa, 0xcccc], 8192),
    ]);
    let leaf_sites = p.top_sites(10, HotSpotKey::LeafFrame);
    let call_sites = p.top_sites(10, HotSpotKey::CallSite);
    assert_eq!(leaf_sites.len(), call_sites.len());
    for (a, b) in leaf_sites.iter().zip(call_sites.iter()) {
        assert_eq!(a.leaf_frame, b.leaf_frame);
        assert_eq!(a.inclusive_bytes, b.inclusive_bytes);
        assert_eq!(a.sample_count, b.sample_count);
    }
}

#[cfg(feature = "symbolicate")]
#[inline(never)]
fn snmalloc_rs_phase_11_3_callsite_probe_alpha() -> Vec<*const u8> {
    let mut frames: Vec<*const u8> = Vec::new();
    backtrace::trace(|frame| {
        frames.push(frame.ip() as *const u8);
        true
    });
    frames
}

#[cfg(feature = "symbolicate")]
#[inline(never)]
fn snmalloc_rs_phase_11_3_callsite_probe_beta() -> Vec<*const u8> {
    let mut frames: Vec<*const u8> = Vec::new();
    backtrace::trace(|frame| {
        frames.push(frame.ip() as *const u8);
        true
    });
    frames
}

#[cfg(feature = "symbolicate")]
#[test]
fn callsite_groups_by_user_caller() {
    let alpha = snmalloc_rs_phase_11_3_callsite_probe_alpha();
    let beta = snmalloc_rs_phase_11_3_callsite_probe_beta();
    assert!(!alpha.is_empty(), "alpha probe captured no frames");
    assert!(!beta.is_empty(), "beta probe captured no frames");

    let p = HeapProfile::from_samples(vec![
        BtSample {
            alloc_ptr: core::ptr::null(),
            requested_size: 64,
            allocated_size: 64,
            weight: 4096,
            stack: alpha.clone(),
        },
        BtSample {
            alloc_ptr: core::ptr::null(),
            requested_size: 64,
            allocated_size: 64,
            weight: 8192,
            stack: beta.clone(),
        },
    ]);

    let sites = p.top_sites(10, HotSpotKey::CallSite);
    assert_eq!(
        sites.len(),
        2,
        "expected 2 CallSite buckets (one per probe), got {}: {:?}",
        sites.len(),
        sites
            .iter()
            .map(|s| (s.leaf_frame, s.inclusive_bytes))
            .collect::<Vec<_>>()
    );
    let total: u128 = sites.iter().map(|s| s.inclusive_bytes).sum();
    assert_eq!(total, 12288u128);
    let count_total: u64 = sites.iter().map(|s| s.sample_count).sum();
    assert_eq!(count_total, 2);
}

#[cfg(feature = "symbolicate")]
#[test]
fn callsite_falls_back_when_no_user_frame() {
    let unresolvable: *const u8 = 0x1 as *const u8;
    let p = HeapProfile::from_samples(vec![BtSample {
        alloc_ptr: core::ptr::null(),
        requested_size: 32,
        allocated_size: 32,
        weight: 1024,
        stack: vec![unresolvable],
    }]);
    let sites = p.top_sites(10, HotSpotKey::CallSite);
    assert_eq!(sites.len(), 1);
    assert_eq!(sites[0].inclusive_bytes, 1024u128);
    assert_eq!(sites[0].sample_count, 1);
    assert_eq!(sites[0].leaf_frame, unresolvable);
}

#[cfg(not(feature = "symbolicate"))]
#[test]
fn callsite_fallback_when_unsymbolicated() {
    let p = HeapProfile::from_samples(vec![
        make_sample(vec![0xaaaa, 0xbbbb], 4096),
        make_sample(vec![0xdddd, 0xeeee], 2048),
    ]);
    let sites = p.top_sites(10, HotSpotKey::CallSite);
    assert_eq!(sites.len(), 2);
    let total: u128 = sites.iter().map(|s| s.inclusive_bytes).sum();
    assert_eq!(total, 6144u128);
}

#[test]
fn lookup_alloc_site_feature_off_returns_none() {
    if cfg!(feature = "profiling") {
        return;
    }
    let a = SnMalloc::new();
    assert!(a.lookup_alloc_site(0x1234 as *const u8).is_none());
    assert!(a.lookup_alloc_site(core::ptr::null()).is_none());
}

#[test]
fn lookup_alloc_site_miss_for_unmapped_addr() {
    let a = SnMalloc::new();
    if !a.profiling_supported() {
        return;
    }
    assert!(a.lookup_alloc_site(0x1 as *const u8).is_none());
}

#[test]
fn lookup_alloc_site_matches_snapshot() {
    let a = SnMalloc::new();
    if !a.profiling_supported() {
        return;
    }

    const RATE: usize = 4096;
    const N: usize = 50_000;
    const SIZE: usize = 256;

    let saved = a.sampling_rate();
    a.set_sampling_rate(RATE);

    let layout = Layout::from_size_align(SIZE, 8).unwrap();
    let mut ptrs: Vec<*mut u8> = Vec::with_capacity(N);
    for _ in 0..N {
        let p = unsafe { a.alloc(layout) };
        assert!(!p.is_null());
        ptrs.push(p);
    }

    let snap = a.snapshot();
    assert!(
        !snap.is_empty(),
        "expected at least one sample after {N} x {SIZE}B allocs at \
         rate {RATE}; got 0"
    );

    let mut interior_checked = 0usize;
    for sample in snap.samples() {
        let base = sample.alloc_ptr;
        if base.is_null() {
            continue;
        }
        let hit = a
            .lookup_alloc_site(base)
            .expect("base-address lookup must succeed for a live sample");
        assert_eq!(hit.base_addr, base);
        assert_eq!(hit.allocated_size, sample.allocated_size);
        assert_eq!(hit.frames.len(), sample.stack.len());
        for (a, b) in hit.frames.iter().zip(sample.stack.iter()) {
            assert_eq!(a, b);
        }

        if sample.allocated_size > 1 {
            let interior = unsafe { (base as *const u8).add(sample.allocated_size / 2) };
            let inside = a
                .lookup_alloc_site(interior)
                .expect("interior-pointer lookup must succeed for a live sample");
            assert_eq!(inside.base_addr, base);
            assert_eq!(inside.allocated_size, sample.allocated_size);
            interior_checked += 1;
        }
    }

    assert!(
        interior_checked > 0,
        "interior-pointer path was never exercised; \
         no sampled allocations had allocated_size > 1?"
    );

    for p in &ptrs {
        unsafe { a.dealloc(*p, layout) };
    }
    if let Some(first_base) = snap
        .samples()
        .iter()
        .map(|s| s.alloc_ptr)
        .find(|p| !p.is_null())
    {
        let post = a.lookup_alloc_site(first_base);
        match post {
            None => { /* expected on a quiescent binary */ }
            Some(f) => {
                let _ = f;
            }
        }
    }

    a.set_sampling_rate(saved);
}
