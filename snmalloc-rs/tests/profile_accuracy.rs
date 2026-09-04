use snmalloc_rs::{SnMalloc, Weight};
use std::alloc::{GlobalAlloc, Layout};
use std::collections::HashSet;
use std::sync::{Arc, Barrier, Mutex, OnceLock};
use std::thread;

fn accuracy_lock() -> std::sync::MutexGuard<'static, ()> {
    static LOCK: OnceLock<Mutex<()>> = OnceLock::new();
    LOCK.get_or_init(|| Mutex::new(()))
        .lock()
        .unwrap_or_else(|poison| poison.into_inner())
}

const RATE: usize = 4096;
const N_PER_THREAD: usize = 100_000;
const SIZE: usize = 64;

#[test]
fn accuracy_single_threaded() {
    let _lock = accuracy_lock();
    let a = SnMalloc::new();
    if !a.profiling_supported() {
        return;
    }

    let saved = a.sampling_rate();
    a.set_sampling_rate(0);
    let baseline = a.snapshot();
    let baseline_count = baseline.len();
    let baseline_requested = baseline.total_requested_bytes();
    drop(baseline);
    a.set_sampling_rate(RATE);

    let layout = Layout::from_size_align(SIZE, 8).unwrap();
    let mut ptrs: Vec<*mut u8> = Vec::with_capacity(N_PER_THREAD);
    for _ in 0..N_PER_THREAD {
        let p = unsafe { a.alloc(layout) };
        assert!(!p.is_null());
        ptrs.push(p);
    }

    let snap = a.snapshot();
    let observed = snap.len().saturating_sub(baseline_count);
    let observed_bytes = snap
        .total_requested_bytes()
        .saturating_sub(baseline_requested);

    let expected = (N_PER_THREAD * SIZE) as f64 / RATE as f64;
    let sigma = expected.sqrt();
    let low = expected - 6.0 * sigma;
    let high = expected + 6.0 * sigma;
    assert!(
        observed > 0,
        "got 0 samples after {N_PER_THREAD} x {SIZE}B; profile slot \
         likely not wired into the Rust shim's Config"
    );
    assert!(
        (observed as f64) >= low && (observed as f64) <= high,
        "single-threaded: observed {observed} samples (baseline \
         {baseline_count}), expected {expected:.1} +/- 6 sigma \
         ({sigma:.1}); window = [{low:.1}, {high:.1}]"
    );

    let expected_bytes_f = (N_PER_THREAD * SIZE) as f64;
    let sigma_bytes = (expected_bytes_f * RATE as f64).sqrt();
    let lo_bytes_f = expected_bytes_f - 6.0 * sigma_bytes;
    let hi_bytes_f = expected_bytes_f + 6.0 * sigma_bytes;
    let lo_bytes: u128 = if lo_bytes_f < 0.0 {
        0
    } else {
        lo_bytes_f as u128
    };
    let hi_bytes: u128 = hi_bytes_f as u128;
    let expected_bytes = expected_bytes_f as u128;
    assert!(
        observed_bytes >= lo_bytes && observed_bytes <= hi_bytes,
        "single-threaded: sum(weight) = {observed_bytes} bytes \
         (baseline {baseline_requested}), expected {expected_bytes} \
         +/- 6 sigma ({sigma_bytes:.0}); window = [{lo_bytes}, {hi_bytes}]"
    );

    for p in ptrs {
        unsafe { a.dealloc(p, layout) };
    }
    a.set_sampling_rate(saved);
}

#[test]
fn accuracy_multi_threaded() {
    let _lock = accuracy_lock();
    let a = SnMalloc::new();
    if !a.profiling_supported() {
        return;
    }

    const THREADS: usize = 8;
    const PER_THREAD: usize = 10_000;

    let saved = a.sampling_rate();
    a.set_sampling_rate(0);
    let baseline = a.snapshot();
    let baseline_count = baseline.len();
    drop(baseline);
    a.set_sampling_rate(RATE);

    let barrier = Arc::new(Barrier::new(THREADS));
    let mut handles = Vec::with_capacity(THREADS);
    for _ in 0..THREADS {
        let b = barrier.clone();
        handles.push(thread::spawn(move || {
            b.wait();
            let alloc = SnMalloc::new();
            let layout = Layout::from_size_align(SIZE, 8).unwrap();
            let mut ptrs: Vec<usize> = Vec::with_capacity(PER_THREAD);
            for _ in 0..PER_THREAD {
                let p = unsafe { alloc.alloc(layout) };
                assert!(!p.is_null());
                ptrs.push(p as usize);
            }
            (ptrs, layout)
        }));
    }

    let mut all_ptrs: Vec<(Vec<usize>, Layout)> = Vec::with_capacity(THREADS);
    for h in handles {
        all_ptrs.push(h.join().expect("worker thread panicked"));
    }

    let snap = a.snapshot();
    let observed = snap.len().saturating_sub(baseline_count);
    let expected = (THREADS * PER_THREAD * SIZE) as f64 / RATE as f64;
    let sigma = expected.sqrt();
    let low = expected - 6.0 * sigma;
    let high = expected + 6.0 * sigma;
    assert!(
        observed > 0,
        "got 0 samples after {THREADS} x {PER_THREAD} x {SIZE}B"
    );
    assert!(
        (observed as f64) >= low && (observed as f64) <= high,
        "multi-threaded: observed {observed} samples (baseline \
         {baseline_count}), expected {expected:.1} +/- 6 sigma \
         ({sigma:.1}); window = [{low:.1}, {high:.1}].  See \
         profile_integration.cc for the documented O(1/N) per-thread \
         teardown straggler."
    );

    for (ptrs, layout) in all_ptrs {
        for p in ptrs {
            unsafe { a.dealloc(p as *mut u8, layout) };
        }
    }
    a.set_sampling_rate(saved);
}

