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
//! See `decay_smaps` (this test's helper module, shared with the
//! sibling `decay_rate_windowed.rs`) for the full reasoning on why
//! `MADV_FREE` requires reading `/proc/self/smaps_rollup`'s
//! `LazyFree:` field rather than `VmRSS`. In short: `MADV_FREE`
//! (what `notify_not_using` issues on Linux) is a lazy hint that does
//! not evict pages from the resident set on its own, so `VmRSS` would
//! not move even when the backend behaves exactly as designed, while
//! `LazyFree:` is updated the instant the kernel processes the
//! `madvise` call.
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
//! Isolation and allocator-invocation style: same rationale as
//! `memory_stats.rs` -- this measures process-global resources (RSS /
//! smaps) that other allocating tests could disturb, hence its own
//! file, and drives `SnMalloc` directly rather than installing
//! `#[global_allocator]`; see `memory_stats.rs`'s doc comment for the
//! full reasoning.

#![cfg(target_os = "linux")]

mod decay_smaps;

use decay_smaps::{read_lazyfree_bytes, read_rss_bytes};
use snmalloc_rs::SnMalloc;
use std::alloc::{GlobalAlloc, Layout};
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

/// Large-object size, well above snmalloc's small/large sizeclass
/// boundary (`MIN_CHUNK_SIZE` == `1 << 14` == 16 KiB on the default
/// build) so this allocation is guaranteed to route through a backend
/// `LargeBuddyRange`, not a per-thread small-object slab cache --
/// but, critically, *below* `LocalCacheSizeBits` (2 MiB,
/// `src/snmalloc/backend/base_constants.h`), the threshold at which
/// `LargeObjectRange`'s per-thread `LargeBuddyRange<21, 21, ...>`
/// (`src/snmalloc/backend/standard_range.h`) bypasses its own
/// `buddy_large` entirely and forwards straight to `Stats` ->
/// `CommitRange<PAL>`, which unconditionally decommits on every
/// `dealloc_range` regardless of `decay_rate_ms()` -- a 12 MiB
/// allocation (this constant's original value) lands in that bypass
/// path and would observe the exact same `LazyFree` behaviour with
/// none of this feature's code compiled in at all, silently testing
/// nothing. 768 KiB rounds up to a 1 MiB chunk (`bits::next_pow2_bits`),
/// comfortably under the 2 MiB bypass threshold, so the free actually
/// reaches `buddy_large.add_block()` on the *per-thread*
/// `LargeBuddyRange` and exercises the `decay_rate_ms() == 0` branch
/// in `dealloc_range` this test exists to prove.
const LARGE_ALLOC_SIZE: usize = 768 * 1024;

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
