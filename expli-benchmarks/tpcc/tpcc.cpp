// TPC-C — C++ port of the plugin spec-compliant TPC-C (explicit API path)
// Based on TPC-C Specification v5.11
//
// 9 tables, 5 transaction types. Flat TM arrays, tx_run pattern.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <random>
#include <thread>
#include <unordered_set>
#include <vector>

#include "../tests/benchmark_test.hpp"

// ── TM runtime declarations ────────────────────────────────────────
extern "C" {
    void     tm_init();
    void     tm_exit();
    void     tm_init_thread();
    void     tm_exit_thread();
    void*    tm_calloc(size_t n, size_t sz);
    long     tm_read_i8(const void* addr);
    void     tm_write_i8(void* addr, long val);
    int32_t  tm_read_i4(const void* addr);
    void     tm_write_i4(void* addr, int32_t val);
    float    tm_read_f4(const void* addr);
    void     tm_write_f4(void* addr, float val);
    void     tm_begin();
    void     tm_end();
    extern __thread int32_t tm_nested_call_counter;
    extern __thread int32_t tm_longjmp_ret;
    extern __thread sigjmp_buf tm_jmpbuf;
}

#define TM_READ_I8(p)     tm_read_i8((const void*)(p))
#define TM_WRITE_I8(p, v) tm_write_i8((void*)(p), (long)(v))

template <typename F>
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

// ── Configuration ──────────────────────────────────────────────────
constexpr int DEFAULT_WAREHOUSES = 1;
constexpr int DEFAULT_DISTRICTS  = 10;
constexpr int DEFAULT_CUSTOMERS  = 3000;
constexpr int DEFAULT_ITEMS      = 100000;
constexpr int PREPOPULATED_ORDERS = 3000;
constexpr int MAX_ORDERS_PER_DISTRICT = 10000;
constexpr int MAX_OL_PER_ORDER   = 15;
constexpr int MAX_HISTORY_ROWS   = 1000000;

static int g_num_threads = 4;
static int g_duration = 10000;
static int g_num_warehouses = DEFAULT_WAREHOUSES;
static int g_num_districts = DEFAULT_DISTRICTS;
static int g_num_customers = DEFAULT_CUSTOMERS;
static int g_num_items = DEFAULT_ITEMS;

static void parse_args(int argc, char* argv[]) {
    for (int i = 1; i < argc; i++) {
        if      (strcmp(argv[i], "-t") == 0 && i+1 < argc) g_num_threads   = atoi(argv[++i]);
        else if (strcmp(argv[i], "-d") == 0 && i+1 < argc) g_duration  = atoi(argv[++i]);
        else if (strcmp(argv[i], "-w") == 0 && i+1 < argc) g_num_warehouses = atoi(argv[++i]);
    }
}

// ── Table structs ──────────────────────────────────────────────────
struct Warehouse {
    int   w_id;
    char  w_name[11];
    char  w_street_1[21];
    char  w_street_2[21];
    char  w_city[21];
    char  w_state[3];
    char  w_zip[10];
    float w_tax;
    float w_ytd;
};

struct District {
    int   d_id;
    int   d_w_id;
    char  d_name[11];
    char  d_street_1[21];
    char  d_street_2[21];
    char  d_city[21];
    char  d_state[3];
    char  d_zip[10];
    float d_tax;
    float d_ytd;
    int   d_next_o_id;
};

struct Customer {
    int   c_id;
    int   c_d_id;
    int   c_w_id;
    char  c_first[17];
    char  c_middle[3];
    char  c_last[17];
    char  c_street_1[21];
    char  c_street_2[21];
    char  c_city[21];
    char  c_state[3];
    char  c_zip[10];
    char  c_phone[17];
    int   c_since;
    char  c_credit[3];
    float c_credit_lim;
    float c_discount;
    float c_balance;
    float c_ytd_payment;
    int   c_payment_cnt;
    int   c_delivery_cnt;
    char  c_data[501];
};

struct History {
    int   h_c_id;
    int   h_c_d_id;
    int   h_c_w_id;
    int   h_d_id;
    int   h_w_id;
    int   h_date;
    float h_amount;
    char  h_data[25];
};

struct Order {
    int   o_id;
    int   o_d_id;
    int   o_w_id;
    int   o_c_id;
    int   o_entry_d;
    int   o_carrier_id;
    int   o_ol_cnt;
    float o_all_local;
};

