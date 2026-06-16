#!/usr/bin/env bash
# Pinned Linux re-run of `snmalloc-rs/benches/profile_bench.rs` for
# heap-profiling perf ticket 86aj0jg36.
#
# macOS bench variance (~4-7% sigma on M4 Pro) is larger than the residual
# overhead the ticket needs to call a verdict on (<1% idle ratio). This
# script reproduces the ticket's recipe on a bare-metal Linux desktop:
#
#   1. Save host state (cpufreq governor per cpu, boost flag, SMT sibling
#      online state) so the cleanup trap can restore it after the run.
#   2. Lock the cpufreq governor of the target CPU (default cpu0) to
#      "performance", disable turbo/boost, and offline the SMT sibling
#      so the bench thread owns its physical core's L1/L2 alone.
#   3. Best-effort stop noisy systemd services (snapd, packagekit,
#      unattended-upgrades, cron) for the duration of the run.
#   4. Run `cargo bench --features profiling --bench profile_bench` with
#      the bench thread pinned via `taskset -c <cpu>`.
#   5. Walk `target/criterion/*/new/estimates.json` and emit:
#        - <out>/linux-pinned-bench.log        — full bench stderr+stdout
#        - <out>/linux-pinned-bench.json       — extracted medians/means/CIs
#        - <out>/linux-pinned-bench.md         — paste-ready markdown
#        - <out>/linux-pinned-bench-host.txt   — host fingerprint (CPU,
#                                                kernel, glibc, rustc,
#                                                governor used at run time)
#   6. Restore the host state captured at step 1, regardless of how the
#      run exits (success, bench fail, Ctrl-C).
#
# Usage:
#   sudo ./snmalloc-rs/scripts/run-linux-pinned-bench.sh             # defaults
#   sudo ./snmalloc-rs/scripts/run-linux-pinned-bench.sh --cpu 2     # pin cpu2 instead
#   sudo ./snmalloc-rs/scripts/run-linux-pinned-bench.sh --out /tmp/bench
#   sudo ./snmalloc-rs/scripts/run-linux-pinned-bench.sh --no-offline-smt
#   sudo ./snmalloc-rs/scripts/run-linux-pinned-bench.sh --skip-setup    # bench only
#   sudo ./snmalloc-rs/scripts/run-linux-pinned-bench.sh --dry-run       # print actions
#
# Why sudo: governor changes, boost flag toggling, and SMT sibling
# offline/online all write to /sys files that require root. The cargo
# build itself runs as the invoking user via `sudo -u` so the target
# tree is owned correctly.
#
# Exit codes:
#   0  bench completed and summary written
#   1  pre-flight failed (wrong OS, missing tool, not running as root)
#   2  state save failed (couldn't read /sys)
#   3  bench failed (cargo returned non-zero)
#   4  extract step failed (estimates.json missing or unparseable)

set -euo pipefail

# ---------------------------------------------------------------------------
# Defaults and CLI parsing.
# ---------------------------------------------------------------------------

CPU=0
OFFLINE_SMT=1
SKIP_SETUP=0
DRY_RUN=0
OUT_DIR=""

usage() {
    sed -n '2,/^$/p' "$0" | sed 's/^# //; s/^#$//'
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --cpu) CPU="$2"; shift 2 ;;
        --out|--output-dir) OUT_DIR="$2"; shift 2 ;;
        --no-offline-smt) OFFLINE_SMT=0; shift ;;
        --skip-setup) SKIP_SETUP=1; shift ;;
        --dry-run) DRY_RUN=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "unknown arg: $1" >&2; usage >&2; exit 1 ;;
    esac
done

# ---------------------------------------------------------------------------
# Pre-flight.
# ---------------------------------------------------------------------------

log()  { printf '[%(%H:%M:%S)T] %s\n' -1 "$*" >&2; }
die()  { log "FATAL: $*"; exit "${2:-1}"; }
# eval is required: callers pass strings with shell redirection (e.g.
# `run "echo X > /sys/.../knob"`) that --dry-run needs to print verbatim.
# shellcheck disable=SC2294
run()  { if [[ $DRY_RUN -eq 1 ]]; then log "DRY: $*"; else eval "$@"; fi; }

[[ "$(uname -s)" == "Linux" ]] || die "Linux-only (saw $(uname -s))"
[[ $EUID -eq 0 ]] || die "must run as root (sudo). cpufreq/boost/SMT writes need it."

REAL_USER="${SUDO_USER:-$USER}"
[[ "$REAL_USER" != "root" ]] || \
    die "could not detect non-root invoker — set SUDO_USER or invoke via sudo"

