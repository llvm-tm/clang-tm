// Shared library for all benchmarks — provides common utilities, RNG, STAMP modules.

pub mod stamp;

// Simple RNG used across benchmarks
pub struct Rng(u64);

impl Rng {
    pub fn new(seed: u64) -> Self {
        Self(seed.wrapping_mul(6364136223846793005).wrapping_add(1442695040888963407))
    }
    pub fn next(&mut self) -> u64 {
        self.0 = self.0.wrapping_mul(6364136223846793005).wrapping_add(1442695040888963407);
        self.0 >> 33
    }
    pub fn range(&mut self, lo: u64, hi: u64) -> f64 {
        lo as f64 + (self.next() % (hi - lo)) as f64
    }
}
