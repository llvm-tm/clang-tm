#pragma once

#include "memory_access.hpp"

// ── Policy-based sorted singly-linked list (no TM/TX annotations) ──
// All shared-memory accesses go through MemoryAccess<UseTM>, so a single
// implementation works both inside and outside transactions.

namespace explicit_slist {

template<typename T>
struct Node {
    T data;
    Node* next;
};

template<typename T>
struct List {
    Node<T>* head = nullptr;
};

// ── insert (sorted, no duplicates) ────────────────────────────────
template<bool UseTM, typename T>
bool insert(List<T>* list, Node<T>* node) {
    using MA = MemoryAccess<UseTM>;
    Node<T>* prev = nullptr;
    Node<T>* curr = MA::load(&list->head);

    while (curr) {
        if (MA::load(&node->data) == MA::load(&curr->data))
            return false; // duplicate
        if (MA::load(&node->data) < MA::load(&curr->data))
            break;
        prev = curr;
        curr = MA::load(&curr->next);
    }

    if (prev) {
        MA::store(&node->next, MA::load(&prev->next));
        MA::store(&prev->next, node);
    } else {
        MA::store(&node->next, MA::load(&list->head));
        MA::store(&list->head, node);
    }
    return true;
}

// ── remove ─────────────────────────────────────────────────────────
template<bool UseTM, typename T>
bool remove(List<T>* list, const T& data) {
    using MA = MemoryAccess<UseTM>;
    Node<T>* prev = nullptr;
    Node<T>* curr = MA::load(&list->head);

    while (curr) {
        if (MA::load(&curr->data) == data) {
            if (prev)
                MA::store(&prev->next, MA::load(&curr->next));
            else
                MA::store(&list->head, MA::load(&curr->next));
            return true;
        }
        prev = curr;
        curr = MA::load(&curr->next);
    }
    return false;
}

// ── contains ───────────────────────────────────────────────────────
template<bool UseTM, typename T>
bool contains(List<T>* list, const T& data) {
    using MA = MemoryAccess<UseTM>;
    Node<T>* curr = MA::load(&list->head);
    while (curr) {
        if (MA::load(&curr->data) == data) return true;
        curr = MA::load(&curr->next);
    }
    return false;
}

} // namespace explicit_slist
