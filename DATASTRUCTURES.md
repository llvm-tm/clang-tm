# TM-Safe Container Reference — Complexity & Performance

---

## Family 1: Plugin-path containers (`backends/tm_impl/common/`)

Used inside `[[tm::shared]]` functions (`__attribute__((annotate("shared")))`) compiled with the LLVM TM pass. Allocation uses `::operator new` (redirected by the pass to `tm_malloc` inside transactions). See `tests/plugin/test_types.cpp` for a minimal example.

```
#include  ──────┐
tests/plugin/   │  benchmarks/plugin/
  test_types.cpp │   bank/bank.cpp
  test_treap_tx  │   datastructures/*.cpp
                 │   STAMP/*.cpp
                 └─── all use these
```

**Source files:**
- `backends/tm_impl/common/tm_vector.hpp` — `TMSafeVector<T>`
- `backends/tm_impl/common/tm_hash_set.hpp` — `TMSafeHashSet<T>`
- `backends/tm_impl/common/tm_safe_map.hpp` — `TMSafeMap<K,V>`, `TMSafeMultiMap<K,V>`

### 1.1 TMSafeVector<T> (`tm_vector.hpp`)

**Replaces:** `std::vector<T>`

| Operation | TMSafeVector | std::vector | Note |
|-----------|-------------|-------------|------|
| `operator[]` | O(1) | O(1) | Same (raw `T*`) |
| `push_back` | O(1) amortized | O(1) amortized | Both 1.5×–2× growth |
| `pop_back` | O(1) | O(1) | |
| `erase(pos)` | O(n) | O(n) | Both shift-based |
| `resize(n)` | O(n) | O(n) | |
| `begin/end` | O(1) | O(1) | Both raw `T*` |

**Perf vs vector (push_back):** 1.7–1.9× slower due to element-wise copy+destroy in grow (no memcpy). Identity of iterators same.

### 1.2 TMSafeHashSet<T> (`tm_hash_set.hpp`)

**Replaces:** `std::unordered_set<T>`

| Operation | TMSafeHashSet | std::unordered_set |
|-----------|--------------|-------------------|
| `insert` | O(1) avg, O(n) worst | O(1) avg, O(n) worst |
| `contains` | O(1) avg | O(1) avg |
| `erase` | O(1) avg (backward-shift) | O(1) avg (node free) |
| `clear` | O(n) | O(n) |
| Iteration | O(capacity) skips empty | O(buckets + nodes) |

| Operation | Measured perf vs unordered_set | Conditions |
|-----------|-------------------------------|-----------|
| Insert (shuffled) | 1.09× **faster** | N=500k, load factor ~0.5 |
| Insert (sorted) | **0.47× slower** | Clustering from linear probing |
| Erase | **5× faster** | Backward-shift vs node deallocation |
| Insert + contains×50 | **2× faster** | Cache-friendly open addressing |
| Contains hit/miss | ~1.0× (identical) | Same cache miss rate |

**Memory:** Contiguous slot array (power of 2). No per-node heap allocations. ~3× less memory than `std::unordered_set` (no bucket array, no node structs).

**Key design:** Open addressing, linear probing, backward-shift deletion (no tombstones), load factor 0.75, power-of-2 capacity. Iteration skips empty slots.

### 1.3 TMSafeMap<K,V> (`tm_safe_map.hpp`)

**Replaces:** `std::map<K,V>` (tree) / `std::unordered_map<K,V>` (hash)

| Operation | TMSafeMap | std::map | std::unordered_map |
|-----------|----------|---------|-------------------|
| `find` | O(1) avg | O(log n) | O(1) avg |
| `operator[]` | O(1) avg | O(log n) | O(1) avg |
| `insert` | O(1) avg | O(log n) | O(1) avg |
| `erase` | O(1) avg (backward-shift) | O(log n) (rebalance) | O(1) avg (tombstone) |
| Iteration | O(capacity) | O(n) (in-order) | O(buckets + nodes) |

**Same hash-table strategy as TMSafeHashSet** — open addressing, linear probing, backward-shift deletion. `MapIter` supports `first`/`second`, `->`, `++`, cross-type `==`/`!=` for const/non-const comparison.

