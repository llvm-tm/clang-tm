// Treap-based map/multimap — proven randomized BST (Aragon & Seidel, 1989).
// Expected O(log n) for all operations.  No opaque libstdc++ calls.
// Every load/store is inline and visible to the TM plugin.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>

// ── Map (unique keys) ─────────────────────────────────────────

template<typename K, typename V>
class TMTreapMap {
    struct Node {
        std::pair<const K, V> data;
        int    prio;
        Node * left;
        Node * right;
        Node * parent;

        static int prio_from_key(const K &k) {
            uint64_t h = (uint64_t)std::hash<K>{}(k);
            h ^= h >> 33;
            h *= 0xFF51AFD7ED558CCDULL;
            h ^= h >> 33;
            h *= 0xC4CEB9FE1A85EC53ULL;
            h ^= h >> 33;
            return (int)(h & 0x7FFFFFFF);
        }

        Node(const K &k, const V &v)
            : data(k, v), prio(prio_from_key(k)),
              left(nullptr), right(nullptr), parent(nullptr) {}
    };

    Node * m_root;
    size_t m_size;

    static void set_parent(Node *n, Node *p) {
        if (n) n->parent = p;
    }

    // Split tree t by key k: left = keys < k, right = keys >= k
    static void split(Node *t, const K &k, Node *&l, Node *&r) {
        if (!t) { l = r = nullptr; return; }
        if (t->data.first < k) {
            split(t->right, k, t->right, r);
            l = t;
            set_parent(l->right, l);
            set_parent(r, nullptr);
        } else {
            split(t->left, k, l, t->left);
            r = t;
            set_parent(r->left, r);
            set_parent(l, nullptr);
        }
    }

    // Merge two treaps where all keys in l < all keys in r
    static Node *merge(Node *l, Node *r) {
        if (!l) { set_parent(r, nullptr); return r; }
        if (!r) { set_parent(l, nullptr); return l; }
        if (l->prio > r->prio) {
            l->right = merge(l->right, r);
            set_parent(l->right, l);
            set_parent(l, nullptr);
            return l;
        } else {
            r->left = merge(l, r->left);
            set_parent(r->left, r);
            set_parent(r, nullptr);
            return r;
        }
    }

    Node *find_node(const K &k) const {
        Node *x = m_root;
        while (x) {
            if (k < x->data.first)       x = x->left;
            else if (x->data.first < k)  x = x->right;
            else                         return x;
        }
        return nullptr;
    }

    void clear_subtree(Node *n) {
        if (!n) return;
        clear_subtree(n->left);
        clear_subtree(n->right);
        delete n;
    }

public:
    using value_type = std::pair<const K, V>;

    class Iterator {
        friend class TMTreapMap;
        Node * m_node;
        explicit Iterator(Node *n) : m_node(n) {}
    public:
        using value_type = std::pair<const K, V>;
        using reference  = std::pair<const K, V> &;
        using pointer    = std::pair<const K, V> *;

        reference operator*()  const { return m_node->data; }
        pointer   operator->() const { return &m_node->data; }

        Iterator &operator++() {
            if (m_node->right) {
                m_node = m_node->right;
                while (m_node->left) m_node = m_node->left;
            } else {
                while (m_node->parent && m_node == m_node->parent->right)
                    m_node = m_node->parent;
                m_node = m_node->parent;
            }
            return *this;
        }

        bool operator==(const Iterator &o) const { return m_node == o.m_node; }
        bool operator!=(const Iterator &o) const { return m_node != o.m_node; }
    };

    TMTreapMap() : m_root(nullptr), m_size(0) {}
    ~TMTreapMap() { clear(); }

    TMTreapMap(const TMTreapMap &) = delete;
    TMTreapMap &operator=(const TMTreapMap &) = delete;

    void clear() {
        clear_subtree(m_root);
        m_root = nullptr;
        m_size = 0;
    }

    Iterator begin() const {
        if (!m_root) return end();
        Node *x = m_root;
        while (x->left) x = x->left;
        return Iterator(x);
    }

    Iterator end() const { return Iterator(nullptr); }

    Iterator find(const K &k) const {
        Node *n = find_node(k);
        return Iterator(n);
    }

    V &operator[](const K &k) {
        Node *ex = find_node(k);
        if (ex) return ex->data.second;

        auto *z = new Node(k, V{});
        Node *l, *r;
        split(m_root, k, l, r);
        m_root = merge(merge(l, z), r);
        m_size++;
        return z->data.second;
    }

