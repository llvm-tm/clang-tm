#pragma once

#include "memory_access.hpp"
#include <functional>

// ── Policy-based open-addressing hash map (no TM/TX annotations) ──
// All shared-memory accesses go through MemoryAccess<UseTM>, so a single
// implementation works both inside and outside transactions.
//
// Uses linear probing.  Tombstone marker for deletion.

namespace explicit_hashmap {

enum class SlotState : uint8_t { EMPTY, OCCUPIED, TOMBSTONE };

template<typename K, typename V>
struct Pair { K key; V val; };

template<typename K, typename V>
struct Slot {
    SlotState state;
    Pair<K,V> data;
};

template<typename K, typename V>
struct Map {
    Slot<K,V>* slots = nullptr;
    size_t capacity = 0;
    size_t size = 0;

    static constexpr double MAX_LOAD = 0.6;
};

// ── find ───────────────────────────────────────────────────────────
template<bool UseTM, typename K, typename V, typename Hash = std::hash<K>>
V* find(Map<K,V>* map, const K& key, Hash hasher = Hash{}) {
    using MA = MemoryAccess<UseTM>;
    size_t cap = MA::load(&map->capacity);
    if (cap == 0) return nullptr;

    size_t idx = hasher(key) % cap;
    for (size_t i = 0; i < cap; i++) {
        size_t j = (idx + i) % cap;
        SlotState st = MA::load(&MA::load(&map->slots)[j].state);
        if (st == SlotState::EMPTY) return nullptr;
        if (st == SlotState::OCCUPIED) {
            K k = MA::load(&MA::load(&map->slots)[j].data.key);
            if (k == key)
                return &MA::load(&map->slots)[j].data.val;
        }
    }
    return nullptr;
}

// ── insert ─────────────────────────────────────────────────────────
template<bool UseTM, typename K, typename V, typename Hash = std::hash<K>>
bool insert(Map<K,V>* map, const K& key, const V& val, Hash hasher = Hash{}) {
    using MA = MemoryAccess<UseTM>;
    size_t cap = MA::load(&map->capacity);
    if (cap == 0) return false;

    size_t idx = hasher(key) % cap;
    size_t first_tomb = (size_t)-1;

    for (size_t i = 0; i < cap; i++) {
        size_t j = (idx + i) % cap;
        SlotState st = MA::load(&MA::load(&map->slots)[j].state);
        if (st == SlotState::EMPTY) {
            size_t ins = (first_tomb != (size_t)-1) ? first_tomb : j;
            MA::store(&MA::load(&map->slots)[ins].state, SlotState::OCCUPIED);
            MA::store(&MA::load(&map->slots)[ins].data.key, key);
            MA::store(&MA::load(&map->slots)[ins].data.val, val);
            MA::store(&map->size, MA::load(&map->size) + 1);
            return true;
        }
        if (st == SlotState::OCCUPIED) {
            K k = MA::load(&MA::load(&map->slots)[j].data.key);
            if (k == key) {
                MA::store(&MA::load(&map->slots)[j].data.val, val);
                return false; // updated existing
            }
        } else { // TOMBSTONE
            if (first_tomb == (size_t)-1)
                first_tomb = j;
        }
    }
    return false; // full
}

// ── erase ──────────────────────────────────────────────────────────
template<bool UseTM, typename K, typename V, typename Hash = std::hash<K>>
bool erase(Map<K,V>* map, const K& key, Hash hasher = Hash{}) {
    using MA = MemoryAccess<UseTM>;
    size_t cap = MA::load(&map->capacity);
    if (cap == 0) return false;

    size_t idx = hasher(key) % cap;
    for (size_t i = 0; i < cap; i++) {
        size_t j = (idx + i) % cap;
        SlotState st = MA::load(&MA::load(&map->slots)[j].state);
        if (st == SlotState::EMPTY) return false;
        if (st == SlotState::OCCUPIED) {
            K k = MA::load(&MA::load(&map->slots)[j].data.key);
            if (k == key) {
                MA::store(&MA::load(&map->slots)[j].state, SlotState::TOMBSTONE);
                MA::store(&map->size, MA::load(&map->size) - 1);
                return true;
            }
        }
    }
    return false;
}

} // namespace explicit_hashmap
