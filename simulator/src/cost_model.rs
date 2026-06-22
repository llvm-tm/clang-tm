// ── Cost Model: event-duration heuristics ──────────────────
// Maps each EventKind to a cycle cost using a MachineProfile,
// enabling the DES engine to estimate execution time.
//
// The cost model is backend-aware: different TM backends have
// different cost formulas for the same event kind.
//
// Two usage modes:
//   1. Generic: `event_cost(kind, &machine, backend)` — compute
//      per-event cost on the fly.
//   2. Calibrated: `CalibratedCostModel` — pre-computes costs
//      from a machine profile for fast lookup.

use crate::event::EventKind;
use crate::machine_profile::MachineProfile;

// ── Backend selection ──────────────────────────────────────

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default, serde::Serialize, serde::Deserialize)]
pub enum BackendProfile {
    #[default]
    Default,
    Tinystm,
    Norec,
    Tl2,
    Swisstm,
    Romulus,
    Tsxsgl,
    TsxSim,
}

impl BackendProfile {
    pub fn from_name(name: &str) -> Self {
        match name {
            "tinystm" => BackendProfile::Tinystm,
            "norec" => BackendProfile::Norec,
            "tl2" => BackendProfile::Tl2,
            "swisstm" => BackendProfile::Swisstm,
            "romulus" => BackendProfile::Romulus,
            "tsxsgl" => BackendProfile::Tsxsgl,
            "tsx-sim" | "tsx_sim" => BackendProfile::TsxSim,
            _ => BackendProfile::Default,
        }
    }

    pub fn machine_profile_name(&self) -> &'static str {
        match self {
            BackendProfile::Default => "default",
            BackendProfile::Tinystm => "tinystm",
            BackendProfile::Norec => "norec",
            BackendProfile::Tl2 => "tl2",
            BackendProfile::Swisstm => "swisstm",
            BackendProfile::Romulus => "romulus",
            BackendProfile::Tsxsgl => "tsxsgl",
            BackendProfile::TsxSim => "tsxsgl",
        }
    }
}

// ── Cycle costs ────────────────────────────────────────────

/// Generic (non-hardware-specific) overhead costs.
const ALLOC_COST: u64 = 50;
const FREE_COST: u64 = 30;
const THREAD_SPAWN_COST: u64 = 1000;
const THREAD_JOIN_COST: u64 = 500;
const CHECKPOINT_COST: u64 = 100;
const ASSERT_COST: u64 = 10;
const LOG_COST: u64 = 20;

/// Compute cycle cost for an event given a machine profile and backend.
pub fn event_cost(
    kind: &EventKind,
    machine: &MachineProfile,
    backend: BackendProfile,
) -> u64 {
    match backend {
        BackendProfile::Tsxsgl | BackendProfile::TsxSim => tsx_event_cost(kind, machine),
        _ => generic_event_cost(kind, machine, backend.machine_profile_name()),
    }
}

fn generic_event_cost(kind: &EventKind, machine: &MachineProfile, backend_name: &str) -> u64 {
    let bk = machine.backend(backend_name);
    match kind {
        EventKind::TxBegin => bk.begin_overhead as u64,
        EventKind::TxEnd => bk.commit_overhead as u64,
        EventKind::Abort { .. } => bk.abort_overhead as u64,
        EventKind::Read { .. } => (machine.tsx.read_l1_cycles + bk.read_overhead) as u64,
        EventKind::Write { .. } => (machine.tsx.write_l1_cycles + bk.write_overhead) as u64,
        EventKind::Alloc { .. } => ALLOC_COST,
        EventKind::Free { .. } => FREE_COST,
        EventKind::ThreadSpawn(_) => THREAD_SPAWN_COST,
        EventKind::ThreadJoin(_) => THREAD_JOIN_COST,
        EventKind::Checkpoint => CHECKPOINT_COST,
        EventKind::Assert { .. } => ASSERT_COST,
        EventKind::Log { .. } => LOG_COST,
        EventKind::Computation { cycles } => *cycles,
    }
}

