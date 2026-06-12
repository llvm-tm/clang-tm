#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <new>
#include <utility>

// TM-safe replacement for std::map.
// Uses an open-addressing hashmap with linear probing — no opaque STL
// calls, so the TM pass can instrument every element access.
// All allocation uses ::operator new/delete (redirected by TM pass
// inside TX).  Deletion uses backward-shift (no tombstones).
template <typename K, typename V> class TMSafeMap
{
	struct Slot {
		bool occupied = false;
		std::pair<K, V> kv;
	};

	Slot*  m_data = nullptr;
	size_t m_cap  = 0;   // power of 2, 0 = empty
	size_t m_size = 0;   // occupied count

	static constexpr double MAX_LOAD = 0.75;

	size_t hash_idx(const K &k) const
	{
		return std::hash<K>{}(k) & (m_cap - 1);
	}

	// Find occupied slot for key, or return m_cap if not found.
	size_t find_occupied(const K &k) const
	{
		if (m_cap == 0) return 0;
		size_t i = hash_idx(k);
		while (m_data[i].occupied) {
			if (m_data[i].kv.first == k) return i;
			i = (i + 1) & (m_cap - 1);
		}
		return m_cap;
	}

	// Return index of empty slot to use for insertion (grow first if needed).
	size_t find_empty(const K &k)
	{
		if (m_size >= static_cast<size_t>(m_cap * MAX_LOAD))
			grow();
		size_t i = hash_idx(k);
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
				if (m_data[i].occupied) {
					size_t j = std::hash<K>{}(m_data[i].kv.first) & (nc - 1);
					while (nd[j].occupied)
						j = (j + 1) & (nc - 1);
					nd[j].occupied = true;
					nd[j].kv = m_data[i].kv;
				}
			}
			::operator delete(m_data);
		}

		m_data = nd;
		m_cap  = nc;
	}

	// Backward-shift deletion (Knuth).  Slides subsequent entries back
	// to fill the gap without breaking probe chains or needing tombstones.
	void do_erase(size_t i)
	{
		m_size--;
		m_data[i].occupied = false;
		size_t hole = i;
		for (;;) {
			size_t next = (hole + 1) & (m_cap - 1);
			if (!m_data[next].occupied) break;
			size_t h = std::hash<K>{}(m_data[next].kv.first) & (m_cap - 1);
			// Entry at next slides into the current hole iff the
			// CURRENT hole position is on the probe chain of next
			// (closed interval [h, next] cyclic).  If not on the
			// chain, sliding would strand 'next' because a future
			// find starting at h would stop at the new empty slot.
			bool must_slide = [&]() {
				if (h <= next)
					return hole >= h && hole <= next;
				else
					return hole >= h || hole <= next;
			}();
			if (!must_slide) break;
			m_data[hole] = m_data[next];
			m_data[next].occupied = false;
			hole = next;
		}
	}

	// Iterator that skips empty slots.  operator* returns a reference
	// to the std::pair<K, V> stored in the slot (lvalue).
	template <typename SlotT, typename PairT> struct MapIter {
		SlotT* slot;
		SlotT* end_slot;

		MapIter &operator++()
		{
			++slot;
			while (slot != end_slot && !slot->occupied)
				++slot;
			return *this;
		}
		bool operator!=(const MapIter &o) const { return slot != o.slot; }
		bool operator==(const MapIter &o) const { return slot == o.slot; }
		template <typename SlotT2, typename PairT2>
		bool operator!=(const MapIter<SlotT2, PairT2> &o) const { return slot != o.slot; }
		template <typename SlotT2, typename PairT2>
		bool operator==(const MapIter<SlotT2, PairT2> &o) const { return slot == o.slot; }
		PairT &operator*()  const { return slot->kv; }
		PairT *operator->() const { return &slot->kv; }
	};

