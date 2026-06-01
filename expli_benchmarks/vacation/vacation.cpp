// Simplified STAMP/vacation benchmark using the explicit TM API.
//
// Models a travel reservation system with 3 resource types (car, flight,
// hotel) and customers.  Each TX reserves or cancels a resource for a
// customer.  Invariant: total reservations = sum of all per-type counts.
//
// Build:
//   make -C expli_benchmarks BACKEND=TINYSTM run-vacation
// or manually:
//   clang++ -std=c++20 -O3 -march=native -pthread -I.. \
//       -DTM_BACKEND_TINYSTM -DDESIGN_WBCTL -I../backends -I../backends/TinySTM \
//       expli_benchmarks/vacation/vacation.cpp \
//       backends/runtimes/TinySTM_runtime.cpp

#include "../../expli_tm_api/tm_api.hpp"

#include <atomic>
#include <chrono>
#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <thread>
#include <vector>

// Retry loop for explicit TM API (bypasses TM<T>::begin/end nesting issue)
template <typename F>
inline void tx_retry(F&& body) {
    tm_nested_call_counter++;
    int done = 0;
    while (!done) {
        tm_longjmp_ret = sigsetjmp(tm_jmpbuf, 0);
        tm_begin();
        if (tm_longjmp_ret != 0)
            continue;
        body();
        tm_end();
        done = 1;
    }
    tm_nested_call_counter--;
}

// ── Resources ───────────────────────────────────────────────
enum ResType { CAR = 0, FLIGHT = 1, HOTEL = 2, NUM_TYPES = 3 };

static const char *kTypeNames[] = {"car", "flight", "hotel"};

// Each resource has an id, a type, and a TM-tracked price and quantity.
struct Resource {
    int id;
    int type;
    expli::TM<int> price;
    expli::TM<int> quantity;
};

// Each customer has an id and a TM-tracked reservation count (total
// number of items reserved across all resource types).
struct Customer {
    int id;
    expli::TM<int> reservations;
};

static std::vector<Resource>  g_resources;
static std::vector<Customer>  g_customers;
static int g_num_resources_per_type;
static int g_num_customers;

// ── TX operations ───────────────────────────────────────────
// Reserve a resource for a customer (if quantity > 0).
// Returns 1 if successful, 0 if sold out.
static int op_reserve(int cust_id, int res_id) {
    int result = 0;
    tx_retry([&]() {
        int qty = g_resources[res_id].quantity.read();
        if (qty > 0) {
            g_resources[res_id].quantity.write(qty - 1);
            int cur = g_customers[cust_id].reservations.read();
            g_customers[cust_id].reservations.write(cur + 1);
            result = 1;
        }
    });
    return result;
}

// Cancel a reservation for a customer (if they have any).
// Returns 1 if successful, 0 if no reservation to cancel.
static int op_cancel(int cust_id, int res_id) {
    int result = 0;
    tx_retry([&]() {
        int cur = g_customers[cust_id].reservations.read();
        if (cur > 0) {
            g_customers[cust_id].reservations.write(cur - 1);
            int qty = g_resources[res_id].quantity.read();
            g_resources[res_id].quantity.write(qty + 1);
            result = 1;
        }
    });
    return result;
}

// Query: sum of all customer reservations (read-only).
static int op_query_total_reservations() {
    int total = 0;
    tx_retry([&]() {
        total = 0;
        for (int i = 0; i < g_num_customers; i++)
            total += g_customers[i].reservations.read();
    });
    return total;
}

// Query: total quantity of a resource type (read-only).
static int op_query_type_quantity(int type) {
    int total = 0;
    tx_retry([&]() {
        total = 0;
        int start = type * g_num_resources_per_type;
        int end   = start + g_num_resources_per_type;
        for (int i = start; i < end; i++)
            total += g_resources[i].quantity.read();
    });
    return total;
}

// ── Control ─────────────────────────────────────────────────
static std::atomic<bool> g_stop{false};

static std::atomic<uint64_t> g_reserve_ok{0};
static std::atomic<uint64_t> g_reserve_fail{0};
static std::atomic<uint64_t> g_cancel_ok{0};
static std::atomic<uint64_t> g_cancel_fail{0};
static std::atomic<uint64_t> g_queries{0};

