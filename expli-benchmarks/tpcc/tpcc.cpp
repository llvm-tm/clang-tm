#include "expli_tm_api/tm_api.hpp"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

// ── Constants ──────────────────────────────────────────────────────────
static const int DEFAULT_WAREHOUSES = 1;
static const int DEFAULT_DISTRICTS = 10;
static const int DEFAULT_CUSTOMERS = 3000;
static const int DEFAULT_ITEMS = 100000;
static const int MAX_ORDERS_PER_DISTRICT = 10000;
static const int MAX_OL_PER_ORDER = 15;

// ── RNG ────────────────────────────────────────────────────────────────
struct Rng {
    uint64_t state;
    explicit Rng(uint64_t seed) : state(seed) {}
    uint64_t next() {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        return state >> 33;
    }
    uint64_t range(uint64_t lo, uint64_t hi) { return lo + next() % (hi - lo); }
};

// ── Table structs (TM<T> has no value ctor — use poke() after default init) ──
struct Warehouse {
    int w_id;
    expli::TM<double> w_tax;
    expli::TM<double> w_ytd;
    Warehouse(int id) : w_id(id) { w_tax.poke(0.19); w_ytd.poke(3000000.0); }
};

struct District {
    int d_id, d_w_id;
    expli::TM<double> d_tax;
    expli::TM<double> d_ytd;
    expli::TM<int> d_next_o_id;
    District(int did, int wid) : d_id(did), d_w_id(wid) {
        d_tax.poke(0.15); d_ytd.poke(3000000.0); d_next_o_id.poke(2101);
    }
};

struct Customer {
    int c_id, c_d_id, c_w_id;
    expli::TM<double> c_credit_lim;
    expli::TM<double> c_discount;
    expli::TM<double> c_balance;
    expli::TM<double> c_ytd_payment;
    expli::TM<int> c_payment_cnt;
    expli::TM<int> c_delivery_cnt;
    Customer(int cid, int did, int wid) : c_id(cid), c_d_id(did), c_w_id(wid) {
        c_credit_lim.poke(50000.0); c_discount.poke(0.3);
        c_balance.poke(-10.0); c_ytd_payment.poke(10.0);
        c_payment_cnt.poke(1); c_delivery_cnt.poke(0);
    }
};

struct History {
    expli::TM<double> h_amount;
    History() { h_amount.poke(10.0); }
};

struct Order {
    int o_id, o_d_id, o_w_id, o_c_id;
    expli::TM<int> o_carrier_id;
    int o_ol_cnt;
    Order(int oid, int did, int wid, int cid, int olc)
        : o_id(oid), o_d_id(did), o_w_id(wid), o_c_id(cid), o_ol_cnt(olc) {
        o_carrier_id.poke(oid <= 2100 ? (oid % 10 + 1) : 0);
    }
};

struct NewOrder {
    expli::TM<int> no_o_id;
    expli::TM<int> no_d_id;
    expli::TM<int> no_w_id;
    NewOrder() { no_o_id.poke(-1); no_d_id.poke(-1); no_w_id.poke(-1); }
    NewOrder(int oid, int did, int wid) {
        no_o_id.poke(oid); no_d_id.poke(did); no_w_id.poke(wid);
    }
};

struct OrderLine {
    int ol_o_id, ol_d_id, ol_w_id, ol_number, ol_i_id;
    expli::TM<double> ol_amount;
    expli::TM<int> ol_delivery_d;
    OrderLine(int oid, int did, int wid, int num, int iid, double amt, int dd)
        : ol_o_id(oid), ol_d_id(did), ol_w_id(wid), ol_number(num), ol_i_id(iid) {
        ol_amount.poke(amt); ol_delivery_d.poke(dd);
    }
};

struct Item {
    int i_id;
    double i_price;
    Item(int id, double price) : i_id(id), i_price(price) {}
};

struct Stock {
    int s_i_id, s_w_id;
    expli::TM<int> s_quantity;
    expli::TM<int> s_ytd;
    expli::TM<int> s_order_cnt;
    expli::TM<int> s_remote_cnt;
    Stock(int iid, int wid) : s_i_id(iid), s_w_id(wid) {
        s_quantity.poke(100); s_ytd.poke(0); s_order_cnt.poke(0); s_remote_cnt.poke(0);
    }
};

// ── Database ───────────────────────────────────────────────────────────
struct TpccDatabase {
    int num_w, num_d, num_c, num_i;
    std::vector<Warehouse> warehouse;
    std::vector<District> district;
    std::vector<Customer> customer;
    std::vector<History> history;
    std::vector<Order> order;
    std::vector<NewOrder> neworder;
    std::vector<OrderLine> orderline;
    std::vector<Item> item;
    std::vector<Stock> stock;

