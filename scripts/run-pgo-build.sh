#!/usr/bin/env bash
# Two-stage PGO build of snmalloc.
#
# Stage 1 (generate)
#   * Configures a build with -fprofile-generate=<dir>.
#   * Builds snmalloc and the func-profile_overhead test, the training
#     workload. It is a self-contained C++ binary, so no Rust toolchain is
#     needed, it exercises both the alloc fast path and the sampling slow
#     path, and it finishes in seconds.
#     For more training data, add binaries to EXTRA_TRAINING_BINS below.
#     Anything built in stage 1 and run before stage 2 feeds the profile.
#   * Runs the workloads, which write .profraw / .gcda into the PGO data
#     directory.
#
# Stage 2 (use)
#   * Merges the .profraw files with llvm-profdata (clang), or uses the
#     .gcda tree in place (gcc).
#   * Configures a second build with -fprofile-use=<file|dir>.
#
# Usage:
#   scripts/run-pgo-build.sh [--gen-dir DIR] [--use-dir DIR] [--profdata FILE]
#
# All paths are optional; sensible defaults under build-pgo-gen / build-pgo-use
# in the repo root are used when unset.

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${here}/.." && pwd)"

# Default directories. Environment variables (PGO_STAGE1_DIR,
# PGO_STAGE2_DIR, PGO_PROFILE_FILE) override these, and CLI flags override
# the environment variables.
gen_build_dir="${PGO_STAGE1_DIR:-${repo_root}/build-pgo-gen}"
use_build_dir="${PGO_STAGE2_DIR:-${repo_root}/build-pgo-use}"
profile_data_dir="${PGO_PROFILE_DATA_DIR:-${gen_build_dir}/pgo-data}"
profile_merged_file="${PGO_PROFILE_FILE:-${gen_build_dir}/pgo.profdata}"

# Extra cmake flags for both stages. CI sets SNMALLOC_RUST_SUPPORT=ON here so
# stage 2 produces libsnmallocshim-rust.a for upload.
extra_cmake_flags="${PGO_EXTRA_CMAKE_FLAGS:-}"

usage() {
  cat <<EOF
Usage: $(basename "$0") [options]

Options:
  --gen-dir DIR      Build directory for the generate stage
                     (default: ${gen_build_dir})
  --use-dir DIR      Build directory for the use stage
                     (default: ${use_build_dir})
  --data-dir DIR     Where .profraw / .gcda files are written
                     (default: ${profile_data_dir})
  --profdata FILE    Where the merged .profdata is written (clang only)
                     (default: ${profile_merged_file})
  --skip-stage1      Skip configure + build + train of the generate stage
                     (use when you already have a populated data dir).
  --skip-stage2      Skip configure + build of the use stage.
  --help             Show this help.

The script will detect whether CC/CXX point at clang or gcc and choose
the right profile-merge path automatically. MSVC is not supported.

Environment variables (used when the matching CLI flag is not passed):
  PGO_STAGE1_DIR         Stage-1 (generate) build directory.
  PGO_STAGE2_DIR         Stage-2 (use) build directory.
  PGO_PROFILE_DATA_DIR   Directory for .profraw / .gcda data.
  PGO_PROFILE_FILE       Merged .profdata file (clang only).
  PGO_EXTRA_CMAKE_FLAGS  Extra flags for both cmake configure steps, for
                         example "-DSNMALLOC_RUST_SUPPORT=ON" to build
                         libsnmallocshim-rust.a in stage 2.
EOF
}

skip_stage1=0
skip_stage2=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --gen-dir)   gen_build_dir="$2"; shift 2 ;;
    --use-dir)   use_build_dir="$2"; shift 2 ;;
    --data-dir)  profile_data_dir="$2"; shift 2 ;;
    --profdata)  profile_merged_file="$2"; shift 2 ;;
    --skip-stage1) skip_stage1=1; shift ;;
    --skip-stage2) skip_stage2=1; shift ;;
    --help|-h)   usage; exit 0 ;;
    *) echo "Unknown argument: $1" >&2; usage; exit 2 ;;
  esac
done

# Detect the compiler family from CXX / CC, defaulting to c++. This only
# decides whether llvm-profdata runs between the stages.
cxx_bin="${CXX:-c++}"
if "${cxx_bin}" --version 2>/dev/null | grep -qiE "clang"; then
  compiler_family="clang"
elif "${cxx_bin}" --version 2>/dev/null | grep -qiE "free software foundation|gcc"; then
  compiler_family="gcc"
else
  echo "Could not determine compiler family for '${cxx_bin}'." >&2
  echo "Set CC/CXX explicitly to clang++ or g++." >&2
  exit 1
fi
echo "[pgo] detected compiler family: ${compiler_family}"

# Training binaries built in stage 1 and run to fill the profile data
# directory. Paths are relative to the generate build directory.
EXTRA_TRAINING_BINS=()
# snmalloc names tests func-<name>-{check,fast}. Train on -fast: it skips the
# extra validation work, so it matches what a production caller links.
TRAINING_BINS=("func-profile_overhead-fast")

