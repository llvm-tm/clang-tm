# LLVM IR Analysis: Recursive AVL Tree

## Structure

File: `benchmarks/datastructures/avltree_recursive.cpp`
Uninstrumented IR: `benchmarks/datastructures/out/avltree_recursive.ll` (5,609 lines)
Instrumented IR:   `benchmarks/datastructures/out/avltree_recursive.instr.ll` (6,467 lines)

Annotated entities from `llvm.global.annotations`:

| Entity | Annotation | Kind |
|--------|-----------|------|
| `node_keys`, `node_values`, `node_left`, `node_right` ... | `"tm"` | 10 TM globals |
| `node_count`, `root`, `free_head` | `"tm"` | 3 TM scalars |
| `txn_insert`, `txn_erase`, `txn_contains`, `txn_get`, `txn_rangeCount` | `"transaction"` | 5 TX functions |
| `worker` | `"thread"` | Thread entry |

## Cloning

The fixed-point propagation cloned **14 functions** with `_tm_clone` suffix:

```
_Z6insertiii_tm_clone         # recursive insert helper
_Z5eraseii_tm_clone            # recursive erase helper
_Z8containsii_tm_clone         # lookup
_Z3getiii_tm_clone             # get by index
_Z13getRangeCountiii_tm_clone  # range query
_Z11rotateRighti_tm_clone      # AVL rotation
_Z10rotateLefti_tm_clone       # AVL rotation
_ZL6heighti_tm_clone           # recursive height computation
_Z10getBalancei_tm_clone       # balance factor
_Z8freeNodei_tm_clone          # node deallocation
_Z7newNodeii_tm_clone          # node allocation
_Z12minValueNodei_tm_clone     # min-value for erase
_Z3maxii_tm_clone              # utility (traces to TM global via argument)
_Z4sizei_tm_clone              # node size accessor
```

**Total: 90 TM read/write calls** across all clones.

## Recursive Call-Graph Redirection

The plugin correctly redirects recursive calls:

### Original `insert`:
```
txn_insert (TX)
  └── insert (cloned to _tm_clone)
        ├── insert _tm_clone       ← recursive call redirected to clone
        ├── height _tm_clone       ← cloned
        ├── max _tm_clone          ← cloned  
        ├── getBalance _tm_clone   ← cloned
        ├── rotateRight _tm_clone  ← cloned
        └── rotateLeft _tm_clone   ← cloned
```

### Clone `insert_tm_clone`:
```
insert_tm_clone
  ├── node_left[root] → tm_read_i4     ← TM-read for traversal
  ├── node_right[root] → tm_read_i4    ← TM-read for traversal
  ├── node_keys[root] → tm_read_i4     ← TM-read for key comparison
  ├── insert_tm_clone(recursive)       ← redirected to itself
  ├── height_tm_clone(...)             ← redirected
  │     ├── node_left[n] → tm_read_i4  
  │     ├── node_right[n] → tm_read_i4 
  │     └── height_tm_clone(recursive) ← redirected
  ├── max_tm_clone(...)                ← redirected
  ├── getBalance_tm_clone(...)         ← redirected
  └── rotateRight_tm_clone(...)        ← redirected
        └── node_left[n] → tm_read_i4
```

### Clone `height_tm_clone`:
```
height_tm_clone(n)
  if n == -1: return 0
  // volatile uint8_t pad[1024] — stack padding preserved
  left_h  = height_tm_clone(node_left[n])   ← tm_read_i4 + recursive clone call
  right_h = height_tm_clone(node_right[n])  ← tm_read_i4 + recursive clone call
  return 1 + max_tm_clone(left_h, right_h)
```

## What Is Instrumented (Per-Access)

Within clones, every access to a TM-annotated global array is replaced:

| Original load | Instrumented call |
|--------------|-------------------|
| `node_left[n]` | `tm_read_i4(&node_left[n])` |
| `node_right[n]` | `tm_read_i4(&node_right[n])` |
| `node_height[n]` | `tm_read_i4(&node_height[n])` |
| `node_keys[n]` | `tm_read_i4(&node_keys[n])` |
| `node_values[n]` | `tm_write_i4(&node_values[n], val)` |

## Key Observations

1. **Recursion is fully preserved**: the clone calls itself recursively, and all calls within the recursive chain go through the cloned (instrumented) versions.

2. **Fixed-point propagation reaches deep**: `height` and `max` are cloned because they receive arguments that trace to TM globals (the node index from `insert`/`erase`). Even `max(int,int)` is cloned — its arguments come from `height` return values which trace through `insert`'s TM-traceable node indices.

3. **Per-access overhead is O(N) per operation**: Each recursive descent re-reads TM arrays at every node. For `height`, each node is visited O(2^depth) times in the worst case (naive recursion recomputes height for each subtree). The cloned instrumentation multiplies this by the TM read cost for each access.

4. **The 1024-byte stack padding (`pad[1024]`) is preserved**: The `volatile` array is kept in the clone, so the stack-frame-size stress test works as intended.

5. **No `tracesFromTMGlobal` needed**: Unlike STMbench7 (which uses `tracesFromTMGlobal` for iterator-based STL element access), the AVL tree uses direct array indexing (`node_left[n]`). The plugin detects TM globals directly via `getBaseObject`/`tracesToTMGlobal`, not through iterator tracing. This is both simpler and more predictable.

6. **Clone count vs tx_read count**: 14 clones × 90 TM calls = 6.4 calls/clone average, showing that most clones are small (few TM accesses) but many helpers need cloning due to argument propagation.

## Correctness Assessment

The instrumentation is correct for this benchmark:
- Recursive calls are properly redirected to instrumented clones
- TM reads/writes correctly wrap all accesses to TM-annotated arrays
- The thread entry/exit lifecycle is proper
- Transaction boundaries wrap each txn_* function as intended

No instrumentation anomalies were found.
