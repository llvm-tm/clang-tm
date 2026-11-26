"""
invariant_checker.py — Static invariant definitions and checking
for TM event traces.

Each invariant is a function:
    check(parsed: dict) -> list[dict]

where `parsed` is the output of event_parser.parse_event_log(). Returns a
list of violations, each:
    { "invariant": str, "severity": str,
      "description": str, "events": [matching events] }

Run it from the command line over a dumped event log (see
backends/tm_impl/common/tm_event_logger.hpp for the format and
docs/DEBUGGING_BACKENDS.md §1 for how to produce one):

    python3 tools/stm_bug_tool/invariant_checker.py --log /tmp/event_log.txt

Exit status is 1 if any CRITICAL violation was found (2 with --strict when
there are only warnings), 0 otherwise, so it can gate CI.
"""

import argparse
import sys

from event_parser import (
    parse_event_log, group_by_thread, filter_by_type,
    extract_tx_ranges, sigsegv_detected
)


# ── Invariant 1: Complete TX Lifecycle ─────────────────────────────────

def is_ring_wrapped(parsed: dict) -> bool:
    """True if ANY dump in the log wrapped (events before dump_start are lost).

    A log may contain several dumps and each has its own wrap state, so a single
    dump_start is not enough: with the driver's per-worker dumps, Wave A's four
    dumps had wrapped (from #1630 and later) while the last dump in the same file
    was pristine, and reading only the last one made the wrapped tails look like
    complete histories.
    """
    starts = parsed.get("dump_starts") or [parsed.get("dump_start", 0)]
    return any(s > 0 for s in starts)


def check_complete_tx_lifecycle(parsed: dict) -> list:
    """Every TX_BEGIN must end with COMMIT_SUCCESS or TX_ABORT, per thread.

    When the ring buffer wraps, orphan ends at the start of the log
    are expected (TX_BEGIN was in the missing portion).

    Pairing is per thread id on purpose. The driver dumps one ring per worker and
    the checker is routinely pointed at a file with several dumps concatenated, so
    pairing in one global stream pairs thread A's BEGIN with thread B's end and
    then reports every leftover as a violation: measured on a TL2 run whose driver
    oracles were all zero, the global pairing produced 9 lifecycle criticals across
    4 threads' dumps where the same shape run at -T 1 produced none.
    """
    violations = []
    wrapped = is_ring_wrapped(parsed)

    for tid, thread_events in group_by_thread(parsed["events"]).items():
        # Trace this thread's begin -> end pairs
        tx_starts = []
        for ev in thread_events:
            if ev["type"] == "TX_BEGIN":
                tx_starts.append(ev)
            elif ev["type"] in ("COMMIT_SUCCESS", "TX_ABORT"):
                if tx_starts:
                    tx_starts.pop()
                elif not wrapped:
                    # Only flag orphan ends when we know we have the full history
                    violations.append({
                        "invariant": "complete_tx_lifecycle",
                        "severity": "CRITICAL",
                        "description": f"Orphan end ({ev['type']}) without matching "
                                       f"TX_BEGIN (thread {tid})",
                        "events": [ev],
                    })

        # Any remaining TX_BEGINs without end
        for ev in tx_starts:
            # This is legitimate — the thread crashed before the TX completed
            violations.append({
                "invariant": "complete_tx_lifecycle",
                "severity": "CRITICAL",
                "description": f"TX_BEGIN (tx={ev['data']}) without matching end "
                               f"(thread {tid})",
                "events": [ev],
            })

    return violations


# ── Invariant 2: Lock Discipline ──────────────────────────────────────

def check_lock_discipline(parsed: dict) -> list:
    """Every COMMIT_LOCK_ACQUIRE must have the same thread's matching LOCK_RELEASE.

    When the ring buffer wraps, initial events may have lock acquires
    whose release was in the missing portion — skip release-less acquires
    that are within the first TX in the window.

    Like the lifecycle check, this pairs per thread id. Locks are shared, but the
    invariant being stated here is "a thread does not acquire a lock and forget to
    release it"; in a merged multi-worker log a global (address-keyed) outstanding
    set lets thread B's acquire collide with thread A's still-held lock and reports
    both a double acquire and a release with no acquire. Measured on the same zero-
    violation TL2 run: 167 findings across the dumps, 0 on the -T 1 run.
    Cross-thread mutual exclusion (two threads holding one lock at once) is a
    different property, and no invariant in this file checks it —
    check_no_orphan_locks only counts each thread's own acquire/release balance.
    """
    violations = []
    wrapped = is_ring_wrapped(parsed)

    for tid, thread_events in group_by_thread(parsed["events"]).items():
        locks_outstanding = {}  # lock_addr -> acquire_event, this thread's holds
        seen_first_tx_end = False

        for ev in thread_events:
            if ev["type"] == "COMMIT_LOCK_ACQUIRE":
                lock_addr = ev["addr1"]
                if lock_addr in locks_outstanding:
                    violations.append({
                        "invariant": "lock_discipline",
                        "severity": "WARNING",
                        "description": f"Double lock acquire at lock=0x{lock_addr:x} "
                                       f"(tx_id={ev['data']}, thread {tid})",
                        "events": [locks_outstanding[lock_addr], ev],
                    })
                locks_outstanding[lock_addr] = ev

            elif ev["type"] == "LOCK_RELEASE":
                lock_addr = ev["addr1"]
                if lock_addr not in locks_outstanding:
                    # If ring wrapped, release may match an acquire in the lost portion
                    if not wrapped or seen_first_tx_end:
                        violations.append({
                            "invariant": "lock_discipline",
                            "severity": "CRITICAL",
                            "description": f"LOCK_RELEASE without matching acquire at "
                                           f"lock=0x{lock_addr:x} (thread {tid})",
                            "events": [ev],
                        })
                else:
                    del locks_outstanding[lock_addr]

            elif ev["type"] in ("COMMIT_SUCCESS", "TX_ABORT") and not seen_first_tx_end:
                seen_first_tx_end = True  # first TX in window completed normally

        # Any locks this thread still holds at the end of the log (if no SIGSEGV)
        if not sigsegv_detected(parsed):
            for lock_addr, acquire_ev in locks_outstanding.items():
                violations.append({
                    "invariant": "lock_discipline",
                    "severity": "CRITICAL",
                    "description": f"Lock never released at lock=0x{lock_addr:x} "
                                   f"(thread {tid})",
                    "events": [acquire_ev],
                })

    return violations


