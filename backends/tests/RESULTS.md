# Backend Test Suite Results

## Test Suites

| Suite | Description | Per-backend binaries |
|---|---|---|
| `test_single` | Basic types, sequential TX, malloc/free, serial lock, ptr null (10 tests) | 7 |
| `test_multi` | Counter, write-set validation, read-set caching, write-write, abort stress, concurrent, alloc stress, FP (8 tests, 4-8 threads) | 7 |
| `test_counter` | Heavy counter stress: 8 threads × 10000 iterations = 80000 increments | 7 |
| `test_stress` | Read-only TX, mixed types, adjacent cache-line, abort+alloc, many reads, many writes, invariant (7 tests, 4-8 threads) | 7 |
| `test_opacity` | Write-skew (1000 rounds), read-set validation, high-contention (3 tests, 2-8 threads) | 7 |

## Results (all backends: 0 failures)

| Backend | test_single | test_multi | test_counter | test_stress | test_opacity |
|---|---|---|---|---|---|
| TinySTM WBCTL | 0 failures | 0 failures | 0 failures | 0 failures | 0 failures |
| TinySTM WBETL | 0 failures | 0 failures | 0 failures | 0 failures | 0 failures |
| TinySTM WT | 0 failures | 0 failures | 0 failures | 0 failures | 0 failures |
| TL2 | 0 failures | 0 failures | 0 failures | 0 failures | 0 failures |
| NOrec | 0 failures | 0 failures | 0 failures | 0 failures | 0 failures |
| SwissTM | 0 failures | 0 failures | 0 failures | 0 failures | 0 failures |
| SGL | 0 failures | 0 failures | 0 failures | 0 failures | 0 failures |

## Existing Heavy Counter Tests (TinySTM-specific, 16 threads × 50000 = 800000)

| Design | Result |
|---|---|
| WBCTL | PASS (1559 ms) |
| WBETL | PASS (624 ms) |
| WT | PASS (1369 ms) |

## Running

```sh
# All test suites, all backends
make run

# Individual suites
make run-counter    # Counter tests
make run-stress     # Stress tests
make run-opacity    # Opacity tests

# Single backend
make run_tl2        # test_single + test_multi for tl2
```
