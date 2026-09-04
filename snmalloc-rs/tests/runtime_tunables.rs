use snmalloc_rs::SnMalloc;
use std::sync::{Mutex, MutexGuard, OnceLock};

fn tunable_lock() -> MutexGuard<'static, ()> {
    static LOCK: OnceLock<Mutex<()>> = OnceLock::new();
    LOCK.get_or_init(|| Mutex::new(()))
        .lock()
        .unwrap_or_else(|poison| poison.into_inner())
}

struct TunableGuard {
    saved_sample_interval: u64,
    saved_decay_rate: u32,
    saved_max_local_cache: u64,
}

impl TunableGuard {
    fn new() -> Self {
        Self {
            saved_sample_interval: SnMalloc::sample_interval(),
            saved_decay_rate: SnMalloc::decay_rate(),
            saved_max_local_cache: SnMalloc::max_local_cache(),
        }
    }
}

impl Drop for TunableGuard {
    fn drop(&mut self) {
        SnMalloc::set_sample_interval(self.saved_sample_interval);
        SnMalloc::set_decay_rate(self.saved_decay_rate);
        SnMalloc::set_max_local_cache(self.saved_max_local_cache);
    }
}

#[test]
fn sample_interval_roundtrip() {
    let _g = tunable_lock();
    let _restore = TunableGuard::new();

    SnMalloc::set_sample_interval(1024);
    assert_eq!(
        SnMalloc::sample_interval(),
        1024,
        "set_sample_interval(1024) must round-trip through \
         sample_interval()"
    );

    SnMalloc::set_sample_interval(0);
    assert_eq!(
        SnMalloc::sample_interval(),
        0,
        "set_sample_interval(0) must round-trip; 0 is a valid \
         'sampling disabled' signal"
    );
}

#[test]
fn decay_rate_roundtrip() {
    let _g = tunable_lock();
    let _restore = TunableGuard::new();

    SnMalloc::set_decay_rate(200);
    assert_eq!(SnMalloc::decay_rate(), 200);

    SnMalloc::set_decay_rate(0);
    assert_eq!(SnMalloc::decay_rate(), 0);

    SnMalloc::set_decay_rate(u32::MAX - 1);
    assert_eq!(SnMalloc::decay_rate(), u32::MAX - 1);
}

#[test]
fn max_local_cache_roundtrip() {
    let _g = tunable_lock();
    let _restore = TunableGuard::new();

    SnMalloc::set_max_local_cache(4 * 1024 * 1024);
    assert_eq!(SnMalloc::max_local_cache(), 4 * 1024 * 1024);

    SnMalloc::set_max_local_cache(0);
    assert_eq!(SnMalloc::max_local_cache(), 0);

    let wide: u64 = 1_u64 << 40;
    SnMalloc::set_max_local_cache(wide);
    assert_eq!(SnMalloc::max_local_cache(), wide);
}

#[test]
fn tunables_are_independent() {
    let _g = tunable_lock();
    let _restore = TunableGuard::new();

    SnMalloc::set_sample_interval(0xA1A1_A1A1_A1A1_A1A1);
    SnMalloc::set_decay_rate(0xB2B2_B2B2);
    SnMalloc::set_max_local_cache(0xC3C3_C3C3_C3C3_C3C3);

    assert_eq!(SnMalloc::sample_interval(), 0xA1A1_A1A1_A1A1_A1A1);
    assert_eq!(SnMalloc::decay_rate(), 0xB2B2_B2B2);
    assert_eq!(SnMalloc::max_local_cache(), 0xC3C3_C3C3_C3C3_C3C3);
}

#[test]
fn tunables_survive_thread_spawn() {
    let _g = tunable_lock();
    let _restore = TunableGuard::new();

    SnMalloc::set_sample_interval(987_654);

    let observed = std::thread::spawn(|| SnMalloc::sample_interval())
        .join()
        .expect("worker thread panicked");

    assert_eq!(
        observed, 987_654,
        "tunable set on main thread must be visible to worker thread \
         (process-wide singleton contract)"
    );

    std::thread::spawn(|| SnMalloc::set_sample_interval(12_345))
        .join()
        .expect("worker thread panicked");
    assert_eq!(SnMalloc::sample_interval(), 12_345);
}

#[test]
fn defaults_are_nonzero() {
    let _g = tunable_lock();
    let _restore = TunableGuard::new();

    assert!(
        SnMalloc::sample_interval() > 0,
        "default sample interval must be non-zero"
    );
    assert!(
        SnMalloc::decay_rate() > 0,
        "default decay rate must be non-zero"
    );
    assert!(
        SnMalloc::max_local_cache() > 0,
        "default max local cache must be non-zero"
    );
}
