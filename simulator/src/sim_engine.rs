// ── Simulation engine ────────────────────────────────────
// Drives a real TM backend through a trace file using the
// `simulation` feature flag for deterministic replay.
// All simulated threads run on the same OS thread; the
// sim_thread_id tells the backend which simulated thread's
// state to access.
//
// Two clock modes:
//   Timestamp — events advance clock to their trace timestamp
//   Cost      — events advance clock by estimated cycle costs
//              from the calibrated cost model, while the real
//              backend detects conflicts/aborts naturally
//
// The cost mode enables "what-if" analysis: given a trace and a
// machine profile, estimate how fast the workload would run on
// different hardware or with a different TM backend.

use crate::backend::Backend;
use crate::checkpoint::{self, Checkpoint};
use crate::computation_profile::ComputationProfile;
use crate::cost_model::CalibratedCostModel;
use crate::deadlock::DeadlockDetector;
use crate::event::{Event, EventKind};
use crate::verifier::Verifier;
use runtime_core::TmxAbort;
use std::cmp;
use std::collections::{HashMap, HashSet, VecDeque};
use std::panic::{self, AssertUnwindSafe};
use std::sync::atomic::AtomicU64;

/// Clock advancement mode for the simulation engine.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SimClockMode {
    /// Use event timestamps directly (original behavior).
    Timestamp,
    /// Accumulate estimated cycle costs per event.
    Cost,
}

fn alloc_tid_base() -> u64 {
    static NEXT: AtomicU64 = AtomicU64::new(1_000_000);
    NEXT.fetch_add(10_000, std::sync::atomic::Ordering::Relaxed)
}