struct NewOrder {
    int no_o_id;
    int no_d_id;
    int no_w_id;
};

struct OrderLine {
    int   ol_o_id;
    int   ol_d_id;
    int   ol_w_id;
    int   ol_number;
    int   ol_i_id;
    int   ol_supply_w_id;
    int   ol_quantity;
    int   ol_delivery_d;
    float ol_amount;
    char  ol_dist_info[25];
};

struct Item {
    int   i_id;
    int   i_im_id;
    char  i_name[25];
    float i_price;
    char  i_data[51];
};

struct Stock {
    int   s_i_id;
    int   s_w_id;
    int   s_quantity;
    char  s_dist_01[25];
    char  s_dist_02[25];
    char  s_dist_03[25];
    char  s_dist_04[25];
    char  s_dist_05[25];
    char  s_dist_06[25];
    char  s_dist_07[25];
    char  s_dist_08[25];
    char  s_dist_09[25];
    char  s_dist_10[25];
    int   s_ytd;
    int   s_order_cnt;
    int   s_remote_cnt;
    char  s_data[51];
};

// ── Global data (TM-allocated flat arrays) ─────────────────────────
static Warehouse* g_warehouse  = nullptr;
static District*  g_district   = nullptr;
static Customer*  g_customer   = nullptr;
static History*   g_history    = nullptr;
static Order*     g_order      = nullptr;
static NewOrder*  g_neworder   = nullptr;
static OrderLine* g_orderline  = nullptr;
static Item*      g_item       = nullptr;
static Stock*     g_stock      = nullptr;
static int*       g_history_count = nullptr;

// ── Index helpers ──────────────────────────────────────────────────
static int idx_w(int w)        { return w - 1; }
static int idx_d(int w, int d) { return (w - 1) * g_num_districts + (d - 1); }
static int idx_c(int w, int d, int c) {
    return ((w - 1) * g_num_districts + (d - 1)) * g_num_customers + (c - 1);
}
static int idx_ord(int w, int d, int o) {
    return ((w - 1) * g_num_districts + (d - 1)) * MAX_ORDERS_PER_DISTRICT + (o - 1);
}
static int idx_no(int w, int d, int o)   { return idx_ord(w, d, o); }
static int idx_ol(int w, int d, int o, int l) {
    return (((w - 1) * g_num_districts + (d - 1)) * MAX_ORDERS_PER_DISTRICT + (o - 1))
           * MAX_OL_PER_ORDER + (l - 1);
}
static int idx_i(int i) { return i - 1; }
static int idx_s(int w, int i) { return (w - 1) * g_num_items + (i - 1); }

// ── Helper: fill char array with repeating pattern ─────────────────
static void gen_string(char* dest, int len, const char* src) {
    int slen = (int)strlen(src);
    for (int i = 0; i < len - 1; i++) dest[i] = src[i % slen];
    dest[len - 1] = '\0';
}

static char* stock_dist_string(Stock* s, int d_id) {
    switch (d_id) {
        case 1:  return s->s_dist_01;
        case 2:  return s->s_dist_02;
        case 3:  return s->s_dist_03;
        case 4:  return s->s_dist_04;
        case 5:  return s->s_dist_05;
        case 6:  return s->s_dist_06;
        case 7:  return s->s_dist_07;
        case 8:  return s->s_dist_08;
        case 9:  return s->s_dist_09;
        case 10: return s->s_dist_10;
        default: return s->s_dist_01;
    }
}

// ── TM read/write helpers for flat arrays (matching plugin) ────────
static float fread(const void* p) { return tm_read_f4(p); }
static void fwrite(void* p, float v) { tm_write_f4(p, v); }
static int iread(const void* p) { return tm_read_i4(p); }
static void iwrite(void* p, int v) { tm_write_i4(p, v); }

