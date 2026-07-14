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

/// Pure parsing over an already-read Linux `/proc/self/maps` file.
///
/// Deliberately NOT behind `#[cfg(target_os = "linux")]`: it has no
/// platform dependency (it parses a `&str`, never touches a real
/// filesystem), and leaving it unconditional means it actually gets
/// compiled and unit-tested on every CI platform this crate builds
/// on -- macOS and Windows included -- rather than only on Linux,
/// where the previous version of this logic lived un-exercised until
/// a downstream consumer's first-ever Linux build caught a compile
/// error in this very file (see this file's `linux` submodule).
///
/// Format per line: `<start>-<end> <perms> <offset> <dev> <inode>
/// [pathname]`. Splits on the first 5 whitespace runs to isolate the
/// fixed-width fields, then treats everything remaining (after
/// trimming the leading padding whitespace between `<inode>` and the
/// pathname) as the path verbatim -- crucially, NOT split further on
/// whitespace. A real Linux pathname can legally contain spaces, and
/// the kernel appends a literal `" (deleted)"` suffix (itself
/// containing a space) when the mapped file has been unlinked since
/// exec -- both cases have a space inside what is still logically a
/// single path field. Splitting again on whitespace after this point
/// (an earlier version of this function took only the last such
/// token) truncates the path and silently fails to match, degrading
/// to an uncorrected bias instead of a wrong one -- not a crash, but
/// a real precision gap this function exists specifically to avoid.
///
/// `allow(dead_code)`: only `linux::load_bias()` calls this outside of
/// tests, so on any non-Linux build it's genuinely unreferenced by
/// production code -- that's the intended tradeoff for keeping it
/// testable everywhere (see the doc comment above).
#[allow(dead_code)]
fn find_load_base(maps: &str, exe_path: &[u8]) -> Option<usize> {
    for line in maps.lines() {
        let mut fields = line.splitn(6, char::is_whitespace);
        let addr_range = fields.next()?;
        let _perms = fields.next()?;
        let _offset = fields.next()?;
        let _dev = fields.next()?;
        let _inode = fields.next()?;
        let path = fields.next().map(str::trim).unwrap_or("");
        if path.is_empty() || path.as_bytes() != exe_path {
            continue;
        }
        let start_hex = addr_range.split('-').next()?;
        return usize::from_str_radix(start_hex, 16).ok();
    }
    None
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

    /// Find the load address of this process's own executable by
    /// scanning `/proc/self/maps` for the first mapping whose
    /// pathname matches `/proc/self/exe`'s target.
    ///
    /// `/proc/self/maps` lists mappings in increasing-address order,
    /// and an ELF executable's segments are contiguous starting at
    /// its load base -- so the first matching line's start address
    /// is exactly the bias we want.  Returns `None` if `/proc` isn't
    /// readable (e.g. a sandboxed environment) or no mapping matches
    /// -- never panics. Parsing itself lives in the parent module's
    /// `find_load_base`, which has no platform dependency and is
    /// unit-tested unconditionally there.
    pub(super) fn load_bias() -> Option<usize> {
        let exe_path = fs::read_link("/proc/self/exe").ok()?;
        let maps = fs::read_to_string("/proc/self/maps").ok()?;
        super::find_load_base(&maps, exe_path.as_os_str().as_encoded_bytes())
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
    // `use super::*` only brings the `std` crate-root binding itself
    // into scope (from the file-top `extern crate std;`), not its own
    // sub-items -- same propagation gap this file's `linux` submodule
    // hit, just one level removed. `String`/`format!` need their own
    // explicit imports here.
    use std::format;
    use std::string::String;

    /// A realistic `/proc/self/maps` fixture: header/rodata/heap/anon
    /// mappings around one line for `exe_path`, matching the real
    /// field layout (multiple padding spaces before the pathname).
    fn maps_fixture(exe_path: &str) -> String {
        format!(
            "555555554000-555555556000 r--p 00000000 08:01 1234       {exe_path}\n\
             555555556000-555555558000 r-xp 00002000 08:01 1234       {exe_path}\n\
             7ffff7c00000-7ffff7c22000 r--p 00000000 08:01 5678       /usr/lib/libc.so.6\n\
             7ffff7fff000-7ffff8000000 rw-p 00000000 00:00 0          \n\
             ffffffffff600000-ffffffffff601000 --xp 00000000 00:00 0  [vdso]\n"
        )
    }

    /// The normal case: no spaces, no deletion -- exercised first so a
    /// regression in the basic path shows up independently of the
    /// adversarial cases below.
    #[test]
    fn find_load_base_matches_normal_path() {
        let maps = maps_fixture("/usr/bin/myapp");
        let base = find_load_base(&maps, b"/usr/bin/myapp");
        assert_eq!(base, Some(0x555555554000));
    }

    /// A pathname containing a literal space is legal on Linux (e.g. a
    /// mount point named with a space). An earlier version of this
    /// parser took only the LAST whitespace-delimited token of the
    /// line, which silently truncated a spaced path down to its final
    /// word and never matched -- this is the regression test for that.
    #[test]
    fn find_load_base_handles_path_with_space() {
        let maps = maps_fixture("/mnt/My Files/myapp");
        let base = find_load_base(&maps, b"/mnt/My Files/myapp");
        assert_eq!(base, Some(0x555555554000));
    }

    /// The kernel appends a literal `" (deleted)"` suffix to a mapped
    /// file's path once the backing inode is unlinked (a real,
    /// not-uncommon pattern -- e.g. a container overlay removing the
    /// binary after copying it in). That suffix itself contains a
    /// space, so it's really the same underlying bug as the
    /// spaced-path case above, not a separate one: both sides
    /// (`/proc/self/maps`'s pathname and `/proc/self/exe`'s readlink
    /// target) get the identical suffix in this situation, so they
    /// still byte-match as long as the parser doesn't truncate either
    /// one on an internal space.
    #[test]
    fn find_load_base_handles_deleted_suffix() {
        let maps = maps_fixture("/usr/bin/myapp (deleted)");
        let base = find_load_base(&maps, b"/usr/bin/myapp (deleted)");
        assert_eq!(base, Some(0x555555554000));
    }

    /// An anonymous mapping (no pathname field at all) must be
    /// skipped, not mistaken for a match against an empty `exe_path`
    /// or cause a panic on a missing field.
    #[test]
    fn find_load_base_skips_anonymous_mappings() {
        let maps = maps_fixture("/usr/bin/myapp");
        // exe_path that appears nowhere in the fixture -- every real
        // line must fail to match, including the anonymous ones,
        // landing on the final `None`.
        assert_eq!(find_load_base(&maps, b"/no/such/binary"), None);
    }

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