# ── Invariant 3: Abort Causality ──────────────────────────────────────

ABORT_CAUSAL_EVENTS = {"READ_VERSION_CHECK", "GAP_CHECK", "WRITE_LOCK_ACQUIRE"}

def check_abort_causality(parsed: dict) -> list:
    """Each TX_ABORT should be preceded, on the same thread, by a causal event.

    Scanned per thread id like the two pairing checks (§38), for the same reason:
    `extract_tx_ranges()` walks one global stream, and the driver dumps one ring per
    worker, so in a merged log thread A's TX_BEGIN pairs with thread B's TX_ABORT and
    the event that really explained the abort falls outside the window. Measured on a
    TL2 `-T 4` run after its commit-path aborts started logging their reason (§42):
    0 of 259 and 0 of 282 aborts lack a cause when scanned per thread, against 33 and
    20 reported by the global-range version of this check on the same files.

    An abort whose TX_BEGIN is not in the retained window is not checked: on a wrapped
    ring the causal event may be in the overwritten portion, which is the same leniency
    the lifecycle and lock-discipline checks apply.
    """
    violations = []

    for tid, thread_events in group_by_thread(parsed["events"]).items():
        in_tx = False
        begin_ev = None
        cause = None
        recent = []

        for ev in thread_events:
            etype = ev["type"]
            if etype == "TX_BEGIN":
                in_tx, begin_ev, cause = True, ev, None
                recent = [ev]
            elif etype in ("COMMIT_SUCCESS", "TX_ABORT"):
                if etype == "TX_ABORT" and in_tx and cause is None:
                    violations.append({
                        "invariant": "abort_causality",
                        "severity": "WARNING",
                        "description": f"TX_ABORT on thread {tid} (TX_BEGIN "
                                       f"#{begin_ev['timestamp']}) has no preceding "
                                       f"causal event (no version check, gap check, or "
                                       f"write-lock conflict since it began)",
                        "events": recent[-3:],
                    })
                in_tx, cause = False, None
                recent = []
            else:
                if in_tx and cause is None and etype in ABORT_CAUSAL_EVENTS:
                    cause = ev
                recent.append(ev)

    return violations


# ── Invariant 4: No Orphan Locks After TX ────────────────────────────

def check_no_orphan_locks(parsed: dict) -> list:
    """Detect locks held at time of SIGSEGV but never released.

    Groups events by thread and checks that lock acquire/release counts
    match within each TX window.
    """
    violations = []
    events = parsed["events"]

    for tid, thread_events in group_by_thread(events).items():
        lock_count = 0
        for ev in thread_events:
            if ev["type"] == "COMMIT_LOCK_ACQUIRE":
                lock_count += 1
            elif ev["type"] == "LOCK_RELEASE":
                lock_count -= 1

        if lock_count > 0:
            violations.append({
                "invariant": "no_orphan_locks",
                "severity": "WARNING",
                "description": f"Thread {tid} has {lock_count} unreleased locks at end of log "
                               f"(may be in-flight abort — SIGSEGV={sigsegv_detected(parsed)})",
                "events": [],
            })

    return violations


# ── Invariant 5: Gap Check → Abort or Extend ────────────────────────

