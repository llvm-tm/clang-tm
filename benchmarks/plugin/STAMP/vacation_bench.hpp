#pragma once

#include "stamp_common.hpp"
#include "../stamp_common/ds/rbtree.hpp"
#include <algorithm>

struct ReservationData {
    long numFree;
    long numUsed;
    long numTotal;
    long price;
};

struct Customer {
    long id;
    long bill;
};

struct TM VacationData {
    RBTree<long, ReservationData>* cars;
    RBTree<long, ReservationData>* rooms;
    RBTree<long, ReservationData>* flights;
    RBNode<long, ReservationData>* cars_pool;
    RBNode<long, ReservationData>* rooms_pool;
    RBNode<long, ReservationData>* flights_pool;
    Customer* customers;
    long num_relations;
    long query_range;
    long num_queries_per_tx;
    long percent_user;
};

static VacationData* g_vacation = nullptr;

inline void vacation_generate_prices() {
    auto data = new VacationData();
    long nr = g_vacation_r;
    long cr = (long)(0.9 * nr) + 10;

    data->num_relations = nr;
    data->query_range = (long)(0.9 * nr);
    data->num_queries_per_tx = g_vacation_n;
    data->percent_user = g_vacation_u;

    data->cars = new RBTree<long, ReservationData>();
    data->rooms = new RBTree<long, ReservationData>();
    data->flights = new RBTree<long, ReservationData>();
    data->cars->init();
    data->rooms->init();
    data->flights->init();

    data->cars_pool = new RBNode<long, ReservationData>[nr]();
    data->rooms_pool = new RBNode<long, ReservationData>[nr]();
    data->flights_pool = new RBNode<long, ReservationData>[nr]();

    data->customers = new Customer[cr]();
    for (long i = 0; i < cr; i++)
        data->customers[i].id = -1;

    PRNG rng(42);
    long* order = new long[nr];
    for (long i = 0; i < nr; i++) order[i] = i + 1;
    for (long i = nr - 1; i > 0; i--) {
        long j = (long)(rng.next() % (i + 1));
        long tmp = order[i]; order[i] = order[j]; order[j] = tmp;
    }

    for (long i = 0; i < nr; i++) {
        long id = order[i];
        long num = (long)(rng.next() % 5 + 1) * 100;
        long price = (long)(rng.next() % 5) * 10 + 50;

        RBNode<long, ReservationData>* cn = &data->cars_pool[i];
        cn->key = id;
        cn->val = {num, 0, num, price};
        rbtree_insert(data->cars, cn);

        RBNode<long, ReservationData>* rn = &data->rooms_pool[i];
        rn->key = id;
        rn->val = {num, 0, num, price};
        rbtree_insert(data->rooms, rn);

        RBNode<long, ReservationData>* fn = &data->flights_pool[i];
        fn->key = id;
        fn->val = {num, 0, num, price};
        rbtree_insert(data->flights, fn);
    }
    delete[] order;

    g_vacation = data;

    printf("Initializing manager... done.\n");
    printf("Initializing clients... done.\n");
    printf("    Relations = %li\n", data->num_relations);
    printf("    Transactions = %i\n", g_vacation_t);
    printf("    Queries/transaction = %li\n", data->num_queries_per_tx);
    printf("    Percent user = %li\n", data->percent_user);
    printf("Running clients...\n");
    fflush(stdout);
}

TX static ReservationData* reservation_find(RBTree<long, ReservationData>* table, long id) {
    return rbtree_find(table, id);
}

TX static long query_reservation(RBTree<long, ReservationData>* table, long id) {
    ReservationData* r = reservation_find(table, id);
    if (!r || r->numTotal == 0) return -1;
    return r->numFree;
}

TX static long query_price(RBTree<long, ReservationData>* table, long id) {
    ReservationData* r = reservation_find(table, id);
    if (!r || r->numTotal == 0) return -1;
    return r->price;
}

TX static bool add_reservation(RBTree<long, ReservationData>* table, long id,
                                long num, long price) {
    ReservationData* r = reservation_find(table, id);
    if (!r) return false;
    r->numFree += num;
    r->numTotal += num;
    if (price >= 0) r->price = price;
    return true;
}

TX static bool delete_reservation(RBTree<long, ReservationData>* table, long id,
                                   long num) {
    ReservationData* r = reservation_find(table, id);
    if (!r || r->numTotal == 0) return false;
    if (r->numFree < num) return false;
    r->numFree -= num;
    r->numTotal -= num;
    return true;
}

