# x86 TSX Patch for gem5

## Overview

The official gem5 stable tree does not include x86 TSX (Transactional
Synchronization Extensions) support. An external patch exists from ~2014
adding `XBEGIN`, `XABORT`, `XEND` (RTM) and `XACQUIRE`/`XRELEASE` (HLE)
support.

- **Review URL:** https://reviews.gem5.org/r/2308/
- **Author:** Pradip Vallathol
- **Created:** July 2014
- **Status:** Not merged (patch requires rebasing)

## What the Patch Adds

- 3 new RTM instructions: `XBEGIN`, `XABORT`, `XEND`
- HLE prefix support: `XACQUIRE`, `XRELEASE`
- L0 cache for transactional state tracking
- L1 cache for memory versioning (speculative writes)
- O3 CPU model modifications for transactional requests
- Supports both RTM and HLE in a single framework

## Limitations

| Limitation | Impact |
|------------|--------|
| O3 CPU only | TimingSimpleCPU and Atomic CPU not supported |
| No nested transactions | Single-level transactions only |
| Per-cache-line versioning | L1 holds speculative state |
| 2014-era patch | Needs rebasing for gem5 v25.1 |
| Memory ordering | Simplified compared to real TSX |
| No interrupt handling | Interrupts during TX not fully modeled |

## Obtaining the Patch

### Option 1: Download (if available)

```bash
scripts/apply-tsx-patch.sh
```

The script searches these locations:
1. `patches/tsx.patch` (local to this repo)
2. `patches/x86-tsx.patch`
3. `tsx.patch` (in project root)
4. Downloads from `https://reviews.gem5.org/r/2308/raw/`

### Option 2: Save from Review Board

Visit https://reviews.gem5.org/r/2308/ and click "Download" → "Raw Diff".
Save to `patches/tsx.patch`.

### Option 3: Manual Porting

If the patch does not apply, the changes need to be ported forward.
Key files to modify:

```
src/arch/x86/isa/decoder/rtm.isa        -- RTM instruction decode
src/arch/x86/insts/microcode/rtm.uca    -- Micro-op sequences
src/cpu/o3/commit.cc                     -- Commit-stage HTM tracking
src/cpu/o3/lsq_unit.cc                   -- Load/store queue HTM handling
src/mem/cache/tags/                       -- Cache versioning support
```

## Applying

```bash
# Build with the patch:
./scripts/build.sh x86-tsx --patch /path/to/tsx.patch

# If applying manually:
cd gem5
git apply /path/to/tsx.patch
```

If `git apply` fails, try with reject files:

```bash
git apply --reject /path/to/tsx.patch
# Fix rejected hunks in *.rej files manually
```

## Verifying

Check that the patch was applied:

```bash
grep -r "XBEGIN\|XABORT\|XEND" src/arch/x86/ | head -5
```

Should show instruction definitions for RTM transactions.

## Building a TSX Workload

Compile with GCC's RTM support:

```bash
gcc -O2 -mrtm -o tsx_test tsx_test.c
```

Example RTM usage:

```c
#include <immintrin.h>

unsigned status = _xbegin();
if (status == _XBEGIN_STARTED) {
    // transactional region
    _xend();
} else {
    // fallback path
}
```
