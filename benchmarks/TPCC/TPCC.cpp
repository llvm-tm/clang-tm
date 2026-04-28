/**
 * TPC-C Benchmark - Full Specification Implementation
 *
 * Based on TPC-C Specification v5.11
 * https://www.tpc.org/tpc_documents_current_versions/pdf/tpc-c_v5.11.0.pdf
 *
 * 9 Tables:
 * - Warehouse (W)
 * - District (D)
 * - Customer (C)
 * - History (H)
 * - Orders (O)
 * - New-Order (NO)
 * - Order-Line (OL)
 * - Item (I)
 * - Stock (S)
 *
 * 5 Transaction Types:
 * - New-Order (45%) - create order with order lines
 * - Payment (43%) - update W, D, C + insert History
 * - Order-Status (4%) - lookup by customer name or ID
 * - Delivery (4%) - deliver oldest orders
 * - Stock-Level (4%) - count items below threshold
 */

#include <iostream>
#include <thread>
#include <vector>
#include <random>
#include <atomic>
#include <chrono>
#include <cstring>
#include <algorithm>
#include <ctime>
#include <cstdlib>

#define TM __attribute__((annotate("tm")))
#define TX __attribute__((annotate("transaction")))

constexpr int DEFAULT_WAREHOUSES = 1;
constexpr int DEFAULT_DISTRICTS = 10;
constexpr int DEFAULT_CUSTOMERS = 1000;
constexpr int DEFAULT_ITEMS = 100000;

constexpr int MAX_OL_QUANTITY = 999999;
constexpr int NAME_LENGTH = 16;
constexpr int ADDRESS_LENGTH = 40;
constexpr int HISTORY_LENGTH = 200;

struct Warehouse {
    int w_id;
    char w_name[11];
    char w_street_1[21];
    char w_street_2[21];
    char w_city[21];
    char w_state[3];
    char w_zip[10];
    float w_tax;
    float w_ytd;
};

struct District {
    int d_id;
    int d_w_id;
    char d_name[11];
    char d_street_1[21];
    char d_street_2[21];
    char d_city[21];
    char d_state[3];
    char d_zip[10];
    float d_tax;
    float d_ytd;
    int d_next_o_id;
};

struct Customer {
    int c_id;
    int c_d_id;
    int c_w_id;
    char c_first[17];
    char c_middle[3];
    char c_last[17];
    char c_street_1[21];
    char c_street_2[21];
    char c_city[21];
    char c_state[3];
    char c_zip[10];
    char c_phone[17];
    int c_since;
    char c_credit[3];
    float c_credit_lim;
    float c_discount;
    float c_balance;
    float c_ytd_payment;
    int c_payment_cnt;
    int c_delivery_cnt;
    char c_data[501];
};

struct History {
    int h_c_id;
    int h_c_d_id;
    int h_c_w_id;
    int h_d_id;
    int h_w_id;
    int h_date;
    float h_amount;
    char h_data[25];
};

struct Order {
    int o_id;
    int o_d_id;
    int o_w_id;
    int o_c_id;
    int o_entry_d;
    int o_carrier_id;
    int o_ol_cnt;
    float o_all_local;
};

struct NewOrder {
    int no_o_id;
    int no_d_id;
    int no_w_id;
};

struct OrderLine {
    int ol_o_id;
    int ol_d_id;
    int ol_w_id;
    int ol_number;
    int ol_i_id;
    int ol_supply_w_id;
    int ol_quantity;
    int ol_delivery_d;
    float ol_amount;
    char ol_dist_info[25];
};

struct Item {
    int i_id;
    int i_im_id;
    char i_name[25];
    float i_price;
    char i_data[51];
};

struct Stock {
    int s_i_id;
    int s_w_id;
    int s_quantity;
    char s_dist_01[25];
    char s_dist_02[25];
    char s_dist_03[25];
    char s_dist_04[25];
    char s_dist_05[25];
    char s_dist_06[25];
    char s_dist_07[25];
    char s_dist_08[25];
    char s_dist_09[25];
    char s_dist_10[25];
    int s_order_cnt;
    int s_remote_cnt;
    char s_data[51];
};

