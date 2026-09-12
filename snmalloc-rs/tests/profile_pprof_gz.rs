#![cfg(feature = "profiling")]

use snmalloc_rs::{HeapProfile, SnMalloc, Weight};
use std::alloc::{GlobalAlloc, Layout};
use std::io::Read;
use std::sync::{Mutex, MutexGuard, OnceLock};

const RATE: usize = 512;
const N_ALLOCS: usize = 5_000;
const SIZE: usize = 64;

fn workload_lock() -> MutexGuard<'static, ()> {
    static LOCK: OnceLock<Mutex<()>> = OnceLock::new();
    LOCK.get_or_init(|| Mutex::new(()))
        .lock()
        .unwrap_or_else(|poison| poison.into_inner())
}

fn run_workload(min_samples: usize) -> (HeapProfile, Box<dyn FnOnce()>) {
    let a = SnMalloc::new();
    let saved = a.sampling_rate();
    a.set_sampling_rate(RATE);

    let layout = Layout::from_size_align(SIZE, 8).expect("valid layout");
    let mut ptrs: Vec<*mut u8> = Vec::with_capacity(N_ALLOCS);
    for _ in 0..N_ALLOCS {
        // SAFETY: layout is non-zero, every pointer is fed back to
        // dealloc in the cleanup closure.
        let p = unsafe { a.alloc(layout) };
        assert!(!p.is_null(), "snmalloc alloc returned NULL");
        ptrs.push(p);
    }

    let snap = a.snapshot();
    assert!(
        snap.len() >= min_samples,
        "expected at least {} samples; got {}.  Increase N_ALLOCS or \
         check the SNMALLOC_PROFILE wiring.",
        min_samples,
        snap.len()
    );

    let cleanup = Box::new(move || {
        let a = SnMalloc::new();
        for p in ptrs {
            // SAFETY: each `p` came from `alloc(layout)` above and
            // has not been freed yet.
            unsafe { a.dealloc(p, layout) };
        }
        a.set_sampling_rate(saved);
    });

    (snap, cleanup)
}

#[test]
fn write_pprof_gz_has_gzip_magic() {
    let _lock = workload_lock();
    let a = SnMalloc::new();
    if !a.profiling_supported() {
        return;
    }

    let (snap, cleanup) = run_workload(50);

    let mut buf: Vec<u8> = Vec::new();
    snap.write_pprof_gz(&mut buf, Weight::Allocated)
        .expect("Vec<u8> write is infallible");
    assert!(
        buf.len() >= 2,
        "gzip stream too short ({} bytes)",
        buf.len()
    );
    assert_eq!(
        buf[0], 0x1f,
        "first byte must be gzip magic 0x1f; got 0x{:02x}",
        buf[0]
    );
    assert_eq!(
        buf[1], 0x8b,
        "second byte must be gzip magic 0x8b; got 0x{:02x}",
        buf[1]
    );

    cleanup();
}

#[test]
fn write_pprof_gz_round_trips_to_write_pprof() {
    let _lock = workload_lock();
    let a = SnMalloc::new();
    if !a.profiling_supported() {
        return;
    }

    let (snap, cleanup) = run_workload(50);

    let weight = Weight::Allocated;

    let mut gz: Vec<u8> = Vec::new();
    snap.write_pprof_gz(&mut gz, weight)
        .expect("Vec<u8> write is infallible");

    let mut uncompressed: Vec<u8> = Vec::new();
    snap.write_pprof(&mut uncompressed, weight)
        .expect("Vec<u8> write is infallible");

    let mut decoded: Vec<u8> = Vec::new();
    flate2::read::GzDecoder::new(gz.as_slice())
        .read_to_end(&mut decoded)
        .expect("gzip decode succeeds");

    assert_eq!(
        decoded.len(),
        uncompressed.len(),
        "decoded gz payload length ({}) != write_pprof length ({})",
        decoded.len(),
        uncompressed.len()
    );
    assert_eq!(
        decoded, uncompressed,
        "decoded gzipped pprof must match the uncompressed pprof byte-for-byte"
    );

    assert!(
        gz.len() >= 18,
        "gz output suspiciously short ({} bytes) -- missing header/trailer?",
        gz.len()
    );

    cleanup();
}

#[test]
fn write_pprof_gz_empty_snapshot() {
    let p = HeapProfile::default();
    assert!(p.is_empty());

    let mut gz: Vec<u8> = Vec::new();
    p.write_pprof_gz(&mut gz, Weight::Allocated)
        .expect("empty profile write is infallible");

    assert!(gz.len() >= 2);
    assert_eq!(gz[0], 0x1f);
    assert_eq!(gz[1], 0x8b);

    let mut uncompressed: Vec<u8> = Vec::new();
    p.write_pprof(&mut uncompressed, Weight::Allocated)
        .expect("empty profile write is infallible");

    let mut decoded: Vec<u8> = Vec::new();
    flate2::read::GzDecoder::new(gz.as_slice())
        .read_to_end(&mut decoded)
        .expect("gzip decode succeeds even on tiny payload");

    assert_eq!(
        decoded, uncompressed,
        "decoded empty-snapshot pprof must match the uncompressed encoding"
    );
}
