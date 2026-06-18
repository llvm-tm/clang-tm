use serde::{Deserialize, Serialize};
use std::collections::HashMap;
use crate::event::Event;

/// Verifies TM correctness properties on a trace or during replay.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Checker {
    /// Per-thread transaction depth for nesting checks.
    pub tx_depth: HashMap<u32, u32>,

    /// Tracks total transactions.
    pub total_tx: u64,
    pub committed_tx: u64,
    pub aborted_tx: u64,

    /// Counters for read/write violations.
    pub out_of_tx_reads: u64,
    pub out_of_tx_writes: u64,

    /// Livelock suspicion threshold.
    pub livelock_threshold: u32,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub enum CheckerResult {
    Pass,
    Fail { reason: String, event: Event },
}

impl Checker {
    pub fn new(livelock_threshold: u32) -> Self {
        Checker {
            tx_depth: HashMap::new(),
            total_tx: 0,
            committed_tx: 0,
            aborted_tx: 0,
            out_of_tx_reads: 0,
            out_of_tx_writes: 0,
            livelock_threshold,
        }
    }

    pub fn check(&mut self, event: &Event) -> Result<(), String> {
        let depth = *self.tx_depth.get(&event.thread_id).unwrap_or(&0);

        match &event.kind {
            crate::event::EventKind::TxBegin => {
                *self.tx_depth.entry(event.thread_id).or_insert(0) += 1;
                if depth == 0 {
                    self.total_tx += 1;
                }
            }
            crate::event::EventKind::TxEnd => {
                let d = self.tx_depth.get_mut(&event.thread_id).ok_or_else(|| {
                    format!("TxEnd without TxBegin on thread {}", event.thread_id)
                })?;
                if *d == 0 {
                    return Err("TxEnd without TxBegin".into());
                }
                *d -= 1;
                if *d == 0 {
                    self.committed_tx += 1;
                }
            }
            crate::event::EventKind::Abort { .. } => {
                let d = self.tx_depth.get_mut(&event.thread_id).ok_or_else(|| {
                    format!("Abort without TxBegin on thread {}", event.thread_id)
                })?;
                if *d == 0 {
                    return Err("Abort without TxBegin".into());
                }
                *d -= 1;
                if *d == 0 {
                    self.aborted_tx += 1;
                }
            }
            crate::event::EventKind::Read { .. } => {
                if depth == 0 {
                    self.out_of_tx_reads += 1;
                }
            }
            crate::event::EventKind::Write { .. } => {
                if depth == 0 {
                    self.out_of_tx_writes += 1;
                }
            }
            _ => {}
        }
        Ok(())
    }

    pub fn finalize(&self) -> Vec<String> {
        let mut warnings = Vec::new();
        for (&tid, &d) in &self.tx_depth {
            if d > 0 {
                warnings.push(format!("Thread {} still in transaction (depth={}) at end of trace", tid, d));
            }
        }
        if self.out_of_tx_reads > 0 {
            warnings.push(format!("{} read(s) performed outside TX", self.out_of_tx_reads));
        }
        if self.out_of_tx_writes > 0 {
            warnings.push(format!("{} write(s) performed outside TX", self.out_of_tx_writes));
        }
        warnings
    }
}