    TpccDatabase(int w);
    int idx_w(int w) const { return w - 1; }
    int idx_d(int w, int d) const { return (w - 1) * num_d + (d - 1); }
    int idx_c(int w, int d, int c) const {
        return ((w - 1) * num_d + (d - 1)) * num_c + (c - 1);
    }
    int idx_ord(int w, int d, int o) const {
        return ((w - 1) * num_d + (d - 1)) * MAX_ORDERS_PER_DISTRICT + (o - 1);
    }
    int idx_no(int w, int d, int o) const { return idx_ord(w, d, o); }
    int idx_ol(int w, int d, int o, int l) const {
        return (((w - 1) * num_d + (d - 1)) * MAX_ORDERS_PER_DISTRICT + (o - 1))
               * MAX_OL_PER_ORDER + (l - 1);
    }
    int idx_i(int i) const { return i - 1; }
    int idx_s(int w, int i) const { return (w - 1) * num_i + (i - 1); }
};

TpccDatabase::TpccDatabase(int w) : num_w(w), num_d(DEFAULT_DISTRICTS),
    num_c(DEFAULT_CUSTOMERS), num_i(DEFAULT_ITEMS)
{
    int total_orders = num_w * num_d * MAX_ORDERS_PER_DISTRICT;
    warehouse.reserve(num_w);
    for (int i = 0; i < num_w; ++i)
        warehouse.emplace_back(i + 1);

    district.reserve(num_w * num_d);
    for (int i = 0; i < num_w * num_d; ++i)
        district.emplace_back(i % num_d + 1, i / num_d + 1);

    customer.reserve(num_w * num_d * num_c);
    for (int i = 0; i < num_w * num_d * num_c; ++i) {
        int cid = i % num_c + 1;
        int did = (i / num_c) % num_d + 1;
        int wid = i / (num_c * num_d) + 1;
        customer.emplace_back(cid, did, wid);
    }

    history.resize(total_orders > 100000 ? total_orders : 100000);

    order.reserve(total_orders);
    for (int i = 0; i < total_orders; ++i) {
        int oid = i % MAX_ORDERS_PER_DISTRICT + 1;
        int did = (i / MAX_ORDERS_PER_DISTRICT) % num_d + 1;
        int wid = i / (MAX_ORDERS_PER_DISTRICT * num_d) + 1;
        int cid = oid % 3000 + 1;
        order.emplace_back(oid, did, wid, cid, oid % 11 + 5);
    }

    neworder.reserve(total_orders);
    for (int i = 0; i < total_orders; ++i) {
        int oid = i % MAX_ORDERS_PER_DISTRICT + 1;
        if (oid > 2100) {
            int did = (i / MAX_ORDERS_PER_DISTRICT) % num_d + 1;
            int wid = i / (MAX_ORDERS_PER_DISTRICT * num_d) + 1;
            neworder.emplace_back(oid, did, wid);
        } else {
            neworder.emplace_back();
        }
    }

    int total_ol = total_orders * MAX_OL_PER_ORDER;
    orderline.reserve(total_ol);
    for (int i = 0; i < total_ol; ++i) {
        int o_idx = i / MAX_OL_PER_ORDER;
        int num = i % MAX_OL_PER_ORDER + 1;
        int oid = o_idx % MAX_ORDERS_PER_DISTRICT + 1;
        int did = (o_idx / MAX_ORDERS_PER_DISTRICT) % num_d + 1;
        int wid = o_idx / (MAX_ORDERS_PER_DISTRICT * num_d) + 1;
        int carrier = oid <= 2100 ? 1000 : 0;
        double amt = (double)oid * 5.0;
        orderline.emplace_back(oid, did, wid, num, o_idx % num_i + 1, amt, carrier);
    }

    item.reserve(num_i);
    for (int i = 0; i < num_i; ++i)
        item.emplace_back(i + 1, (double)(i % 100 + 1));

    stock.reserve(num_w * num_i);
    for (int i = 0; i < num_w * num_i; ++i)
        stock.emplace_back(i % num_i + 1, i / num_i + 1);
}