def check_gap_check_outcome(parsed: dict) -> list:
    """Every GAP_CHECK must be followed by either TX_ABORT or the
    event log should show that extend() succeeded (the TX continued)."""
    violations = []
    events = parsed["events"]

    for i, ev in enumerate(events):
        if ev["type"] != "GAP_CHECK":
            continue

        # A GAP_CHECK occurring within a TX that eventually commits or aborts
        # is OK — the gap was either resolved (extend succeeded) or abort.
        # Only flag if there is NO TX end event for many events after.
        gap_data = ev["data"]
        gap_end_version = ev["addr2"]
        gap_commit_version = gap_data

        # Check if gap was successfully closed: look for COMMIT_SUCCESS
        # or TX_ABORT within the next 1000 events
        found_end = False
        for j in range(i + 1, min(i + 1000, len(events))):
            if events[j]["type"] in ("COMMIT_SUCCESS", "TX_ABORT"):
                found_end = True
                break

        if not found_end and not sigsegv_detected(parsed):
            violations.append({
                "invariant": "gap_check_outcome",
                "severity": "WARNING",
                "description": f"GAP_CHECK at event #{i} (end_v={gap_end_version}, "
                               f"commit_v={gap_commit_version}) has no resolution "
                               f"within next 1000 events",
                "events": [ev],
            })

    return violations


# ── Invariant 6: Version Ordering ─────────────────────────────────────

def check_version_ordering(parsed: dict) -> list:
    """Within a single thread, read-set versions must be <= TX start version.

    In WBCTL, a lock with version <= start_version means the lock was last
    written before the TX began — the read is safe. A lock with version >
    start_version means a concurrent writer may have modified the data,
    so the TX must call extend() to validate.

    When the ring buffer wraps, TX_BEGIN events may be missing, so we can
    only verify ordering for TXs whose TX_BEGIN is in the visible window.

    Stale read = read_version > tx_start_version
    (Not inverted: read_version < tx_start_version is NORMAL.)

    WARNING, not CRITICAL, and deliberately so. This encodes the start-version
    discipline of the versioned-lock backends; the ones that validate by *value*
    over the whole read set — NOrec and NORECBF (their `validate()` re-reads every
    logged address and aborts on any change, backends/tm_impl/norec/NOrec.hpp:246)
    and TL2 (which validates every read-set entry at commit) — legitimately read a
    guard newer than their start version, because safety comes from that value check
    rather than from the version comparison. Measured on 2026-11-26 (docs/
    CORRECTNESS_FIXES.md §40): NOrec's snapshot advances 895 to 26 408 times per
    driver run, its full-read-set value check accounts for 100 % of the aborts the
    driver reports in every shape measured, and all four consistency oracles stay at
    0 — so these findings are expected traffic there, not evidence of a stale read.
    A CRITICAL has to be something no backend can explain; making this
    backend-aware (rather than merely softer) is still open in `TODO.md`.
    """
    violations = []
    events = parsed["events"]

    for tid, thread_events in group_by_thread(events).items():
        tx_start_version = 0
        tx_start_visible = False
        in_tx = False

        for ev in thread_events:
            if ev["type"] == "TX_BEGIN":
                tx_start_version = ev["data"]
                tx_start_visible = True
                in_tx = True
            elif ev["type"] in ("COMMIT_SUCCESS", "TX_ABORT"):
                in_tx = False
            elif ev["type"] == "READ_LOCK_ACQUIRE" and in_tx and tx_start_visible:
                read_version = ev["data"]
                if read_version == 0:
                    continue  # unowned lock — always valid regardless of start version
                if read_version > tx_start_version:
                    violations.append({
                        "invariant": "version_ordering",
                        "severity": "WARNING",
                        "description": f"Read version {read_version} > TX start version "
                                       f"{tx_start_version} — stale read on a "
                                       f"versioned-lock backend, expected traffic on a "
                                       f"value-validated one (see the invariant's "
                                       f"docstring)",
                        "events": [ev],
                    })

    return violations


# ── Invariant 7: Address Validity ──────────────────────────────────

# These event types carry actual memory addresses in addr1.
# TX_BEGIN/TX_ABORT/COMMIT_SUCCESS carry tx_id (small int) in addr1.
MEM_ADDR_EVENTS = {
    "READ_LOCK_ACQUIRE", "READ_VERSION_CHECK", "WRITE_LOCK_ACQUIRE",
    "WRITE_SET_INSERT", "COMMIT_LOCK_ACQUIRE", "COMMIT_WRITEBACK",
    "LOCK_RELEASE", "DOUBLE_FREE",
}

def addr_category(addr: int) -> str:
    """Classify an address for diagnostic purposes."""
    if addr == 0:
        return "null"
    if addr < 0x1000:
        return "page_zero"
    # Kernel-space on x86_64 (48-bit), non-canonical otherwise
    if addr >= 0xFFFF800000000000:
        return "kernel"
    # User stack region
    if 0x700000000000 <= addr <= 0x7FFFFFFFFFFF:
        return "stack"
    # Heap / data
    return "heap"

def describe_addr(addr: int) -> str:
    """Short human description of address classification."""
    cat = addr_category(addr)
    labels = {
        "null": "null pointer (0x0)",
        "page_zero": f"page-zero region (0x{addr:x})",
        "kernel": f"kernel-space (0x{addr:x})",
        "stack": f"stack (0x{addr:x})",
        "heap": f"heap (0x{addr:x})",
    }
    return labels.get(cat, f"0x{addr:x}")