/// Default TM region base address used when the trace address range is unavailable.
pub const DEFAULT_TM_BASE: u64 = 0x7f00_0000_0000;
pub const DEFAULT_TM_SIZE: usize = 256 * 1024 * 1024;

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
    /// Clock advancement mode.
    pub sim_clock_mode: SimClockMode,
    /// Calibrated cost model (used in Cost mode).
    pub cost_model: Option<CalibratedCostModel>,
    /// Accumulated estimated cycles (Cost mode).
    pub estimated_cycles: u64,
    /// Frequency in GHz for time estimation.
    pub freq_ghz: f64,
    /// Baseline computation profile (optional).
    pub computation_profile: Option<ComputationProfile>,
    /// Address translation addend: trace_addr + addr_addend → backend mmap addr.
    /// Zero when no translation is needed.
    pub addr_addend: i64,
    /// Pending TxBegin events buffered for Phase 4 batch processing.
    /// Stores raw event thread_ids (not btid'd).
    /// Flushed before the first non-TxBegin event in each iteration.
    pending_begins: VecDeque<u64>,
    /// Cross-transaction abort accumulator (per raw thread_id).
    /// When a thread reaches max_retries consecutive aborts across
    /// iterations, its next begin is forced to SGL fallback.
    /// This compensates for the sequential event model's inability
    /// to interleave retries within a single tm_begin() call.
    persistent_retries: HashMap<u64, u64>,
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
            sim_clock_mode: SimClockMode::Timestamp,
            cost_model: None,
            estimated_cycles: 0,
            freq_ghz: 3.0,
            computation_profile: None,
            addr_addend: 0,
            pending_begins: VecDeque::new(),
            persistent_retries: HashMap::new(),
        }
    }

    /// Set cost mode with a calibrated model and frequency.
    pub fn set_cost_mode(&mut self, model: CalibratedCostModel, freq_ghz: f64) {
        self.sim_clock_mode = SimClockMode::Cost;
        self.cost_model = Some(model);
        self.freq_ghz = freq_ghz;
        self.estimated_cycles = 0;
    }

    /// Set a computation baseline profile for total wall-time estimation.
    pub fn set_computation_profile(&mut self, profile: ComputationProfile) {
        self.computation_profile = Some(profile);
    }

    /// Get the cycle cost for an event kind.
    fn event_cost(&self, kind: &EventKind) -> u64 {
        match &self.sim_clock_mode {
            SimClockMode::Cost => {
                if let Some(ref model) = self.cost_model {
                    model.event_cost(kind)
                } else {
                    0
                }
            }
            SimClockMode::Timestamp => 0,
        }
    }

    fn btid(&self, event_tid: u64) -> u64 {
        self.base_tid + event_tid
    }

    /// Translate a trace address to the mapped backend address.
    fn translate_addr(&self, trace_addr: u64) -> u64 {
        (trace_addr as i64 + self.addr_addend) as u64
    }

    /// Initialize the backend and back the TM address space.
    /// Uses the default fixed address (0x7f00_0000_0000).
    pub fn init(&mut self) {
        self.init_at(0x7f00_0000_0000 as *mut libc::c_void, 256 * 1024 * 1024);
    }

    /// Initialize with a specific base address and size.
    /// Scans a slice of events to determine the address range if base is null.
    pub fn init_from_events(&mut self, events: &[Event]) {
        let (base, size) = compute_address_range(events);
        self.init_at(base, size);
    }

    /// Low-level initialization at a given address range.
    /// Maps memory at the trace's expected address if possible, otherwise
    /// maps at the default TM region and sets an address translation addend.
    pub fn init_at(&mut self, trace_addr: *mut libc::c_void, size: usize) {
        // Map at the default TM region (safe — never overlaps process segments).
        // The trace_addr computed from events may overlap the process stack
        // or heap, so we always map at the safe default and translate.
        let (mapped_base, addend) = unsafe {
            let r = libc::mmap(
                DEFAULT_TM_BASE as *mut libc::c_void, size,
                libc::PROT_READ | libc::PROT_WRITE,
                libc::MAP_PRIVATE | libc::MAP_ANONYMOUS | libc::MAP_FIXED,
                -1, 0,
            );
            if r != libc::MAP_FAILED {
                let addend = DEFAULT_TM_BASE as i64 - trace_addr as i64;
                (DEFAULT_TM_BASE, addend)
            } else {
                // Fall back to kernel-chosen address.
                let r = libc::mmap(
                    std::ptr::null_mut(), size,
                    libc::PROT_READ | libc::PROT_WRITE,
                    libc::MAP_PRIVATE | libc::MAP_ANONYMOUS,
                    -1, 0,
                );
                if r == libc::MAP_FAILED {
                    panic!("mmap failed: {}", std::io::Error::last_os_error());
                }
                let addend = r as i64 - trace_addr as i64;
                eprintln!("  [sim] kernel-chosen mmap at {:p}, addend={:#x}", r, addend);
                (r as u64, addend)
            }
        };

        self.addr_addend = addend;

        // Zero the mapped region so uninitialized reads return 0.
        unsafe { std::ptr::write_bytes(mapped_base as *mut u8, 0, size); }
        eprintln!("  [sim] addr_addend={:#x} mapped={:#x} size={}", addend, mapped_base, size);

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

        // TxBegin: buffer for Phase 4 batch processing (tsx-sim backend)
        // or immediate dispatch (other backends).
        if matches!(event.kind, EventKind::TxBegin) {
            if self.backend.is_tsx_sim() {
                // Phase 4: buffer begin for batch retry processing.
                // Costs are accumulated inside flush_pending_begins().
                // Store raw event thread_id (not btid'd).
                self.pending_begins.push_back(event.thread_id as u64);
                self.events_processed += 1;
                return;
            }
            // Non-TSX backend: dispatch immediately (no retry needed)
        } else {
            // Non-TxBegin: flush any pending begins first so that all
            // threads' begin events are processed before the first
            // read/write/end event of the iteration.
            if !self.pending_begins.is_empty() {
                self.flush_pending_begins();
            }
        }

        let b = self.backend;
        b.sim_set_thread_id(tid);

        // Accumulate cycle cost before dispatching
        let event_cost = self.event_cost(&event.kind);
        self.estimated_cycles += event_cost;

        let result = self.dispatch_event(event);

        // Charge abort overhead if the abort came from the backend
        if result.is_err() {
            let abort_cost = self.event_cost(&EventKind::Abort { reason: 0 });
            self.estimated_cycles += abort_cost;
        }

        b.sim_clear_thread_id();

        // In cost mode, print a summary line every 10k events
        if self.sim_clock_mode == SimClockMode::Cost
            && self.events_processed > 0
            && self.events_processed % 10000 == 0
        {
            let secs = self.estimated_cycles as f64 / (self.freq_ghz * 1e9);
            eprintln!(
                "  [cost] events={} cycles={} est_time={:.3}s",
                self.events_processed, self.estimated_cycles, secs
            );
        }

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

    /// Flush any buffered TxBegin events.  For the tsx-sim backend, this
    /// runs the retry loop round-robin (Phase 4): each thread gets one
    /// try_begin() attempt per round, and other threads' attempts
    /// interleave naturally.  After max_retries rounds without success,
    /// remaining threads are forced to SGL fallback.  For all other
    /// backends, simply calls begin() for each buffered thread.
    ///
    /// `pending_begins` stores raw event thread_ids; `self.btid(raw)`
    /// converts to the backend's thread id.
    fn flush_pending_begins(&mut self) {
        let mut pending: Vec<u64> = self.pending_begins.drain(..).collect();
        let max_r = 5; // matches TSXSGL and tsx-sim default

        if !self.backend.is_tsx_sim() {
            // Non-TSX backends: no retry needed
            for &raw_tid in &pending {
                let btid = self.btid(raw_tid);
                self.backend.sim_set_thread_id(btid);
                self.backend.begin();
                self.backend.sim_clear_thread_id();
                self.verifier.tx_begin(raw_tid);
                self.in_tx.insert(raw_tid, true);
                self.aborted.insert(raw_tid, false);
                self.current_write_set.insert(raw_tid, Vec::new());
            }
            return;
        }

        // TSX-sim backend: Phase 4 batch retry loop.
        // Round-robin through pending threads.  Each thread that fails
        // try_begin() (LOCK_BUSY due to SGL_OWNER held) charges cost and
        // retries in the next round.  After max_r rounds, remaining
        // threads enter SGL fallback.
        let mut attempts: HashMap<u64, u64> = HashMap::new();
        let mut in_tx: HashSet<u64> = HashSet::new();

        for _round in 0..max_r {
            if pending.is_empty() { break; }
            let still_pending: Vec<u64> = pending.clone();
            for &raw_tid in &still_pending {
                if in_tx.contains(&raw_tid) { continue; }
                let a = attempts.entry(raw_tid).or_insert(0);
                let btid = self.btid(raw_tid);

                self.backend.sim_set_thread_id(btid);
                let ok = self.backend.try_begin();
                self.backend.sim_clear_thread_id();
                *a += 1;

                // Charge cost for each retry attempt
                self.estimated_cycles += 60; // COST_XBEGIN
                if !ok {
                    self.estimated_cycles += 1500; // COST_XABORT
                }

                if ok {
                    in_tx.insert(raw_tid);
                    self.verifier.tx_begin(raw_tid);
                    self.in_tx.insert(raw_tid, true);
                    self.aborted.insert(raw_tid, false);
                    self.current_write_set.insert(raw_tid, Vec::new());
                } else if *a >= max_r {
                    // Force SGL fallback after max_r consecutive failures
                    self.backend.sim_set_thread_id(btid);
                    self.backend.force_sgl();
                    self.backend.sim_clear_thread_id();
                    self.estimated_cycles += 75; // COST_MUTEX_LOCK
                    in_tx.insert(raw_tid);
                    self.verifier.tx_begin(raw_tid);
                    self.in_tx.insert(raw_tid, true);
                    self.aborted.insert(raw_tid, false);
                    self.current_write_set.insert(raw_tid, Vec::new());
                }
            }
            // Remove threads that entered TX from pending list
            pending.retain(|&raw_tid| !in_tx.contains(&raw_tid));
        }

        // Any stragglers that didn't enter (shouldn't happen since we force
        // SGL after max_r rounds, but be safe): force SGL
        for &raw_tid in &pending {
            if in_tx.contains(&raw_tid) { continue; }
            let btid = self.btid(raw_tid);
            self.backend.sim_set_thread_id(btid);
            self.backend.force_sgl();
            self.backend.sim_clear_thread_id();
            self.estimated_cycles += 75;
            self.verifier.tx_begin(raw_tid);
            self.in_tx.insert(raw_tid, true);
            self.aborted.insert(raw_tid, false);
            self.current_write_set.insert(raw_tid, Vec::new());
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
            EventKind::Abort { reason } => {
                self.in_tx.insert(tid, false);
                self.aborted.insert(tid, true);
                let ws = self.current_write_set.remove(&tid).unwrap_or_default();
                self.stats.aborts += 1;
                self.verifier.tx_abort(tid);
                self.deadlock.record_abort(btid, &ws);
                b.abort();
                Err(format!("abort (reason={})", reason))
            }
            EventKind::Read { addr, width } => {
                let mem_addr = self.translate_addr(*addr);
                let in_tx = self.in_tx.get(&tid).copied().unwrap_or(false);
                let _verified = self.verifier.check_read(tid, *addr);

                if !in_tx {
                    self.stats.reads_outside_tx += 1;
                }

                let val: u64 = match *width {
                    1 | 2 | 4 | 8 => {
                        let read_result = panic::catch_unwind(AssertUnwindSafe(|| {
                            match width {
                                1 => b.read_u8(mem_addr as *mut u8) as u64,
                                2 => b.read_u16(mem_addr as *mut u16) as u64,
                                4 => b.read_u32(mem_addr as *mut u32) as u64,
                                8 => b.read_u64(mem_addr as *mut u64),
                                _ => unreachable!(),
                            }
                        }));
                        match read_result {
                            Ok(v) => v,
                            Err(e) => {
                                if e.downcast_ref::<TmxAbort>().is_some() {
                                    self.in_tx.insert(tid, false);
                                    self.aborted.insert(tid, true);
                                    let ws = self.current_write_set.remove(&tid).unwrap_or_default();
                                    self.stats.aborts += 1;
                                    self.verifier.tx_abort(tid);
                                    self.deadlock.record_abort(btid, &ws);
                                    return Err("aborted during read".into());
                                }
                                panic::resume_unwind(e);
                            }
                        }
                    }
                    w => return Err(format!("unsupported read width {}", w)),
                };

                if in_tx {
                    self.verifier.record_value(*addr, val);
                }

                Ok(())
            }
            EventKind::Write { addr, width, val } => {
                let mem_addr = self.translate_addr(*addr);
                let in_tx = self.in_tx.get(&tid).copied().unwrap_or(false);
                let _verified = self.verifier.check_write(tid, *addr, *val);

                if !in_tx {
                    self.stats.writes_outside_tx += 1;
                }

                match *width {
                    1 | 2 | 4 | 8 => {
                        let write_result = panic::catch_unwind(AssertUnwindSafe(|| {
                            match width {
                                1 => b.write_u8(mem_addr as *mut u8, *val as u8),
                                2 => b.write_u16(mem_addr as *mut u16, *val as u16),
                                4 => b.write_u32(mem_addr as *mut u32, *val as u32),
                                8 => b.write_u64(mem_addr as *mut u64, *val),
                                _ => unreachable!(),
                            }
                        }));
                        if let Err(e) = write_result {
                            if e.downcast_ref::<TmxAbort>().is_some() {
                                self.in_tx.insert(tid, false);
                                self.aborted.insert(tid, true);
                                let ws = self.current_write_set.remove(&tid).unwrap_or_default();
                                self.stats.aborts += 1;
                                self.verifier.tx_abort(tid);
                                self.deadlock.record_abort(btid, &ws);
                                return Err("aborted during write".into());
                            }
                            panic::resume_unwind(e);
                        }
                    }
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
        self.estimated_cycles = 0;
        self.pending_begins.clear();
    }
}

/// Compute the address range that covers all memory-access events in a trace.
/// Returns (base_address, region_size).  Falls back to the default 0x7f00 range
/// if the trace is empty or has no address events.
pub fn compute_address_range(events: &[Event]) -> (*mut libc::c_void, usize) {
    const DEFAULT_BASE: u64 = 0x7f00_0000_0000;
    const DEFAULT_SIZE: usize = 256 * 1024 * 1024;
    const TM_REGION_SIZE: usize = 256 * 1024 * 1024;

    let mut min_addr = u64::MAX;
    let mut max_addr = 0u64;

    for event in events {
        match &event.kind {
            EventKind::Read { addr, .. } | EventKind::Write { addr, .. } | EventKind::Alloc { addr, .. } | EventKind::Free { addr } => {
                if *addr < min_addr { min_addr = *addr; }
                if *addr > max_addr { max_addr = *addr; }
            }
            _ => {}
        }
    }

    if min_addr == u64::MAX || max_addr == 0 {
        return (DEFAULT_BASE as *mut libc::c_void, DEFAULT_SIZE);
    }

    // Round down to page boundary
    let page_size = 4096u64;
    let base = (min_addr / page_size) * page_size;

    // Compute how much we need: from base to max_addr, padded to the TM region size
    let range = (max_addr - base) as usize;
    let size = cmp::max(range + TM_REGION_SIZE / 4, TM_REGION_SIZE);
    // Round up to page boundary
    let size = ((size + page_size as usize - 1) / page_size as usize) * page_size as usize;

    (base as *mut libc::c_void, size)
}
