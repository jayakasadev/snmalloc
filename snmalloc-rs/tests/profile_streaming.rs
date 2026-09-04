#![cfg(feature = "profiling")]

use snmalloc_rs::{ProfilingSession, SnMalloc, StreamingError};
use std::alloc::{GlobalAlloc, Layout};
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, Mutex, OnceLock};
use std::thread;

fn session_lock() -> &'static Mutex<()> {
    static LOCK: OnceLock<Mutex<()>> = OnceLock::new();
    LOCK.get_or_init(|| Mutex::new(()))
}

const TEST_RATE: usize = 4096;
const TEST_ALLOCS: usize = 50_000;
const TEST_SIZE: usize = 64;

fn workload(a: &SnMalloc) {
    let layout = Layout::from_size_align(TEST_SIZE, 8).unwrap();
    let mut ptrs: Vec<*mut u8> = Vec::with_capacity(TEST_ALLOCS);
    for _ in 0..TEST_ALLOCS {
        let p = unsafe { a.alloc(layout) };
        assert!(!p.is_null());
        ptrs.push(p);
    }
    for p in ptrs {
        unsafe { a.dealloc(p, layout) };
    }
}

#[test]
fn smoke_session_receives_samples() {
    let _guard = session_lock().lock().unwrap_or_else(|e| e.into_inner());

    let a = SnMalloc::new();
    if !a.profiling_supported() {
        return;
    }
    let saved_rate = a.sampling_rate();
    a.set_sampling_rate(TEST_RATE);

    let counter = Arc::new(AtomicU64::new(0));
    let counter_cb = Arc::clone(&counter);

    let session = ProfilingSession::start(move |sample| {
        let _ = sample.alloc_ptr();
        let _ = sample.requested_size();
        let _ = sample.allocated_size();
        let _ = sample.weight();
        let _ = sample.stack();
        counter_cb.fetch_add(1, Ordering::Relaxed);
    })
    .expect("first ProfilingSession::start must succeed");

    workload(&a);

    drop(session);

    let observed = counter.load(Ordering::Relaxed);
    assert!(
        observed > 0,
        "streaming handler must have observed at least one sample after \
         {TEST_ALLOCS} x {TEST_SIZE}B allocs at rate {TEST_RATE}; got 0"
    );

    a.set_sampling_rate(saved_rate);
}

#[test]
fn double_start_errors_then_recovers() {
    let _guard = session_lock().lock().unwrap_or_else(|e| e.into_inner());

    let a = SnMalloc::new();
    if !a.profiling_supported() {
        return;
    }

    let first = ProfilingSession::start(|_sample| {}).expect("first start must succeed");

    let second = ProfilingSession::start(|_sample| {});
    assert!(
        matches!(second, Err(StreamingError::AlreadyActive)),
        "second start while first is alive must return \
         Err(StreamingError::AlreadyActive); got {second:?}"
    );

    drop(first);

    let third = ProfilingSession::start(|_sample| {});
    assert!(
        third.is_ok(),
        "after dropping the first session a fresh start must \
         succeed; got {third:?}"
    );
    drop(third);
}

#[test]
fn drop_unregisters_handler() {
    let _guard = session_lock().lock().unwrap_or_else(|e| e.into_inner());

    let a = SnMalloc::new();
    if !a.profiling_supported() {
        return;
    }
    let saved_rate = a.sampling_rate();
    a.set_sampling_rate(TEST_RATE);

    let flag = Arc::new(AtomicBool::new(false));
    let flag_cb = Arc::clone(&flag);

    let session = ProfilingSession::start(move |_sample| {
        flag_cb.store(true, Ordering::Relaxed);
    })
    .expect("start must succeed");

    workload(&a);
    let observed_during = flag.load(Ordering::Relaxed);
    assert!(
        observed_during,
        "handler should have observed a sample during the session"
    );

    drop(session);
    flag.store(false, Ordering::Relaxed);

    workload(&a);

    assert!(
        !flag.load(Ordering::Relaxed),
        "handler must NOT be invoked after the session is dropped; \
         the flag was set, implying the Rust slot still holds our \
         closure or the C-side trampoline is still registered"
    );

    a.set_sampling_rate(saved_rate);
}

#[test]
fn thread_safety_concurrent_workload() {
    let _guard = session_lock().lock().unwrap_or_else(|e| e.into_inner());

    let a = SnMalloc::new();
    if !a.profiling_supported() {
        return;
    }
    let saved_rate = a.sampling_rate();
    a.set_sampling_rate(TEST_RATE);

    let counter = Arc::new(AtomicU64::new(0));
    let counter_cb = Arc::clone(&counter);

    let session = ProfilingSession::start(move |sample| {
        let _ = sample.alloc_ptr();
        let _ = sample.requested_size();
        let _ = sample.allocated_size();
        let _ = sample.weight();
        let _ = sample.stack();
        counter_cb.fetch_add(1, Ordering::Relaxed);
    })
    .expect("start must succeed");

    let mut handles = Vec::new();
    for _ in 0..4 {
        handles.push(thread::spawn(|| {
            let a = SnMalloc::new();
            let layout = Layout::from_size_align(TEST_SIZE, 8).unwrap();
            let mut ptrs: Vec<*mut u8> = Vec::with_capacity(TEST_ALLOCS / 4);
            for _ in 0..(TEST_ALLOCS / 4) {
                let p = unsafe { a.alloc(layout) };
                assert!(!p.is_null());
                ptrs.push(p);
            }
            for p in ptrs {
                unsafe { a.dealloc(p, layout) };
            }
        }));
    }
    for h in handles {
        h.join().expect("worker thread must not panic");
    }

    drop(session);

    assert!(
        counter.load(Ordering::Relaxed) > 0,
        "expected the streaming handler to observe at least one \
         sample across {} concurrent workers",
        4
    );

    a.set_sampling_rate(saved_rate);
}