// ── Data initialization ────────────────────────────────────────────
static void init_data() {
    int nw = g_num_warehouses, nd = g_num_districts;
    int nc = g_num_customers, ni = g_num_items;

    // Warehouse
    for (int w = 1; w <= nw; w++) {
        int wi = idx_w(w);
        g_warehouse[wi].w_id = w;
        gen_string(g_warehouse[wi].w_name, 11, "Warehouse");
        gen_string(g_warehouse[wi].w_street_1, 21, "Street");
        gen_string(g_warehouse[wi].w_street_2, 21, "Ave");
        gen_string(g_warehouse[wi].w_city, 21, "City");
        gen_string(g_warehouse[wi].w_state, 3, "ST");
        gen_string(g_warehouse[wi].w_zip, 10, "123456789");
        g_warehouse[wi].w_tax = 0.1900f;
        g_warehouse[wi].w_ytd = 3000000.00f;
    }

    // District
    for (int w = 1; w <= nw; w++) {
        for (int d = 1; d <= nd; d++) {
            int di = idx_d(w, d);
            g_district[di].d_id = d;
            g_district[di].d_w_id = w;
            gen_string(g_district[di].d_name, 11, "District");
            gen_string(g_district[di].d_street_1, 21, "Street");
            gen_string(g_district[di].d_street_2, 21, "Ave");
            gen_string(g_district[di].d_city, 21, "City");
            gen_string(g_district[di].d_state, 3, "ST");
            gen_string(g_district[di].d_zip, 10, "123456789");
            g_district[di].d_tax = 0.1500f;
            g_district[di].d_ytd = 3000000.00f;
            g_district[di].d_next_o_id = PREPOPULATED_ORDERS + 1;
        }
    }

    // Customer
    for (int w = 1; w <= nw; w++) {
        for (int d = 1; d <= nd; d++) {
            for (int c = 1; c <= nc; c++) {
                int ci = idx_c(w, d, c);
                g_customer[ci].c_id = c;
                g_customer[ci].c_d_id = d;
                g_customer[ci].c_w_id = w;
                gen_string(g_customer[ci].c_first, 17, "FIRSTNAME");
                gen_string(g_customer[ci].c_middle, 3, "OE");
                gen_string(g_customer[ci].c_last, 17, "LASTNAME");
                gen_string(g_customer[ci].c_street_1, 21, "Street");
                gen_string(g_customer[ci].c_street_2, 21, "Ave");
                gen_string(g_customer[ci].c_city, 21, "City");
                gen_string(g_customer[ci].c_state, 3, "ST");
                gen_string(g_customer[ci].c_zip, 10, "123456789");
                gen_string(g_customer[ci].c_phone, 17, "5555555555");
                g_customer[ci].c_since = (int)::time(nullptr);
                gen_string(g_customer[ci].c_credit, 3, "GC");
                g_customer[ci].c_credit_lim = 50000.00f;
                g_customer[ci].c_discount = 0.3000f;
                g_customer[ci].c_balance = -10.00f;
                g_customer[ci].c_ytd_payment = 10.00f;
                g_customer[ci].c_payment_cnt = 1;
                g_customer[ci].c_delivery_cnt = 0;
                gen_string(g_customer[ci].c_data, 501, "data");
            }
        }
    }

    // Item
    for (int i = 1; i <= ni; i++) {
        int ii = idx_i(i);
        g_item[ii].i_id = i;
        g_item[ii].i_im_id = (i % 99999) + 1;
        gen_string(g_item[ii].i_name, 25, "ItemName");
        g_item[ii].i_price = (float)((i % 100) + 1);
        gen_string(g_item[ii].i_data, 51, "OriginalData");
    }

    // Stock
    for (int w = 1; w <= nw; w++) {
        for (int i = 1; i <= ni; i++) {
            int si = idx_s(w, i);
            g_stock[si].s_i_id = i;
            g_stock[si].s_w_id = w;
            g_stock[si].s_quantity = 100;
            for (int d = 1; d <= 10; d++)
                gen_string(stock_dist_string(&g_stock[si], d), 25, "DistStr");
            g_stock[si].s_ytd = 0;
            g_stock[si].s_order_cnt = 0;
            g_stock[si].s_remote_cnt = 0;
            gen_string(g_stock[si].s_data, 51, "OriginalData");
        }
    }

    // Pre-populate History
    int hist_count = 0;
    for (int w = 1; w <= nw; w++) {
        for (int d = 1; d <= nd; d++) {
            for (int c = 1; c <= nc; c++) {
                int hi = hist_count++;
                g_history[hi].h_c_id = c;
                g_history[hi].h_c_d_id = d;
                g_history[hi].h_c_w_id = w;
                g_history[hi].h_d_id = d;
                g_history[hi].h_w_id = w;
                g_history[hi].h_date = (int)::time(nullptr);
                g_history[hi].h_amount = 10.00f;
                gen_string(g_history[hi].h_data, 25, "InitHistory");
            }
        }
    }
    *g_history_count = hist_count;

    // Pre-populate Orders, Order-Lines, New-Orders
    for (int w = 1; w <= nw; w++) {
        for (int d = 1; d <= nd; d++) {
            std::mt19937 ord_rng((unsigned)(w * 1000 + d));

            for (int o = 1; o <= PREPOPULATED_ORDERS; o++) {
                int c_id = (int)(ord_rng() % nc) + 1;
                int ol_cnt = (int)(ord_rng() % 11) + 5;

                int o_idx = idx_ord(w, d, o);
                g_order[o_idx].o_id = o;
                g_order[o_idx].o_d_id = d;
                g_order[o_idx].o_w_id = w;
                g_order[o_idx].o_c_id = c_id;
                g_order[o_idx].o_entry_d = (int)::time(nullptr);
                g_order[o_idx].o_carrier_id = (o <= 2100) ? ((int)(ord_rng() % 10) + 1) : 0;
                g_order[o_idx].o_ol_cnt = ol_cnt;
                g_order[o_idx].o_all_local = 1.0f;

                std::unordered_set<int> used_items;
                for (int l = 1; l <= ol_cnt; l++) {
                    int ol_idx = idx_ol(w, d, o, l);
                    int i_id;
                    do { i_id = (int)(ord_rng() % ni) + 1; } while (used_items.count(i_id));
                    used_items.insert(i_id);

                    g_orderline[ol_idx].ol_o_id = o;
                    g_orderline[ol_idx].ol_d_id = d;
                    g_orderline[ol_idx].ol_w_id = w;
                    g_orderline[ol_idx].ol_number = l;
                    g_orderline[ol_idx].ol_i_id = i_id;
                    g_orderline[ol_idx].ol_supply_w_id = w;
                    g_orderline[ol_idx].ol_quantity = 5;
                    g_orderline[ol_idx].ol_delivery_d = (o <= 2100) ? (int)::time(nullptr) : 0;
                    g_orderline[ol_idx].ol_amount = 5.0f * g_item[idx_i(i_id)].i_price;
                    gen_string(g_orderline[ol_idx].ol_dist_info, 25,
                               stock_dist_string(&g_stock[idx_s(w, i_id)], d));
                }

                if (o > 2100) {
                    int no_idx = idx_no(w, d, o);
                    g_neworder[no_idx].no_o_id = o;
                    g_neworder[no_idx].no_d_id = d;
                    g_neworder[no_idx].no_w_id = w;
                }
            }
        }
    }
}

