# gem5 Simulation Integration Plan

**Goal.** Integrate the TM-SIM gem5 simulation infrastructure into `tm_api_cpp`
so that TM benchmarks can be cycle-accurately simulated on machines without
hardware TSX/TME/HTM support. gem5 sources are **not** committed; a setup
script clones and builds them on demand.

---

## 1. Overview

TM-SIM (`~/Projects/SIM/TM-SIM`) is a standalone repo with gem5 as a git
submodule. This integration:

1. Copies the simulation configs, scripts, workloads, patches, and docs into
   `gem5_sim/`.
2. Provides `gem5_sim/setup.sh` -- a single script that clones gem5 (v25.1.0.1),
   applies the in-tree x86 TSX patch, and builds the `X86_TSX` target.
3. Updates `.gitignore` to exclude gem5 sources and build artifacts.
4. Adds a `make gem5` top-level target for convenience.

**What stays out:**
- `gem5/` sources and build artifacts (gitignored, created by `setup.sh`)
- `upstream-v251/` (reference diff dir, not needed)
- `tm-plugin/` (stale out-of-tree patches; the in-tree TSX commit replaces them)
- `tsx-comparison/benchmarks/tsx_sim_bank/` (Rust tsx-sim bank already in `simulator/`)

---

## 2. New directory structure

```
tm_api_cpp/
├── gem5_sim/                          # NEW - simulation infrastructure
│   ├── README.md                      # Quick-start + command cheat-sheet
│   ├── setup.sh                       # Clone gem5 + apply TSX + build
│   ├── .gem5-version                  # Pinned gem5 version (v25.1.0.1)
│   ├── configs/                       # gem5 simulation configs
│   │   ├── x86-se-bank.py             # SE-mode bank benchmark
│   │   ├── x86-se-bank-classic.py     # Classic-memory diagnostic variant
│   │   ├── x86-tsx-fs.py             # FS-mode (Phase 2, KVM/ATOMIC)
│   │   ├── power8-htm.py             # POWER8 HTM SE config
│   │   ├── arm-tme-kvm.py            # ARM TME FS config
│   │   └── components/               # Ruby MESI_Three_Level_HTM hierarchy
│   │       ├── __init__.py
│   │       ├── mesi_three_level_htm_cache_hierarchy.py
│   │       ├── htm_l1_cache.py
│   │       ├── htm_l2_cache.py
│   │       ├── htm_l3_cache.py
│   │       ├── htm_directory.py
│   │       └── htm_dma_controller.py
│   ├── scripts/                       # Build & run helpers
│   │   ├── run-bank-gem5.sh           # Build-via-Docker + run + verify
│   │   ├── run-x86-tsx.sh             # x86 TSX simulation
│   │   ├── run-power8-htm.sh          # POWER8 HTM simulation
│   │   ├── run-arm-tme.sh             # ARM TME simulation
│   │   ├── run-benchmark.sh           # In-guest benchmark runner (m5 ops)
│   │   ├── apply-tsx-patch.sh         # Download/apply TSX patch (if needed)
│   │   └── compare_gem5_tsxsim.py     # gem5 vs tsx-sim cost comparison
│   ├── patches/                       # x86 TSX patches (reference)
│   │   ├── 001-one-byte-decoder.patch
│   │   ├── 002-two-byte-decoder.patch
│   │   ├── 003-scons.patch
│   │   ├── 004-includes.patch
│   │   └── 005-cpuid.patch
│   ├── workloads/                     # HTM benchmark sources
│   │   ├── common/
│   │   │   └── htm_bench.h
│   │   ├── rtm/                       # x86 RTM workloads
│   │   │   ├── array_sum.c
│   │   │   ├── hash_table.c
│   │   │   └── Makefile
│   │   ├── tme/                       # ARM TME workloads
│   │   │   ├── array_sum.c
│   │   │   ├── hash_table.c
│   │   │   └── Makefile
│   │   └── power8/                    # POWER8 HTM workloads
│   │       ├── array_sum.c
│   │       ├── array_sum_bench.c
│   │       ├── htm_test.S
│   │       ├── tbegin_tend.S
│   │       ├── tm_bare.c
│   │       ├── tm_bench.c
│   │       ├── tm_minimal.c
│   │       └── Makefile
│   └── docs/
│       ├── build.md                   # Detailed build guide
│       ├── workflow.md                # Simulation workflows
│       ├── x86-tsx-patch.md           # TSX patch details
│       ├── gem5-tsx-calibration.md    # Calibration guide
│       ├── power8-htm-implementation.md
│       └── arm-tme-details.md
├── benchmarks/cpp/
│   ├── gem5_roi.hpp                   # (already exists) m5 ROI markers
│   └── bank/
│       └── bank.cpp                   # (already exists) -n quota mode
└── .gitignore                         # Updated: gem5/ excluded
```

