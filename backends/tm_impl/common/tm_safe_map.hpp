#pragma once

#include <cstddef>
#include <new>
#include <utility>

// TM-safe replacement for std::map.
// Uses a sorted dynamic array with explicit inline binary search and
// inline insert/erase — no opaque STL calls, so the TM pass can
// instrument every element access.
// NOTE: All element copies use field-by-field assignment (not struct
// assignment) to prevent LLVM from lowering to opaque memcpy calls.
template <typename K, typename V> class TMSafeMap
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

	size_t find_pos(const K &k) const
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

public:
	TMSafeMap() = default;
	TMSafeMap(const TMSafeMap& o) { reserve(o.m_size); for (size_t i = 0; i < o.m_size; i++) { m_data[i].first = o.m_data[i].first; m_data[i].second = o.m_data[i].second; } m_size = o.m_size; }
	TMSafeMap& operator=(const TMSafeMap& o) { if (this != &o) { clear(); reserve(o.m_size); for (size_t i = 0; i < o.m_size; i++) { m_data[i].first = o.m_data[i].first; m_data[i].second = o.m_data[i].second; } m_size = o.m_size; } return *this; }
	~TMSafeMap() { clear(); ::operator delete(m_data); }

	using const_iterator = const std::pair<K, V>*;

	void reserve(size_t n) { if (n > m_cap) grow(n); }

	const_iterator find(const K &k) const
	{
		auto pos = find_pos(k);
		if (pos < m_size && m_data[pos].first == k)
			return m_data + pos;
		return m_data + m_size;
	}

	V &operator[](const K &k)
	{
		auto pos = find_pos(k);
		if (pos < m_size && m_data[pos].first == k)
			return m_data[pos].second;
		if (m_size >= m_cap) grow(m_size + 1);
		for (size_t i = m_size; i > pos; i--) {
			m_data[i].first  = m_data[i - 1].first;
			m_data[i].second = m_data[i - 1].second;
		}
		m_data[pos].first  = k;
		m_data[pos].second = V{};
		m_size++;
		return m_data[pos].second;
	}

	size_t erase(const K &k)
	{
		auto pos = find_pos(k);
		if (pos >= m_size || m_data[pos].first != k)
			return 0;
		for (size_t i = pos + 1; i < m_size; i++) {
			m_data[i - 1].first  = m_data[i].first;
			m_data[i - 1].second = m_data[i].second;
		}
		m_size--;
		return 1;
	}

	void clear()
	{
		m_size = 0;
	}

	size_t size() const { return m_size; }
	bool empty() const { return m_size == 0; }

	using iterator = const std::pair<K, V>*;
	iterator begin() const { return m_data; }
	iterator end()   const { return m_data + m_size; }
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
