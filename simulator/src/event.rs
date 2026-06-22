use serde::{Deserialize, Serialize};

/// A single event in the discrete event simulation.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Event {
    pub timestamp: u64,
    pub thread_id: u32,
    pub seq: u64,
    pub kind: EventKind,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub enum EventKind {
    ThreadSpawn(u32),              // (child_id)
    ThreadJoin(u32),               // (child_id)
    TxBegin,
    TxEnd,
    Abort {
        reason: u64,
    },
    Read {
        addr: u64,
        width: u8,
    },
    Write {
        addr: u64,
        width: u8,
        val: u64,
    },
    Alloc {
        addr: u64,
        size: u64,
    },
    Free {
        addr: u64,
    },
    /// Estimated cycles of non-TM computation (emitted by LLVM pass via --emit-tm-trace).
    /// The cycles value represents the sum of TargetTransformInfo::TCK_RecipThroughput
    /// costs for all non-TM instructions between TM events in the original function.
    Computation {
        cycles: u64,
    },
    Checkpoint,
    Assert {
        cond: bool,
        msg: String,
    },
    Log {
        msg: String,
    },
}

impl Event {
    pub fn new(ts: u64, tid: u32, seq: u64, kind: EventKind) -> Self {
        Event {
            timestamp: ts,
            thread_id: tid,
            seq,
            kind,
        }
    }

    pub fn is_tx_lifecycle(&self) -> bool {
        matches!(self.kind, EventKind::TxBegin | EventKind::TxEnd | EventKind::Abort { .. })
    }

    pub fn is_memory_op(&self) -> bool {
        matches!(
            self.kind,
            EventKind::Read { .. } | EventKind::Write { .. }
        )
    }

    pub fn is_tx_boundary(&self) -> bool {
        matches!(self.kind, EventKind::TxBegin | EventKind::TxEnd | EventKind::Abort { .. })
    }
}
