#![cfg(feature = "symbolicate")]

use snmalloc_rs::SnMalloc;
use std::alloc::{GlobalAlloc, Layout};
use std::collections::HashSet;
use std::sync::{Mutex, OnceLock};

fn lock() -> std::sync::MutexGuard<'static, ()> {
    static L: OnceLock<Mutex<()>> = OnceLock::new();
    L.get_or_init(|| Mutex::new(()))
        .lock()
        .unwrap_or_else(|poison| poison.into_inner())
}

const RATE: usize = 4096;
const N: usize = 100_000;
const SIZE: usize = 64;

const MIN_RESOLVE_RATIO: f64 = 0.5;

#[test]
fn symbolize_resolves_majority_of_live_frames() {
    let _l = lock();
    let a = SnMalloc::new();
    if !a.profiling_supported() {
        return;
    }

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
        snap.len() >= 100,
        "expected at least 100 samples, got {}; rate or workload too small?",
        snap.len()
    );

    let resolved = snap.symbolize();

    let mut unique: HashSet<*const u8> = HashSet::new();
    for s in snap.samples() {
        for &f in &s.stack {
            unique.insert(f);
        }
    }
    assert!(
        !unique.is_empty(),
        "live snapshot must contain at least one frame"
    );
    for f in &unique {
        assert!(
            resolved.contains_key(f),
            "unique frame {:?} missing from resolved map",
            f
        );
    }
    assert_eq!(
        resolved.len(),
        unique.len(),
        "resolved map has extra keys not present in snapshot"
    );

    let named = resolved.values().filter(|f| f.name.is_some()).count();
    let ratio = named as f64 / resolved.len() as f64;
    assert!(
        ratio >= MIN_RESOLVE_RATIO,
        "only {named}/{} ({:.1}%) unique frames resolved; expected \
         >= {:.0}%",
        resolved.len(),
        ratio * 100.0,
        MIN_RESOLVE_RATIO * 100.0
    );

    for p in ptrs {
        unsafe { a.dealloc(p, layout) };
    }
    a.set_sampling_rate(saved);
}

#[test]
fn flamegraph_symbolicated_renders_cleanly() {
    let _l = lock();
    let a = SnMalloc::new();
    if !a.profiling_supported() {
        return;
    }

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
    assert!(snap.len() >= 100, "snapshot too small: {}", snap.len());

    let mut buf: Vec<u8> = Vec::new();
    snap.write_flamegraph(&mut buf)
        .expect("Vec<u8> write is infallible");
    let text = std::str::from_utf8(&buf).expect("folded format is ASCII");

    let mut seen: HashSet<String> = HashSet::new();
    let mut sum: u128 = 0;
    let mut line_count = 0usize;
    for line in text.lines() {
        line_count += 1;
        let mut it = line.rsplitn(2, ' ');
        let weight_str = it.next().expect("trailing weight");
        let stack_str = it.next().expect("leading stack");
        let weight: u128 = weight_str
            .parse()
            .unwrap_or_else(|_| panic!("non-integer weight in {line:?}"));

        for frame in stack_str.split(';') {
            assert!(
                !frame.contains(' '),
                "frame {frame:?} in line {line:?} contains a space"
            );
            if frame.starts_with("0x") {
                assert_eq!(frame.len(), 18, "hex frame {frame:?} not 16 digits");
                assert!(
                    frame[2..].chars().all(|c| c.is_ascii_hexdigit()),
                    "hex frame {frame:?} contains a non-hex digit"
                );
            }
        }

        assert!(
            seen.insert(stack_str.to_string()),
            "duplicate stack in symbolized folded output: {stack_str:?}"
        );

        sum = sum.saturating_add(weight);
    }
    assert!(line_count > 0, "symbolized folded output is empty");

    let expected = snap.total_allocated_bytes();
    assert_eq!(
        sum, expected,
        "symbolized folded weight sum ({sum}) must equal \
         total_allocated_bytes ({expected})"
    );

    for p in ptrs {
        unsafe { a.dealloc(p, layout) };
    }
    a.set_sampling_rate(saved);
}
