// Vacation — C++ port of the original STAMP spec (explicit API path)
// Original spec: https://github.com/ccaominh/stamp/tree/master/vacation
//
// Parameters (matching original spec):
//   -n <num>   Queries per transaction  (default: 2)
//   -q <pct>   Percent relations queried (default: 90)
//   -r <num>   Number of relations       (default: 16384)
//   -u <pct>   Percent user transactions (default: 98)
//   -t <num>   Total transactions        (default: 4096)

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <csetjmp>
#include <cstdint>
#include <thread>
#include <vector>
#include <chrono>
#include <atomic>

static long g_queries_per_tx      = 2;
static long g_pct_query           = 90;
static long g_num_relations       = 16384;
static long g_pct_user            = 98;
static long g_num_transactions    = 4096;
static long g_num_threads         = 4;

// ── TM abstraction (expli only) ────────────────────────────────────
#include <csetjmp>

extern "C" {
    void     tm_begin();
    void     tm_end();
    long     tm_read_i8(const long*);
    void     tm_write_i8(long*, long);
    void     tm_init();
    void     tm_exit();
    void     tm_init_thread();
    void     tm_exit_thread();
    void*    tm_calloc(size_t, size_t);
}
extern __thread int32_t tm_nested_call_counter;
extern __thread sigjmp_buf tm_jmpbuf;

#define TX_FUNC
#define TM_READ_I8(p)     tm_read_i8((const long*)(p))
#define TM_WRITE_I8(p, v) tm_write_i8((long*)(p), (long)(v))