static void worker(int seed) {
    expli::TM<int>::thread_init();
    auto rng = std::mt19937(seed);

    auto pick_resource = [&]() -> int {
        return rng() % (g_num_resources_per_type * NUM_TYPES);
    };
    auto pick_customer = [&]() -> int {
        return rng() % g_num_customers;
    };

    while (!g_stop.load()) {
        double roll = std::uniform_real_distribution<double>(0, 100.0)(rng);
        if (roll < 50.0) {
            // 50%: reserve a resource
            if (op_reserve(pick_customer(), pick_resource()))
                g_reserve_ok.fetch_add(1);
            else
                g_reserve_fail.fetch_add(1);
        } else if (roll < 80.0) {
            // 30%: cancel a reservation
            if (op_cancel(pick_customer(), pick_resource()))
                g_cancel_ok.fetch_add(1);
            else
                g_cancel_fail.fetch_add(1);
        } else if (roll < 95.0) {
            // 15%: query total reservations (read-only)
            op_query_total_reservations();
            g_queries.fetch_add(1);
        } else {
            // 5%: query type quantities (read-only)
            op_query_type_quantity(rng() % NUM_TYPES);
            g_queries.fetch_add(1);
        }
    }
    expli::TM<int>::thread_exit();
}

static int total_initial_quantity() {
    int total = 0;
    for (auto &r : g_resources)
        total += r.quantity.peek();
    return total;
}

static int total_current_quantity() {
    int total = 0;
    for (auto &r : g_resources)
        total += r.quantity.peek();
    return total;
}

static int total_customer_reservations() {
    int total = 0;
    for (auto &c : g_customers)
        total += c.reservations.peek();
    return total;
}

int main(int argc, char *argv[]) {
    int num_threads      = argc > 1 ? atoi(argv[1]) : 4;
    int duration_ms      = argc > 2 ? atoi(argv[2]) : 5000;
    g_num_resources_per_type = argc > 3 ? atoi(argv[3]) : 1000;
    g_num_customers      = argc > 4 ? atoi(argv[4]) : 100;
    unsigned seed        = argc > 5 ? atoi(argv[5]) : 42;

    printf("Vacation — Explicit TM API\n");
    printf("Threads: %d  Duration: %d ms  Resources/type: %d  Customers: %d\n",
           num_threads, duration_ms,
           g_num_resources_per_type, g_num_customers);

    expli::TM<int>::init();

    // Initialize resources
    auto rng_init = std::mt19937(seed);
    g_resources.reserve(g_num_resources_per_type * NUM_TYPES);
    for (int t = 0; t < NUM_TYPES; t++) {
        for (int i = 0; i < g_num_resources_per_type; i++) {
            int price    = (rng_init() % 1000) + 50;
            int quantity = (rng_init() % 100) + 1;
            g_resources.push_back({i + t * g_num_resources_per_type,
                                   t, {}, {}});
            g_resources.back().price.poke(price);
            g_resources.back().quantity.poke(quantity);
        }
    }

    // Initialize customers
    g_customers.reserve(g_num_customers);
    for (int i = 0; i < g_num_customers; i++) {
        g_customers.push_back({i, {}});
        g_customers.back().reservations.poke(0);
    }

    int init_qty = total_initial_quantity();
    printf("Initial total quantity: %d\n", init_qty);

    // Run workers
    auto start = std::chrono::high_resolution_clock::now();
    std::thread *threads = new std::thread[num_threads];
    for (int i = 0; i < num_threads; i++)
        new (&threads[i]) std::thread(worker, (unsigned)(1234 + i));

    std::this_thread::sleep_for(std::chrono::milliseconds(duration_ms));
    g_stop.store(true);
    for (int i = 0; i < num_threads; i++)
        threads[i].join();
    auto end = std::chrono::high_resolution_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       end - start).count();

    // Verify invariant: reservations + remaining quantity = initial quantity
    int rem_qty  = total_current_quantity();
    int cust_res = total_customer_reservations();
    int total    = rem_qty + cust_res;

    uint64_t ro = g_reserve_ok.load();
    uint64_t rf = g_reserve_fail.load();
    uint64_t co = g_cancel_ok.load();
    uint64_t cf = g_cancel_fail.load();
    uint64_t q  = g_queries.load();
    uint64_t txns = ro + rf + co + cf + q;

    printf("\nResults (%lld ms):\n", (long long)elapsed);
    printf("  Reserve OK: %llu  Fail: %llu\n",
           (unsigned long long)ro, (unsigned long long)rf);
    printf("  Cancel  OK: %llu  Fail: %llu\n",
           (unsigned long long)co, (unsigned long long)cf);
    printf("  Queries:    %llu\n", (unsigned long long)q);
    printf("  Total TXNs: %llu  TXNs/sec: %.0f\n",
           (unsigned long long)txns, txns * 1000.0 / elapsed);
    printf("  Remaining qty: %d  Customer reservations: %d  Total: %d\n",
           rem_qty, cust_res, total);

    if (total == init_qty) {
        printf("INVARIANT: quantity conservation: PASS (%d == %d)\n",
               total, init_qty);
    } else {
        printf("INVARIANT: quantity conservation: FAIL "
               "(got %d, expected %d, diff=%d)\n",
               total, init_qty, total - init_qty);
        delete[] threads;
        expli::TM<int>::exit();
        return 1;
    }

    printf("PASS\n");
    delete[] threads;
    expli::TM<int>::exit();
    return 0;
}
