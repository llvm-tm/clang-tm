// ── TSX ground-truth validation ────────────────────────────
// Validates runtime/tsx_sim against benchmarks/tsx/ground_truth_intel14v2.txt
// (intel14v2 E5-2660 v4, Broadwell-EP, 2026-08-29, pinned 1/13).
//
// Ground truth (200k iters, short TX, free-running, pinned 1/13):
//   RR: both 0% abort — reads never conflict
//   RW: reader ~60% abort, writer ~0.1% — writer wins
//   Ground truth single-thread spurious: 0.0006% @1M
//
// This test uses deterministic overlap (both TX begin before either commits)
// via SimEngine sequential events, so the expected outcome is:
//   RR: both commit (no write to conflict)
//   RW: reader aborts, writer commits (WR check)
//   WR: symmetric
//   WW: first committer wins, second aborts? For 1-access WW free-running
//       the overlap window is tiny (0.1% aborts), so the deterministic
//       overlap case is the relevant oracle: one writer must abort.
//       Our fix changes WW from "both abort" to "other abort only".

use tm_des::backend::Backend;
use tm_des::event::{Event, EventKind};
use tm_des::sim_engine::SimEngine;

fn mmap_tm_region() {
    static MMAP: std::sync::OnceLock<()> = std::sync::OnceLock::new();
    MMAP.get_or_init(|| unsafe {
        let addr = 0x7f00_0000_0000 as *mut libc::c_void;
        let r = libc::mmap(addr, 256 * 1024 * 1024, libc::PROT_READ | libc::PROT_WRITE,
                           libc::MAP_PRIVATE | libc::MAP_ANONYMOUS | libc::MAP_FIXED, -1, 0);
        assert_ne!(r, libc::MAP_FAILED, "mmap failed");
    });
}

fn ev(ts: u64, tid: u32, seq: u64, kind: EventKind) -> Event { Event::new(ts, tid, seq, kind) }

fn run(backend: Backend, events: &[Event]) -> SimEngine {
    mmap_tm_region();
    let mut e = SimEngine::new(backend);
    e.init();
    for ev in events { e.process_event(ev); }
    e
}

const LINE: u64 = 0x7f00_0000_8000;

#[test]
fn tsx_rr_both_commit() {
    // No writes -> no conflict, both must commit. Disable spurious for determinism.
    std::env::set_var("TSX_SIM_SPURIOUS_RATE", "0");
    let events = vec![
        ev(0, 0, 1, EventKind::TxBegin),
        ev(1, 0, 2, EventKind::Read { addr: LINE, width: 8 }),
        ev(2, 1, 3, EventKind::TxBegin),
        ev(3, 1, 4, EventKind::Read { addr: LINE, width: 8 }),
        ev(4, 0, 5, EventKind::TxEnd),
        ev(5, 1, 6, EventKind::TxEnd),
    ];
    let eng = run(Backend::TsxSim, &events);
    assert_eq!(eng.stats.commits, 2, "RR: both should commit (got {} commits, {} aborts)", eng.stats.commits, eng.stats.aborts);
    assert_eq!(eng.stats.aborts, 0, "RR: no aborts");
}

#[test]
fn tsx_rw_reader_aborts() {
    std::env::set_var("TSX_SIM_SPURIOUS_RATE", "0");
    let events = vec![
        ev(0, 0, 1, EventKind::TxBegin),
        ev(1, 0, 2, EventKind::Read { addr: LINE, width: 8 }),
        ev(2, 1, 3, EventKind::TxBegin),
        ev(3, 1, 4, EventKind::Write { addr: LINE, width: 8, val: 2 }),
        ev(4, 1, 5, EventKind::TxEnd), // writer commits first -> aborts reader
        ev(5, 0, 6, EventKind::TxEnd), // reader should be aborted
    ];
    let eng = run(Backend::TsxSim, &events);
    assert_eq!(eng.stats.commits, 1, "RW writer-first commit: 1 commit (writer wins)");
    assert_eq!(eng.stats.aborts, 1, "RW writer-first commit: 1 abort (reader)");
}

#[test]
fn tsx_ww_first_committer_wins() {
    std::env::set_var("TSX_SIM_SPURIOUS_RATE", "0");
    // Both write same line, first committer should win, second abort (not both abort)
    let events = vec![
        ev(0, 0, 1, EventKind::TxBegin),
        ev(1, 0, 2, EventKind::Write { addr: LINE, width: 8, val: 1 }),
        ev(2, 1, 3, EventKind::TxBegin),
        ev(3, 1, 4, EventKind::Write { addr: LINE, width: 8, val: 2 }),
        ev(4, 0, 5, EventKind::TxEnd), // T0 commits first -> aborts T1 (WW), T0 itself commits
        ev(5, 1, 6, EventKind::TxEnd), // T1 already aborted
    ];
    let eng = run(Backend::TsxSim, &events);
    assert_eq!(eng.stats.commits, 1, "WW first committer wins: 1 commit");
    assert_eq!(eng.stats.aborts, 1, "WW first committer wins: 1 abort (other writer)");
}

#[test]
fn tsx_spurious_rate_sanity() {
    std::env::set_var("TSX_SIM_SPURIOUS_RATE", "0.000006");
    // Single-thread 10k transactions: spurious aborts should be ~0-2 at 6e-06 rate.
    // With TSX_SIM_SPURIOUS_RATE=6e-06, 10k * 6e-06 = 0.06 expected aborts.
    // We check that spurious doesn't cause mass aborts.
    let mut events = Vec::new();
    let mut ts = 0u64;
    let mut seq = 0u64;
    for _ in 0..10000 {
        seq += 1; events.push(ev(ts, 0, seq, EventKind::TxBegin)); ts += 1;
        seq += 1; events.push(ev(ts, 0, seq, EventKind::Read { addr: LINE, width: 8 })); ts += 1;
        seq += 1; events.push(ev(ts, 0, seq, EventKind::TxEnd)); ts += 1;
    }
    let eng = run(Backend::TsxSim, &events);
    assert!(eng.stats.aborts <= 10, "spurious: {} aborts in 10k single-thread (expected ~0-1 at 6e-06)", eng.stats.aborts);
    assert!(eng.stats.commits + eng.stats.aborts == 10000);
    // Reset to disable spurious for other tests
    std::env::set_var("TSX_SIM_SPURIOUS_RATE", "0");
}