// ── Transaction: New-Order (§2.4) ──────────────────────────────────
// 45% of transaction mix
static int txn_new_order(int w_id, int d_id, int c_id,
                         int num_items, int* item_ids,
                         int* supplier_ws, int* quantities) {
    int d_idx = idx_d(w_id, d_id);
    int o_id = iread(&g_district[d_idx].d_next_o_id);
    iwrite(&g_district[d_idx].d_next_o_id, o_id + 1);

    int o_idx = idx_ord(w_id, d_id, o_id);
    iwrite(&g_order[o_idx].o_id, o_id);
    iwrite(&g_order[o_idx].o_d_id, d_id);
    iwrite(&g_order[o_idx].o_w_id, w_id);
    iwrite(&g_order[o_idx].o_c_id, c_id);
    iwrite(&g_order[o_idx].o_entry_d, (int)::time(nullptr));
    iwrite(&g_order[o_idx].o_carrier_id, 0);
    iwrite(&g_order[o_idx].o_ol_cnt, num_items);

    int no_idx = idx_no(w_id, d_id, o_id);
    iwrite(&g_neworder[no_idx].no_o_id, o_id);
    iwrite(&g_neworder[no_idx].no_d_id, d_id);
    iwrite(&g_neworder[no_idx].no_w_id, w_id);

    float total_amount = 0;
    int all_local = 1;

    for (int i = 0; i < num_items; i++) {
        int ol_number = i + 1;
        int ol_idx = idx_ol(w_id, d_id, o_id, ol_number);
        int ol_i_id = item_ids[i];
        int ol_sw_id = supplier_ws[i];
        int ol_qty = quantities[i];

        if (ol_sw_id != w_id) all_local = 0;

        iwrite(&g_orderline[ol_idx].ol_o_id, o_id);
        iwrite(&g_orderline[ol_idx].ol_d_id, d_id);
        iwrite(&g_orderline[ol_idx].ol_w_id, w_id);
        iwrite(&g_orderline[ol_idx].ol_number, ol_number);
        iwrite(&g_orderline[ol_idx].ol_i_id, ol_i_id);
        iwrite(&g_orderline[ol_idx].ol_supply_w_id, ol_sw_id);
        iwrite(&g_orderline[ol_idx].ol_quantity, ol_qty);
        iwrite(&g_orderline[ol_idx].ol_delivery_d, 0);

        float i_price = iread((int*)&g_item[idx_i(ol_i_id)].i_price);
        float ol_amount = (float)ol_qty * i_price;
        fwrite(&g_orderline[ol_idx].ol_amount, ol_amount);
        total_amount += ol_amount;

        int s_idx = idx_s(ol_sw_id, ol_i_id);
        int sqty = iread(&g_stock[s_idx].s_quantity);
        if (sqty - ol_qty >= 10)
            iwrite(&g_stock[s_idx].s_quantity, sqty - ol_qty);
        else
            iwrite(&g_stock[s_idx].s_quantity, sqty - ol_qty + 91);

        iwrite(&g_stock[s_idx].s_ytd, iread(&g_stock[s_idx].s_ytd) + ol_qty);
        iwrite(&g_stock[s_idx].s_order_cnt, iread(&g_stock[s_idx].s_order_cnt) + 1);
        if (ol_sw_id != w_id)
            iwrite(&g_stock[s_idx].s_remote_cnt, iread(&g_stock[s_idx].s_remote_cnt) + 1);

        gen_string(g_orderline[ol_idx].ol_dist_info, 25,
                   stock_dist_string(&g_stock[s_idx], d_id));
    }

    // Write o_all_local as float via TM
    fwrite(&g_order[o_idx].o_all_local, all_local ? 1.0f : 0.0f);
    return o_id;
}

