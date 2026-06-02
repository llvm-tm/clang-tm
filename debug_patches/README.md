# DEBUG_PATCH — Temporary Debug Patches for TM Backends

## Problem
Adding `fprintf(stderr, ...)`, extra assertions, ad-hoc logging, or
experimental code directly into source files pollutes git history
and can accidentally be committed.

## Solution: Two approaches

### Approach 1: `TM_EVENT_LOG` (compile-time gated, lives in source)

The event logger (`backends/tm_event_logger.hpp`) is permanently embedded
in the source but compiles to zero overhead unless activated:

```bash
# Build with event logging
make CXXFLAGS="-DTM_EVENT_LOG -UNDEBUG" -C backends/tests clean all

# Run with SIGSEGV auto-dump
./bin/test_foo
# On crash, last 512 events are printed to stderr

# Or dump events manually:
TM_EVENT_DUMP(64);  // print last 64 events from calling thread
```

The event logger is APPROPRIATE for **production debugging** — zero
overhead when off, always available.  See `backends/tm_event_logger.hpp`
for event types.

### Approach 2: `debug_patches/` (external patches, source stays clean)

For **one-off or invasive debugging** (per-call `fprintf`, type-specific
breakpoint traps, custom assertions), use git-formatted patches:

```bash
# 1. HACK the source files (add your debug code):
   vim backends/SwissTM/SwissTM.hpp

# 2. Create a patch:
   cd $PROJ_ROOT
   git diff > debug_patches/patches/001-my-debug.patch

# 3. Revert the working tree back to clean:
   git checkout -- backends/SwissTM/SwissTM.hpp
   # or:  git checkout -- .

# 4. Apply the patch for debugging:
   ./debug_patches/apply.sh

# 5. Build & debug...

# 6. Remove when done:
   ./debug_patches/remove.sh
```

## Creating a good debug patch

1. Start from a **clean working tree** (`git status` shows no modified
   files in the backend or plugin directories).
2. Make ONLY the debug changes — no production fixes mixed in.
3. Create the patch with `git diff $PROJ_ROOT > patches/XXX-name.patch`.
   This captures only staged/unstaged changes.
4. Revert the working tree with `git checkout -- .`.
5. Test apply/remove before committing the patch.

## Naming convention

```
patches/001-short-descriptive-name.patch
patches/002-another-debug.patch
patches/...
```

Patches are applied in sorted order (globbing order).

## Patch lifecycle

```
debug session:
  apply.sh → build → run → debug → fix → remove.sh

committing a new patch:
  create patch → git add debug_patches/ → git commit
  (the patch itself becomes a permanent debugging artifact)

retiring a patch:
  add .retired suffix:  git mv patches/001-foo.patch patches/001-foo.patch.retired
  keep for reference, never auto-applied
```

## Safety

- `apply.sh` refuses to run if any source file is already modified
  (`git status --porcelain` is not empty).  Commit or stash first.
- `remove.sh` checks that each patch applies cleanly in reverse.
  If a patch conflicts (source changed since patch was created),
  it prints the error and stops.
- `status.sh` shows which patches are applied and which are missing.
