#!/usr/bin/env bash
# run_strict_addr_test.sh — regression guard for the address-tracking contract
# (B-21: plugin TL2/SwissTM silently bypass transactional accesses to heap data
# allocated outside the TM region, causing lost updates / money drift).
#
# Contract under test:
#   - DEFAULT (no env): plugin keeps the perf bypass; the binary must NOT abort
#     on the address contract (exit code is not SIGABRT / 134, no diagnostic).
#   - TM_STRICT_ADDR=1: a transactional access to an untracked (out-of-region,
#     non-global) address must abort LOUDLY with the TM-ADDR-CONTRACT message.
#
# Uses the plugin bank_tl2 binary (TL2 backend + heap-allocated TMSafeVector),
# which is the canonical B-21 trigger. Skips if it cannot be built.
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"          # tests/plugin
ROOT="$(cd "$HERE/../.." && pwd)"
BIN="$ROOT/benchmarks/plugin/bank/bin/bank_tl2"

[[ -x "$BIN" ]] || make -C "$ROOT/benchmarks/plugin/bank" bank_tl2 >/dev/null 2>&1 || true
if [[ ! -x "$BIN" ]]; then
	echo "SKIP: bank_tl2 unavailable (plugin not built)"; exit 0
fi

echo "[1/2] DEFAULT mode: must not abort on address contract"
DEF_ERR="$("$BIN" -t 2 -d 150 --test 2>&1 >/dev/null)"; DEF_RC=$?
if [[ "$DEF_ERR" == *"TM-ADDR-CONTRACT"* ]] || [[ $DEF_RC -eq 134 ]]; then
	echo "FAIL(default): unexpected abort rc=$DEF_RC"; echo "$DEF_ERR" | head -3; exit 1
fi
echo "      ok: no contract abort (rc=$DEF_RC, bypass retained)"

echo "[2/2] TM_STRICT_ADDR=1: must abort loudly on untracked access"
ST_ERR="$(TM_STRICT_ADDR=1 "$BIN" -t 2 -d 150 --test 2>&1 >/dev/null)"; ST_RC=$?
if [[ "$ST_ERR" != *"TM-ADDR-CONTRACT"* ]]; then
	echo "FAIL(strict): missing TM-ADDR-CONTRACT diagnostic (rc=$ST_RC)"; exit 1
fi
if [[ $ST_RC -eq 0 || $ST_RC -ge 128 && $ST_RC -ne 134 ]]; then
	echo "FAIL(strict): expected SIGABRT (134), got rc=$ST_RC"; exit 1
fi
echo "      ok: aborted on untracked address (rc=$ST_RC)"
echo "$ST_ERR" | grep -m1 "TM-ADDR-CONTRACT" | sed 's/^/      > /'
echo "PASS: address-tracking contract guard works (default bypass, strict loud)"