---

## 3. Files to create/modify

### 3.1 NEW: `gem5_sim/setup.sh`

Single entry point that clones gem5, applies the TSX patch, and builds.
Replaces the TM-SIM submodule approach.

Key behaviors:
- Clones `https://github.com/gem5/gem5.git` into `gem5_sim/gem5/` (gitignored).
- Checks out the pinned version from `gem5_sim/.gem5-version`.
- The in-tree TSX patch is already in gem5 v25.1.0.1 (commit `6eb3dca1e7`);
  `setup.sh` verifies the TSX files exist; if not, applies the reference
  patches from `gem5_sim/patches/`.
- Builds with `scons build/X86_TSX/gem5.opt --with-ruby`.
- Prints the path to the built binary on success.

### 3.2 NEW: `gem5_sim/.gem5-version`

Single line: `v25.1.0.1` (or the commit hash `6eb3dca1e7`).

### 3.3 MOVE: configs from TM-SIM

Copy `tsx-comparison/configs/` to `gem5_sim/configs/`:
- `x86-se-bank.py` -- adapt path to binary (no longer `../../../TM/tm_api_cpp`)
- `x86-se-bank-classic.py`
- `x86-tsx-kvm.py` -> rename to `x86-tsx-fs.py` (Phase 2)
- `power8-htm.py`
- `arm-tme-kvm.py`
- `components/` (6 Python files)

Path adaptations in `x86-se-bank.py`:
- Binary path: currently `--binary <path>/bank_gem5` (user-provided, no change needed)
- Import paths: `from components.mesi_three_level_htm_cache_hierarchy import ...`
  works when run from `gem5_sim/configs/` directory.

### 3.4 MOVE: scripts from TM-SIM

Copy `tsx-comparison/scripts/` to `gem5_sim/scripts/`:
- `run-bank-gem5.sh` -- adapt `GEM5_DIR` default and `TM_API` default
- `run-x86-tsx.sh`
- `run-power8-htm.sh`
- `run-arm-tme.sh`
- `run-benchmark.sh`
- `apply-tsx-patch.sh`
- `compare_gem5_tsxsim.py`

Path adaptations in `run-bank-gem5.sh`:
```
- GEM5_DIR=${GEM5_DIR:-$(cd "$HERE/../../gem5" && pwd)}
+ GEM5_DIR=${GEM5_DIR:-$(cd "$HERE/../gem5" && pwd)}
- TM_API=${TM_API:-$(cd "$HERE/../../../TM/tm_api_cpp" ...)}
+ TM_API=${TM_API:-$(cd "$HERE/../.." && pwd)}
```

### 3.5 MOVE: patches from TM-SIM

Copy `tm-plugin/patches/` to `gem5_sim/patches/` (5 patch files).
These are kept for reference; `setup.sh` applies them only if the in-tree
TSX is missing.

### 3.6 MOVE: workloads from TM-SIM

Copy `tsx-comparison/workloads/` to `gem5_sim/workloads/`:
- `common/htm_bench.h`
- `rtm/` (array_sum, hash_table, Makefile)
- `tme/` (array_sum, hash_table, Makefile)
- `power8/` (array_sum, array_sum_bench, htm_test.S, tbegin_tend.S, etc.)

### 3.7 MOVE: docs from TM-SIM

Copy `tsx-comparison/docs/` to `gem5_sim/docs/`:
- `build.md`, `workflow.md`, `x86-tsx-patch.md`, `gem5-tsx-calibration.md`,
  `power8-htm-implementation.md`, `arm-tme-details.md`

Adapt paths in docs to reference `gem5_sim/` instead of `tsx-comparison/`.