template<typename F>
static void tx_run(F&& body) {
    volatile bool done = false;
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

// ── RNG (LCG matching original STAMP) ──────────────────────────────
static thread_local uint32_t tls_rng_state = 1;

static void rng_seed(unsigned s) { tls_rng_state = s ? s : 1; }
static uint32_t rng_next() {
    tls_rng_state = tls_rng_state * 1103515245 + 12345;
    return tls_rng_state & 0x7fffffff;
}

// ── Table storage ──────────────────────────────────────────────────
// Flat TM arrays indexed by id (1-based). Sentinel -1 = non-existent.
static long* g_car_price   = nullptr;
static long* g_car_count   = nullptr;
static long* g_flight_price= nullptr;
static long* g_flight_count= nullptr;
static long* g_room_price  = nullptr;
static long* g_room_count  = nullptr;
static long* g_customer_bill = nullptr;

enum TableType { CAR = 0, FLIGHT = 1, ROOM = 2 };

static long* price_of(TableType t) {
    return t == CAR ? g_car_price : (t == FLIGHT ? g_flight_price : g_room_price);
}
static long* count_of(TableType t) {
    return t == CAR ? g_car_count : (t == FLIGHT ? g_flight_count : g_room_count);
}

// ── Spec-compliant transaction helpers (TX_FUNC = empty for expli) ─

TX_FUNC static long tx_read_price(TableType t, long id) {
    return TM_READ_I8(&price_of(t)[id]);
}

TX_FUNC static long tx_read_count(TableType t, long id) {
    return TM_READ_I8(&count_of(t)[id]);
}

TX_FUNC static void tx_write_count(TableType t, long id, long val) {
    TM_WRITE_I8(&count_of(t)[id], val);
}

TX_FUNC static long tx_read_bill(long customer_id) {
    return TM_READ_I8(&g_customer_bill[customer_id]);
}

TX_FUNC static void tx_write_bill(long customer_id, long val) {
    TM_WRITE_I8(&g_customer_bill[customer_id], val);
}

// make_reservation: decrement count, return price (or -1 if unavailable)
TX_FUNC static long tx_make_reservation(TableType t, long id) {
    long c = tx_read_count(t, id);
    if (c <= 0) return -1;
    tx_write_count(t, id, c - 1);
    return tx_read_price(t, id);
}

TX_FUNC static bool tx_add_reservation(TableType t, long id, long num, long price) {
    long c = tx_read_count(t, id);
    if (c < 0) c = 0;
    tx_write_count(t, id, c + num);
    if (price >= 0) TM_WRITE_I8(&price_of(t)[id], price);
    return true;
}

TX_FUNC static bool tx_delete_reservation(TableType t, long id, long num) {
    long c = tx_read_count(t, id);
    if (c < num) return false;
    tx_write_count(t, id, c - num);
    return true;
}

TX_FUNC static void tx_add_customer(long customer_id) {
    long b = tx_read_bill(customer_id);
    if (b < 0) tx_write_bill(customer_id, 0);
}

TX_FUNC static long tx_delete_customer(long customer_id) {
    long b = tx_read_bill(customer_id);
    if (b < 0) return -1;
    tx_write_bill(customer_id, -1);
    return b;
}

// ── Spec-compliant user transaction ────────────────────────────────
// Query N random items (N = 1..num_queries_per_tx), for each table type
// track the best-priced available item, then reserve the best of each type.
TX_FUNC static void tx_make_reservation_tx(long customer_id, long num_queries_per_tx,
                                            long query_range) {
    tx_add_customer(customer_id);

    long best_prices[3] = {-1, -1, -1};
    long best_ids[3]    = {-1, -1, -1};
    bool found = false;

    long nq = (long)(rng_next() % (uint32_t)num_queries_per_tx) + 1;
    for (long i = 0; i < nq; i++) {
        TableType t = (TableType)(rng_next() % 3);
        long id = (long)(rng_next() % (uint32_t)query_range) + 1;

        long avail = tx_read_count(t, id);
        if (avail > 0) {
            long price = tx_read_price(t, id);
            if (price > best_prices[(int)t]) {
                best_prices[(int)t] = price;
                best_ids[(int)t] = id;
                found = true;
            }
        }
    }

    if (found) {
        for (int t = 0; t < 3; t++) {
            if (best_ids[t] > 0) {
                long p = tx_make_reservation((TableType)t, best_ids[t]);
                if (p >= 0) {
                    long bill = tx_read_bill(customer_id);
                    tx_write_bill(customer_id, bill + p);
                }
            }
        }
    }
}

// ── Spec-compliant admin transactions ──────────────────────────────
// delete_customer_tx: erase customer record
TX_FUNC static long tx_delete_customer_tx(long customer_id) {
    return tx_delete_customer(customer_id);
}

// update_tables_tx: add or delete 100 units of a random resource
TX_FUNC static void tx_update_tables_tx(long query_range) {
    TableType t = (TableType)(rng_next() % 3);
    long id = (long)(rng_next() % (uint32_t)query_range) + 1;
    long op = (long)(rng_next() % 2);

    if (op == 1) {
        long price = (long)(rng_next() % 5) * 10 + 50;
        tx_add_reservation(t, id, 100, price);
    } else {
        tx_delete_reservation(t, id, 100);
    }
}

// ── Worker thread ──────────────────────────────────────────────────
static void worker(long thread_id) {
    rng_seed(42 + (unsigned)thread_id);
    tm_init_thread();

    long query_range = (long)((double)g_pct_query / 100.0 * g_num_relations + 0.5);
    long tasks_per_thread = g_num_transactions / g_num_threads;
    long extra = g_num_transactions % g_num_threads;
    long start = thread_id * tasks_per_thread + (thread_id < extra ? thread_id : extra);
    long end = start + tasks_per_thread + (thread_id < extra ? 1 : 0);

    for (long iter = start; iter < end; iter++) {
        long r = (long)(rng_next() % 100);
        long customer_id = (long)(rng_next() % (uint32_t)query_range) + 1;

        if (r < g_pct_user) {
            tx_run([&]() {
                tx_make_reservation_tx(customer_id, g_queries_per_tx, query_range);
            });
        } else {
            long sub = (long)(rng_next() % 2);
            if (sub == 0) {
                tx_run([&]() { tx_delete_customer_tx(customer_id); });
            } else {
                tx_run([&]() { tx_update_tables_tx(query_range); });
            }
        }
    }
}

// ── Main ───────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    for (int i = 1; i < argc; i++) {
        if      (strcmp(argv[i], "-n") == 0 && i+1 < argc)
            g_queries_per_tx = atol(argv[++i]);
        else if (strcmp(argv[i], "-q") == 0 && i+1 < argc)
            g_pct_query = atol(argv[++i]);
        else if (strcmp(argv[i], "-r") == 0 && i+1 < argc)
            g_num_relations = atol(argv[++i]);
        else if (strcmp(argv[i], "-u") == 0 && i+1 < argc)
            g_pct_user = atol(argv[++i]);
        else if (strcmp(argv[i], "-t") == 0 && i+1 < argc)
            g_num_transactions = atol(argv[++i]);
        else if (strcmp(argv[i], "-c") == 0 && i+1 < argc)
            g_num_threads = atol(argv[++i]);
    }

    printf("Vacation (STAMP spec)\n");
    printf("  Relations:       %ld\n", g_num_relations);
    printf("  Queries/tx:      %ld\n", g_queries_per_tx);
    printf("  Query %%:         %ld\n", g_pct_query);
    printf("  User %%:          %ld\n", g_pct_user);
    printf("  Transactions:    %ld\n", g_num_transactions);
    printf("  Threads:         %ld\n", g_num_threads);

    tm_init();

    // Allocate tables (+1 for 1-based indexing)
    long max_id = g_num_relations + 1;
    g_car_price    = (long*)tm_calloc(max_id, sizeof(long));
    g_car_count    = (long*)tm_calloc(max_id, sizeof(long));
    g_flight_price = (long*)tm_calloc(max_id, sizeof(long));
    g_flight_count = (long*)tm_calloc(max_id, sizeof(long));
    g_room_price   = (long*)tm_calloc(max_id, sizeof(long));
    g_room_count   = (long*)tm_calloc(max_id, sizeof(long));
    g_customer_bill= (long*)tm_calloc(max_id, sizeof(long));

    // Initialize to -1 (non-existent)
    for (long i = 0; i < max_id; i++) {
        g_car_price[i] = -1;
        g_flight_price[i] = -1;
        g_room_price[i] = -1;
        g_customer_bill[i] = -1;
    }

    // Generate prices (matching plugin spec: rng seeded 42)
    rng_seed(42);
    for (long i = 1; i <= g_num_relations; i++) {
        long num = (long)(rng_next() % 5 + 1) * 100;
        long price = (long)(rng_next() % 5) * 10 + 50;
        g_car_price[i] = price;  g_car_count[i] = num;
        price = (long)(rng_next() % 5) * 10 + 50;
        g_flight_price[i] = price; g_flight_count[i] = num;
        price = (long)(rng_next() % 5) * 10 + 50;
        g_room_price[i] = price;  g_room_count[i] = num;
    }
    printf("  Initialized %ld relations x 3 tables\n", g_num_relations);

    auto t1 = std::chrono::steady_clock::now();

    std::vector<std::thread> threads;
    for (long i = 0; i < g_num_threads; i++)
        threads.emplace_back(worker, i);
    for (auto& t : threads) t.join();

    auto t2 = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(t2 - t1).count();

    printf("\nResults:\n");
    printf("  Time:   %.6f sec\n", elapsed);
    printf("  Rate:   %.0f txns/sec\n", g_num_transactions / elapsed);

    tm_exit();
    return 0;
}
