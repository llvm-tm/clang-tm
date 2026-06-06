#pragma once

#include "tm_common.hpp"

namespace stm {

// ── Bitmap redo entry: write-set slot at aligned address ────────
// Stores up to 8 bytes with a valid-byte bitmap.
template <typename A = void *>
struct BitmapRedoEntry {
	A addr{};           // 8-byte aligned address
	word_t value{};     // full 8-byte value (unwritten bytes = 0)
	uint8_t valid{};    // bit i → byte i at addr+i was written
};

// ── Bitmap read entry: read-set slot at aligned address ─────────
template <typename A = void *>
struct BitmapReadEntry {
	A addr{};            // 8-byte aligned address
	word_t value{};      // full 8-byte observed value
	uint8_t valid{};     // bit i → byte i was read
	word_t observed_version = 0;
};

// ── Bitmap full entry: undo + redo at aligned address ───────────
template <typename A = void *>
struct BitmapFullEntry {
	A addr{};           // 8-byte aligned address
	word_t new_value{};
	word_t old_value{};
	uint8_t new_valid{};
	uint8_t old_valid{};
};

// ── Legacy type-based entries (kept for migration compat) ───────
template <typename A = void *>
struct RedoEntry {
	A addr{};
	any_type_t new_val{};
	ValueType type{};
};

template <typename A = void *>
struct UndoEntry {
	A addr{};
	any_type_t old_val{};
	ValueType type{};
};

template <typename A = void *>
struct FullEntry {
	A addr{};
	any_type_t new_val{};
	any_type_t old_val{};
	ValueType type{};
};

template <typename A = void *>
struct ReadEntry {
	A addr{};
	any_type_t observed_val{};
	ValueType type{};
	word_t observed_version = 0;
};

} // namespace stm
