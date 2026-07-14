//! Integration test for the `decay_rate_ms() > 0` "time-windowed decay"
//! backend read-side.
//!
//! Background: this is the sibling of `decay_rate_immediate.rs`, which
//! covers `decay_rate_ms() == 0`. This file covers the other branch:
//! `LargeBuddyRange::Type::maybe_sweep_decay()` (in
//! `src/snmalloc/backend_helpers/largebuddyrange.h`).
//!
//! ## The mechanism under test (read the C++ source before touching
//! ## this file -- summarised here only for the reader's convenience)
//!
//! When a freed chunk lands in a `LargeBuddyRange`'s own cache/tree
//! (i.e. `add_block` does NOT consolidate it all the way up to a block
//! big enough to hand back to the parent range) and `decay_rate_ms()`
//! is non-zero, `dealloc_range` calls `touch_bucket(idx)`, which
//! records `PAL::time_in_ms()` into `last_touched_ms[idx]`, where `idx`
//! is the chunk's bucket (`log2(size) - MIN_CHUNK_BITS`).
//!
//! **There is no background thread or timer callback.** Both
//! `alloc_range` and `dealloc_range` call `maybe_sweep_decay()` at
//! their tail. That function does a single cheap `now <
//! next_sweep_due_ms` check and bails out immediately unless a full
//! `decay_rate_ms()` window has elapsed since the last sweep. When due,
//! it walks *every* bucket (not just the one most recently touched),
//! and for each bucket whose `last_touched_ms[idx]` is older than
//! `decay_rate_ms()`, it calls `PAL::notify_not_using` (== `madvise`
//! with `MADV_FREE` on Linux, same primitive `decay_rate_immediate.rs`
//! already exercises for the `== 0` case) on every chunk cached in that
//! bucket, then reschedules `next_sweep_due_ms = now + decay_ms`.
//!
//! Consequently, sleeping past the decay window and then immediately
//! re-checking OS state observes *nothing*: no code re-enters
//! `alloc_range`/`dealloc_range` during the sleep, so
//! `maybe_sweep_decay()` never actually runs. Some additional backend
//! chunk-granularity alloc/dealloc activity is required *after* the
//! sleep to give the opportunistic check a chance to fire.
//!
//! That activity must not touch the same bucket we're trying to
//! observe: `alloc_range`'s cache-hit path calls
//! `DefaultPal::notify_using` (clearing any decommitted flag) on
//! whatever chunk it hands out, so allocating another object of the
//! *same* size class right after the sweep could immediately recommit
//! (and thus mask) the very evidence we're trying to check. This test
//! sidesteps that by allocating+freeing a *different* size class
//! (`SWEEP_TRIGGER_ALLOC_SIZE`, a different bucket index than
//! `OBSERVED_ALLOC_SIZE`) purely to give `maybe_sweep_decay()` a call
//! site to run from; the sweep itself walks every bucket, including the
//! one `OBSERVED_ALLOC_SIZE`'s chunk sits in, as a side effect.
//!
//! ## Which `LargeBuddyRange` instance this test actually needs to hit
//!
//! This is the part that is easy to get wrong, and got this test's
//! first draft wrong: there is more than one live `LargeBuddyRange`
//! instance, and only *one* of them ever runs either decay policy.
//! `standard_range.h`'s `StandardLocalState` (the default backend
//! config) wires up:
//!
//! - `GlobalR` -- the true process-wide singleton, instantiated as
//!   `LargeBuddyRange<GlobalCacheSizeBits=24, bits::BITS-1, ...,
//!   MANAGES_COMMITTED_MEMORY=false>`. `MANAGES_COMMITTED_MEMORY=false`
//!   makes `touch_bucket`/`maybe_sweep_decay` compile away to no-ops
//!   for this instance entirely -- it caches reserved-but-uncommitted
//!   address space, and calling `PAL::notify_not_using` on that is
//!   unsafe (see the long comment on `MANAGES_COMMITTED_MEMORY` in
//!   `largebuddyrange.h`, which documents a real `SIGBUS` this was
//!   observed to cause during development).
//! - `Stats = Pipe<GlobalR, CommitRange<PAL>, StatsRange>`. `Pipe`
//!   composes back-to-front (the *last* type argument is outermost, so
//!   calls flow `StatsRange -> CommitRange -> GlobalR`); critically,
//!   `CommitRange::dealloc_range` calls `PAL::notify_not_using`
//!   *unconditionally*, on every dealloc that reaches it, with no
//!   `decay_rate_ms()` check at all.
//! - `LargeObjectRange = Pipe<Stats, StaticConditionalRange<
//!   LargeBuddyRange<LocalCacheSizeBits=21, LocalCacheSizeBits=21, ...>
//!   >>` -- the **per-thread** local large-object cache, with the
//!   default `MANAGES_COMMITTED_MEMORY=true`. This is the *only*
//!   instance in the default config where either decay policy is
//!   live. `get_object_range()->alloc_range`/`dealloc_range` (called
//!   from `Backend::alloc_chunk`/`dealloc_chunk` in `backend.h`) route
//!   here first.
//!
//! `LargeBuddyRange<21, 21, ...>::alloc_range`/`dealloc_range` bail out
//! to their parent (`Stats`, i.e. straight into `CommitRange`'s
//! unconditional decommit) whenever `size >= bits::mask_bits(21)`
//! (`mask_bits(n) = (1 << n) - 1`, so effectively `size >= 2 MiB`).  A
//! request that rounds (via `large_size_to_chunk_size` ==
//! `bits::next_pow2`) to a chunk of 2 MiB or larger **never reaches the
//! decay-conditional code at all** -- it is unconditionally decommitted
//! by `CommitRange` regardless of `decay_rate_ms()`, every time, decay
//! feature or not. A first draft of this test used a 12 MiB allocation
//! (mirroring `decay_rate_immediate.rs::LARGE_ALLOC_SIZE`, which rounds
//! to a 16 MiB chunk) and would have "passed" for the wrong reason:
//! `CommitRange` would have marked it `LazyFree` immediately on free
//! regardless of the decay-rate setting, defeating both the windowed
//! assertion and the negative/contrast check below.
//!
//! To actually exercise `LargeBuddyRange<21,21>`'s own
//! `touch_bucket`/`maybe_sweep_decay`, both `OBSERVED_ALLOC_SIZE` and
//! `SWEEP_TRIGGER_ALLOC_SIZE` below are chosen so their chunk size
//! (`next_pow2` of the request) is *strictly less than* 2 MiB -- see
//! each constant's own doc for the exact numbers -- while still being
//! "large" objects per `MAX_SMALL_SIZECLASS_SIZE` (64 KiB on the
//! default build) so they actually reach `Backend::alloc_chunk` /
//! `get_object_range()` in the first place, rather than being absorbed
//! by the small-object frontend slab allocator, which never calls into
//! this backend range at all.
//!
//! (Whether `decay_rate_immediate.rs`'s own 12 MiB allocation is
//! actually exercising the `decay_rate_ms() == 0` branch in
//! `dealloc_range`, as opposed to also incidentally landing on
//! `CommitRange`'s unconditional decommit, is a fair question raised
//! by the above -- but out of scope to fix here; see this file's
//! accompanying report for that finding. This file does not modify
//! `decay_rate_immediate.rs`'s test logic, only extracts the shared
//! `smaps_rollup`-reading helpers.)
//!
//! ## Why this measures `LazyFree`, not `VmRSS`
//!
//! See the equivalent section in `decay_rate_immediate.rs` -- the
//! reasoning is identical (`MADV_FREE` is a lazy hint; `VmRSS` does not
//! drop on its own, but `/proc/self/smaps_rollup`'s `LazyFree:` field is
//! updated the instant the kernel tags the pages). This file reuses
//! those exact helpers rather than duplicating the reasoning -- see
//! `decay_smaps/mod.rs`.
//!
//! Linux-only, for the same reasons as `decay_rate_immediate.rs`:
//! `/proc/self/smaps_rollup` is a Linux-specific interface, and
//! `MADV_FREE` is a Linux PAL detail.
//!
//! Isolation: like `decay_rate_immediate.rs` and `memory_stats.rs`, this
//! measures process-global resources (smaps) that other allocating
//! tests in the same binary could disturb, and depends on a
//! process-wide singleton tunable (`decay_rate`). Cargo runs each file
//! under `tests/` as its own binary/process, so this file's own
//! `#[test]` fns only need to serialise against *each other*
//! (`tunable_lock`), not against `decay_rate_immediate.rs`.
//!
//! There is a second, thread-scoped isolation subtlety specific to this
//! mechanism: `last_touched_ms`/`next_sweep_due_ms` live on the
//! per-thread `LargeObjectRange` instance (thread-local storage), not
//! on a process-wide singleton. Running this whole test body on a
//! single thread (which `#[test]` already does by default -- Rust does
//! not hop a test function across OS threads mid-body) is what makes
//! the "which thread's sweep state are we observing" question
//! unambiguous; it is the *same* thread's cache for the observed
//! allocation, the sleep, and the sweep-trigger allocations.
//!
//! Allocator-invocation style: same as `decay_rate_immediate.rs` --
//! drives `SnMalloc` directly via `SnMalloc::new()` + `GlobalAlloc`,
//! no `#[global_allocator]` needed.

