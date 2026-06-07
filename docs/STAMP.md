# STAMP Benchmark Suite

Three independent implementations exist in this repo:

| Path | Language | Notes |
|------|----------|-------|
| `benchmarks/plugin/STAMP/` | C++ (LLVM plugin) | Single `STAMP.cpp` + `bench_*.hpp`, compiled via `plugin/tm_pipeline.mk` |
| `benchmarks/cpp/STAMP/` | C++ (expli API) | Per-benchmark `.cpp` files, compiled via `benchmarks/cpp/Makefile` |
| `benchmarks/rust/src/clis/` | Rust (expli API) | Per-benchmark `.rs` files via `cargo build` |

All three have identical algorithm structure and accept the same flags (modulo naming differences — see per-benchmark notes).

## Reference Input Configurations

From the STAMP paper (IISWC 2008, Table IV). Suffixes `-low` and `-high` indicate relative contention; appended `+` indicates larger input sizes.

### bayes

```
bayes        -v32 -r1024 -n2 -p20 -i2 -e2
bayes+       -v32 -r4096 -n2 -p20 -i2 -e2
bayes++      -v32 -r4096 -n10 -p40 -i2 -e8 -s1
```

Dependencies for `v` variables are learned from `r` records, which have `n`–`p` parents per variable on average. Edge insertion has penalty `i`, up to `e` edges per variable.

**Our runner** `-v 16 -r 32 -n 2 -p 2 -i 2 -e 4` — tuned for fast validation, does **not** match any paper variant. Use reference values for comparable results.

### genome

```
genome       -g256 -s16 -n16384
genome+      -g512 -s32 -n32768
genome++     -g16384 -s64 -n16777216
```

Gene segments of `s` nucleotides sampled from a gene with `g` nucleotides. `n` segments are analyzed to reconstruct the original gene.

**Our runner** `-g 16384 -s 64 -n 1000000` — matches `genome++` gene/segment length but uses fewer segments (1M vs 16M). This was tuned for ~60s runtime.

### intruder

```
intruder     -a10 -l4 -n2048 -s1
intruder+    -a10 -l16 -n4096 -s1
intruder++   -a10 -l128 -n262144 -s1
```

`n` traffic flows are analyzed, `a` of which have attacks. Each flow has at most `l` packets, random seed `s`.

**Our runner** `-a 10 -l 128 -n 5120 -s 1` — message length matches `intruder++` but flow count is closer to `intruder+`.

### kmeans

```
kmeans-high  -m15 -n15 -t0.05 -i random-n2048-d16-c16
kmeans-high+ -m15 -n15 -t0.05 -i random-n16384-d24-c16
kmeans-high++ -m15 -n15 -t0.00001 -i random-n65536-d32-c16

kmeans-low    -m40 -n40 -t0.05 -i random-n2048-d16-c16
kmeans-low+   -m40 -n40 -t0.05 -i random-n16384-d24-c16
kmeans-low++  -m40 -n40 -t0.00001 -i random-n65536-d32-c16
```

Cluster centers varied from `m` to `n`. Convergence threshold `t`. Input `i`: `n` points of `d` dimensions generated about `c` centers.

**Our runner** (expli/rust) `-k 8 -d 2 -n 200 -t 0.00001` — tiny debugging workload, **not** comparable to paper.
**Plugin** uses defaults `-m 40 -n 40 -t 0.00001` and requires an input file for `-i`.

**Input file**: The `-i` argument points to a file of `d`-dimensional points. Format (one per line):
```
<dim1> <dim2> ... <dimD>
```
The reference inputs are at https://github.com/ccaominh/stamp/tree/master/kmeans/inputs — they are `random-n16384-d24-c16.txt.gz` etc. These files are synthetically generated; the algorithm below reproduces them.

> **Generating kmeans inputs** (`tools/gen_kmeans_input.py` planned):  
> `c` cluster centers are chosen uniformly at random within a unit hypercube of `d` dimensions. Each of `n` points is assigned to the nearest center plus Gaussian noise (`σ = 0.01`). This matches the reference STAMP generator.

### labyrinth

```
labyrinth    -i random-x32-y32-z3-n96
labyrinth+   -i random-x48-y48-z3-n64
labyrinth++  -i random-x512-y512-z7-n512
```

Maze of dimensions `x × y × z`, `n` paths to route.

**Our runner** `-x 8 -y 8 -z 8 -n 64` — tiny 8×8×8 cube for fast validation. The `-i` flag is NOT used; coordinates are passed directly as `-x -y -z -n`.

