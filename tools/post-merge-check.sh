#!/usr/bin/env bash
# Post-merge verification — runs the matrix described in
# docs/POST_MERGE_TEST_PLAN.md as a single, fail-fast gate.
#
#   make post-merge-check                          # run the full matrix
#   ./tools/post-merge-check.sh                    # or directly
#   POST_MERGE_ONLY=cpp ./tools/post-merge-check.sh        # run one section
#   POST_MERGE_BACKENDS="NOREC TL2" ./tools/post-merge-check.sh  # subset
#
# Sections (POST_MERGE_ONLY=<name>):
#   toolchain   §0  toolchain sanity
#   plugin      §3  LLVM plugin build + 18 instrumented tests
#   cpp         §1  C++ test_tx/test_ds across all explicit-API backends
#   rust        §4  Rust workspace build + tests
#   simulator   §5  Rust simulator tests
#   integrity   §8  merge-integrity grep sweep
#   lint        §9  clang-format + rustfmt + clippy -D warnings (make fmt-check)
#   units       §10 tests/backends/tm_impl unit matrix (all 11 CPU backends)
#   gpu         §11 GPU HIP build + benchmark battery (only if hipcc present)
#   tla         §7  TLA+ TLC smoke (only if the TLC jar is present)
#   gem5        §6  gem5 smoke (only if gem5 is built)
#
# Known pre-existing failures are allowlisted below (EXPECT_FAIL_CPP /
# EXPECT_FAIL_UNITS, keyed review-05 id); an allowlisted test that suddenly
# PASSES is also an error, so the lists cannot silently rot.
#
# `set -euo pipefail` fails on the first hard error; each section is wrapped in
# `timeout` so a hung step can't stall the run. Target: <10 min on CI. The gem5
# portion is skipped unless gem5 is built (and, per plan §9 / F36, would run
# single-threaded `t=1` until the 2-thread livelock is fixed).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

# Per-section timeouts (seconds); override with POST_MERGE_TIMEOUT_<name>.
TOOLCHAIN_T="${POST_MERGE_TIMEOUT_TOOLCHAIN:-90}"
PLUGIN_T="${POST_MERGE_TIMEOUT_PLUGIN:-420}"
CPP_T="${POST_MERGE_TIMEOUT_CPP:-180}"      # per backend
RUST_T="${POST_MERGE_TIMEOUT_RUST:-420}"
SIM_T="${POST_MERGE_TIMEOUT_SIMULATOR:-300}"
INTEGRITY_T="${POST_MERGE_TIMEOUT_INTEGRITY:-60}"

LINT_T="${POST_MERGE_TIMEOUT_LINT:-1200}"
UNITS_T="${POST_MERGE_TIMEOUT_UNITS:-1200}"
GPU_T="${POST_MERGE_TIMEOUT_GPU:-2400}"

ONLY="${POST_MERGE_ONLY:-}"
run_section() { [ -z "$ONLY" ] || [ "$ONLY" = "$1" ]; }

# review-05 allowlists: "<key>"="BUG id". Key = "backend:suite".
declare -A EXPECT_FAIL_CPP=()
declare -A EXPECT_FAIL_UNITS=()
# check <rc> <key> <allowlist-name> — returns 0 (pass or expected-fail) or 1.
check_rc() { # $1=rc $2=key $3=name-of-allow-array
  local rc=$1 key=$2
  local -n _allow=$3
  local id="${_allow[$key]:-}"
  if [ "$rc" = "0" ]; then
    if [ -n "$id" ]; then
      echo "  FAIL: $key passed but is allowlisted as expected-fail ($id) — fix the allowlist"; return 1
    fi
    return 0
  fi
  if [ -n "$id" ]; then
    echo "  $key: FAIL — EXPECTED ($id, pre-existing on HEAD); see review-05/BUGS.md"; return 0
  fi
  return 1
}
step() { printf '\n\033[1m=== %s ===\033[0m\n' "$*"; }

START=$(date +%s)

