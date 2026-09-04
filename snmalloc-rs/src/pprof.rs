//! pprof protobuf encoder for [`HeapProfile`].
//!
//! Emits the subset of Google's pprof
//! [`Profile`](https://github.com/google/pprof/blob/main/proto/profile.proto)
//! schema needed to drive `go tool pprof`, Pyroscope, Polar Signals,
//! Parca, and the Datadog continuous-profiler front-ends from a
//! snmalloc heap profile snapshot.
//!
//! Encoding strategy
//! -----------------
//!
//! We **hand-roll** the protobuf encoder rather than bringing in
//! `prost`/`prost-build`. Reasons:
//!
//! 1. The Profile message is small (~10 top-level fields) and the
//!  `proto3` wire format we need is just two encodings — varint
//!  and length-delimited. A from-scratch encoder is ~80 lines.
//! 2. Avoids adding `prost` (which transitively pulls in `bytes`,
//!  `prost-derive`, syn, quote, ...) for a single message format.
//!  This keeps `--features profiling` lean: zero new transitive
//!  dependencies versus the existing `profiling` feature.
//! 3. `prost-build` would require a `build.rs` for the `snmalloc-rs`
//!  crate — right now we have none. Keeping `snmalloc-rs` free of
//!  build scripts speeds up downstream compiles.
//!
//! The output is **not** gzipped. The pprof tooling accepts both
//! compressed (`Content-Encoding: gzip`) and uncompressed Profile
//! bytes; `go tool pprof file.pb` happily ingests either, with the
//! convention being that `.pb` is uncompressed and `.pb.gz` is gzipped.
//! Skipping gzip avoids pulling in a `flate2` dependency. Callers
//! that need gzip can wrap the writer in `flate2::GzEncoder`
//! themselves.
//!
//! Unsymbolicated frames
//! ---------------------
//!
//! When the `symbolicate` feature is **off**, every captured frame
//! address is emitted as a [`Function`] whose `name` is the
//! `0x` + 16-hex-digit rendering of the raw address and whose
//! `filename` and `start_line` are empty / zero. This mirrors the
//! contract of [`HeapProfile::write_flamegraph`] in the same build
//! configuration. pprof viewers render that as
//! "`0x000000010a4b9c30`" on the flamegraph leaves.
//!
//! With the `symbolicate` feature on, function names resolve via
//! [`HeapProfile::symbolize`] when available, with the hex fallback
//! used for any frame the symbol backend can't resolve.
//!
//! Offline symbolication and ASLR
//! -------------------------------
//!
//! `Location.address` is corrected for the running process's load
//! bias (see [`load_bias`]) before it is written out, so the emitted
//! address is a stable, file-relative offset rather than a raw
//! runtime pointer. This matters because ASLR/PIE gives every
//! process run a different base address for its own executable;
//! without the correction, an address baked into a `.pb` file would
//! only mean the right thing to the exact process instance that
//! produced it, and would resolve to the wrong symbol (or nothing at
//! all) when a *different* process — `go tool pprof`, `addr2line`,
//! `atos`, or a symbol server — later loads the same on-disk binary
//! at a different base and tries to interpret the file.
//!
//! The correction is unconditional: it applies whether or not the
//! `symbolicate` feature is enabled, because it matters most exactly
//! when `symbolicate` is off — a stripped release binary whose
//! profile needs later, external resolution has no other way to
//! recover the right addresses. It doesn't touch the in-process
//! symbol lookup in [`HeapProfile::symbolize`], which resolves
//! against the raw runtime pointer as `backtrace::resolve` expects.
//!
//! No [`Mapping`](https://github.com/google/pprof/blob/main/proto/profile.proto)
//! message is emitted (`Location.mapping_id` stays `0`), so an
//! external consumer has no build-id or file-range metadata to
//! correct the address itself — the correction has to happen here,
//! at encode time, or not at all.

extern crate alloc;
extern crate std;

