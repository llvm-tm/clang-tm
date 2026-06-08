# Explicit TM Instrumentation — Code Duplication Avoidance

## Problem
Data structures (RBTree, SortedList, HashMap) used inside transactions need TM
access (`tm_read`/`tm_write`).  Used outside transactions, they should use
plain loads/stores.  Without abstraction, you'd maintain two copies.

## Solution: `MemoryAccess<UseTM>`

The header `memory_access.hpp` provides a compile-time policy:

| `UseTM` | `load(ptr)` | `store(ptr, val)` |
|---------|-------------|-------------------|
| `true`  | `tm_read_*` | `tm_write_*`      |
| `false` | `*ptr`      | `*ptr = val`      |

Data structures are templated on `bool UseTM` and use `MemoryAccess<UseTM>::load/store`
for every shared-memory access:

```cpp
#include "explicit_rbtree.hpp"

// Inside a transaction:
auto* r = explicit_rbtree::find<true>(&cars, id);
long used = r ? MemoryAccess<true>::load(&r->num_used) : -1;

// During init (outside TX):
explicit_rbtree::insert<false>(&cars, new_node);
```

## Available Data Structures

| File | Description |
|------|-------------|
| `explicit_rbtree.hpp`     | Red-black tree: `lookup`, `find`, `contains`, `insert` |
| `explicit_sorted_list.hpp`| Sorted linked list: `insert`, `remove`, `contains`      |
| `explicit_hashmap.hpp`    | Open-addressing hash map: `find`, `insert`, `erase`     |

## Rust Equivalent

The `containers/src/memory_access.rs` crate defines a `MemAccess` trait with
`TmAccess` and `UntrackedAccess` implementors.  Data structures are generic
over `A: MemAccess` and use `A::load`/`A::store` for shared-memory access.