#![cfg(target_os = "linux")]

mod decay_smaps;

use decay_smaps::read_lazyfree_bytes;
use snmalloc_rs::SnMalloc;
use std::alloc::{GlobalAlloc, Layout};
use std::sync::{Mutex, MutexGuard, OnceLock};
use std::time::Duration;

/// Serialise every test in this file against each other. Mirrors
/// `decay_rate_immediate.rs` / `runtime_tunables.rs`'s own
/// per-file locks -- `SnMalloc::set_decay_rate` is a process-wide
/// singleton (see `runtime_tunables.rs`), so two tests in this binary
/// racing on it could clobber each other's window mid-measurement.
fn tunable_lock() -> MutexGuard<'static, ()> {
    static LOCK: OnceLock<Mutex<()>> = OnceLock::new();
    LOCK.get_or_init(|| Mutex::new(()))
        .lock()
        .unwrap_or_else(|poison| poison.into_inner())
}

/// RAII restore-on-drop for the decay-rate tunable. Same pattern as
/// `decay_rate_immediate.rs`'s `DecayRateGuard` / `runtime_tunables.rs`'s
/// `TunableGuard`, scoped to just the one knob this file touches.
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

/// Size of the allocation whose decay we actually observe. Chosen so
/// that `bits::next_pow2(OBSERVED_ALLOC_SIZE)` (what
/// `large_size_to_chunk_size` turns the request into before it reaches
/// the backend, see `sizeclasstable.h`) lands *strictly below* the
/// per-thread `LargeBuddyRange<LocalCacheSizeBits=21, ...>`'s
/// `size >= bits::mask_bits(21)` (~2 MiB) bypass threshold -- see the
/// module doc's "Which `LargeBuddyRange` instance" section for why that
/// matters. 768 KiB rounds up to exactly `1 MiB == 2^20` (bucket
/// `idx = 20 - MIN_CHUNK_BITS(14) = 6`, the *last* in-range bucket for
/// this instance, since `MAX_SIZE_BITS - MIN_CHUNK_BITS = 21 - 14 = 7`
/// buckets total, indices 0..6). Still comfortably above
/// `MAX_SMALL_SIZECLASS_SIZE` (64 KiB on the default build), so this is
/// unambiguously routed as a "large" object through
/// `Backend::alloc_chunk`/`get_object_range()`, not absorbed by a
/// small-object slab that never touches this backend range at all.
const OBSERVED_ALLOC_SIZE: usize = 768 * 1024;

