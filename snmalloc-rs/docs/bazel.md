# Bazel integration cookbook

How to collect snmalloc-rs heap profiles from a Bazel-built binary or
`rust_test`. It assumes you know the `profiling` Cargo feature; see
`snmalloc-rs/README.md` for the API.

## Profile-output path resolution

`snmalloc_rs::profile::default_output_path()` (needs the `profiling`
feature) returns a `PathBuf`. It checks these in order and uses the
first match:

1. **`SNMALLOC_PROFILE_OUT`** — used verbatim. Wire it into a
   `--test_env=` flag (`bazel test`) or `--action_env=` flag
   (`bazel run`) to redirect output without recompiling.
2. **`$TEST_UNDECLARED_OUTPUTS_DIR/heap.folded`** — Bazel's per-test
   scratch directory. Under `bazel test`, Bazel collects anything
   written there as a test artefact, so the file lands in `outputs.zip`
   and shows up in the Build Event Service (BES) UI with no extra
   wiring.
3. **`$TMPDIR/heap_{pid}.folded`** — fallback for plain `cargo run`,
   `cargo test`, or interactive `bazel run`. The PID keeps concurrent
   processes from overwriting each other.

Only `SNMALLOC_PROFILE_OUT` uses the literal path you give; the other
two build a filename for you. Writing pprof or another format? Call
`with_extension(...)` on the result. The default `.folded` suffix
matches what `HeapProfile::write_flamegraph` emits.

## BES upload size considerations

Bazel's Build Event Service uploads test artefacts on every
`bazel test`. Per-file upload caps are common — often around 10 MiB —
and larger profiles are truncated or rejected depending on the backend.
So:

- The default 512 KiB sampling rate keeps a folded-stack profile small
  for workloads running a few minutes. For longer runs, raise the rate
  (`SNMALLOC_PROFILE_RATE=2097152` for 2 MiB) to bound the output.
- For very long runs, rotate the output: snapshot every N seconds into
  `$TEST_UNDECLARED_OUTPUTS_DIR/heap_{N}.folded` and stitch the files
  downstream. `default_output_path` resolves a single path; rotation is
  a `with_file_name()` away.
- Gzipped pprof (`HeapProfile::write_pprof_gz`) is smaller than folded
  output. If you already collect pprof, prefer the gzipped form here.

## Example `BUILD.bazel` snippet

The smallest opt-in for a `rust_test` that dumps a heap profile on exit:

```python
load("@rules_rust//rust:defs.bzl", "rust_test")

rust_test(
    name = "my_heap_profile_test",
    srcs = ["tests/my_heap_profile_test.rs"],
    edition = "2021",
    deps = [
        "//snmalloc-rs:snmalloc_rs",
    ],
    # Opt the test into snmalloc's heap profiler at a 256 KiB
    # sampling rate.  `SNMALLOC_PROFILE_OUT` is left unset so the
    # path-resolution chain falls through to TEST_UNDECLARED_OUTPUTS_DIR,
    # which Bazel auto-uploads as a test artefact.
    env = {
        "SNMALLOC_PROFILE_ENABLE": "1",
        "SNMALLOC_PROFILE_RATE": "262144",
    },
)
```

To pick the path yourself — say, to feed a downstream `genrule` — add
`SNMALLOC_PROFILE_OUT`:

```python
    env = {
        "SNMALLOC_PROFILE_ENABLE": "1",
        "SNMALLOC_PROFILE_RATE": "262144",
        "SNMALLOC_PROFILE_OUT": "/tmp/explicit_heap.folded",
    },
```

For a `rust_binary` run with `bazel run`, set the same keys on a wrapper
`sh_binary` or pass `--action_env=...` on the command line.
`default_output_path()` reads the process environment at call time, so
the rule kind does not matter.

## Choosing a profiling variant

Three `rust_library` targets cover the profiling matrix. Pick by how the
consumer binary will read the output:

| Target | Cargo features | Output frames | When to use |
| --- | --- | --- | --- |
| `:snmalloc_rs` | _(none)_ | n/a (no profiler) | Default. Cargo `profiling` off, C-side `SNMALLOC_PROFILE` off. |
| `:snmalloc_rs_profiling` | `profiling` | 16-hex-digit raw addresses | Profiler on; symbolize externally with `atos`, `addr2line`, or `llvm-symbolizer`. Smallest dependency footprint. |
| `:snmalloc_rs_profiling_symbolicated` | `profiling`, `symbolicate` | Resolved `function (file:line)` frames | Profiler on and frames resolved in-process at dump time. Drop the pprof into Grafana Pyroscope, Polar Signals, or `go tool pprof -http=:8080 -` and see function names. |

A fourth target, `:snmalloc_rs_profile_compat`, binds the
profile-enabled C archive but leaves the Cargo `profiling` feature off,
so it does not need flate2. Use it from downstream Bazel modules that
cannot resolve `@crates//:flate2` from this fork's `crate_universe`
extension.

### Switching a downstream binary to symbolicated output

Change one `deps` entry in the consumer `BUILD.bazel`:

```python
# Before — operators have to atos every frame by hand:
rust_binary(
    name = "konfig_bin_heapprof",
    srcs = [...],
    deps = [
        "@snmalloc//snmalloc-rs:snmalloc_rs_profiling",
        # ...
    ],
)

# After — frames are resolved at dump time, pprof viewers show
# function names directly:
rust_binary(
    name = "konfig_bin_heapprof",
    srcs = [...],
    deps = [
        "@snmalloc//snmalloc-rs:snmalloc_rs_profiling_symbolicated",
        # ...
    ],
)
```

No API change is needed. `HeapProfile::write_flamegraph` and
`HeapProfile::write_pprof_gz` are the same calls as before; frames that
cannot be resolved (kernel, JIT, stripped code) fall back to 16 hex
digits. See "Symbolicated output" in `snmalloc-rs/README.md` for the
Cargo-side recipe.

### Cost

`backtrace` pulls in `addr2line`, `gimli`, and `object`, and parses the
binary's debug info on first use. If the consumer binary already links
those crates (panic backtraces, `tracing-error`, and so on), the extra
cost is small. If you measure it and it is too high, stay on
`:snmalloc_rs_profiling` and symbolize the raw pprof externally.
