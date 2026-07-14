//! Integration test for the `decay_rate_ms() == 0` "decay immediately"
//! backend read-side.
//!
//! Background: `SnMalloc::set_decay_rate(0)` used to be storage-only
//! on the Rust side -- the value round-tripped through
//! `SnMalloc::decay_rate()` (see `runtime_tunables.rs`) but nothing on
//! the C++ backend ever looked at it.  `LargeBuddyRange::dealloc_range`
//! (in `src/snmalloc/backend_helpers/largebuddyrange.h`) now checks
//! `RuntimeConfig::decay_rate_ms() == 0` when a freed chunk is
//! absorbed into the buddy allocator's own cache/tree (i.e. it did
//! NOT consolidate all the way up into a block big enough to hand
//! back to the parent range) and, if so, calls
//! `DefaultPal::notify_not_using` on it immediately, rather than
//! leaving the chunk fully committed until some later consolidation
//! or process shutdown.  The matching `alloc_range` path re-notifies
//! the PAL that the chunk is back in use (`notify_using`) before
//! handing it out again, since it may have been marked reclaimable
//! while cached.
//!
//! ## Why this measures `LazyFree`, not `VmRSS`
//!
//! On Linux, `notify_not_using` (`src/snmalloc/pal/pal_linux.h`) calls
//! `madvise(p, size, madvise_free_flags)`, and `madvise_free_flags` is
//! `MADV_FREE` on any kernel that has it (>= 4.5; snmalloc only falls
//! back to `MADV_DONTNEED` on older kernels). `MADV_FREE` is a *lazy*
//! hint: the kernel marks the pages as reclaimable immediately but
//! does NOT evict them from the process's resident set until either
//! (a) memory pressure forces reclamation, or (b) the pages are
//! written to again (triggering a fresh zero-fill). Concretely,
//! `VmRSS` in `/proc/self/status` does NOT drop right after
//! `MADV_FREE` -- confirmed experimentally: a 12 MiB `MADV_FREE`
//! leaves `VmRSS` completely unchanged, while an immediately
//! following `MADV_DONTNEED` on the same range drops it right away.
//! Asserting on `VmRSS` here would therefore fail even when the
//! backend is behaving exactly as designed.
//!
//! The actually-correct, immediately-observable signal for
//! `MADV_FREE` is the `LazyFree:` field of `/proc/self/smaps_rollup`
//! (also surfaced per-mapping in `/proc/self/smaps`): the kernel
//! tags `MADV_FREE`d-but-still-resident pages there the instant the
//! `madvise` call returns. This is what this test asserts grows by
//! (approximately) the size of our allocation after the free -- that
//! is the real, kernel-visible proof that snmalloc issued the
//! immediate-decay `madvise` at all, without waiting on memory
//! pressure or racing a kernel version's choice of `MADV_FREE` vs
//! `MADV_DONTNEED`.
//!
//! This test proves that end-to-end from Rust: with the decay rate
//! set to 0, a large allocation that is freed should have its pages
//! marked lazily-reclaimable by the backend, not just have an
//! internal byte-accounting counter decremented. `full_stats.rs` /
//! `memory_stats.rs` already cover the internal-counter side; this
//! test is the external, OS-observable side.
//!
//! Linux-only: both `/proc/self/status` and `/proc/self/smaps_rollup`
//! are Linux-specific interfaces, and the `madvise` flavour asserted
//! on above (`MADV_FREE` vs `MADV_DONTNEED`) is itself a Linux PAL
//! detail (`src/snmalloc/pal/pal_linux.h`). Other platforms have
//! their own PAL-specific decommit primitives (e.g. Apple's
//! `MADV_FREE_REUSABLE`) that would need a different observability
//! technique entirely.
//!
//! Like `memory_stats.rs`, this measures process-global resources
//! (RSS / smaps) that other allocating tests running in parallel
//! threads of the same binary could disturb. Cargo runs each file
//! under `tests/` as its own binary/process, so putting this test in
//! its own file gives it the isolation it needs -- mirroring the
//! rationale at the top of `memory_stats.rs`.
//!
//! Allocator-invocation style: this test drives `SnMalloc` directly
//! via `SnMalloc::new()` + the `GlobalAlloc` trait methods, the same
//! pattern `memory_stats.rs` and `full_stats.rs` use, rather than
//! installing `#[global_allocator]`. The `madvise` calls under test
//! happen at the PAL level inside the C++ backend and are triggered
//! by our own explicit `alloc`/`dealloc` calls; we don't need Rust's
//! incidental allocations (`Vec`, `String`, thread spawn, etc.) to
//! also route through SnMalloc for this test to be meaningful, so
//! there is no need to install `#[global_allocator]`.

#![cfg(target_os = "linux")]

use snmalloc_rs::SnMalloc;
use std::alloc::{GlobalAlloc, Layout};
use std::fs;
use std::sync::{Mutex, MutexGuard, OnceLock};

/// Serialise every test in this file. `SnMalloc::set_decay_rate` is a
/// process-wide singleton (see `runtime_tunables.rs`); if two tests in
/// this binary raced on it, one test's "restore" could clobber the
/// other's "set 0" mid-measurement. There is only one `#[test]` here
/// today, but the lock costs nothing and guards against future
/// additions silently becoming flaky.
fn tunable_lock() -> MutexGuard<'static, ()> {
    static LOCK: OnceLock<Mutex<()>> = OnceLock::new();
    LOCK.get_or_init(|| Mutex::new(()))
        .lock()
        .unwrap_or_else(|poison| poison.into_inner())
}

