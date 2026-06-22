// ── Simulation engine: core DES loop with cost model ──────
// Processes events from the queue, advances the clock by
// accumulating estimated cycle costs per event, and checks
// TM correctness invariants.
//
// The clock can be driven in two modes:
//   MODE_TIMESTAMP: match the trace timestamp (original behavior)
//   MODE_COST: accumulate event costs from the machine profile
//
// In cost mode, cross-LP conflict detection is active: when two
// transactions overlap on the same address, one is aborted and a
// retry penalty is added. This makes throughput drop with thread
// count, matching real backends.

use serde::{Deserialize, Serialize};
use crate::event::{Event, EventKind};
use crate::queue::EventQueue;
use crate::lp::LpState;
use crate::memory::ShadowMemory;
use crate::checker::Checker;
use crate::cost_model::{BackendProfile, CalibratedCostModel};
use crate::machine_profile::MachineProfile;
use std::collections::HashMap;

/// Clock advancement mode.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub enum ClockMode {
    /// Use trace timestamps directly (original behavior).
    Timestamp,
    /// Accumulate estimated cycle costs per event.
    Cost,
}

/// Simulation engine state.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct SimState {
    pub clock: u64,
    pub queue: EventQueue,
    pub lps: HashMap<u32, LpState>,
    pub memory: ShadowMemory,
    pub checker: Checker,
    pub events_processed: u64,
    pub retire_watermark: u64,
    pub clock_mode: ClockMode,
    pub total_estimated_cycles: u64,
    pub total_tx_commits: u64,
    pub total_tx_aborts: u64,
    /// Machine hardware profile (defaults to Skylake estimates if not loaded).
    #[serde(default = "MachineProfile::skylake_default")]
    pub machine_profile: MachineProfile,
    /// Backend profile for cost model.
    #[serde(default)]
    pub backend_profile: BackendProfile,
    /// Pre-computed cost model for fast dispatch (rebuilt when profile changes).
    #[serde(skip)]
    pub calibrated: CalibratedCostModel,
    /// Cross-LP conflict tracking: addresses written by each in-flight LP.
    #[serde(default)]
    pub in_flight_writes: HashMap<u32, Vec<u64>>,
    /// Cross-LP conflict tracking: addresses read by each in-flight LP.
    #[serde(default)]
    pub in_flight_reads: HashMap<u32, Vec<u64>>,
    /// Multiplier applied to abort cost on synthetic conflicts (P3).
    /// Accounts for retry-loop overhead (re-executing TX body).
    #[serde(default = "default_retry_mult")]
    pub retry_cost_multiplier: u64,
    /// Aborts triggered by synthetic conflict detection (not from trace).
    #[serde(default)]
    pub conflict_aborts: u64,
    /// Effective CPU frequency in GHz (from profiling data or manual override).
    #[serde(default)]
    pub effective_freq_ghz: f64,
}

fn default_retry_mult() -> u64 { 3 }

impl SimState {
    pub fn new(threads: u32, tm_base: u64, tm_size: u64) -> Self {
        let mut lps = HashMap::new();
        for tid in 0..threads {
            lps.insert(tid, LpState::new(tid, 0));
        }
        let machine = MachineProfile::skylake_default();
        let backend = BackendProfile::Default;
        SimState {
            clock: 0,
            queue: EventQueue::new(),
            lps,
            memory: ShadowMemory::new(tm_base, tm_size),
            checker: Checker::new(1000),
            events_processed: 0,
            retire_watermark: 0,
            clock_mode: ClockMode::Timestamp,
            total_estimated_cycles: 0,
            total_tx_commits: 0,
            total_tx_aborts: 0,
            machine_profile: machine.clone(),
            backend_profile: backend,
            calibrated: CalibratedCostModel::from_profile(&machine, backend),
            in_flight_writes: HashMap::new(),
            in_flight_reads: HashMap::new(),
            retry_cost_multiplier: default_retry_mult(),
            conflict_aborts: 0,
            effective_freq_ghz: 0.0,
        }
    }

    pub fn set_clock_mode(&mut self, mode: ClockMode) {
        self.clock_mode = mode;
    }

    pub fn set_machine_profile(&mut self, profile: MachineProfile) {
        self.machine_profile = profile.clone();
        self.calibrated = CalibratedCostModel::from_profile(&profile, self.backend_profile);
    }

    pub fn set_backend_profile(&mut self, backend: BackendProfile) {
        self.backend_profile = backend;
        self.calibrated = CalibratedCostModel::from_profile(&self.machine_profile, backend);
    }

