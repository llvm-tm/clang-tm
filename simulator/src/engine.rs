// ── Simulation engine: core DES loop with cost model ──────
// Processes events from the queue, advances the clock by
// accumulating estimated cycle costs per event, and checks
// TM correctness invariants.
//
// The clock can be driven in two modes:
//   MODE_TIMESTAMP: match the trace timestamp (original behavior)
//   MODE_COST: accumulate event costs from the machine profile
//
// The cost mode enables "what-if" analysis: how fast would this
// workload run on different hardware or with a different backend?

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
}

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

    fn dispatch(&mut self, event: &Event) {
        self.advance_clock(event);
        let lp = self.lps.entry(event.thread_id)
            .or_insert_with(|| LpState::new(event.thread_id, 0));

        if let Err(reason) = self.checker.check(event) {
            eprintln!("VIOLATION at ts={} tid={}: {}", event.timestamp, event.thread_id, reason);
        }

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
                lp.in_tx = false;
                lp.read_set.clear();
                lp.write_set.clear();
                self.total_tx_commits += 1;
            }
            EventKind::Abort { .. } => {
                lp.in_tx = false;
                lp.read_set.clear();
                lp.write_set.clear();
                self.total_tx_aborts += 1;
            }
            EventKind::Read { addr, .. } => {
                if lp.in_tx {
                    lp.read_set.push(crate::lp::ReadEntry { addr: *addr, version: 0 });
                }
            }
            EventKind::Write { addr, val, .. } => {
                if lp.in_tx {
                    lp.write_set.push(crate::lp::WriteEntry { addr: *addr, old_val: 0, new_val: *val });
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
                    eprintln!("ASSERTION FAILED at ts={} tid={}: {}", event.timestamp, event.thread_id, msg);
                }
            }
            EventKind::Log { msg } => {
                eprintln!("[LOG ts={} tid={}] {}", event.timestamp, event.thread_id, msg);
            }
            EventKind::ThreadJoin(_) => {}
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
        eprintln!("── SIMULATION SUMMARY ──");
        eprintln!("  Events processed: {}", self.events_processed);
        eprintln!("  Final clock: {}", self.clock);
        eprintln!("  Clock mode: {:?}", self.clock_mode);
        eprintln!("  TX commits: {}", self.total_tx_commits);
        eprintln!("  TX aborts: {}", self.total_tx_aborts);
        if self.total_tx_commits + self.total_tx_aborts > 0 {
            let rate = 100.0 * self.total_tx_aborts as f64
                / (self.total_tx_commits + self.total_tx_aborts) as f64;
            eprintln!("  Abort rate: {:.1}%", rate);
        }
        if self.clock_mode == ClockMode::Cost {
            eprintln!("  Estimated cycles: {}", self.total_estimated_cycles);
            eprintln!("  Machine: {} @ {:.1} GHz", self.machine_profile.cpu, self.machine_profile.freq_ghz);
            eprintln!("  Backend profile: {:?}", self.backend_profile);
            if self.total_estimated_cycles > 0 && self.machine_profile.freq_ghz > 0.0 {
                let estimated_us = self.total_estimated_cycles as f64 / (self.machine_profile.freq_ghz * 1000.0);
                eprintln!("  Estimated time: {:.2} us ({:.3} ms)", estimated_us, estimated_us / 1000.0);
            }
        }
    }
}