// ── Transaction: Payment (§2.5) ────────────────────────────────────
// 43% of transaction mix
static void txn_payment(int w_id, int d_id, int c_id, float amount) {
    int w_idx = idx_w(w_id);
    fwrite(&g_warehouse[w_idx].w_ytd, fread(&g_warehouse[w_idx].w_ytd) + amount);

    int d_idx = idx_d(w_id, d_id);
    fwrite(&g_district[d_idx].d_ytd, fread(&g_district[d_idx].d_ytd) + amount);

    int c_idx = idx_c(w_id, d_id, c_id);
    fwrite(&g_customer[c_idx].c_balance, fread(&g_customer[c_idx].c_balance) - amount);
    fwrite(&g_customer[c_idx].c_ytd_payment, fread(&g_customer[c_idx].c_ytd_payment) + amount);
    iwrite(&g_customer[c_idx].c_payment_cnt, iread(&g_customer[c_idx].c_payment_cnt) + 1);

    int h_idx = iread(g_history_count);
    iwrite(g_history_count, h_idx + 1);
    iwrite(&g_history[h_idx].h_c_id, c_id);
    iwrite(&g_history[h_idx].h_c_d_id, d_id);
    iwrite(&g_history[h_idx].h_c_w_id, w_id);
    iwrite(&g_history[h_idx].h_d_id, d_id);
    iwrite(&g_history[h_idx].h_w_id, w_id);
    iwrite(&g_history[h_idx].h_date, (int)::time(nullptr));
    fwrite(&g_history[h_idx].h_amount, amount);
    gen_string(g_history[h_idx].h_data, 25, "Payment");
}

// ── Transaction: Order-Status (§2.6) ───────────────────────────────
// 4% of transaction mix
static float txn_order_status(int w_id, int d_id, int c_id) {
    int c_idx = idx_c(w_id, d_id, c_id);
    float balance = fread(&g_customer[c_idx].c_balance);

    int d_idx = idx_d(w_id, d_id);
    int max_o = iread(&g_district[d_idx].d_next_o_id) - 1;
    int latest_o_id = -1;

    for (int oid = max_o; oid >= 1; oid--) {
        int o_idx = idx_ord(w_id, d_id, oid);
        if (iread(&g_order[o_idx].o_c_id) == c_id && iread(&g_order[o_idx].o_w_id) != 0) {
            latest_o_id = oid;
            break;
        }
    }

    if (latest_o_id > 0) {
        int o_idx = idx_ord(w_id, d_id, latest_o_id);
        int ol_cnt = iread(&g_order[o_idx].o_ol_cnt);
        for (int l = 1; l <= ol_cnt; l++) {
            int ol_idx = idx_ol(w_id, d_id, latest_o_id, l);
            volatile float amt = fread(&g_orderline[ol_idx].ol_amount);
            (void)amt;
        }
    }

    return balance;
}

