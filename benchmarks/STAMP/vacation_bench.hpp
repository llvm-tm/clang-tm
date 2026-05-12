#pragma once

#include "stamp_common.hpp"
#include <algorithm>
#include <map>
#include <string>
#include <vector>

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
    std::map<int, Reservation> cars;
    std::map<int, Reservation> rooms;
    std::map<int, Reservation> flights;
    std::map<int, Customer> customers;
    int num_relations;
    int query_range;
    int num_queries_per_tx;
    int percent_user;
};

static VacationData* g_vacation = nullptr;

inline void vacation_generate_prices() {
    auto data = new VacationData();
    data->num_relations = 1 << 12;
    data->query_range = (int)(0.9 * data->num_relations);
    data->num_queries_per_tx = 10;
    data->percent_user = 80;

    PRNG rng(42);
    for (int i = 1; i <= data->num_relations; i++) {
        int num = (int)(rng.next() % 5 + 1) * 100;
        int price = (int)(rng.next() % 5) * 10 + 50;
        data->cars[i] = {i, 0, num, num, price};
        data->rooms[i] = {i, 0, num, num, price};
        data->flights[i] = {i, 0, num, num, price};
    }

    g_vacation = data;
}

TX static int query_reservation(std::map<int, Reservation>* table, int id) {
    auto it = table->find(id);
    if (it == table->end()) return -1;
    return it->second.num_free;
}

TX static int query_price(std::map<int, Reservation>* table, int id) {
    auto it = table->find(id);
    if (it == table->end()) return -1;
    return it->second.price;
}

TX static bool add_reservation(std::map<int, Reservation>* table, int id, int num, int price) {
    auto it = table->find(id);
    if (it != table->end()) {
        it->second.num_free += num;
        it->second.num_total += num;
        it->second.price = price;
    } else {
        (*table)[id] = {id, 0, num, num, price};
    }
    return true;
}

TX static bool delete_reservation(std::map<int, Reservation>* table, int id, int num) {
    auto it = table->find(id);
    if (it == table->end()) return false;
    if (it->second.num_free < num) return false;
    it->second.num_free -= num;
    it->second.num_total -= num;
    if (it->second.num_total == 0) {
        table->erase(it);
    }
    return true;
}

TX static int make_reservation(std::map<int, Reservation>* table, int id) {
    auto it = table->find(id);
    if (it == table->end() || it->second.num_free <= 0) return -1;
    it->second.num_used++;
    it->second.num_free--;
    return it->second.price;
}

TX static int cancel_reservation(std::map<int, Reservation>* table, int id) {
    auto it = table->find(id);
    if (it == table->end() || it->second.num_used <= 0) return -1;
    it->second.num_used--;
    it->second.num_free++;
    return it->second.price;
}

TX static void add_customer(VacationData* data, int customer_id) {
    if (data->customers.find(customer_id) == data->customers.end()) {
        data->customers[customer_id] = {customer_id, 0};
    }
}

TX static int query_customer_bill(VacationData* data, int customer_id) {
    auto it = data->customers.find(customer_id);
    if (it == data->customers.end()) return -1;
    return it->second.bill;
}

TX static int delete_customer(VacationData* data, int customer_id) {
    auto it = data->customers.find(customer_id);
    if (it == data->customers.end()) return -1;
    int bill = it->second.bill;
    data->customers.erase(it);
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

        std::map<int, Reservation>* table = nullptr;
        if (t == 0) table = &data->cars;
        else if (t == 1) table = &data->flights;
        else table = &data->rooms;

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
                std::map<int, Reservation>* table = nullptr;
                if (t == 0) table = &data->cars;
                else if (t == 1) table = &data->flights;
                else table = &data->rooms;

                int p = make_reservation(table, best_ids[t]);
                if (p >= 0) {
                    data->customers[customer_id].bill += p;
                }
            }
        }
    }
}

TX static void delete_customer_tx(VacationData* data, int customer_id) {
    auto it = data->customers.find(customer_id);
    if (it != data->customers.end()) {
        data->customers.erase(it);
    }
}

TX static void update_tables_tx(VacationData* data) {
    PRNG rng(rdtsc());
    int type = (int)(rng.next() % 3);
    int id = (int)(rng.next() % data->query_range) + 1;
    int op = (int)(rng.next() % 2);

    std::map<int, Reservation>* table = nullptr;
    if (type == 0) table = &data->cars;
    else if (type == 1) table = &data->flights;
    else table = &data->rooms;

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

    for (int iter = 0; iter < td->loops && !stop_workers; iter++) {
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