run_stage1() {
  echo "[pgo] stage 1: configure (${gen_build_dir})"
  # shellcheck disable=SC2086 # extra_cmake_flags is intentionally word-split
  cmake \
    -S "${repo_root}" \
    -B "${gen_build_dir}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DSNMALLOC_PROFILE=ON \
    -DSNMALLOC_PROFILE_PGO=generate \
    -DSNMALLOC_PGO_PROFILE_DIR="${profile_data_dir}" \
    ${extra_cmake_flags}

  echo "[pgo] stage 1: build"
  # Build snmalloc and every training binary. Not `--target all`, so a
  # missing optional dependency still leaves the binaries we need.
  local build_targets=()
  for t in "${TRAINING_BINS[@]}" "${EXTRA_TRAINING_BINS[@]}"; do
    build_targets+=(--target "${t}")
  done
  if [[ ${#build_targets[@]} -eq 0 ]]; then
    cmake --build "${gen_build_dir}"
  else
    # cmake --build only accepts one --target group; pass them together.
    cmake --build "${gen_build_dir}" "${build_targets[@]}"
  fi

  echo "[pgo] stage 1: train (writing into ${profile_data_dir})"
  mkdir -p "${profile_data_dir}"
  # LLVM reads LLVM_PROFILE_FILE. The template keeps concurrent processes
  # from overwriting each other: %m = binary signature, %p = pid.
  export LLVM_PROFILE_FILE="${profile_data_dir}/default_%m_%p.profraw"
  for bin in "${TRAINING_BINS[@]}" "${EXTRA_TRAINING_BINS[@]}"; do
    local bin_path
    bin_path="$(find "${gen_build_dir}" -type f -name "${bin}" -perm -u+x | head -n1 || true)"
    if [[ -z "${bin_path}" ]]; then
      echo "[pgo] stage 1: training binary '${bin}' not found under ${gen_build_dir}; skipping" >&2
      continue
    fi
    echo "[pgo]   running ${bin_path}"
    "${bin_path}"
  done

  if [[ "${compiler_family}" = "clang" ]]; then
    echo "[pgo] stage 1: llvm-profdata merge -> ${profile_merged_file}"
    local profdata_bin
    profdata_bin="$(command -v llvm-profdata || true)"
    if [[ -z "${profdata_bin}" ]]; then
      # Apple toolchains ship llvm-profdata via xcrun rather than on PATH.
      if command -v xcrun >/dev/null 2>&1; then
        profdata_bin="$(xcrun -f llvm-profdata 2>/dev/null || true)"
      fi
    fi
    if [[ -z "${profdata_bin}" ]]; then
      echo "[pgo] llvm-profdata not found; install LLVM (or 'xcrun -f llvm-profdata' on macOS) and retry" >&2
      exit 1
    fi
    # `find … -print0 | xargs -0` handles odd filenames and long lists.
    find "${profile_data_dir}" -name '*.profraw' -print0 \
      | xargs -0 "${profdata_bin}" merge -o "${profile_merged_file}"
    echo "[pgo] stage 1: merged $(find "${profile_data_dir}" -name '*.profraw' | wc -l | tr -d ' ') .profraw files"
  else
    # gcc reads .gcda directly from the data dir; no merge step.
    echo "[pgo] stage 1: gcc workflow, .gcda files left in place under ${profile_data_dir}"
  fi
}

run_stage2() {
  echo "[pgo] stage 2: configure (${use_build_dir})"
  # shellcheck disable=SC2086 # extra_cmake_flags is intentionally word-split
  if [[ "${compiler_family}" = "clang" ]]; then
    cmake \
      -S "${repo_root}" \
      -B "${use_build_dir}" \
      -DCMAKE_BUILD_TYPE=Release \
      -DSNMALLOC_PROFILE=ON \
      -DSNMALLOC_PROFILE_PGO=use \
      -DSNMALLOC_PGO_PROFILE_FILE="${profile_merged_file}" \
      ${extra_cmake_flags}
  else
    cmake \
      -S "${repo_root}" \
      -B "${use_build_dir}" \
      -DCMAKE_BUILD_TYPE=Release \
      -DSNMALLOC_PROFILE=ON \
      -DSNMALLOC_PROFILE_PGO=use \
      -DSNMALLOC_PGO_PROFILE_DIR="${profile_data_dir}" \
      ${extra_cmake_flags}
  fi

  echo "[pgo] stage 2: build"
  cmake --build "${use_build_dir}"
  echo "[pgo] done. Optimized artifacts under ${use_build_dir}"
}

if [[ "${skip_stage1}" -eq 0 ]]; then
  run_stage1
else
  echo "[pgo] skipping stage 1 (--skip-stage1)"
fi

if [[ "${skip_stage2}" -eq 0 ]]; then
  run_stage2
else
  echo "[pgo] skipping stage 2 (--skip-stage2)"
fi
