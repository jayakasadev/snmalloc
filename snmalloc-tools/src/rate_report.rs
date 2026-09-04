//! Reports per-site rates from a [`snmalloc_rs::ProfilingSession`] log.
//!
//! ## On-disk format
//!
//! Input is UTF-8 JSON Lines. Extra fields are ignored.
//!
//! ```jsonl
//! {"ts_ns": 1000000, "kind": "alloc", "site": "0x55a0c0001000", "size": 4096}
//! {"ts_ns": 1001000, "kind": "alloc", "site": "0x55a0c0002000", "size": 256}
//! {"ts_ns": 1002000, "kind": "dealloc", "site": "0x55a0c0001000", "size": 4096}
//! ```
//!
//! Field semantics:
//!
//! - `ts_ns` (optional `u64`): monotonic nanoseconds. Rates use the
//!  earliest and latest available timestamps.
//! - `kind` (required string): `"alloc"` or `"dealloc"` changes totals.
//!  Other values are size-neutral.
//! - `site` (required string): stable site key, often a leaf address.
//! - `size` (optional `u64`): added or removed bytes; defaults to zero.
//!
//! ## Streaming guarantees
//!
//! Memory use grows with distinct sites, not events.

use std::collections::HashMap;
use std::fs::File;
use std::io::{self, BufRead, BufReader, Read};
use std::path::Path;

use anyhow::{Context, Result};
use serde::{Deserialize, Serialize};

/// One site's rate data.
///
/// `peak_live_bytes` is the largest running `alloc_size - dealloc_size`.
/// `alloc_rate_per_sec` is
/// `alloc_count / (last_ts_ns - first_ts_ns) * 1e9`; if the log has
/// fewer than two distinct timestamps, it is `0.0`.
#[derive(Clone, Debug, Default, PartialEq, Serialize, Deserialize)]
pub struct RateRow {
    /// Site key copied from the log.
    pub site: String,
    /// Number of `kind == "alloc"` events for this site.
    pub alloc_count: u64,
    /// Number of `kind == "dealloc"` events for this site.
    pub dealloc_count: u64,
    /// Largest running live-byte total.
    pub peak_live_bytes: u64,
    /// Allocations per second, or `0.0` without a usable time span.
    pub alloc_rate_per_sec: f64,
}

/// Permissive on-disk record. Every field is optional except `kind`
/// and `site` (verified during reduction). Extra fields are ignored.
#[derive(Debug, Deserialize)]
struct RawEvent {
    #[serde(default)]
    ts_ns: Option<u64>,
    kind: Option<String>,
    site: Option<String>,
    #[serde(default)]
    size: Option<u64>,
}

/// Per-site running accumulator. Owned by the reducer; never escapes
/// the function. Keeping this off the `RateRow` keeps the public
/// output type narrow and serialisation-friendly.
#[derive(Default)]
struct SiteAcc {
    alloc_count: u64,
    dealloc_count: u64,
    /// Running `alloc_bytes - dealloc_bytes` for this site. Saturates
    /// at zero on underflow so a log that emits a dealloc before its
    /// matching alloc (e.g. wraparound after a restart) doesn't panic.
    live_bytes: u64,
    /// Watermark of `live_bytes`.
    peak_live_bytes: u64,
}

/// Read a log file into per-site rows.
///
/// Reads one line at a time. Invalid and blank lines are skipped.
/// Rows sort by descending `alloc_count`, then ascending `site`.
pub fn read_path<P: AsRef<Path>>(path: P) -> Result<Vec<RateRow>> {
    let p = path.as_ref();
    let f =
        File::open(p).with_context(|| format!("opening streaming event log {}", p.display()))?;
    read_reader(BufReader::new(f))
}

/// Like [`read_path`], but reads any [`Read`].
pub fn read_reader<R: Read>(reader: R) -> Result<Vec<RateRow>> {
    let buf = BufReader::new(reader);
    reduce_lines(buf.lines())
}

