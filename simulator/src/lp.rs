use serde::{Deserialize, Serialize};
use crate::rng::CheckpointableRng;

/// Logical process (thread) state.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct LpState {
    pub thread_id: u32,
    pub rng: CheckpointableRng,

    // TM state
    pub in_tx: bool,
    pub tx_start_ts: u64,
    pub read_set: Vec<ReadEntry>,
    pub write_set: Vec<WriteEntry>,
    pub retry_count: u32,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ReadEntry {
    pub addr: u64,
    pub version: u64,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct WriteEntry {
    pub addr: u64,
    pub old_val: u64,
    pub new_val: u64,
}

impl LpState {
    pub fn new(thread_id: u32, seed: u64) -> Self {
        let mut rng = CheckpointableRng::new(seed.wrapping_add(thread_id as u64));
        // Warm up
        for _ in 0..10 {
            rng.next_u64();
        }
        LpState {
            thread_id,
            rng,
            in_tx: false,
            tx_start_ts: 0,
            read_set: Vec::new(),
            write_set: Vec::new(),
            retry_count: 0,
        }
    }
}