def check_address_validity(parsed: dict) -> list:
    """Validate all addresses in TM events.

    Checks:
    - No null pointer (0x0) addresses in read/write/lock events
    - No page-zero addresses (< 0x1000)
    - No kernel-space addresses (>= 0xFFFF800000000000)
    - Warn on stack addresses (0x7FF...) — OK for write-back,
      problematic for eager-read backends (aggregated to avoid spam)
    """
    violations = []
    crash_addr = parsed.get("sigsegv_addr")
    crash_addr_int = int(crash_addr, 16) if crash_addr else None
    crash_related = []

    # Track stack warnings by thread for aggregation
    stack_warnings = {}  # thread_id -> list of (addr, event_type)
    critical_suspicious = []  # null/page-zero/kernel addresses

    for ev in parsed["events"]:
        if ev["type"] not in MEM_ADDR_EVENTS:
            continue

        # Only check addr1 — it's always the primary data address.
        # addr2 is often metadata (ValueType enum, version, etc.) and
        # would produce false positives (e.g., VT=4 → "page-zero").
        addr = ev.get("addr1", 0)
        if addr == 0:
            continue

        cat = addr_category(addr)

        if cat == "null":
            critical_suspicious.append(ev)

        elif cat == "page_zero":
            critical_suspicious.append(ev)

        elif cat == "kernel":
            critical_suspicious.append(ev)

        elif cat == "stack":
            tid = ev.get("thread_id", "?")
            stack_warnings.setdefault(tid, []).append((addr, ev["type"], ev))

        # Correlate with SIGSEGV address
        if crash_addr_int and abs(addr - crash_addr_int) < 0x1000:
            crash_related.append(ev)

    # Emit critical violations
    for ev in critical_suspicious:
        addr = ev.get("addr1", 0)
        cat = addr_category(addr)
        violations.append({
            "invariant": "address_validity",
            "severity": "CRITICAL",
            "description": f"{cat.title()}-zone address 0x{addr:x} in {ev['type']} "
                           f"at event #{parsed['events'].index(ev)}",
            "events": [ev],
        })

    # Emit one aggregated stack warning per thread
    for tid, entries in stack_warnings.items():
        addrs = sorted(set(a for a, _, _ in entries))
        violations.append({
            "invariant": "address_validity",
            "severity": "WARNING",
            "description": f"Stack addresses in {len(entries)} events on thread {tid}: "
                           f"{len(addrs)} unique addrs (first: 0x{addrs[0]:x}, "
                           f"last: 0x{addrs[-1]:x})"
                           " — write-back OK, eager-read may crash",
            "events": [entries[0][2], entries[-1][2]],
        })

    # Crash site
    if crash_addr:
        violations.append({
            "invariant": "address_validity",
            "severity": "CRITICAL",
            "description": f"SIGSEGV at {describe_addr(crash_addr_int)} — "
                           f"{len(crash_related)} nearby address events in log",
            "events": crash_related[-5:] if crash_related else [],
        })

    return violations


# ── Invariant 8: Crash Signature Analysis ──────────────────────────

def check_crash_signature(parsed: dict) -> list:
    """Analyze the event log leading up to a SIGSEGV crash.

    Finds the last TX before the crash and identifies the probable
    root cause by looking at the sequence of events within that TX.
    """
    violations = []
    crash_addr = parsed.get("sigsegv_addr")
    if not crash_addr:
        return violations  # no crash, nothing to analyze

    crash_addr_int = int(crash_addr, 16)
    crash_cat = addr_category(crash_addr_int)
    events = parsed["events"]

    # Find the last TX_BEGIN before dump end
    last_tx_begin = None
    last_tx_begin_idx = -1
    for i, ev in enumerate(events):
        if ev["type"] == "TX_BEGIN":
            last_tx_begin = ev
            last_tx_begin_idx = i

    # Collect events in the last TX
    tx_events = events[last_tx_begin_idx:] if last_tx_begin_idx >= 0 else events

    # Look for invalid address patterns in the last TX
    # Only check events that carry actual memory addresses (not TX_BEGIN etc.)
    invalid_steps = []
    for ev in tx_events:
        if ev["type"] not in MEM_ADDR_EVENTS:
            continue
        for field in ("addr1", "addr2"):
            addr = ev.get(field, 0)
            if addr == 0:
                continue
            cat = addr_category(addr)
            if cat in ("null", "page_zero", "kernel"):
                invalid_steps.append(ev)

    # Build root-cause narrative
    narrative_parts = []

    if crash_cat == "null":
        narrative_parts.append(f"crashed on null-ptr access at 0x0")
    elif crash_cat == "page_zero":
        narrative_parts.append(f"crashed in unmapped page-zero region at {describe_addr(crash_addr_int)}")
    elif crash_cat == "kernel":
        narrative_parts.append(f"crashed with kernel-space address {describe_addr(crash_addr_int)}")

    if invalid_steps:
        first_bad = invalid_steps[0]
        narrative_parts.append(
            f"first suspicious address was {describe_addr(first_bad.get('addr1', 0))} "
            f"at event type {first_bad['type']}"
        )

    if last_tx_begin:
        narrative_parts.append(f"within TX that started at event #{last_tx_begin_idx}")

    crashes_before = sum(1 for ev in tx_events if ev["type"] == "TX_ABORT")
    if crashes_before > 0:
        narrative_parts.append(f"{crashes_before} abort(s) occurred before crash")

    description = "; ".join(narrative_parts) if narrative_parts else f"SIGSEGV at {describe_addr(crash_addr_int)}"

    violations.append({
        "invariant": "crash_signature",
        "severity": "CRITICAL",
        "description": description,
        "events": tx_events[-10:],
    })

    return violations


