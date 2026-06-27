// ── SimEngine integration tests ──────────────────────────
// Tests the full pipeline: construct events, feed through
// SimEngine, verify stats for both NOrec and TL2.
//
// NOTE: All backends use shared global state (GLOBAL_LOCK for NOrec,
// LOCK_TABLE/COMMIT_LOCK/G_CLOCK for TL2, lock table for TinySTM),
// so multiple SimEngine instances running in parallel produce false
// conflicts. Requires `--test-threads=1`. All tests pass serially.

use tm_des::backend::Backend;
use tm_des::event::{Event, EventKind};
use tm_des::sim_engine::SimEngine;

fn mmap_tm_region() {
    static MMAP: std::sync::OnceLock<()> = std::sync::OnceLock::new();
    MMAP.get_or_init(|| {
        unsafe {
            let addr = 0x7f00_0000_0000 as *mut libc::c_void;
            let result = libc::mmap(
                addr,
                256 * 1024 * 1024,
                libc::PROT_READ | libc::PROT_WRITE,
                libc::MAP_PRIVATE | libc::MAP_ANONYMOUS | libc::MAP_FIXED,
                -1,
                0,
            );
            if result == libc::MAP_FAILED {
                panic!("mmap failed: {}", std::io::Error::last_os_error());
            }
        }
    });
}

/// Helper: run a sequence of events through SimEngine
fn run_events(backend: Backend, events: &[Event]) -> SimEngine {
    mmap_tm_region();
    let mut engine = SimEngine::new(backend);
    engine.init();
    for e in events {
        engine.process_event(e);
    }
    engine
}

fn make_event(ts: u64, tid: u32, seq: u64, kind: EventKind) -> Event {
    Event::new(ts, tid, seq, kind)
}

// ── Basic single-threaded transactions ──────────────────

#[test]
fn test_simple_read_write_commit() {
    for b in [Backend::Norec, Backend::Tl2] {
        let events = vec![
            make_event(0, 0, 1, EventKind::ThreadSpawn(0)),
            make_event(1, 0, 2, EventKind::TxBegin),
            make_event(2, 0, 3, EventKind::Read { addr: 0x7f00_0000_8000, width: 8 }),
            make_event(3, 0, 4, EventKind::Write { addr: 0x7f00_0000_8000, width: 8, val: 42 }),
            make_event(4, 0, 5, EventKind::TxEnd),
        ];
        let engine = run_events(b, &events);
        assert_eq!(engine.stats.commits, 1, "{}: expected 1 commit", b.name());
        assert_eq!(engine.stats.aborts, 0, "{}: expected 0 aborts", b.name());
    }
}

#[test]
fn test_read_only_transaction() {
    for b in [Backend::Norec, Backend::Tl2] {
        let events = vec![
            make_event(0, 0, 1, EventKind::TxBegin),
            make_event(1, 0, 2, EventKind::Read { addr: 0x7f00_0000_8000, width: 8 }),
            make_event(2, 0, 3, EventKind::Read { addr: 0x7f00_0000_8008, width: 8 }),
            make_event(3, 0, 4, EventKind::TxEnd),
        ];
        let engine = run_events(b, &events);
        assert_eq!(engine.stats.commits, 1, "{}: read-only tx commits", b.name());
        assert_eq!(engine.stats.aborts, 0, "{}: no aborts", b.name());
    }
}

#[test]
fn test_abort_detected_outside_tx_read() {
    for b in [Backend::Norec, Backend::Tl2] {
        let events = vec![
            make_event(0, 0, 1, EventKind::Read { addr: 0x7f00_0000_8000, width: 8 }),
        ];
        let engine = run_events(b, &events);
        assert_eq!(engine.stats.reads_outside_tx, 1, "{}: outside read counted", b.name());
    }
}

#[test]
fn test_abort_detected_outside_tx_write() {
    for b in [Backend::Norec, Backend::Tl2] {
        let events = vec![
            make_event(0, 0, 1, EventKind::Write { addr: 0x7f00_0000_8000, width: 8, val: 99 }),
        ];
        let engine = run_events(b, &events);
        assert_eq!(engine.stats.writes_outside_tx, 1, "{}: outside write counted", b.name());
    }
}

// ── Conflict detection ─────────────────────────────────

