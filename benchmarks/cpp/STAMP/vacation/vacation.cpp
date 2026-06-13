// STAMP/vacation benchmark — explicit TM API port
//
// Matches the plugin vacation_bench.hpp algorithm:
// multi-table travel reservation with best-price selection.
// Build: make -C expli-benchmarks BACKEND=TINYSTM run-vacation

#include "expli_tm_api/tm_api.hpp"
#include "../../tests/benchmark_test.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <thread>
#include <vector>
#include "../../include/scratch_set.hpp"
#include <chrono>
#include <atomic>

// ── Retry loop ─────────────────────────────────────────────
template <typename F>
inline void tx_retry(F&& body) {
    tm_nested_call_counter++;
    int done = 0;
    while (!done) {
        sigsetjmp(tm_jmpbuf, 0);
        tm_nested_call_counter = 1;
        tm_begin();
        body();
        tm_end();
        done = true;
    }
    tm_nested_call_counter = 0;
}

// ── RNG (matches plugin's FastPRNG<std::mt19937_64>) ───────
using PRNG = std::mt19937_64;

// ── Data structures ───────────────────────────────────────
struct Reservation {
    expli::TM<int> id;
    expli::TM<int> num_used;
    expli::TM<int> num_free;
    expli::TM<int> num_total;
    expli::TM<int> price;
    expli::TM<int> active;   // 1 if entry exists, 0 if erased
};

struct Customer {
    expli::TM<int> id;
    expli::TM<int> bill;
    expli::TM<int> active;   // 1 if exists, 0 otherwise
};

struct VacationData {
    ScratchVector<Reservation> cars;
    ScratchVector<Reservation> rooms;
    ScratchVector<Reservation> flights;
    ScratchVector<Customer> customers;
    int num_relations;
    int query_range;
    int num_queries_per_tx;
    int percent_user;
    int total_tasks;
    int num_threads;
};

static VacationData g_data;

// ── Table operations (inside TX) ──────────────────────────
static int query_num_free(ScratchVector<Reservation>* table, int id) {
    int idx = id - 1;
    if (idx < 0 || idx >= (int)table->size())
        return -1;
    Reservation& r = (*table)[idx];
    if (!r.active.read())
        return -1;
    return r.num_free.read();
}

static int query_price(ScratchVector<Reservation>* table, int id) {
    int idx = id - 1;
    if (idx < 0 || idx >= (int)table->size())
        return -1;
    Reservation& r = (*table)[idx];
    if (!r.active.read())
        return -1;
    return r.price.read();
}

static bool add_reservation(ScratchVector<Reservation>* table, int id, int num, int price) {
    int idx = id - 1;
    if (idx < 0 || idx >= (int)table->size())
        return false;
    Reservation& r = (*table)[idx];
    if (r.active.read()) {
        r.num_free.write(r.num_free.read() + num);
        r.num_total.write(r.num_total.read() + num);
        r.price.write(price);
        r.id.write(id);
    } else {
        r.active.write(1);
        r.id.write(id);
        r.num_used.write(0);
        r.num_free.write(num);
        r.num_total.write(num);
        r.price.write(price);
    }
    return true;
}

static bool delete_reservation(ScratchVector<Reservation>* table, int id, int num) {
    int idx = id - 1;
    if (idx < 0 || idx >= (int)table->size())
        return false;
    Reservation& r = (*table)[idx];
    if (!r.active.read())
        return false;
    if (r.num_free.read() < num)
        return false;
    r.num_free.write(r.num_free.read() - num);
    r.num_total.write(r.num_total.read() - num);
    if (r.num_total.read() == 0)
        r.active.write(0);
    return true;
}

static int make_reservation(ScratchVector<Reservation>* table, int id) {
    int idx = id - 1;
    if (idx < 0 || idx >= (int)table->size())
        return -1;
    Reservation& r = (*table)[idx];
    if (!r.active.read() || r.num_free.read() <= 0)
        return -1;
    r.num_used.write(r.num_used.read() + 1);
    r.num_free.write(r.num_free.read() - 1);
    return r.price.read();
}

