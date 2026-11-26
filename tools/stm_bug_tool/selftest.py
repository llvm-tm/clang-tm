"""
selftest.py — Fixtures and expectations for invariant_checker.py.

The checker exists to turn an event log into a verdict, so two failures are
possible: it misses something (silent) or it cries wolf until nobody reads the
output (the failure mode the NOrec findings of 2026-11-26 were one step from —
docs/CORRECTNESS_FIXES.md §39/§40). Neither is caught by `compileall`, and the
checker is deliberately not run against real backends in CI because its
per-backend vocabulary is still unsettled (see TODO.md). So the checks it *can*
make are pinned here, against hand-written traces whose only content is the
defect under test:

  * each fixture asserts the exact per-invariant finding count and severity, so
    a severity change is a deliberate edit to this file and not a side effect;
  * a clean trace must produce nothing at all, and at least one fixture must
    produce a CRITICAL — otherwise the suite proves nothing;
  * the CLI's exit codes (0 / 1 / 2 with --strict) are checked too, because CI
    and docs/DEBUGGING_BACKENDS.md both tell people to gate on them.

Run:

    python3 tools/stm_bug_tool/selftest.py        # CI: the lint job runs this
    make check-pytools

Severity is part of the contract, not decoration: `Version ordering` was
CRITICAL until §40 measured that the value-validated backends (NOrec, NORECBF,
TL2) legitimately log reads newer than their start version, and it is a WARNING
since. The `stale_read` fixture pins that it is a warning *and* that it is still
reported at all, so re-promoting it has to be an explicit choice.
"""

import os
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from invariant_checker import ALL_INVARIANTS, check_all  # noqa: E402
from event_parser import parse_event_log  # noqa: E402

# check_all() labels findings with the display name from ALL_INVARIANTS; the
# fixtures state expectations with the snake_case key each check uses internally
# (`address_validity`, not `Address validity`), which is what the docs and
# TODO.md call them. Derive one from the other rather than hard-coding a table.
DISPLAY_TO_KEY = {name: fn.__name__[len("check_"):] for name, fn in ALL_INVARIANTS}

# Heap-looking addresses: addr_category() must classify every one as "heap",
# otherwise Address validity fires for the wrong reason.
DATA = "0x55555555aa00"
LOCK = "0x55555555bb00"

HEADER = "--- Event log (%d entries, dumping from #0) ---"


def log(*lines) -> str:
    """Wrap event lines in the dump header a ring dump has (unwrapped)."""
    events = "\n".join(
        f"[{i:>8}]  thr=0x1 {line}" for i, line in enumerate(lines, start=1)
    )
    return HEADER % len(lines) + "\n" + events + "\n"


def ev(etype, addr1="0x0", addr2="0x0", data=0) -> str:
    return f"{etype} addr1={addr1} addr2={addr2} data={data}"


def logx(*rows) -> str:
    """Interleave events from several threads: rows are (thread_id, event).

    The driver dumps one ring per worker, so real logs are exactly this — several
    threads' histories concatenated. Checks that pair or sequence events have to be
    thread-aware (`docs/CORRECTNESS_FIXES.md` §38, §42), and these are the traces that
    tell whether they are.
    """
    out = []
    for i, (tid, line) in enumerate(rows, start=1):
        out.append(f"[{i:>8}]  thr={tid} {line}")
    return (HEADER % len(rows)) + "\n" + "\n".join(out) + "\n"


# ── Fixtures ──────────────────────────────────────────────────────────
# Each is: name -> (log text, expected findings, note).
# `expected findings` maps invariant name -> (count, severity) and must match
# exactly: anything extra the fixture triggers is a bug in the fixture or in the
# check, and either way it should have to be explained here.

BEGIN = ev("TX_BEGIN", data=100)  # start version 100
READ = ev("READ_LOCK_ACQUIRE", DATA, "0x8", 90)  # version 90 <= 100: fine
WSET = ev("WRITE_SET_INSERT", DATA, "0x8")
ACQ = ev("COMMIT_LOCK_ACQUIRE", LOCK, "0x65", 100)
WB = ev("COMMIT_WRITEBACK", DATA, "0x8", 7)
REL = ev("LOCK_RELEASE", LOCK, "0x66")
COMMIT = ev("COMMIT_SUCCESS", data=102)
ABORT = ev("TX_ABORT", data=1)

