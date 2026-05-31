#!/usr/bin/env python3
"""
timeline_viz.py — PDF timeline visualizer for event logs with invariant violation highlighting.

Usage:
    python3 timeline_viz.py --log <event_log.txt> --output <timeline.pdf>
    python3 timeline_viz.py --log <...> --window 200 --output <...>
    python3 timeline_viz.py --backend swisstm --benchmark counter \\
        --threads 4 --iters 2000 --counters 1 --output bug_timeline.pdf

Produces a horizontal timeline PDF with one lane per thread, TX
boundaries as colored segments, key events as markers, and invariant
violations highlighted in red.
"""

import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))

from event_parser import parse_event_log, group_by_thread, extract_tx_ranges, filter_by_type
from invariant_checker import check_all

# ── Try matplotlib (PDF via pgf backend) ────────────────────────
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import FancyBboxPatch, FancyArrowPatch
from matplotlib.lines import Line2D

# ── Color map by event type ─────────────────────────────────────
EVENT_COLORS = {
    "TX_BEGIN":           "#888888",
    "COMMIT_SUCCESS":     "#2ecc71",
    "TX_ABORT":           "#e74c3c",
    "READ_LOCK_ACQUIRE":  "#3498db",
    "WRITE_LOCK_ACQUIRE": "#e67e22",
    "COMMIT_LOCK_ACQUIRE":"#9b59b6",
    "COMMIT_WRITEBACK":   "#1abc9c",
    "LOCK_RELEASE":       "#95a5a6",
    "WRITE_SET_INSERT":   "#f39c12",
    "READ_VERSION_CHECK": "#c0392b",
    "GAP_CHECK":          "#e91e63",
}

# ── Find violation events ──────────────────────────────────────
def find_violation_events(parsed: dict, violations: list) -> dict:
    """Return {event_idx: violation_description} for events that triggered violations."""
    flagged = {}
    for v in violations:
        for ev_ref in v.get("events", []):
            ev_idx = ev_ref.get("_event_idx", None)
            if ev_idx is not None:
                flagged[ev_idx] = v["description"]
    return flagged


def annotate_event_indices(parsed: dict):
    """Add _event_idx to every event dict (in-place)."""
    for i, ev in enumerate(parsed["events"]):
        ev["_event_idx"] = i


def annotate_violation_events(violations: list, parsed: dict):
    """Link violation events back to their event indices by scanning
    for matching (thread_id, timestamp, type) in the parsed events."""
    for v in violations:
        for ev_ref in v.get("events", []):
            if "_event_idx" in ev_ref:
                continue
            # Heuristic match: same thread_id + type within timestamp drift
            tgt_tid = ev_ref.get("thread_id")
            tgt_type = ev_ref.get("type")
            tgt_ts = ev_ref.get("timestamp", 0)
            for i, pv in enumerate(parsed["events"]):
                if (pv["thread_id"] == tgt_tid and pv["type"] == tgt_type and
                        abs(pv["timestamp"] - tgt_ts) < 5000):
                    ev_ref["_event_idx"] = i
                    break