public:
	TMSafeMap() = default;
	TMSafeMap(const TMSafeMap &o) { for (size_t i = 0; i < o.m_cap; i++) { if (o.m_data[i].occupied) (*this)[o.m_data[i].kv.first] = o.m_data[i].kv.second; } }
	TMSafeMap &operator=(const TMSafeMap &o) { if (this != &o) { clear(); for (size_t i = 0; i < o.m_cap; i++) { if (o.m_data[i].occupied) (*this)[o.m_data[i].kv.first] = o.m_data[i].kv.second; } } return *this; }
	~TMSafeMap() { ::operator delete(m_data); }

	using iterator       = MapIter<      Slot,       std::pair<K,       V>>;
	using const_iterator = MapIter<const Slot, const std::pair<K, V>>;

	const_iterator begin() const
	{
		if (!m_data) return {nullptr, nullptr};
		for (size_t i = 0; i < m_cap; i++)
			if (m_data[i].occupied) return {m_data + i, m_data + m_cap};
		return {m_data + m_cap, m_data + m_cap};
	}
	const_iterator end() const
	{
		if (!m_data) return {nullptr, nullptr};
		return {m_data + m_cap, m_data + m_cap};
	}

	iterator begin()
	{
		if (!m_data) return {nullptr, nullptr};
		for (size_t i = 0; i < m_cap; i++)
			if (m_data[i].occupied) return {m_data + i, m_data + m_cap};
		return {m_data + m_cap, m_data + m_cap};
	}
	iterator end()
	{
		if (!m_data) return {nullptr, nullptr};
		return {m_data + m_cap, m_data + m_cap};
	}

	const_iterator find(const K &k) const
	{
		size_t i = find_occupied(k);
		if (i < m_cap)
			return {m_data + i, m_data + m_cap};
		return end();
	}

	V &operator[](const K &k)
	{
		size_t i = find_occupied(k);
		if (i < m_cap)
			return m_data[i].kv.second;
		i = find_empty(k);
		m_data[i].occupied = true;
		m_data[i].kv.first  = k;
		m_data[i].kv.second = V{};
		m_size++;
		return m_data[i].kv.second;
	}

	size_t erase(const K &k)
	{
		size_t i = find_occupied(k);
		if (i >= m_cap) return 0;
		do_erase(i);
		return 1;
	}

	void clear()
	{
		for (size_t i = 0; i < m_cap; i++)
			m_data[i].occupied = false;
		m_size = 0;
	}

	size_t size() const { return m_size; }
	bool empty() const { return m_size == 0; }
};

// TM-safe replacement for std::multimap.
// Same explicit-loop + field-by-field assignment approach as TMSafeMap.
template <typename K, typename V> class TMSafeMultiMap
{
	std::pair<K, V>* m_data = nullptr;
	size_t           m_size = 0;
	size_t           m_cap  = 0;

	void grow(size_t min_cap)
	{
		size_t nc = m_cap ? m_cap : 4;
		while (nc < min_cap) nc *= 2;
		auto* nd = static_cast<std::pair<K, V>*>(::operator new(nc * sizeof(std::pair<K, V>)));
		for (size_t i = 0; i < m_size; i++) {
			nd[i].first  = m_data[i].first;
			nd[i].second = m_data[i].second;
		}
		::operator delete(m_data);
		m_data = nd;
		m_cap  = nc;
	}

	size_t lower_pos(const K &k) const
	{
		auto first = m_data;
		auto count = m_size;
		while (count > 0) {
			auto step = count / 2;
			auto it = first + step;
			if (it->first < k) {
				first = it + 1;
				count -= step + 1;
			} else {
				count = step;
			}
		}
		return static_cast<size_t>(first - m_data);
	}

	size_t upper_pos(const K &k) const
	{
		auto first = m_data;
		auto count = m_size;
		while (count > 0) {
			auto step = count / 2;
			auto it = first + step;
			if (!(k < it->first)) {
				first = it + 1;
				count -= step + 1;
			} else {
				count = step;
			}
		}
		return static_cast<size_t>(first - m_data);
	}

public:
	TMSafeMultiMap() = default;
	TMSafeMultiMap(const TMSafeMultiMap& o) { reserve(o.m_size); for (size_t i = 0; i < o.m_size; i++) { m_data[i].first = o.m_data[i].first; m_data[i].second = o.m_data[i].second; } m_size = o.m_size; }
	TMSafeMultiMap& operator=(const TMSafeMultiMap& o) { if (this != &o) { clear(); reserve(o.m_size); for (size_t i = 0; i < o.m_size; i++) { m_data[i].first = o.m_data[i].first; m_data[i].second = o.m_data[i].second; } m_size = o.m_size; } return *this; }
	~TMSafeMultiMap() { clear(); ::operator delete(m_data); }

	using const_iterator = const std::pair<K, V>*;

	void reserve(size_t n) { if (n > m_cap) grow(n); }

	void insert(const std::pair<K, V> &p)
	{
		auto pos = upper_pos(p.first);
		if (m_size >= m_cap) grow(m_size + 1);
		for (size_t i = m_size; i > pos; i--) {
			m_data[i].first  = m_data[i - 1].first;
			m_data[i].second = m_data[i - 1].second;
		}
		m_data[pos].first  = p.first;
		m_data[pos].second = p.second;
		m_size++;
	}

	using iterator = const std::pair<K, V>*;

	void clear()
	{
		m_size = 0;
	}

	size_t size() const { return m_size; }
	bool empty() const { return m_size == 0; }

	iterator begin() const { return m_data; }
	iterator end()   const { return m_data + m_size; }

	const_iterator lower_bound(const K &k) const
	{
		auto pos = lower_pos(k);
		return m_data + pos;
	}
};