// ── Transaction types ──────────────────────────────────────────────────
int txn_new_order(TpccDatabase &db, int w_id, int d_id, Rng &rng) {
    int d_idx = db.idx_d(w_id, d_id);
    int next_o_id = db.district[d_idx].d_next_o_id.read();
    db.district[d_idx].d_next_o_id.write(next_o_id + 1);
    int o_id = next_o_id;
    int num_items = (int)rng.range(5, 16);
    int o_idx = db.idx_ord(w_id, d_id, o_id);

    db.order[o_idx].o_carrier_id.write(0);

    int no_idx = db.idx_no(w_id, d_id, o_id);
    db.neworder[no_idx].no_o_id.write(o_id);
    db.neworder[no_idx].no_d_id.write(d_id);
    db.neworder[no_idx].no_w_id.write(w_id);

    for (int ol_num = 1; ol_num <= num_items; ++ol_num) {
        int i_id = (int)rng.range(1, db.num_i + 1);
        int ol_idx = db.idx_ol(w_id, d_id, o_id, ol_num);
        double amount = db.item[db.idx_i(i_id)].i_price * 5.0;

        int s_idx = db.idx_s(w_id, i_id);
        int qty = db.stock[s_idx].s_quantity.read();
        db.stock[s_idx].s_quantity.write(qty >= 5 ? qty - 5 : qty - 5 + 91);
        db.stock[s_idx].s_ytd.write(db.stock[s_idx].s_ytd.read() + 5);
        db.stock[s_idx].s_order_cnt.write(db.stock[s_idx].s_order_cnt.read() + 1);

        db.orderline[ol_idx].ol_amount.write(amount);
        db.orderline[ol_idx].ol_delivery_d.write(0);
    }
    return next_o_id;
}

void txn_payment(TpccDatabase &db, int w_id, int d_id, int c_id, double amount) {
    int w_idx = db.idx_w(w_id);
    db.warehouse[w_idx].w_ytd.write(db.warehouse[w_idx].w_ytd.read() + amount);

    int d_idx = db.idx_d(w_id, d_id);
    db.district[d_idx].d_ytd.write(db.district[d_idx].d_ytd.read() + amount);

    int c_idx = db.idx_c(w_id, d_id, c_id);
    db.customer[c_idx].c_balance.write(
        db.customer[c_idx].c_balance.read() - amount);
    db.customer[c_idx].c_ytd_payment.write(
        db.customer[c_idx].c_ytd_payment.read() + amount);
    db.customer[c_idx].c_payment_cnt.write(
        db.customer[c_idx].c_payment_cnt.read() + 1);
}

double txn_order_status(TpccDatabase &db, int w_id, int d_id, int c_id) {
    int c_idx = db.idx_c(w_id, d_id, c_id);
    return db.customer[c_idx].c_balance.read();
}

void txn_delivery(TpccDatabase &db, int w_id, int carrier_id) {
    for (int d_id = 1; d_id <= db.num_d; ++d_id) {
        int found_o_id = -1;
        for (int o_id = 2101; o_id < MAX_ORDERS_PER_DISTRICT; ++o_id) {
            int no_idx = db.idx_no(w_id, d_id, o_id);
            if (db.neworder[no_idx].no_o_id.read() == o_id) {
                found_o_id = o_id;
                break;
            }
        }
        if (found_o_id < 0) continue;
        int o_id = found_o_id;

        int o_idx = db.idx_ord(w_id, d_id, o_id);
        db.order[o_idx].o_carrier_id.write(carrier_id);
        int ol_cnt = db.order[o_idx].o_ol_cnt;

        double ol_total = 0.0;
        for (int l = 1; l <= ol_cnt; ++l) {
            int ol_idx = db.idx_ol(w_id, d_id, o_id, l);
            ol_total += db.orderline[ol_idx].ol_amount.read();
            db.orderline[ol_idx].ol_delivery_d.write(1000);
        }

        int c_id = db.order[o_idx].o_c_id;
        int c_idx = db.idx_c(w_id, d_id, c_id);
        db.customer[c_idx].c_balance.write(
            db.customer[c_idx].c_balance.read() + ol_total);
        db.customer[c_idx].c_delivery_cnt.write(
            db.customer[c_idx].c_delivery_cnt.read() + 1);

        db.neworder[db.idx_no(w_id, d_id, o_id)].no_o_id.write(-1);
    }
}

int txn_stock_level(TpccDatabase &db, int w_id, int d_id, int threshold) {
    int d_idx = db.idx_d(w_id, d_id);
    int next_o_id = db.district[d_idx].d_next_o_id.read();
    int start = next_o_id > 20 ? next_o_id - 20 : 1;

    int below = 0;
    // Track seen item IDs (non-TM set)
    bool seen[100001] = {false};
    for (int o_id = start; o_id < next_o_id; ++o_id) {
        int o_idx = db.idx_ord(w_id, d_id, o_id);
        int ol_cnt = db.order[o_idx].o_ol_cnt;
        for (int l = 1; l <= ol_cnt; ++l) {
            int ol_idx = db.idx_ol(w_id, d_id, o_id, l);
            int i_id = db.orderline[ol_idx].ol_i_id;
            if (i_id < 1 || i_id > db.num_i) continue;
            if (!seen[i_id]) {
                seen[i_id] = true;
                int qty = db.stock[db.idx_s(w_id, i_id)].s_quantity.read();
                if (qty < threshold) ++below;
            }
        }
    }
    return below;
}