command -v cargo   >/dev/null || die "cargo not in PATH (PATH=$PATH)"
command -v python3 >/dev/null || die "python3 not in PATH"
command -v taskset >/dev/null || die "taskset not in PATH (install util-linux)"

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${here}/../.." && pwd)"
[[ -f "${repo_root}/snmalloc-rs/Cargo.toml" ]] || \
    die "expected snmalloc-rs at ${repo_root}/snmalloc-rs"

if [[ -z "$OUT_DIR" ]]; then
    OUT_DIR="${repo_root}/snmalloc-rs/bench-results"
fi
mkdir -p "$OUT_DIR"

# Sanity: the requested CPU must be present in /sys.
[[ -d "/sys/devices/system/cpu/cpu${CPU}" ]] || \
    die "cpu${CPU} not present (try --cpu N for a CPU that exists)"

# Detect SMT sibling of $CPU. /sys exposes it as a CSV under
# thread_siblings_list (e.g. "0,1" means cpu0 and cpu1 share a core).
SIBLING=""
sib_list="/sys/devices/system/cpu/cpu${CPU}/topology/thread_siblings_list"
if [[ -r "$sib_list" ]]; then
    while IFS= read -r tok; do
        if [[ "$tok" != "$CPU" ]]; then
            SIBLING="$tok"
            break
        fi
    done < <(tr ',' '\n' < "$sib_list" | tr -d ' ')
fi

# Detect boost knob (Intel uses no_turbo, AMD uses boost).
BOOST_FILE=""
BOOST_DISABLE_VAL=""
BOOST_RESTORE_VAL=""
if [[ -w /sys/devices/system/cpu/intel_pstate/no_turbo ]]; then
    BOOST_FILE="/sys/devices/system/cpu/intel_pstate/no_turbo"
    BOOST_DISABLE_VAL=1
elif [[ -w /sys/devices/system/cpu/cpufreq/boost ]]; then
    BOOST_FILE="/sys/devices/system/cpu/cpufreq/boost"
    BOOST_DISABLE_VAL=0
fi

# ---------------------------------------------------------------------------
# State capture.
# ---------------------------------------------------------------------------

STATE_FILE="$(mktemp -t snmalloc-bench-state.XXXXXX.sh)"
log "saving host state to ${STATE_FILE}"

{
    echo "#!/usr/bin/env bash"
    echo "# Auto-generated by run-linux-pinned-bench.sh. Restores host state."
    echo "set -e"

    # Save governor for every cpu.
    for gov in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
        [[ -r "$gov" ]] || continue
        cur="$(cat "$gov")"
        echo "echo ${cur} > ${gov}"
    done

    if [[ -n "$BOOST_FILE" ]]; then
        BOOST_RESTORE_VAL="$(cat "$BOOST_FILE")"
        echo "echo ${BOOST_RESTORE_VAL} > ${BOOST_FILE}"
    fi

    if [[ -n "$SIBLING" && -r "/sys/devices/system/cpu/cpu${SIBLING}/online" ]]; then
        sib_state="$(cat "/sys/devices/system/cpu/cpu${SIBLING}/online")"
        echo "echo ${sib_state} > /sys/devices/system/cpu/cpu${SIBLING}/online"
    fi

    # Service restore list — only set if we successfully stopped them later.
    echo "true # service restores appended at stop-time"
} > "$STATE_FILE"
chmod +x "$STATE_FILE"

restore() {
    log "restoring host state from ${STATE_FILE}"
    [[ $DRY_RUN -eq 1 ]] || bash "$STATE_FILE" || \
        log "WARN: some restore steps failed; inspect ${STATE_FILE} manually"
    log "restore done"
}
trap restore EXIT

# ---------------------------------------------------------------------------
# Setup.
# ---------------------------------------------------------------------------

apply_setup() {
    log "applying perf-pinning setup (cpu=${CPU}, sibling=${SIBLING:-none}, boost=${BOOST_FILE:-none})"

    # Set every cpu's governor to performance. We could just lock cpu${CPU}
    # but neighbour cores ramping under load can still steal frequency
    # budget on some platforms — safer to lock all.
    for gov in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
        [[ -w "$gov" ]] || continue
        run "echo performance > ${gov}"
    done

    if [[ -n "$BOOST_FILE" ]]; then
        run "echo ${BOOST_DISABLE_VAL} > ${BOOST_FILE}"
    fi

    if [[ $OFFLINE_SMT -eq 1 && -n "$SIBLING" ]]; then
        sib_online="/sys/devices/system/cpu/cpu${SIBLING}/online"
        if [[ -w "$sib_online" ]]; then
            run "echo 0 > ${sib_online}"
            log "offlined SMT sibling cpu${SIBLING}"
        fi
    fi

    # Best-effort: stop noisy services. Append restore steps to STATE_FILE
    # so the trap restarts whatever we stopped.
    for svc in snapd packagekit unattended-upgrades cron crond; do
        if systemctl is-active --quiet "$svc" 2>/dev/null; then
            run "systemctl stop ${svc}" || true
            echo "systemctl start ${svc} || true" >> "$STATE_FILE"
            log "stopped ${svc} (will restart on exit)"
        fi
    done
}