# ── Timeline plot ──────────────────────────────────────────────
def build_timeline(parsed: dict, violations: list,
                   window_events: int = 80, output: str = "timeline.pdf"):
    """Build and save a timeline PDF."""
    annotate_event_indices(parsed)
    annotate_violation_events(violations, parsed)
    flagged = find_violation_events(parsed, violations)

    events = parsed["events"]
    if not events:
        print("No events to plot.")
        return

    # Focus window around first violation, or last events
    violation_idxs = sorted(flagged.keys())
    if violation_idxs:
        center = violation_idxs[0]
    else:
        center = len(events) - 1
    start = max(0, center - window_events // 2)
    end = min(len(events), center + window_events // 2)
    window = events[start:end]

    # Group by thread
    by_thread = group_by_thread(window)
    thread_ids = sorted(by_thread.keys(), key=lambda x: (int(x, 16), x))

    # Assign dense x-positions (event index in window) to compress time gaps.
    # All events within the window get an even spacing — avoids wasted axis
    # space from silent periods between event clusters.
    for pos, ev in enumerate(window):
        ev["_pos"] = pos

    def x_pos(ev):
        return ev["_pos"]

    # Compute TX ranges per thread (for segment drawing)
    tx_ranges = {}
    for tid in thread_ids:
        tx_ranges[tid] = extract_tx_ranges(by_thread[tid])

    # ── Plot setup ─────────────────────────────────────────────
    n_threads = len(thread_ids)
    n_events = end - start
    fig_w = max(16, n_events * 0.08)
    fig_h = max(3.5, n_threads * 1.5 + 1.5)
    fig, ax = plt.subplots(figsize=(fig_w, fig_h))
    ax.set_yticks(range(n_threads))
    ax.set_yticklabels([f"thr=0x{t}" for t in thread_ids], fontsize=8)
    ax.set_ylim(-0.5, n_threads + 0.8)
    ax.set_xlim(-1, n_events)
    ax.set_xlabel("Event sequence (dense)", fontsize=9)
    ax.set_title("STM Event Timeline", fontsize=11, fontweight="bold")
    ax.tick_params(axis="x", labelsize=6)
    ax.grid(True, axis="x", alpha=0.2, linewidth=0.5)

    # Place a few timestamp labels on a secondary x-axis
    n_labels = min(8, n_events)
    label_step = max(1, n_events // n_labels)
    label_positions = []
    label_texts = []
    for i in range(0, n_events, label_step):
        ev = window[i]
        ts_us = (ev["timestamp"] % 1000000) / 1000.0  # μs within a second
        label_positions.append(i)
        label_texts.append(f"{ts_us:.0f}μs")
    if label_positions and label_positions[-1] != n_events - 1:
        ev = window[-1]
        ts_us = (ev["timestamp"] % 1000000) / 1000.0
        label_positions.append(n_events - 1)
        label_texts.append(f"{ts_us:.0f}μs")

    if len(label_positions) > 1:
        ax2 = ax.twiny()
        ax2.set_xlim(-1, n_events)
        ax2.set_xticks(label_positions)
        ax2.set_xticklabels(label_texts, fontsize=5, rotation=60)
        ax2.set_xlabel("Timestamp (μs)", fontsize=7)

    # ── Draw TX bands + baselines for each thread ─────────────
    for ti, tid in enumerate(thread_ids):
        t_events = by_thread[tid]
        ax.axhline(y=ti, color="#e0e0e0", linewidth=0.8, zorder=0)

        for begin_i, end_i, end_type in tx_ranges[tid]:
            p_begin = t_events[begin_i]["_pos"]
            p_end = t_events[end_i]["_pos"]
            bg = "#2ecc71" if end_type == "COMMIT_SUCCESS" else "#e74c3c"
            ax.axvspan(p_begin, p_end, ymin=(ti - 0.3) / n_threads if n_threads > 1 else 0,
                       ymax=(ti + 0.3) / n_threads if n_threads > 1 else 1,
                       alpha=0.06, color=bg, zorder=1)
            ax.plot([p_begin, p_begin], [ti - 0.25, ti + 0.25],
                    color=bg, linewidth=1.2, zorder=3)
            ax.plot([p_end, p_end], [ti - 0.25, ti + 0.25],
                    color=bg, linewidth=1.2, zorder=3)

    # ── Draw event markers ─────────────────────────────────────
    plotted_types = set()
    for ev in window:
        tid = ev["thread_id"]
        if tid not in thread_ids:
            continue
        ti = thread_ids.index(tid)
        et = ev["type"]
        x = x_pos(ev)
        y = ti
        is_violation = ev["_event_idx"] in flagged

        color = EVENT_COLORS.get(et, "#666")
        plotted_types.add(et)

        # Base dot
        ax.plot([x], [y], marker="o", color=color,
                markersize=3, alpha=0.7, zorder=2)

        # Violation: red ring + vertical dashed line to top
        if is_violation:
            ax.plot([x], [y], marker="o", color="#e74c3c",
                    markersize=8, markeredgewidth=2, markerfacecolor="none",
                    zorder=5)
            ax.plot([x], [y], marker="x", color="#e74c3c",
                    markersize=9, markeredgewidth=1.8,
                    zorder=6)

            # Vertical dashed line from event up to label area
            top_y = n_threads - 0.1
            ax.plot([x, x], [y, top_y], color="#e74c3c", linestyle=":",
                    linewidth=0.6, alpha=0.4, zorder=1)

            # Rotated (90°) annotation above all lanes
            desc = flagged[ev["_event_idx"]]
            label = f"#{ev['_event_idx']} {desc[:50]}"
            ax.annotate(label, (x, top_y),
                        xytext=(x, n_threads + 0.3),
                        fontsize=5, color="#c0392b",
                        rotation=90, va="bottom", ha="center",
                        annotation_clip=False,
                        bbox=dict(boxstyle="round,pad=0.1", fc="#fff0f0",
                                  ec="#c0392b", lw=0.5),
                        zorder=10)

        # Event type label (rotated 90°, faint, for key events)
        if et in ("TX_BEGIN", "COMMIT_SUCCESS", "TX_ABORT"):
            short = et[4:] if et.startswith("TX_") else (et[:4] if et == "COMMIT_SUCCESS" else et[:4])
            ax.annotate(short, (x, y), (x, max(ti - 0.6, -0.45)),
                        fontsize=4, color=color, alpha=0.6,
                        rotation=90, va="top", ha="center",
                        annotation_clip=False, zorder=2)

    # ── Legend ─────────────────────────────────────────────────
    legend_handles = []
    for et in sorted(plotted_types):
        c = EVENT_COLORS.get(et, "#666")
        legend_handles.append(Line2D([0], [0], marker="o", color="w",
                                     markerfacecolor=c, markersize=5,
                                     label=et))
    legend_handles.append(Line2D([0], [0], marker="o", color="w",
                                 markerfacecolor="none", markeredgecolor="#e74c3c",
                                 markeredgewidth=2, markersize=7,
                                 label="INVARIANT"))
    legend_handles.append(Line2D([0], [0], marker="x", color="#e74c3c",
                                 markersize=7, label="violation detail"))
    ax.legend(handles=legend_handles, loc="upper left",
              fontsize=5.5, ncol=5, framealpha=0.85,
              handletextpad=0.5, columnspacing=1)

    # ── Footer summary ─────────────────────────────────────────
    n_beg = len(filter_by_type(window, "TX_BEGIN"))
    n_cmt = len(filter_by_type(window, "COMMIT_SUCCESS"))
    n_abr = len(filter_by_type(window, "TX_ABORT"))
    n_vio = len(flagged)
    txt = f"Events #{start}–{end-1}  TX={n_beg}  OK={n_cmt}  ABORT={n_abr}  VIOLATIONS={n_vio}"
    fig.text(0.5, 0.005, txt, ha="center", fontsize=6, color="#666")

    plt.tight_layout(rect=[0, 0.02, 1, 1])
    fig.savefig(output, format="pdf", bbox_inches="tight", dpi=200)
    plt.close(fig)
    print(f"Timeline saved to {output} (events {start}–{end-1})")


# ── CLI ──────────────────────────────────────────────────────────
def main():
    ap = argparse.ArgumentParser(description="STM event log timeline visualizer")
    ap.add_argument("--log", help="Event log file to visualize")
    ap.add_argument("--output", default="timeline.pdf",
                    help="Output PDF path")
    ap.add_argument("--window", type=int, default=200,
                    help="Number of events in the focused window")
    ap.add_argument("--backend", help="Backend name (swisstm, tinystm, ...)")
    ap.add_argument("--benchmark", default="counter",
                    help="Benchmark name (counter, bank)")
    ap.add_argument("--threads", type=int, default=4)
    ap.add_argument("--iters", type=int, default=2000)
    ap.add_argument("--counters", type=int, default=1)
    ap.add_argument("--keep-bin", action="store_true",
                    help="Keep built binary after run")

    args = ap.parse_args()

    if args.log:
        with open(args.log) as f:
            text = f.read()
        parsed = parse_event_log(text)
        result = check_all(parsed)
        violations = result["violations"]
        if violations:
            print(f"{len(violations)} violation(s) detected.")
            for v in violations:
                print(f"  [{v['severity']}] {v['invariant']}: {v['description']}")
        else:
            print("No violations detected.")
        build_timeline(parsed, violations, window_events=args.window,
                       output=args.output)
        return

    # Live run: build & run benchmark, parse output, generate timeline
    if not args.backend:
        print("Either --log or --backend must be provided.")
        sys.exit(1)

    root = Path(__file__).resolve().parent.parent.parent
    bin_dir = Path(__file__).resolve().parent / "bin"
    bin_dir.mkdir(exist_ok=True)

    # Backend config
    backend_map = {
        "tinystm":  ("TinySTM",  "TinySTM_runtime.cpp"),
        "swisstm":  ("SwissTM",  "SwissTM_runtime.cpp"),
        "tl2":      ("TL2",      "tl2_runtime.cpp"),
        "norec":    ("NOrec",    "NOrec_runtime.cpp"),
        "wt":       ("TinySTM",  "TinySTM_runtime.cpp"),
        "wbetl":    ("TinySTM",  "TinySTM_runtime.cpp"),
    }
    if args.backend not in backend_map:
        print(f"Unknown backend: {args.backend}")
        sys.exit(1)
    bdir, runtime = backend_map[args.backend]

    # Backend-specific defines
    backend_defs = {
        "tinystm": "-DTM_BACKEND_TINYSTM -Ibackends/TinySTM",
        "swisstm": "-DTM_BACKEND_SWISSTM -Ibackends/SwissTM",
        "tl2":     "-DTM_BACKEND_TL2 -Ibackends/TL2",
        "norec":   "-DTM_BACKEND_NOREC -Ibackends/NOrec",
        "wt":      "-DTM_BACKEND_TINYSTM -DDESIGN_WT -Ibackends/TinySTM",
        "wbetl":   "-DTM_BACKEND_TINYSTM -DDESIGN_WBETL -Ibackends/TinySTM",
    }

    bench_dir = Path(__file__).resolve().parent / "benchmarks"
    bench_file = bench_dir / f"fuzz_{args.benchmark}.cpp"
    if not bench_file.exists():
        print(f"Benchmark not found: {bench_file}")
        sys.exit(1)

    binary = bin_dir / f"fuzz_{args.benchmark}_{args.backend}"
    runtime_path = root / "backends" / "runtimes" / runtime

    defines = backend_defs[args.backend]

    # Build
    seed = 42
    cmd = (f"clang++ -std=c++20 -O0 -pthread -g "
           f"-I{root} -I{root}/backends "
           f"{defines} "
           f"-DTM_EVENT_LOG "
           f"-o {binary} {bench_file} {runtime_path} -lpthread -ldl")
    print(f"BUILD: {cmd}")
    ret = os.system(cmd)
    if ret != 0:
        print("BUILD FAILED")
        sys.exit(1)

    # Run
    run_cmd = f"{binary} {args.threads} {args.iters} {args.counters} {seed} 2>&1"
    print(f"RUN: {run_cmd}")
    result = subprocess.run(run_cmd, shell=True, capture_output=True, text=True,
                            timeout=120)
    text = result.stdout
    print(text[-500:] if len(text) > 500 else text)

    if not args.keep_bin:
        try:
            os.unlink(binary)
        except OSError:
            pass

    # Parse and visualize
    parsed = parse_event_log(text)
    result = check_all(parsed)
    violations = result["violations"]
    if violations:
        print(f"\n{len(violations)} violation(s) detected.")
        for v in violations:
            print(f"  [{v['severity']}] {v['invariant']}: {v['description']}")
    else:
        print("\nNo violations detected.")

    build_timeline(parsed, violations, window_events=args.window,
                   output=args.output)


if __name__ == "__main__":
    main()
