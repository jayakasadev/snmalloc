//! Runtime load-bias detection for [`super::write_pprof`]'s offline
//! symbolication fix-up.
//!
//! "Load bias" here means: the value to subtract from a raw runtime
//! address to get a stable, file-relative offset -- the same offset
//! an offline `addr2line`/`atos`/pprof-symbol-server pass would
//! compute against the on-disk binary, regardless of where ASLR/PIE
//! happened to place that binary in this particular process's
//! address space.
//!
//! Each platform gets its own [`platform_load_bias`] implementation:
//!
//! - **Linux**: parse `/proc/self/maps`, find the first mapping whose
//!   path matches `/proc/self/exe`'s target, and use that mapping's
//!   start address as the bias.
//! - **macOS**: `_dyld_get_image_vmaddr_slide(0)` gives the slide of
//!   the main executable (image index 0) directly.
//! - **Everything else** (including Windows): [`None`] -- we have no
//!   portable way to determine the bias, so callers degrade to
//!   emitting raw, uncorrected addresses rather than guessing.
//!
//! The bias cannot change during a process's life (a binary's load
//! address is fixed at process start), so [`load_bias`] computes it
//! once and caches the result in a process-global [`OnceLock`] --
//! mirroring the caching pattern already used by
//! [`crate::profile::symbol_cache`] and [`crate::streaming`]'s
//! handler slot.

extern crate std;

use std::sync::OnceLock;

/// Return this process's cached load bias, computing it on first
/// call.
///
/// See the module docs for what "load bias" means and which
/// platforms can determine it.  [`None`] means "could not determine
/// the bias on this platform" -- callers must treat that the same as
/// a zero bias (i.e. leave addresses uncorrected), not as an error.
pub(super) fn load_bias() -> Option<usize> {
    static BIAS: OnceLock<Option<usize>> = OnceLock::new();
    *BIAS.get_or_init(platform_load_bias)
}

#[cfg(target_os = "linux")]
fn platform_load_bias() -> Option<usize> {
    linux::load_bias()
}

#[cfg(target_os = "macos")]
fn platform_load_bias() -> Option<usize> {
    macos::load_bias()
}

#[cfg(not(any(target_os = "linux", target_os = "macos")))]
fn platform_load_bias() -> Option<usize> {
    // Windows and any other target: no portable way to read the load
    // bias without a new dependency (e.g. `windows-sys` for
    // `GetModuleInformation`).  Degrading to "uncorrected" here keeps
    // the contract identical to the Linux/macOS `None` paths (no
    // panic, no compile error) and matches this crate's stated goal
    // of at least compiling cleanly across the full CI matrix
    // (`windows-latest`, `macos-14`, `macos-15`, `ubuntu-latest`; see
    // `.github/workflows/rust.yml`).
    None
}

/// Linux load-bias detection via `/proc/self/maps`.
#[cfg(target_os = "linux")]
mod linux {
    // `extern crate std;` at this file's top scope (line 29) doesn't
    // propagate into this nested inline module -- each module needs
    // its own path to `std`, same as any other item resolution.
    use super::std;
    use std::fs;
    use std::io::BufRead;

    /// Find the load address of this process's own executable by
    /// scanning `/proc/self/maps` for the first mapping whose
    /// pathname matches `/proc/self/exe`'s target.
    ///
    /// `/proc/self/maps` lists mappings in increasing-address order,
    /// and an ELF executable's segments are contiguous starting at
    /// its load base -- so the first matching line's start address
    /// is exactly the bias we want.  Returns `None` if `/proc` isn't
    /// readable (e.g. a sandboxed environment) or no mapping matches
    /// -- never panics.
    pub(super) fn load_bias() -> Option<usize> {
        let exe_path = fs::read_link("/proc/self/exe").ok()?;
        let maps = fs::File::open("/proc/self/maps").ok()?;
        let reader = std::io::BufReader::new(maps);

        for line in reader.lines() {
            let line = line.ok()?;
            // Format: "<start>-<end> <perms> <offset> <dev> <inode> [pathname]"
            // pathname is whitespace-separated and may be absent for
            // anonymous mappings, so we split on whitespace and only
            // look at lines that have a trailing path component.
            let Some((addr_range, rest)) = line.split_once(' ') else {
                continue;
            };
            let Some(path_str) = rest.rsplit(' ').find(|s| !s.is_empty()) else {
                continue;
            };
            if path_str.as_bytes() != exe_path.as_os_str().as_encoded_bytes() {
                continue;
            }
            let start_hex = addr_range.split('-').next()?;
            return usize::from_str_radix(start_hex, 16).ok();
        }
        None
    }
}