static int cancel_reservation(ScratchVector<Reservation>* table, int id) {
    int idx = id - 1;
    if (idx < 0 || idx >= (int)table->size())
        return -1;
    Reservation& r = (*table)[idx];
    if (!r.active.read() || r.num_used.read() <= 0)
        return -1;
    r.num_used.write(r.num_used.read() - 1);
    r.num_free.write(r.num_free.read() + 1);
    return r.price.read();
}

static void add_customer(VacationData* data, int customer_id) {
    if (customer_id < 1 || customer_id > (int)data->customers.size())
        return;
    Customer& c = data->customers[customer_id - 1];
    if (!c.active.read()) {
        c.active.write(1);
        c.id.write(customer_id);
        c.bill.write(0);
    }
}

static int query_customer_bill(VacationData* data, int customer_id) {
    if (customer_id < 1 || customer_id > (int)data->customers.size())
        return -1;
    Customer& c = data->customers[customer_id - 1];
    if (!c.active.read())
        return -1;
    return c.bill.read();
}

static int delete_customer(VacationData* data, int customer_id) {
    if (customer_id < 1 || customer_id > (int)data->customers.size())
        return -1;
    Customer& c = data->customers[customer_id - 1];
    if (!c.active.read())
        return -1;
    int bill = c.bill.read();
    c.active.write(0);
    return bill;
}

// ── Transaction bodies ────────────────────────────────────
static void make_reservation_tx(VacationData* data, int customer_id) {
    tx_retry([data, customer_id]() {
        add_customer(data, customer_id);

        PRNG rng(std::chrono::steady_clock::now().time_since_epoch().count() + customer_id);
        int types[3] = {0, 1, 2};
        int ids[3];
        int best_prices[3] = {-1, -1, -1};
        int best_ids[3] = {-1, -1, -1};
        bool found = false;

        int nq = (int)(rng() % data->num_queries_per_tx) + 1;
        for (int i = 0; i < nq; i++) {
            int t = (int)(rng() % 3);
            int id = (int)(rng() % data->query_range) + 1;

            ScratchVector<Reservation>* table = nullptr;
            if (t == 0) table = &data->cars;
            else if (t == 1) table = &data->flights;
            else table = &data->rooms;

            int avail = query_num_free(table, id);
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
                    ScratchVector<Reservation>* table = nullptr;
                    if (t == 0) table = &data->cars;
                    else if (t == 1) table = &data->flights;
                    else table = &data->rooms;

                    int p = make_reservation(table, best_ids[t]);
                    if (p >= 0) {
                        Customer& c = data->customers[customer_id - 1];
                        c.bill.write(c.bill.read() + p);
                    }
                }
            }
        }
    });
}

static void delete_customer_tx(VacationData* data, int customer_id) {
    tx_retry([data, customer_id]() {
        if (customer_id >= 1 && customer_id <= (int)data->customers.size()) {
            Customer& c = data->customers[customer_id - 1];
            c.active.write(0);
        }
    });
}

static void update_tables_tx(VacationData* data) {
    tx_retry([data]() {
        PRNG rng(std::chrono::steady_clock::now().time_since_epoch().count());
        int type = (int)(rng() % 3);
        int id = (int)(rng() % data->query_range) + 1;
        int op = (int)(rng() % 2);

        ScratchVector<Reservation>* table = nullptr;
        if (type == 0) table = &data->cars;
        else if (type == 1) table = &data->flights;
        else table = &data->rooms;

        if (op == 1) {
            int price = (int)(rng() % 5) * 10 + 50;
            add_reservation(table, id, 100, price);
        } else {
            delete_reservation(table, id, 100);
        }
    });
}

// ── Worker thread ─────────────────────────────────────────
static std::atomic<uint64_t> g_total_ops{0};