// ── Globals ────────────────────────────────────────────────────────────
std::atomic<bool> g_stop{false};
std::atomic<uint64_t> g_total_ops{0};
std::atomic<uint64_t> g_tx_counts[5] = {};
TpccDatabase *g_db = nullptr;

// ── Worker ─────────────────────────────────────────────────────────────
void run_worker(int tid) {
    expli::TM<int>::thread_init();
    Rng rng(tid * 12345ULL + 42);

    while (!g_stop.load()) {
        int r = (int)(rng.next() % 100);
        int w_id = (int)rng.range(1, g_db->num_w + 1);
        int d_id = (int)rng.range(1, g_db->num_d + 1);
        int c_id = (int)rng.range(1, g_db->num_c + 1);

        if (r < 45) {
            expli::TM<int>::transaction([&]() {
                txn_new_order(*g_db, w_id, d_id, rng);
            });
            g_tx_counts[0].fetch_add(1);
        } else if (r < 88) {
            double amount = 100.0 + (double)(rng.next() % 9900);
            expli::TM<int>::transaction([&]() {
                txn_payment(*g_db, w_id, d_id, c_id, amount);
            });
            g_tx_counts[1].fetch_add(1);
        } else if (r < 92) {
            expli::TM<int>::transaction([&]() {
                txn_order_status(*g_db, w_id, d_id, c_id);
            });
            g_tx_counts[2].fetch_add(1);
        } else if (r < 96) {
            int carrier = (int)(rng.next() % 10) + 1;
            expli::TM<int>::transaction([&]() {
                txn_delivery(*g_db, w_id, carrier);
            });
            g_tx_counts[3].fetch_add(1);
        } else {
            int threshold = (int)rng.range(10, 21);
            expli::TM<int>::transaction([&]() {
                txn_stock_level(*g_db, w_id, d_id, threshold);
            });
            g_tx_counts[4].fetch_add(1);
        }
        g_total_ops.fetch_add(1);
    }
    expli::TM<int>::thread_exit();
}

// ── Main ───────────────────────────────────────────────────────────────
int main(int argc, char *argv[]) {
    int threads = 4, duration = 10000, warehouses = DEFAULT_WAREHOUSES;

    for (int i = 1; i < argc; ++i) {
        if      (!strcmp(argv[i], "-t") && i+1 < argc) threads = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-d") && i+1 < argc) duration = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-w") && i+1 < argc) warehouses = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            printf("Usage: tpcc [-t n] [-d ms] [-w n]\n");
            return 0;
        }
    }

    printf("========= TPC-C Benchmark (v5.11) =========\n");
    printf("============================================\n");
    printf("Warehouses: %d\n", warehouses);
    printf("Districts:  %d\n", warehouses * DEFAULT_DISTRICTS);
    printf("Customers:  %d\n", warehouses * DEFAULT_DISTRICTS * DEFAULT_CUSTOMERS);
    printf("Items:      %d\n", DEFAULT_ITEMS);
    printf("Threads:    %d\n", threads);
    printf("Duration:   %dms\n\n", duration);

    expli::TM<int>::init();
    g_db = new TpccDatabase(warehouses);

    std::vector<std::thread> thrds;
    for (int t = 0; t < threads; ++t)
        thrds.emplace_back([t]() { run_worker(t); });

    std::this_thread::sleep_for(std::chrono::milliseconds(duration));
    g_stop.store(true);
    for (auto &th : thrds) th.join();

    uint64_t ops = g_total_ops.load();
    double secs = duration / 1000.0;

    printf("\nResults\n");
    printf("=======\n");
    printf("Elapsed:  %.1fs\n", secs);
    printf("Total ops: %llu\n", (unsigned long long)ops);
    printf("Throughput: %.0f txn/s\n", ops / secs);
    printf("\nTransaction breakdown:\n");
    printf("  New-Order:    %llu\n", (unsigned long long)g_tx_counts[0].load());
    printf("  Payment:      %llu\n", (unsigned long long)g_tx_counts[1].load());
    printf("  Order-Status: %llu\n", (unsigned long long)g_tx_counts[2].load());
    printf("  Delivery:     %llu\n", (unsigned long long)g_tx_counts[3].load());
    printf("  Stock-Level:  %llu\n", (unsigned long long)g_tx_counts[4].load());

    delete g_db;
    expli::TM<int>::exit();
    return 0;
}
