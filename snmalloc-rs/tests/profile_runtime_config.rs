use snmalloc_rs::{ProfileConfig, SnMalloc, ENV_PROFILE_ENABLE, ENV_PROFILE_RATE};
use std::env;
use std::sync::{Mutex, MutexGuard, OnceLock};

fn env_lock() -> MutexGuard<'static, ()> {
    static LOCK: OnceLock<Mutex<()>> = OnceLock::new();
    LOCK.get_or_init(|| Mutex::new(()))
        .lock()
        .unwrap_or_else(|poison| poison.into_inner())
}

struct EnvGuard {
    saved_rate: usize,
    saved_rate_env: Option<String>,
    saved_enable_env: Option<String>,
}

impl EnvGuard {
    fn new() -> Self {
        let a = SnMalloc::new();
        let g = EnvGuard {
            saved_rate: a.sampling_rate(),
            saved_rate_env: env::var(ENV_PROFILE_RATE).ok(),
            saved_enable_env: env::var(ENV_PROFILE_ENABLE).ok(),
        };
        env::remove_var(ENV_PROFILE_RATE);
        env::remove_var(ENV_PROFILE_ENABLE);
        g
    }
}

impl Drop for EnvGuard {
    fn drop(&mut self) {
        match &self.saved_rate_env {
            Some(v) => env::set_var(ENV_PROFILE_RATE, v),
            None => env::remove_var(ENV_PROFILE_RATE),
        }
        match &self.saved_enable_env {
            Some(v) => env::set_var(ENV_PROFILE_ENABLE, v),
            None => env::remove_var(ENV_PROFILE_ENABLE),
        }
        let a = SnMalloc::new();
        a.set_sampling_rate(self.saved_rate);
    }
}

#[test]
fn init_from_env_no_vars_is_noop() {
    let _lock = env_lock();
    let _guard = EnvGuard::new();
    let a = SnMalloc::new();

    a.set_sampling_rate(0);

    let applied = a.init_profiling_from_env();
    assert_eq!(applied, None, "no env vars -> no rate applied");
    assert_eq!(
        a.sampling_rate(),
        0,
        "init_profiling_from_env must not touch the rate when env is empty"
    );
}

#[test]
fn init_from_env_rate_only() {
    let _lock = env_lock();
    let _guard = EnvGuard::new();
    let a = SnMalloc::new();

    env::set_var(ENV_PROFILE_RATE, "4096");
    let applied = a.init_profiling_from_env();
    assert_eq!(
        applied,
        Some(4096),
        "RATE=4096 should resolve to Some(4096)"
    );
    if cfg!(feature = "profiling") {
        assert_eq!(a.sampling_rate(), 4096);
    } else {
        assert_eq!(a.sampling_rate(), 0);
    }
}

#[test]
fn init_from_env_enable_false() {
    let _lock = env_lock();
    let _guard = EnvGuard::new();
    let a = SnMalloc::new();

    a.set_sampling_rate(8192);

    env::set_var(ENV_PROFILE_ENABLE, "0");
    let applied = a.init_profiling_from_env();
    assert_eq!(applied, Some(0), "ENABLE=0 should resolve to Some(0)");
    assert_eq!(a.sampling_rate(), 0, "ENABLE=0 must set the rate to 0");
}

#[test]
fn init_from_env_enable_true_uses_default_rate() {
    let _lock = env_lock();
    let _guard = EnvGuard::new();
    let a = SnMalloc::new();

    a.set_sampling_rate(0);

    env::set_var(ENV_PROFILE_ENABLE, "1");
    let applied = a.init_profiling_from_env();
    assert_eq!(
        applied,
        Some(524_288),
        "ENABLE=1 with no RATE should resolve to the 512 KiB default"
    );
    if cfg!(feature = "profiling") {
        assert_eq!(a.sampling_rate(), 524_288);
    } else {
        assert_eq!(a.sampling_rate(), 0);
    }
}