static void worker(int thread_id, int num_threads, int total_tasks,
                   int query_range, int percent_user, VacationData* data) {
    expli::TM<int>::thread_init();

    PRNG rng(42 + thread_id);

    int tasks_per_thread = total_tasks / num_threads;
    int extra = total_tasks % num_threads;
    int start = thread_id * tasks_per_thread + std::min(thread_id, extra);
    int end = start + tasks_per_thread + (thread_id < extra ? 1 : 0);

    for (int iter = start; iter < end; iter++) {
        int r = (int)(rng() % 100);
        int customer_id = (int)(rng() % query_range) + 1;

        if (r < percent_user) {
            make_reservation_tx(data, customer_id);
        } else if (r % 2 == 0) {
            delete_customer_tx(data, customer_id);
        } else {
            update_tables_tx(data);
        }

        g_total_ops.fetch_add(1, std::memory_order_relaxed);
    }

    expli::TM<int>::thread_exit();
}

// ── Main ──────────────────────────────────────────────────
static void print_usage(const char* prog) {
    fprintf(stderr, "Usage: %s [-n <queries>] [-r <relations>] [-u <pct_user>] [-t <tasks>] "
                    "[-p <num_threads>]\n", prog);
}

static void parse_args(int argc, char* argv[], int& num_relations,
                       int& num_queries_per_tx, int& percent_user,
                       int& total_tasks, int& num_threads) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-n") == 0 && i + 1 < argc)
            num_queries_per_tx = atoi(argv[++i]);
        else if (strcmp(argv[i], "-r") == 0 && i + 1 < argc)
            num_relations = atoi(argv[++i]);
        else if (strcmp(argv[i], "-u") == 0 && i + 1 < argc)
            percent_user = atoi(argv[++i]);
        else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc)
            total_tasks = atoi(argv[++i]);
        else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc)
            num_threads = atoi(argv[++i]);
        else if (strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            exit(0);
        }
    }
}

static int test_cli_flags() {
    printf("  test_cli_flags...\n");
    {
        int nr = 16384, nq = 2, pct = 98, tasks = 4096, thr = 4;
        const char* fake[] = {"vacation"};
        parse_args(1, (char**)fake, nr, nq, pct, tasks, thr);
        TEST_EQ(nr, 16384, "default num_relations");
        TEST_EQ(nq, 2, "default num_queries_per_tx");
        TEST_EQ(pct, 98, "default percent_user");
        TEST_EQ(tasks, 4096, "default total_tasks");
        TEST_EQ(thr, 4, "default num_threads");
    }
    {
        int nr = 16384, nq = 2, pct = 98, tasks = 4096, thr = 4;
        const char* fake[] = {"vacation", "-n", "5", "-r", "100",
                              "-u", "50", "-t", "200", "-p", "8"};
        parse_args(11, (char**)fake, nr, nq, pct, tasks, thr);
        TEST_EQ(nr, 100, "override num_relations");
        TEST_EQ(nq, 5, "override num_queries_per_tx");
        TEST_EQ(pct, 50, "override percent_user");
        TEST_EQ(tasks, 200, "override total_tasks");
        TEST_EQ(thr, 8, "override num_threads");
    }
    return test_result();
}

static int test_rng() {
    printf("  test_rng...\n");
    test_rng_determinism<PRNG>();
    return test_result();
}

static int test_manager() {
    printf("  test_manager...\n");
    expli::TM<int>::init();
    expli::TM<int>::thread_init();

    VacationData data;
    int n = 10;
    data.num_relations = n;
    data.query_range = n;
    data.cars.reserve(n);
    for (int j = 1; j <= n; j++) {
        data.cars.push_back({});
        Reservation& r = data.cars.back();
        r.active.poke(0);
        r.id.poke(j);
        r.num_used.poke(0);
        r.num_free.poke(0);
        r.num_total.poke(0);
        r.price.poke(0);
    }

    int free = -1, price = -1;
    bool ok = false;
    tx_retry([&]() {
        ok = add_reservation(&data.cars, 1, 100, 200);
    });
    TEST_EQ(ok, true, "add car reservation");

    tx_retry([&]() {
        free = query_num_free(&data.cars, 1);
    });
    TEST_EQ(free, 100, "car num_free after add");

    tx_retry([&]() {
        price = query_price(&data.cars, 1);
    });
    TEST_EQ(price, 200, "car price after add");

    tx_retry([&]() {
        ok = delete_reservation(&data.cars, 1, 60);
    });
    TEST_EQ(ok, true, "delete partial reservation");

    tx_retry([&]() {
        free = query_num_free(&data.cars, 1);
    });
    TEST_EQ(free, 40, "car num_free after partial delete");

    tx_retry([&]() {
        ok = delete_reservation(&data.cars, 1, 40);
    });
    TEST_EQ(ok, true, "delete remaining reservation");

    TEST_EQ(data.cars[0].num_free.peek(), 0, "car num_free after full delete");
    TEST_EQ(data.cars[0].active.peek(), 0, "car inactive after all removed");

    expli::TM<int>::thread_exit();
    expli::TM<int>::exit();
    return test_result();
}

