#pragma once

#include "stamp_common.hpp"
#include <algorithm>

struct Reservation {
    int id;
    int num_used;
    int num_free;
    int num_total;
    int price;
};

struct Customer {
    int id;
    int bill;
};

struct ReservationInfo {
    int type;
    int id;
    int price;
};

struct TM VacationData {
    Reservation* cars;
    Reservation* rooms;
    Reservation* flights;
    Customer* customers;
    int num_relations;
    int query_range;
    int num_queries_per_tx;
    int percent_user;
};

static VacationData* g_vacation = nullptr;

inline void vacation_generate_prices() {
    auto data = new VacationData();
    data->num_relations = g_vacation_r;
    data->query_range = (int)(0.9 * data->num_relations);
    data->num_queries_per_tx = g_vacation_n;
    data->percent_user = g_vacation_u;

    int nr = data->num_relations;
    int cr = data->query_range + 10;
    data->cars = new Reservation[nr]();
    data->rooms = new Reservation[nr]();
    data->flights = new Reservation[nr]();
    data->customers = new Customer[cr]();

    for (int i = 0; i < cr; i++)
        data->customers[i].id = -1;

    PRNG rng(42);
    for (int i = 1; i <= nr; i++) {
        int num = (int)(rng.next() % 5 + 1) * 100;
        int price = (int)(rng.next() % 5) * 10 + 50;
        data->cars[i - 1] = {i, 0, num, num, price};
        data->rooms[i - 1] = {i, 0, num, num, price};
        data->flights[i - 1] = {i, 0, num, num, price};
    }

    g_vacation = data;

    printf("Initializing manager... done.\n");
    printf("Initializing clients... done.\n");
    printf("    Relations = %i\n", data->num_relations);
    printf("    Transactions = %i\n", g_vacation_t);
    printf("    Queries/transaction = %i\n", data->num_queries_per_tx);
    printf("    Percent user = %i\n", data->percent_user);
    printf("Running clients...\n");
    fflush(stdout);
}

static inline bool reservation_exists(Reservation* table, int id) {
    return table[id - 1].num_total > 0;
}

TX static int query_reservation(Reservation* table, int id) {
    if (!reservation_exists(table, id)) return -1;
    return table[id - 1].num_free;
}

TX static int query_price(Reservation* table, int id) {
    if (!reservation_exists(table, id)) return -1;
    return table[id - 1].price;
}

TX static bool add_reservation(Reservation* table, int id, int num, int price) {
    Reservation& r = table[id - 1];
    r.num_free += num;
    r.num_total += num;
    r.price = price;
    return true;
}

TX static bool delete_reservation(Reservation* table, int id, int num) {
    Reservation& r = table[id - 1];
    if (r.num_total == 0) return false;
    if (r.num_free < num) return false;
    r.num_free -= num;
    r.num_total -= num;
    return true;
}

TX static int make_reservation(Reservation* table, int id) {
    Reservation& r = table[id - 1];
    if (r.num_total == 0 || r.num_free <= 0) return -1;
    r.num_used++;
    r.num_free--;
    return r.price;
}

TX static int cancel_reservation(Reservation* table, int id) {
    Reservation& r = table[id - 1];
    if (r.num_total == 0 || r.num_used <= 0) return -1;
    r.num_used--;
    r.num_free++;
    return r.price;
}

TX static void add_customer(VacationData* data, int customer_id) {
    Customer& c = data->customers[customer_id - 1];
    if (c.id == -1) {
        c.id = customer_id;
        c.bill = 0;
    }
}

TX static int query_customer_bill(VacationData* data, int customer_id) {
    Customer& c = data->customers[customer_id - 1];
    if (c.id == -1) return -1;
    return c.bill;
}

TX static int delete_customer(VacationData* data, int customer_id) {
    Customer& c = data->customers[customer_id - 1];
    if (c.id == -1) return -1;
    int bill = c.bill;
    c.id = -1;
    c.bill = 0;
    return bill;
}

TX static void make_reservation_tx(VacationData* data, int customer_id) {
    add_customer(data, customer_id);

    PRNG rng(rdtsc() + customer_id);
    int types[3] = {0, 1, 2};
    int ids[3];
    int best_prices[3] = {-1, -1, -1};
    int best_ids[3] = {-1, -1, -1};
    bool found = false;

    int nq = (int)(rng.next() % data->num_queries_per_tx) + 1;
    for (int i = 0; i < nq; i++) {
        int t = (int)(rng.next() % 3);
        int id = (int)(rng.next() % data->query_range) + 1;

        Reservation* table = nullptr;
        if (t == 0) table = data->cars;
        else if (t == 1) table = data->flights;
        else table = data->rooms;

        int avail = query_reservation(table, id);
        if (avail > 0) {
            int price = query_price(table, id);
            if (price > best_prices[t]) {
                best_prices[t] = price;
                best_ids[t] = id;
                found = true;
            }
        }
    }

    if (found) {
        for (int t = 0; t < 3; t++) {
            if (best_ids[t] > 0) {
                Reservation* table = nullptr;
                if (t == 0) table = data->cars;
                else if (t == 1) table = data->flights;
                else table = data->rooms;

                int p = make_reservation(table, best_ids[t]);
                if (p >= 0) {
                    data->customers[customer_id - 1].bill += p;
                }
            }
        }
    }
}

TX static void delete_customer_tx(VacationData* data, int customer_id) {
    Customer& c = data->customers[customer_id - 1];
    if (c.id != -1) {
        c.id = -1;
        c.bill = 0;
    }
}

TX static void update_tables_tx(VacationData* data) {
    PRNG rng(rdtsc());
    int type = (int)(rng.next() % 3);
    int id = (int)(rng.next() % data->query_range) + 1;
    int op = (int)(rng.next() % 2);

    Reservation* table = nullptr;
    if (type == 0) table = data->cars;
    else if (type == 1) table = data->flights;
    else table = data->rooms;

    if (op == 1) {
        int price = (int)(rng.next() % 5) * 10 + 50;
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
        int r = (int)(rng.next() % 100);
        int customer_id = (int)(rng.next() % data->query_range) + 1;

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
