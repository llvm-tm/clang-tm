"""
event_parser.py — Parse event logger output into structured data.

Usage:
    events = parse_event_log(text)

Each event is a dict:
    { "timestamp": int, "thread_id": str, "type": str,
      "addr1": int, "addr2": int, "data": int }
"""

import re

# Pattern for event log lines
EVENT_RE = re.compile(
    r'^\[(\d+)\]\s+thr=0x([0-9a-fA-F]+)\s+(\S+)\s+'
    r'addr1=0x([0-9a-fA-F]+)\s+addr2=0x([0-9a-fA-F]+)\s+'
    r'data=(\d+)'
)

# Pattern for SIGSEGV header
SIGSEGV_RE = re.compile(r'^=== SIGSEGV at address (0x[0-9a-fA-F]+) ===$')

# Pattern for event log header
EVENT_HEADER_RE = re.compile(
    r'^--- Event log \((\d+) entries, dumping from #(\d+)\) ---$'
)


def parse_event_log(text: str) -> dict:
    """Parse event log text into structured data.

    Returns:
        {
            "events": [list of event dicts],
            "sigsegv_addr": str or None,
            "total_entries": int,
            "dump_start": int
        }
    """
    result = {
        "events": [],
        "sigsegv_addr": None,
        "total_entries": 0,
        "dump_start": 0,
    }

    in_log = False

    for line in text.splitlines():
        line = line.strip()
        if not line:
            continue

        # Check for SIGSEGV header
        m = SIGSEGV_RE.match(line)
        if m:
            result["sigsegv_addr"] = m.group(1)
            continue

        # Check for event log header
        m = EVENT_HEADER_RE.match(line)
        if m:
            result["total_entries"] = int(m.group(1))
            result["dump_start"] = int(m.group(2))
            in_log = True
            continue

        # Parse event line
        m = EVENT_RE.match(line)
        if m and in_log:
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
