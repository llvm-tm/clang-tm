#pragma once

#ifndef TM
#define TM __attribute__((annotate("tm")))
#endif

template<typename T>
struct TM SLNode {
    T val;
    SLNode* next;
};

template<typename T>
struct TM SortedList {
    SLNode<T>* head;
    long (*cmp)(const T&, const T&);

    void init(long (*c)(const T&, const T&) = nullptr) {
        head = nullptr;
        cmp = c ? c : [](const T& a, const T& b) -> long { return (a > b) - (a < b); };
    }
};

template<typename T>
static inline void slist_insert(SortedList<T>* list, SLNode<T>* node) {
    node->next = nullptr;
    T val = node->val;

    if (!list->head) {
        list->head = node;
        return;
    }

    long (*cmp)(const T&, const T&) = list->cmp;
    if (cmp(val, list->head->val) <= 0) {
        node->next = list->head;
        list->head = node;
        return;
    }

    SLNode<T>* cur = list->head;
    while (cur->next && cmp(val, cur->next->val) > 0)
        cur = cur->next;

    node->next = cur->next;
    cur->next = node;
}

template<typename T>
static inline bool slist_remove(SortedList<T>* list, const T& val) {
    if (!list->head) return false;

    long (*cmp)(const T&, const T&) = list->cmp;
    if (cmp(val, list->head->val) == 0) {
        list->head = list->head->next;
        return true;
    }

    SLNode<T>* cur = list->head;
    while (cur->next) {
        if (cmp(val, cur->next->val) == 0) {
            cur->next = cur->next->next;
            return true;
        }
        cur = cur->next;
    }
    return false;
}

template<typename T>
static inline bool slist_contains(SortedList<T>* list, const T& val) {
    long (*cmp)(const T&, const T&) = list->cmp;
    SLNode<T>* cur = list->head;
    while (cur) {
        long c = cmp(val, cur->val);
        if (c == 0) return true;
        if (c < 0) return false;
        cur = cur->next;
    }
    return false;
}
