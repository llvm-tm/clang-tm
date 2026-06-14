use std::collections::{HashMap, HashSet};
use crate::backend::Backend;
use crate::event::Event;
use crate::verifier::Verifier;
use crate::sim_engine::ReplayStats;

/// Serializable snapshot of the simulation engine state.
#[derive(Debug, Clone, serde::Serialize, serde::Deserialize)]
pub struct Checkpoint {
    pub events_remaining: Vec<Event>,
    pub events_processed: u64,
    pub stats: ReplayStats,
    pub verifier: Verifier,
    pub in_tx: HashMap<u64, bool>,
    pub seen_threads: HashSet<u64>,
    pub base_tid: u64,
    /// Opaque bincode-serialized backend state (HashMap<tid, Option<TxState>>).
    pub backend_blob: Vec<u8>,
}

impl Checkpoint {
    pub fn save_to_file(&self, path: &str) -> Result<(), String> {
        let encoded = bincode::serialize(self)
            .map_err(|e| format!("serialize checkpoint: {}", e))?;
        std::fs::write(path, &encoded)
            .map_err(|e| format!("write checkpoint: {}", e))
    }

    pub fn load_from_file(path: &str) -> Result<Self, String> {
        let data = std::fs::read(path)
            .map_err(|e| format!("read checkpoint: {}", e))?;
        bincode::deserialize(&data)
            .map_err(|e| format!("deserialize checkpoint: {}", e))
    }
}

/// Snapshot the current engine state for checkpointing.
pub fn snapshot_engine(
    backend: &Backend,
    events_remaining: &[Event],
    events_processed: u64,
    stats: &ReplayStats,
    verifier: &Verifier,
    in_tx: &HashMap<u64, bool>,
    seen_threads: &HashSet<u64>,
    base_tid: u64,
) -> Checkpoint {
    let blob = backend.sim_snapshot_bytes();

    Checkpoint {
        events_remaining: events_remaining.to_vec(),
        events_processed,
        stats: stats.clone(),
        verifier: verifier.clone(),
        in_tx: in_tx.clone(),
        seen_threads: seen_threads.clone(),
        base_tid,
        backend_blob: blob,
    }
}

/// Restore engine state from a checkpoint.
pub fn restore_engine(
    cp: &Checkpoint,
    backend: &Backend,
) -> Result<
    (Vec<Event>, ReplayStats, Verifier, HashMap<u64, bool>, HashSet<u64>, u64),
    String,
> {
    backend.sim_restore_bytes(&cp.backend_blob)?;

    Ok((
        cp.events_remaining.clone(),
        cp.stats.clone(),
        cp.verifier.clone(),
        cp.in_tx.clone(),
        cp.seen_threads.clone(),
        cp.base_tid,
    ))
}
