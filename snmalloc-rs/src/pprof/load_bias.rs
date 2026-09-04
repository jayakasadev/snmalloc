//! Runtime load-bias detection for [`super::write_pprof`]'s offline
//! symbolication fix-up.
//!
//! "Load bias" here means: the value to subtract from a raw runtime
//! address to get a stable, file-relative offset — the same offset
//! an offline `addr2line`/`atos`/pprof-symbol-server pass would
//! compute against the on-disk binary, regardless of where ASLR/PIE
//! happened to place that binary in this particular process's
//! address space.
//!
//! Each platform gets its own [`platform_load_bias`] implementation:
//!
//! - **Linux**: parse `/proc/self/maps`, find the first mapping whose
//!  path matches `/proc/self/exe`'s target, and use that mapping's
//!  start address as the bias.
//! - **macOS**: `_dyld_get_image_vmaddr_slide(0)` gives the slide of
//!  the main executable (image index 0) directly.
//! - **Everything else** (including Windows): [`None`] — we have no
//!  portable way to determine the bias, so callers degrade to
//!  emitting raw, uncorrected addresses rather than guessing.
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
/// platforms can determine it. [`None`] means "could not determine
/// the bias on this platform" — callers must treat that the same as
/// a zero bias (i.e. leave addresses uncorrected), not as an error.
pub(super) fn load_bias() -> Option<usize> {
    static BIAS: OnceLock<Option<usize>> = OnceLock::new();
    *BIAS.get_or_init(platform_load_bias)
}

/// Pure parsing over an already-read Linux `/proc/self/maps` file.
///
/// Deliberately NOT behind `#[cfg(target_os = "linux")]`: it has no
/// platform dependency (it parses a `&str`, never touches a real
/// filesystem), and leaving it unconditional means it gets
/// compiled and unit-tested on every CI platform this crate builds
/// on — macOS and Windows included — rather than only on Linux,
/// a downstream consumer's first-ever Linux build caught a compile
/// error in this very file (see this file's `linux` submodule).
///
/// Parse `<start>-<end> <perms> <offset> <dev> <inode> [pathname]`.
///
/// The pathname remains whole, including spaces or `" (deleted)"`.
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
    None
}

/// Linux load-bias detection via `/proc/self/maps`.
#[cfg(target_os = "linux")]
mod linux {
    use super::std;
    use std::fs;

    /// Find the load address of this process's own executable by
    /// scanning `/proc/self/maps` for the first mapping whose
    /// pathname matches `/proc/self/exe`'s target.
    ///
    /// `/proc/self/maps` lists mappings in increasing-address order,
    /// and an ELF executable's segments are contiguous starting at
    /// its load base — so the first matching line's start address
    /// is exactly the bias we want. Returns `None` if `/proc` isn't
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
        usize::try_from(slide).ok()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::format;
    use std::string::String;

    fn maps_fixture(exe_path: &str) -> String {
        format!(
            "555555554000-555555556000 r--p 00000000 08:01 1234       {exe_path}\n\
             555555556000-555555558000 r-xp 00002000 08:01 1234       {exe_path}\n\
             7ffff7c00000-7ffff7c22000 r--p 00000000 08:01 5678       /usr/lib/libc.so.6\n\
             7ffff7fff000-7ffff8000000 rw-p 00000000 00:00 0          \n\
             ffffffffff600000-ffffffffff601000 --xp 00000000 00:00 0  [vdso]\n"
        )
    }

    #[test]
    fn find_load_base_matches_normal_path() {
        let maps = maps_fixture("/usr/bin/myapp");
        let base = find_load_base(&maps, b"/usr/bin/myapp");
        assert_eq!(base, Some(0x555555554000));
    }

    #[test]
    fn find_load_base_handles_path_with_space() {
        let maps = maps_fixture("/mnt/My Files/myapp");
        let base = find_load_base(&maps, b"/mnt/My Files/myapp");
        assert_eq!(base, Some(0x555555554000));
    }

    #[test]
    fn find_load_base_handles_deleted_suffix() {
        let maps = maps_fixture("/usr/bin/myapp (deleted)");
        let base = find_load_base(&maps, b"/usr/bin/myapp (deleted)");
        assert_eq!(base, Some(0x555555554000));
    }

    #[test]
    fn find_load_base_skips_anonymous_mappings() {
        let maps = maps_fixture("/usr/bin/myapp");
        assert_eq!(find_load_base(&maps, b"/no/such/binary"), None);
    }

    fn apply_bias(addr: usize, bias: Option<usize>) -> usize {
        match bias {
            Some(b) => addr.saturating_sub(b),
            None => addr,
        }
    }

    #[test]
    fn apply_bias_round_trips() {
        let addr = 0x1_0000_2000usize;
        let bias = 0x1_0000_0000usize;
        let corrected = apply_bias(addr, Some(bias));
        assert_eq!(bias + corrected, addr);
    }

    #[test]
    fn apply_bias_none_is_identity() {
        let addr = 0xdeadbeefusize;
        assert_eq!(apply_bias(addr, None), addr);
    }

    #[test]
    fn apply_bias_saturates_instead_of_underflowing() {
        assert_eq!(apply_bias(10, Some(20)), 0);
    }

    #[test]
    fn load_bias_round_trips_on_this_platform() {
        fn marker() {}
        let addr = marker as fn() as usize;

        match load_bias() {
            Some(bias) => {
                assert!(
                    addr >= bias,
                    "function address {addr:#x} below detected load bias {bias:#x}"
                );
                let offset = addr - bias;
                assert_eq!(bias + offset, addr);
            }
            None => {
                assert!(apply_bias(addr, None) == addr);
            }
        }
    }

    #[test]
    fn load_bias_is_stable_across_calls() {
        assert_eq!(load_bias(), load_bias());
    }
}
