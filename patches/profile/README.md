# STAMP Profiling Patch

Adds detailed TM metrics tracking to TinySTM and provides scripts
for collecting and analyzing STAMP benchmark characterization data.

## Patch

`0001-add-tm-metrics.patch` — applies to the repo root:

- `backends/tm_impl/tiny_stm/TinySTM_runtime.cpp`: adds per-commit
  read-set/write-set statistics (total, avg, min, max) printed as a
  `TM_STATS:` line to stdout at program exit.
- `benchmarks/plugin/STAMP/STAMP.cpp`: changes bayes parent-chance
  flag from `-p` (conflicts with thread count flag) to `-c`.
- `benchmarks/scripts/profile_stamp.py`: updates bayes params to use
  `-c` and sets `-p` for all impls.

## Scripts

- `run_profiling.py` — runs all 10 benchmark variants, parses
  `TM_STATS:` lines, and compares against `benchmarks/stamp_characterization.csv`
  (OCR'd Table VI from the STAMP paper, IISWC 2008).

- `compare_table_vi.py <results.csv>` — standalone comparison of a
  previous run's CSV output against Table VI reference data.

## Metrics Collected

| Metric | Source | Description |
|--------|--------|-------------|
| commits | TM_STATS | Total committed transactions |
| avg_reads | TM_STATS | Average read-set size per commit |
| min_reads | TM_STATS | Minimum read-set size |
| max_reads | TM_STATS | Maximum read-set size |
| avg_writes | TM_STATS | Average write-set size per commit |
| min_writes | TM_STATS | Minimum write-set size |
| max_writes | TM_STATS | Maximum write-set size |
| total_reads | computed | Total read barriers (avg_reads × commits) |
| total_writes | computed | Total write barriers (avg_writes × commits) |
| aborts | TM_STATS | Total aborted transactions |

## Table VI Columns (IISWC 2008)

| Column | Our Metric |
|--------|------------|
| Transactions | commits |
| Read Set | avg_reads |
| Write Set | avg_writes |
| Read Barriers | total_reads |
| Write Barriers | total_writes |

## Usage

```bash
# Apply the patch
git am patches/profile/0001-add-tm-metrics.patch

# Build and run profiling
cd benchmarks/plugin/STAMP && make -j stamp_tinystm
cd ../../.. && python3 patch/profile/run_profiling.py

# Or to pass to comparison later:
python3 patch/profile/run_profiling.py --benchmarks genome intruder 2>&1 | tee /tmp/profile.log
# Then extract CSV and compare:
python3 patch/profile/compare_table_vi.py /tmp/stamp_profile_results.csv
```