use alloc::collections::BTreeMap;
use alloc::string::String;
use alloc::vec::Vec;
use core::fmt::Write as _;

use std::io;
use std::io::Write;

use crate::profile::{BtSample, HeapProfile, Weight};

mod load_bias;
use load_bias::load_bias;

const WIRE_TYPE_VARINT: u32 = 0;
const WIRE_TYPE_LEN: u32 = 2;

/// Encode a u64 varint into `out`.
fn varint(out: &mut Vec<u8>, mut value: u64) {
    while value >= 0x80 {
        out.push((value as u8) | 0x80);
        value >>= 7;
    }
    out.push(value as u8);
}

/// Encode a field tag (field number + wire type) into `out`.
fn tag(out: &mut Vec<u8>, field_number: u32, wire_type: u32) {
    varint(out, ((field_number << 3) | wire_type) as u64);
}

/// Encode a `(field, varint)` pair into `out`.
fn write_uint64(out: &mut Vec<u8>, field_number: u32, value: u64) {
    tag(out, field_number, WIRE_TYPE_VARINT);
    varint(out, value);
}

/// Encode a `(field, int64)` pair into `out`. proto3 represents
/// negative int64 as a 10-byte varint; we only ever emit non-negative
/// values so the bit pattern is the same as a u64.
fn write_int64(out: &mut Vec<u8>, field_number: u32, value: i64) {
    tag(out, field_number, WIRE_TYPE_VARINT);
    varint(out, value as u64);
}

/// Encode a `(field, length-delimited bytes)` pair into `out`. Used
/// for both string fields and nested messages.
fn write_bytes(out: &mut Vec<u8>, field_number: u32, bytes: &[u8]) {
    tag(out, field_number, WIRE_TYPE_LEN);
    varint(out, bytes.len() as u64);
    out.extend_from_slice(bytes);
}

/// Encode a packed-repeated `int64` field into `out`. Used by
/// `Sample.value` and `Sample.location_id`. An empty slice still
/// writes a zero-length record so the consumer can distinguish "field
/// not set" from "field set to an empty list" (the latter matters for
/// pprof's `period_type`-vs-`sample_type` alignment checks).
fn write_packed_uint64(out: &mut Vec<u8>, field_number: u32, values: &[u64]) {
    if values.is_empty() {
        return;
    }
    let mut buf: Vec<u8> = Vec::new();
    for &v in values {
        varint(&mut buf, v);
    }
    write_bytes(out, field_number, &buf);
}

/// Encode a packed-repeated `int64` field into `out` (same wire
/// format as `write_packed_uint64`, separate signature for
/// readability at the call site — pprof has both `value` (int64) and
/// `location_id` (uint64) packed repeated fields).
fn write_packed_int64(out: &mut Vec<u8>, field_number: u32, values: &[i64]) {
    if values.is_empty() {
        return;
    }
    let mut buf: Vec<u8> = Vec::new();
    for &v in values {
        varint(&mut buf, v as u64);
    }
    write_bytes(out, field_number, &buf);
}

struct StringTable {
    /// Insertion-ordered list of strings. Index 0 is always "".
    strings: Vec<String>,
    /// Reverse lookup: string -> index. Avoids O(N) scans when the
    /// same name appears in many frames (e.g. a hot allocator
    /// entrypoint shared across thousands of samples).
    index: BTreeMap<String, u32>,
}

impl StringTable {
    fn new() -> Self {
        let mut t = Self {
            strings: Vec::new(),
            index: BTreeMap::new(),
        };
        t.intern("");
        t
    }

    /// Look up or insert `s`, returning its index. Indices are
    /// monotonically increasing; once assigned, they are stable for
    /// the lifetime of this table.
    fn intern(&mut self, s: &str) -> u32 {
        if let Some(&idx) = self.index.get(s) {
            return idx;
        }
        let idx = self.strings.len() as u32;
        self.strings.push(String::from(s));
        self.index.insert(String::from(s), idx);
        idx
    }
}

