"""
event_parser.py — Parse event logger output into structured data.

Usage:
    events = parse_event_log(text)

Each event is a dict:
    { "timestamp": int, "thread_id": str, "type": str,
      "addr1": int, "addr2": int, "data": int }

Truncation:
    Since event logs can be very long (100k+ lines for 4 threads × 2000 TXs),
    use window_around_failure() to extract only the region surrounding a
    failing TX event:
        tx_logo = window_around_failure(parsed, window=200)
    This returns only the last 200 events from each thread before the
    final INVARIANT FAIL line, keeping the log small for analysis.
"""

import re

# Pattern for event log lines — handles right-aligned "[           0]" format
EVENT_RE = re.compile(
    r'^\[\s*(\d+)\]\s+thr=0x([0-9a-fA-F]+)\s+(\S+)\s+'
    r'addr1=0x([0-9a-fA-F]+)\s+addr2=0x([0-9a-fA-F]+)\s+'
    r'data=(\d+)'
)

# Pattern for SIGSEGV header
SIGSEGV_RE = re.compile(r'^=== SIGSEGV at address (0x[0-9a-fA-F]+) ===$')

# Pattern for event log header
EVENT_HEADER_RE = re.compile(
    r'^--- Event log \((\d+) entries, dumping from #(\d+)\) ---$'
)

# Pattern for invariant failure line (printed to stdout, may appear in stderr)
INVARIANT_FAIL_RE = re.compile(r'INVARIANT:.*FAIL')

# Pattern for result line (printed by fuzz_bank/fuzz_counter/fuzz_alloc to stderr)
INVARIANT_RESULT_RE = re.compile(r'^INVARIANT:')


def parse_event_log(text: str) -> dict:
    """Parse event log text into structured data.

    Returns:
        {
            "events": [list of event dicts],
            "sigsegv_addr": str or None,
            "total_entries": int,
            "dump_start": int,
            "invariant_fail": bool,
            "invariant_result": str or None
        }
    """
    result = {
        "events": [],
        "sigsegv_addr": None,
        "total_entries": 0,
        "dump_start": 0,
        "invariant_fail": False,
        "invariant_result": None,
    }

    in_log = False
    # Dict tracking the last M events for each thread during parsing
    # (only when in_log is True)

    for line in text.splitlines():
        # DON'T strip — we need leading spaces for bracket matching
        if not line.strip():
            continue

        # Check for SIGSEGV header
        m = SIGSEGV_RE.match(line)
        if m:
            result["sigsegv_addr"] = m.group(1)
            continue

        # Check for invariant result
        m = INVARIANT_RESULT_RE.match(line)
        if m:
            result["invariant_result"] = line.strip()
            if "FAIL" in line:
                result["invariant_fail"] = True
            continue

        # Check for event log header
        m = EVENT_HEADER_RE.match(line)
        if m:
            result["total_entries"] = int(m.group(1))
            result["dump_start"] = int(m.group(2))
            in_log = True
            continue

        # Parse event line (only inside a log section)
        if in_log:
            m = EVENT_RE.match(line)
            if m:
                event = {
                    "timestamp": int(m.group(1)),
                    "thread_id": m.group(2),
                    "type": m.group(3),
                    "addr1": int(m.group(4), 16),
                    "addr2": int(m.group(5), 16),
                    "data": int(m.group(6)),
                }
                result["events"].append(event)

    return result


def group_by_thread(events: list) -> dict:
    """Group events by thread_id."""
    groups = {}
    for ev in events:
        tid = ev["thread_id"]
        groups.setdefault(tid, []).append(ev)
    return groups


def filter_by_type(events: list, event_type: str) -> list:
    """Return events matching a specific type."""
    return [ev for ev in events if ev["type"] == event_type]


def extract_tx_ranges(events: list) -> list:
    """Extract (begin_idx, end_idx) pairs for each complete TX.

    A TX range starts at TX_BEGIN and ends at COMMIT_SUCCESS or TX_ABORT.
    Returns list of (start_index, end_index, end_type).
    """
    ranges = []
    begin_idx = None

    for i, ev in enumerate(events):
        if ev["type"] == "TX_BEGIN" and begin_idx is None:
            begin_idx = i
        elif ev["type"] in ("COMMIT_SUCCESS", "TX_ABORT") and begin_idx is not None:
            ranges.append((begin_idx, i, ev["type"]))
            begin_idx = None

    return ranges


def sigsegv_detected(parsed: dict) -> bool:
    """Return True if a SIGSEGV was captured in the event log."""
    return parsed["sigsegv_addr"] is not None


def window_around_failure(parsed: dict, window: int = 200) -> dict:
    """Truncate the event log to the last N events per thread.

    Event logs from fuzz runs can grow to 100k+ lines (4 threads ×
    2000 TXs × ~12 events/TX).  This function keeps only the last
    `window` events from each thread, discarding the bulk at the
    beginning.  The tail is where the bug manifests — earlier events
    are normal TM execution.

    Use after parse_event_log() to create a slimmed-down copy:

        parsed = parse_event_log(raw_text)
        slim = window_around_failure(parsed, window=100)
        for ev in slim["events"]:
            print(ev)
    """
    by_thread = group_by_thread(parsed.get("events", []))
    kept = []
    for tid, evs in by_thread.items():
        kept.extend(evs[-window:])
    kept.sort(key=lambda e: e["timestamp"])
    return {
        "events": kept,
        "sigsegv_addr": parsed["sigsegv_addr"],
        "total_entries": len(parsed.get("events", [])),
        "dump_start": parsed["dump_start"],
    }


def split_by_tx(events: list) -> list:
    """Group events into per-TX lists.

    Returns a list of (tx_number, events_list, end_type) tuples where
    end_type is 'COMMIT_SUCCESS' or 'TX_ABORT'.
    Useful for isolating the exact TX that caused the loss.
    """
    txs = []
    tx_events = []
    recents = None

    for ev in events:
        if ev["type"] == "TX_BEGIN":
            if tx_events:
                txs.append((len(txs), tx_events, "COMMIT_SUCCESS"))
            tx_events = [ev]
        else:
            tx_events.append(ev)
            if ev["type"] in ("COMMIT_SUCCESS", "TX_ABORT"):
                txs.append((len(txs), tx_events, ev["type"]))
                tx_events = []
                recents = None

    return txs
