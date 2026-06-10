use crate::event::{Event, EventKind};

/// Generate a single transfer transaction trace events.
pub fn transfer_tx(ts: u64, tid: u32, seq: u64, src_addr: u64, dst_addr: u64, _amount: u32) -> Vec<Event> {
    vec![
        Event::new(ts, tid, seq, EventKind::TxBegin),
        Event::new(ts + 1, tid, seq + 1, EventKind::Read { addr: src_addr, width: 4 }),
        Event::new(ts + 2, tid, seq + 2, EventKind::Write { addr: src_addr, width: 4, val: 0 }),
        Event::new(ts + 3, tid, seq + 3, EventKind::Read { addr: dst_addr, width: 4 }),
        Event::new(ts + 4, tid, seq + 4, EventKind::Write { addr: dst_addr, width: 4, val: 0 }),
        Event::new(ts + 5, tid, seq + 5, EventKind::TxEnd),
    ]
}

/// Generate a read-only scan transaction (read all accounts).
pub fn scan_tx(ts: u64, tid: u32, seq: u64, base_addr: u64, n_accounts: u32) -> Vec<Event> {
    let mut events = vec![Event::new(ts, tid, seq, EventKind::TxBegin)];
    for i in 0..n_accounts {
        events.push(Event::new(
            ts + 1 + i as u64,
            tid,
            seq + 1 + i as u64,
            EventKind::Read {
                addr: base_addr + i as u64 * 8,
                width: 4,
            },
        ));
    }
    events.push(Event::new(
        ts + 1 + n_accounts as u64,
        tid,
        seq + 1 + n_accounts as u64,
        EventKind::TxEnd,
    ));
    events
}

// ── Bank scenarios as trace generators ────────────────────────────────

/// Account addresses: each balance is a u32 at base + account_id * 8
pub const ACCOUNT_STRIDE: u64 = 8;

pub fn account_addr(base: u64, idx: u32) -> u64 {
    base + idx as u64 * ACCOUNT_STRIDE
}

/// Scenario 1: Simple transfer — one thread moves money between two accounts.
/// Trace: begin → read(src) → write(src) → read(dst) → write(dst) → commit
/// Expected: no aborts, money conserved.
pub fn scenario_simple_transfer(base: u64) -> Vec<Event> {
    let mut events = vec![
        Event::new(1, 0, 0, EventKind::Log {
            msg: "Scenario 1: simple transfer (no contention)".into(),
        }),
    ];
    events.extend(transfer_tx(10, 0, 1, account_addr(base, 0), account_addr(base, 1), 1));
    events
}

/// Scenario 2: Read-only scan — thread reads all 4 accounts.
/// Expected: consistent snapshot, no writes.
pub fn scenario_read_only_scan(base: u64) -> Vec<Event> {
    let mut events = vec![
        Event::new(1, 0, 0, EventKind::Log {
            msg: "Scenario 2: read-only scan of 4 accounts".into(),
        }),
    ];
    events.extend(scan_tx(10, 0, 1, base, 4));
    events
}

/// Scenario 3: Concurrent transfers to the SAME account (conflict expected).
/// Two threads both transfer from account 0 to account 1 simultaneously.
/// Expected: one commit, one abort; money conserved.
pub fn scenario_same_account_conflict(base: u64) -> Vec<Event> {
    let mut events = vec![
        Event::new(1, 0, 0, EventKind::Log {
            msg: "Scenario 3: two threads transfer same account (conflict)".into(),
        }),
    ];
    // Thread 0: transfer A→B
    events.extend(transfer_tx(10, 0, 1, account_addr(base, 0), account_addr(base, 1), 1));
    // Thread 1: transfer A→B (same accounts!)
    events.extend(transfer_tx(10, 1, 1, account_addr(base, 0), account_addr(base, 1), 1));
    events
}

/// Scenario 4: Disjoint transfers (no conflict expected).
/// Thread 0: transfer A→B, Thread 1: transfer C→D
/// Expected: both commit, money conserved.
pub fn scenario_disjoint_transfers(base: u64) -> Vec<Event> {
    let mut events = vec![
        Event::new(1, 0, 0, EventKind::Log {
            msg: "Scenario 4: two threads, disjoint accounts (no conflict)".into(),
        }),
    ];
    events.extend(transfer_tx(10, 0, 1, account_addr(base, 0), account_addr(base, 1), 1));
    events.extend(transfer_tx(10, 1, 1, account_addr(base, 2), account_addr(base, 3), 1));
    events
}

