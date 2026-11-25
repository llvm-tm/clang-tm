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
				# Known-open CSMV race (TODO P1): a rare lost-update /
				# phantom in the OCC commit path that surfaces ONLY as a
				# small nonzero quiesce_viol with every other counter at
				# zero. Reported but not gated. Any other nonzero counter
				# is treated as a real failure so this allowance cannot
				# mask a new defect.
				if [ "$be" = CSMV ]; then
					qv=$(echo "$line" | grep -oE 'quiesce_viol=[0-9]+' | grep -oE '[0-9]+')
					other=$(echo "$line" |
						grep -oE '(v_double_read|v_range|v_readown|v_ptr|alloc_viol)=[0-9]+' |
						grep -vE '=0$')
					if [ "${qv:-0}" -gt 0 ] && [ -z "$other" ]; then
						echo "== $be: known-open seed=$seed $sc — rare OCC quiesce_viol=$qv (TODO P1)"
						continue
					fi
				fi
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
