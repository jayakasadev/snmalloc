//! Shared `/proc/self/smaps_rollup` reading helpers for the decay-rate
//! tests (`decay_rate_immediate.rs` and `decay_rate_windowed.rs`).
//!
//! Factored out here rather than duplicated: both files need the exact
//! same "read a `kB`-suffixed field out of `smaps_rollup`" logic and
//! the exact same reasoning for *why* `LazyFree` (not `VmRSS`) is the
//! correct signal for `MADV_FREE`-based decay, and letting that drift
//! into two independently-maintained copies risks the reasoning going
//! stale in one file but not the other.
//!
//! This lives at `tests/decay_smaps/mod.rs` (a `mod`-style submodule
//! path, `#[path]`-free) rather than `tests/decay_smaps.rs` on
//! purpose: cargo auto-discovers every top-level file directly under
//! `tests/` as its own independent test-binary target, which would
//! turn this helper module into a (test-less, harmless but noisy)
//! extra binary in `cargo test`'s output. A subdirectory with a
//! `mod.rs` is the standard idiom for code shared between integration
//! test binaries without cargo trying to build it as one itself; each
//! consumer pulls it in with `mod decay_smaps;` (which resolves to
//! `decay_smaps/mod.rs` relative to that test binary's crate root,
//! i.e. the `tests/*.rs` file itself).
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
//! Asserting on `VmRSS` would therefore fail even when the backend is
//! behaving exactly as designed.
//!
//! The actually-correct, immediately-observable signal for
//! `MADV_FREE` is the `LazyFree:` field of `/proc/self/smaps_rollup`
//! (also surfaced per-mapping in `/proc/self/smaps`): the kernel tags
//! `MADV_FREE`d-but-still-resident pages there the instant the
//! `madvise` call returns. This is the real, kernel-visible proof that
//! snmalloc issued the decay `madvise` at all, without waiting on
//! memory pressure or racing a kernel version's choice of `MADV_FREE`
//! vs `MADV_DONTNEED`.

use std::fs;

/// Read one `kB`-suffixed field out of `/proc/self/smaps_rollup`,
/// e.g. `field_kb("LazyFree:")` or `field_kb("Rss:")`. Lines look
/// like:
///
/// ```text
/// LazyFree:              0 kB
/// ```
///
/// `smaps_rollup` aggregates across every mapping in the process, so
/// this is a single cheap read rather than summing per-VMA entries out
/// of the (much larger) `/proc/self/smaps`.
pub fn read_smaps_rollup_field_kb(field: &str) -> u64 {
    let text = fs::read_to_string("/proc/self/smaps_rollup")
        .expect("failed to read /proc/self/smaps_rollup");
    for line in text.lines() {
        if let Some(rest) = line.strip_prefix(field) {
            return rest
                .trim()
                .trim_end_matches("kB")
                .trim()
                .parse()
                .unwrap_or_else(|e| panic!("failed to parse {field} value from {rest:?}: {e}"));
        }
    }
    panic!("{field} line not found in /proc/self/smaps_rollup");
}

/// Bytes currently tagged `MADV_FREE`-but-still-resident, aggregated
/// across the whole process.
pub fn read_lazyfree_bytes() -> u64 {
    read_smaps_rollup_field_kb("LazyFree:") * 1024
}

/// Total resident set size, aggregated across the whole process. Used
/// only as a monotonicity sanity check by callers -- see the module
/// doc for why `LazyFree` (not a `Rss` drop) is the correct signal for
/// `MADV_FREE`-based decay.
pub fn read_rss_bytes() -> u64 {
    read_smaps_rollup_field_kb("Rss:") * 1024
}
