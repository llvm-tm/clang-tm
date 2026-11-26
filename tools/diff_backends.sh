#!/usr/bin/env bash
# tools/diff_backends.sh — T1 differential concurrency sweep (see
# tests/concurrency/drv_diff_ops.cpp for the invariant and harness notes).
#
# Builds bin/drv_diff_ops for every backend and runs the same seeded
# concurrency workloads on each. The driver is self-verifying (additive
# deltas ⇒ order-independent final state), so PASS/FAIL needs no oracle
# binary. Backends that fail to build are reported and skipped.
#
# usage: tools/diff_backends.sh [seeds...]        (default: 1 2 3)
#        BACKENDS="TL2 SGL" tools/diff_backends.sh
set -u
cd "$(dirname "$0")/.." || exit 1

SEEDS="${*:-1 2 3}"
BACKENDS="${BACKENDS:-TINYSTM WBETL WT NOREC NORECBF SWISSTM TL2 TSC_TM MVLOG SGL LEFTRIGHT ROMULUS XTM SPHT TSXSGL GPU_STM_CPU CSMV JVSTM GACCO CALVIN ACCELTL2}"
# -T threads -n txns/thread per scenario (quick CI set; add more locally)
SCENARIOS=("-T 4 -n 1500" "-T 8 -n 1200")

fails=""; builds=""; passed=""
for be in $BACKENDS; do
	if ! make -C benchmarks/cpp bin/drv_diff_ops BACKEND="$be" >/dev/null 2>&1; then
		echo "== $be: BUILD FAILED (skipped)"
		builds="$builds $be"
		continue
	fi
	ok=1
	for seed in $SEEDS; do
		for sc in "${SCENARIOS[@]}"; do
			out=$(./benchmarks/cpp/bin/drv_diff_ops -S "$seed" $sc 2>&1)
			rc=$?
			line=$(echo "$out" | grep RESULT | tail -1)
			if [ $rc -ne 0 ]; then
				ok=0
				echo "== $be: FAIL seed=$seed $sc"
				echo "$out" | grep -m5 VIOLATION
				echo "   $line"
			fi
		done
	done
	# Granularity pins: cheap single-threaded checks that localise a failure
	# to a specific mechanism rather than "the model disagreed".
	if make -C benchmarks/cpp bin/repro_width_clobber BACKEND="$be" >/dev/null 2>&1; then
		if ! ./benchmarks/cpp/bin/repro_width_clobber >/dev/null 2>&1; then
			ok=0
			echo "== $be: FAIL repro_width_clobber (sub-word commit clobbers neighbours)"
		fi
	fi
	if make -C benchmarks/cpp bin/repro_bucket_alias BACKEND="$be" >/dev/null 2>&1; then
		if ! ./benchmarks/cpp/bin/repro_bucket_alias >/dev/null 2>&1; then
			ok=0
			echo "== $be: FAIL repro_bucket_alias (version store aliases word-sharing objects)"
		fi
	fi
	# Accelerator teardown pin: only ACCELTL2 has a device-shared guard table to
	# mis-time, so the program SKIPs (exit 0) for every other backend. With
	# TM_ACCEL_MIN=1 a host that does have a fine-grain-SVM device re-inits the
	# STM mid-process and must keep dispatching; a host without one skips.
	if make -C benchmarks/cpp bin/repro_accel_teardown BACKEND="$be" >/dev/null 2>&1; then
		if ! TM_ACCEL_MIN=1 ./benchmarks/cpp/bin/repro_accel_teardown >"/tmp/accel_td_$be.log" 2>&1; then
			ok=0
			echo "== $be: FAIL repro_accel_teardown (tm_exit freed the shared guard table under the STM)"
			tail -6 "/tmp/accel_td_$be.log"
		fi
	fi
	if [ $ok -eq 1 ]; then
		echo "== $be: PASS"
		passed="$passed $be"
	else
		fails="$fails $be"
	fi
done

echo
echo "diff_backends: PASS:${passed:- none} BUILD_FAIL:${builds:- none} FAIL:${fails:- none}"
[ -z "$fails" ] && [ -z "$builds" ] || exit 1