/// RAII restore-on-drop for the decay-rate tunable, mirroring the
/// `TunableGuard` pattern in `runtime_tunables.rs` (scoped here to
/// just the one knob this test touches).
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

/// Read one `kB`-suffixed field out of `/proc/self/smaps_rollup`,
/// e.g. `field_kb("LazyFree:")` or `field_kb("Rss:")`. Lines look
/// like:
///
/// ```text
/// LazyFree:              0 kB
/// ```
///
/// `smaps_rollup` aggregates across every mapping in the process, so
/// this is a single cheap read rather than summing per-VMA entries
/// out of the (much larger) `/proc/self/smaps`.
fn read_smaps_rollup_field_kb(field: &str) -> u64 {
    let text = fs::read_to_string("/proc/self/smaps_rollup")
        .expect("failed to read /proc/self/smaps_rollup");
    for line in text.lines() {
        if let Some(rest) = line.strip_prefix(field) {
            return rest
                .trim()
                .trim_end_matches("kB")
                .trim()
                .parse()
                .unwrap_or_else(|e| {
                    panic!(
                        "failed to parse {field} value from {rest:?}: {e}"
                    )
                });
        }
    }
    panic!("{field} line not found in /proc/self/smaps_rollup");
}

/// Bytes currently tagged `MADV_FREE`-but-still-resident, aggregated
/// across the whole process.
fn read_lazyfree_bytes() -> u64 {
    read_smaps_rollup_field_kb("LazyFree:") * 1024
}

/// Total resident set size, aggregated across the whole process.
/// Used only as a monotonicity sanity check here -- see the
/// module doc for why `LazyFree` (not a `Rss` drop) is the correct
/// signal for `MADV_FREE`-based immediate decay.
fn read_rss_bytes() -> u64 {
    read_smaps_rollup_field_kb("Rss:") * 1024
}

/// Large-object size, well above snmalloc's small/large sizeclass
/// boundary (`MAX_SMALL_SIZECLASS_SIZE`, which sits at or below
/// `MIN_CHUNK_SIZE` == `1 << 14` == 16 KiB on the default build) so
/// this allocation is guaranteed to route through the backend
/// `LargeBuddyRange`, not a per-thread small-object slab cache. 12 MiB
/// gives comfortable margin above that boundary and above the
/// per-thread local large-object cache cap (`LocalCacheSizeBits` ==
/// 21 bits == 2 MiB), so the free lands in the *global*
/// `LargeBuddyRange` (`LargeBuddyRange<GlobalCacheSizeBits,
/// bits::BITS - 1, ...>` in `src/snmalloc/backend/standard_range.h`)
/// whose `MAX_SIZE_BITS` is effectively unbounded -- so a lone free
/// essentially never consolidates all the way up to a block "too big
/// for this buddy allocator", meaning `add_block` reliably returns
/// null (chunk absorbed into the buddy's own cache) and the
/// immediate-decay branch in `dealloc_range` fires.
const LARGE_ALLOC_SIZE: usize = 12 * 1024 * 1024;

/// With `decay_rate_ms() == 0`, freeing a large allocation whose
/// backend chunk lands in `LargeBuddyRange`'s own cache/tree (rather
/// than consolidating up to the parent range) must eagerly call
/// `notify_not_using` on those pages -- observable as the kernel
/// immediately tagging them `LazyFree` (Linux's `MADV_FREE`
/// bookkeeping), not just an internal accounting change.
#[test]
fn decay_rate_zero_marks_pages_lazyfree_after_free() {
    let _g = tunable_lock();
    let _restore = DecayRateGuard::new();

    // Force the immediate-decay backend path.
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

    // Actually fault in every page: a fresh mmap-backed allocation is
    // often lazily committed, and skipping this could leave pages
    // never resident in the first place, making the later "did it
    // get marked LazyFree" signal meaningless (nothing was resident
    // to mark). Write one byte per page (4 KiB stride is conservative
    // for every common page size) so the allocation is genuinely
    // resident before we free it.
    unsafe {
        let mut offset = 0usize;
        while offset < LARGE_ALLOC_SIZE {
            ptr.add(offset).write_volatile(0xAA);
            offset += 4096;
        }
        // Also touch the very last byte in case the size isn't an
        // exact multiple of the stride.
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

    // `MADV_FREE` (what `notify_not_using` issues on Linux, see the
    // module doc) does not evict pages from the resident set on its
    // own -- it only tags them reclaimable -- so RSS must NOT be
    // expected to drop here. RSS is otherwise not load-bearing for
    // this test: freeing can still cause small amounts of *new*
    // resident memory (backend metadata/pagemap bookkeeping touched
    // during the free), so we don't assert `rss_after <= rss_peak`.
    // It is captured only for diagnostics in the failure message
    // below.
    let _ = rss_after;

    // The load-bearing assertion: the kernel must have tagged
    // (approximately) the freed allocation's worth of pages as
    // `LazyFree` immediately after `dealloc`. We don't assert
    // byte-exact equality -- other allocator/runtime bookkeeping
    // (metadata pages, TLS, background allocations from the test
    // harness itself, and other chunks already cached from prior
    // process activity) mean the delta need not exactly equal
    // `LARGE_ALLOC_SIZE`. Instead we assert that at least 60% of our
    // allocation's size was newly marked `LazyFree`, which is a real
    // but not overly strict margin: comfortably clears normal
    // process-memory noise while still failing hard if immediate
    // decay regresses to "never calls notify_not_using".
    let lazyfree_delta = lazyfree_after.saturating_sub(lazyfree_before);
    let observed_fraction =
        lazyfree_delta as f64 / LARGE_ALLOC_SIZE as f64;

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