/// Second, *different* size class used purely to give
/// `maybe_sweep_decay()` a call site to run from after the sleep (see
/// the module doc). 100 KiB rounds up to `128 KiB == 2^17` (bucket
/// `idx = 17 - 14 = 3`), a distinct bucket three away from
/// `OBSERVED_ALLOC_SIZE`'s bucket 6, so that allocating/freeing this
/// cannot itself satisfy from, or recommit, `OBSERVED_ALLOC_SIZE`'s own
/// chunk. Still above `MAX_SMALL_SIZECLASS_SIZE` and, like
/// `OBSERVED_ALLOC_SIZE`, strictly below the `LargeBuddyRange<21,21>`
/// bypass threshold, so it round-trips through the *same* decay-capable
/// per-thread instance rather than a range that never calls
/// `maybe_sweep_decay` at all.
const SWEEP_TRIGGER_ALLOC_SIZE: usize = 100 * 1024;

/// Decay window used by this test. Must be:
///
/// - Short enough that sleeping past it doesn't make the test slow
///   (this sleeps past it once per test).
/// - Long enough that normal scheduler/timer jitter cannot make the
///   sleep finish *before* the window has actually elapsed from the
///   sweep's point of view, which would make the "should have decayed"
///   assertion flaky.
///
/// `decay_rate_immediate.rs` picked its own margin (60% of the
/// allocation size counted as `LazyFree`) empirically, to comfortably
/// clear normal process-memory noise while still catching a real
/// regression. This test needs an analogous margin, but on the *time*
/// axis rather than the *byte* axis: we set the window to 200ms and
/// sleep for 400ms (2x the window) before triggering the sweep. A
/// missed wakeup or scheduler delay of "merely" tens of milliseconds
/// (ordinary `thread::sleep` jitter under load, well within what CI
/// runners routinely exhibit) still leaves us comfortably past the
/// 200ms threshold; only a multi-hundred-millisecond stall would flip
/// this negative, which is not a "the test is flaky" condition so much
/// as "the machine is unable to schedule threads at all" -- consistent
/// with the spirit of the 60% margin chosen in
/// `decay_rate_immediate.rs`. 200ms (rather than something even
/// shorter like 20-50ms) also keeps us well clear of double-counting
/// any coarse OS scheduling tick, and keeps overall test wall time
/// trivial (well under half a second) so running this file 10+ times
/// in a row for a flakiness check, as this feature requires, is cheap.
const DECAY_WINDOW_MS: u32 = 200;