#[test]
fn flamegraph_correctness_over_live_snapshot() {
    let _lock = accuracy_lock();
    let a = SnMalloc::new();
    if !a.profiling_supported() {
        return;
    }

    let saved = a.sampling_rate();
    a.set_sampling_rate(RATE);

    let layout = Layout::from_size_align(SIZE, 8).unwrap();
    let mut ptrs: Vec<*mut u8> = Vec::with_capacity(N_PER_THREAD);
    for _ in 0..N_PER_THREAD {
        let p = unsafe { a.alloc(layout) };
        assert!(!p.is_null());
        ptrs.push(p);
    }

    let snap = a.snapshot();
    assert!(
        snap.len() >= 100,
        "expected at least 100 samples; got {}.  Increase \
         N_PER_THREAD or check that the profile slot is wired in.",
        snap.len()
    );

    let mut buf: Vec<u8> = Vec::new();
    snap.write_flamegraph_raw(&mut buf)
        .expect("Vec<u8> write is infallible");
    let text = std::str::from_utf8(&buf).expect("folded format is ASCII");

    let mut seen_stacks: HashSet<String> = HashSet::new();
    let mut sum_weights: u128 = 0;
    let mut line_count: usize = 0;

    for line in text.lines() {
        line_count += 1;
        let mut it = line.rsplitn(2, ' ');
        let weight_str = it.next().expect("trailing weight");
        let stack_str = it.next().expect("leading stack");

        let weight: u128 = weight_str
            .parse()
            .unwrap_or_else(|_| panic!("non-integer weight in line {line:?}"));

        if !stack_str.is_empty() {
            for frame in stack_str.split(';') {
                assert!(
                    frame.starts_with("0x") && frame.len() == 18,
                    "frame {frame:?} in line {line:?} is not a 16-hex code pointer"
                );
                assert!(
                    frame[2..].chars().all(|c| c.is_ascii_hexdigit()),
                    "frame {frame:?} contains a non-hex character"
                );
            }
        }

        assert!(
            seen_stacks.insert(stack_str.to_string()),
            "duplicate stack in folded output: {stack_str:?}"
        );

        sum_weights = sum_weights.saturating_add(weight);
    }

    assert!(
        line_count > 0,
        "folded output is empty over a >=100-sample snapshot"
    );
    assert!(
        line_count <= snap.len(),
        "unique-stack line count {line_count} cannot exceed sample count {}",
        snap.len()
    );

    let expected = snap.total_allocated_bytes();
    assert_eq!(
        sum_weights, expected,
        "sum of folded weights ({sum_weights}) must equal \
         HeapProfile::total_allocated_bytes ({expected}) under the \
         default Weight::Allocated projection"
    );

    let mut buf2: Vec<u8> = Vec::new();
    snap.write_flamegraph_with(Weight::Requested, &mut buf2)
        .expect("Vec<u8> write is infallible");
    let text2 = std::str::from_utf8(&buf2).expect("folded format is ASCII");
    let mut sum2: u128 = 0;
    for line in text2.lines() {
        let mut it = line.rsplitn(2, ' ');
        let w: u128 = it.next().unwrap().parse().unwrap();
        let _ = it.next().unwrap();
        sum2 += w;
    }
    assert_eq!(
        sum2,
        snap.total_requested_bytes(),
        "Weight::Requested sum mismatches total_requested_bytes"
    );

    for p in ptrs {
        unsafe { a.dealloc(p, layout) };
    }
    a.set_sampling_rate(saved);
}

#[test]
fn flamegraph_empty_snapshot_writes_nothing() {
    let _lock = accuracy_lock();
    let a = SnMalloc::new();
    let snap = a.snapshot();
    if !snap.is_empty() {
        return;
    }
    let mut buf: Vec<u8> = Vec::new();
    snap.write_flamegraph(&mut buf).expect("infallible");
    assert!(buf.is_empty());
}