fn conflict_events_same_value() -> Vec<Event> {
    // Two threads, same address, write SAME value (0).
    // NOrec value-based validation won't detect this as conflict,
    // TL2 version-based WILL detect it.
    vec![
        make_event(0, 0, 1, EventKind::ThreadSpawn(0)),
        make_event(0, 1, 2, EventKind::ThreadSpawn(1)),
        // Thread 0
        make_event(1, 0, 3, EventKind::TxBegin),
        make_event(2, 0, 4, EventKind::Read { addr: 0x7f00_0000_8000, width: 8 }),
        // Thread 1
        make_event(1, 1, 5, EventKind::TxBegin),
        make_event(2, 1, 6, EventKind::Read { addr: 0x7f00_0000_8000, width: 8 }),
        // Both write 0 (same value)
        make_event(3, 0, 7, EventKind::Write { addr: 0x7f00_0000_8000, width: 8, val: 0 }),
        make_event(3, 1, 8, EventKind::Write { addr: 0x7f00_0000_8000, width: 8, val: 0 }),
        // Commit (T0 first, T1 second)
        make_event(4, 0, 9, EventKind::TxEnd),
        make_event(4, 1, 10, EventKind::TxEnd),
    ]
}

#[test]
fn test_idempotent_write_conflict_norec() {
    // NOrec value-based validation: same value = no conflict
    let events = conflict_events_same_value();
    let engine = run_events(Backend::Norec, &events);
    assert_eq!(engine.stats.commits, 2, "NOrec: idempotent writes → no conflict");
    assert_eq!(engine.stats.aborts, 0);
}

#[test]
fn test_idempotent_write_conflict_tl2() {
    // TL2 version-based validation: T1 sees stale version → aborts
    let events = conflict_events_same_value();
    let engine = run_events(Backend::Tl2, &events);
    assert_eq!(engine.stats.commits, 1, "TL2: idempotent writes → version conflict → abort");
    assert_eq!(engine.stats.aborts, 1);
}

fn conflict_events_different_values() -> Vec<Event> {
    // Two threads, same address, write DIFFERENT values.
    // Both backends should detect this.
    vec![
        make_event(0, 0, 1, EventKind::ThreadSpawn(0)),
        make_event(0, 1, 2, EventKind::ThreadSpawn(1)),
        make_event(1, 0, 3, EventKind::TxBegin),
        make_event(2, 0, 4, EventKind::Read { addr: 0x7f00_0000_8000, width: 8 }),
        make_event(1, 1, 5, EventKind::TxBegin),
        make_event(2, 1, 6, EventKind::Read { addr: 0x7f00_0000_8000, width: 8 }),
        // T0 writes 100, T1 writes 200
        make_event(3, 0, 7, EventKind::Write { addr: 0x7f00_0000_8000, width: 8, val: 100 }),
        make_event(3, 1, 8, EventKind::Write { addr: 0x7f00_0000_8000, width: 8, val: 200 }),
        make_event(4, 0, 9, EventKind::TxEnd), // T0 commits
        make_event(4, 1, 10, EventKind::TxEnd), // T1 should abort
    ]
}

#[test]
fn test_conflict_different_values_norec() {
    let events = conflict_events_different_values();
    let engine = run_events(Backend::Norec, &events);
    // NOrec: T1 observed 0, T0 wrote 100, T1 validates: observed=0 ≠ current=100 → abort
    assert_eq!(engine.stats.commits, 1, "NOrec: different values → abort");
    assert_eq!(engine.stats.aborts, 1);
}

#[test]
fn test_conflict_different_values_tl2() {
    let events = conflict_events_different_values();
    let engine = run_events(Backend::Tl2, &events);
    // TL2: T1's version of A's lock is stale after T0's commit → abort
    assert_eq!(engine.stats.commits, 1, "TL2: different values → abort");
    assert_eq!(engine.stats.aborts, 1);
}

// ── Multi-threaded disjoint access (no conflict) ────────

fn disjoint_access_events() -> Vec<Event> {
    vec![
        make_event(0, 0, 1, EventKind::ThreadSpawn(0)),
        make_event(0, 1, 2, EventKind::ThreadSpawn(1)),
        // T0 works on addr A
        make_event(1, 0, 3, EventKind::TxBegin),
        make_event(2, 0, 4, EventKind::Read { addr: 0x7f00_0000_8000, width: 8 }),
        make_event(3, 0, 5, EventKind::Write { addr: 0x7f00_0000_8000, width: 8, val: 10 }),
        make_event(4, 0, 6, EventKind::TxEnd),
        // T1 works on addr B
        make_event(1, 1, 7, EventKind::TxBegin),
        make_event(2, 1, 8, EventKind::Read { addr: 0x7f00_0000_9000, width: 8 }),
        make_event(3, 1, 9, EventKind::Write { addr: 0x7f00_0000_9000, width: 8, val: 20 }),
        make_event(4, 1, 10, EventKind::TxEnd),
    ]
}

