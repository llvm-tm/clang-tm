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
        matches!(self.kind, EventKind::TxBegin | EventKind::TxEnd)
    }

    pub fn is_memory_op(&self) -> bool {
        matches!(
            self.kind,
            EventKind::Read { .. } | EventKind::Write { .. }
        )
    }

    pub fn is_tx_boundary(&self) -> bool {
        matches!(self.kind, EventKind::TxBegin | EventKind::TxEnd)
    }
}