# ── §0 toolchain sanity ────────────────────────────────────────────────────
if run_section toolchain; then
  step "§0 toolchain sanity"
  timeout "$TOOLCHAIN_T" bash -c '
    rustc  --version
    cargo  --version
    (clang++-22 --version || g++ --version) | head -n1
    if command -v llvm-config-22 >/dev/null; then
      echo "llvm-config-22 $(llvm-config-22 --version)"
    else
      echo "llvm-config-22: NOT FOUND (plugin pipeline needs it)"
    fi
  '
fi

# ── §3 LLVM plugin (build + 18 instrumented tests) ─────────────────────────
if run_section plugin; then
  step "§3 LLVM plugin (build + 18 tests)"
  timeout "$PLUGIN_T" make -C plugin run
fi

# ── §1 C++ explicit-API backends ───────────────────────────────────────────
if run_section cpp; then
  step "§1 C++ backends (test_tx/test_ds)"
  # SGL/LEFTRIGHT/ROMULUS use explicit tm_init/tm_exit → build only (the plan
  # runs their test binaries manually), so they are not auto-run here.
  BACKENDS="${POST_MERGE_BACKENDS:-TINYSTM WBETL WT NOREC NORECBF SWISSTM TL2 TSC_TM MVLOG SGL LEFTRIGHT ROMULUS XTM SPHT TSXSGL GPU_STM_CPU GPUTX GACCO CSMV}"
  for be in $BACKENDS; do
    echo "--- $be ---"
    make -C benchmarks/cpp clean BACKEND="$be" >/dev/null 2>&1 || true
    if ! timeout "$CPP_T" make -C benchmarks/cpp -j4 bin/test_tx bin/test_ds BACKEND="$be" > /tmp/post-merge-$be-build.log 2>&1; then
      echo "  $be: BUILD FAIL (tail:)"; tail -n5 /tmp/post-merge-$be-build.log; exit 1
    fi
    case "$be" in
      SGL|LEFTRIGHT|ROMULUS)
        echo "  $be: build OK (run ./bin/test_tx | ./bin/test_ds manually)" ;;
      *)
        rc=0; timeout 60 ./benchmarks/cpp/bin/test_tx > /tmp/post-merge-$be-tx.log 2>&1 || rc=$?
        check_rc "$rc" "$be:test_tx" EXPECT_FAIL_CPP || { echo "  $be: test_tx FAIL (tail:)"; tail -n5 /tmp/post-merge-$be-tx.log; exit 1; }
        echo "  $be: tx-rc=$rc $(tail -n1 /tmp/post-merge-$be-tx.log)"
        rc=0; timeout 60 ./benchmarks/cpp/bin/test_ds > /tmp/post-merge-$be-ds.log 2>&1 || rc=$?
        check_rc "$rc" "$be:test_ds" EXPECT_FAIL_CPP || { echo "  $be: test_ds FAIL (tail:)"; tail -n5 /tmp/post-merge-$be-ds.log; exit 1; }
        echo "  $be: ds-rc=$rc $(tail -n1 /tmp/post-merge-$be-ds.log)" ;;
    esac
  done
fi

# ── §4 Rust workspace ──────────────────────────────────────────────────────
if run_section rust; then
  step "§4 Rust workspace (build + tests, single-threaded)"
  timeout "$RUST_T" cargo test --manifest-path explicit_api/rust/workspace/Cargo.toml -- --test-threads=1
fi

# ── §5 Simulator ───────────────────────────────────────────────────────────
if run_section simulator; then
  step "§5 Simulator (tests, single-threaded)"
  timeout "$SIM_T" cargo test --manifest-path simulator/Cargo.toml
fi

# ── §8 merge-integrity sweep ───────────────────────────────────────────────
if run_section integrity; then
  step "§8 merge-integrity sweep"
  timeout "$INTEGRITY_T" bash -c '
    set -e
    # expli_instr was renamed to explicit_api; no non-doc file may still
    # reference the old path. (*.md docs that describe the rename are excluded.)
    if git grep -lI expli_instr -- . ":(exclude)*.md" ":(exclude)tools/post-merge-check.sh" 2>/dev/null; then
      echo "FAIL: expli_instr still referenced in a non-doc file (should be explicit_api)"; exit 1
    fi
    echo "  no expli_instr references outside docs: OK"
    [ -d explicit_api ] && echo "  explicit_api/ present: OK"
    if grep -q "explicit_api" benchmarks/cpp/Makefile; then
      echo "  benchmarks/cpp/Makefile -> explicit_api: OK"
    else
      echo "FAIL: benchmarks/cpp/Makefile does not reference explicit_api"; exit 1
    fi
    if [ -L docs/AGENTS.md ]; then echo "  docs/AGENTS.md symlink: OK"; else echo "  docs/AGENTS.md is not a symlink (expected)"; fi
  '
