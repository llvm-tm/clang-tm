pub mod stamp;

pub struct Rng(Mt19937_64);

impl Rng {
    pub fn new(seed: u64) -> Self {
        Self(Mt19937_64::new(seed))
    }
    pub fn next(&mut self) -> u64 {
        self.0.next()
    }
    pub fn uniform(&mut self) -> f64 {
        self.next() as f64 / u64::MAX as f64
    }
}

/// 64-bit Mersenne Twister (MT19937-64), matching C++ std::mt19937_64.
struct Mt19937_64 {
    mt: [u64; 312],
    mti: usize,
}

const N: usize = 312;
const M: usize = 156;
const MATRIX_A: u64 = 0xB5026F5AA96619E9;
const UPPER_MASK: u64 = 0xFFFFFFFF80000000;
const LOWER_MASK: u64 = 0x7FFFFFFF;

impl Mt19937_64 {
    fn new(seed: u64) -> Self {
        let mut mt = [0u64; N];
        mt[0] = seed;
        for i in 1..N {
            mt[i] = 6364136223846793005u64
                .wrapping_mul(mt[i - 1] ^ (mt[i - 1] >> 62))
                .wrapping_add(i as u64);
        }
        Self { mt, mti: N }
    }

    fn twist(&mut self) {
        for i in 0..N {
            let y = (self.mt[i] & UPPER_MASK) | (self.mt[(i + 1) % N] & LOWER_MASK);
            self.mt[i] = self.mt[(i + M) % N] ^ (y >> 1) ^ (if y & 1 == 1 { MATRIX_A } else { 0 });
        }
        self.mti = 0;
    }

    fn next(&mut self) -> u64 {
        if self.mti >= N {
            self.twist();
        }
        let mut x = self.mt[self.mti];
        self.mti += 1;

        x ^= (x >> 29) & 0x5555555555555555;
        x ^= (x << 17) & 0x3D6E8C0B60B7B9E6;
        x ^= (x << 37) & 0x0C17A4B04B702A6B;
        x ^= x >> 43;

        x
    }
}
