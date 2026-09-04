#[cfg(feature = "profiling")]
mod workload {
    use snmalloc_rs::SnMalloc;
    use std::alloc::{GlobalAlloc, Layout};
    use std::sync::{Mutex, MutexGuard, OnceLock};

    pub const RATE: usize = 512;
    pub const N_ALLOCS: usize = 5_000;
    pub const SIZE: usize = 64;

    pub fn workload_lock() -> MutexGuard<'static, ()> {
        static LOCK: OnceLock<Mutex<()>> = OnceLock::new();
        LOCK.get_or_init(|| Mutex::new(()))
            .lock()
            .unwrap_or_else(|poison| poison.into_inner())
    }

    pub fn run_workload(min_samples: usize) -> (snmalloc_rs::HeapProfile, Box<dyn FnOnce()>) {
        let a = SnMalloc::new();
        let saved = a.sampling_rate();
        a.set_sampling_rate(RATE);

        let layout = Layout::from_size_align(SIZE, 8).expect("valid layout");
        let mut ptrs: Vec<*mut u8> = Vec::with_capacity(N_ALLOCS);
        for _ in 0..N_ALLOCS {
            // SAFETY: layout is non-zero and aligned; we feed every
            // pointer back into dealloc with the same layout below.
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
                // has not been freed.
                unsafe { a.dealloc(p, layout) };
            }
            a.set_sampling_rate(saved);
        });

        (snap, cleanup)
    }
}

#[cfg(feature = "profiling")]
#[test]
fn inferno_roundtrip() {
    let _lock = workload::workload_lock();
    let a = snmalloc_rs::SnMalloc::new();
    if !a.profiling_supported() {
        return;
    }

    let (snap, cleanup) = workload::run_workload(50);

    let mut folded: Vec<u8> = Vec::new();
    snap.write_flamegraph(&mut folded)
        .expect("Vec<u8> write is infallible");
    assert!(
        !folded.is_empty(),
        "folded output unexpectedly empty after a >=50-sample snapshot"
    );

    let mut svg: Vec<u8> = Vec::new();
    let mut opts = inferno::flamegraph::Options::default();
    let _ = &mut opts;

    let cursor = std::io::Cursor::new(&folded[..]);
    inferno::flamegraph::from_reader(&mut opts, cursor, &mut svg)
        .expect("inferno must accept the folded stream we produced");

    let svg_text = std::str::from_utf8(&svg).expect("inferno emits UTF-8 SVG");

    assert!(
        svg_text.contains("<svg"),
        "inferno output missing <svg root tag; first 200 chars: {:?}",
        &svg_text.chars().take(200).collect::<String>()
    );
    let has_group = svg_text.contains("<g>") || svg_text.contains("<g ");
    assert!(
        has_group,
        "inferno output missing any <g> stack-frame node; this usually \
         means the folded stream rendered to a 'no stacks' fallback. \
         First 400 chars of SVG: {:?}",
        &svg_text.chars().take(400).collect::<String>()
    );

    cleanup();
}

#[cfg(feature = "profiling")]
#[test]
fn speedscope_folded_import() {
    let _lock = workload::workload_lock();
    let a = snmalloc_rs::SnMalloc::new();
    if !a.profiling_supported() {
        return;
    }

    let (snap, cleanup) = workload::run_workload(50);

    let mut folded: Vec<u8> = Vec::new();
    snap.write_flamegraph(&mut folded)
        .expect("Vec<u8> write is infallible");
    let text = std::str::from_utf8(&folded).expect("folded format is ASCII");

    fn speedscope_matches(line: &str) -> bool {
        let mut it = line.rsplitn(2, ' ');
        let weight = match it.next() {
            Some(s) if !s.is_empty() => s,
            _ => return false,
        };
        let stack = match it.next() {
            Some(s) => s,
            None => return false,
        };
        if stack.is_empty() || stack.chars().any(|c| c.is_whitespace()) {
            return false;
        }
        weight.chars().all(|c| c.is_ascii_digit()) && !weight.is_empty()
    }

    let mut total: usize = 0;
    let mut matched: usize = 0;
    for line in text.lines() {
        if line.is_empty() {
            continue;
        }
        total += 1;
        if speedscope_matches(line) {
            matched += 1;
        }
    }
    assert!(total > 0, "folded output empty over a >=50-sample snapshot");

    assert!(
        matched.saturating_mul(100) >= total.saturating_mul(95),
        "only {}/{} folded lines ({}%) match speedscope's importer \
         regex `^([^\\s]+) (\\d+)$`; required >= 95%",
        matched,
        total,
        (matched.saturating_mul(100)) / total.max(1)
    );

    cleanup();
}

#[cfg(feature = "profiling")]
#[test]
fn round_trip_weight_invariance() {
    let _lock = workload::workload_lock();
    let a = snmalloc_rs::SnMalloc::new();
    if !a.profiling_supported() {
        return;
    }

    let (snap, cleanup) = workload::run_workload(50);

    let mut folded: Vec<u8> = Vec::new();
    snap.write_flamegraph(&mut folded)
        .expect("Vec<u8> write is infallible");
    let text = std::str::from_utf8(&folded).expect("folded format is ASCII");

    let mut sum: u128 = 0;
    for line in text.lines() {
        let mut it = line.rsplitn(2, ' ');
        let weight: u128 = it
            .next()
            .expect("trailing weight")
            .parse()
            .unwrap_or_else(|_| panic!("non-integer weight in line {:?}", line));
        let _stack = it.next().expect("leading stack");
        sum = sum.saturating_add(weight);
    }

    assert_eq!(
        sum,
        snap.total_allocated_bytes(),
        "sum of folded weights does not match HeapProfile::total_allocated_bytes; \
         the BTreeMap collapse step in write_flamegraph dropped or duplicated a stack"
    );

    cleanup();
}

#[test]
fn empty_snapshot_viewer_safety() {
    let p = snmalloc_rs::HeapProfile::default();
    assert!(p.is_empty());

    let mut folded: Vec<u8> = Vec::new();
    p.write_flamegraph(&mut folded)
        .expect("empty profile write is infallible");
    assert!(
        folded.is_empty(),
        "empty profile must produce zero-length folded output; got {} bytes",
        folded.len()
    );

    let mut svg: Vec<u8> = Vec::new();
    let mut opts = inferno::flamegraph::Options::default();
    let cursor = std::io::Cursor::new(&folded[..]);
    let result = inferno::flamegraph::from_reader(&mut opts, cursor, &mut svg);
    assert!(
        result.is_err(),
        "inferno should reject an empty folded stream with an Err, \
         not silently produce an SVG; got Ok(()) with {} bytes of SVG",
        svg.len()
    );
}