#[test]
fn init_from_env_enable_truthy_aliases() {
    let _lock = env_lock();
    let _guard = EnvGuard::new();
    let a = SnMalloc::new();

    for v in ["true", "TRUE", "yes", " 1 ", "Yes"] {
        a.set_sampling_rate(0);
        env::remove_var(ENV_PROFILE_RATE);
        env::set_var(ENV_PROFILE_ENABLE, v);
        let applied = a.init_profiling_from_env();
        assert_eq!(
            applied,
            Some(524_288),
            "ENABLE={v:?} should be truthy and resolve to the default rate"
        );
    }
}

#[test]
fn init_from_env_rate_overrides_enable() {
    let _lock = env_lock();
    let _guard = EnvGuard::new();
    let a = SnMalloc::new();

    a.set_sampling_rate(0);
    env::set_var(ENV_PROFILE_RATE, "16384");
    env::set_var(ENV_PROFILE_ENABLE, "0");
    let applied = a.init_profiling_from_env();
    assert_eq!(applied, Some(16_384), "RATE=16384 should override ENABLE=0");
    if cfg!(feature = "profiling") {
        assert_eq!(a.sampling_rate(), 16_384);
    } else {
        assert_eq!(a.sampling_rate(), 0);
    }
}

#[test]
fn init_from_env_rate_zero_disables() {
    let _lock = env_lock();
    let _guard = EnvGuard::new();
    let a = SnMalloc::new();

    a.set_sampling_rate(8192);
    env::set_var(ENV_PROFILE_RATE, "0");
    env::set_var(ENV_PROFILE_ENABLE, "1");
    let applied = a.init_profiling_from_env();
    assert_eq!(applied, Some(0), "RATE=0 wins, resolves to Some(0)");
    assert_eq!(a.sampling_rate(), 0);
}

#[test]
fn init_from_env_unparseable_rate_falls_through() {
    let _lock = env_lock();
    let _guard = EnvGuard::new();
    let a = SnMalloc::new();

    a.set_sampling_rate(0);
    env::set_var(ENV_PROFILE_RATE, "not-a-number");
    env::set_var(ENV_PROFILE_ENABLE, "1");
    let applied = a.init_profiling_from_env();
    assert_eq!(
        applied,
        Some(524_288),
        "garbage RATE should be ignored; ENABLE=1 then drives the default rate"
    );
}

#[test]
fn configure_profiling_end_to_end() {
    let _lock = env_lock();
    let _guard = EnvGuard::new();
    let a = SnMalloc::new();

    a.configure_profiling(ProfileConfig {
        sampling_rate: 32_768,
        enable_from_env: false,
    });

    if cfg!(feature = "profiling") {
        assert_eq!(a.sampling_rate(), 32_768);
    } else {
        assert_eq!(a.sampling_rate(), 0);
    }

    a.configure_profiling(ProfileConfig::default());
    assert_eq!(a.sampling_rate(), 0);
}

#[test]
fn configure_profiling_honours_enable_from_env() {
    let _lock = env_lock();
    let _guard = EnvGuard::new();
    let a = SnMalloc::new();

    env::set_var(ENV_PROFILE_RATE, "8192");
    a.configure_profiling(ProfileConfig {
        sampling_rate: 1,
        enable_from_env: true,
    });

    assert_eq!(
        a.sampling_rate(),
        if cfg!(feature = "profiling") { 8192 } else { 0 }
    );
}

#[test]
fn profiling_and_runtime_sampling_apis_agree() {
    let a = SnMalloc::new();
    if !a.profiling_supported() {
        return;
    }

    let saved = SnMalloc::sample_interval();
    a.set_sampling_rate(12_345);
    assert_eq!(SnMalloc::sample_interval(), 12_345);

    SnMalloc::set_sample_interval(54_321);
    assert_eq!(a.sampling_rate(), 54_321);
    SnMalloc::set_sample_interval(saved);
}
