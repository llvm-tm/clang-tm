# Bank Benchmark — Money Conservation Correctness Test

A simple banking benchmark that tests concurrent money transfers
between accounts.  Verifies **money conservation** (total balance
stays constant across all threads).

## Usage

```sh
./bin/bank_<backend> -t <threads> -a <accounts> -d <ms> -r <read%> -w <write%>
```

| Flag        | Default | Description                              |
|-------------|---------|------------------------------------------|
| `-t`        | 2       | Number of threads                        |
| `-a`        | 256     | Number of accounts                       |
| `-d`        | 10000   | Duration in milliseconds                 |
| `-r`        | 10      | Read-all transactions (%)                |
| `-w`        | 0       | Write-all transactions (%)               |
| `--disjoint`| off     | Partition accounts per thread (no conflict) |

## Build

```sh
cd benchmarks/test/bank

make all                            # All backends + uninstrumented
make bank_singlelock                # SingleGlobalLock
make bank_norec                     # NOrec
make bank_tl2                       # TL2
make bank_tinystm                   # TinySTM
make bank_swiss                     # SwissTM
make bank_persistentsgl             # PersistentSGL
make bank_distributedsgl            # DistributedSGL
make bank_uninstrumented            # Baseline (no TM calls)
```

## Results (4 threads, 256 accounts, 3s, 10% read-all)

| Backend          | 1T         | 2T         | 4T         | Correctness |
|------------------|------------|------------|------------|-------------|
| SingleGlobalLock | 5.14 M/s   | 3.10 M/s   | 2.20 M/s   | ✅          |
| NOrec            | 618 K/s    | 410 K/s    | 236 K/s    | ✅          |
| TL2              | 2.18 M/s   | 2.92 M/s   | 3.38 M/s   | ✅          |
| TinySTM (WBCTL)  | 424 K/s    | 444 K/s    | 402 K/s    | ✅          |

## Specification

- Accounts hold integer balances, initialized to 1000.
- Transfer(x, y, 1): atomic debit from x, credit to y.
- INVARIANT: sum(balances) must remain constant.

This is a self-validating benchmark: the program returns 0 if money
is conserved and 1 if violated.
