# snmalloc-tools

Command-line tools that join external PMU output (Linux `perf`) with
snmalloc's allocation-site lookup and branch-hint inventory.

This crate automates the workflow in
[`docs/profiling-pmu.md`](../docs/profiling-pmu.md). It wraps
`SnMalloc::lookup_alloc_site`, `HeapProfile::top_sites`, and the
`branch_hints.json` file in a clap CLI.

## Subcommands

```
snmalloc-tools profile-top --input <profile.pb> --n 10
    Print the top N allocation sites.  Note: the rows come from the
    live in-process snapshot, not from --input.  The in-tree pprof
    decoder is not shipped yet; --input is read only so I/O errors
    behave the same way they eventually will.

snmalloc-tools pmu-join cache-misses --perf-script <file> [--top N] [--json]
    Parse `perf script` output; for samples with a data address, look
    up the allocating call site and rank by miss count.

snmalloc-tools pmu-join c2c --perf-c2c <file> [--top N] [--json]
    Parse `perf c2c report --stdio`; group HITM events by cache line
    and print the owning allocation site per line.

snmalloc-tools branch-misses --perf-script <file> --hints <branch_hints.json> [--top N] [--json]
    Parse `perf script` output and cross-reference it with the
    branch-hint inventory.  Hints with a high miss rate are candidates
    for a `LIKELY` <-> `UNLIKELY` swap.

snmalloc-tools rate-report --input <streaming-log.jsonl> [--top N] [--pretty]
    Read a snmalloc streaming event log (JSON Lines) and print one row
    per site: alloc/dealloc counts, peak live bytes, allocations per
    second.  Reads the log as a stream, so memory use scales with the
    number of distinct sites, not the number of events.
```

Every subcommand except `rate-report` prints a plain-text table by
default and accepts `--json`. `rate-report` prints CSV by default and a
fixed-width table with `--pretty`.

## Streaming event-log schema (`rate-report`)

`rate-report` reads **JSON Lines**: UTF-8, one event object per line.
The producer is usually an application using
[`snmalloc_rs::ProfilingSession`](../snmalloc-rs/src/streaming.rs) that
writes each callback to a file.

```jsonl
{"ts_ns": 1000000, "kind": "alloc", "site": "0x55a0c0001000", "size": 4096}
{"ts_ns": 1001000, "kind": "dealloc", "site": "0x55a0c0001000", "size": 4096}
```

Fields:

- `ts_ns` (u64, optional) — monotonic timestamp in nanoseconds. Used as
  the rate denominator; if no record has one, the rate column is `0.0`.
- `kind` (string, required) — `"alloc"`, `"dealloc"`, or `"resize"`.
  Unknown values are skipped.
- `site` (string, required) — the allocation site key, usually the leaf
  frame address as `0x` plus 16 hex digits. Matches the `site_leaf`
  field from the other subcommands.
- `size` (u64, optional) — bytes for this event.

Malformed lines are skipped without an error, so truncated tails and
blank lines are safe. See
`tests/fixtures/streaming_log_sample.jsonl` for a worked example.

## Snapshot vs streaming

`profile-top` reads a `HeapProfile::snapshot()` — the sampled
allocations that are live right now — so it leans toward long-lived
data. `rate-report` reads a streaming log, so it also captures
short-lived churn. See "When to use snapshot vs streaming" in
[`../snmalloc-rs/README.md`](../snmalloc-rs/README.md).

## Live-process limitation (important)

`SnMalloc::lookup_alloc_site` only resolves addresses sampled in the
**current** process; it reads the in-memory `SampledList`, not a saved
snapshot. So `pmu-join cache-misses` and `pmu-join c2c` work in only two
situations:

1. **In-process joiner.** The workload itself calls `snmalloc-tools` as
   a library (see `src/lib.rs`) at the end of the run, while the
   allocations are still live. The integration test
   `cache_miss_joiner_resolves_in_process_allocation` shows the shape:
   hold a live allocation, then feed its address to the joiner.

2. **Replay.** A second process re-runs the same allocation pattern at a
   high enough sampling rate that the addresses line up again. This is
   best-effort; prefer option 1.

Running out-of-process against a pre-recorded perf file from a
*different* process marks every sample unattributed. `pmu-join c2c`
still prints those lines with `site_leaf = "<unattributed>"` so you can
see the HITM counts.

`branch-misses` has no such limitation: the branch-hint inventory is a
static file.

## Fixtures

`tests/fixtures/` holds a small hand-written sample for each parser:

- `perf_script_sample.txt` — three samples: branch-miss (IP only),
  cache-miss (IP only), and a mem-load with a data address.
- `perf_c2c_sample.txt` — two contended cache lines with detail rows.
- `branch_hints_sample.json` — three hint sites in the schema produced
  by `scripts/dump_branch_hints.py`.
- `streaming_log_sample.jsonl` — eight events across two sites covering
  alloc, dealloc, resize, and a peak-then-drop pattern.

`tests/integration.rs` runs each parser against these fixtures.

## Cross-references

- Allocation-site lookup — `src/snmalloc/profile/addr_lookup.h` and
  `snmalloc-rs/src/profile.rs::SnMalloc::lookup_alloc_site`
- Branch-hint inventory — `scripts/dump_branch_hints.py` and the
  `branch_hints_inventory` CMake target
- PMU profiling workflow — `docs/profiling-pmu.md`