// ── Transaction: Delivery (§2.7) ───────────────────────────────────
// 4% of transaction mix
static void txn_delivery(int w_id, int carrier_id) {
    for (int d_id = 1; d_id <= g_num_districts; d_id++) {
        int found_no = -1;
        for (int oid = 1; oid < MAX_ORDERS_PER_DISTRICT; oid++) {
            int no_idx = idx_no(w_id, d_id, oid);
            if (iread(&g_neworder[no_idx].no_o_id) == oid) {
                found_no = oid;
                break;
            }
        }
        if (found_no < 0) continue;

        int o_idx = idx_ord(w_id, d_id, found_no);
        iwrite(&g_order[o_idx].o_carrier_id, carrier_id);

        int ol_cnt = iread(&g_order[o_idx].o_ol_cnt);
        float ol_total = 0;
        for (int l = 1; l <= ol_cnt; l++) {
            int ol_idx = idx_ol(w_id, d_id, found_no, l);
            ol_total += fread(&g_orderline[ol_idx].ol_amount);
            iwrite(&g_orderline[ol_idx].ol_delivery_d, (int)::time(nullptr));
        }

        int c_id = iread(&g_order[o_idx].o_c_id);
        int c_idx = idx_c(w_id, d_id, c_id);
        fwrite(&g_customer[c_idx].c_balance, fread(&g_customer[c_idx].c_balance) + ol_total);
        iwrite(&g_customer[c_idx].c_delivery_cnt, iread(&g_customer[c_idx].c_delivery_cnt) + 1);

        iwrite(&g_neworder[idx_no(w_id, d_id, found_no)].no_o_id, -1);
    }
}

// ── Transaction: Stock-Level (§2.8) ────────────────────────────────
// 4% of transaction mix
static int txn_stock_level(int w_id, int d_id, int threshold) {
    int d_idx = idx_d(w_id, d_id);
    int next_o_id = iread(&g_district[d_idx].d_next_o_id);

    int start_o_id = next_o_id - 20;
    if (start_o_id < 1) start_o_id = 1;

    std::unordered_set<int> counted;

    for (int oid = start_o_id; oid < next_o_id; oid++) {
        int o_idx = idx_ord(w_id, d_id, oid);
        if (iread(&g_order[o_idx].o_w_id) == 0) continue;

        int ol_cnt = iread(&g_order[o_idx].o_ol_cnt);
        for (int l = 1; l <= ol_cnt; l++) {
            int ol_idx = idx_ol(w_id, d_id, oid, l);
            int i_id = iread(&g_orderline[ol_idx].ol_i_id);
            if (counted.count(i_id)) continue;
            counted.insert(i_id);

            int s_idx = idx_s(w_id, i_id);
            if (iread(&g_stock[s_idx].s_quantity) < threshold) {
                // low-stock — counted in distinct set
            }
        }
    }

    return (int)counted.size();
}

// ── Worker ─────────────────────────────────────────────────────────
static std::atomic<bool> g_done{false};
static std::atomic<uint64_t> g_total_ops{0};
static std::atomic<int> g_counts[5] = {};

