#pragma once

#include "tm_common.hpp"

namespace stm
{
namespace merge
{

// ── Alignment constants (8-byte slot = 64-bit word) ───────────
constexpr unsigned K_BITS_PER_BYTE_SHIFT = 3;   // log2(8)
constexpr uintptr_t K_ALIGN_MASK = sizeof(stm::word_t) - 1;

// ── Align a raw address down to 8-byte boundary ────────────────
inline void *align_down_8(void *addr)
{
	return reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(addr) & ~K_ALIGN_MASK);
}

// ── Compute byte-offset from aligned base ──────────────────────
inline unsigned byte_offset(void *raw_addr)
{
	return reinterpret_cast<uintptr_t>(raw_addr) & K_ALIGN_MASK;
}

// ── Write sub-word value into a bitmap entry ───────────────────
// Merges `val` (of type `sz`) at `raw_addr` into the entry's
// `value` field and sets the corresponding valid bits.
inline void bitmap_write(word_t &entry_value,
                         uint8_t &entry_valid,
                         const any_type_t &val,
                         ValueType sz,
                         void *raw_addr)
{
	unsigned nbytes = type_size(sz);
	unsigned shift = byte_offset(raw_addr) << K_BITS_PER_BYTE_SHIFT;
	uint8_t need = static_cast<uint8_t>(BYTE_MASK(nbytes) << shift);
	word_t write_word = any_to_u64(val, sz) & BYTE_MASK(nbytes);
	word_t clr_mask = static_cast<word_t>(BYTE_MASK(nbytes)) << shift;
	entry_value = (entry_value & ~clr_mask) | (write_word << shift);
	entry_valid |= need;
}

// ── Read typed value from a bitmap entry ───────────────────────
// Returns true if all required bytes are present in the valid
// bitmap; fills `out` with the extracted value.
inline bool bitmap_read(any_type_t &out,
                        word_t entry_value,
                        uint8_t entry_valid,
                        ValueType sz,
                        void *raw_addr)
{
	unsigned nbytes = type_size(sz);
	unsigned shift = byte_offset(raw_addr) << K_BITS_PER_BYTE_SHIFT;
	uint8_t need = static_cast<uint8_t>(BYTE_MASK(nbytes) << shift);
	if ((entry_valid & need) != need)
		return false;
	u64_to_any(out, (entry_value >> shift) & BYTE_MASK(nbytes), sz);
	return true;
}

// ── Initialize a bitmap write entry ───────────────────────────
// Sets addr, zeroes value/valid, then calls bitmap_write.
// Template works with any entry that has `addr`, `value`, `valid`.
template <typename E>
inline void init_write(E &entry,
                       void *aligned,
                       const any_type_t &val,
                       ValueType sz,
                       void *raw_addr)
{
	entry.addr = aligned;
	entry.value = 0;
	entry.valid = 0;
	bitmap_write(entry.value, entry.valid, val, sz, raw_addr);
}

// ── Initialize a bitmap read entry from a fresh memory read ───
template <typename E>
inline void init_read(E &entry,
                      void *aligned,
                      word_t version,
                      const any_type_t &val,
                      ValueType sz,
                      void *raw_addr)
{
	entry.addr = aligned;
	entry.observed_version = version;
	unsigned nbytes = type_size(sz);
	unsigned shift = byte_offset(raw_addr) << K_BITS_PER_BYTE_SHIFT;
	entry.valid = static_cast<uint8_t>(BYTE_MASK(nbytes) << shift);
	entry.value = any_to_u64(val, sz) << shift;
}

// ── Merge a new read into an existing bitmap read entry ───────
template <typename E>
inline void merge_read(E &entry,
                       word_t version,
                       const any_type_t &val,
                       ValueType sz,
                       void *raw_addr)
{
	entry.observed_version = version;
	unsigned nbytes = type_size(sz);
	unsigned shift = byte_offset(raw_addr) << K_BITS_PER_BYTE_SHIFT;
	uint8_t need = static_cast<uint8_t>(BYTE_MASK(nbytes) << shift);
	entry.valid |= need;
	uint64_t clr = static_cast<uint64_t>(BYTE_MASK(nbytes)) << shift;
	entry.value = (entry.value & ~clr) | (any_to_u64(val, sz) << shift);
}

// ── Factory: return a fully initialized read entry ─────────────
// Works with any BitmapReadEntry-based type.
template <typename E>
inline E make_read_entry(void *aligned,
                         word_t version,
                         const any_type_t &val,
                         ValueType sz,
                         void *raw_addr)
{
	E e;
	init_read(e, aligned, version, val, sz, raw_addr);
	return e;
}

// ── Factory: return a fully initialized write entry ─────────────
// For entries WITHOUT a .version field (e.g. NOrec).
template <typename E>
inline E make_write_entry(void *aligned,
                          const any_type_t &val,
                          ValueType sz,
                          void *raw_addr)
{
	E e;
	init_write(e, aligned, val, sz, raw_addr);
	return e;
}

// ── Factory: return a fully initialized write entry (with version)
// For TinySTM-style entries that have a .version field.
template <typename E>
inline E make_write_entry(void *aligned,
                          const any_type_t &val,
                          ValueType sz,
                          void *raw_addr,
                          word_t version)
{
	E e;
	init_write(e, aligned, val, sz, raw_addr);
	e.version = version;
	return e;
}

// ── Reconstruct wider value from byte entries (fallback) ───────
// Scans `nbytes` bytes starting at `addr`, calling `get_byte` for
// each.  If all found, fills `out_val` and returns true.
template <typename F>
inline bool read_merge_bytes(void *addr, unsigned nbytes, F &&get_byte, word_t &out_val)
{
	out_val = 0;
	for (unsigned i = 0; i < nbytes; i++) {
		void *byte_addr = reinterpret_cast<void *>((uintptr_t)addr + i);
		auto entry = get_byte(byte_addr);
		if (!entry)
			return false;
		out_val |= static_cast<word_t>(*entry) << (i << K_BITS_PER_BYTE_SHIFT);
	}
	return true;
}

// ══════════════════════════════════════════════════════════════
// Legacy type-based merge helpers (kept for SwissTM / TL2 compat)
// ══════════════════════════════════════════════════════════════

// Merge a narrower value into a wider word at a byte offset.
inline word_t merge_words(word_t base_val,
                          word_t write_val,
                          unsigned write_sz,
                          unsigned offset)
{
	word_t mask = static_cast<word_t>(BYTE_MASK(write_sz)) << (offset << K_BITS_PER_BYTE_SHIFT);
	return (base_val & ~mask) | ((write_val & BYTE_MASK(write_sz)) << (offset << K_BITS_PER_BYTE_SHIFT));
}

// Merge a same-address entry when types differ.
// If the existing entry is wider, the new value is merged in and
// true is returned.  Otherwise returns false (caller should create
// a new entry).
inline bool same_address(any_type_t &existing,
                         ValueType existing_type,
                         const any_type_t &new_val,
                         ValueType new_type)
{
	unsigned esz = type_size(existing_type);
	unsigned nsz = type_size(new_type);
	if (esz >= nsz) {
		word_t ew = any_to_u64(existing, existing_type);
		word_t nw = any_to_u64(new_val, new_type);
		ew = merge_words(ew, nw, nsz, 0);
		u64_to_any(existing, ew, existing_type);
		return true;
	}
	return false;
}

} // namespace merge
} // namespace stm