/// Render a raw code-pointer address as `0x` + 16 hex digits. Used
/// as the fallback function name when no symbolicated name is
/// available (the unsymbolicated build path).
fn hex_addr(addr: usize) -> String {
    let mut s = String::with_capacity(18);
    write!(&mut s, "0x{:016x}", addr).expect("writing to String is infallible");
    s
}

/// Write the [`HeapProfile`] as a pprof Profile protobuf message
/// into `w`.
///
/// The emitted Profile has two sample-type axes:
///
/// 1. `("alloc_objects", "count")` — always `1` per sample. Lets
///  pprof aggregate by *sample count* (i.e. distinct sampled
///  allocations) as well as by bytes.
/// 2. `("alloc_space", "bytes")` — the per-sample byte contribution
///  under the requested [`Weight`] projection. Summing this axis
///  across all samples equals [`HeapProfile::total_allocated_bytes`]
///  (for `Weight::Allocated`) or [`HeapProfile::total_requested_bytes`]
///  (for `Weight::Requested`).
///
/// `default_sample_type` is set to `alloc_space` so that pprof's
/// `top` / `web` views default to the bytes view, matching what most
/// heap-attribution dashboards want.
///
/// The output isn't gzipped. See the module-level docs for the
/// rationale.
///
/// This call is total: it produces a valid (but tiny) Profile even
/// for an empty snapshot. An empty pprof Profile still contains the
/// `sample_type` and `string_table` fields — consumers like `go tool
/// pprof` will display an empty profile cleanly rather than rejecting
/// the input.
pub(crate) fn write_pprof<W: Write>(
    profile: &HeapProfile,
    weight: Weight,
    w: &mut W,
) -> io::Result<()> {
    write_pprof_with_bias(profile, weight, load_bias(), w)
}