### 3.8 NEW: `gem5_sim/README.md`

Quick-start guide:
1. `./gem5_sim/setup.sh` -- clone + build gem5 (~15 min)
2. Build benchmark: `make -C benchmarks/cpp BACKEND=TSXSGL GEM5=1 bin/bank_gem5_tsxsgl`
3. Run simulation: `./gem5_sim/scripts/run-bank-gem5.sh TSXSGL 1 64 2000`
4. Verify: `PASS: Money conserved` + ROI stats in `gem5_sim/m5out/`

### 3.9 MODIFY: `.gitignore`

Add at the end:

```
# gem5 simulation (cloned by gem5_sim/setup.sh, never committed)
gem5_sim/gem5/
gem5_sim/m5out/
```

### 3.10 MODIFY: top-level `Makefile`

Add a `gem5` target:

```makefile
# --- gem5 simulation -------------------------------------------------------
.PHONY: gem5 gem5-clean
gem5:
	./gem5_sim/setup.sh

gem5-clean:
	rm -rf gem5_sim/gem5/build
```

---

## 4. Path mapping (TM-SIM to tm_api_cpp)

| TM-SIM path | tm_api_cpp path | Notes |
|-------------|-----------------|-------|
| `gem5/` (submodule) | `gem5_sim/gem5/` (gitignored) | Created by `setup.sh` |
| `tm-plugin/patches/` | `gem5_sim/patches/` | Reference only |
| `tm-plugin/gem5-mod/` | **not copied** | In-tree in gem5 v25.1 |
| `tsx-comparison/configs/` | `gem5_sim/configs/` | |
| `tsx-comparison/scripts/` | `gem5_sim/scripts/` | Path defaults adapted |
| `tsx-comparison/workloads/` | `gem5_sim/workloads/` | |
| `tsx-comparison/docs/` | `gem5_sim/docs/` | Path refs updated |
| `tsx-comparison/m5out/` | `gem5_sim/m5out/` (gitignored) | Simulation output |
| `upstream-v251/` | **not copied** | Reference only |
| `IMPROVEMENT_PLAN.md` | **not copied** | Superseded by this plan |
| `x86-tsx-integration-plan.md` | **not copied** | Superseded by this plan |

---

## 5. What is NOT needed in tm_api_cpp

- **gem5 source code** -- cloned by `setup.sh`, gitignored.
- **`tm-plugin/` out-of-tree source module** -- TSX is in-tree since gem5
  v25.1.0.1. The patches in `gem5_sim/patches/` are kept for reference only.
- **`upstream-v251/`** -- diff reference, not needed.
- **`tsx-comparison/benchmarks/tsx_sim_bank/`** -- Rust tsx-sim bank already
  exists in `simulator/`.
- **`tsx-comparison/.gitignore`** -- merged into top-level `.gitignore`.

---

## 6. Build workflow

### First-time setup

```bash
# 1. Clone + build gem5 (takes ~15 min on M1 Pro)
./gem5_sim/setup.sh

# 2. Build the bank benchmark for gem5 (static x86-64 via Docker)
docker run --rm --platform linux/amd64 \
  -v "$(pwd)":/w -w /w/benchmarks/cpp alpine:3.20 \
  sh -c "apk add --no-cache g++ make >/dev/null && \
         make BACKEND=TSXSGL GEM5=1 bin/bank_gem5_tsxsgl"

# 3. Run SE-mode simulation
./gem5_sim/scripts/run-bank-gem5.sh TSXSGL 1 64 2000
```

### Subsequent runs

```bash
# Rebuild gem5 only if needed
./gem5_sim/setup.sh --build

# Run with different parameters
./gem5_sim/scripts/run-bank-gem5.sh NOREC 4 128 5000

# With TM trace capture
TRACE=1 ./gem5_sim/scripts/run-bank-gem5.sh TSXSGL 1 64 500
```

### Benchmark matrix

| Backend | Threads | Description |
|---------|---------|-------------|
| TSXSGL | 1 | TSX fast-path, no conflicts |
| TSXSGL | 2-4 | TSX with conflict aborts |
| SPHT | 1-4 | Second RTM consumer |
| NOREC | 1-4 | Software STM cross-check |
| TinySTM-WBCTL | 1-4 | Software STM cross-check |

