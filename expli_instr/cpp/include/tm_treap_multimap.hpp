#pragma once

#include <atomic>
#include "tm_api.hpp"

namespace expli {

// ── TM-aware treap multimap (no mutexes, pure TM sync) ──────
//
// All node fields are accessed through TM reads/writes when inside
// a transaction (tm_nested_call_counter > 0), and through direct
// memory access during single-threaded initialization.
//
// Expected O(log n) for insert / lower_bound.  Duplicate keys allowed.
template<typename K, typename V>
class tm_treap_multimap {
    struct Node {
        K     key;
        V     val;
        int   prio;
        Node *left;
        Node *right;
        Node *parent;

        Node(const K &k, const V &v, int p)
            : key(k), val(v), prio(p),
              left(nullptr), right(nullptr), parent(nullptr) {}
    };

    Node  *m_root;
    size_t m_size;

    static bool in_tx() { return tm_nested_call_counter > 0; }

    // ── TM-aware field accessors ───────────────────────────

    static K rd_key(Node *n) {
        if (in_tx()) return TM<K>::read_at(&n->key);
        return n->key;
    }
    static void wr_key(Node *n, const K &v) {
        if (in_tx()) TM<K>::write_at(&n->key, v);
        else n->key = v;
    }

    static V rd_val(Node *n) {
        if (in_tx()) return TM<V>::read_at(&n->val);
        return n->val;
    }
    static void wr_val(Node *n, const V &v) {
        if (in_tx()) TM<V>::write_at(&n->val, v);
        else n->val = v;
    }

    static int rd_prio(Node *n) {
        if (in_tx()) return TM<int>::read_at(&n->prio);
        return n->prio;
    }

    static Node *rd_left(Node *n) {
        if (in_tx())
            return static_cast<Node *>(tm_read_ptr(reinterpret_cast<void **>(&n->left)));
        return n->left;
    }
    static void wr_left(Node *n, Node *v) {
        if (in_tx()) tm_write_ptr(reinterpret_cast<void **>(&n->left), v);
        else n->left = v;
    }

    static Node *rd_right(Node *n) {
        if (in_tx())
            return static_cast<Node *>(tm_read_ptr(reinterpret_cast<void **>(&n->right)));
        return n->right;
    }
    static void wr_right(Node *n, Node *v) {
        if (in_tx()) tm_write_ptr(reinterpret_cast<void **>(&n->right), v);
        else n->right = v;
    }

    static Node *rd_parent(Node *n) {
        if (in_tx())
            return static_cast<Node *>(tm_read_ptr(reinterpret_cast<void **>(&n->parent)));
        return n->parent;
    }
    static void wr_parent(Node *n, Node *v) {
        if (in_tx()) tm_write_ptr(reinterpret_cast<void **>(&n->parent), v);
        else n->parent = v;
    }

    // ── Priority generation ────────────────────────────────

    static int random_prio() {
        static std::atomic<uint64_t> s_counter{0};
        uint64_t v = s_counter.fetch_add(1, std::memory_order_relaxed);
        v ^= v >> 33;
        v *= 0xFF51AFD7ED558CCDULL;
        v ^= v >> 33;
        v *= 0xC4CEB9FE1A85EC53ULL;
        v ^= v >> 33;
        return static_cast<int>(v & 0x7FFFFFFF);
    }

    // ── Tree helpers ───────────────────────────────────────

    static Node *min_node(Node *x) {
        if (!x) return nullptr;
        while (rd_left(x)) x = rd_left(x);
        return x;
    }

    // ── Copy/clear helpers ─────────────────────────────────

    void clear_subtree(Node *n) {
        if (!n) return;
        Node *x = n;
        while (x) {
            if (rd_left(x)) {
                x = rd_left(x);
            } else if (rd_right(x)) {
                x = rd_right(x);
            } else {
                Node *p = rd_parent(x);
                if (p) {
                    if (rd_left(p) == x) wr_left(p, nullptr);
                    else                 wr_right(p, nullptr);
                }
                tm_free(x);
                x = p;
            }
        }
    }

public:
    using value_type = std::pair<K, V>;

