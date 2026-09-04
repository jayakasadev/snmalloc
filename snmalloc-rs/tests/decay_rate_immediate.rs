#![cfg(target_os = "linux")]

mod decay_smaps;

use decay_smaps::{read_lazyfree_bytes, read_rss_bytes};
use snmalloc_rs::SnMalloc;
use std::alloc::{GlobalAlloc, Layout};
use std::sync::{Mutex, MutexGuard, OnceLock};

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

const LARGE_ALLOC_SIZE: usize = 768 * 1024;

#[test]
fn decay_rate_zero_marks_pages_lazyfree_after_free() {
    let _g = tunable_lock();
    let _restore = DecayRateGuard::new();

    SnMalloc::set_decay_rate(0);
    assert_eq!(
        SnMalloc::decay_rate(),
        0,
        "decay rate must round-trip to 0 before we rely on the \
         immediate-decay backend behaviour"
    );

    let alloc = SnMalloc::new();
    let layout = Layout::from_size_align(LARGE_ALLOC_SIZE, 64).unwrap();

    let lazyfree_before = read_lazyfree_bytes();
    let rss_before = read_rss_bytes();

    let ptr = unsafe { alloc.alloc(layout) };
    assert!(!ptr.is_null(), "large allocation must not return null");

    unsafe {
        let mut offset = 0usize;
        while offset < LARGE_ALLOC_SIZE {
            ptr.add(offset).write_volatile(0xAA);
            offset += 4096;
        }
        ptr.add(LARGE_ALLOC_SIZE - 1).write_volatile(0xAA);
    }

    let rss_peak = read_rss_bytes();
    assert!(
        rss_peak >= rss_before + (LARGE_ALLOC_SIZE as u64) / 2,
        "RSS after touching a {LARGE_ALLOC_SIZE}-byte allocation \
         should rise by roughly that amount (before = {rss_before}, \
         peak = {rss_peak})"
    );

    unsafe { alloc.dealloc(ptr, layout) };

    let lazyfree_after = read_lazyfree_bytes();
    let rss_after = read_rss_bytes();

    let lazyfree_delta = lazyfree_after.saturating_sub(lazyfree_before);
    let observed_fraction = lazyfree_delta as f64 / LARGE_ALLOC_SIZE as f64;

    assert!(
        observed_fraction >= 0.60,
        "expected decay_rate=0 to eagerly mark most of the freed \
         {LARGE_ALLOC_SIZE}-byte allocation LazyFree immediately \
         after free; lazyfree_before={lazyfree_before}, \
         lazyfree_after={lazyfree_after}, delta={lazyfree_delta} \
         ({:.1}% of the allocation size); rss_before={rss_before}, \
         rss_peak={rss_peak}, rss_after={rss_after}",
        observed_fraction * 100.0
    );
}