/// Scenario 5: Write-after-read conflict.
/// Thread 0: read(A) → Thread 1: write(A) → Thread 0: write(A)
/// This is the classic lost update pattern.
/// Expected: Thread 1's commit invalidates Thread 0's read-set, causing abort.
pub fn scenario_lost_update(base: u64) -> Vec<Event> {
    let mut events = vec![
        Event::new(1, 0, 0, EventKind::Log {
            msg: "Scenario 5: lost update (write-after-read conflict)".into(),
        }),
    ];
    let a = account_addr(base, 0);
    // Thread 0: begin, read(A)
    events.push(Event::new(10, 0, 1, EventKind::TxBegin));
    events.push(Event::new(11, 0, 2, EventKind::Read { addr: a, width: 4 }));
    // Thread 1: begin, read(A), write(A), commit
    events.push(Event::new(12, 1, 1, EventKind::TxBegin));
    events.push(Event::new(13, 1, 2, EventKind::Read { addr: a, width: 4 }));
    events.push(Event::new(14, 1, 3, EventKind::Write { addr: a, width: 4, val: 5 }));
    events.push(Event::new(15, 1, 4, EventKind::TxEnd));
    // Thread 0: write(A) — should abort because read-set is stale
    events.push(Event::new(16, 0, 3, EventKind::Write { addr: a, width: 4, val: 10 }));
    events.push(Event::new(17, 0, 4, EventKind::TxEnd));
    events
}

/// Scenario 6: Write-skew pattern.
/// Constraint: A + B > 0 (both accounts must always have non-negative total)
/// Thread 0: read(A)=1, read(B)=1 → write(B)=0 (OK since A+B=1 > 0)
/// Thread 1: read(A)=1, read(B)=1 → write(A)=0 (OK individually)
/// But both commit: A=0, B=0, violating A+B > 0.
/// Expected: at least one tx aborts (opacity violation).
pub fn scenario_write_skew(base: u64) -> Vec<Event> {
    let mut events = vec![
        Event::new(1, 0, 0, EventKind::Log {
            msg: "Scenario 6: write-skew (read-set staleness)".into(),
        }),
    ];
    let a = account_addr(base, 0);
    let b = account_addr(base, 1);
    // Initial state: A=1, B=1
    // Thread 0: read(A), read(B), write(B)=0
    events.push(Event::new(10, 0, 1, EventKind::TxBegin));
    events.push(Event::new(11, 0, 2, EventKind::Read { addr: a, width: 4 }));
    events.push(Event::new(12, 0, 3, EventKind::Read { addr: b, width: 4 }));
    events.push(Event::new(13, 0, 4, EventKind::Write { addr: b, width: 4, val: 0 }));
    events.push(Event::new(14, 0, 5, EventKind::TxEnd));
    // Thread 1: read(A), read(B), write(A)=0
    events.push(Event::new(10, 1, 1, EventKind::TxBegin));
    events.push(Event::new(11, 1, 2, EventKind::Read { addr: a, width: 4 }));
    events.push(Event::new(12, 1, 3, EventKind::Read { addr: b, width: 4 }));
    events.push(Event::new(13, 1, 4, EventKind::Write { addr: a, width: 4, val: 0 }));
    events.push(Event::new(14, 1, 5, EventKind::TxEnd));
    events
}

/// Generate all scenarios as a single trace file.
pub fn generate_all_scenarios(base: u64) -> Vec<Event> {
    let mut events = Vec::new();
    events.extend(scenario_simple_transfer(base));
    events.push(Event::new(100, 0, 0, EventKind::Checkpoint));
    events.extend(scenario_read_only_scan(base));
    events.push(Event::new(200, 0, 0, EventKind::Checkpoint));
    events.extend(scenario_same_account_conflict(base));
    events.push(Event::new(300, 0, 0, EventKind::Checkpoint));
    events.extend(scenario_disjoint_transfers(base));
    events.push(Event::new(400, 0, 0, EventKind::Checkpoint));
    events.extend(scenario_lost_update(base));
    events.push(Event::new(500, 0, 0, EventKind::Checkpoint));
    events.extend(scenario_write_skew(base));
    events
}