FIXTURES = {
    # A well-formed committed transaction: every invariant must stay silent.
    "clean": (
        log(BEGIN, READ, WSET, ACQ, WB, REL, COMMIT),
        {},
        "a healthy transaction finds nothing",
    ),
    # §40: reads newer than the start version are reported, but as warnings —
    # the versioned-lock discipline they encode does not hold for the
    # value-validated backends, so CRITICAL here would flood every NOrec log.
    "stale_read": (
        log(BEGIN, ev("READ_LOCK_ACQUIRE", DATA, "0x8", 140), WSET, ACQ, WB, REL,
            COMMIT),
        {"version_ordering": (1, "WARNING")},
        "read version above start version: reported, not fatal",
    ),
    # §39: this is what made NOrec's commit lock unexpressible — an acquire with
    # no release is a leaked global lock, which serialises the runtime forever.
    "lock_never_released": (
        log(BEGIN, READ, WSET, ACQ, WB, COMMIT),
        {"lock_discipline": (1, "CRITICAL"), "no_orphan_locks": (1, "WARNING")},
        "COMMIT_LOCK_ACQUIRE without its LOCK_RELEASE",
    ),
    # The §39 payload bug itself: the clock value (0x38) logged where the address
    # of the lock belongs. Acquire and release are balanced, so only Address
    # validity may fire — on both events, since both carry the bad value — and it
    # must not be able to shrug this off.
    "lock_addr_is_a_version": (
        log(BEGIN, READ, WSET, ev("COMMIT_LOCK_ACQUIRE", "0x38", "0x39", 100), WB,
            ev("LOCK_RELEASE", "0x38", "0x3a"), COMMIT),
        {"address_validity": (2, "CRITICAL")},
        "page-zero address in a lock event",
    ),
    # An end with no begin, in a log that did not wrap, is a lost transaction.
    "orphan_end": (
        log(ACQ, WB, REL, COMMIT),
        {"complete_tx_lifecycle": (1, "CRITICAL")},
        "COMMIT_SUCCESS with no TX_BEGIN in an unwrapped log",
    ),
    # A transaction that aborts and then commits anyway is a lost-update witness.
    "commit_after_abort": (
        log(BEGIN, READ, ev("WRITE_LOCK_ACQUIRE", DATA, "0x8"), ABORT, COMMIT),
        {
            "complete_tx_lifecycle": (1, "CRITICAL"),
            "no_commit_after_abort": (1, "CRITICAL"),
            "abort_causality": (0, "WARNING"),
        },
        "TX_ABORT followed by COMMIT_SUCCESS",
    ),
    # The known vocabulary gap (TODO.md): abort_causality's causal event set does
    # not include the conflict evidence every backend logs, so it warns on clean
    # runs. Pinned as a warning so a future fix to ABORT_CAUSAL_EVENTS shows up as
    # a changed count here rather than as silently different output.
    "abort_without_causal_event": (
        log(BEGIN, READ, WSET, ABORT),
        {"abort_causality": (1, "WARNING")},
        "TX_ABORT with no event the check recognises as a cause",
    ),
    # The same transaction with the cause present must not warn.
    "abort_with_causal_event": (
        log(BEGIN, READ, ev("WRITE_LOCK_ACQUIRE", DATA, "0x8"), ABORT),
        {},
        "an abort with its conflict evidence is not a finding",
    ),
    # §42: and the cause has to be on the *same* thread. In a merged per-worker dump
    # thread B's conflict lands immediately before thread A's abort in file order, so a
    # check that scans one global stream accepts another thread's event as the
    # explanation — which is how 33 and 20 clean-run findings were reported for TL2
    # after its own abort reasons were fixed.
    "abort_cause_on_other_thread": (
        logx(("0xa", BEGIN), ("0xa", READ), ("0xb", BEGIN),
             ("0xb", ev("WRITE_LOCK_ACQUIRE", DATA, "0x8")), ("0xb", COMMIT),
             ("0xa", ABORT)),
        {"abort_causality": (1, "WARNING")},
        "the only conflict evidence belongs to a different thread",
    ),
    # The mirror: interleaving alone must not make a properly explained abort look
    # unexplained.
    "abort_cause_same_thread_interleaved": (
        logx(("0xa", BEGIN), ("0xb", BEGIN), ("0xa", READ),
             ("0xa", ev("WRITE_LOCK_ACQUIRE", DATA, "0x8")), ("0xb", READ),
             ("0xb", COMMIT), ("0xa", ABORT)),
        {},
        "interleaving is not the same as a missing cause",
    ),
    # Freeing memory nothing allocated.
    "orphan_free": (
        log(BEGIN, READ, ACQ, ev("FREE", DATA, "0x8"), REL, COMMIT),
        {"memory_lifecycle": (1, "CRITICAL")},
        "FREE with no prior MALLOC in an unwrapped log",
    ),
    # Use-after-free: touch an address the same thread already flushed away.
    "read_after_flush": (
        log(BEGIN, ev("MALLOC", DATA, "0x8"), ev("FREE", DATA, "0x8"),
            ev("FLUSH_DEFERRED", DATA, "0x8"), ev("READ_LOCK_ACQUIRE", DATA, "0x8",
                                                  90), COMMIT),
        {"address_reuse": (1, "CRITICAL")},
        "READ_LOCK_ACQUIRE at a flushed address",
    ),
    # The other oracle format in this tree, printed by the
    # tm_register_invariant_callback() wrapper (tm_hooks.cpp:831, verbatim format
    # string). It was invisible twice over before 2026-11-26: the dead `_raw_text`
    # field, and an anchored result-line regex that skipped the `TM-` prefix.
    "oracle_callback_failure": (
        log(BEGIN, READ, WSET, ACQ, WB, REL, COMMIT)
        + "TM-INVARIANT: FAIL (callback returned 1 after tx 7 on thread 0x7f0000000000)\n",
        {"user_oracle": (1, "CRITICAL")},
        "the callback oracle's TM-INVARIANT prefix reaches the report too",
    ),
    # The program's own oracle verdict, in the format fuzz_bank actually prints
    # (`INVARIANT: <name>: FAIL …`). The PASS line after it is what used to hide
    # the FAIL: the parser kept only the last result line.
    "oracle_failure": (
        log(BEGIN, READ, WSET, ACQ, WB, REL, COMMIT)
        + "INVARIANT: money conservation: FAIL (sum=99 expected=100)\n"
        + "INVARIANT: counter sum: PASS (100 == 100)\n",
        {"user_oracle": (1, "CRITICAL")},
        "an oracle FAIL in the log text, even after a PASS",
    ),
}