if [[ $SKIP_SETUP -eq 0 ]]; then
    apply_setup
else
    log "--skip-setup: leaving host state as-is"
fi

# Sleep a beat so the governor + boost changes settle, then snapshot what
# we *actually* ended up with.
sleep 1
GOV_ACTUAL="$(cat "/sys/devices/system/cpu/cpu${CPU}/cpufreq/scaling_governor" 2>/dev/null || echo unknown)"
FREQ_ACTUAL_KHZ="$(cat "/sys/devices/system/cpu/cpu${CPU}/cpufreq/scaling_cur_freq" 2>/dev/null || echo 0)"
BOOST_ACTUAL="$(cat "${BOOST_FILE}" 2>/dev/null || echo n/a)"
SIB_ONLINE_ACTUAL="$(cat "/sys/devices/system/cpu/cpu${SIBLING}/online" 2>/dev/null || echo n/a)"

# ---------------------------------------------------------------------------
# Host fingerprint — captured before the bench so it lines up with the run.
# ---------------------------------------------------------------------------

HOST_TXT="${OUT_DIR}/linux-pinned-bench-host.txt"
{
    echo "# Host fingerprint — captured $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "hostname:        $(hostname)"
    echo "uname:           $(uname -srvm)"
    echo "kernel:          $(uname -r)"
    echo "cpu_model:       $(grep -m1 'model name' /proc/cpuinfo | sed 's/^[^:]*: *//')"
    echo "logical_cpus:    $(nproc)"
    echo "ram_kB:          $(grep -m1 MemTotal /proc/meminfo | awk '{print $2}')"
    echo "rustc:           $(sudo -u "$REAL_USER" rustc --version 2>/dev/null || echo missing)"
    echo "cargo:           $(sudo -u "$REAL_USER" cargo --version 2>/dev/null || echo missing)"
    echo "glibc:           $(ldd --version | head -n1)"
    echo "----"
    echo "bench_cpu:       ${CPU}"
    echo "smt_sibling:     ${SIBLING:-none}"
    echo "smt_offlined:    $([[ $OFFLINE_SMT -eq 1 ]] && echo yes || echo no) (sibling online=${SIB_ONLINE_ACTUAL})"
    echo "governor:        ${GOV_ACTUAL}"
    echo "cur_freq_kHz:    ${FREQ_ACTUAL_KHZ}"
    echo "boost_setting:   ${BOOST_FILE:-(none)} = ${BOOST_ACTUAL}"
    echo "----"
    echo "bench_settings:  small=50samples/5s, medium+mixed=200samples/20s (per profile_bench.rs after ticket 86aj0jg36)"
} > "$HOST_TXT"
log "wrote host fingerprint to ${HOST_TXT}"

# ---------------------------------------------------------------------------
# Run the bench.
# ---------------------------------------------------------------------------

LOG_FILE="${OUT_DIR}/linux-pinned-bench.log"
log "running bench (pinned to cpu${CPU}); log → ${LOG_FILE}"

# Cargo runs as the invoking user. We pass through the bench's target
# directory by using the workspace root as cwd so criterion writes to
# the canonical target/criterion/ tree.
# shellcheck disable=SC2054
# (the commas in --preserve-env=HOME,PATH,CARGO_HOME,RUSTUP_HOME are part of
#  one sudo flag value, not array separators — false positive.)
BENCH_CMD=(taskset -c "$CPU" \
    sudo -u "$REAL_USER" --preserve-env=HOME,PATH,CARGO_HOME,RUSTUP_HOME \
    cargo bench --features profiling --bench profile_bench)

cd "$repo_root"

if [[ $DRY_RUN -eq 1 ]]; then
    log "DRY: ${BENCH_CMD[*]}"
else
    if ! "${BENCH_CMD[@]}" 2>&1 | tee "$LOG_FILE"; then
        die "cargo bench failed; see ${LOG_FILE}" 3
    fi
fi

# ---------------------------------------------------------------------------
# Extract — walk target/criterion/{group}/{variant}/new/estimates.json and
# emit a paste-ready summary + JSON. Use python3 because every glibc-modern
# linux desktop ships it and jq parsing of nested estimate objects is awkward.
# ---------------------------------------------------------------------------

JSON_FILE="${OUT_DIR}/linux-pinned-bench.json"
MD_FILE="${OUT_DIR}/linux-pinned-bench.md"

