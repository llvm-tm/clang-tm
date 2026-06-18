// ── Simulation engine ────────────────────────────────────
// Drives a real TM backend through a trace file using the
// `simulation` feature flag for deterministic replay.
// All simulated threads run on the same OS thread; the
// sim_thread_id tells the backend which simulated thread's
// state to access.
//
// The engine also runs a Verifier alongside the backend to catch
// memory violations (double-free, use-after-free, out-of-TX
// accesses), check money conservation, detect livelock cycles,
// and checkpoint/restore simulation state.

use crate::backend::Backend;
use crate::checkpoint::{self, Checkpoint};
use crate::deadlock::DeadlockDetector;
use crate::event::{Event, EventKind};
use crate::verifier::Verifier;
use std::collections::{HashMap, HashSet};
use std::sync::atomic::AtomicU64;

fn alloc_tid_base() -> u64 {
    static NEXT: AtomicU64 = AtomicU64::new(1_000_000);
    NEXT.fetch_add(10_000, std::sync::atomic::Ordering::Relaxed)
}

/// Statistics collected during replay.
#[derive(Debug, Clone, Default, serde::Serialize, serde::Deserialize)]
pub struct ReplayStats {
    pub total_events: u64,
    pub commits: u64,
    pub aborts: u64,
    pub aborts_by_reason: Vec<(String, u64)>,
    pub reads_outside_tx: u64,
    pub writes_outside_tx: u64,
}

impl ReplayStats {
    pub fn abort_rate(&self) -> f64 {
        let total = self.commits + self.aborts;
        if total == 0 { 0.0 } else { 100.0 * self.aborts as f64 / total as f64 }
    }
}

/// The simulation engine.
pub struct SimEngine {
    pub stats: ReplayStats,
    pub verifier: Verifier,
    pub backend: Backend,
    pub deadlock: DeadlockDetector,
    base_tid: u64,
    in_tx: HashMap<u64, bool>,
    aborted: HashMap<u64, bool>,
    seen_threads: HashSet<u64>,
    current_write_set: HashMap<u64, Vec<u64>>,
    events_processed: u64,
}

impl SimEngine {
    pub fn new(backend: Backend) -> Self {
        SimEngine {
            stats: ReplayStats::default(),
            verifier: Verifier::new(),
            backend,
            deadlock: DeadlockDetector::new(10),
            base_tid: alloc_tid_base(),
            in_tx: HashMap::new(),
            aborted: HashMap::new(),
            seen_threads: HashSet::new(),
            current_write_set: HashMap::new(),
            events_processed: 0,
        }
    }

    fn btid(&self, event_tid: u64) -> u64 {
        self.base_tid + event_tid
    }

    /// Initialize the backend and back the TM address space.
    pub fn init(&mut self) {
        unsafe {
            let addr = 0x7f00_0000_0000 as *mut libc::c_void;
            let size: usize = 256 * 1024 * 1024;
            let result = libc::mmap(
                addr,
                size,
                libc::PROT_READ | libc::PROT_WRITE,
                libc::MAP_PRIVATE | libc::MAP_ANONYMOUS | libc::MAP_FIXED,
                -1,
                0,
            );
            if result == libc::MAP_FAILED {
                panic!("mmap of TM address space at 0x7f0000000000 failed: {}", std::io::Error::last_os_error());
            }
        }

        let b = self.backend;
        let tid0 = self.btid(0);
        b.init();
        b.sim_set_thread_id(tid0);
        b.init_thread();
        b.sim_clear_thread_id();
        self.seen_threads.insert(tid0);
    }

    /// Ensure a simulated thread has been initialized in the backend.
    fn ensure_thread(&mut self, tid: u64) {
        if self.seen_threads.insert(tid) {
            let b = self.backend;
            b.sim_set_thread_id(tid);
            b.init_thread();
            b.sim_clear_thread_id();
        }
    }

    /// Process a single event.
    pub fn process_event(&mut self, event: &Event) {
        self.stats.total_events += 1;
        let tid = self.btid(event.thread_id as u64);

        self.ensure_thread(tid);
        let b = self.backend;
        b.sim_set_thread_id(tid);
        let result = self.dispatch_event(event);
        b.sim_clear_thread_id();

        if let Err(why) = result {
            eprintln!(
                "  [ts={}] tid={}: {}",
                event.timestamp, event.thread_id, why
            );
        }

        // Run deadlock check periodically
        self.events_processed += 1;
        if self.events_processed % 100 == 0 {
            let reports = self.deadlock.check();
            for r in &reports {
                eprintln!(
                    "  ⚠ LIVELOCK CYCLE: threads [{}] at addrs [{}] — {} retries",
                    r.cycle.iter().map(|t| t.to_string()).collect::<Vec<_>>().join(", "),
                    r.conflicting_addrs.iter().map(|a| format!("0x{:x}", a)).collect::<Vec<_>>().join(", "),
                    r.retries,
                );
            }
        }
    }

