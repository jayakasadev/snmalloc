# PMU profiling with snmalloc

This guide shows how to trace CPU performance-monitoring-unit (PMU)
events — cache misses, false sharing, and branch mispredictions — back
to the snmalloc call sites that caused them.

snmalloc does not sample PMU counters itself. Your operating system's
profiler does that (`perf` on Linux, Instruments on macOS). snmalloc
supplies the metadata needed to **join** those samples with allocator
state:

- an allocation-site lookup API, `snmalloc::lookup_alloc_site(addr)`;
- a branch-hint inventory file, `branch_hints.json`.

The [`snmalloc-tools`](../snmalloc-tools/README.md) CLI automates some
of the joins below. Read its "Live-process limitation" section before
using the cache-miss and false-sharing joiners.

## Overview

| Question | snmalloc API | External tool | `snmalloc-tools` subcommand |
| -------- | ------------ | ------------- | --------------------------- |
| Allocation hot-spots | `HeapProfile::top_sites()` | none | `profile-top` |
| Cache-miss attribution (Linux) | `snmalloc::lookup_alloc_site(addr)` | `perf record -e cache-misses` | `pmu-join cache-misses` |
| False sharing (Linux) | `snmalloc::lookup_alloc_site(addr)` | `perf c2c record` | `pmu-join c2c` |
| Cache-miss attribution (macOS) | `snmalloc::lookup_alloc_site(addr)` | Instruments (System Trace → Counters) | none; join by hand |
| Branch-hint miss rates | `branch_hints.json` | `perf record -e branch-misses` | `branch-misses` |

Each section below is one recipe.

## 1. Allocation hot-spots

snmalloc answers this one on its own. The heap profiler records call
stacks for sampled allocations (see
[Heap Profiling](../README.md#heap-profiling)). `top_sites()` groups
those samples by their leaf frame and returns the heaviest sites by
bytes requested.

### Rust example

```rust
use snmalloc_rs::SnMalloc;

#[global_allocator]
static ALLOC: SnMalloc = SnMalloc;

fn main() {
    SnMalloc::init_profiling_from_env();

    // ... run the workload ...

    let snapshot = SnMalloc::heap_profile().expect("profiling enabled");
    for site in snapshot.top_sites(10) {
        println!(
            "{:>10} bytes  {:>6} samples  {}",
            site.bytes_requested,
            site.sample_count,
            site.leaf_symbol.as_deref().unwrap_or("<unresolved>"),
        );
    }
}
```

### Example output

```
   8.45 MiB     132 samples  my_app::parser::Token::clone
   4.21 MiB      67 samples  my_app::graph::Node::new
   2.10 MiB      33 samples  alloc::vec::Vec::reserve
   ...
```

The numbers are unbiased Poisson estimates of the total bytes requested
through each leaf frame.

Automated by `snmalloc-tools profile-top`.

## 2. Cache-miss attribution (Linux)

`perf` samples the hardware cache-miss counter and records the
instruction pointer and call stack at each sample. Pass the data address
that missed to `lookup_alloc_site(addr)` and you get back the call site
that allocated the chunk containing it.

### Capture

```bash
# Pick the target PID. -p replaces -a if you only want this process.
perf record \
    -e cache-misses \
    --call-graph dwarf \
    -p "$PID" \
    -- sleep 30

perf script > samples.txt
```

`perf script` writes one block per sample: an event header, the data
address, the instruction pointer, and the stack. Not every event carries
a data address — `mem_load_*` events do, raw `cache-misses` may not.

### Join with snmalloc

For each sample whose data address is inside an snmalloc-managed region,
call `snmalloc::lookup_alloc_site(addr)` (from C++, or through the Rust
wrapper) to recover the allocation stack. Pairing the two stacks tells
you who was *reading* the memory and who *allocated* it, which is what
you need to fix a layout problem.

Samples without a data address fall back to instruction-pointer-only
attribution.

Automated by `snmalloc-tools pmu-join cache-misses`.

## 3. False-sharing detection (Linux)

`perf c2c` ("cache-to-cache") finds HITM events — loads served from a
*modified* line in another core's cache — and groups them by cache line.
Lines with many HITMs are your false-sharing suspects.

### Capture

```bash
perf c2c record -a -- ./my-app

# --stdio dumps the full report; the curses TUI is also useful interactively.
perf c2c report --stdio > c2c.txt
```

The "Shared Data Cache Line Table" lists each contended line with its
physical and virtual address, the offsets accessed, and the code
locations involved.

### Join with snmalloc

Pass each contended line's virtual address to
`snmalloc::lookup_alloc_site(addr)`. Because the lookup resolves the
*chunk* containing the address, offsets within a line still resolve to
the allocation site. Two common findings:

- Two struct fields share a line → reorder or pad the struct.
- Two elements of a shared container collide → align the allocation to a
  cache line.

Automated by `snmalloc-tools pmu-join c2c`.

## 4. Cache-miss attribution (macOS)

macOS has no public `perf` equivalent; the kperf framework that drives
per-CPU counters is a private SPI. Use **Instruments**, which needs no
root.

### Capture

1. Launch **Instruments** (ships with Xcode).
2. Choose the **System Trace** template.
3. Add the **Counters** instrument and configure it to sample a
   cache-miss event (`L1D_CACHE_MISS_LD`, `L2_TLB_MISS`, etc. — the
   available names depend on the CPU family).
4. Attach to your process and record.
5. **File → Export…** the trace as XML or a `.trace` package.

### Join with snmalloc

There is no `snmalloc-tools` subcommand for Instruments traces. Read the
exported Counters samples yourself, extract the data addresses and IP
stacks, and pass the addresses to `lookup_alloc_site` as on Linux.

### Limitations

- kperf is a private SPI, so per-process sampling without root is more
  limited than `perf`. Some events are only visible system-wide.
- Data addresses are not exposed for every event on every Apple Silicon
  generation. Without them you only see *who* was missing, not *which
  allocation* they missed on.
- Instruments traces are large; prefer short captures (10–30s).

## 5. Branch-hint miss rates

snmalloc's hot path is annotated with `SNMALLOC_LIKELY` and
`SNMALLOC_UNLIKELY`. A hint whose real probability has drifted from the
source assumption costs a mispredicted branch each time through. The
build emits a `branch_hints.json` file listing every hint site with its
source location and predicted direction.

### Capture

```bash
perf record -e branch-misses -- ./my-app
perf report --stdio --no-children | head -100 > branch-misses.txt
```

Restrict the report to snmalloc symbols to cut the noise:

```bash
perf report --stdio --no-children --symbol-filter='snmalloc' \
    > snmalloc-branch-misses.txt
```

### Join with `branch_hints.json`

One entry per hint site:

```json
{
  "file": "src/snmalloc/mem/freelist.h",
  "line": 412,
  "direction": "LIKELY",
  "symbol": "snmalloc::FreeListBuilder<...>::add"
}
```

For each high-count entry in `branch-misses.txt`, resolve its source
location with `addr2line` against the binary's DWARF and match it
against `branch_hints.json`. A hint site missing more than about 5% of
the time is worth inverting (swap `LIKELY` and `UNLIKELY`) or removing.

Automated by `snmalloc-tools branch-misses`.

## What snmalloc does not do

The allocator itself contains no PMU sampling code. It does not:

- call `perf_event_open`, link libpfm, or arm hardware counters;
- call kperf or any private SPI on macOS;
- register ETW providers for PMU events on Windows;
- learn about cache misses at runtime.

All attribution is offline, after `perf` or Instruments has finished
recording.