/// Fold line results into per-site state.
fn reduce_lines<I>(lines: I) -> Result<Vec<RateRow>>
where
    I: IntoIterator<Item = io::Result<String>>,
{
    let mut sites: HashMap<String, SiteAcc> = HashMap::new();
    let mut first_ts: Option<u64> = None;
    let mut last_ts: Option<u64> = None;
    let mut any_ts = false;

    for line in lines {
        let line = match line {
            Ok(l) => l,
            Err(_) => break,
        };
        let trimmed = line.trim();
        if trimmed.is_empty() {
            continue;
        }

        let raw: RawEvent = match serde_json::from_str(trimmed) {
            Ok(r) => r,
            Err(_) => continue,
        };

        let site = match raw.site {
            Some(s) if !s.is_empty() => s,
            _ => continue,
        };
        let kind = match raw.kind.as_deref() {
            Some(k) => k,
            None => continue,
        };
        let size = raw.size.unwrap_or(0);

        if let Some(ts) = raw.ts_ns {
            any_ts = true;
            first_ts = Some(first_ts.map(|f| f.min(ts)).unwrap_or(ts));
            last_ts = Some(last_ts.map(|l| l.max(ts)).unwrap_or(ts));
        }

        let acc = sites.entry(site).or_default();
        match kind {
            "alloc" => {
                acc.alloc_count += 1;
                acc.live_bytes = acc.live_bytes.saturating_add(size);
                if acc.live_bytes > acc.peak_live_bytes {
                    acc.peak_live_bytes = acc.live_bytes;
                }
            }
            "dealloc" => {
                acc.dealloc_count += 1;
                acc.live_bytes = acc.live_bytes.saturating_sub(size);
            }
            _ => {}
        }
    }

    let span_sec: f64 = match (first_ts, last_ts, any_ts) {
        (Some(f), Some(l), true) if l > f => (l - f) as f64 / 1_000_000_000.0,
        _ => 0.0,
    };

    let mut rows: Vec<RateRow> = sites
        .into_iter()
        .map(|(site, acc)| {
            let rate = if span_sec > 0.0 {
                acc.alloc_count as f64 / span_sec
            } else {
                0.0
            };
            RateRow {
                site,
                alloc_count: acc.alloc_count,
                dealloc_count: acc.dealloc_count,
                peak_live_bytes: acc.peak_live_bytes,
                alloc_rate_per_sec: rate,
            }
        })
        .collect();

    rows.sort_by(|a, b| {
        b.alloc_count
            .cmp(&a.alloc_count)
            .then_with(|| a.site.cmp(&b.site))
    });

    Ok(rows)
}

/// Write rows in CSV format with a header line. Numeric columns are
/// rendered without thousands separators; the rate column is rendered
/// with six decimal digits (enough resolution for sub-Hz rates).
pub fn write_csv<W: io::Write>(rows: &[RateRow], w: &mut W) -> io::Result<()> {
    writeln!(
        w,
        "site,alloc_count,dealloc_count,peak_live_bytes,alloc_rate_per_sec"
    )?;
    for r in rows {
        writeln!(
            w,
            "{},{},{},{},{:.6}",
            r.site, r.alloc_count, r.dealloc_count, r.peak_live_bytes, r.alloc_rate_per_sec
        )?;
    }
    Ok(())
}