/// [`write_pprof`] with an explicit load bias.
fn write_pprof_with_bias<W: Write>(
    profile: &HeapProfile,
    weight: Weight,
    bias: Option<usize>,
    w: &mut W,
) -> io::Result<()> {
    let mut strings = StringTable::new();

    let s_alloc_objects = strings.intern("alloc_objects");
    let s_count = strings.intern("count");
    let s_alloc_space = strings.intern("alloc_space");
    let s_bytes = strings.intern("bytes");

    #[cfg(feature = "symbolicate")]
    let resolved = profile.symbolize();

    let mut addr_to_loc: BTreeMap<usize, u64> = BTreeMap::new();
    let mut addr_to_func: BTreeMap<usize, u64> = BTreeMap::new();
    let mut next_location_id: u64 = 1;
    let mut next_function_id: u64 = 1;

    let mut functions_buf: Vec<Vec<u8>> = Vec::new();
    let mut locations_buf: Vec<Vec<u8>> = Vec::new();

    for s in profile.samples() {
        for &frame in &s.stack {
            let addr = frame as usize;
            if addr_to_loc.contains_key(&addr) {
                continue;
            }
            #[cfg(feature = "symbolicate")]
            let (name_idx, file_idx, line_no) = {
                let r = resolved.get(&(frame as *const u8));
                let name = r.and_then(|r| r.name.as_deref());
                let file = r.and_then(|r| r.file.as_deref()).unwrap_or("");
                let line = r.and_then(|r| r.line).unwrap_or(0) as i64;
                let nm = match name {
                    Some(n) => strings.intern(n),
                    None => strings.intern(&hex_addr(addr)),
                };
                (nm, strings.intern(file), line)
            };
            #[cfg(not(feature = "symbolicate"))]
            let (name_idx, file_idx, line_no) = {
                let nm = strings.intern(&hex_addr(addr));
                (nm, 0u32, 0i64)
            };

            let function_id = next_function_id;
            next_function_id += 1;
            addr_to_func.insert(addr, function_id);

            let mut func_buf: Vec<u8> = Vec::new();
            write_uint64(&mut func_buf, 1, function_id);
            write_int64(&mut func_buf, 2, name_idx as i64);
            write_int64(&mut func_buf, 3, name_idx as i64);
            write_int64(&mut func_buf, 4, file_idx as i64);
            write_int64(&mut func_buf, 5, 0);
            functions_buf.push(func_buf);

            let location_id = next_location_id;
            next_location_id += 1;
            addr_to_loc.insert(addr, location_id);

            let mut line_buf: Vec<u8> = Vec::new();
            write_uint64(&mut line_buf, 1, function_id);
            write_int64(&mut line_buf, 2, line_no);

            let mut loc_buf: Vec<u8> = Vec::new();
            write_uint64(&mut loc_buf, 1, location_id);
            write_uint64(&mut loc_buf, 2, 0);
            write_uint64(&mut loc_buf, 3, corrected_address(addr, bias));
            write_bytes(&mut loc_buf, 4, &line_buf);
            locations_buf.push(loc_buf);
        }
    }

    let mut samples_buf: Vec<Vec<u8>> = Vec::with_capacity(profile.samples().len());
    for s in profile.samples() {
        let loc_ids: Vec<u64> = s
            .stack
            .iter()
            .map(|&p| {
                *addr_to_loc
                    .get(&(p as usize))
                    .expect("every frame address was indexed in step 1")
            })
            .collect();
        let alloc_objects: i64 = 1;
        let alloc_space: i64 = sample_weight(s, weight) as i64;
        let values: [i64; 2] = [alloc_objects, alloc_space];

        let mut sample_buf: Vec<u8> = Vec::new();
        write_packed_uint64(&mut sample_buf, 1, &loc_ids);
        write_packed_int64(&mut sample_buf, 2, &values);
        samples_buf.push(sample_buf);
    }

    let mut out: Vec<u8> = Vec::new();

    {
        let mut vt: Vec<u8> = Vec::new();
        write_int64(&mut vt, 1, s_alloc_objects as i64);
        write_int64(&mut vt, 2, s_count as i64);
        write_bytes(&mut out, 1, &vt);
    }
    {
        let mut vt: Vec<u8> = Vec::new();
        write_int64(&mut vt, 1, s_alloc_space as i64);
        write_int64(&mut vt, 2, s_bytes as i64);
        write_bytes(&mut out, 1, &vt);
    }

    for sample_buf in &samples_buf {
        write_bytes(&mut out, 2, sample_buf);
    }
    for loc_buf in &locations_buf {
        write_bytes(&mut out, 4, loc_buf);
    }
    for func_buf in &functions_buf {
        write_bytes(&mut out, 5, func_buf);
    }
    for s in &strings.strings {
        write_bytes(&mut out, 6, s.as_bytes());
    }
    write_int64(&mut out, 14, s_alloc_space as i64);

    w.write_all(&out)
}

/// Correct a raw runtime frame address for ASLR/PIE load bias.
///
/// Returns `addr - bias` when `bias` is `Some`, unchanged `addr` when
/// `bias` is `None` (the "couldn't determine bias" case — see
/// [`load_bias`]). Uses `saturating_sub` rather than plain
/// subtraction so a frame belonging to a mapping based *below* the
/// main executable (e.g. a dynamically loaded library) degrades to
/// `0` instead of underflow-panicking; that mirrors the `None` case's
/// "can't correct, don't crash" contract rather than treating it as
/// an error.
fn corrected_address(addr: usize, bias: Option<usize>) -> u64 {
    match bias {
        Some(b) => (addr as u64).saturating_sub(b as u64),
        None => addr as u64,
    }
}