fi

# ── §9 Lint gate (clang-format + rustfmt + clippy) ────────────────────────
if run_section lint; then
  step "§9 Lint (make fmt-check)"
  timeout "$LINT_T" make fmt-check
fi

# ── §10 Unit matrix (tests/backends/tm_impl, all CPU backends) ────────────
if run_section units; then
  step "§10 Unit matrix (tests/backends/tm_impl)"
  UNITS_BACKENDS="${POST_MERGE_UNITS_BACKENDS:-tl2 tinystm wt wbetl norec swisstm xtm gpu_stm_cpu gputx gacco csmv}"
  for be in $UNITS_BACKENDS; do
    rc=0; timeout "$UNITS_T" make -C tests/backends/tm_impl "run-$be" > "/tmp/post-merge-units-$be.log" 2>&1 || rc=$?
    check_rc "$rc" "$be" EXPECT_FAIL_UNITS || { echo "  units $be: FAIL (tail:)"; tail -n8 "/tmp/post-merge-units-$be.log"; exit 1; }
    echo "  units $be: rc=$rc $(tail -n1 "/tmp/post-merge-units-$be.log")"
  done
fi

# ── §11 GPU battery (only if hipcc is present) ────────────────────────────
if run_section gpu; then
  step "§11 GPU battery (gpu/benchmarks, HIP)"
  if command -v hipcc >/dev/null 2>&1 || [ -x /usr/bin/hipcc ]; then
    timeout "$GPU_T" make -C gpu/benchmarks HIP=1 -j4 > /tmp/post-merge-gpu-build.log 2>&1       || { echo "  GPU BUILD FAIL (tail:)"; tail -n15 /tmp/post-merge-gpu-build.log; exit 1; }
    for b in gpu_bank gpu_fuzz_counter gpu_gust_smoke gpu_kmeans gpu_memcached gpu_memcached_gust gpu_tpcc gpu_ycsb gpu_ycsb_gust; do
      rc=0; timeout 300 "gpu/benchmarks/bin/$b" > "/tmp/post-merge-gpu-$b.log" 2>&1 || rc=$?
      if [ "$rc" != "0" ]; then
        echo "  $b: FAIL rc=$rc (tail:)"; tail -n5 "/tmp/post-merge-gpu-$b.log"; exit 1
      fi
      echo "  $b: OK ($(grep -ciE 'PASS|OK|INVARIANT' "/tmp/post-merge-gpu-$b.log") pass-markers)"
    done
  else
    echo "  hipcc not present — skipping (GPU battery needs ROCm)"
  fi
fi

# ── §7 TLA+ (only if the TLC jar is present) ──────────────────────────────
if run_section tla; then
  step "§7 TLA+ TLC smoke (if jar present)"
  if [ -f /tmp/tla2tools.jar ] && command -v java >/dev/null; then
    make -C docs/proofs smoke-check
  else
    echo "  TLC jar (/tmp/tla2tools.jar) or java not present — skipping (see docs/proofs/README.md)"
  fi
fi

# ── §6 gem5 (only if built) ────────────────────────────────────────────────
if run_section gem5; then
  step "§6 gem5 (if built)"
  if compgen -G "gem5_sim/gem5/build/*/gem5.opt" >/dev/null 2>&1; then
    echo "  gem5 is built — run the power-target smoke per plan §9 (t=1 until F36 fixed)."
  else
    echo "  gem5 not built — skipping (build with 'make gem5')."
  fi
fi

printf '\n\033[1m=== post-merge-check: ALL SECTIONS PASSED in %ss ===\033[0m\n' "$(( $(date +%s) - START ))"