> **Input generation**: When `-i` is omitted, the labyrinth binary generates a random maze internally using the `-x -y -z -n` parameters. The maze is a 3D grid with randomly placed obstacles (blocked cells). Reference inputs at https://github.com/ccaominh/stamp/tree/master/labyrinth/inputs are the same format produced by the internal generator with those dimensions.

### ssca2

```
ssca2        -s13 -i1.0 -u1.0 -l3 -p3
ssca2+       -s14 -i1.0 -u1.0 -l9 -p9
ssca2++      -s20 -i1.0 -u1.0 -l3 -p3
```

Graph with `2^s` nodes. Probability of inter-clique and unidirectional edges: `i`, `u`. Max path length `l`, max parallel edges `p`.

**Our runner** `-s 14 -i 1.0 -u 1.0 -l 3 -p 3` (plugin) / `-m 3` (expli/rust) — matches `ssca2+` scale but uses `ssca2` max-edge length and parallelism.

> **Input generation**: SSCA2 is fully synthetic; all four parameters (`s,i,u,l,p`) define the random graph generator. No external input file needed.

### vacation

```
vacation-high    -n4 -q60 -u90 -r16384 -t4096
vacation-high+   -n4 -q60 -u90 -r1048576 -t4096
vacation-high++  -n4 -q60 -u90 -r1048576 -t4194304
vacation-low     -n2 -q90 -u98 -r16384 -t4096
vacation-low+    -n2 -q90 -u98 -r1048576 -t4096
vacation-low++   -n2 -q90 -u98 -r1048576 -t4194304
```

Database has `r` records per reservation item; clients perform `t` sessions. `u%` reserve/cancel, remainder create/destroy. `n` items per session, `q%` of records queried.

**Our runner** `-n 2 -q 90 -u 98 -r 16384 -t 4096` — matches `vacation-low` exactly.
> **Note**: The plugin binary accepts `-q`; expli/rust compute `query_range = 0.9 * num_relations` internally (equivalent to `-q 90`).

### yada

```
yada         -a20 -i 633.2
yada+        -a10 -i ttimeu10000.2
yada++       -a15 -i ttimeu1000000.2
```

Input mesh `i` is refined to minimum angle `a`. File `633.2` = 1264 elements; `ttimeu10000.2` = 19 998 elements; `ttimeu1000000.2` = 1 999 998 elements.

**Our runner** `-a 20 -j 0.5` — `-a` matches `yada` (low). No `-i` is passed: the binary generates a random mesh internally from the angle and jitter parameters.

> **Input generation**: When `-i` is omitted, yada generates a random Delaunay mesh internally using `-a` (minimum angle) and `-j` (jitter). The reference mesh files at https://github.com/ccaominh/stamp/tree/master/yada/inputs can be downloaded and decompressed to reproduce the paper workloads exactly:
> ```
> curl -L https://github.com/ccaominh/stamp/raw/master/yada/inputs/ttimeu10000.2.ele.gz | gunzip > /tmp/ttimeu10000.2.ele
> curl -L https://github.com/ccaominh/stamp/raw/master/yada/inputs/ttimeu10000.2.node.gz | gunzip > /tmp/ttimeu10000.2.node
> ./stamp_yada -a10 -i ttimeu10000.2
> ```

## Input Generation Strategy

We do **not** store input files in the repo. Instead:

1. **Synthetic benchmarks** (genome, intruder, ssca2, vacation, bayes): All parameters are passed as flags. The binary generates random data internally. No external files needed.

2. **Labyrinth**: Coordinates passed as `-x -y -z -n`. Maze is generated internally. The `-i` file variant exists but is not needed.

3. **Yada**: Without `-i`, a random mesh is generated from `-a -j`. To match paper workloads, download `.ele`/`.node` files from https://github.com/ccaominh/stamp/tree/master/yada/inputs.

4. **Kmeans**: Requires `-i` input file. Generate with the script at `tools/gen_kmeans_input.py` (see below).

### kmeans Input Generator

The reference STAMP distribution generates kmeans inputs with this algorithm:

```
for c in 1..num_centers:
    center[c][d] = uniform_random(0.0, 1.0)  for d in 1..dims

for p in 1..num_points:
    c = uniform_random(1, num_centers)
    for d in 1..dims:
        point[p][d] = center[c][d] + gaussian_noise(sigma=0.01)
```

Output format: one line per point, space-separated floating-point values.

A Python generator is planned at `tools/gen_kmeans_input.py` (not yet implemented). Until then, use the inputs from the reference repository, or skip the `-i` flag (kmeans will generate random data internally, which is acceptable for relative comparison).
