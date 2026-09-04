#![cfg(all(target_os = "linux", not(feature = "qemu")))]

mod decay_smaps;

use decay_smaps::read_lazyfree_bytes;
use snmalloc_rs::SnMalloc;
use std::alloc::{GlobalAlloc, Layout};
use std::sync::{Mutex, MutexGuard, OnceLock};
use std::time::Duration;

fn tunable_lock() -> MutexGuard<'static, ()> {
    static LOCK: OnceLock<Mutex<()>> = OnceLock::new();
    LOCK.get_or_init(|| Mutex::new(()))
        .lock()
        .unwrap_or_else(|poison| poison.into_inner())
}

struct DecayRateGuard {
    saved: u32,
}

impl DecayRateGuard {
    fn new() -> Self {
        Self {
            saved: SnMalloc::decay_rate(),
        }
    }
}

impl Drop for DecayRateGuard {
    fn drop(&mut self) {
        SnMalloc::set_decay_rate(self.saved);
    }
}

const OBSERVED_ALLOC_SIZE: usize = 768 * 1024;

const SWEEP_TRIGGER_ALLOC_SIZE: usize = 100 * 1024;

const DECAY_WINDOW_MS: u32 = 200;

const SLEEP_PAST_WINDOW: Duration = Duration::from_millis(2 * DECAY_WINDOW_MS as u64);

unsafe fn touch_all_pages(ptr: *mut u8, len: usize) {
    let mut offset = 0usize;
    while offset < len {
        ptr.add(offset).write_volatile(0xAA);
        offset += 4096;
    }
    ptr.add(len - 1).write_volatile(0xAA);
}

unsafe fn alloc_touch_and_free(alloc: &SnMalloc, size: usize) {
    let layout = Layout::from_size_align(size, 64).unwrap();
    let ptr = alloc.alloc(layout);
    assert!(
        !ptr.is_null(),
        "{size}-byte allocation must not return null"
    );
    touch_all_pages(ptr, size);
    alloc.dealloc(ptr, layout);
}

#[test]
fn decay_rate_windowed_marks_pages_lazyfree_after_window_elapses() {
    let _g = tunable_lock();
    let _restore = DecayRateGuard::new();

    SnMalloc::set_decay_rate(DECAY_WINDOW_MS);
    assert_eq!(
        SnMalloc::decay_rate(),
        DECAY_WINDOW_MS,
        "decay rate must round-trip before we rely on the windowed \
         backend behaviour"
    );

    let alloc = SnMalloc::new();
    let layout = Layout::from_size_align(OBSERVED_ALLOC_SIZE, 64).unwrap();

    let lazyfree_before = read_lazyfree_bytes();

    let ptr = unsafe { alloc.alloc(layout) };
    assert!(!ptr.is_null(), "observed allocation must not return null");
    unsafe { touch_all_pages(ptr, OBSERVED_ALLOC_SIZE) };

    unsafe { alloc.dealloc(ptr, layout) };

    let lazyfree_immediately_after_free = read_lazyfree_bytes();
    let immediate_delta = lazyfree_immediately_after_free.saturating_sub(lazyfree_before);
    let immediate_fraction = immediate_delta as f64 / OBSERVED_ALLOC_SIZE as f64;
    assert!(
        immediate_fraction < 0.10,
        "with decay_rate={DECAY_WINDOW_MS}ms, freeing must NOT \
         immediately mark pages LazyFree (that is the decay_rate=0 \
         behaviour tested by decay_rate_immediate.rs); \
         lazyfree_before={lazyfree_before}, \
         lazyfree_immediately_after_free={lazyfree_immediately_after_free}, \
         delta={immediate_delta} ({:.1}% of the allocation size)",
        immediate_fraction * 100.0
    );

    std::thread::sleep(SLEEP_PAST_WINDOW);

    for _ in 0..4 {
        unsafe { alloc_touch_and_free(&alloc, SWEEP_TRIGGER_ALLOC_SIZE) };
    }

    let lazyfree_after_sweep = read_lazyfree_bytes();

    let sweep_delta = lazyfree_after_sweep.saturating_sub(lazyfree_before);
    let sweep_fraction = sweep_delta as f64 / OBSERVED_ALLOC_SIZE as f64;

    assert!(
        sweep_fraction >= 0.60,
        "expected decay_rate={DECAY_WINDOW_MS}ms to mark most of the \
         freed {OBSERVED_ALLOC_SIZE}-byte allocation LazyFree once the \
         decay window elapsed and the sweep ran; \
         lazyfree_before={lazyfree_before}, \
         lazyfree_immediately_after_free={lazyfree_immediately_after_free}, \
         lazyfree_after_sweep={lazyfree_after_sweep}, \
         delta={sweep_delta} ({:.1}% of the allocation size)",
        sweep_fraction * 100.0
    );
}
