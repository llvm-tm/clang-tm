# STMbench7 LLVM IR Assessment Report

## Why STMbench7 Hangs With TinySTM/TL2 But Works With SingleGlobalLock

### 1. IR Growth

```
Uninstrumented: 21,708 lines
Instrumented:   24,650 lines (+13%)
```

### 2. Instrumentation Scale

| Metric | Count |
|--------|-------|
| TX-annotated functions | 38 |
| TM-annotated globals | 17 (`g_modules`, `g_connections`, `g_atomicParts`, etc.) |
| Static TM reads/writes | 86 |
| Distinct cloned functions | **114** |
| `_tm_clone` symbol occurrences | 425 |

### 3. Root Cause: Over-Cloning of STL Internals

The fixed-point propagation cloned **114 functions**, the majority of which are
STL internal methods that NEVER access TM data:

**Vector methods cloned (55):**
```
vector<T>::begin()        — returns iterator, no TM access
vector<T>::end()          — returns iterator, no TM access
vector<T>::size()         — returns size, no TM access
vector<T>::empty()        — returns bool, no TM access
vector<T>::make_iter()    — constructs iterator, no TM access
vector<T>::push_back()    — modifies vector internals (NOT TM-annotated)
vector<T>::emplace_back() — same
vector<T>::erase()        — same
vector<T>::operator[]     — returns reference to element
    ^- this one IS needed (accesses TM data through the returned reference)
```

**`std::map`/`__tree` internals cloned (18):**
```
__tree::__root_ptr()       — pointer arithmetic, no TM access
__tree::__end_node()       — pointer arithmetic, no TM access
__tree::__root()           — pointer arithmetic, no TM access
__tree::value_comp()       — comparison object, no TM access
__tree::__find_equal()     — tree traversal, compares keys (key is TM data)
__tree::find()             — tree lookup, reads keys (TM data)
__tree_iterator constructor — wraps pointer, no TM access
__tree_node::__get_value() — pointer offset, no TM access
```

**Iterator infrastructure cloned (12):**
```
__wrap_iter<T> constructors — wrap pointer, no TM access
__wrap_iter<T> copy/move   — wrap pointer, no TM access
```

**Other utility (29):**
```
pair constructors           — aggregate init, no TM access
pointer_traits::pointer_to — address-of, no TM access
less::operator()            — comparison, no TM access
__annotate_contiguous_container — debug annotation, no TM access
```

### 4. Propagation Trace

```
TX function op_st2_traverse
  └── for (auto &conn : g_connections)  ← range-for over TM vector
        ├── g_connections.begin()       ← cloned (vector::begin)
        │     └── __wrap_iter(ptr)      ← cloned (constructor)
        ├── g_connections.end()         ← cloned (vector::end)
        ├── conn.src → tm_read_i4       ← CORRECT instrumentation
        ├── conn.dst → tm_read_i4       ← CORRECT instrumentation
        ├── conn.type → tm_read_i4      ← CORRECT instrumentation
        └── ++iter                       ← cloned (operator++)
              └── __wrap_iter(ptr+1)    ← cloned (pointer arithmetic, NO TM)
```

The `this` pointer of `g_connections` is TM-traceable, so ALL methods of
`vector<Connection>` get cloned — even `begin()`, `end()`, `make_iter()`
which only compute pointers without reading TM data through them.

### 5. Dynamic Execution Cost

For a single long traversal over `g_connections` (300k elements, 4 fields):

```
Static instrumentations:       86 tm_read/tm_write
Dynamic per-traversal reads:   300k × 4 = 1.2M tm_read_i4 calls
Read-set entries created:       1.2M distinct addresses
Vector internal calls cloned:   begin/end/make_iter: 3 × 300k = 900k cloned calls
                                each doing hash lookups on write_set/read_set
                                for the VECTOR's internal pointers (not TM data!)
Total dynamic instrumented ops: ~2M+  (each with guard read + validation)
```

Each cloned `vector::begin()` call:
1. Enters the cloned function (branch)
2. Calls `write_set.find(this)` — hash lookup (this is a vector, not TM data)
3. Calls `read_set.find(this)` — another hash lookup
4. Returns the pointer with TM-read
5. The returned pointer is then used by `tm_read_i4(ptr)` for the actual field read

So each iteration does **unnecessary** TM instrumentation on the vector's
internal pointer (which is not TM data) BEFORE the actual field read.

### 6. The `extend()` / `validate()` Bottleneck

With a read-set of 1.2M entries:
- Each `validate()` iterates all 1.2M entries
- Each entry: hash table traversal + atomic load + decode + comparison
- With `std::unordered_map`, traversing 1.2M entries requires iterating
  through the entire bucket array + chained elements
- `extend()` calls `validate()` which is O(read_set_size)

If the transaction reads 1.2M addresses, then `version > tx->end_version`
triggers `extend()` during the read. This calls `validate()` on a read-set
that is partially built (say 600k entries). Next version check triggers it
again (now 800k entries). This O(N²) behavior is catastrophic.

### 7. Why SingleGlobalLock Works

SingleGlobalLock acquires ONE mutex in `tm_begin()` and releases in `tm_end()`.
The `tm_read_i4` and `tm_write_i4` implementations are DIRECT MEMORY ACCESSES
with no read-set, write-set, validation, or logging. There is zero per-access
overhead. Over-cloning of STL internal functions adds branches but the clones
just read/write directly through the global lock. No data structures are built.

### 8. Root Cause Summary

```
Problem: Over-cloning via fixed-point propagation
  └── 114 STL internal functions cloned (55 vector, 18 __tree, etc.)
        └── Each cloned call does TM instrumentation on non-TM data
              └── 1.2M+ unnecessary hash table operations
                    └── Multi-million entry read_set
                          └── validate() iterates millions of entries
                                └── O(N²) time per long transaction
                                      └── Effectively hangs
```

### 9. Proposed Fix

The cloning propagation should distinguish between:

1. **Data-access functions** — actually read/write TM globals through the
   TM-traceable pointer (should be cloned)
2. **Pointer-computation functions** — only compute addresses/offsets
   without dereferencing (should NOT be cloned)

Examples of functions that should NOT be cloned:
- `vector::begin()` / `vector::end()` — return iterators
- `__wrap_iter` constructors — wrap pointers  
- `__tree::__root_ptr()` — pointer arithmetic
- `pair` constructors — aggregate init
- `pointer_traits::pointer_to` — address-of operator

Examples that SHOULD be cloned:
- `vector::operator[]` — accesses element (TM data)
- Functions that LOAD/STORE through TM-traceable pointers

The heuristic: a function should only be cloned if it (or a function it
calls) actually dereferences a TM-traceable pointer to access memory.
Functions that only pass the pointer through (GEP, bitcast, return as-is)
without dereferencing should not propagate the TM-traceable status.
