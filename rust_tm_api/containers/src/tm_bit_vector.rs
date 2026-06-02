use std::sync::atomic::{AtomicU64, Ordering};

/// Compact bitset using atomic u64[] for individual bit operations.
/// Higher-level invariants (e.g., "only clear if all bits are set") must be
/// protected by the caller's TM transaction.
pub struct TMBitVector<const N: usize> {
    words: Vec<AtomicU64>,
}

impl<const N: usize> TMBitVector<N> {
    pub fn new() -> Self {
        let nwords = (N + 63) / 64;
        let mut words = Vec::with_capacity(nwords);
        for _ in 0..nwords {
            words.push(AtomicU64::new(0));
        }
        TMBitVector { words }
    }

    fn word_idx(bit: usize) -> (usize, u64) {
        (bit / 64, 1u64 << (bit % 64))
    }

    pub fn test(&self, bit: usize) -> bool {
        let (w, m) = Self::word_idx(bit);
        self.words[w].load(Ordering::Relaxed) & m != 0
    }

    pub fn set(&self, bit: usize) {
        let (w, m) = Self::word_idx(bit);
        self.words[w].fetch_or(m, Ordering::Relaxed);
    }

    pub fn clear(&self, bit: usize) {
        let (w, m) = Self::word_idx(bit);
        self.words[w].fetch_and(!m, Ordering::Relaxed);
    }

    pub fn any(&self) -> bool {
        self.words.iter().any(|w| w.load(Ordering::Relaxed) != 0)
    }

    pub fn all(&self) -> bool {
        let nwords = (N + 63) / 64;
        let last_bits = N % 64;
        for i in 0..nwords - 1 {
            if self.words[i].load(Ordering::Relaxed) != !0u64 {
                return false;
            }
        }
        let last_mask = if last_bits == 0 {
            !0u64
        } else {
            (1u64 << last_bits) - 1
        };
        self.words[nwords - 1].load(Ordering::Relaxed) == last_mask
    }

    pub fn count(&self) -> usize {
        self.words
            .iter()
            .map(|w| w.load(Ordering::Relaxed).count_ones() as usize)
            .sum()
    }
}
