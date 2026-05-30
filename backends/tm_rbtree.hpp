// TMRBTreeMap / TMRBTreeMultiMap
//
// Replacements for std::map / std::multimap that avoid libstdc++'s opaque
// _Rb_tree_* functions (whose stores bypass TM instrumentation).  Under
// normal circumstances a developer would use std::map or std::multimap, but
// their internal rebalancing happens in a shared library that the LLVM TM
// plugin never sees as IR.
//
// These implementations are entirely inlined header code, so every load and
// store is visible to the plugin's always-instrument pass.

#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>

template <typename K, typename V> class TMRBTreeMap
{
	enum Color { RED, BLACK };
	struct Node {
		std::pair<const K, V> data;
		Color color;
		Node *left;
		Node *right;
		Node *parent;

		Node(const K &k, const V &v)
		    : data(k, v),
		      color(RED),
		      left(nullptr),
		      right(nullptr),
		      parent(nullptr)
		{
		}
	};

	Node *m_root;
	size_t m_size;

	Node *minimum(Node *x) const
	{
		while (x && x->left)
			x = x->left;
		return x;
	}

	void rotate_left(Node *x)
	{
		Node *y = x->right;
		x->right = y->left;
		if (y->left)
			y->left->parent = x;
		y->parent = x->parent;
		if (!x->parent)
			m_root = y;
		else if (x == x->parent->left)
			x->parent->left = y;
		else
			x->parent->right = y;
		y->left = x;
		x->parent = y;
	}

	void rotate_right(Node *x)
	{
		Node *y = x->left;
		x->left = y->right;
		if (y->right)
			y->right->parent = x;
		y->parent = x->parent;
		if (!x->parent)
			m_root = y;
		else if (x == x->parent->right)
			x->parent->right = y;
		else
			x->parent->left = y;
		y->right = x;
		x->parent = y;
	}

	void insert_fixup(Node *z)
	{
		while (z->parent && z->parent->color == RED) {
			if (z->parent == z->parent->parent->left) {
				Node *y = z->parent->parent->right;
				if (y && y->color == RED) {
					z->parent->color = BLACK;
					y->color = BLACK;
					z->parent->parent->color = RED;
					z = z->parent->parent;
				} else {
					if (z == z->parent->right) {
						z = z->parent;
						rotate_left(z);
					}
					z->parent->color = BLACK;
					z->parent->parent->color = RED;
					rotate_right(z->parent->parent);
				}
			} else {
				Node *y = z->parent->parent->left;
				if (y && y->color == RED) {
					z->parent->color = BLACK;
					y->color = BLACK;
					z->parent->parent->color = RED;
					z = z->parent->parent;
				} else {
					if (z == z->parent->left) {
						z = z->parent;
						rotate_right(z);
					}
					z->parent->color = BLACK;
					z->parent->parent->color = RED;
					rotate_left(z->parent->parent);
				}
			}
		}
		m_root->color = BLACK;
	}

	void transplant(Node *u, Node *v)
	{
		if (!u->parent)
			m_root = v;
		else if (u == u->parent->left)
			u->parent->left = v;
		else
			u->parent->right = v;
		if (v)
			v->parent = u->parent;
	}

	void erase_fixup(Node *x, Node *x_parent)
	{
		while (x != m_root && (!x || x->color == BLACK)) {
			if (x == x_parent->left) {
				Node *w = x_parent->right;
				if (w && w->color == RED) {
					w->color = BLACK;
					x_parent->color = RED;
					rotate_left(x_parent);
					w = x_parent->right;
				}
				if ((!w->left || w->left->color == BLACK) &&
				    (!w->right || w->right->color == BLACK)) {
					if (w)
						w->color = RED;
					x = x_parent;
					x_parent = x_parent->parent;
				} else {
					if (!w->right || w->right->color == BLACK) {
						if (w->left)
							w->left->color = BLACK;
						if (w)
							w->color = RED;
						rotate_right(w);
						w = x_parent->right;
					}
					if (w)
						w->color = x_parent->color;
					x_parent->color = BLACK;
					if (w && w->right)
						w->right->color = BLACK;
					rotate_left(x_parent);
					x = m_root;
					x_parent = nullptr;
				}
			} else {
				Node *w = x_parent->left;
				if (w && w->color == RED) {
					w->color = BLACK;
					x_parent->color = RED;
					rotate_right(x_parent);
					w = x_parent->left;
				}
				if ((!w->right || w->right->color == BLACK) &&
				    (!w->left || w->left->color == BLACK)) {
					if (w)
						w->color = RED;
					x = x_parent;
					x_parent = x_parent->parent;
				} else {
					if (!w->left || w->left->color == BLACK) {
						if (w->right)
							w->right->color = BLACK;
						if (w)
							w->color = RED;
						rotate_left(w);
						w = x_parent->left;
					}
					if (w)
						w->color = x_parent->color;
					x_parent->color = BLACK;
					if (w && w->left)
						w->left->color = BLACK;
					rotate_right(x_parent);
					x = m_root;
					x_parent = nullptr;
				}
			}
		}
		if (x)
			x->color = BLACK;
	}

	void clear_subtree(Node *n)
	{
		if (!n)
			return;
		clear_subtree(n->left);
		clear_subtree(n->right);
		delete n;
	}

public:
	using value_type = std::pair<const K, V>;

	class Iterator
	{
		friend class TMRBTreeMap;
		Node *m_node;
		explicit Iterator(Node *n)
		    : m_node(n)
		{
		}

	public:
		using iterator_category = std::bidirectional_iterator_tag;
		using value_type = std::pair<const K, V>;
		using reference = std::pair<const K, V> &;
		using pointer = std::pair<const K, V> *;

		reference operator*() const { return m_node->data; }
		pointer operator->() const { return &m_node->data; }

		Iterator &operator++()
		{
			if (m_node->right) {
				m_node = m_node->right;
				while (m_node->left)
					m_node = m_node->left;
			} else {
				Node *p = m_node->parent;
				while (p && m_node == p->right) {
					m_node = p;
					p = p->parent;
				}
				m_node = p;
			}
			return *this;
		}
		bool operator==(const Iterator &o) const { return m_node == o.m_node; }
		bool operator!=(const Iterator &o) const { return m_node != o.m_node; }
	};

	TMRBTreeMap()
	    : m_root(nullptr),
	      m_size(0)
	{
	}
	~TMRBTreeMap() { clear_subtree(m_root); }

	TMRBTreeMap(const TMRBTreeMap &) = delete;
	TMRBTreeMap &operator=(const TMRBTreeMap &) = delete;

	void clear()
	{
		clear_subtree(m_root);
		m_root = nullptr;
		m_size = 0;
	}

	Iterator begin() const
	{
		if (!m_root)
			return end();
		return Iterator(minimum(m_root));
	}

	Iterator end() const { return Iterator(nullptr); }

	Iterator find(const K &k) const
	{
		Node *x = m_root;
		while (x) {
			if (k < x->data.first)
				x = x->left;
			else if (x->data.first < k)
				x = x->right;
			else
				return Iterator(x);
		}
		return end();
	}

	V &operator[](const K &k)
	{
		Node *x = m_root;
		Node *p = nullptr;
		while (x) {
			p = x;
			if (k < x->data.first)
				x = x->left;
			else if (x->data.first < k)
				x = x->right;
			else
				return x->data.second;
		}
		auto *z = new Node(k, V{});
		z->parent = p;
		if (!p)
			m_root = z;
		else if (z->data.first < p->data.first)
			p->left = z;
		else
			p->right = z;
		insert_fixup(z);
		m_size++;
		return z->data.second;
	}

	size_t erase(const K &k)
	{
		Node *z = m_root;
		while (z) {
			if (k < z->data.first)
				z = z->left;
			else if (z->data.first < k)
				z = z->right;
			else
				break;
		}
		if (!z)
			return 0;

		Node *y = z;
		Node *x = nullptr;
		Node *x_parent = nullptr;
		Color y_orig = y->color;

		if (!z->left) {
			x = z->right;
			x_parent = z->parent;
			transplant(z, z->right);
		} else if (!z->right) {
			x = z->left;
			x_parent = z->parent;
			transplant(z, z->left);
		} else {
			y = minimum(z->right);
			y_orig = y->color;
			x = y->right;
			if (y->parent == z) {
				if (x)
					x->parent = y;
				x_parent = y;
			} else {
				x_parent = y->parent;
				transplant(y, y->right);
				y->right = z->right;
				if (y->right)
					y->right->parent = y;
			}
			transplant(z, y);
			y->left = z->left;
			if (y->left)
				y->left->parent = y;
			y->color = z->color;
		}

		if (y_orig == BLACK)
			erase_fixup(x, x_parent);

		delete z;
		m_size--;
		return 1;
	}

	size_t size() const { return m_size; }
	bool empty() const { return m_size == 0; }
};