    pub fn load_events(&mut self, events: Vec<Event>) {
        for e in events {
            self.queue.push(e);
        }
    }

    fn get_event_cost(&self, kind: &EventKind) -> u64 {
        if self.clock_mode == ClockMode::Cost {
            self.calibrated.event_cost(kind)
        } else {
            0
        }
    }

    /// Advance the clock based on the event.  In Timestamp mode,
    /// the clock is set to the event's timestamp.  In Cost mode,
    /// the clock is incremented by the event's estimated cycle cost.
    fn advance_clock(&mut self, event: &Event) {
        match self.clock_mode {
            ClockMode::Timestamp => {
                self.clock = event.timestamp.max(self.clock);
            }
            ClockMode::Cost => {
                let cost = self.get_event_cost(&event.kind);
                self.clock = self.clock.saturating_add(cost);
                self.total_estimated_cycles = self.clock;
            }
        }
    }

    /// Track an address as read by an LP during its current TX.
    fn track_read(&mut self, lp_id: u32, addr: u64) {
        self.in_flight_reads.entry(lp_id).or_default().push(addr);
    }

    /// Track an address as written by an LP during its current TX.
    fn track_write(&mut self, lp_id: u32, addr: u64) {
        self.in_flight_writes.entry(lp_id).or_default().push(addr);
    }

    /// Remove all tracking for an LP (on commit or abort).
    fn untrack(&mut self, lp_id: u32) {
        self.in_flight_writes.remove(&lp_id);
        self.in_flight_reads.remove(&lp_id);
    }

    /// Check for Write-After-Read conflict: another LP is reading `addr`.
    fn check_raw_conflict(&self, lp_id: u32, addr: u64) -> Option<u32> {
        for (&other, addrs) in &self.in_flight_reads {
            if other != lp_id && addrs.contains(&addr) {
                return Some(other);
            }
        }
        None
    }

    /// Check for Read-After-Write conflict: another LP is writing `addr`.
    fn check_war_conflict(&self, lp_id: u32, addr: u64) -> Option<u32> {
        for (&other, addrs) in &self.in_flight_writes {
            if other != lp_id && addrs.contains(&addr) {
                return Some(other);
            }
        }
        None
    }

    /// Abort another LP's transaction (synthetic conflict).
    /// Returns the number of additional cycles charged (retry penalty).
    fn abort_lp(&mut self, lp_id: u32) -> u64 {
        let was_in_tx = self.lps.get(&lp_id).map(|lp| lp.in_tx).unwrap_or(false);
        if !was_in_tx {
            return 0;
        }
        if let Some(lp) = self.lps.get_mut(&lp_id) {
            lp.in_tx = false;
            lp.read_set.clear();
            lp.write_set.clear();
        }
        self.untrack(lp_id);
        self.conflict_aborts += 1;
        self.total_tx_aborts += 1;

        // Charge abort cost plus retry penalty (P3): the aborted
        // transaction's body will be re-executed on retry.
        if self.clock_mode == ClockMode::Cost {
            let base = self.calibrated.event_cost(&EventKind::Abort { reason: 0 });
            let penalty = base.saturating_mul(self.retry_cost_multiplier);
            self.clock = self.clock.saturating_add(penalty);
            self.total_estimated_cycles = self.clock;
            penalty
        } else {
            0
        }
    }

    /// Get whether an LP is in a transaction (without holding a borrow on self.lps).
    fn lp_in_tx(&self, tid: u32) -> bool {
        self.lps.get(&tid).map(|lp| lp.in_tx).unwrap_or(false)
    }