---

## 7. Implementation steps (ordered)

### Step 1: Create `gem5_sim/` skeleton

```
mkdir -p gem5_sim/{configs/components,scripts,patches,workloads/{common,rtm,tme,power8},docs}
```

### Step 2: Write `gem5_sim/setup.sh`

Clone + version-pinned checkout + TSX verification + scons build.
See Section 3.1 for design.

### Step 3: Write `gem5_sim/.gem5-version`

### Step 4: Copy configs from TM-SIM

```bash
cp ~/Projects/SIM/TM-SIM/tsx-comparison/configs/x86-se-bank.py gem5_sim/configs/
cp ~/Projects/SIM/TM-SIM/tsx-comparison/configs/x86-se-bank-classic.py gem5_sim/configs/
cp ~/Projects/SIM/TM-SIM/tsx-comparison/configs/x86-tsx-kvm.py gem5_sim/configs/x86-tsx-fs.py
cp ~/Projects/SIM/TM-SIM/tsx-comparison/configs/power8-htm.py gem5_sim/configs/
cp ~/Projects/SIM/TM-SIM/tsx-comparison/configs/arm-tme-kvm.py gem5_sim/configs/
cp -r ~/Projects/SIM/TM-SIM/tsx-comparison/configs/components/ gem5_sim/configs/components/
```

Adapt path references in `run-bank-gem5.sh` and docs.

### Step 5: Copy scripts from TM-SIM

```bash
cp ~/Projects/SIM/TM-SIM/tsx-comparison/scripts/*.sh gem5_sim/scripts/
cp ~/Projects/SIM/TM-SIM/tsx-comparison/scripts/*.py gem5_sim/scripts/
chmod +x gem5_sim/scripts/*.sh
```

Adapt `GEM5_DIR` and `TM_API` defaults in `run-bank-gem5.sh`.

### Step 6: Copy patches from TM-SIM

```bash
cp ~/Projects/SIM/TM-SIM/tm-plugin/patches/*.patch gem5_sim/patches/
```

### Step 7: Copy workloads from TM-SIM

```bash
cp -r ~/Projects/SIM/TM-SIM/tsx-comparison/workloads/* gem5_sim/workloads/
```

### Step 8: Copy docs from TM-SIM

```bash
cp ~/Projects/SIM/TM-SIM/tsx-comparison/docs/*.md gem5_sim/docs/
```

Adapt paths in docs (`tsx-comparison/` -> `gem5_sim/`).

### Step 9: Write `gem5_sim/README.md`

### Step 10: Update `.gitignore`

Append gem5-specific ignore rules.

### Step 11: Update top-level `Makefile`

Add `gem5` and `gem5-clean` targets.

### Step 12: Verify

```bash
# Test setup.sh (clone + build)
./gem5_sim/setup.sh

# Test benchmark build
make -C benchmarks/cpp BACKEND=TSXSGL GEM5=1 bin/bank_gem5_tsxsgl

# Test SE run
./gem5_sim/scripts/run-bank-gem5.sh TSXSGL 1 64 2000
```

---

## 8. Risks and mitigations

| Risk | Impact | Mitigation |
|------|--------|------------|
| gem5 build fails on macOS ARM | Blocks simulation | Docker cross-build for benchmarks; gem5 itself builds natively on ARM |
| SE-mode timing too slow | Long iteration cycles | Use `--cpu-type atomic` for functional checks; Timing for final measurements |
| TSX patch not in pinned gem5 version | Build failure | `setup.sh` falls back to applying reference patches from `gem5_sim/patches/` |
| Path breakage after copy | Script failures | Validate all scripts with dry-run after copy; test with `bash -n` |
| gem5 v25.1 API changes | Config scripts break | Pin exact version in `.gem5-version`; test after clone |

---

## 9. Future work (post-integration)

1. **Phase 2**: Full-system workflow with checkpoint/restore
2. **Phase 3**: Multi-threaded TSX correctness in gem5 (XABORT path, conflict/capacity aborts)
3. **Phase 4**: Metrics extraction, calibration loop, CPU-model comparison
4. **Phase 5**: Benchmark matrix expansion (STAMP, fuzz_counter, fuzz_bank)
5. **CI**: GitHub Actions job for gem5 SE smoke test