static int test_all() {
    printf("Running self-tests...\n");
    int fails = 0;
    fails += test_cli_flags();
    fails += test_rng();
    fails += test_manager();
    printf("Self-tests: %s\n", fails ? "FAILED" : "ALL PASSED");
    return fails;
}

int main(int argc, char* argv[]) {
    int num_relations = 16384;
    int num_queries_per_tx = 2;
    int percent_user = 98;
    int total_tasks = 4096;
    int num_threads = 4;

    if (argc > 1 && strcmp(argv[1], "--test") == 0) {
        int fails = test_all();
        return fails ? 1 : 0;
    }

    parse_args(argc, argv, num_relations, num_queries_per_tx,
               percent_user, total_tasks, num_threads);

    printf("Initializing manager... done.\n");
    printf("Initializing clients... done.\n");
    printf("    Relations = %i\n", num_relations);
    printf("    Transactions = %i\n", total_tasks);
    printf("    Queries/transaction = %i\n", num_queries_per_tx);
    printf("    Percent user = %i\n", percent_user);
    printf("Running clients...\n");
    fflush(stdout);

    expli::TM<int>::init();

    // Init data — matches plugin vacation_generate_prices()
    g_data.num_relations = num_relations;
    g_data.query_range = (int)(0.9 * num_relations);
    g_data.num_queries_per_tx = num_queries_per_tx;
    g_data.percent_user = percent_user;
    g_data.total_tasks = total_tasks;
    g_data.num_threads = num_threads;

    PRNG init_rng(42);
    {
        int n = num_relations;
        g_data.cars.reserve(n);
        g_data.rooms.reserve(n);
        g_data.flights.reserve(n);
        for (int j = 1; j <= n; j++) {
            int num = (int)(init_rng() % 5 + 1) * 100;
            int price_val = (int)(init_rng() % 5) * 10 + 50;
            for (auto* t : {&g_data.cars, &g_data.rooms, &g_data.flights}) {
                t->push_back({});
                Reservation& r = t->back();
                r.active.poke(1);
                r.id.poke(j);
                r.num_used.poke(0);
                r.num_free.poke(num);
                r.num_total.poke(num);
                r.price.poke(price_val);
            }
        }
    }
    g_data.customers.reserve(num_relations);
    for (int i = 0; i < num_relations; i++) {
        g_data.customers.push_back({});
        g_data.customers.back().active.poke(0);
        g_data.customers.back().id.poke(i + 1);
        g_data.customers.back().bill.poke(0);
    }

    // Run workers
    auto start_time = std::chrono::high_resolution_clock::now();
    std::vector<std::thread> threads;
    threads.reserve(num_threads);
    for (int i = 0; i < num_threads; i++) {
        threads.emplace_back(worker, i, num_threads, total_tasks,
                             g_data.query_range, percent_user, &g_data);
    }
    for (auto& t : threads)
        t.join();
    auto end_time = std::chrono::high_resolution_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       end_time - start_time).count();

    uint64_t ops = g_total_ops.load();

    printf("done.\n");
    printf("Time = %f\n", elapsed / 1000.0);
    printf("Checking tables... done.\n");
    printf("Total ops = %lu\n", (unsigned long)ops);

    expli::TM<int>::exit();
    return 0;
}