TX static long make_reservation(RBTree<long, ReservationData>* table, long id) {
    ReservationData* r = reservation_find(table, id);
    if (!r || r->numTotal == 0 || r->numFree <= 0) return -1;
    r->numUsed++;
    r->numFree--;
    return r->price;
}

TX static long cancel_reservation(RBTree<long, ReservationData>* table, long id) {
    ReservationData* r = reservation_find(table, id);
    if (!r || r->numTotal == 0 || r->numUsed <= 0) return -1;
    r->numUsed--;
    r->numFree++;
    return r->price;
}

TX static void add_customer(VacationData* data, long customer_id) {
    Customer& c = data->customers[customer_id - 1];
    if (c.id == -1) {
        c.id = customer_id;
        c.bill = 0;
    }
}

TX static long query_customer_bill(VacationData* data, long customer_id) {
    Customer& c = data->customers[customer_id - 1];
    if (c.id == -1) return -1;
    return c.bill;
}

TX static long delete_customer(VacationData* data, long customer_id) {
    Customer& c = data->customers[customer_id - 1];
    if (c.id == -1) return -1;
    long bill = c.bill;
    c.id = -1;
    c.bill = 0;
    return bill;
}

TX static void make_reservation_tx(VacationData* data, long customer_id) {
    add_customer(data, customer_id);

    PRNG rng(rdtsc() + customer_id);
    long best_prices[3] = {-1, -1, -1};
    long best_ids[3] = {-1, -1, -1};
    bool found = false;

    long nq = (long)(rng.next() % data->num_queries_per_tx) + 1;
    for (long i = 0; i < nq; i++) {
        long t = (long)(rng.next() % 3);
        long id = (long)(rng.next() % data->query_range) + 1;

        RBTree<long, ReservationData>* table = nullptr;
        if (t == 0) table = data->cars;
        else if (t == 1) table = data->flights;
        else table = data->rooms;

        long avail = query_reservation(table, id);
        if (avail > 0) {
            long price = query_price(table, id);
            if (price > best_prices[t]) {
                best_prices[t] = price;
                best_ids[t] = id;
                found = true;
            }
        }
    }

    if (found) {
        for (long t = 0; t < 3; t++) {
            if (best_ids[t] > 0) {
                RBTree<long, ReservationData>* table = nullptr;
                if (t == 0) table = data->cars;
                else if (t == 1) table = data->flights;
                else table = data->rooms;

                long p = make_reservation(table, best_ids[t]);
                if (p >= 0)
                    data->customers[customer_id - 1].bill += p;
            }
        }
    }
}

TX static void delete_customer_tx(VacationData* data, long customer_id) {
    Customer& c = data->customers[customer_id - 1];
    if (c.id != -1) {
        c.id = -1;
        c.bill = 0;
    }
}

TX static void update_tables_tx(VacationData* data) {
    PRNG rng(rdtsc());
    long type = (long)(rng.next() % 3);
    long id = (long)(rng.next() % data->query_range) + 1;
    long op = (long)(rng.next() % 2);

    RBTree<long, ReservationData>* table = nullptr;
    if (type == 0) table = data->cars;
    else if (type == 1) table = data->flights;
    else table = data->rooms;

    if (op == 1) {
        long price = (long)(rng.next() % 5) * 10 + 50;
        add_reservation(table, id, 100, price);
    } else {
        delete_reservation(table, id, 100);
    }
}

THREAD void worker_vacation(ThreadData* td) {
    auto data = g_vacation;
    PRNG rng(42 + td->thread_id);

    int tasks_per_thread = g_vacation_t / g_num_threads;
    int extra = g_vacation_t % g_num_threads;
    int start = td->thread_id * tasks_per_thread + std::min(td->thread_id, extra);
    int end = start + tasks_per_thread + (td->thread_id < extra ? 1 : 0);

    for (int iter = start; iter < end; iter++) {
        long r = (long)(rng.next() % 100);
        long customer_id = (long)(rng.next() % data->query_range) + 1;

        if (r < data->percent_user) {
            make_reservation_tx(data, customer_id);
        } else if (r % 2 == 0) {
            delete_customer_tx(data, customer_id);
        } else {
            update_tables_tx(data);
        }

        total_ops.fetch_add(1, std::memory_order_relaxed);
    }
}