static void run_worker(int id, int loops) {
    tm_init_thread();
    std::mt19937 rng((unsigned)(id * 12345 + 42));
    std::uniform_int_distribution<int> wdist(1, g_num_warehouses);
    std::uniform_int_distribution<int> ddist(1, g_num_districts);
    std::uniform_int_distribution<int> cdist(1, g_num_customers);
    std::uniform_int_distribution<int> op_dist(0, 99);
    std::uniform_int_distribution<int> item_dist(1, g_num_items);
    std::uniform_int_distribution<int> qty_dist(1, 10);
    std::uniform_int_distribution<int> olcount_dist(5, 15);
    std::uniform_int_distribution<int> threshold_dist(10, 20);

    while (!g_done.load() && loops > 0) {
        loops--;
        int r = op_dist(rng);
        int w_id = wdist(rng);
        int d_id = ddist(rng);

        if (r < 45) {
            int c_id = cdist(rng);
            int num_items = olcount_dist(rng);

            int item_ids[15];
            int supplier_ws[15];
            int quantities[15];

            for (int i = 0; i < num_items; i++) {
                int r2 = (int)(rng() % 100);
                item_ids[i] = item_dist(rng);
                for (int j = 0; j < i; j++) {
                    if (item_ids[j] == item_ids[i]) {
                        item_ids[i] = item_dist(rng);
                        j = -1;
                    }
                }
                if (r2 < 1 && g_num_warehouses > 1) {
                    int other_w = wdist(rng);
                    while (other_w == w_id) other_w = wdist(rng);
                    supplier_ws[i] = other_w;
                } else {
                    supplier_ws[i] = w_id;
                }
                quantities[i] = qty_dist(rng);
            }

            tx_run([&]() {
                txn_new_order(w_id, d_id, c_id, num_items, item_ids, supplier_ws, quantities);
            });
            g_counts[0].fetch_add(1);

        } else if (r < 88) {
            int pay_w_id = w_id;
            if ((rng() % 100) < 15 && g_num_warehouses > 1) {
                do { pay_w_id = wdist(rng); } while (pay_w_id == w_id);
            }
            int c_id = cdist(rng);
            float amount = 100.00f + (float)(rng() % 9900);
            tx_run([&]() {
                txn_payment(pay_w_id, d_id, c_id, amount);
            });
            g_counts[1].fetch_add(1);

        } else if (r < 92) {
            int c_id = cdist(rng);
            tx_run([&]() {
                txn_order_status(w_id, d_id, c_id);
            });
            g_counts[2].fetch_add(1);

        } else if (r < 96) {
            int carrier_id = (int)(rng() % 10) + 1;
            tx_run([&]() {
                txn_delivery(w_id, carrier_id);
            });
            g_counts[3].fetch_add(1);

        } else {
            int threshold = threshold_dist(rng);
            tx_run([&]() {
                txn_stock_level(w_id, d_id, threshold);
            });
            g_counts[4].fetch_add(1);
        }

        g_total_ops.fetch_add(1);
    }

    tm_exit_thread();
}

static void test_cli_flags() {
    printf("  Testing CLI flags...\n");
    int save_t = g_num_threads, save_d = g_duration, save_w = g_num_warehouses;
    TEST_EQ(g_num_threads, 4, "default threads");
    TEST_EQ(g_duration, 10000, "default duration");
    TEST_EQ(g_num_warehouses, 1, "default warehouses");
    const char* test_args[] = {"prog", "-t", "2", "-d", "500", "-w", "3"};
    parse_args(7, (char**)test_args);
    TEST_EQ(g_num_threads, 2, "override threads");
    TEST_EQ(g_duration, 500, "override duration");
    TEST_EQ(g_num_warehouses, 3, "override warehouses");
    g_num_threads = save_t; g_duration = save_d; g_num_warehouses = save_w;
    if (test_result() != 0) exit(1);
}

static void test_rng() {
    printf("  Testing RNG determinism...\n");
    test_rng_determinism<std::mt19937>();
    if (test_result() != 0) exit(1);
}

static void test_logic() {
    printf("  Testing TPC-C logic...\n");
    // Test warehouse data generation (no TM needed)
    int save_w = g_num_warehouses;
    g_num_warehouses = 1;
    tm_init();

    g_warehouse = (Warehouse*)tm_calloc((size_t)g_num_warehouses, sizeof(Warehouse));
    g_district  = (District*)tm_calloc((size_t)(g_num_warehouses * g_num_districts), sizeof(District));
    g_customer  = (Customer*)tm_calloc((size_t)(g_num_warehouses * g_num_districts * g_num_customers), sizeof(Customer));
    g_history   = (History*)tm_calloc(MAX_HISTORY_ROWS, sizeof(History));
    g_history_count = (int*)tm_calloc(1, sizeof(int));
    int total_order_slots = g_num_warehouses * g_num_districts * MAX_ORDERS_PER_DISTRICT;
    g_order     = (Order*)tm_calloc((size_t)total_order_slots, sizeof(Order));
    g_neworder  = (NewOrder*)tm_calloc((size_t)total_order_slots, sizeof(NewOrder));
    int total_ol_slots = total_order_slots * MAX_OL_PER_ORDER;
    g_orderline = (OrderLine*)tm_calloc((size_t)total_ol_slots, sizeof(OrderLine));
    g_item      = (Item*)tm_calloc((size_t)g_num_items, sizeof(Item));
    g_stock     = (Stock*)tm_calloc((size_t)(g_num_warehouses * g_num_items), sizeof(Stock));

    init_data();

    TEST_EQ(g_warehouse[0].w_id, 1, "warehouse id");
    TEST_EQ(g_district[0].d_id, 1, "district id");
    TEST_EQ(g_item[0].i_id, 1, "item id");
    TEST_ASSERT(g_item[0].i_price > 0, "item price > 0");
    TEST_EQ(g_stock[0].s_quantity, 100, "stock quantity");
    TEST_ASSERT(g_customer[0].c_id == 1, "customer id");

    tm_exit();
    g_num_warehouses = save_w;
    if (test_result() != 0) exit(1);
}