#[test]
fn test_disjoint_access_no_conflict() {
    for b in [Backend::Norec, Backend::Tl2] {
        let events = disjoint_access_events();
        let engine = run_events(b, &events);
        assert_eq!(engine.stats.commits, 2, "{}: disjoint → 2 commits", b.name());
        assert_eq!(engine.stats.aborts, 0, "{}: no conflict", b.name());
    }
}

// ── Verifier integration ───────────────────────────────

#[test]
fn test_verifier_tracks_inside_tx_operations() {
    let events = vec![
        make_event(0, 0, 1, EventKind::TxBegin),
        make_event(1, 0, 2, EventKind::Read { addr: 0x7f00_0000_8000, width: 8 }),
        make_event(2, 0, 3, EventKind::Write { addr: 0x7f00_0000_8000, width: 8, val: 7 }),
        make_event(3, 0, 4, EventKind::TxEnd),
    ];
    let engine = run_events(Backend::Norec, &events);
    assert_eq!(engine.verifier.commits, 1);
    assert_eq!(engine.verifier.aborts, 0);
    assert_eq!(engine.verifier.reads_outside, 0);
    assert_eq!(engine.verifier.writes_outside, 0);
    assert!(engine.verifier.violations.is_empty());
}

#[test]
fn test_verifier_shadow_alloc_free_detection() {
    let events = vec![
        make_event(0, 0, 1, EventKind::Alloc { addr: 0x7f00_0000_8000, size: 64 }),
        make_event(1, 0, 2, EventKind::Free { addr: 0x7f00_0000_8000 }),
        // Double free!
        make_event(2, 0, 3, EventKind::Free { addr: 0x7f00_0000_8000 }),
    ];
    let engine = run_events(Backend::Norec, &events);
    assert_eq!(engine.verifier.violations.len(), 1);
    assert!(engine.verifier.violations[0].contains("DOUBLE-FREE"));
}

#[test]
fn test_verifier_free_of_unallocated() {
    let events = vec![
        make_event(0, 0, 1, EventKind::Free { addr: 0x7f00_0000_BEEF }),
    ];
    let engine = run_events(Backend::Norec, &events);
    assert_eq!(engine.verifier.violations.len(), 1);
    assert!(engine.verifier.violations[0].contains("FREE of unallocated"));
}

#[test]
fn test_verifier_use_after_free() {
    let events = vec![
        make_event(0, 0, 1, EventKind::Alloc { addr: 0x7f00_0000_8000, size: 64 }),
        make_event(1, 0, 2, EventKind::Free { addr: 0x7f00_0000_8000 }),
        make_event(2, 0, 3, EventKind::TxBegin),
        make_event(3, 0, 4, EventKind::Read { addr: 0x7f00_0000_8000, width: 8 }),
        make_event(4, 0, 5, EventKind::TxEnd),
    ];
    let engine = run_events(Backend::Norec, &events);
    assert_eq!(engine.verifier.violations.len(), 1);
    assert!(engine.verifier.violations[0].contains("USE-AFTER-FREE"));
}

// ── Scenario / checkpoint boundary ─────────────────────

#[test]
fn test_scenario_boundary_reset() {
    for b in [Backend::Norec, Backend::Tl2] {
        let events = vec![
            // Scenario 0
            make_event(0, 0, 1, EventKind::TxBegin),
            make_event(1, 0, 2, EventKind::Write { addr: 0x7f00_0000_8000, width: 8, val: 42 }),
            make_event(2, 0, 3, EventKind::TxEnd),
            // Checkpoint = scenario boundary
            make_event(3, 0, 4, EventKind::Checkpoint),
            // Scenario 1
            make_event(4, 0, 5, EventKind::TxBegin),
            make_event(5, 0, 6, EventKind::Read { addr: 0x7f00_0000_8000, width: 8 }),
            make_event(6, 0, 7, EventKind::TxEnd),
        ];

        mmap_tm_region();
        let mut engine = SimEngine::new(b);
        engine.init();
        for e in &events {
            if matches!(e.kind, EventKind::Checkpoint) {
                engine.reset();
                continue;
            }
            engine.process_event(e);
        }

        assert_eq!(engine.stats.commits, 2, "{}: both scenarios commit", b.name());
        assert_eq!(engine.stats.aborts, 0, "{}: no aborts", b.name());
    }
}

