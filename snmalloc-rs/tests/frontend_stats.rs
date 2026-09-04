#![cfg(feature = "stats-basic")]

use snmalloc_rs::SnMalloc;
use std::alloc::{GlobalAlloc, Layout};

#[global_allocator]
static ALLOC: SnMalloc = SnMalloc;
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::sync::Arc;
use std::thread;

const K: usize = 128;
const CROSS_OBJ_SIZE: usize = 512;

#[test]
fn fast_path_alloc_counter_grows() {
    let alloc = SnMalloc::new();
    let before = SnMalloc::full_stats();

    const N: usize = 1000;
    let layout = Layout::from_size_align(32, 16).unwrap();
    let mut ptrs = Vec::with_capacity(N);
    for _ in 0..N {
        let p = unsafe { alloc.alloc(layout) };
        assert!(!p.is_null(), "alloc must succeed");
        ptrs.push(p);
    }

    let after_alloc = SnMalloc::full_stats();
    let alloc_delta = after_alloc.fast_path_allocs - before.fast_path_allocs;
    assert!(
        alloc_delta >= (N as u64) - 10,
        "fast_path_allocs delta (={}) must rise by at least {} after {} \
         small allocations",
        alloc_delta,
        (N as u64) - 10,
        N
    );

    assert!(
        after_alloc.slow_path_allocs > before.slow_path_allocs,
        "slow_path_allocs must rise across slab opens \
         (before={}, after={})",
        before.slow_path_allocs,
        after_alloc.slow_path_allocs,
    );

    for p in ptrs.drain(..) {
        unsafe { alloc.dealloc(p, layout) };
    }
    let after_dealloc = SnMalloc::full_stats();
    let dealloc_delta = after_dealloc.fast_path_deallocs - before.fast_path_deallocs;
    assert!(
        dealloc_delta >= (N as u64) - 10,
        "fast_path_deallocs delta (={}) must rise by at least {} after {} \
         same-thread allocs+frees (the counter is pre-credited at refill, \
         so it is measured cumulatively vs `before`)",
        dealloc_delta,
        (N as u64) - 10,
        N
    );
}

#[test]
fn cross_thread_messages_grow() {
    let main_alloc = SnMalloc::new();
    let before = SnMalloc::full_stats();

    let layout = Layout::from_size_align(CROSS_OBJ_SIZE, 16).unwrap();
    let mut ptrs: Vec<usize> = Vec::with_capacity(K);
    for _ in 0..K {
        let p = unsafe { main_alloc.alloc(layout) };
        assert!(!p.is_null());
        ptrs.push(p as usize);
    }
    // SAFETY: We're going to transfer ownership of these raw pointers
    // to the worker thread. Wrapping as `usize` strips the
    // `*mut u8`'s `!Send` so we can move the Vec across threads;
    // the worker reconstructs the pointers locally.
    let ptrs_for_worker = Arc::new(ptrs);
    let go = Arc::new(AtomicBool::new(false));
    let done_count = Arc::new(AtomicUsize::new(0));

    let ptrs_w = Arc::clone(&ptrs_for_worker);
    let go_w = Arc::clone(&go);
    let done_w = Arc::clone(&done_count);

    let worker = thread::spawn(move || {
        let alloc = SnMalloc::new();
        while !go_w.load(Ordering::Acquire) {
            std::hint::spin_loop();
        }
        for &addr in ptrs_w.iter() {
            unsafe { alloc.dealloc(addr as *mut u8, layout) };
        }
        done_w.store(K, Ordering::Release);
    });

    go.store(true, Ordering::Release);
    worker.join().expect("worker join");
    assert_eq!(done_count.load(Ordering::Acquire), K);

    let after_worker = SnMalloc::full_stats();
    let remote_delta = after_worker.remote_deallocs - before.remote_deallocs;
    assert!(
        remote_delta >= K as u64,
        "remote_deallocs delta (={}) must rise by at least K={} after \
         {} cross-thread frees",
        remote_delta,
        K,
        K,
    );

    let mut local = Vec::with_capacity(4096);
    for _ in 0..4096 {
        let p = unsafe { main_alloc.alloc(layout) };
        assert!(!p.is_null());
        local.push(p);
    }
    for p in local {
        unsafe { main_alloc.dealloc(p, layout) };
    }

    let after_drain = SnMalloc::full_stats();
    let msgs_delta =
        after_drain.cross_thread_messages_received - before.cross_thread_messages_received;
    let drains_delta = after_drain.message_queue_drains - before.message_queue_drains;
    assert!(
        msgs_delta >= 1,
        "cross_thread_messages_received delta (={}) must rise by at \
         least 1 after worker posts and main drains",
        msgs_delta,
    );
    assert!(
        drains_delta >= 1,
        "message_queue_drains delta (={}) must rise by at least 1 \
         after main enters the queue-drain slow path",
        drains_delta,
    );
}
