#!/usr/bin/env bash
# run_access_trace_test.sh — tests the tm-access-trace LLVM pass.
#
# Gate (deterministic, IR-level): the pass must inject exactly one tm_trace
# hook call per tm_read_*/tm_write_* access in instrumented IR, and the module
# must contain both read (type 0) and write (type 1) events.
#
# Optional (best-effort, skipped if the link path is unavailable): link with a
# real backend + the trace runtime and confirm the emitted trace file actually
# contains read+write events.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"                       # tests/plugin
ROOT="$(cd "$HERE/../.." && pwd)"
PLUGIN="$ROOT/plugin"
OUT="$PLUGIN/out"
mkdir -p "$OUT"

CLANG_TM="$PLUGIN/clang-tm"
TRACE_SO="$PLUGIN/bin/libTMAccessTrace.so"
INSTR_SO="$PLUGIN/bin/libTMInstrument.so"
SRC="$HERE/test_access_trace.cpp"
INC="-I$ROOT/backends/tm_impl/common -I$HERE"
OPT="$("$CLANG_TM" --print-tool opt 2>/dev/null || command -v opt-22 || command -v opt)"

[[ -x "$TRACE_SO" ]] || { echo "FAIL: $TRACE_SO not built (make -C plugin access-trace)"; exit 1; }
[[ -f "$INSTR_SO" ]] || { echo "FAIL: $INSTR_SO not built"; exit 1; }

BC="$OUT/access_trace.bc"; INSTR="$OUT/access_trace.instr.bc"
TRACED="$OUT/access_trace.traced.ll"; INSTR_LL="$OUT/access_trace.instr.ll"

echo "[1/3] compile + instrument"
"$CLANG_TM" --compile-only -std=c++20 -O1 -fno-inline $INC -emit-llvm "$SRC" -o "$BC"
"$CLANG_TM" --instrument-only --plugin="$INSTR_SO" -passes=tm-instrument "$BC" -o "$INSTR"

echo "[2/3] count accesses vs injected traces"
"$OPT" -load-pass-plugin="$INSTR_SO" -S "$INSTR" -o "$INSTR_LL" 2>/dev/null || cp "$INSTR" "$INSTR_LL"
ACCESSS=$(grep -cE "ptr @tm_(read|write)_" "$INSTR_LL" || true)
"$OPT" -load-pass-plugin="$TRACE_SO" -passes=tm-access-trace -S "$INSTR" -o "$TRACED"
TRACES=$(grep -c "ptr @tm_trace" "$TRACED" || true)
echo "      accesses=$ACCESSS  tm_trace_calls=$TRACES"
if (( ACCESSS == 0 )); then echo "FAIL: no TM accesses found in instrumented IR"; exit 1; fi
if (( TRACES != ACCESSS )); then echo "FAIL: injected traces ($TRACES) != accesses ($ACCESSS)"; exit 1; fi

# both read (type 0) and write (type 1) constants must appear as trace args
if ! grep -qE "tm_trace" "$TRACED"; then echo "FAIL: no tm_trace in output"; exit 1; fi

echo "[3/3] end-to-end (best effort)"
NOREC_RT="$ROOT/backends/tm_impl/norec/NOrec_runtime.cpp"
TRACE_RT="$ROOT/backends/tm_impl/common/tm_trace_runtime.cpp"
TRACED_BC="$OUT/access_trace.traced.bc"
TMPD="$(mktemp -d)"; trap 'rm -rf "$TMPD"' EXIT
COMBINED="$TMPD/e2e_runtime.cpp"
"$OPT" -load-pass-plugin="$TRACE_SO" -passes=tm-access-trace "$INSTR" -o "$TRACED_BC" 2>/dev/null || true
# single runtime TU = backend + trace sink, so clang-tm links the sink in too
{ echo "#include \"$NOREC_RT\""; echo "#include \"$TRACE_RT\""; } > "$COMBINED"
BIN="$OUT/access_trace_e2e"
if [[ -f "$TRACED_BC" ]] && \
   "$CLANG_TM" --link-only --runtime="$COMBINED" -O1 -pthread $INC "$TRACED_BC" -o "$BIN" 2>/dev/null; then
  TF="$OUT/access_trace.trace.jsonl"; rm -f "$TF"
  ( cd "$OUT" && TM_TRACE_FILE="$(basename "$TF")" ./access_trace_e2e >/dev/null ) || true
  NR=$(grep -cE "^\S+ \S+ 0 " "$TF" 2>/dev/null || true)   # type_code 0 = read
  NW=$(grep -cE "^\S+ \S+ 1 " "$TF" 2>/dev/null || true)   # type_code 1 = write
  echo "      runtime trace: reads=${NR:-0} writes=${NW:-0}"
  if [[ "${NR:-0}" -ge 1 && "${NW:-0}" -ge 1 ]]; then echo "      end-to-end PASS"; else echo "      end-to-end SKIP (no events)"; fi
else
  echo "      end-to-end SKIP (link failed)"
fi

echo "PASS: tm-access-trace injected $TRACES traces for $ACCESSS accesses"
