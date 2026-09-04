#![cfg(feature = "profiling")]

use snmalloc_rs::{HeapProfile, SnMalloc, Weight};
use std::alloc::{GlobalAlloc, Layout};
use std::fs;
use std::io::Write;
use std::path::PathBuf;
use std::process::Command;
use std::sync::{Mutex, MutexGuard, OnceLock};
use std::time::SystemTime;

fn skip_if_no_go() -> bool {
    let probe = Command::new("go").arg("version").output();
    match probe {
        Ok(out) if out.status.success() => false,
        Ok(out) => {
            eprintln!(
                "test skipped: `go version` exited {:?} (stderr: {:?})",
                out.status.code(),
                String::from_utf8_lossy(&out.stderr)
            );
            true
        }
        Err(e) => {
            eprintln!("test skipped: `go` not on PATH ({})", e);
            true
        }
    }
}

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

fn unique_pprof_path(label: &str) -> PathBuf {
    let nanos = SystemTime::now()
        .duration_since(SystemTime::UNIX_EPOCH)
        .map(|d| d.as_nanos())
        .unwrap_or(0);
    let mut p = std::env::temp_dir();
    p.push(format!(
        "snmalloc-pprof-roundtrip-{}-{}-{}.pb",
        label,
        std::process::id(),
        nanos
    ));
    p
}

const PPROF_RAW_MARKERS: &[&str] = &[
    "Samples:",
    "sample_type",
    "PeriodType",
    "alloc_space",
    "alloc_objects",
];

fn has_pprof_marker(haystack: &str) -> bool {
    PPROF_RAW_MARKERS.iter().any(|m| haystack.contains(m))
}

#[test]
fn pprof_roundtrip_via_go_tool() {
    let _lock = workload_lock();

    let a = SnMalloc::new();
    if !a.profiling_supported() {
        return;
    }

    if skip_if_no_go() {
        return;
    }

    let (snap, cleanup) = run_workload(50);

    let mut buf: Vec<u8> = Vec::new();
    snap.write_pprof(&mut buf, Weight::Allocated)
        .expect("Vec<u8> write is infallible");
    assert!(!buf.is_empty(), "pprof bytes unexpectedly empty");

    let path = unique_pprof_path("workload");
    {
        let mut f = fs::File::create(&path)
            .unwrap_or_else(|e| panic!("create {} failed: {}", path.display(), e));
        f.write_all(&buf)
            .unwrap_or_else(|e| panic!("write {} failed: {}", path.display(), e));
    }

    let out = Command::new("go")
        .args(["tool", "pprof", "-raw"])
        .arg(&path)
        .output()
        .unwrap_or_else(|e| panic!("spawning `go tool pprof` failed: {}", e));

    let stdout = String::from_utf8_lossy(&out.stdout).to_string();
    let stderr = String::from_utf8_lossy(&out.stderr).to_string();
    let _ = fs::remove_file(&path);

    assert!(
        out.status.success(),
        "`go tool pprof -raw` exited {:?}\nstdout:\n{}\nstderr:\n{}",
        out.status.code(),
        stdout,
        stderr
    );
    assert!(
        has_pprof_marker(&stdout),
        "`go tool pprof -raw` stdout missing any structural marker \
         ({:?}); stdout was:\n{}\nstderr was:\n{}",
        PPROF_RAW_MARKERS,
        stdout,
        stderr
    );

    cleanup();
}

#[test]
fn empty_snapshot_pprof_roundtrip() {
    if skip_if_no_go() {
        return;
    }

    let p = HeapProfile::default();
    assert!(p.is_empty());

    let mut buf: Vec<u8> = Vec::new();
    p.write_pprof(&mut buf, Weight::Allocated)
        .expect("empty profile write is infallible");
    assert!(
        !buf.is_empty(),
        "even an empty Profile must contain sample_type axes + string \
         table; got zero bytes"
    );

    let path = unique_pprof_path("empty");
    {
        let mut f = fs::File::create(&path)
            .unwrap_or_else(|e| panic!("create {} failed: {}", path.display(), e));
        f.write_all(&buf)
            .unwrap_or_else(|e| panic!("write {} failed: {}", path.display(), e));
    }

    let out = Command::new("go")
        .args(["tool", "pprof", "-raw"])
        .arg(&path)
        .output()
        .unwrap_or_else(|e| panic!("spawning `go tool pprof` failed: {}", e));

    let stdout = String::from_utf8_lossy(&out.stdout).to_string();
    let stderr = String::from_utf8_lossy(&out.stderr).to_string();
    let _ = fs::remove_file(&path);

    assert!(
        out.status.success(),
        "`go tool pprof -raw` rejected an empty Profile; exited {:?}\n\
         stdout:\n{}\nstderr:\n{}",
        out.status.code(),
        stdout,
        stderr
    );
    assert!(
        has_pprof_marker(&stdout),
        "`go tool pprof -raw` stdout on empty Profile missing any \
         structural marker ({:?}); stdout was:\n{}\nstderr was:\n{}",
        PPROF_RAW_MARKERS,
        stdout,
        stderr
    );
}
