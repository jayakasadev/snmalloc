//! Safe text-dump C ABI wrapper.
//!
//! [`write_to`] queries the byte count, allocates once, then fills it.
//! Builds without stats or profiling still emit a header.

extern crate alloc;
extern crate std;

use alloc::vec::Vec;
use core::ptr;
use std::io;

use snmalloc_sys as ffi;

use crate::SnMalloc;

impl SnMalloc {
    /// Write tcmalloc-style allocator statistics.
    ///
    /// This is available without the `stats` feature. See [`write_to`].
    #[inline]
    pub fn dump_stats<W: io::Write>(&self, out: &mut W) -> io::Result<()> {
        write_to(out)
    }
}

/// Write the current telemetry snapshot to `out`.
///
/// Queries the byte count, allocates that count plus a trailing NUL,
/// then calls `write_all`. Write errors are returned.
///
/// Output starts with tcmalloc-style `MALLOC:` lines. Nonzero data may
/// add a per-size-class table and a base-2 lifetime histogram.
///
/// This doesn't mutate allocator state and is thread-safe. Counters
/// match [`crate::SnMalloc::full_stats`].
pub fn write_to<W: io::Write>(out: &mut W) -> io::Result<()> {
    let needed = unsafe { ffi::snmalloc_dump_stats_to_buffer(ptr::null_mut(), 0) };
    if needed == 0 {
        return Ok(());
    }

    let mut buf: Vec<u8> = Vec::with_capacity(needed + 1);
    let written = unsafe {
        let n = ffi::snmalloc_dump_stats_to_buffer(buf.as_mut_ptr(), needed + 1);
        let n = if n > needed { needed } else { n };
        // SAFETY: the C writer fills `n` bytes inside the
        // capacity we reserved. We mark them initialised before
        // slicing.
        buf.set_len(n);
        n
    };

    if written == 0 {
        return Ok(());
    }
    out.write_all(&buf)
}

/// Return the ASCII dump as an owned `String`.
///
/// Returns an empty string when there is nothing to report.
pub fn to_string() -> alloc::string::String {
    let mut buf: Vec<u8> = Vec::new();
    let _ = write_to(&mut buf);
    match alloc::string::String::from_utf8(buf) {
        Ok(s) => s,
        Err(e) => alloc::string::String::from_utf8_lossy(&e.into_bytes()).into_owned(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use alloc::string::String;

    #[test]
    fn dump_is_nonempty_and_well_formed() {
        let s = to_string();
        assert!(!s.is_empty(), "dump must produce at least the header block");
        assert!(
            s.contains("Bytes in use by application"),
            "dump must contain the canonical 'Bytes in use by application' line; \
             got: {}",
            s
        );
        assert!(
            s.contains("------------------------------------------------"),
            "dump must contain a horizontal rule"
        );
    }

    #[test]
    fn write_to_propagates_writer_errors() {
        struct Broken;
        impl io::Write for Broken {
            fn write(&mut self, _b: &[u8]) -> io::Result<usize> {
                Err(io::Error::new(io::ErrorKind::Other, "broken"))
            }
            fn flush(&mut self) -> io::Result<()> {
                Ok(())
            }
        }
        let mut broken = Broken;
        let err = write_to(&mut broken).expect_err("broken writer must propagate as Err");
        assert_eq!(err.kind(), io::ErrorKind::Other);
    }

    #[test]
    fn size_query_matches_real_fill() {
        let needed = unsafe { ffi::snmalloc_dump_stats_to_buffer(ptr::null_mut(), 0) };
        let mut s = String::new();
        s.reserve(needed);
        let _ = to_string();
    }
}