fn sample_weight(s: &BtSample, weight: Weight) -> u128 {
    match weight {
        Weight::Requested => s.weight as u128,
        Weight::Allocated => {
            if s.requested_size == 0 {
                0
            } else {
                let w = s.weight as u128;
                let a = s.allocated_size as u128;
                let r = s.requested_size as u128;
                w.saturating_mul(a) / r
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::profile::BtSample;
    use alloc::vec;

    #[test]
    fn varint_round_trip() {
        let cases: &[(u64, &[u8])] = &[
            (0, &[0x00]),
            (1, &[0x01]),
            (127, &[0x7f]),
            (128, &[0x80, 0x01]),
            (300, &[0xac, 0x02]),
            (16384, &[0x80, 0x80, 0x01]),
        ];
        for &(v, expected) in cases {
            let mut buf: Vec<u8> = Vec::new();
            varint(&mut buf, v);
            assert_eq!(buf.as_slice(), expected, "varint({}) mismatch", v);
        }
    }

    #[test]
    fn empty_profile_is_valid() {
        let p = HeapProfile::default();
        let mut buf: Vec<u8> = Vec::new();
        write_pprof(&p, Weight::Allocated, &mut buf).unwrap();

        assert!(!buf.is_empty(), "empty profile produced zero bytes");

        let bytes = &buf[..];
        for needle in &["alloc_objects", "count", "alloc_space", "bytes"] {
            assert!(
                bytes.windows(needle.len()).any(|w| w == needle.as_bytes()),
                "expected string {:?} in empty Profile output",
                needle
            );
        }
    }

    #[test]
    fn alloc_space_axis_matches_total_allocated_bytes() {
        let p = HeapProfile::from_samples(vec![
            BtSample {
                alloc_ptr: core::ptr::null(),
                requested_size: 64,
                allocated_size: 64,
                weight: 4096,
                stack: vec![0x1usize as *const u8, 0x2usize as *const u8],
            },
            BtSample {
                alloc_ptr: core::ptr::null(),
                requested_size: 100,
                allocated_size: 128,
                weight: 8192,
                stack: vec![0x3usize as *const u8],
            },
        ]);
        let mut buf: Vec<u8> = Vec::new();
        write_pprof(&p, Weight::Allocated, &mut buf).unwrap();

        let total = decode_alloc_space_sum(&buf);
        assert_eq!(total, p.total_allocated_bytes() as i64);
    }

    #[test]
    fn alloc_space_axis_matches_total_requested_bytes() {
        let p = HeapProfile::from_samples(vec![BtSample {
            alloc_ptr: core::ptr::null(),
            requested_size: 100,
            allocated_size: 128,
            weight: 8192,
            stack: vec![0x3usize as *const u8],
        }]);
        let mut buf: Vec<u8> = Vec::new();
        write_pprof(&p, Weight::Requested, &mut buf).unwrap();

        let total = decode_alloc_space_sum(&buf);
        assert_eq!(total, p.total_requested_bytes() as i64);
    }

    fn decode_alloc_space_sum(buf: &[u8]) -> i64 {
        let mut sum: i64 = 0;
        let mut i: usize = 0;
        while i < buf.len() {
            let (tag, n) = read_varint(&buf[i..]);
            i += n;
            let field = (tag >> 3) as u32;
            let wire = (tag & 0x7) as u32;
            match (field, wire) {
                (2, WIRE_TYPE_LEN) => {
                    let (len, n) = read_varint(&buf[i..]);
                    i += n;
                    let end = i + len as usize;
                    sum += decode_sample_alloc_space(&buf[i..end]);
                    i = end;
                }
                (_, WIRE_TYPE_LEN) => {
                    let (len, n) = read_varint(&buf[i..]);
                    i += n;
                    i += len as usize;
                }
                (_, WIRE_TYPE_VARINT) => {
                    let (_, n) = read_varint(&buf[i..]);
                    i += n;
                }
                _ => panic!("unsupported wire type {} for field {}", wire, field),
            }
        }
        sum
    }

    fn decode_sample_alloc_space(buf: &[u8]) -> i64 {
        let mut i: usize = 0;
        while i < buf.len() {
            let (tag, n) = read_varint(&buf[i..]);
            i += n;
            let field = (tag >> 3) as u32;
            let wire = (tag & 0x7) as u32;
            match (field, wire) {
                (2, WIRE_TYPE_LEN) => {
                    let (len, n) = read_varint(&buf[i..]);
                    i += n;
                    let end = i + len as usize;
                    let mut values: Vec<i64> = Vec::new();
                    let mut j = i;
                    while j < end {
                        let (v, n) = read_varint(&buf[j..]);
                        j += n;
                        values.push(v as i64);
                    }
                    if values.len() >= 2 {
                        return values[1];
                    }
                    i = end;
                }
                (_, WIRE_TYPE_LEN) => {
                    let (len, n) = read_varint(&buf[i..]);
                    i += n;
                    i += len as usize;
                }
                (_, WIRE_TYPE_VARINT) => {
                    let (_, n) = read_varint(&buf[i..]);
                    i += n;
                }
                _ => panic!("unsupported wire type {} for field {}", wire, field),
            }
        }
        0
    }

    fn read_varint(buf: &[u8]) -> (u64, usize) {
        let mut value: u64 = 0;
        let mut shift: u32 = 0;
        for (i, &b) in buf.iter().enumerate() {
            value |= ((b & 0x7f) as u64) << shift;
            if b & 0x80 == 0 {
                return (value, i + 1);
            }
            shift += 7;
            if shift >= 64 {
                panic!("varint overflow");
            }
        }
        panic!("truncated varint");
    }

    #[test]
    fn unique_frames_dedup_function_and_location() {
        let shared = 0xdeadbeefusize as *const u8;
        let p = HeapProfile::from_samples(vec![
            BtSample {
                alloc_ptr: core::ptr::null(),
                requested_size: 64,
                allocated_size: 64,
                weight: 4096,
                stack: vec![shared, 0x1usize as *const u8],
            },
            BtSample {
                alloc_ptr: core::ptr::null(),
                requested_size: 64,
                allocated_size: 64,
                weight: 4096,
                stack: vec![shared, 0x2usize as *const u8],
            },
        ]);
        let mut buf: Vec<u8> = Vec::new();
        write_pprof(&p, Weight::Allocated, &mut buf).unwrap();

        let (n_loc, n_fn) = count_locations_and_functions(&buf);
        assert_eq!(n_loc, 3, "expected 3 unique locations");
        assert_eq!(n_fn, 3, "expected 3 unique functions");
    }

    fn count_locations_and_functions(buf: &[u8]) -> (usize, usize) {
        let mut n_loc = 0usize;
        let mut n_fn = 0usize;
        let mut i: usize = 0;
        while i < buf.len() {
            let (tag, n) = read_varint(&buf[i..]);
            i += n;
            let field = (tag >> 3) as u32;
            let wire = (tag & 0x7) as u32;
            match (field, wire) {
                (4, WIRE_TYPE_LEN) => {
                    n_loc += 1;
                    let (len, n) = read_varint(&buf[i..]);
                    i += n;
                    i += len as usize;
                }
                (5, WIRE_TYPE_LEN) => {
                    n_fn += 1;
                    let (len, n) = read_varint(&buf[i..]);
                    i += n;
                    i += len as usize;
                }
                (_, WIRE_TYPE_LEN) => {
                    let (len, n) = read_varint(&buf[i..]);
                    i += n;
                    i += len as usize;
                }
                (_, WIRE_TYPE_VARINT) => {
                    let (_, n) = read_varint(&buf[i..]);
                    i += n;
                }
                _ => panic!("unsupported wire type {} for field {}", wire, field),
            }
        }
        (n_loc, n_fn)
    }

    #[test]
    fn string_table_slot_zero_is_empty() {
        let mut t = StringTable::new();
        assert_eq!(t.intern(""), 0);
        assert_eq!(t.intern(""), 0);
        assert_eq!(t.intern("alloc_objects"), 1);
    }

    #[test]
    fn corrected_address_subtracts_bias() {
        assert_eq!(corrected_address(0x2000, Some(0x1000)), 0x1000);
        assert_eq!(corrected_address(0x2000, None), 0x2000);
        assert_eq!(corrected_address(0x100, Some(0x2000)), 0);
    }

    fn decode_location_addresses(buf: &[u8]) -> Vec<u64> {
        let mut addrs = Vec::new();
        let mut i: usize = 0;
        while i < buf.len() {
            let (tag, n) = read_varint(&buf[i..]);
            i += n;
            let field = (tag >> 3) as u32;
            let wire = (tag & 0x7) as u32;
            match (field, wire) {
                (4, WIRE_TYPE_LEN) => {
                    let (len, n) = read_varint(&buf[i..]);
                    i += n;
                    let end = i + len as usize;
                    addrs.push(decode_location_address(&buf[i..end]));
                    i = end;
                }
                (_, WIRE_TYPE_LEN) => {
                    let (len, n) = read_varint(&buf[i..]);
                    i += n;
                    i += len as usize;
                }
                (_, WIRE_TYPE_VARINT) => {
                    let (_, n) = read_varint(&buf[i..]);
                    i += n;
                }
                _ => panic!("unsupported wire type {} for field {}", wire, field),
            }
        }
        addrs
    }

    fn decode_location_address(buf: &[u8]) -> u64 {
        let mut i: usize = 0;
        let mut address = 0u64;
        while i < buf.len() {
            let (tag, n) = read_varint(&buf[i..]);
            i += n;
            let field = (tag >> 3) as u32;
            let wire = (tag & 0x7) as u32;
            match (field, wire) {
                (3, WIRE_TYPE_VARINT) => {
                    let (v, n) = read_varint(&buf[i..]);
                    i += n;
                    address = v;
                }
                (_, WIRE_TYPE_LEN) => {
                    let (len, n) = read_varint(&buf[i..]);
                    i += n;
                    i += len as usize;
                }
                (_, WIRE_TYPE_VARINT) => {
                    let (_, n) = read_varint(&buf[i..]);
                    i += n;
                }
                _ => panic!("unsupported wire type {} for field {}", wire, field),
            }
        }
        address
    }

    #[test]
    fn write_pprof_with_bias_corrects_location_addresses() {
        let raw_addrs: [usize; 2] = [0x1_0000_3000, 0x1_0000_4000];
        let bias: usize = 0x1_0000_0000;
        let p = HeapProfile::from_samples(vec![BtSample {
            alloc_ptr: core::ptr::null(),
            requested_size: 64,
            allocated_size: 64,
            weight: 4096,
            stack: raw_addrs.iter().map(|&a| a as *const u8).collect(),
        }]);

        let mut buf: Vec<u8> = Vec::new();
        write_pprof_with_bias(&p, Weight::Allocated, Some(bias), &mut buf).unwrap();

        let mut got = decode_location_addresses(&buf);
        got.sort_unstable();
        let mut want: Vec<u64> = raw_addrs.iter().map(|&a| (a - bias) as u64).collect();
        want.sort_unstable();
        assert_eq!(got, want, "Location.address must be bias-corrected");

        for &raw in &raw_addrs {
            assert!(
                !got.contains(&(raw as u64)),
                "found uncorrected raw address {raw:#x} in output"
            );
        }
    }

    #[test]
    fn write_pprof_with_bias_none_emits_raw_addresses() {
        let raw_addr = 0x1_0000_3000usize;
        let p = HeapProfile::from_samples(vec![BtSample {
            alloc_ptr: core::ptr::null(),
            requested_size: 64,
            allocated_size: 64,
            weight: 4096,
            stack: vec![raw_addr as *const u8],
        }]);

        let mut buf: Vec<u8> = Vec::new();
        write_pprof_with_bias(&p, Weight::Allocated, None, &mut buf).unwrap();

        let got = decode_location_addresses(&buf);
        assert_eq!(got, vec![raw_addr as u64]);
    }
}