    static Node *erase_node(Node *t, const K &k) {
        if (!t) return nullptr;
        if (k < t->data.first) {
            t->left = erase_node(t->left, k);
            set_parent(t->left, t);
        } else if (t->data.first < k) {
            t->right = erase_node(t->right, k);
            set_parent(t->right, t);
        } else {
            Node *n = merge(t->left, t->right);
            delete t;
            return n;
        }
        return t;
    }

    size_t erase(const K &k) {
        if (!find_node(k)) return 0;
        m_root = erase_node(m_root, k);
        m_size--;
        return 1;
    }

    size_t size() const { return m_size; }
    bool empty() const { return m_size == 0; }
};

// ── MultiMap (duplicate keys allowed) ─────────────────────────

template<typename K, typename V>
class TMTreapMultiMap {
    struct Node {
        std::pair<const K, V> data;
        int    prio;
        Node * left;
        Node * right;
        Node * parent;

        static int prio_from_key(const K &k) {
            uint64_t h = (uint64_t)std::hash<K>{}(k);
            h ^= h >> 33;
            h *= 0xFF51AFD7ED558CCDULL;
            h ^= h >> 33;
            h *= 0xC4CEB9FE1A85EC53ULL;
            h ^= h >> 33;
            return (int)(h & 0x7FFFFFFF);
        }

        Node(const K &k, const V &v)
            : data(k, v), prio(prio_from_key(k)),
              left(nullptr), right(nullptr), parent(nullptr) {}
    };

    Node * m_root;
    size_t m_size;

    static void set_parent(Node *n, Node *p) {
        if (n) n->parent = p;
    }

    static void split(Node *t, const K &k, Node *&l, Node *&r) {
        if (!t) { l = r = nullptr; return; }
        if (t->data.first < k) {
            split(t->right, k, t->right, r);
            l = t;
            set_parent(l->right, l);
            set_parent(r, nullptr);
        } else {
            split(t->left, k, l, t->left);
            r = t;
            set_parent(r->left, r);
            set_parent(l, nullptr);
        }
    }

    static Node *merge(Node *l, Node *r) {
        if (!l) { set_parent(r, nullptr); return r; }
        if (!r) { set_parent(l, nullptr); return l; }
        if (l->prio > r->prio) {
            l->right = merge(l->right, r);
            set_parent(l->right, l);
            set_parent(l, nullptr);
            return l;
        } else {
            r->left = merge(l, r->left);
            set_parent(r->left, r);
            set_parent(r, nullptr);
            return r;
        }
    }



    void clear_subtree(Node *n) {
        if (!n) return;
        clear_subtree(n->left);
        clear_subtree(n->right);
        delete n;
    }

public:
    using value_type = std::pair<const K, V>;

    class Iterator {
        friend class TMTreapMultiMap;
        Node * m_node;
        explicit Iterator(Node *n) : m_node(n) {}
    public:
        using value_type = std::pair<const K, V>;
        using reference  = std::pair<const K, V> &;
        using pointer    = std::pair<const K, V> *;

        reference operator*()  const { return m_node->data; }
        pointer   operator->() const { return &m_node->data; }

        Iterator &operator++() {
            if (m_node->right) {
                m_node = m_node->right;
                while (m_node->left) m_node = m_node->left;
            } else {
                while (m_node->parent && m_node == m_node->parent->right)
                    m_node = m_node->parent;
                m_node = m_node->parent;
            }
            return *this;
        }

        bool operator==(const Iterator &o) const { return m_node == o.m_node; }
        bool operator!=(const Iterator &o) const { return m_node != o.m_node; }
    };

    TMTreapMultiMap() : m_root(nullptr), m_size(0) {}
    ~TMTreapMultiMap() { clear(); }

    TMTreapMultiMap(const TMTreapMultiMap &) = delete;
    TMTreapMultiMap &operator=(const TMTreapMultiMap &) = delete;

    void clear() {
        clear_subtree(m_root);
        m_root = nullptr;
        m_size = 0;
    }

    Iterator begin() const {
        if (!m_root) return end();
        Node *x = m_root;
        while (x->left) x = x->left;
        return Iterator(x);
    }

    Iterator end() const { return Iterator(nullptr); }

    Iterator lower_bound(const K &k) const {
        Node *x = m_root;
        Node *ans = nullptr;
        while (x) {
            if (!(x->data.first < k)) {
                ans = x;
                x = x->left;
            } else {
                x = x->right;
            }
        }
        return Iterator(ans);
    }

    void insert(const std::pair<K, V> &p) {
        auto *z = new Node(p.first, p.second);
        Node *l, *r;
        split(m_root, p.first, l, r);   // l: keys < p.first, r: keys >= p.first
        m_root = merge(merge(l, z), r);
        m_size++;
    }

    size_t size() const { return m_size; }
    bool empty() const { return m_size == 0; }
};