// ── Assert events ──────────────────────────────────────

#[test]
fn test_assert_true_passes() {
    let events = vec![
        make_event(0, 0, 1, EventKind::Assert { cond: true, msg: "should pass".into() }),
    ];
    run_events(Backend::Norec, &events); // no panic
}

#[test]
fn test_assert_false_fails() {
    let events = vec![
        make_event(0, 0, 1, EventKind::Assert { cond: false, msg: "should fail".into() }),
    ];
    // The engine prints the error but doesn't panic — check no crash
    let _engine = run_events(Backend::Norec, &events);
}

// ── Log events ─────────────────────────────────────────

#[test]
fn test_log_event_does_not_crash() {
    let events = vec![
        make_event(0, 0, 1, EventKind::Log { msg: "test log".into() }),
    ];
    run_events(Backend::Norec, &events);
}

// ── Thread spawn/join ──────────────────────────────────

#[test]
fn test_thread_spawn_initializes_thread() {
    let events = vec![
        make_event(0, 0, 1, EventKind::ThreadSpawn(1)),
        make_event(1, 1, 2, EventKind::TxBegin),
        make_event(2, 1, 3, EventKind::Read { addr: 0x7f00_0000_8000, width: 8 }),
        make_event(3, 1, 4, EventKind::TxEnd),
    ];
    for b in [Backend::Norec, Backend::Tl2] {
        let engine = run_events(b, &events);
        assert_eq!(engine.stats.commits, 1, "{}: spawned thread commits", b.name());
    }
}

#[test]
fn test_thread_join_does_not_crash() {
    let events = vec![
        make_event(0, 0, 1, EventKind::ThreadSpawn(1)),
        make_event(1, 0, 2, EventKind::ThreadJoin(1)),
    ];
    run_events(Backend::Norec, &events);
}

// ─── various read/write widths ──────────────────────────

#[test]
fn test_u8_read_write_width() {
    for b in [Backend::Norec, Backend::Tl2] {
        let events = vec![
            make_event(0, 0, 1, EventKind::TxBegin),
            make_event(1, 0, 2, EventKind::Write { addr: 0x7f00_0000_8000, width: 1, val: 0xAB }),
            make_event(2, 0, 3, EventKind::Read { addr: 0x7f00_0000_8000, width: 1 }),
            make_event(3, 0, 4, EventKind::TxEnd),
        ];
        let engine = run_events(b, &events);
        assert_eq!(engine.stats.commits, 1, "{}: u8 read/write", b.name());
    }
}

#[test]
fn test_u16_read_write_width() {
    for b in [Backend::Norec, Backend::Tl2] {
        let events = vec![
            make_event(0, 0, 1, EventKind::TxBegin),
            make_event(1, 0, 2, EventKind::Write { addr: 0x7f00_0000_8000, width: 2, val: 0xABCD }),
            make_event(2, 0, 3, EventKind::Read { addr: 0x7f00_0000_8000, width: 2 }),
            make_event(3, 0, 4, EventKind::TxEnd),
        ];
        let engine = run_events(b, &events);
        assert_eq!(engine.stats.commits, 1, "{}: u16 read/write", b.name());
    }
}

#[test]
fn test_u32_read_write_width() {
    for b in [Backend::Norec, Backend::Tl2] {
        let events = vec![
            make_event(0, 0, 1, EventKind::TxBegin),
            make_event(1, 0, 2, EventKind::Write { addr: 0x7f00_0000_8000, width: 4, val: 0xDEADBEEF }),
            make_event(2, 0, 3, EventKind::Read { addr: 0x7f00_0000_8000, width: 4 }),
            make_event(3, 0, 4, EventKind::TxEnd),
        ];
        let engine = run_events(b, &events);
        assert_eq!(engine.stats.commits, 1, "{}: u32 read/write", b.name());
    }
}

#[test]
fn test_unsupported_read_width_errors() {
    let events = vec![
        make_event(0, 0, 1, EventKind::TxBegin),
        make_event(1, 0, 2, EventKind::Read { addr: 0x7f00_0000_8000, width: 3 }),
        make_event(2, 0, 3, EventKind::TxEnd),
    ];
    // Just verify no crash
    let _engine = run_events(Backend::Norec, &events);
}