// ── Multimap: allows duplicate keys ──────────────────────────

template <typename K, typename V> class TMRBTreeMultiMap
{
	struct Node {
		std::pair<const K, V> data;
		bool is_red;
		Node *left;
		Node *right;
		Node *parent;

		Node(const K &k, const V &v)
		    : data(k, v),
		      is_red(true),
		      left(nullptr),
		      right(nullptr),
		      parent(nullptr)
		{
		}
	};

	Node *m_root;
	size_t m_size;

	Node *minimum(Node *x) const
	{
		while (x && x->left)
			x = x->left;
		return x;
	}

	void rotate_left(Node *x)
	{
		Node *y = x->right;
		x->right = y->left;
		if (y->left)
			y->left->parent = x;
		y->parent = x->parent;
		if (!x->parent)
			m_root = y;
		else if (x == x->parent->left)
			x->parent->left = y;
		else
			x->parent->right = y;
		y->left = x;
		x->parent = y;
	}

	void rotate_right(Node *x)
	{
		Node *y = x->left;
		x->left = y->right;
		if (y->right)
			y->right->parent = x;
		y->parent = x->parent;
		if (!x->parent)
			m_root = y;
		else if (x == x->parent->right)
			x->parent->right = y;
		else
			x->parent->left = y;
		y->right = x;
		x->parent = y;
	}

	void insert_fixup(Node *z)
	{
		while (z->parent && z->parent->is_red) {
			if (z->parent == z->parent->parent->left) {
				Node *y = z->parent->parent->right;
				if (y && y->is_red) {
					z->parent->is_red = false;
					y->is_red = false;
					z->parent->parent->is_red = true;
					z = z->parent->parent;
				} else {
					if (z == z->parent->right) {
						z = z->parent;
						rotate_left(z);
					}
					z->parent->is_red = false;
					z->parent->parent->is_red = true;
					rotate_right(z->parent->parent);
				}
			} else {
				Node *y = z->parent->parent->left;
				if (y && y->is_red) {
					z->parent->is_red = false;
					y->is_red = false;
					z->parent->parent->is_red = true;
					z = z->parent->parent;
				} else {
					if (z == z->parent->left) {
						z = z->parent;
						rotate_right(z);
					}
					z->parent->is_red = false;
					z->parent->parent->is_red = true;
					rotate_left(z->parent->parent);
				}
			}
		}
		m_root->is_red = false;
	}

	void clear_subtree(Node *n)
	{
		if (!n)
			return;
		clear_subtree(n->left);
		clear_subtree(n->right);
		delete n;
	}

public:
	using value_type = std::pair<const K, V>;

	class Iterator
	{
		friend class TMRBTreeMultiMap;
		Node *m_node;
		explicit Iterator(Node *n)
		    : m_node(n)
		{
		}

	public:
		using value_type = std::pair<const K, V>;
		using reference = std::pair<const K, V> &;
		using pointer = std::pair<const K, V> *;

		reference operator*() const { return m_node->data; }
		pointer operator->() const { return &m_node->data; }

		Iterator &operator++()
		{
			if (m_node->right) {
				m_node = m_node->right;
				while (m_node->left)
					m_node = m_node->left;
			} else {
				Node *p = m_node->parent;
				while (p && m_node == p->right) {
					m_node = p;
					p = p->parent;
				}
				m_node = p;
			}
			return *this;
		}
		bool operator==(const Iterator &o) const { return m_node == o.m_node; }
		bool operator!=(const Iterator &o) const { return m_node != o.m_node; }
	};

	TMRBTreeMultiMap()
	    : m_root(nullptr),
	      m_size(0)
	{
	}
	~TMRBTreeMultiMap() { clear_subtree(m_root); }

	TMRBTreeMultiMap(const TMRBTreeMultiMap &) = delete;
	TMRBTreeMultiMap &operator=(const TMRBTreeMultiMap &) = delete;

	void clear()
	{
		clear_subtree(m_root);
		m_root = nullptr;
		m_size = 0;
	}

	Iterator begin() const
	{
		if (!m_root)
			return end();
		return Iterator(minimum(m_root));
	}

	Iterator end() const { return Iterator(nullptr); }

	Iterator lower_bound(const K &k) const
	{
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

	void insert(const std::pair<K, V> &p)
	{
		Node *x = m_root;
		Node *parent = nullptr;
		while (x) {
			parent = x;
			if (!(x->data.first < p.first))
				x = x->left;
			else
				x = x->right;
		}
		auto *z = new Node(p.first, p.second);
		z->parent = parent;
		if (!parent)
			m_root = z;
		else if (!(parent->data.first < z->data.first))
			parent->left = z;
		else
			parent->right = z;
		insert_fixup(z);
		m_size++;
	}

	size_t size() const { return m_size; }
	bool empty() const { return m_size == 0; }
};