# Locate the criterion tree. `cargo metadata` is the authoritative source.
TARGET_DIR="$(sudo -u "$REAL_USER" --preserve-env=HOME,PATH,CARGO_HOME,RUSTUP_HOME \
    cargo metadata --format-version 1 --no-deps 2>/dev/null \
    | python3 -c 'import sys,json;print(json.load(sys.stdin)["target_directory"])')"
CRIT_DIR="${TARGET_DIR}/criterion"
[[ -d "$CRIT_DIR" ]] || die "criterion tree not found at ${CRIT_DIR}" 4

if [[ $DRY_RUN -eq 0 ]]; then
    log "extracting estimates → ${JSON_FILE} + ${MD_FILE}"

    python3 - "$CRIT_DIR" "$JSON_FILE" "$MD_FILE" "$HOST_TXT" <<'PY' || die "extract failed" 4
import json, sys, os, pathlib

crit_dir, json_out, md_out, host_txt = sys.argv[1:5]
groups   = ["small_allocs", "medium_allocs", "mixed"]
variants = ["profile-off", "profile-on-inactive", "profile-on-active"]

data = {"groups": {}}

for g in groups:
    data["groups"][g] = {}
    for v in variants:
        est_path = pathlib.Path(crit_dir) / g / v / "new" / "estimates.json"
        if not est_path.is_file():
            print(f"WARN: missing {est_path}", file=sys.stderr)
            continue
        with open(est_path) as f:
            e = json.load(f)
        data["groups"][g][v] = {
            "median_ns":  e["median"]["point_estimate"],
            "mean_ns":    e["mean"]["point_estimate"],
            "std_ns":     e["std_dev"]["point_estimate"],
            "ci_lower_ns": e["mean"]["confidence_interval"]["lower_bound"],
            "ci_upper_ns": e["mean"]["confidence_interval"]["upper_bound"],
        }

def ratio(num, den):
    if not den:
        return None
    return num / den

for g in groups:
    cells = data["groups"].get(g, {})
    off = cells.get("profile-off", {}).get("median_ns")
    idle = cells.get("profile-on-inactive", {}).get("median_ns")
    active = cells.get("profile-on-active", {}).get("median_ns")
    data["groups"][g]["ratios_median"] = {
        "ratio_idle":   ratio(idle, off),
        "ratio_active": ratio(active, off),
    }

with open(json_out, "w") as f:
    json.dump(data, f, indent=2)

host_meta = pathlib.Path(host_txt).read_text() if pathlib.Path(host_txt).is_file() else ""

def fmt(x, prec=2):
    return "—" if x is None else f"{x:.{prec}f}"

with open(md_out, "w") as f:
    f.write("# Linux pinned bench — ticket 86aj0jg36\n\n")
    f.write("Per-allocation latency measured by\n")
    f.write("`snmalloc-rs/benches/profile_bench.rs`.\n")
    f.write("Bench thread pinned via `taskset -c <cpu>` with cpufreq\n")
    f.write("governor=performance, boost disabled, SMT sibling offlined.\n\n")
    f.write("## Host fingerprint\n\n```\n")
    f.write(host_meta)
    f.write("```\n\n## Raw medians (paste-ready)\n\n")

    for g in groups:
        f.write(f"### `{g}`\n\n")
        f.write("| Variant | Median (ns) | Mean (ns) | Std (ns) | CI95 lower | CI95 upper |\n")
        f.write("|---|---:|---:|---:|---:|---:|\n")
        for v in variants:
            c = data["groups"][g].get(v, {})
            if not c:
                continue
            f.write(
                f"| {v} | {fmt(c.get('median_ns'))} | {fmt(c.get('mean_ns'))} | "
                f"{fmt(c.get('std_ns'))} | {fmt(c.get('ci_lower_ns'))} | "
                f"{fmt(c.get('ci_upper_ns'))} |\n"
            )
        f.write("\n")

    f.write("## Ratios (median-based)\n\n")
    f.write("`ratio_idle = median(profile-on-inactive) / median(profile-off)`\n\n")
    f.write("`ratio_active = median(profile-on-active) / median(profile-off)`\n\n")
    f.write("| Group | ratio_idle | ratio_active |\n")
    f.write("|---|---:|---:|\n")
    for g in groups:
        r = data["groups"][g].get("ratios_median", {})
        f.write(f"| {g} | {fmt(r.get('ratio_idle'), 4)} | {fmt(r.get('ratio_active'), 4)} |\n")
    f.write("\n")
    f.write("**Verdict gate** (per ticket 86aj0jg36): if every ratio_idle\n")
    f.write("and ratio_active is < 1.01, perf ticket 86aj0hfmc closes.\n")
PY
fi

log "done. outputs:"
log "  ${HOST_TXT}"
log "  ${LOG_FILE}"
log "  ${JSON_FILE}"
log "  ${MD_FILE}"
log "  (criterion HTML reports under ${CRIT_DIR}/*)"