# ── Invariant 9: No Phantom Lock Acquire After Abort ───────────────

def check_no_commit_after_abort(parsed: dict) -> list:
    """A TX that aborted must not show COMMIT_LOCK_ACQUIRE or
    COMMIT_WRITEBACK between the abort and the next TX_BEGIN."""
    violations = []
    events = parsed["events"]

    for tid, thread_events in group_by_thread(events).items():
        for i, ev in enumerate(thread_events):
            if ev["type"] != "TX_ABORT":
                continue
            # Search forward until next TX_BEGIN or end
            for ev2 in thread_events[i + 1:]:
                if ev2["type"] == "TX_BEGIN":
                    break
                if ev2["type"] in ("COMMIT_LOCK_ACQUIRE", "COMMIT_WRITEBACK", "COMMIT_SUCCESS"):
                    violations.append({
                        "invariant": "no_commit_after_abort",
                        "severity": "CRITICAL",
                        "description": f"Commit operation {ev2['type']} after TX_ABORT at event "
                                       f"#{events.index(ev2)} (thread {tid})",
                        "events": [ev, ev2],
                    })

    return violations


# ── Invariant 10: MALLOC/FREE Lifecycle ─────────────────────────────

# Memory event types
MEM_ALLOC_EVENTS = {"MALLOC"}
MEM_FREE_EVENTS = {"FREE"}
MEM_FLUSH_EVENTS = {"FLUSH_DEFERRED"}
MEM_CLEAR_SPEC_EVENTS = {"CLEAR_SPEC_ALLOC"}
MEM_DOUBLE_FREE_EVENT = "DOUBLE_FREE"


def has_double_free_event(parsed: dict) -> bool:
    """Return True if a DOUBLE_FREE event was captured in the log."""
    return any(ev["type"] == MEM_DOUBLE_FREE_EVENT for ev in parsed["events"])


def check_memory_lifecycle(parsed: dict) -> list:
    """Every FREE must have a matching MALLOC for the same address
    within the same thread.  Detect double-frees and orphan frees.

    Within a TX:
      MALLOC(A) ... FREE(A) ✓
      FREE(A) ... FREE(A)   ✗ double-free
      FREE(A)               ✗ orphan (no prior MALLOC in events)
      FLUSH_DEFERRED(A)     ✓ (if matched with prior FREE)

    Cross-TX (same thread): addresses can be reused after a flush,
    so we reset the tracking after FLUSH_DEFERRED or CLEAR_SPEC_ALLOC.
    """
    violations = []
    events = parsed["events"]

    for tid, thread_events in group_by_thread(events).items():
        # addr → {"mcount": int, "fcount": int, "flushed": bool}
        state = {}

        for ev in thread_events:
            addr = ev.get("addr1", 0)
            if addr == 0:
                continue
            etype = ev["type"]

            if etype in MEM_ALLOC_EVENTS:
                if addr not in state:
                    state[addr] = {"mcount": 0, "fcount": 0, "flushed": False}
                state[addr]["mcount"] += 1
                state[addr]["flushed"] = False

            elif etype in MEM_FREE_EVENTS:
                if addr not in state or state[addr]["mcount"] == state[addr]["fcount"]:
                    # No outstanding allocation → potential double-free or orphan.
                    # Check for DOUBLE_FREE event in log: if the runtime asserted
                    # double-free, this is a genuine double-free (CRITICAL).
                    # Otherwise if ring wrapped, it's likely a lost MALLOC (WARNING).
                    genuine = has_double_free_event(parsed)
                    wrapped = is_ring_wrapped(parsed)

                    if addr in state and state[addr]["flushed"]:
                        severity = "CRITICAL" if genuine else "WARNING"
                        violations.append({
                            "invariant": "memory_lifecycle",
                            "severity": severity,
                            "description": (
                                f"FREE at 0x{addr:x} on thread {tid} "
                                f"without matching MALLOC in this TX"
                                + (" [DOUBLE_FIRE event confirmed]" if genuine else
                                   " (last flush — ring may have wrapped)")
                            ),
                            "events": [ev],
                        })
                    else:
                        severity = "CRITICAL" if genuine else ("WARNING" if wrapped else "CRITICAL")
                        violations.append({
                            "invariant": "memory_lifecycle",
                            "severity": severity,
                            "description": (
                                f"FREE at 0x{addr:x} on thread {tid} "
                                f"with no prior MALLOC"
                                + (" [DOUBLE_FREE event confirmed]" if genuine else
                                   " (orphan free — ring wrap may have lost MALLOC)" if wrapped else
                                   " (orphan free — likely double-free)")
                            ),
                            "events": [ev],
                        })
                    continue
                state[addr]["fcount"] += 1

            elif etype in MEM_FLUSH_EVENTS:
                if addr not in state:
                    wrapped = is_ring_wrapped(parsed)
                    genuine = has_double_free_event(parsed)
                    severity = "CRITICAL" if (genuine or not wrapped) else "WARNING"
                    violations.append({
                        "invariant": "memory_lifecycle",
                        "severity": severity,
                        "description": (
                            f"FLUSH_DEFERRED at 0x{addr:x} on thread {tid} "
                            f"with no prior MALLOC/FREE tracking"
                            + (" [DOUBLE_FREE event confirmed]" if genuine else
                               " (ring wrap may have lost MALLOC)" if wrapped else "")
                        ),
                        "events": [ev],
                    })
                    continue
                # Mark as flushed — next FREE without MALLOC is a re-free
                state[addr]["flushed"] = True
                state[addr]["flush_event_idx"] = [
                    i for i, e in enumerate(events) if e is ev
                ][0] if len(events) < 100000 else 0

            elif etype in MEM_CLEAR_SPEC_EVENTS:
                if addr not in state:
                    violations.append({
                        "invariant": "memory_lifecycle",
                        "severity": "CRITICAL",
                        "description": (
                            f"CLEAR_SPEC_ALLOC at 0x{addr:x} on thread {tid} "
                            f"with no prior MALLOC tracking"
                        ),
                        "events": [ev],
                    })
                    continue
                # CLEAR_SPEC_ALLOC closes one MALLOC (aborted TX →
                # the speculatively-allocated memory is freed).  Decrement
                # mcount so subsequent FREE/FLUSH sees the correct balance.
                if state[addr]["mcount"] > 0:
                    state[addr]["mcount"] -= 1
                state[addr]["flushed"] = True

        # Check for leaks: outstanding MALLOCs with no matching FREE at end of log
        # Only flag if we have the complete log (no SIGSEGV and no wrap)
        if not sigsegv_detected(parsed) and not is_ring_wrapped(parsed):
            for addr, s in state.items():
                outstanding = s["mcount"] - s["fcount"]
                if outstanding > 0:
                    violations.append({
                        "invariant": "memory_lifecycle",
                        "severity": "WARNING",
                        "description": (
                            f"MALLOC leak at 0x{addr:x} on thread {tid}: "
                            f"{outstanding} outstanding free(s) at end of log"
                        ),
                        "events": [],
                    })

    return violations