### 1.4 TMSafeMultiMap<K,V> (`tm_safe_map.hpp`)

**Replaces:** `std::multimap<K,V>`

| Operation | TMSafeMultiMap | std::multimap |
|-----------|---------------|---------------|
| `insert` | O(n) (sorted shift) | O(log n) |
| `find_first` | O(n) (linear scan) | O(log n) |
| `lower_bound` | O(log n) (binary search) | O(log n) |
| Iteration | O(n) (contiguous) | O(n) (in-order) |

**Flat sorted array** (vector of pairs). O(n) insert is acceptable for small maps. `lower_bound` uses binary search.

---

## Family 2: Expli-path containers (`explicit_api/cpp/include/`)

Allocation uses `tm_malloc`/`tm_free` directly. Built for the explicit instrumentation API.

**Source files:**
- `explicit_api/cpp/include/tm_api.hpp` — `expli::vector<T>`
- `explicit_api/cpp/include/tm_map.hpp` — `expli::flat_set<K>`, `expli::flat_map<K,V>`, `expli::flat_multimap<K,V>`
- `tests/expli-api/test_tx.cpp` — unit tests using these containers

### 2.1 expli::vector<T> (`tm_api.hpp`, namespace `expli`)

**Replaces:** `std::vector<T>`

Same design as TMSafeVector. 2× growth factor. Raw `T*` iterators.

| Operation | Complexity | Note |
|-----------|-----------|------|
| `operator[]` | O(1) | Raw pointer |
| `push_back` | O(1) amortized | 2× growth |
| `erase(pos)` | O(n) | Shift-based |
| `resize(n)` | O(n) | Default-construct |
| `begin/end` | O(1) | Raw `T*` |

Missing: `resize(n, val)` overload, `insert(pos)`.

### 2.2 expli::flat_set<K> (`tm_map.hpp`)

**Replaces:** `std::set<K>` / `std::unordered_set<K>`

| Operation | flat_set | std::set | std::unordered_set |
|-----------|---------|----------|-------------------|
| `insert` | **O(n)** | O(log n) | O(1) avg |
| `contains` | **O(log n)** | O(log n) | O(1) avg |
| `erase` | **O(n)** | O(log n) | O(1) avg |
| `begin/end` | O(1) | O(1) | O(1) |
| Iterate | O(n) | O(n) | O(capacity) |

**Performance vs set (measured at N=100k):**
- Contains hit: **2.7× faster** (contiguous memory binary search)
- Contains miss: **1.5× faster** (early exit on binary search)
- Insert: **16× slower** (O(n) shift vs O(log n) tree insert)
- Iterate: **13× faster** (contiguous vs pointer-chasing)

**Flat sorted array** — sorted insertion with element shift, binary search for lookups. Best for small sets (≤100 items) that are read more often than written.

### 2.3 expli::flat_map<K,V> (`tm_map.hpp`)

**Replaces:** `std::map<K,V>`

Same strategy as flat_set — sorted array of pairs. Uses `expli::pair<K,V>` instead of `std::pair`.

| Operation | flat_map | std::map |
|-----------|---------|----------|
| `find` | O(log n) | O(log n) |
| `insert` | O(n) | O(log n) |
| `operator[]` | O(n) (if absent) | O(log n) |
| `erase` | O(n) | O(log n) |

Returns `V*` from `find()` (nullptr if absent), not iterator. Copy operations deleted (move-only).

### 2.4 expli::flat_multimap<K,V> (`tm_map.hpp`)

**Replaces:** `std::multimap<K,V>`

Sorted array allowing duplicate keys.

| Operation | flat_multimap | std::multimap |
|-----------|--------------|---------------|
| `insert` | O(n) | O(log n) |
| `find_first` | O(n) linear scan | O(log n) |
| `lower_bound` | O(log n) | O(log n) |
| Iteration | O(n) contiguous | O(n) tree walk |

Has `Iter` struct with `->`, `*`, `++`, `!=`. Supports range-for. `lower_bound` returns `Iter`.

---

## Family 3: Scratch containers (`benchmarks/cpp/include/`)

Used inside expli benchmarks for local scratch data. NOT TM-tracked — regular `::operator new`/`::operator delete`. TM safety comes from explicit `tm_read_i8`/`tm_write_i8` calls on shared data.