#[test]
fn test_unsupported_write_width_errors() {
    let events = vec![
        make_event(0, 0, 1, EventKind::TxBegin),
        make_event(1, 0, 2, EventKind::Write { addr: 0x7f00_0000_8000, width: 5, val: 0 }),
        make_event(2, 0, 3, EventKind::TxEnd),
    ];
    run_events(Backend::Norec, &events);
}

// ── TinySTM WBCTL: write-only address conflict ───────────
// Regression test: C++ TinySTM WBCTL adds read-set entries for written
// addresses so validate() catches version changes from concurrent writers.
// Rust TinySTM WBCTL omitted this, allowing lost updates when a write-only
// address was modified by another transaction.
#[test]
fn test_tinystm_write_only_conflict() {
    // T0: read A, write B=100. T1: write B=200.
    // T1 commits FIRST (advancing B's lock version).
    // With the fix, T0 also adds B to read_set during write, so validation
    // catches B's version change and aborts T0.
    // Without the fix, T0 would commit (lost update).
    let events = vec![
        make_event(0, 0, 1, EventKind::ThreadSpawn(0)),
        make_event(0, 1, 2, EventKind::ThreadSpawn(1)),
        // Both begin at roughly the same "time"
        make_event(1, 0, 3, EventKind::TxBegin),
        make_event(1, 1, 4, EventKind::TxBegin),
        // T0 reads A (adds A to read_set)
        make_event(2, 0, 5, EventKind::Read { addr: 0x7f00_0000_8000, width: 8 }),
        // T1 writes B=200 (adds B to read_set via write fix)
        make_event(2, 1, 6, EventKind::Write { addr: 0x7f00_0000_9000, width: 8, val: 200 }),
        // T0 writes B=100 (adds B to read_set via write fix)
        make_event(3, 0, 7, EventKind::Write { addr: 0x7f00_0000_9000, width: 8, val: 100 }),
        // T1 commits FIRST → writes B=200 to memory, advances B's lock version
        make_event(3, 1, 8, EventKind::TxEnd),
        // T0 commits → validates read_set: B's version changed → ABORT
        make_event(4, 0, 9, EventKind::TxEnd),
    ];
    let engine = run_events(Backend::Tinystm, &events);
    assert_eq!(engine.stats.commits, 1, "TinySTM: write-only address conflict → 1 commit");
    assert_eq!(engine.stats.aborts, 1, "TinySTM: T0 aborts due to B's version change");
    assert!(engine.verifier.violations.is_empty(), "no memory violations");
}

// ── Romulus backend integration ──────────────────────────

#[test]
fn test_romulus_simple_tx() {
    let events = vec![
        make_event(0, 0, 1, EventKind::ThreadSpawn(0)),
        make_event(1, 0, 2, EventKind::TxBegin),
        make_event(2, 0, 3, EventKind::Read { addr: 0x7f00_0000_C000, width: 8 }),
        make_event(3, 0, 4, EventKind::Write { addr: 0x7f00_0000_C000, width: 8, val: 77 }),
        make_event(4, 0, 5, EventKind::TxEnd),
    ];
    let engine = run_events(Backend::Romulus, &events);
    assert_eq!(engine.stats.commits, 1, "Romulus: simple tx commits");
    assert_eq!(engine.stats.aborts, 0);
}

#[test]
fn test_romulus_conflict_detection() {
    let events = conflict_events_different_values();
    let engine = run_events(Backend::Romulus, &events);
    // Romulus OCC: T1 validates and sees T0's version bump → abort
    assert_eq!(engine.stats.commits, 1, "Romulus: conflict → abort");
    assert_eq!(engine.stats.aborts, 1);
}

#[test]
fn test_romulus_disjoint_no_conflict() {
    let events = disjoint_access_events();
    let engine = run_events(Backend::Romulus, &events);
    assert_eq!(engine.stats.commits, 2, "Romulus: disjoint → 2 commits");
    assert_eq!(engine.stats.aborts, 0);
}

// ── SwissTM backend integration ─────────────────────────

