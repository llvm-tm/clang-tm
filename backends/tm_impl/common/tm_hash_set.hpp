#pragma once

#include <cstddef>
#include <functional>
#include <new>

// Open-addressing hash set with linear probing and backward-shift deletion.
// No STL dependencies — all allocations go through ::operator new/delete
// (redirected by the TM pass inside transactions).
// Load factor: 0.75.  Capacity is always a power of two.
template <typename T, typename Hash = std::hash<T>>
class TMSafeHashSet
{
	struct Slot {
		bool occupied = false;
		T    value;
	};

	Slot*  m_data = nullptr;
	size_t m_cap  = 0;
	size_t m_size = 0;

	static constexpr double MAX_LOAD = 0.75;

	size_t hash_idx(const T& v) const
	{
		return Hash{}(v) & (m_cap - 1);
	}

	size_t find_occupied(const T& v) const
	{
		if (m_cap == 0) return 0;
		size_t i = hash_idx(v);
		while (m_data[i].occupied) {
			if (m_data[i].value == v) return i;
			i = (i + 1) & (m_cap - 1);
		}
		return m_cap;
	}

	size_t find_empty(const T& v)
	{
		if (m_size >= static_cast<size_t>(m_cap * MAX_LOAD))
			grow();
		size_t i = hash_idx(v);
		while (m_data[i].occupied)
			i = (i + 1) & (m_cap - 1);
		return i;
	}

	void grow()
	{
		size_t nc = m_cap ? m_cap * 2 : 16;
		auto* nd = static_cast<Slot*>(::operator new(nc * sizeof(Slot)));
		for (size_t i = 0; i < nc; i++)
			nd[i].occupied = false;

		if (m_data) {
			for (size_t i = 0; i < m_cap; i++) {
				if (!m_data[i].occupied) continue;
				size_t j = Hash{}(m_data[i].value) & (nc - 1);
				while (nd[j].occupied)
					j = (j + 1) & (nc - 1);
				nd[j].occupied = true;
				nd[j].value    = m_data[i].value;
			}
			::operator delete(m_data);
		}

		m_data = nd;
		m_cap  = nc;
	}

	// Backward-shift deletion (Knuth).  Entries following the hole are
	// slid backward iff the CURRENT hole is on their probe chain.
	// This preserves probe chain integrity without tombstones.
	void do_erase(size_t i)
	{
		m_size--;
		m_data[i].occupied = false;
		size_t hole = i;

		for (;;) {
			size_t next = (hole + 1) & (m_cap - 1);
			if (!m_data[next].occupied) break;

			size_t h = Hash{}(m_data[next].value) & (m_cap - 1);

			// 'next' can slide into 'hole' iff the CURRENT hole is
			// on the probe chain of 'next' (closed interval [h, next],
			// cyclic).  If NOT on the chain, sliding would strand 'next'
			// — a future find starting at h would stop at the new hole.
			bool must_slide;
			if (h <= next)
				must_slide = (hole >= h && hole <= next);
			else
				must_slide = (hole >= h || hole <= next);

			if (!must_slide) break;

			m_data[hole] = m_data[next];
			m_data[next].occupied = false;
			hole = next;
		}
	}

public:
	// Forward iterator that skips empty slots.
	struct Iter {
		Slot* slot;
		Slot* end_slot;
		using value_type = T;
		Iter& operator++() {
			++slot;
			while (slot < end_slot && !slot->occupied) ++slot;
			return *this;
		}
		const T& operator*() const { return slot->value; }
		bool operator!=(const Iter& o) const { return slot != o.slot; }
	};

	Iter begin() const
	{
		Slot* p = m_data;
		while (p < m_data + m_cap && !p->occupied) ++p;
		return {p, m_data + m_cap};
	}
	Iter end() const { return {m_data + m_cap, m_data + m_cap}; }

	TMSafeHashSet() = default;
	TMSafeHashSet(const TMSafeHashSet& o)
	{
		for (size_t i = 0; i < o.m_cap; i++)
			if (o.m_data[i].occupied)
				insert(o.m_data[i].value);
	}
	TMSafeHashSet& operator=(const TMSafeHashSet& o)
	{
		if (this != &o) {
			clear();
			for (size_t i = 0; i < o.m_cap; i++)
				if (o.m_data[i].occupied)
					insert(o.m_data[i].value);
		}
		return *this;
	}
	~TMSafeHashSet() { ::operator delete(m_data); }

	bool contains(const T& v) const
	{
		return find_occupied(v) < m_cap;
	}

	bool insert(const T& v)
	{
		size_t i = find_occupied(v);
		if (i < m_cap) return false;
		i = find_empty(v);
		m_data[i].occupied = true;
		m_data[i].value    = v;
		m_size++;
		return true;
	}

	bool erase(const T& v)
	{
		size_t i = find_occupied(v);
		if (i >= m_cap) return false;
		do_erase(i);
		return true;
	}

	void clear()
	{
		for (size_t i = 0; i < m_cap; i++)
			m_data[i].occupied = false;
		m_size = 0;
	}

	size_t size() const { return m_size; }
	bool   empty() const { return m_size == 0; }
};