// ── Main ───────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    if (argc > 1 && strcmp(argv[1], "--test") == 0) {
        printf("Running self-tests for tpcc...\n");
        test_cli_flags();
        test_rng();
        test_logic();
        printf("All tests passed.\n");
        return 0;
    }
    parse_args(argc, argv);

    tm_init();

    // Allocate flat arrays in TM region
    g_warehouse = (Warehouse*)tm_calloc((size_t)g_num_warehouses, sizeof(Warehouse));
    g_district  = (District*)tm_calloc((size_t)(g_num_warehouses * g_num_districts), sizeof(District));
    g_customer  = (Customer*)tm_calloc((size_t)(g_num_warehouses * g_num_districts * g_num_customers), sizeof(Customer));
    g_history   = (History*)tm_calloc(MAX_HISTORY_ROWS, sizeof(History));
    g_history_count = (int*)tm_calloc(1, sizeof(int));
    int total_order_slots = g_num_warehouses * g_num_districts * MAX_ORDERS_PER_DISTRICT;
    g_order     = (Order*)tm_calloc((size_t)total_order_slots, sizeof(Order));
    g_neworder  = (NewOrder*)tm_calloc((size_t)total_order_slots, sizeof(NewOrder));
    int total_ol_slots = total_order_slots * MAX_OL_PER_ORDER;
    g_orderline = (OrderLine*)tm_calloc((size_t)total_ol_slots, sizeof(OrderLine));
    g_item      = (Item*)tm_calloc((size_t)g_num_items, sizeof(Item));
    g_stock     = (Stock*)tm_calloc((size_t)(g_num_warehouses * g_num_items), sizeof(Stock));

    printf("========= TPC-C Benchmark (v5.11) =========\n");
    printf("============================================\n");
    printf("Configuration:\n");
    printf("  Warehouses: %d\n", g_num_warehouses);
    printf("  Districts:  %d\n", g_num_warehouses * g_num_districts);
    printf("  Customers:  %d per district\n", g_num_customers);
    printf("  Items:      %d\n", g_num_items);
    printf("  Orders:     %d per district (pre-populated)\n", PREPOPULATED_ORDERS);
    printf("  Threads:    %d\n", g_num_threads);
    printf("  Duration:   %d ms\n\n", g_duration);

    printf("Initializing data...\n");
    init_data();
    printf("  Warehouses: %d\n", g_num_warehouses);
    printf("  Districts:  %d\n", g_num_warehouses * g_num_districts);
    printf("  Customers:  %d\n", g_num_warehouses * g_num_districts * g_num_customers);
    printf("  Items:      %d\n", g_num_items);
    printf("  Done\n\n");

    int loops = g_duration / 10;
    auto t1 = std::chrono::steady_clock::now();

    std::vector<std::thread> threads_v;
    for (int t = 0; t < g_num_threads; t++)
        threads_v.emplace_back(run_worker, t, loops);

    std::this_thread::sleep_for(std::chrono::milliseconds(g_duration));
    g_done = true;

    for (auto& th : threads_v) th.join();
    auto t2 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double>(t2 - t1).count() * 1000;

    uint64_t ops = g_total_ops.load();

    printf("Results\n");
    printf("=======\n");
    printf("Elapsed:       %.0f ms\n", ms);
    printf("Total ops:     %llu\n", (unsigned long long)ops);
    printf("Ops/sec:       %.0f\n\n", ops / (ms / 1000.0));

    printf("Transaction breakdown:\n");
    printf("  New-Order:     %d\n", g_counts[0].load());
    printf("  Payment:       %d\n", g_counts[1].load());
    printf("  Order-Status:  %d\n", g_counts[2].load());
    printf("  Delivery:      %d\n", g_counts[3].load());
    printf("  Stock-Level:   %d\n", g_counts[4].load());

    tm_exit();
    return 0;
}