fn tsx_event_cost(kind: &EventKind, machine: &MachineProfile) -> u64 {
    let bk = machine.backend("tsxsgl");
    match kind {
        EventKind::TxBegin => machine.tsx.xbegin_cycles as u64,
        EventKind::TxEnd => machine.tsx.xend_cycles as u64,
        EventKind::Abort { reason } => {
            let base = machine.tsx.xabort_cycles as u64;
            if *reason == 0xFF || *reason == 0x01 {
                // Explicit abort with spin (LOCK_BUSY, OWNER_CHANGED)
                base + 200
            } else {
                base
            }
        }
        EventKind::Read { .. } => {
            (machine.tsx.read_l1_cycles + machine.tsx.bloom_check_cycles + bk.read_overhead) as u64
        }
        EventKind::Write { .. } => {
            (machine.tsx.write_l1_cycles + machine.tsx.bloom_check_cycles + bk.write_overhead) as u64
        }
        EventKind::Alloc { .. } => ALLOC_COST,
        EventKind::Free { .. } => FREE_COST,
        EventKind::ThreadSpawn(_) => THREAD_SPAWN_COST,
        EventKind::ThreadJoin(_) => THREAD_JOIN_COST,
        EventKind::Checkpoint => CHECKPOINT_COST,
        EventKind::Assert { .. } => ASSERT_COST,
        EventKind::Log { .. } => LOG_COST,
        EventKind::Computation { cycles } => *cycles,
    }
}

// ── Calibrated cost model (pre-computed per-event costs) ───
// For fast dispatch during simulation.

#[derive(Debug, Clone)]
pub struct CalibratedCostModel {
    pub tx_begin_cost: u64,
    pub tx_end_cost: u64,
    pub abort_cost: u64,
    pub read_cost: u64,
    pub write_cost: u64,
    pub sgl_begin_cost: u64,
    pub sgl_end_cost: u64,
}

impl Default for CalibratedCostModel {
    fn default() -> Self {
        CalibratedCostModel {
            tx_begin_cost: 50,
            tx_end_cost: 40,
            abort_cost: 100,
            read_cost: 10,
            write_cost: 12,
            sgl_begin_cost: 100,
            sgl_end_cost: 100,
        }
    }
}

impl CalibratedCostModel {
    /// Build from a machine profile and backend.
    pub fn from_profile(machine: &MachineProfile, backend: BackendProfile) -> Self {
        CalibratedCostModel {
            tx_begin_cost: event_cost(&EventKind::TxBegin, machine, backend),
            tx_end_cost: event_cost(&EventKind::TxEnd, machine, backend),
            abort_cost: event_cost(&EventKind::Abort { reason: 0 }, machine, backend),
            read_cost: event_cost(&EventKind::Read { addr: 0, width: 8 }, machine, backend),
            write_cost: event_cost(&EventKind::Write { addr: 0, width: 8, val: 0 }, machine, backend),
            sgl_begin_cost: (machine.backend("tsxsgl").begin_overhead + machine.tsx.mutex_lock_cycles) as u64,
            sgl_end_cost: (machine.backend("tsxsgl").commit_overhead + machine.tsx.mutex_unlock_cycles) as u64,
        }
    }

    pub fn event_cost(&self, kind: &EventKind) -> u64 {
        match kind {
            EventKind::TxBegin => self.tx_begin_cost,
            EventKind::TxEnd => self.tx_end_cost,
            EventKind::Abort { .. } => self.abort_cost,
            EventKind::Read { .. } => self.read_cost,
            EventKind::Write { .. } => self.write_cost,
            EventKind::ThreadSpawn(_) => THREAD_SPAWN_COST,
            EventKind::ThreadJoin(_) => THREAD_JOIN_COST,
            EventKind::Alloc { .. } => ALLOC_COST,
            EventKind::Free { .. } => FREE_COST,
            EventKind::Checkpoint => CHECKPOINT_COST,
            EventKind::Assert { .. } => ASSERT_COST,
            EventKind::Log { .. } => LOG_COST,
            EventKind::Computation { cycles } => *cycles,
        }
    }
}
