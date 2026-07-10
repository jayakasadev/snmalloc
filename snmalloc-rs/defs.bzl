# Reusable macro for instantiating an snmalloc-rs profiling variant against a
# caller-supplied crate universe.
#
# snmalloc's own MODULE.bazel `crate_universe` extension (flate2/backtrace)
# is `dev_dependency = True`, so `@crates` is invisible to downstream
# consumers (bzlmod skips dev_dependency extensions entirely for non-root
# modules). Consumers that need the `profiling`/`symbolicate` Cargo features
# therefore can't just depend on a fixed target wired to `@crates` — they
# call this macro from their OWN BUILD.bazel, passing labels from their OWN
# crate universe, which instantiates a private copy of the rust_library
# bound to those labels instead.
#
# `snmalloc-rs/BUILD.bazel`'s `:snmalloc_rs_profiling` /
# `:snmalloc_rs_profiling_symbolicated` targets use this same macro
# internally (bound to this repo's own `@crates`, for its own tests/CI) —
# see that file for the fixed, non-reusable instantiations.
#
# All source-file labels use `Label(...)`, not bare strings: a macro's bare
# relative labels resolve against the CALLING package (e.g. a downstream
# consumer's BUILD.bazel), not the package that defines this .bzl file.
# `Label(...)` anchors to snmalloc-rs's own package regardless of caller.

load("@rules_rust//rust:defs.bzl", "rust_library")

def snmalloc_rs_profiling_library(
        name,
        crate_features,
        flate2_dep = None,
        backtrace_dep = None,
        **kwargs):
    """Instantiates an snmalloc-rs `rust_library` for a profiling feature combo.

    Args:
        name: target name.
        crate_features: Cargo features to enable — some combination of
            `profiling` and `symbolicate`. `symbolicate` requires
            `backtrace_dep` to be set.
        flate2_dep: label of the caller's own `flate2` crate (needed by the
            `profiling` feature's `HeapProfile::write_pprof_gz`). Required
            if `"profiling"` is in `crate_features`.
        backtrace_dep: label of the caller's own `backtrace` crate (needed
            by the `symbolicate` feature). Required if `"symbolicate"` is in
            `crate_features`.
        **kwargs: forwarded to `rust_library` (e.g. `visibility`).
    """
    if "profiling" in crate_features and not flate2_dep:
        fail("snmalloc_rs_profiling_library: crate_features includes " +
             "'profiling', which needs flate2_dep set")
    if "symbolicate" in crate_features and not backtrace_dep:
        fail("snmalloc_rs_profiling_library: crate_features includes " +
             "'symbolicate', which needs backtrace_dep set")

    extra_deps = []
    if flate2_dep:
        extra_deps.append(flate2_dep)
    if backtrace_dep:
        extra_deps.append(backtrace_dep)

    rust_library(
        name = name,
        srcs = [Label("//snmalloc-rs:lib_srcs")],
        crate_features = crate_features,
        crate_name = "snmalloc_rs",
        crate_root = Label("//snmalloc-rs:src/lib.rs"),
        edition = "2021",
        deps = [Label("//snmalloc-rs/snmalloc-sys:snmalloc_sys_profiling")] + extra_deps,
        **kwargs
    )