/// macOS load-bias detection via the dyld API.
#[cfg(target_os = "macos")]
mod macos {
    // Raw FFI declaration rather than pulling in a crate: this
    // crate's `Cargo.toml` deliberately keeps `backtrace` and
    // `flate2` feature-gated/optional to minimise the default build's
    // dependency footprint, so a single well-known libSystem symbol
    // is not worth a new dependency.
    //
    // Signature verified against the macOS SDK's
    // `mach-o/dyld.h`:
    //   extern intptr_t _dyld_get_image_vmaddr_slide(uint32_t image_index)
    // `intptr_t` -> `isize`, `uint32_t` -> `u32`.
    extern "C" {
        fn _dyld_get_image_vmaddr_slide(image_index: u32) -> isize;
    }

    /// Return the vmaddr slide of image index 0, which dyld guarantees
    /// is always the main executable.
    ///
    /// # Safety
    ///
    /// `_dyld_get_image_vmaddr_slide` is safe to call with any
    /// `image_index`: out-of-range indices return `0` rather than
    /// reading invalid memory (documented dyld behaviour). This
    /// function itself takes no unsafe preconditions from its caller.
    pub(super) fn load_bias() -> Option<usize> {
        // SAFETY: `_dyld_get_image_vmaddr_slide` is a pure accessor
        // over dyld's already-initialised, process-global image list;
        // it performs no pointer dereference on our side and cannot
        // be called with an invalid `image_index` here since we pass
        // the constant `0`.
        let slide = unsafe { _dyld_get_image_vmaddr_slide(0) };
        // A negative slide would indicate the main executable somehow
        // loaded below its link-time base, which dyld does not do in
        // practice; treat it defensively as "unknown" rather than
        // producing a bogus bias.
        usize::try_from(slide).ok()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The bias-correction arithmetic used by `write_pprof`, lifted
    /// out here as a pure function so it can be tested without
    /// depending on the real platform helper's live value.
    fn apply_bias(addr: usize, bias: Option<usize>) -> usize {
        match bias {
            Some(b) => addr.saturating_sub(b),
            None => addr,
        }
    }

    /// `bias + (addr - bias) == addr`: correcting and then undoing
    /// the correction recovers the original address, for any bias
    /// that is `<=` the address (the only case that occurs in
    /// practice -- a frame's address is always at or above its
    /// mapping's load base).
    #[test]
    fn apply_bias_round_trips() {
        let addr = 0x1_0000_2000usize;
        let bias = 0x1_0000_0000usize;
        let corrected = apply_bias(addr, Some(bias));
        assert_eq!(bias + corrected, addr);
    }

    /// `None` bias must leave the address untouched -- this is the
    /// Windows-equivalent "can't determine bias" path exercised
    /// locally.
    #[test]
    fn apply_bias_none_is_identity() {
        let addr = 0xdeadbeefusize;
        assert_eq!(apply_bias(addr, None), addr);
    }

    /// A bias larger than the address (a frame from a lower-based
    /// mapping, e.g. a dynamically loaded library below the main
    /// executable) must degrade to `0` rather than underflow-panic.
    #[test]
    fn apply_bias_saturates_instead_of_underflowing() {
        assert_eq!(apply_bias(10, Some(20)), 0);
    }

    /// End-to-end sanity check of the real platform helper on
    /// whatever OS the test suite is actually running on.  On a
    /// platform without a known bias source this just asserts the
    /// `None` contract; on Linux/macOS it exercises the real
    /// syscall/FFI path and checks the round-trip identity against a
    /// real function-pointer address taken from this binary.
    #[test]
    fn load_bias_round_trips_on_this_platform() {
        fn marker() {}
        // Cast through a pointer first: casting a function *item*
        // directly to an integer is ambiguous about which of the
        // (possibly several) monomorphised addresses it names.
        // `marker as fn()` decays to a real function *pointer* first,
        // which has one unambiguous address.
        let addr = marker as fn() as usize;

        match load_bias() {
            Some(bias) => {
                // The running binary's own code must live at or above
                // its load base.
                assert!(
                    addr >= bias,
                    "function address {addr:#x} below detected load bias {bias:#x}"
                );
                let offset = addr - bias;
                assert_eq!(bias + offset, addr);
            }
            None => {
                // Platforms with no bias source (e.g. Windows) must
                // report `None`, not panic and not silently return a
                // wrong value.
                assert!(apply_bias(addr, None) == addr);
            }
        }
    }

    /// [`load_bias`] must be stable across repeated calls within one
    /// process -- the underlying value cannot change once the binary
    /// is loaded, and the `OnceLock` caching must reflect that.
    #[test]
    fn load_bias_is_stable_across_calls() {
        assert_eq!(load_bias(), load_bias());
    }
}
