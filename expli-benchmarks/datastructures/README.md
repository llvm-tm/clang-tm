# Expli Datastructures Benchmarks

TM-stressing data structure benchmarks using the explicit C++ API.

Planned benchmarks:
- list.cpp        — sorted linked list (pointer chase, high abort rate)
- hashmap.cpp     — open-addressing hash table (bucket contention)
- treap.cpp       — randomized BST (balanced, pointer-heavy)
- rbtree.cpp      — red-black tree (balanced BST)
- skip_list.cpp   — skip list (randomized, concurrent)
- heap.cpp        — priority queue (heap property)
- queue.cpp       — FIFO queue (multiple producers/consumers)
- btree.cpp       — B-tree (wide nodes, range queries)

Each uses `tm_api.hpp` template API with `tm_begin`/`tm_commit`.