    fn dispatch_event(&mut self, event: &Event) -> Result<(), String> {
        let tid = event.thread_id as u64;
        let btid = self.btid(tid);
        let b = self.backend;

        match &event.kind {
            EventKind::TxBegin => {
                self.verifier.tx_begin(tid);
                b.begin();
                self.in_tx.insert(tid, true);
                self.aborted.insert(tid, false);
                self.current_write_set.insert(tid, Vec::new());
                Ok(())
            }
            EventKind::TxEnd => {
                self.in_tx.insert(tid, false);
                let ws = self.current_write_set.remove(&tid).unwrap_or_default();
                if *self.aborted.get(&tid).unwrap_or(&false) {
                    self.aborted.insert(tid, false);
                    return Err("TxEnd after abort — skipped".into());
                }
                let ok = b.commit();
                if ok {
                    self.stats.commits += 1;
                    self.verifier.tx_commit(tid);
                    self.deadlock.record_commit(btid, &ws);
                    Ok(())
                } else {
                    self.stats.aborts += 1;
                    self.aborted.insert(tid, true);
                    self.verifier.tx_abort(tid);
                    self.deadlock.record_abort(btid, &ws);
                    b.abort();
                    Err("commit failed".into())
                }
            }
            EventKind::Read { addr, width } => {
                let in_tx = self.in_tx.get(&tid).copied().unwrap_or(false);
                let _verified = self.verifier.check_read(tid, *addr);

                if !in_tx {
                    self.stats.reads_outside_tx += 1;
                }

                let val: u64 = match width {
                    1 => b.read_u8(*addr as *mut u8) as u64,
                    2 => b.read_u16(*addr as *mut u16) as u64,
                    4 => b.read_u32(*addr as *mut u32) as u64,
                    8 => b.read_u64(*addr as *mut u64),
                    w => return Err(format!("unsupported read width {}", w)),
                };

                if in_tx {
                    self.verifier.record_value(*addr, val);
                }

                Ok(())
            }
            EventKind::Write { addr, width, val } => {
                let in_tx = self.in_tx.get(&tid).copied().unwrap_or(false);
                let _verified = self.verifier.check_write(tid, *addr, *val);

                if !in_tx {
                    self.stats.writes_outside_tx += 1;
                }

                match width {
                    1 => b.write_u8(*addr as *mut u8, *val as u8),
                    2 => b.write_u16(*addr as *mut u16, *val as u16),
                    4 => b.write_u32(*addr as *mut u32, *val as u32),
                    8 => b.write_u64(*addr as *mut u64, *val),
                    w => return Err(format!("unsupported write width {}", w)),
                }

                if in_tx {
                    self.verifier.record_value(*addr, *val);
                    self.current_write_set.entry(tid).or_default().push(*addr);
                }

                Ok(())
            }
            EventKind::ThreadSpawn(child_id) => {
                self.ensure_thread(*child_id as u64);
                Ok(())
            }
            EventKind::Alloc { addr, size } => {
                self.verifier.alloc(*addr, *size);
                Ok(())
            }
            EventKind::Free { addr } => {
                self.verifier.free(*addr);
                Ok(())
            }
            EventKind::Checkpoint => {
                Ok(())
            }
            EventKind::Assert { cond, msg } => {
                if !cond {
                    return Err(format!("assertion failed: {}", msg));
                }
                Ok(())
            }
            EventKind::Log { msg } => {
                eprintln!("  [log] {}", msg);
                Ok(())
            }
            EventKind::ThreadJoin(_) => {
                Ok(())
            }
        }
    }

    /// Create a snapshot of the full engine state for checkpointing.
    pub fn snapshot(&self, events_remaining: &[Event]) -> Checkpoint {
        checkpoint::snapshot_engine(
            &self.backend,
            events_remaining,
            self.events_processed,
            &self.stats,
            &self.verifier,
            &self.in_tx,
            &self.seen_threads,
            self.base_tid,
        )
    }

    /// Restore engine state from a checkpoint.
    pub fn restore(&mut self, cp: &Checkpoint) -> Result<(), String> {
        let (_remaining, stats, verifier, in_tx, seen_threads, base_tid) =
            checkpoint::restore_engine(cp, &self.backend)?;
        self.stats = stats;
        self.verifier = verifier;
        self.in_tx = in_tx;
        self.seen_threads = seen_threads;
        self.base_tid = base_tid;
        self.deadlock.reset();
        Ok(())
    }

    /// Reset state between scenarios.
    pub fn reset(&mut self) {
        for &tid in self.seen_threads.iter() {
            self.backend.sim_set_thread_id(tid);
            self.backend.sim_reset();
            self.backend.sim_clear_thread_id();
        }
        self.in_tx.clear();
        self.aborted.clear();
        self.seen_threads.clear();
        self.verifier.reset();
        self.deadlock.reset();
        self.current_write_set.clear();
    }
}