**Source files:**
- `benchmarks/cpp/include/scratch_set.hpp` — `ScratchVector<T>`, `ScratchSet<T>`

### 3.1 ScratchVector<T> (`scratch_set.hpp`)

**Replaces:** `std::vector<T>` (local/scratch)

| Operation | ScratchVector | std::vector | Note |
|-----------|--------------|-------------|------|
| `operator[]` | O(1) | O(1) | Both raw `T*` |
| `push_back` | O(1) amortized | O(1) amortized | 1.5× growth |
| `resize(n,val)` | O(n) | O(n) | Has overload |
| `begin/end` | O(1) | O(1) | Raw `T*` |

**Perf vs vector (push_back):** 1.7–1.9× slower due to element-wise copy in grow (no `memcpy`/`realloc`). This is intentional — element-wise copy is visible to the TM pass.

### 3.2 ScratchSet<T> (`scratch_set.hpp`)

**Replaces:** `std::set<T>` (local/scratch)

| Operation | ScratchSet | std::set |
|-----------|-----------|----------|
| `contains` | **O(log n)** binary search | O(log n) tree walk |
| `insert` | **O(n)** sorted shift | O(log n) |
| `erase` | **O(n)** shift | O(log n) |
| Iteration | O(n) contiguous | O(n) in-order |

**Perf vs set (N=100k):**
- Contains hit: **2.7× faster** (cache-friendly binary search vs pointer-chasing tree)
- Insert: **16× slower** (O(n) shift vs O(log n) tree)
- Iterate: **13× faster** (contiguous array)

Acceptable for small sets ≤100 elements where lookup speed matters more than insert speed.

---

## Cross-Family Comparison

| Container | Memory per entry | Allocation strategy | TM-safe | Best for |
|-----------|-----------------|-------------------|---------|----------|
| TMSafeVector | 1 ptr (contiguous) | `::operator new` (redirected) | Yes | Dynamic arrays in plugin TX |
| TMSafeHashSet | 1 slot (contiguous) | `::operator new` (redirected) | Yes | Unique-set membership in plugin TX |
| TMSafeMap | 1 slot (key+val) | `::operator new` (redirected) | Yes | Key-value store in plugin TX |
| TMSafeMultiMap | 1 pair (contiguous) | `::operator new` (redirected) | Yes | Small multimap in plugin TX |
| expli::vector | 1 ptr (contiguous) | `tm_malloc`/`tm_free` | Yes | Dynamic arrays in expli TX |
| expli::flat_set | 1 element (contiguous) | `tm_malloc`/`tm_free` | Yes | Small read-heavy sets |
| expli::flat_map | 1 pair (contiguous) | `tm_malloc`/`tm_free` | Yes | Small key-value in expli TX |
| expli::flat_multimap | 1 pair (contiguous) | `tm_malloc`/`tm_free` | Yes | Small multimap in expli TX |
| ScratchVector | 1 ptr (contiguous) | `::operator new`/`::operator delete` | No | Local scratch arrays |
| ScratchSet | 1 element (contiguous) | `::operator new`/`::operator delete` | No | Local scratch membership |

---

## Benchmarked performance (summary)

| Test | Container vs STL | Result |
|------|-----------------|--------|
| Insert shuffled (500k) | TMSafeHashSet vs unordered_set | **1.09× faster** |
| Insert sorted (500k) | TMSafeHashSet vs unordered_set | **0.47× slower** (clustering) |
| Erase (500k) | TMSafeHashSet vs unordered_set | **5× faster** |
| Insert+contains×50 (500k) | TMSafeHashSet vs unordered_set | **2× faster** |
| Contains hit/miss (500k) | TMSafeHashSet vs unordered_set | ~1.0× (identical) |
| push_back | ScratchVector vs vector | **1.7–1.9× slower** |
| Contains hit (100k) | ScratchSet vs set | **2.7× faster** |
| Contains miss (100k) | ScratchSet vs set | **1.5× faster** |
| Insert (100k) | ScratchSet vs set | **16× slower** |
| Iterate (100k) | ScratchSet vs set | **13× faster** |
