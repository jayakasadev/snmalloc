#![cfg(feature = "profiling")]

use snmalloc_rs::streaming::EventKind;
use snmalloc_rs::{ProfilingSession, SnMalloc};
use std::alloc::{GlobalAlloc, Layout};
use std::sync::atomic::{AtomicU64, AtomicUsize, Ordering};
use std::sync::{Arc, Mutex, OnceLock};

fn session_lock() -> &'static Mutex<()> {
    static LOCK: OnceLock<Mutex<()>> = OnceLock::new();
    LOCK.get_or_init(|| Mutex::new(()))
}

#[test]
fn streaming_sees_resize_event_on_inplace_realloc() {
    let _guard = session_lock().lock().unwrap_or_else(|e| e.into_inner());

    let a = SnMalloc::new();
    if !a.profiling_supported() {
        return;
    }
    let saved_rate = a.sampling_rate();
    a.set_sampling_rate(1);

    let resize_count = Arc::new(AtomicU64::new(0));
    let alloc_count = Arc::new(AtomicU64::new(0));
    let last_resize_req = Arc::new(AtomicUsize::new(0));
    let last_resize_alloc = Arc::new(AtomicUsize::new(0));

    let rc = Arc::clone(&resize_count);
    let ac = Arc::clone(&alloc_count);
    let lrq = Arc::clone(&last_resize_req);
    let lra = Arc::clone(&last_resize_alloc);

    let session = ProfilingSession::start(move |sample| match sample.kind() {
        EventKind::Resize => {
            rc.fetch_add(1, Ordering::Relaxed);
            lrq.store(sample.requested_size(), Ordering::Relaxed);
            lra.store(sample.allocated_size(), Ordering::Relaxed);
        }
        EventKind::Alloc => {
            ac.fetch_add(1, Ordering::Relaxed);
        }
    })
    .expect("first ProfilingSession::start must succeed");

    const ITERS: usize = 4096;
    const BASE_SIZE: usize = 100; // rounds up to the 128-byte sizeclass
    const GROW_SIZE: usize = 101; // still rounds up to 128
    let base_layout = Layout::from_size_align(BASE_SIZE, 8).unwrap();
    for _ in 0..ITERS {
        let p = unsafe { a.alloc(base_layout) };
        assert!(!p.is_null());
        let p2 = unsafe { a.realloc(p, base_layout, GROW_SIZE) };
        assert!(!p2.is_null());
        let grow_layout = Layout::from_size_align(GROW_SIZE, 8).unwrap();
        unsafe { a.dealloc(p2, grow_layout) };
    }

    drop(session);

    let observed_resize = resize_count.load(Ordering::Relaxed);
    let observed_alloc = alloc_count.load(Ordering::Relaxed);
    let observed_last_req = last_resize_req.load(Ordering::Relaxed);
    let observed_last_alloc = last_resize_alloc.load(Ordering::Relaxed);

    a.set_sampling_rate(saved_rate);

    assert!(
        observed_alloc > 0,
        "streaming handler must have seen at least one Alloc broadcast \
         after {ITERS} alloc/realloc cycles at rate=1; got {observed_alloc}"
    );
    assert!(
        observed_resize > 0,
        "streaming handler must have seen at least one Resize broadcast \
         from the in-place realloc fast path after {ITERS} iterations \
         at rate=1; got {observed_resize} (alloc events: {observed_alloc})"
    );
    assert_eq!(
        observed_last_req, GROW_SIZE,
        "Resize broadcast requested_size should match the grow-to value"
    );
    assert!(
        observed_last_alloc >= observed_last_req,
        "Resize allocated_size {observed_last_alloc} must be >= requested_size {observed_last_req}"
    );
}

#[test]
fn snapshot_kind_is_always_alloc() {
    let a = SnMalloc::new();
    if !a.profiling_supported() {
        return;
    }
    let saved_rate = a.sampling_rate();
    a.set_sampling_rate(1);

    let layout = Layout::from_size_align(100, 8).unwrap();
    let mut leaked: Vec<*mut u8> = Vec::new();
    for _ in 0..64 {
        let p = unsafe { a.alloc(layout) };
        assert!(!p.is_null());
        let p2 = unsafe { a.realloc(p, layout, 101) };
        assert!(!p2.is_null());
        leaked.push(p2);
    }

    let snap = a.snapshot();
    for sample in snap.samples() {
        assert_eq!(
            sample.kind(),
            snmalloc_rs::profile::SampleKind::Alloc,
            "snapshot samples must always carry SampleKind::Alloc; \
             saw a Resize-tagged sample which means the persisted \
             slot's kind byte was mis-set by record_realloc"
        );
    }

    let grow_layout = Layout::from_size_align(101, 8).unwrap();
    for p in leaked {
        unsafe { a.dealloc(p, grow_layout) };
    }

    a.set_sampling_rate(saved_rate);
}