# ── Invariant 11: TX-Local Memory Balance ─────────────────────────

def check_tx_memory_balance(parsed: dict) -> list:
    """Within each TX, the count of successful MALLOCs that survived
    to commit (not cleared by spec_alloc on abort) should equal the
    count of FREEs that were flushed.

    This detects TX-level alloc/free imbalances that may indicate
    memory corruption (freed pointers still in use).
    """
    violations = []
    events = parsed["events"]
    tx_ranges = extract_tx_ranges(events)

    for start_idx, end_idx, end_type in tx_ranges:
        if end_type != "COMMIT_SUCCESS":
            continue  # aborted TX: spec_alloc cleared, no problem

        tx_events = events[start_idx:end_idx + 1]
        mallocs = {}
        frees = {}
        flushes = {}
        spec_clears = {}

        for ev in tx_events:
            addr = ev.get("addr1", 0)
            if addr == 0:
                continue
            if ev["type"] in MEM_ALLOC_EVENTS:
                mallocs[addr] = mallocs.get(addr, 0) + 1
            elif ev["type"] in MEM_FREE_EVENTS:
                frees[addr] = frees.get(addr, 0) + 1
            elif ev["type"] in MEM_FLUSH_EVENTS:
                flushes[addr] = flushes.get(addr, 0) + 1
            elif ev["type"] in MEM_CLEAR_SPEC_EVENTS:
                spec_clears[addr] = spec_clears.get(addr, 0) + 1

        # After a committed TX: every FLUSH_DEFERRED(addr) should
        # correspond to a FREE(addr).  Spec-clear is only on abort.
        for addr, fcount in frees.items():
            flush_count = flushes.get(addr, 0)
            if flush_count > fcount:
                violations.append({
                    "invariant": "tx_memory_balance",
                    "severity": "CRITICAL",
                    "description": (
                        f"TX #{start_idx}—#{end_idx}: FLUSH_DEFERRED({flush_count}) "
                        f"> FREE({fcount}) for addr 0x{addr:x} — "
                        f"flush without prior free (double-free risk)"
                    ),
                    "events": [],
                })

        # Spec-cleared addresses should NOT also be freed (double-free)
        for addr in spec_clears:
            if addr in frees:
                violations.append({
                    "invariant": "tx_memory_balance",
                    "severity": "CRITICAL",
                    "description": (
                        f"TX #{start_idx}—#{end_idx}: CLEAR_SPEC_ALLOC + FREE "
                        f"for addr 0x{addr:x} — double-free risk"
                    ),
                    "events": [],
                })

    return violations


# ── Invariant 12: Address Reuse Cross-Check ──────────────────────