/// Write rows in a fixed-width pretty table (no external crate
/// dependency). Column widths are constants — the output is
/// readable in 120-column terminals and stable across runs.
pub fn write_pretty<W: io::Write>(rows: &[RateRow], w: &mut W) -> io::Result<()> {
    writeln!(
        w,
        "{:<20} {:>11} {:>13} {:>16} {:>20}",
        "site", "alloc_count", "dealloc_count", "peak_live_bytes", "alloc_rate_per_sec"
    )?;
    for r in rows {
        writeln!(
            w,
            "{:<20} {:>11} {:>13} {:>16} {:>20.6}",
            r.site, r.alloc_count, r.dealloc_count, r.peak_live_bytes, r.alloc_rate_per_sec
        )?;
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn lines_iter(s: &str) -> Vec<io::Result<String>> {
        s.lines().map(|l| Ok(l.to_string())).collect()
    }

    #[test]
    fn empty_input_yields_no_rows() {
        let rows = reduce_lines(lines_iter("")).unwrap();
        assert!(rows.is_empty());
    }

    #[test]
    fn skips_malformed_and_blank_lines() {
        let log = "\n\
                   not-json\n\
                   {\"kind\":\"alloc\",\"site\":\"0xA\",\"size\":10}\n\
                   {garbled}\n\
                   \n";
        let rows = reduce_lines(lines_iter(log)).unwrap();
        assert_eq!(rows.len(), 1);
        assert_eq!(rows[0].site, "0xA");
        assert_eq!(rows[0].alloc_count, 1);
    }

    #[test]
    fn peak_live_tracks_running_max_not_final() {
        let log = "{\"kind\":\"alloc\",\"site\":\"0xA\",\"size\":100,\"ts_ns\":0}\n\
                   {\"kind\":\"alloc\",\"site\":\"0xA\",\"size\":50,\"ts_ns\":1}\n\
                   {\"kind\":\"dealloc\",\"site\":\"0xA\",\"size\":120,\"ts_ns\":2}\n";
        let rows = reduce_lines(lines_iter(log)).unwrap();
        assert_eq!(rows.len(), 1);
        assert_eq!(rows[0].peak_live_bytes, 150);
        assert_eq!(rows[0].alloc_count, 2);
        assert_eq!(rows[0].dealloc_count, 1);
    }

    #[test]
    fn rate_uses_timestamp_span() {
        let log = "{\"kind\":\"alloc\",\"site\":\"0xA\",\"size\":1,\"ts_ns\":0}\n\
                   {\"kind\":\"alloc\",\"site\":\"0xA\",\"size\":1,\"ts_ns\":1000000000}\n";
        let rows = reduce_lines(lines_iter(log)).unwrap();
        assert_eq!(rows.len(), 1);
        assert!((rows[0].alloc_rate_per_sec - 2.0).abs() < 1e-9);
    }

    #[test]
    fn rate_is_zero_when_no_timestamps() {
        let log = "{\"kind\":\"alloc\",\"site\":\"0xA\",\"size\":1}\n\
                   {\"kind\":\"alloc\",\"site\":\"0xA\",\"size\":1}\n";
        let rows = reduce_lines(lines_iter(log)).unwrap();
        assert_eq!(rows.len(), 1);
        assert_eq!(rows[0].alloc_rate_per_sec, 0.0);
    }

    #[test]
    fn sort_is_alloc_count_desc_then_site_asc() {
        let log = "{\"kind\":\"alloc\",\"site\":\"0xB\",\"size\":1}\n\
                   {\"kind\":\"alloc\",\"site\":\"0xA\",\"size\":1}\n\
                   {\"kind\":\"alloc\",\"site\":\"0xA\",\"size\":1}\n";
        let rows = reduce_lines(lines_iter(log)).unwrap();
        assert_eq!(rows[0].site, "0xA"); // 2 allocs wins
        assert_eq!(rows[1].site, "0xB");
    }

    #[test]
    fn unknown_kind_is_ignored_for_counts() {
        let log = "{\"kind\":\"alloc\",\"site\":\"0xA\",\"size\":10}\n\
                   {\"kind\":\"resize\",\"site\":\"0xA\",\"size\":99}\n";
        let rows = reduce_lines(lines_iter(log)).unwrap();
        assert_eq!(rows[0].alloc_count, 1);
        assert_eq!(rows[0].peak_live_bytes, 10);
    }

    #[test]
    fn dealloc_underflow_saturates_at_zero() {
        let log = "{\"kind\":\"dealloc\",\"site\":\"0xA\",\"size\":999}\n";
        let rows = reduce_lines(lines_iter(log)).unwrap();
        assert_eq!(rows[0].peak_live_bytes, 0);
        assert_eq!(rows[0].dealloc_count, 1);
    }

    #[test]
    fn write_csv_has_header_and_one_row_per_site() {
        let log = "{\"kind\":\"alloc\",\"site\":\"0xA\",\"size\":1,\"ts_ns\":0}\n";
        let rows = reduce_lines(lines_iter(log)).unwrap();
        let mut out: Vec<u8> = Vec::new();
        write_csv(&rows, &mut out).unwrap();
        let s = String::from_utf8(out).unwrap();
        assert!(s.starts_with("site,alloc_count,"));
        assert!(s.contains("0xA,1,0,1,"));
    }

    #[test]
    fn write_pretty_emits_aligned_columns() {
        let log = "{\"kind\":\"alloc\",\"site\":\"0xA\",\"size\":1}\n";
        let rows = reduce_lines(lines_iter(log)).unwrap();
        let mut out: Vec<u8> = Vec::new();
        write_pretty(&rows, &mut out).unwrap();
        let s = String::from_utf8(out).unwrap();
        assert!(s.lines().next().unwrap().starts_with("site"));
        assert!(s.lines().nth(1).unwrap().starts_with("0xA"));
    }
}