#[test]
fn test_swisstm_simple_tx() {
    let events = vec![
        make_event(0, 0, 1, EventKind::ThreadSpawn(0)),
        make_event(1, 0, 2, EventKind::TxBegin),
        make_event(2, 0, 3, EventKind::Read { addr: 0x7f00_0000_D000, width: 8 }),
        make_event(3, 0, 4, EventKind::Write { addr: 0x7f00_0000_D000, width: 8, val: 99 }),
        make_event(4, 0, 5, EventKind::TxEnd),
    ];
    let engine = run_events(Backend::Swisstm, &events);
    assert_eq!(engine.stats.commits, 1, "SwissTM: simple tx commits");
    assert_eq!(engine.stats.aborts, 0);
}

#[test]
fn test_swisstm_conflict_detection() {
    let events = conflict_events_different_values();
    let engine = run_events(Backend::Swisstm, &events);
    assert_eq!(engine.stats.commits, 1, "SwissTM: conflict → abort");
    assert_eq!(engine.stats.aborts, 1);
}

#[test]
fn test_swisstm_disjoint_no_conflict() {
    let events = disjoint_access_events();
    let engine = run_events(Backend::Swisstm, &events);
    assert_eq!(engine.stats.commits, 2, "SwissTM: disjoint → 2 commits");
    assert_eq!(engine.stats.aborts, 0);
}

#[test]
fn test_swisstm_thread_spawn() {
    let events = vec![
        make_event(0, 0, 1, EventKind::ThreadSpawn(0)),
        make_event(0, 1, 2, EventKind::ThreadSpawn(1)),
        make_event(1, 1, 3, EventKind::TxBegin),
        make_event(2, 1, 4, EventKind::Read { addr: 0x7f00_0000_E000, width: 8 }),
        make_event(3, 1, 5, EventKind::TxEnd),
    ];
    let engine = run_events(Backend::Swisstm, &events);
    assert_eq!(engine.stats.commits, 1, "SwissTM: spawned thread commits");
}

// ── Checkpoint roundtrip ───────────────────────────────────
// Verify that checkpoint/restore produces identical results.
#[test]
fn test_checkpoint_roundtrip() {
    use tm_des::checkpoint;
    for b in [Backend::Norec, Backend::Tl2, Backend::Tinystm, Backend::Romulus, Backend::Swisstm] {
        mmap_tm_region();

        // Generate a multi-scenario trace
        let events = vec![
            make_event(0, 0, 1, EventKind::ThreadSpawn(0)),
            make_event(1, 0, 2, EventKind::TxBegin),
            make_event(2, 0, 3, EventKind::Write { addr: 0x7f00_0000_8000, width: 8, val: 10 }),
            make_event(3, 0, 4, EventKind::TxEnd),
            make_event(4, 0, 5, EventKind::Checkpoint),
            make_event(5, 0, 6, EventKind::TxBegin),
            make_event(6, 0, 7, EventKind::Write { addr: 0x7f00_0000_8000, width: 8, val: 20 }),
            make_event(7, 0, 8, EventKind::TxEnd),
        ];

        // Run all events in one shot (baseline)
        let mut engine_baseline = SimEngine::new(b);
        engine_baseline.init();
        for e in &events {
            engine_baseline.process_event(e);
        }

        // Run with checkpoint/restore after the Checkpoint event
        let mut engine_cp = SimEngine::new(b);
        engine_cp.init();
        let mut remaining: Vec<Event> = events.to_vec();

        // Process events up to and including the first Checkpoint
        while let Some(pos) = remaining.iter().position(|e| matches!(e.kind, EventKind::Checkpoint)) {
            let before: Vec<Event> = remaining.drain(..=pos).collect();
            for e in &before {
                engine_cp.process_event(e);
            }
            // Snapshot and restore
            let cp = engine_cp.snapshot(&remaining);
            let bytes = bincode::serialize(&cp).expect("serialize checkpoint");
            let cp_restored: checkpoint::Checkpoint =
                bincode::deserialize(&bytes).expect("deserialize checkpoint");
            engine_cp.restore(&cp_restored).expect("restore checkpoint");
        }

        // Process remaining events
        for e in &remaining {
            engine_cp.process_event(e);
        }

        // Compare
        assert_eq!(engine_baseline.stats.commits, engine_cp.stats.commits,
                   "{}: commits match after checkpoint roundtrip", b.name());
        assert_eq!(engine_baseline.stats.aborts, engine_cp.stats.aborts,
                   "{}: aborts match after checkpoint roundtrip", b.name());
        assert_eq!(engine_baseline.verifier.violations.len(), engine_cp.verifier.violations.len(),
                   "{}: violation count matches after checkpoint roundtrip", b.name());
    }
}
