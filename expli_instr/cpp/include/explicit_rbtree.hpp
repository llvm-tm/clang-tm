#pragma once

#include "memory_access.hpp"

// ── Policy-based red-black tree (no TM/TX annotations) ──────────────
// All shared-memory loads/stores go through MemoryAccess<UseTM>::load/store,
// so a single implementation works both inside and outside transactions.
//
// The tree and its nodes are plain C++ structs (no `struct TM`).  They
// must be allocated in TM-tracked memory (via tm_malloc/tm_calloc) when
// UseTM=true is used inside a transaction.
//
// Example usage inside a transaction:
//
//   expli::TM<>::transaction([&]{
//       auto* r = explicit_rbtree::find<true>(&cars, id);
//       long used = r ? MemoryAccess<true>::load(&r->num_used) : -1;
//   });
//
// Same code outside a transaction (during init, for example):
//
//   explicit_rbtree::insert<false>(&cars, new_node);

namespace explicit_rbtree {

template<typename K, typename V>
struct Node {
    K key;
    V val;
    Node* left;
    Node* right;
    Node* parent;
    long color;

    static constexpr long RED   = 0;
    static constexpr long BLACK = 1;
};

template<typename K, typename V>
struct Tree {
    Node<K,V>* root = nullptr;
};

// ── lookup ──────────────────────────────────────────────────────────
template<bool UseTM, typename K, typename V>
Node<K,V>* lookup(Tree<K,V>* tree, K key) {
    using MA = MemoryAccess<UseTM>;
    Node<K,V>* n = MA::load(&tree->root);
    while (n) {
        K nk = MA::load(&n->key);
        if (key == nk) return n;
        n = (key < nk) ? MA::load(&n->left) : MA::load(&n->right);
    }
    return nullptr;
}

// ── find ────────────────────────────────────────────────────────────
template<bool UseTM, typename K, typename V>
V* find(Tree<K,V>* tree, K key) {
    Node<K,V>* n = lookup<UseTM>(tree, key);
    return n ? &n->val : nullptr;
}

// ── contains ────────────────────────────────────────────────────────
template<bool UseTM, typename K, typename V>
bool contains(Tree<K,V>* tree, K key) {
    return lookup<UseTM>(tree, key) != nullptr;
}

// ── insert ──────────────────────────────────────────────────────────
// Returns nullptr on success, or pointer to existing node with same key.
// Only UseTM=false is needed during init; UseTM=true is provided for
// completeness (e.g., inserting inside a transaction).
template<bool UseTM, typename K, typename V>
Node<K,V>* insert(Tree<K,V>* tree, Node<K,V>* node) {
    using MA = MemoryAccess<UseTM>;

    MA::store(&node->left,   nullptr);
    MA::store(&node->right,  nullptr);
    MA::store(&node->parent, nullptr);
    MA::store(&node->color,  Node<K,V>::BLACK);

    K key = node->key;
    Node<K,V>* t = MA::load(&tree->root);

    if (!t) {
        MA::store(&tree->root, node);
        return nullptr;
    }

    while (true) {
        K tk = MA::load(&t->key);
        if (key == tk) return t;
        if (key < tk) {
            Node<K,V>* left = MA::load(&t->left);
            if (left) { t = left; continue; }
            MA::store(&node->parent, t);
            MA::store(&t->left, node);
            break;
        } else {
            Node<K,V>* right = MA::load(&t->right);
            if (right) { t = right; continue; }
            MA::store(&node->parent, t);
            MA::store(&t->right, node);
            break;
        }
    }

    MA::store(&node->color, Node<K,V>::RED);
    while (MA::load(&node) != MA::load(&tree->root) &&
           MA::load(&MA::load(&node->parent)->color) == Node<K,V>::RED)
    {
        Node<K,V>* parent   = MA::load(&node->parent);
        Node<K,V>* gp       = MA::load(&parent->parent);
        if (!gp) break;

        Node<K,V>* left_gp  = MA::load(&gp->left);
        Node<K,V>* right_gp = MA::load(&gp->right);

        if (MA::load(&parent) == left_gp) {
            Node<K,V>* uncle = right_gp;
            if (uncle && MA::load(&uncle->color) == Node<K,V>::RED) {
                MA::store(&parent->color, Node<K,V>::BLACK);
                MA::store(&uncle->color,  Node<K,V>::BLACK);
                MA::store(&gp->color,     Node<K,V>::RED);
                node = gp;
            } else {
                if (MA::load(&node) == MA::load(&parent->right)) {
                    node = parent;
                    Node<K,V>* r = MA::load(&node->right);
                    Node<K,V>* rl = MA::load(&r->left);
                    MA::store(&node->right, rl);
                    if (rl) MA::store(&rl->parent, node);
                    MA::store(&r->parent, MA::load(&node->parent));
                    if (!MA::load(&node->parent))
                        MA::store(&tree->root, r);
                    else if (MA::load(&MA::load(&node->parent)->left) == node)
                        MA::store(&MA::load(&node->parent)->left, r);
                    else
                        MA::store(&MA::load(&node->parent)->right, r);
                    MA::store(&r->left, node);
                    MA::store(&node->parent, r);
                    parent = MA::load(&node->parent);
                }
                MA::store(&parent->color, Node<K,V>::BLACK);
                MA::store(&gp->color,     Node<K,V>::RED);
                Node<K,V>* l  = MA::load(&gp->left);
                Node<K,V>* lr = MA::load(&l->right);
                MA::store(&gp->left, lr);
                if (lr) MA::store(&lr->parent, gp);
                MA::store(&l->parent, MA::load(&gp->parent));
                if (!MA::load(&gp->parent))
                    MA::store(&tree->root, l);
                else if (MA::load(&MA::load(&gp->parent)->left) == gp)
                    MA::store(&MA::load(&gp->parent)->left, l);
                else
                    MA::store(&MA::load(&gp->parent)->right, l);
                MA::store(&l->right, gp);
                MA::store(&gp->parent, l);
                node = MA::load(&tree->root);
                break;
            }
        } else {
            Node<K,V>* uncle = left_gp;
            if (uncle && MA::load(&uncle->color) == Node<K,V>::RED) {
                MA::store(&parent->color, Node<K,V>::BLACK);
                MA::store(&uncle->color,  Node<K,V>::BLACK);
                MA::store(&gp->color,     Node<K,V>::RED);
                node = gp;
            } else {
                if (MA::load(&node) == MA::load(&parent->left)) {
                    node = parent;
                    Node<K,V>* l  = MA::load(&node->left);
                    Node<K,V>* lr = MA::load(&l->right);
                    MA::store(&node->left, lr);
                    if (lr) MA::store(&lr->parent, node);
                    MA::store(&l->parent, MA::load(&node->parent));
                    if (!MA::load(&node->parent))
                        MA::store(&tree->root, l);
                    else if (MA::load(&MA::load(&node->parent)->right) == node)
                        MA::store(&MA::load(&node->parent)->right, l);
                    else
                        MA::store(&MA::load(&node->parent)->left,  l);
                    MA::store(&l->right, node);
                    MA::store(&node->parent, l);
                    parent = MA::load(&node->parent);
                }
                MA::store(&parent->color, Node<K,V>::BLACK);
                MA::store(&gp->color,     Node<K,V>::RED);
                Node<K,V>* r  = MA::load(&gp->right);
                Node<K,V>* rl = MA::load(&r->left);
                MA::store(&gp->right, rl);
                if (rl) MA::store(&rl->parent, gp);
                MA::store(&r->parent, MA::load(&gp->parent));
                if (!MA::load(&gp->parent))
                    MA::store(&tree->root, r);
                else if (MA::load(&MA::load(&gp->parent)->right) == gp)
                    MA::store(&MA::load(&gp->parent)->right, r);
                else
                    MA::store(&MA::load(&gp->parent)->left,  r);
                MA::store(&r->left, gp);
                MA::store(&gp->parent, r);
                node = MA::load(&tree->root);
                break;
            }
        }
    }
    MA::store(&MA::load(&tree->root)->color, Node<K,V>::BLACK);
    return nullptr;
}

} // namespace explicit_rbtree