    class Iterator {
        friend class tm_treap_multimap;
        Node *m_node;
        explicit Iterator(Node *n) : m_node(n) {}
    public:
        using value_type = expli::pair<K, V>;
        using reference  = expli::pair<K, V> &;
        using pointer    = expli::pair<K, V> *;

        // These return fresh copies (not references) because we may
        // be reading through TM — no stable address exists.
        value_type operator*()  const { return {rd_key(m_node), rd_val(m_node)}; }

        struct arrow_proxy {
            value_type kv;
            const value_type *operator->() const noexcept { return &kv; }
        };
        arrow_proxy operator->() const { return arrow_proxy{operator*()}; }

        K first()  const { return rd_key(m_node); }
        V second() const { return rd_val(m_node); }

        Iterator &operator++() {
            Node *x = m_node;
            if (rd_right(x)) {
                x = rd_right(x);
                while (rd_left(x)) x = rd_left(x);
            } else {
                while (rd_parent(x) && x == rd_right(rd_parent(x)))
                    x = rd_parent(x);
                x = rd_parent(x);
            }
            m_node = x;
            return *this;
        }
        bool operator==(const Iterator &o) const { return m_node == o.m_node; }
        bool operator!=(const Iterator &o) const { return m_node != o.m_node; }
    };

    tm_treap_multimap() : m_root(nullptr), m_size(0) {}
    ~tm_treap_multimap() { clear(); }
    tm_treap_multimap(const tm_treap_multimap &) = delete;
    tm_treap_multimap &operator=(const tm_treap_multimap &) = delete;

    void clear() {
        clear_subtree(m_root);
        m_root = nullptr;
        m_size = 0;
    }

    Iterator begin() const {
        return Iterator(min_node(m_root));
    }

    Iterator end() const { return Iterator(nullptr); }

    Iterator lower_bound(const K &k) const {
        Node *x = m_root;
        Node *ans = nullptr;
        while (x) {
            if (!(rd_key(x) < k)) {
                ans = x;
                x = rd_left(x);
            } else {
                x = rd_right(x);
            }
        }
        return Iterator(ans);
    }

    void insert(const K &k, const V &v) {
        auto *z = static_cast<Node *>(tm_malloc(sizeof(Node)));
        new (z) Node(k, v, random_prio());

        if (!m_root) {
            m_root = z;
            m_size = 1;
            return;
        }

        // BST insert as leaf
        Node *x = m_root, *y = nullptr;
        while (x) {
            y = x;
            if (k < rd_key(x))
                x = rd_left(x);
            else
                x = rd_right(x);
        }
        wr_parent(z, y);
        if (k < rd_key(y))
            wr_left(y, z);
        else
            wr_right(y, z);

        // Rotate up by priority
        while (rd_parent(z) && rd_prio(z) > rd_prio(rd_parent(z))) {
            Node *p = rd_parent(z);
            Node *g = rd_parent(p);
            if (rd_left(p) == z) {
                // Right rotation at p
                wr_left(p, rd_right(z));
                if (rd_right(z)) wr_parent(rd_right(z), p);
                wr_right(z, p);
            } else {
                // Left rotation at p
                wr_right(p, rd_left(z));
                if (rd_left(z)) wr_parent(rd_left(z), p);
                wr_left(z, p);
            }
            wr_parent(p, z);
            wr_parent(z, g);
            if (g) {
                if (rd_left(g) == p) wr_left(g, z);
                else                 wr_right(g, z);
            }
        }
        if (!rd_parent(z))
            m_root = z;
        m_size++;
    }

    size_t size() const { return m_size; }
    bool empty() const { return m_size == 0; }
};

} // namespace expli
