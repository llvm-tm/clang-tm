# Bank Benchmark

A simple banking benchmark that tests concurrent money transfers between accounts. Verifies **money conservation** (total balance stays constant).

## Usage

```bash
./bin/bank_<backend> -t <threads> -a <accounts> -d <ms> -r <read%> -w <write%>
```

| Flag | Default | Description |
|---|---|---|
| `-t` | 2 | Number of threads |
| `-a` | 256 | Number of accounts |
| `-d` | 10000 | Duration in milliseconds |
| `-r` | 10 | Read-all transactions (%) |
| `-w` | 0 | Write-all transactions (%) |
| `--disjoint` | off | Disjoint account access |

## Build

```bash
# All backends
make all

# Specific backend
make bank_norec
make bank_tl2
make bank_tinystm
make bank_swiss
make bank_singlelock

# Uninstrumented baseline
make bank_uninstrumented
```

## Results

| Backend | 2T (txns/sec) | 4T (txns/sec) | Correctness |
|---|---|---|---|
| SingleGlobalLock | 1.44M | 1.11M | ✅ Always |
| TL2 | 1.74M | 2.40M | ✅ 2T, ❌ 4T (money off ±3) |
| NOrec | 282k | 170k | ✅ Always |
| TinySTM | ⚠️ Slow | ⚠️ Slow | ✅ Correct |
| SwissTM | ⚠️ Slow | ⚠️ Slow | ✅ Correct |