static const char* FIRST_NAMES[] = {
    "BARBAREE", "CHRISTIA", "DAVINDER", "EVELYNE", "FRANS",
    "GERARDO", "HEIKE", "INGRID", "JOAQUIM", "KATJA"
};

static const char* LAST_NAMES[] = {
    "BARBER", "CAPSHAW", "DANDRIDGE", "DYER", "EASTER",
    "FAY", "FRANKE", "GADDIS", "GALLAGHER", "GREEN"
};

static const char ALPHANUM[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";

TM Warehouse* g_warehouse = nullptr;
TM District* g_district = nullptr;
TM Customer* g_customer = nullptr;
TM History* g_history = nullptr;
TM Order* g_order = nullptr;
TM NewOrder* g_neworder = nullptr;
TM OrderLine* g_orderline = nullptr;
TM Item* g_item = nullptr;
TM Stock* g_stock = nullptr;

TM int g_num_warehouses = DEFAULT_WAREHOUSES;
TM int g_num_districts = DEFAULT_DISTRICTS;
TM int g_num_customers = DEFAULT_CUSTOMERS;
TM int g_num_items = DEFAULT_ITEMS;

TM int g_district_next_o_id[100];
TM int g_order_count = 0;
TM int g_history_count = 0;
TM int g_neworder_count = 0;

TM int g_neworder_transactions = 0;
TM int g_payment_transactions = 0;
TM int g_orderstatus_transactions = 0;
TM int g_delivery_transactions = 0;
TM int g_stocklevel_transactions = 0;

static inline int idx_w(int w_id) { return w_id - 1; }
static inline int idx_d(int w_id, int d_id) { return (w_id - 1) * g_num_districts + (d_id - 1); }
static inline int idx_c(int w_id, int d_id, int c_id) {
    return ((w_id - 1) * g_num_districts + (d_id - 1)) * g_num_customers + (c_id - 1);
}
static inline int idx_ord(int w_id, int d_id, int o_id) {
    return ((w_id - 1) * g_num_districts + (d_id - 1)) * (g_num_districts * 100) + (o_id - 1);
}
static inline int idx_no(int w_id, int d_id, int o_id) {
    return ((w_id - 1) * g_num_districts + (d_id - 1)) * (g_num_districts * 100) + (o_id - 1);
}
static inline int idx_ol(int w_id, int d_id, int o_id, int ol_num) {
    return (((w_id - 1) * g_num_districts + (d_id - 1)) * (g_num_districts * 100) + (o_id - 1)) * 15 + (ol_num - 1);
}
static inline int idx_i(int i_id) { return i_id - 1; }
static inline int idx_s(int w_id, int i_id) { return (w_id - 1) * g_num_items + (i_id - 1); }

static void gen_string(char* dest, int len, const char* src) {
    int slen = strlen(src);
    for (int i = 0; i < len - 1; i++) {
        dest[i] = src[i % slen];
    }
    dest[len - 1] = '\0';
}

TM void init_data() {
    int num_warehouses = g_num_warehouses;
    int num_districts = g_num_districts;
    int num_customers = g_num_customers;
    int num_items = g_num_items;

    for (int w = 1; w <= num_warehouses; w++) {
        int wi = idx_w(w);
        g_warehouse[wi].w_id = w;
        gen_string(g_warehouse[wi].w_name, 11, "Warehouse");
        gen_string(g_warehouse[wi].w_street_1, 21, "Street");
        gen_string(g_warehouse[wi].w_street_2, 21, "Ave");
        gen_string(g_warehouse[wi].w_city, 21, "City");
        gen_string(g_warehouse[wi].w_state, 3, "ST");
        gen_string(g_warehouse[wi].w_zip, 10, "123456789");
        g_warehouse[wi].w_tax = 0.001f * (w % 10);
        g_warehouse[wi].w_ytd = 0.0f;
    }

    for (int w = 1; w <= num_warehouses; w++) {
        for (int d = 1; d <= num_districts; d++) {
            int di = idx_d(w, d);
            g_district[di].d_id = d;
            g_district[di].d_w_id = w;
            gen_string(g_district[di].d_name, 11, "District");
            gen_string(g_district[di].d_street_1, 21, "Street");
            gen_string(g_district[di].d_street_2, 21, "Ave");
            gen_string(g_district[di].d_city, 21, "City");
            gen_string(g_district[di].d_state, 3, "ST");
            gen_string(g_district[di].d_zip, 10, "123456789");
            g_district[di].d_tax = 0.001f * (d % 10);
            g_district[di].d_ytd = 0.0f;
            g_district[di].d_next_o_id = 3001;
            g_district_next_o_id[di] = 3001;
        }
    }

    for (int w = 1; w <= num_warehouses; w++) {
        for (int d = 1; d <= num_districts; d++) {
            for (int c = 1; c <= num_customers; c++) {
                int ci = idx_c(w, d, c);
                g_customer[ci].c_id = c;
                g_customer[ci].c_d_id = d;
                g_customer[ci].c_w_id = w;
                gen_string(g_customer[ci].c_first, 17, FIRST_NAMES[c % 10]);
                gen_string(g_customer[ci].c_middle, 3, "OE");
                gen_string(g_customer[ci].c_last, 17, LAST_NAMES[c % 10]);
                gen_string(g_customer[ci].c_street_1, 21, "Street");
                gen_string(g_customer[ci].c_street_2, 21, "Ave");
                gen_string(g_customer[ci].c_city, 21, "City");
                gen_string(g_customer[ci].c_state, 3, "ST");
                gen_string(g_customer[ci].c_zip, 10, "123456789");
                gen_string(g_customer[ci].c_phone, 17, "5555555555");
                g_customer[ci].c_since = 0;
                gen_string(g_customer[ci].c_credit, 3, c % 10 == 0 ? "BC" : "GC");
                g_customer[ci].c_credit_lim = 50000.0f;
                g_customer[ci].c_discount = 0.0001f * (c % 100);
                g_customer[ci].c_balance = 0.0f;
                g_customer[ci].c_ytd_payment = 0.0f;
                g_customer[ci].c_payment_cnt = 0;
                g_customer[ci].c_delivery_cnt = 0;
                gen_string(g_customer[ci].c_data, 501, "data");
            }
        }
    }

    for (int i = 1; i <= num_items; i++) {
        int ii = idx_i(i);
        g_item[ii].i_id = i;
        g_item[ii].i_im_id = (i % 10000) + 1;
        gen_string(g_item[ii].i_name, 25, "Item");
        g_item[ii].i_price = 1.00f * (i % 100) + 1.00f;
        gen_string(g_item[ii].i_data, 51, "Original");
    }

    for (int w = 1; w <= num_warehouses; w++) {
        for (int i = 1; i <= num_items; i++) {
            int si = idx_s(w, i);
            g_stock[si].s_i_id = i;
            g_stock[si].s_w_id = w;
            g_stock[si].s_quantity = 10 + (i % 90);
            for (int d = 1; d <= 10; d++) {
                char* dist = nullptr;
                switch(d) {
                    case 1: dist = g_stock[si].s_dist_01; break;
                    case 2: dist = g_stock[si].s_dist_02; break;
                    case 3: dist = g_stock[si].s_dist_03; break;
                    case 4: dist = g_stock[si].s_dist_04; break;
                    case 5: dist = g_stock[si].s_dist_05; break;
                    case 6: dist = g_stock[si].s_dist_06; break;
                    case 7: dist = g_stock[si].s_dist_07; break;
                    case 8: dist = g_stock[si].s_dist_08; break;
                    case 9: dist = g_stock[si].s_dist_09; break;
                    case 10: dist = g_stock[si].s_dist_10; break;
                }
                gen_string(dist, 25, "Dist");
            }
            g_stock[si].s_order_cnt = 0;
            g_stock[si].s_remote_cnt = 0;
            gen_string(g_stock[si].s_data, 51, "Original");
        }
    }
}

TX int txn_new_order(int w_id, int d_id, int c_id, int num_items, int* item_ids, int* supplier_ws, int* quantities) {
    int d_idx = idx_d(w_id, d_id);
    int o_id = g_district[d_idx].d_next_o_id++;
    g_order_count++;

    int o_idx = idx_ord(w_id, d_id, o_id);
    g_order[o_idx].o_id = o_id;
    g_order[o_idx].o_d_id = d_id;
    g_order[o_idx].o_w_id = w_id;
    g_order[o_idx].o_c_id = c_id;
    g_order[o_idx].o_entry_d = (int)(time(nullptr));
    g_order[o_idx].o_carrier_id = 0;
    g_order[o_idx].o_ol_cnt = num_items;
    g_order[o_idx].o_all_local = 1;

    int no_idx = idx_no(w_id, d_id, o_id);
    g_neworder[no_idx].no_o_id = o_id;
    g_neworder[no_idx].no_d_id = d_id;
    g_neworder[no_idx].no_w_id = w_id;
    g_neworder_count++;

    float total_amount = 0;
    for (int i = 0; i < num_items; i++) {
        int ol_number = i + 1;
        int ol_idx = idx_ol(w_id, d_id, o_id, ol_number);

        g_orderline[ol_idx].ol_o_id = o_id;
        g_orderline[ol_idx].ol_d_id = d_id;
        g_orderline[ol_idx].ol_w_id = w_id;
        g_orderline[ol_idx].ol_number = ol_number;
        g_orderline[ol_idx].ol_i_id = item_ids[i];
        g_orderline[ol_idx].ol_supply_w_id = supplier_ws[i];
        g_orderline[ol_idx].ol_quantity = quantities[i];

        int s_idx = idx_s(supplier_ws[i], item_ids[i]);
        float i_price = g_item[idx_i(item_ids[i])].i_price;
        float ol_amount = quantities[i] * i_price;
        total_amount += ol_amount;

        g_orderline[ol_idx].ol_amount = ol_amount;
        g_orderline[ol_idx].ol_delivery_d = 0;
        gen_string(g_orderline[ol_idx].ol_dist_info, 25, "Dist");

        if (g_stock[s_idx].s_quantity - quantities[i] >= 10) {
            g_stock[s_idx].s_quantity -= quantities[i];
        } else {
            g_stock[s_idx].s_quantity += quantities[i] + 91;
        }
        g_stock[s_idx].s_order_cnt++;
    }

    g_neworder_transactions++;
    return o_id;
}

TX void txn_payment(int w_id, int d_id, int c_id, float amount) {
    int w_idx = idx_w(w_id);
    g_warehouse[w_idx].w_ytd += amount;

    int d_idx = idx_d(w_id, d_id);
    g_district[d_idx].d_ytd += amount;

    int c_idx = idx_c(w_id, d_id, c_id);
    g_customer[c_idx].c_balance -= amount;
    g_customer[c_idx].c_ytd_payment += amount;
    g_customer[c_idx].c_payment_cnt++;

    int h_idx = g_history_count++;
    g_history[h_idx].h_c_id = c_id;
    g_history[h_idx].h_c_d_id = d_id;
    g_history[h_idx].h_c_w_id = w_id;
    g_history[h_idx].h_d_id = d_id;
    g_history[h_idx].h_w_id = w_id;
    g_history[h_idx].h_date = (int)(time(nullptr));
    g_history[h_idx].h_amount = amount;
    gen_string(g_history[h_idx].h_data, 25, "Payment");

    g_payment_transactions++;
}

TX float txn_order_status(int w_id, int d_id, int c_id) {
    int c_idx = idx_c(w_id, d_id, c_id);
    float balance = g_customer[c_idx].c_balance;

    int latest_o_id = -1;
    for (int o_id = 3000; o_id >= 1; o_id--) {
        int o_idx = idx_ord(w_id, d_id, o_id);
        if (g_order[o_idx].o_c_id == c_id) {
            latest_o_id = o_id;
            break;
        }
    }

    if (latest_o_id > 0) {
        int o_idx = idx_ord(w_id, d_id, latest_o_id);
        int ol_cnt = g_order[o_idx].o_ol_cnt;
    }

    g_orderstatus_transactions++;
    return balance;
}

TX void txn_delivery(int w_id, int carrier_id) {
    for (int d_id = 1; d_id <= g_num_districts; d_id++) {
        int no_idx = idx_no(w_id, d_id, 1);
        int found_no = -1;

        for (int o_id = 1; o_id < 3000; o_id++) {
            int no_check = idx_no(w_id, d_id, o_id);
            if (g_neworder[no_check].no_o_id == o_id) {
                found_no = o_id;
                break;
            }
        }

        if (found_no < 0) continue;

        int o_idx = idx_ord(w_id, d_id, found_no);
        int c_id = g_order[o_idx].o_c_id;
        float ol_total = 0;

        int ol_cnt = g_order[o_idx].o_ol_cnt;
        for (int ol_num = 1; ol_num <= ol_cnt; ol_num++) {
            int ol_idx = idx_ol(w_id, d_id, found_no, ol_num);
            ol_total += g_orderline[ol_idx].ol_amount;
            g_orderline[ol_idx].ol_delivery_d = (int)(time(nullptr));
        }

        g_order[o_idx].o_carrier_id = carrier_id;

        int c_idx = idx_c(w_id, d_id, c_id);
        g_customer[c_idx].c_balance += ol_total;
        g_customer[c_idx].c_delivery_cnt++;

        g_neworder[no_idx].no_o_id = -1;
    }

    g_delivery_transactions++;
}

TX int txn_stock_level(int w_id, int d_id, int threshold) {
    int d_idx = idx_d(w_id, d_id);
    int next_o_id = g_district[d_idx].d_next_o_id;
    int stock_count = 0;

    int start_o_id = next_o_id - 20;
    if (start_o_id < 1) start_o_id = 1;

    for (int o_id = start_o_id; o_id < next_o_id; o_id++) {
        int o_idx = idx_ord(w_id, d_id, o_id);
        if (g_order[o_idx].o_w_id == 0) continue;

        int ol_cnt = g_order[o_idx].o_ol_cnt;
        for (int ol_num = 1; ol_num <= ol_cnt; ol_num++) {
            int ol_idx = idx_ol(w_id, d_id, o_id, ol_num);
            int i_id = g_orderline[ol_idx].ol_i_id;
            int s_idx = idx_s(w_id, i_id);
            if (g_stock[s_idx].s_quantity < threshold) {
                stock_count++;
            }
        }
    }

    g_stocklevel_transactions++;
    return stock_count;
}

std::atomic<bool> done{false};
std::atomic<uint64_t> total_ops{0};

struct Worker {
    int id;
    int loops;
    std::mt19937* rng;
};

void run(Worker* w) {
    std::uniform_int_distribution<int> wdist(1, g_num_warehouses);
    std::uniform_int_distribution<int> ddist(1, g_num_districts);
    std::uniform_int_distribution<int> cdist(1, g_num_customers);
    std::uniform_int_distribution<int> op_dist(0, 99);
    std::uniform_int_distribution<int> item_dist(1, g_num_items);
    std::uniform_int_distribution<int> qty_dist(1, 10);

    while (!done.load() && w->loops > 0) {
        w->loops--;
        int r = op_dist(*w->rng);
        int w_id = wdist(*w->rng);
        int d_id = ddist(*w->rng);
        int c_id = cdist(*w->rng);

        if (r < 45) {
            int num_items = 5 + (r % 10);
            int item_ids[15];
            int supplier_ws[15];
            int quantities[15];

            for (int i = 0; i < num_items; i++) {
                item_ids[i] = item_dist(*w->rng);
                supplier_ws[i] = wdist(*w->rng);
                quantities[i] = qty_dist(*w->rng);
            }

            txn_new_order(w_id, d_id, c_id, num_items, item_ids, supplier_ws, quantities);
        } else if (r < 88) {
            txn_payment(w_id, d_id, c_id, 100.0f + r);
        } else if (r < 92) {
            txn_order_status(w_id, d_id, c_id);
        } else if (r < 96) {
            txn_delivery(w_id, r % 10 + 1);
        } else {
            txn_stock_level(w_id, d_id, 20);
        }

        total_ops.fetch_add(1, std::memory_order_relaxed);
    }
}

int main(int argc, char* argv[]) {
    int threads = 4;
    int duration = 10000;
    g_num_warehouses = DEFAULT_WAREHOUSES;
    g_num_districts = DEFAULT_DISTRICTS;
    g_num_customers = DEFAULT_CUSTOMERS;
    g_num_items = DEFAULT_ITEMS;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) threads = std::atoi(argv[++i]);
        else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) duration = std::atoi(argv[++i]);
        else if (strcmp(argv[i], "-w") == 0 && i + 1 < argc) g_num_warehouses = std::atoi(argv[++i]);
    }

    size_t warehouse_size = g_num_warehouses * sizeof(Warehouse);
    size_t district_size = g_num_warehouses * g_num_districts * sizeof(District);
    size_t customer_size = g_num_warehouses * g_num_districts * g_num_customers * sizeof(Customer);
    size_t history_size = 100000 * sizeof(History);
    size_t order_size = g_num_warehouses * g_num_districts * 3000 * sizeof(Order);
    size_t neworder_size = g_num_warehouses * g_num_districts * 3000 * sizeof(NewOrder);
    size_t orderline_size = g_num_warehouses * g_num_districts * 3000 * 15 * sizeof(OrderLine);
    size_t item_size = g_num_items * sizeof(Item);
    size_t stock_size = g_num_warehouses * g_num_items * sizeof(Stock);

    g_warehouse = (Warehouse*)malloc(warehouse_size);
    g_district = (District*)malloc(district_size);
    g_customer = (Customer*)malloc(customer_size);
    g_history = (History*)malloc(history_size);
    g_order = (Order*)malloc(order_size);
    g_neworder = (NewOrder*)malloc(neworder_size);
    g_orderline = (OrderLine*)malloc(orderline_size);
    g_item = (Item*)malloc(item_size);
    g_stock = (Stock*)malloc(stock_size);

    std::cout << "TPC-C (Full Specification v5.11)\n";
    std::cout << "================================\n";
    std::cout << "Configuration:\n";
    std::cout << "  Warehouses: " << g_num_warehouses << "\n";
    std::cout << "  Districts:  " << g_num_districts << "\n";
    std::cout << "  Customers:  " << g_num_customers << "\n";
    std::cout << "  Items:      " << g_num_items << "\n";
    std::cout << "  Threads:    " << threads << "\n";
    std::cout << "  Duration:   " << duration << " ms\n\n";

    std::cout << "Initializing data..." << std::endl;
    init_data();
    std::cout << "  Warehouses: " << g_num_warehouses << "\n";
    std::cout << "  Districts:  " << (g_num_warehouses * g_num_districts) << "\n";
    std::cout << "  Customers:  " << (g_num_warehouses * g_num_districts * g_num_customers) << "\n";
    std::cout << "  Items:      " << g_num_items << "\n";
    std::cout << "  Stock:      " << (g_num_warehouses * g_num_items) << "\n\n";

    int loops = duration / 10;
    std::vector<Worker> ws(threads);
    std::vector<std::thread> thr;
    std::vector<std::mt19937> rngs(threads);

    for (int i = 0; i < threads; i++) {
        rngs[i] = std::mt19937(i * 12345 + 42);
        ws[i].id = i;
        ws[i].loops = loops;
        ws[i].rng = &rngs[i];
    }

    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < threads; i++) thr.emplace_back(run, &ws[i]);
    std::this_thread::sleep_for(std::chrono::milliseconds(duration));
    done = true;
    for (auto& t : thr) t.join();
    auto end = std::chrono::high_resolution_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    uint64_t ops = total_ops.load();

    std::cout << "Results\n";
    std::cout << "=======\n";
    std::cout << "Elapsed:      " << ms << " ms\n";
    std::cout << "Total ops:    " << ops << "\n";
    std::cout << "Ops/sec:      " << (ops * 1000.0 / ms) << "\n\n";

    std::cout << "Transaction breakdown:\n";
    std::cout << "  New-Order:     " << g_neworder_transactions << "\n";
    std::cout << "  Payment:        " << g_payment_transactions << "\n";
    std::cout << "  Order-Status:  " << g_orderstatus_transactions << "\n";
    std::cout << "  Delivery:       " << g_delivery_transactions << "\n";
    std::cout << "  Stock-Level:    " << g_stocklevel_transactions << "\n";

    free(g_warehouse);
    free(g_district);
    free(g_customer);
    free(g_history);
    free(g_order);
    free(g_neworder);
    free(g_orderline);
    free(g_item);
    free(g_stock);

    return 0;
}