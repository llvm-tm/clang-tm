use serde::{Deserialize, Serialize};

/// Simple xorshift64* PRNG. Fully serializable — just stores one u64.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct CheckpointableRng {
    state: u64,
    counter: u64,
}

impl CheckpointableRng {
    pub fn new(seed: u64) -> Self {
        // Avoid zero state (would lock up)
        let state = if seed == 0 { 1 } else { seed };
        CheckpointableRng { state, counter: 0 }
    }

    pub fn next_u64(&mut self) -> u64 {
        self.counter += 1;
        // xorshift64*
        let mut x = self.state;
        x ^= x >> 12;
        x ^= x << 25;
        x ^= x >> 27;
        self.state = x;
        x.wrapping_mul(0x2545_f491_4f6c_dd1d)
    }

    pub fn next_u32(&mut self) -> u32 {
        self.next_u64() as u32
    }

    pub fn range_u64(&mut self, lo: u64, hi: u64) -> u64 {
        if lo >= hi {
            return lo;
        }
        lo + self.next_u64() % (hi - lo)
    }

    pub fn counter(&self) -> u64 {
        self.counter
    }
}