/// How long to sleep past the window before triggering the sweep. 2x
/// the window rather than e.g. 1.05x: gives comfortable margin above
/// jitter (see `DECAY_WINDOW_MS` doc) while still keeping the test
/// fast.
const SLEEP_PAST_WINDOW: Duration = Duration::from_millis(2 * DECAY_WINDOW_MS as u64);

/// Touch every page of `len` bytes starting at `ptr`, so the
/// allocation is genuinely resident (not just lazily-mapped-but-never-
/// faulted-in) before it is freed. Same reasoning and stride as
/// `decay_rate_immediate.rs`.
unsafe fn touch_all_pages(ptr: *mut u8, len: usize) {
    let mut offset = 0usize;
    while offset < len {
        ptr.add(offset).write_volatile(0xAA);
        offset += 4096;
    }
    ptr.add(len - 1).write_volatile(0xAA);
}

/// Allocate, touch, and free a `size`-byte block via `alloc`. Used only
/// for the sweep-trigger activity, where the specific pointer value
/// does not matter -- only that an `alloc_range`/`dealloc_range` pair
/// on this thread's `LargeObjectRange` happens after the sleep.
unsafe fn alloc_touch_and_free(alloc: &SnMalloc, size: usize) {
    let layout = Layout::from_size_align(size, 64).unwrap();
    let ptr = alloc.alloc(layout);
    assert!(
        !ptr.is_null(),
        "{size}-byte allocation must not return null"
    );
    touch_all_pages(ptr, size);
    alloc.dealloc(ptr, layout);
}