def run_cases() -> list:
    failures = []
    seen_critical = 0
    seen_clean = 0

    for name, (text, expected, note) in sorted(FIXTURES.items()):
        n_before = len(failures)
        parsed = parse_event_log(text)
        if not parsed["events"]:
            failures.append(f"{name}: fixture parsed to zero events")
            continue

        got = {}
        for v in check_all(parsed)["violations"]:
            key = DISPLAY_TO_KEY.get(v["invariant"], v["invariant"])
            sev = v["severity"]
            count, prev = got.get(key, (0, None))
            if prev is not None and prev != sev:
                failures.append(f"{name}: {key} emitted mixed severities "
                                f"({prev} and {sev})")
            got[key] = (count + 1, sev)

        for key, (want_count, want_sev) in sorted(expected.items()):
            have_count, have_sev = got.get(key, (0, None))
            if want_count and have_sev != want_sev:
                failures.append(f"{name}: {key} expected {want_sev} x{want_count}, "
                                f"got {have_sev} x{have_count} — {note}")
            elif have_count != want_count:
                failures.append(f"{name}: {key} expected {want_count} finding(s), "
                                f"got {have_count} — {note}")
        extra = {k: v for k, v in got.items() if k not in expected}
        for key, (count, sev) in sorted(extra.items()):
            failures.append(f"{name}: unexpected {sev} from {key} x{count} — {note}")

        criticals = sum(c for c, s in got.values() if s == "CRITICAL")
        seen_critical += 1 if criticals else 0
        seen_clean += 1 if not got else 0
        status = "ok  " if len(failures) == n_before else "FAIL"
        print(f"  {status} {name:26s} "
              f"{', '.join(f'{k}={c}/{s[0]}' for k, (c, s) in sorted(got.items())) or '-'}")

    # Non-vacuity: a suite in which nothing is ever CRITICAL, or in which nothing
    # is ever clean, is not testing the checker.
    if not seen_critical:
        failures.append("no fixture produced a CRITICAL: the fixtures are vacuous")
    if not seen_clean:
        failures.append("every fixture produced findings: the clean case is broken")

    print(f"\nfixtures: {len(FIXTURES)}, "
          f"with criticals: {seen_critical}, clean: {seen_clean}")
    return failures


def run_cli_cases() -> list:
    """The exit codes are the contract docs/DEBUGGING_BACKENDS.md gates on, and
    they are a separate code path from check_all(), so drive the real CLI."""
    checker = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           "invariant_checker.py")
    cases = [
        ("clean", [], 0),
        ("stale_read", [], 0),
        ("stale_read", ["--strict"], 2),
        ("lock_never_released", [], 1),
        ("orphan_end", ["--strict"], 1),
    ]
    failures = []
    with tempfile.TemporaryDirectory() as d:
        for name, flags, want in cases:
            path = os.path.join(d, name + ".log")
            with open(path, "w") as f:
                f.write(FIXTURES[name][0])
            p = subprocess.run([sys.executable, checker, "--log", path, "--quiet"]
                               + flags, capture_output=True, text=True)
            got = p.returncode
            ok = got == want
            print(f"  {'ok  ' if ok else 'FAIL'} exit {want} <- {' '.join(flags) or '(none)'}"
                  f" {name}: got {got}")
            if not ok:
                failures.append(f"cli {name} {' '.join(flags)}: expected exit {want},"
                                f" got {got}\n{p.stdout}{p.stderr}")
    return failures


def main() -> int:
    print("invariant_checker fixtures:")
    failures = run_cases()
    print("invariant_checker exit codes:")
    failures += run_cli_cases()
    if failures:
        print(f"\nselftest FAILED ({len(failures)}):")
        for f in failures:
            print(f"  - {f}")
        return 1
    print("\n=== stm_bug_tool selftest: ALL PASSED ===")
    return 0


if __name__ == "__main__":
    sys.exit(main())