def check_address_reuse(parsed: dict) -> list:
    """Detect use-after-free: READ/WRITE on a freed address that was
    not re-allocated.

    For each thread: track MALLOC → FREE → FLUSH_DEFERRED cycles.
    Flag any READ_LOCK_ACQUIRE or WRITE_SET_INSERT or WRITE_SET_INSERT
    or COMMIT_WRITEBACK at an address that was FLUSH_DEFERRED but
    not re-MALLOC'd within that thread's event window.

    Only checks single-thread reuse (cross-thread requires global
    timeline not available in per-thread ring buffers).
    """
    violations = []
    events = parsed["events"]

    for tid, thread_events in group_by_thread(events).items():
        # Track the set of addresses that are "freed and not re-allocated"
        freed_not_reallocd = {}  # addr -> flush_event

        for ev in thread_events:
            addr = ev.get("addr1", 0)
            if addr == 0:
                continue
            etype = ev["type"]

            if etype in MEM_ALLOC_EVENTS:
                # Re-allocated: remove from freed set
                if addr in freed_not_reallocd:
                    del freed_not_reallocd[addr]

            elif etype in MEM_FLUSH_EVENTS:
                # Flushed: track as freed until re-allocated
                freed_not_reallocd[addr] = ev

            elif etype in MEM_CLEAR_SPEC_EVENTS:
                # Spec alloc cleared (abort): also tracked as freed
                freed_not_reallocd[addr] = ev

            elif etype in {"READ_LOCK_ACQUIRE", "WRITE_SET_INSERT",
                           "COMMIT_WRITEBACK"}:
                if addr in freed_not_reallocd:
                    violations.append({
                        "invariant": "address_reuse",
                        "severity": "CRITICAL",
                        "description": (
                            f"{etype} at 0x{addr:x} on thread {tid} "
                            f"after FLUSH_DEFERRED/CLEAR_SPEC_ALLOC — "
                            f"use-after-free risk"
                        ),
                        "events": [freed_not_reallocd[addr], ev],
                    })

            elif etype in MEM_FREE_EVENTS:
                # FREE doesn't free immediately; just check double-free
                pass

    return violations


# ── Invariant 13: Sequential Baseline Comparison ──────────────────

def check_sequential_baseline(parsed: dict) -> list:
    """Compare multi-threaded trace against single-threaded baseline.

    If a baseline file was provided (via load_baseline), compare the
    final state of each address across both runs.  Any address that
    differs indicates a TM correctness violation.

    Baseline is loaded from 'baseline' key in parsed dict.
    """
    violations = []
    baseline = parsed.get("baseline", {})
    if not baseline:
        return violations

    # Build the final state per address from this trace
    final_state = {}
    for ev in parsed["events"]:
        if ev["type"] == "WRITE" and ev.get("addr1", 0) != 0:
            addr = ev["addr1"]
            final_state[addr] = ev["data"]

    # Compare with baseline
    for addr, expected_val in baseline.items():
        actual_val = final_state.get(addr)
        if actual_val is not None and actual_val != expected_val:
            violations.append({
                "invariant": "sequential_baseline",
                "severity": "CRITICAL",
                "description": (
                    f"Address 0x{addr:x}: expected 0x{expected_val:x} "
                    f"(baseline), got 0x{actual_val:x} (multi-threaded)"
                ),
                "events": [],
            })

    return violations


# ── Invariant 14: User Oracle Support ─────────────────────────────

def check_user_oracle(parsed: dict) -> list:
    """Surface the program's own invariant verdicts in the report.

    The fuzz/oracle programs (`benchmarks/cpp/fuzz-bank/fuzz_bank.cpp`,
    `benchmarks/cpp/fuzz-counter/fuzz_counter.cpp`) print
    `INVARIANT: <name>: PASS|FAIL …` next to their event dump, and the parser
    collects those lines as `oracle_lines` because they are not events.

    Before 2026-11-26 this read `parsed["_raw_text"]`, which nothing ever set, and
    searched for "TM-INVARIANT: FAIL", a string no program in the tree prints: the
    invariant therefore reported ok for every log ever fed to it. The fixture
    `oracle_failure` in `selftest.py` is what keeps it alive now.
    """
    violations = []

    for line in parsed.get("oracle_lines", []):
        if "FAIL" not in line:
            continue
        violations.append({
            "invariant": "user_oracle",
            "severity": "CRITICAL",
            "description": line.strip(),
            "events": [],
        })

    # Legacy path: a caller that attached the raw text (e.g. a harness that only
    # has the console output) used the older TM-INVARIANT prefix.
    for line in parsed.get("_raw_text", "").splitlines():
        if "TM-INVARIANT: FAIL" in line:
            violations.append({
                "invariant": "user_oracle",
                "severity": "CRITICAL",
                "description": line.strip(),
                "events": [],
            })

    return violations


# ── Baseline loader ─────────────────────────────────────────────────

def load_baseline(path: str) -> dict:
    """Load a baseline file (format: '0xaddr 0xvalue' per line).

    Baseline files are generated by running the application
    single-threaded with stubs (no TM) and recording final
    address/value pairs.
    """
    baseline = {}
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split()
            if len(parts) >= 2:
                addr = int(parts[0], 16)
                val = int(parts[1], 16)
                baseline[addr] = val
    return baseline


# ── Run all checks ────────────────────────────────────────────────────

