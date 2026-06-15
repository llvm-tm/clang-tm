#pragma once

#ifndef TM
#define TM __attribute__((annotate("tm")))
#endif

#ifndef TX
#define TX __attribute__((annotate("shared"), noinline))
#endif

template<typename K, typename V>
struct TM RBNode {
    K key;
    V val;
    RBNode* left;
    RBNode* right;
    RBNode* parent;
    long color;

    static constexpr long RED = 0;
    static constexpr long BLACK = 1;
};

template<typename K, typename V>
struct TM RBTree {
    RBNode<K,V>* root;

    void init() {
        root = nullptr;
    }
};

template<typename K, typename V>
TX static inline RBNode<K,V>* rbtree_lookup(RBTree<K,V>* tree, K key) {
    RBNode<K,V>* n = tree->root;
    while (n) {
        if (key == n->key) return n;
        n = (key < n->key) ? n->left : n->right;
    }
    return nullptr;
}

template<typename K, typename V>
TX static inline V* rbtree_find(RBTree<K,V>* tree, K key) {
    RBNode<K,V>* n = rbtree_lookup(tree, key);
    return n ? &n->val : nullptr;
}

template<typename K, typename V>
static inline RBNode<K,V>* rbtree_insert(RBTree<K,V>* tree, RBNode<K,V>* node) {
    node->left = nullptr;
    node->right = nullptr;
    node->parent = nullptr;
    node->color = RBNode<K,V>::BLACK;

    K key = node->key;
    RBNode<K,V>* t = tree->root;

    if (!t) {
        tree->root = node;
        return nullptr;
    }

    while (true) {
        if (key == t->key) return t;
        if (key < t->key) {
            if (t->left) { t = t->left; continue; }
            node->parent = t;
            t->left = node;
            break;
        } else {
            if (t->right) { t = t->right; continue; }
            node->parent = t;
            t->right = node;
            break;
        }
    }

    node->color = RBNode<K,V>::RED;
    while (node != tree->root && node->parent->color == RBNode<K,V>::RED) {
        RBNode<K,V>* parent = node->parent;
        RBNode<K,V>* grandparent = parent->parent;
        if (!grandparent) break;

        if (parent == grandparent->left) {
            RBNode<K,V>* uncle = grandparent->right;
            if (uncle && uncle->color == RBNode<K,V>::RED) {
                parent->color = RBNode<K,V>::BLACK;
                uncle->color = RBNode<K,V>::BLACK;
                grandparent->color = RBNode<K,V>::RED;
                node = grandparent;
            } else {
                if (node == parent->right) {
                    node = parent;
                    RBNode<K,V>* r = node->right;
                    RBNode<K,V>* rl = r->left;
                    node->right = rl;
                    if (rl) rl->parent = node;
                    r->parent = node->parent;
                    if (!node->parent) tree->root = r;
                    else if (node->parent->left == node) node->parent->left = r;
                    else node->parent->right = r;
                    r->left = node;
                    node->parent = r;
                    parent = node->parent;
                }
                parent->color = RBNode<K,V>::BLACK;
                grandparent->color = RBNode<K,V>::RED;
                RBNode<K,V>* l = grandparent->left;
                RBNode<K,V>* lr = l->right;
                grandparent->left = lr;
                if (lr) lr->parent = grandparent;
                l->parent = grandparent->parent;
                if (!grandparent->parent) tree->root = l;
                else if (grandparent->parent->left == grandparent) grandparent->parent->left = l;
                else grandparent->parent->right = l;
                l->right = grandparent;
                grandparent->parent = l;
                node = tree->root;
                break;
            }
        } else {
            RBNode<K,V>* uncle = grandparent->left;
            if (uncle && uncle->color == RBNode<K,V>::RED) {
                parent->color = RBNode<K,V>::BLACK;
                uncle->color = RBNode<K,V>::BLACK;
                grandparent->color = RBNode<K,V>::RED;
                node = grandparent;
            } else {
                if (node == parent->left) {
                    node = parent;
                    RBNode<K,V>* l = node->left;
                    RBNode<K,V>* lr = l->right;
                    node->left = lr;
                    if (lr) lr->parent = node;
                    l->parent = node->parent;
                    if (!node->parent) tree->root = l;
                    else if (node->parent->right == node) node->parent->right = l;
                    else node->parent->left = l;
                    l->right = node;
                    node->parent = l;
                    parent = node->parent;
                }
                parent->color = RBNode<K,V>::BLACK;
                grandparent->color = RBNode<K,V>::RED;
                RBNode<K,V>* r = grandparent->right;
                RBNode<K,V>* rl = r->left;
                grandparent->right = rl;
                if (rl) rl->parent = grandparent;
                r->parent = grandparent->parent;
                if (!grandparent->parent) tree->root = r;
                else if (grandparent->parent->right == grandparent) grandparent->parent->right = r;
                else grandparent->parent->left = r;
                r->left = grandparent;
                grandparent->parent = r;
                node = tree->root;
                break;
            }
        }
    }

    tree->root->color = RBNode<K,V>::BLACK;
    return nullptr;
}

template<typename K, typename V>
TX static inline bool rbtree_contains(RBTree<K,V>* tree, K key) {
    RBNode<K,V>* n = rbtree_lookup(tree, key);
    return n != nullptr;
}