/// With `decay_rate_ms() > 0`, a large allocation whose backend chunk
/// lands in the per-thread `LargeBuddyRange`'s own cache/tree must be
/// left fully committed immediately after `dealloc` (unlike the
/// `decay_rate_ms() == 0` immediate-decay path covered by
/// `decay_rate_immediate.rs`), but must have its pages marked
/// `LazyFree` once (a) at least one full decay window has elapsed
/// since the free and (b) some subsequent backend chunk activity has
/// given `maybe_sweep_decay()` a chance to run.
#[test]
fn decay_rate_windowed_marks_pages_lazyfree_after_window_elapses() {
    let _g = tunable_lock();
    let _restore = DecayRateGuard::new();

    SnMalloc::set_decay_rate(DECAY_WINDOW_MS);
    assert_eq!(
        SnMalloc::decay_rate(),
        DECAY_WINDOW_MS,
        "decay rate must round-trip before we rely on the windowed \
         backend behaviour"
    );

    let alloc = SnMalloc::new();
    let layout = Layout::from_size_align(OBSERVED_ALLOC_SIZE, 64).unwrap();

    let lazyfree_before = read_lazyfree_bytes();

    let ptr = unsafe { alloc.alloc(layout) };
    assert!(!ptr.is_null(), "observed allocation must not return null");
    unsafe { touch_all_pages(ptr, OBSERVED_ALLOC_SIZE) };

    unsafe { alloc.dealloc(ptr, layout) };

    // --- Negative/contrast check ---------------------------------
    //
    // Immediately after the free (well before the decay window has
    // elapsed, and before any further backend activity has given
    // `maybe_sweep_decay()` a chance to run at all), this mechanism
    // must not yet have marked the pages `LazyFree`. Unlike
    // `decay_rate_immediate.rs`'s test, there is no race here worth
    // worrying about: nothing in this process calls
    // `alloc_range`/`dealloc_range` again between the `dealloc` above
    // and this read, so there is no code path that could have run
    // `maybe_sweep_decay` yet, let alone had a 200ms window elapse
    // during the handful of microseconds this takes. This makes the
    // "not yet" side of the check essentially non-flaky -- it is not
    // a tight timing race, it is "nothing happened, so nothing should
    // have changed" -- which is why it's safe to include unlike a
    // genuinely racy negative check.
    let lazyfree_immediately_after_free = read_lazyfree_bytes();
    let immediate_delta = lazyfree_immediately_after_free.saturating_sub(lazyfree_before);
    let immediate_fraction = immediate_delta as f64 / OBSERVED_ALLOC_SIZE as f64;
    assert!(
        immediate_fraction < 0.10,
        "with decay_rate={DECAY_WINDOW_MS}ms, freeing must NOT \
         immediately mark pages LazyFree (that is the decay_rate=0 \
         behaviour tested by decay_rate_immediate.rs); \
         lazyfree_before={lazyfree_before}, \
         lazyfree_immediately_after_free={lazyfree_immediately_after_free}, \
         delta={immediate_delta} ({:.1}% of the allocation size)",
        immediate_fraction * 100.0
    );

    // --- Sleep past the window ------------------------------------
    //
    // See `SLEEP_PAST_WINDOW`'s doc for the margin rationale. No
    // backend call happens during this sleep, so nothing sweeps yet
    // even though the window has now elapsed -- see the module doc.
    std::thread::sleep(SLEEP_PAST_WINDOW);

    // --- Trigger the sweep via a *different* size class -----------
    //
    // This must not reuse or recommit OBSERVED_ALLOC_SIZE's own bucket
    // (idx 6) -- see SWEEP_TRIGGER_ALLOC_SIZE's doc for why bucket 3
    // is a safe choice. A few rounds, not just one, to make sure at
    // least one `alloc_range`/`dealloc_range` call happens strictly
    // after the sleep above and observes `now >= next_sweep_due_ms`.
    for _ in 0..4 {
        unsafe { alloc_touch_and_free(&alloc, SWEEP_TRIGGER_ALLOC_SIZE) };
    }

    let lazyfree_after_sweep = read_lazyfree_bytes();

    // The load-bearing assertion: after the window has elapsed and
    // the sweep has had a chance to run, (approximately) our
    // allocation's worth of pages must now be tagged LazyFree. Same
    // 60% margin as `decay_rate_immediate.rs`, for the same reason:
    // other allocator/runtime bookkeeping (metadata pages, the
    // sweep-trigger allocations' own bookkeeping, prior process
    // activity) means the delta need not exactly equal
    // `OBSERVED_ALLOC_SIZE`, but 60% comfortably clears that noise
    // while still failing hard if the windowed sweep regresses to
    // "never calls notify_not_using".
    let sweep_delta = lazyfree_after_sweep.saturating_sub(lazyfree_before);
    let sweep_fraction = sweep_delta as f64 / OBSERVED_ALLOC_SIZE as f64;

    assert!(
        sweep_fraction >= 0.60,
        "expected decay_rate={DECAY_WINDOW_MS}ms to mark most of the \
         freed {OBSERVED_ALLOC_SIZE}-byte allocation LazyFree once the \
         decay window elapsed and the sweep ran; \
         lazyfree_before={lazyfree_before}, \
         lazyfree_immediately_after_free={lazyfree_immediately_after_free}, \
         lazyfree_after_sweep={lazyfree_after_sweep}, \
         delta={sweep_delta} ({:.1}% of the allocation size)",
        sweep_fraction * 100.0
    );
}