ALL_INVARIANTS = [
    ("Complete TX lifecycle", check_complete_tx_lifecycle),
    ("Lock discipline", check_lock_discipline),
    ("Abort causality", check_abort_causality),
    ("No orphan locks", check_no_orphan_locks),
    ("Gap check outcome", check_gap_check_outcome),
    ("Version ordering", check_version_ordering),
    ("Address validity", check_address_validity),
    ("Crash signature", check_crash_signature),
    ("No commit after abort", check_no_commit_after_abort),
    ("Memory lifecycle", check_memory_lifecycle),
    ("TX memory balance", check_tx_memory_balance),
    ("Address reuse check", check_address_reuse),
    ("Sequential baseline", check_sequential_baseline),
    ("User oracle", check_user_oracle),
]


def check_all(parsed: dict, invariants: list = None) -> dict:
    """Run all (or specified) invariant checks on parsed event log.

    Args:
        parsed: Output of event_parser.parse_event_log()
        invariants: List of (name, func) pairs, or None for all.

    Returns:
        { "violations": [list of violation dicts],
          "total": int, "passed": int, "failed": int,
          "severity": str }
    """
    if invariants is None:
        invariants = ALL_INVARIANTS

    all_violations = []

    for name, check_fn in invariants:
        violations = check_fn(parsed)
        for v in violations:
            v["invariant"] = name
        all_violations.extend(violations)

    # Count severity levels
    critical = sum(1 for v in all_violations if v["severity"] == "CRITICAL")
    warnings = sum(1 for v in all_violations if v["severity"] == "WARNING")

    severity = "PASS"
    if critical > 0:
        severity = "CRITICAL"
    elif warnings > 0:
        severity = "WARNING"

    return {
        "violations": all_violations,
        "total": len(all_violations),
        "critical": critical,
        "warnings": warnings,
        "severity": severity,
    }


# ── Command line ──────────────────────────────────────────────────────


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(
        description="Check static invariants over an STM event log dumped by "
        "backends/tm_impl/common/tm_event_logger.hpp (TM_EVENT_LOG builds).",
    )
    ap.add_argument("--log", required=True, help="event-log file to check")
    ap.add_argument(
        "--baseline",
        help="sequential-baseline file, '0xaddr 0xvalue' per line "
        "(feeds the Sequential baseline invariant)",
    )
    ap.add_argument(
        "--events",
        help="comma-separated substrings of invariant names to run (default: all)",
    )
    ap.add_argument(
        "--max-show", type=int, default=20, help="violations to print (default 20)"
    )
    ap.add_argument("--quiet", action="store_true", help="print only the summary line")
    ap.add_argument(
        "--strict", action="store_true", help="exit 2 on warnings as well as criticals"
    )
    args = ap.parse_args(argv)

    with open(args.log) as f:
        text = f.read()
    parsed = parse_event_log(text)
    if args.baseline:
        parsed["baseline"] = load_baseline(args.baseline)

    invariants = ALL_INVARIANTS
    if args.events:
        want = [s.strip().lower() for s in args.events.split(",") if s.strip()]
        invariants = [(n, fn) for n, fn in ALL_INVARIANTS if any(w in n.lower() for w in want)]
        if not invariants:
            ap.error(
                "--events matched nothing; available: "
                + ", ".join(n for n, _ in ALL_INVARIANTS)
            )

    res = check_all(parsed, invariants)
    per_inv = {}
    for v in res["violations"]:
        per_inv[v["invariant"]] = per_inv.get(v["invariant"], 0) + 1

    wrapped = is_ring_wrapped(parsed)
    dumps = len(parsed.get("dump_starts", []))
    print(f"log: {args.log}")
    print(
        f"events: {len(parsed['events'])} parsed"
        f" (last header claims {parsed.get('total_entries', '?')}),"
        f" dumps: {dumps},"
        f" threads: {len(group_by_thread(parsed['events']))},"
        f" ring wrapped: {'yes (early history lost)' if wrapped else 'no'}"
    )
    if wrapped and not args.quiet:
        print(
            "note: the ring wrapped, so lifecycle checks only see the retained tail;"
            "\n      a clean result here is weaker than on an unwrapped log."
        )
    if dumps > 1 and not args.quiet:
        print(
            f"note: {dumps} per-thread dumps are concatenated here. Complete TX"
            "\n      lifecycle and Lock discipline pair events per thread id, so the"
            "\n      merge is safe; the thread ids are the worker threads' handles and"
            "\n      a thread that ran in an earlier wave can reuse one."
        )

    if not args.quiet:
        for name, _ in invariants:
            print(f"  {'FAIL' if per_inv.get(name) else 'ok  '} {name}: "
                  f"{per_inv.get(name, 0)}")
        for v in res["violations"][: args.max_show]:
            print(f"[{v['severity']}] {v['invariant']}: {v['description']}")
        if len(res["violations"]) > args.max_show:
            print(f"... {len(res['violations']) - args.max_show} more")

    print(
        f"RESULT {res['severity']}: {res['critical']} critical,"
        f" {res['warnings']} warnings ({len(invariants)} invariants)"
    )

    if res["critical"]:
        return 1
    if args.strict and res["warnings"]:
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