    fn dispatch(&mut self, event: &Event) {
        self.advance_clock(event);
        let tid = event.thread_id;

        if let Err(reason) = self.checker.check(event) {
            eprintln!("VIOLATION at ts={} tid={}: {}", event.timestamp, tid, reason);
        }

        // Handle conflict detection BEFORE touching lp (avoids borrow conflicts).
        let conflict_abort = match &event.kind {
            EventKind::Read { addr, .. } if self.lp_in_tx(tid) => {
                self.check_war_conflict(tid, *addr)
            }
            EventKind::Write { addr, .. } if self.lp_in_tx(tid) => {
                self.check_raw_conflict(tid, *addr)
            }
            _ => None,
        };
        if let Some(victim) = conflict_abort {
            self.abort_lp(victim);
        }

        let lp = self.lps.entry(tid)
            .or_insert_with(|| LpState::new(tid, 0));

        match &event.kind {
            EventKind::ThreadSpawn(child_id) => {
                let seed = event.timestamp.wrapping_add(*child_id as u64);
                self.lps.entry(*child_id).or_insert_with(|| LpState::new(*child_id, seed));
            }
            EventKind::TxBegin => {
                lp.in_tx = true;
                lp.tx_start_ts = event.timestamp;
                lp.retry_count = 0;
            }
            EventKind::TxEnd => {
                if lp.in_tx {
                    self.total_tx_commits += 1;
                }
                lp.in_tx = false;
                lp.read_set.clear();
                lp.write_set.clear();
                self.untrack(tid);
            }
            EventKind::Abort { .. } => {
                let was_in_tx = lp.in_tx;
                lp.in_tx = false;
                lp.read_set.clear();
                lp.write_set.clear();
                self.untrack(tid);
                if was_in_tx {
                    self.total_tx_aborts += 1;
                }
            }
            EventKind::Read { addr, .. } => {
                if lp.in_tx {
                    lp.read_set.push(crate::lp::ReadEntry { addr: *addr, version: 0 });
                    self.track_read(tid, *addr);
                }
            }
            EventKind::Write { addr, val, .. } => {
                if lp.in_tx {
                    lp.write_set.push(crate::lp::WriteEntry { addr: *addr, old_val: 0, new_val: *val });
                    self.track_write(tid, *addr);
                }
            }
            EventKind::Alloc { addr, size } => {
                self.memory.alloc(*addr, *size);
            }
            EventKind::Free { addr } => {
                if let Err(e) = self.memory.free(*addr) {
                    eprintln!("MEMORY VIOLATION at ts={}: {}", event.timestamp, e);
                }
            }
            EventKind::Checkpoint => {
                self.retire_watermark = event.timestamp;
            }
            EventKind::Assert { cond, msg } => {
                if !cond {
                    eprintln!("ASSERTION FAILED at ts={} tid={}: {}", event.timestamp, tid, msg);
                }
            }
            EventKind::Log { msg } => {
                eprintln!("[LOG ts={} tid={}] {}", event.timestamp, tid, msg);
            }
            EventKind::ThreadJoin(_) => {}
            EventKind::Computation { cycles } => {
                // In cost mode, cycles are accumulated; in timestamp mode, no-op.
                // The DES engine's clock advances on its own, so no action needed.
                let _ = cycles;
            }
        }
    }

    /// Process events until queue is empty or max_events limit reached.
    pub fn run(&mut self, max_events: u64) -> Vec<Event> {
        let mut processed = Vec::new();
        while let Some(event) = self.queue.pop() {
            if max_events > 0 && processed.len() >= max_events as usize {
                self.queue.push(event);
                break;
            }
            self.dispatch(&event);
            processed.push(event);
            self.events_processed += 1;
        }
        processed
    }

    /// Print execution summary.
    pub fn print_summary(&self) {
        eprintln!("═══ TM-DES SIMULATION ═══");
        eprintln!("  Events processed: {}", self.events_processed);
        eprintln!("  Clock mode: {:?}", self.clock_mode);
        eprintln!("  TX commits: {}  aborts: {} (conflict: {})",
            self.total_tx_commits, self.total_tx_aborts, self.conflict_aborts);
        if self.total_tx_commits + self.total_tx_aborts > 0 {
            let rate = 100.0 * self.total_tx_aborts as f64
                / (self.total_tx_commits + self.total_tx_aborts) as f64;
            eprintln!("  Abort rate: {:.1}%", rate);
        }
        if self.clock_mode == ClockMode::Cost {
            eprintln!("  Estimated cycles: {}", self.total_estimated_cycles);
            eprintln!("  Machine: {} @ {:.1} GHz", self.machine_profile.cpu, self.machine_profile.freq_ghz);
            eprintln!("  Backend profile: {:?}", self.backend_profile);
            let freq = if self.effective_freq_ghz > 0.0 { self.effective_freq_ghz } else { self.machine_profile.freq_ghz };
            if self.total_estimated_cycles > 0 && freq > 0.0 {
                let estimated_us = self.total_estimated_cycles as f64 / (freq * 1000.0);
                eprintln!("  Effective freq: {:.2} GHz  Estimated time: {:.2} us ({:.3} ms)",
                    freq, estimated_us, estimated_us / 1000.0);
            }
            eprintln!("  Retry cost multiplier: {}", self.retry_cost_multiplier);
        }
        eprintln!("═══");
    }
}
