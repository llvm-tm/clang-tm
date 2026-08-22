#pragma once

// tm_bloom_filter.hpp — fixed-size Bloom filter with O(1) disjointness tests.
//
// A Bloom filter represents a set of hashable keys in a fixed bit array.
// Two important properties make it the right tool for TM conflict detection:
//
//   1. NO FALSE NEGATIVES.  If key ∈ set, every membership probe hits a set
//      bit.  Consequently, if (A & B) == 0 over the whole bit array, then A
//      and B are *definitely* disjoint.  This means an empty intersection is
//      a sound proof of "no conflict" in a transactional validator.
//
//   2. CONSTANT-TIME DISJOINTNESS.  Whether ANY item of one set appears in
//      the other is answered by AND-ing the two fixed bit arrays: O(WORDS)
//      word operations, independent of how many elements each set holds.
//      (Exact intersection in O(1) is information-theoretically impossible;
//      the Bloom filter trades false positives for constant time.)
//
// A non-empty intersection is only a *maybe*: it may be a false positive, so
// callers must fall back to an exact check.  Because false negatives are
// impossible, a "disjoint" answer is always exact — safe to use as a fast
// path in NOrec-style validators.
//
// Thread-safety: this implementation uses std::atomic<uint64_t> words.
// insert() is a fetch_or (safe for a single writer); reads are relaxed
// loads.  Callers that need stronger ordering (e.g. publish the filter
// before a release store) must order via their own synchronisation —
// NOrec-BF relies on the global sequence lock for that.

#include <atomic>
#include <cstdint>
#include <cstddef>

namespace stm
{

// mix64: 64-bit finalizer (splitmix64 tail).  Deterministic, no
// architecture dependency, excellent avalanche.
inline uint64_t bloom_mix64(uint64_t z)
{
	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
	return z ^ (z >> 31);
}

// Fixed-size Bloom filter, WORDS × 64 bits, k = 2 independent hashes.
template <size_t WORDS = 16>
class BloomFilter
{
public:
	static constexpr size_t kWords = WORDS;
	static constexpr size_t kBits = WORDS * 64; // power of two

	BloomFilter() { clear(); }

	// Reset to empty.  Only safe when no other thread is reading the filter
	// concurrently (NOrec-BF rebuilds under the global commit lock).
	void clear()
	{
		for (size_t i = 0; i < WORDS; i++) words_[i].store(0, std::memory_order_relaxed);
	}

	// Insert a key (fetch_or; single-writer safe).
	void insert(uint64_t key)
	{
		uint64_t h1 = bloom_mix64(key);
		uint64_t h2 = bloom_mix64(key ^ 0x9e3779b97f4a7c15ULL);
		words_[bit_index(h1)].fetch_or(bit_mask(h1), std::memory_order_relaxed);
		words_[bit_index(h2)].fetch_or(bit_mask(h2), std::memory_order_relaxed);
	}

	// Membership probe (no false negatives).
	bool contains(uint64_t key) const
	{
		uint64_t h1 = bloom_mix64(key);
		uint64_t h2 = bloom_mix64(key ^ 0x9e3779b97f4a7c15ULL);
		if (!(words_[bit_index(h1)].load(std::memory_order_relaxed) & bit_mask(h1))) return false;
		if (!(words_[bit_index(h2)].load(std::memory_order_relaxed) & bit_mask(h2))) return false;
		return true;
	}

	// O(WORDS) disjointness test: does ANY element of *this appear in other?
	//   - returns true  → definitely disjoint (no false negatives)
	//   - returns false → maybe intersecting (fall back to an exact check)
	bool disjoint_with(const BloomFilter &other) const
	{
		for (size_t i = 0; i < WORDS; i++) {
			uint64_t a = words_[i].load(std::memory_order_relaxed);
			uint64_t b = other.words_[i].load(std::memory_order_relaxed);
			if (a & b) return false;
		}
		return true;
	}

	// Number of set bits (writers call this under the commit lock to decide
	// when to rebuild).
	size_t popcount() const
	{
		size_t n = 0;
		for (size_t i = 0; i < WORDS; i++) {
			uint64_t w = words_[i].load(std::memory_order_relaxed);
			n += (size_t)__builtin_popcountll(w);
		}
		return n;
	}

	bool empty() const
	{
		for (size_t i = 0; i < WORDS; i++) {
			if (words_[i].load(std::memory_order_relaxed)) return false;
		}
		return true;
	}

private:
	static inline size_t bit_index(uint64_t h) { return (h >> 6) & (WORDS - 1); }
	static inline uint64_t bit_mask(uint64_t h) { return 1ULL << (h & 63); }

	std::atomic<uint64_t> words_[WORDS];
};

} // namespace stm
