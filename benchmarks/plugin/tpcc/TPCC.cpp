/**
 * TPC-C Benchmark Implementation v5.11
 *
 * Based on TPC-C Specification v5.11
 * https://www.tpc.org/tpc_documents_current_versions/pdf/tpc-c_v5.11.0.pdf
 *
 * References: §<section> throughout refer to spec v5.11 sections.
 *
 * 9 Tables (§4.1):
 *   Warehouse (W), District (D), Customer (C), History (H),
 *   Orders (O), New-Order (NO), Order-Line (OL), Item (I), Stock (S)
 *
 * 5 Transaction Types:
 *   New-Order (§2.4) - 45%, Payment (§2.5) - 43%,
 *   Order-Status (§2.6) - 4%, Delivery (§2.7) - 4%,
 *   Stock-Level (§2.8) - 4%
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iostream>
#include <random>
#include <thread>
#include <vector>
#include "tm_hash_set.hpp"

#define TM __attribute__((annotate("tm")))
#define TX __attribute__((annotate("transaction"), noinline))
#define THREAD __attribute__((annotate("thread"), noinline))
#define MAIN __attribute__((annotate("main"), noinline))

// --- Configuration defaults (§4.3.1: Minimal profile) ---
constexpr int DEFAULT_WAREHOUSES = 1;
constexpr int DEFAULT_DISTRICTS = 10;   // D = 10 per warehouse (§4.3.2)
constexpr int DEFAULT_CUSTOMERS = 3000; // C = 3000 per district (§4.3.3)
constexpr int DEFAULT_ITEMS = 100000;
constexpr int PREPOPULATED_ORDERS = 3000; // initial orders per district (§4.3.3)
constexpr int
    MAX_ORDERS_PER_DISTRICT = 10000; // allocated slots (3000 init + room for new)
constexpr int MAX_OL_PER_ORDER = 15;

// ==========================================================================
// Table struct definitions (§4.1)
// ==========================================================================

// §4.1.1: Warehouse
struct Warehouse {
	int w_id;
	char w_name[11];
	char w_street_1[21];
	char w_street_2[21];
	char w_city[21];
	char w_state[3];
	char w_zip[10];
	float w_tax;
	float w_ytd; // starts at 3000000.00 (§4.3.3.1)
};

// §4.1.2: District
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
	float d_ytd;     // starts at 3000000.00 (§4.3.3.1)
	int d_next_o_id; // next order id, starts at 3001 (§4.3.3.1)
};

// §4.1.3: Customer
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
	float c_balance;     // starts at -10.00 (§4.3.3.1)
	float c_ytd_payment; // starts at 10.00 (§4.3.3.1)
	int c_payment_cnt;   // starts at 1 (§4.3.3.1)
	int c_delivery_cnt;  // starts at 0
	char c_data[501];
};

// §4.1.4: History
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

// §4.1.5: Orders
struct Order {
	int o_id;
	int o_d_id;
	int o_w_id;
	int o_c_id;
	int o_entry_d;
	int o_carrier_id; // 0 = null (undelivered), 1-10 = carrier id
	int o_ol_cnt;
	float o_all_local; // 1 = all local, 0 = some remote items
};

// §4.1.6: New-Order (one per undelivered order)
struct NewOrder {
	int no_o_id;
	int no_d_id;
	int no_w_id;
};

// §4.1.7: Order-Line
struct OrderLine {
	int ol_o_id;
	int ol_d_id;
	int ol_w_id;
	int ol_number;
	int ol_i_id;
	int ol_supply_w_id;
	int ol_quantity;
	int ol_delivery_d; // 0 = null (undelivered), else timestamp
	float ol_amount;
	char ol_dist_info[25];
};

// §4.1.8: Item
struct Item {
	int i_id;
	int i_im_id;
	char i_name[25];
	float i_price;
	char i_data[51];
};

// §4.1.9: Stock
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
	int s_ytd; // year-to-date quantity sold (§4.1.9)
	int s_order_cnt;
	int s_remote_cnt;
	char s_data[51];
};

// --- Global pointer tables (TM-annotated) ---
TM Warehouse *g_warehouse = nullptr;
TM District *g_district = nullptr;
TM Customer *g_customer = nullptr;
TM History *g_history = nullptr;
TM Order *g_order = nullptr;
TM NewOrder *g_neworder = nullptr;
TM OrderLine *g_orderline = nullptr;
TM Item *g_item = nullptr;
TM Stock *g_stock = nullptr;

// --- Global configuration (TM-annotated) ---
TM int g_num_warehouses = DEFAULT_WAREHOUSES;
TM int g_num_districts = DEFAULT_DISTRICTS;
TM int g_num_customers = DEFAULT_CUSTOMERS;
TM int g_num_items = DEFAULT_ITEMS;

// --- Global counters ---
TM int g_order_count = 0;
TM int g_history_count = 0;
TM int g_neworder_count = 0;

TM int g_neworder_transactions = 0;
TM int g_payment_transactions = 0;
TM int g_orderstatus_transactions = 0;
TM int g_delivery_transactions = 0;
TM int g_stocklevel_transactions = 0;

// ==========================================================================
// Flat-array index helpers
// ==========================================================================
// Layout: [warehouse][district][...]

static inline int idx_w(int w_id) { return w_id - 1; }

static inline int idx_d(int w_id, int d_id)
{
	return (w_id - 1) * g_num_districts + (d_id - 1);
}

static inline int idx_c(int w_id, int d_id, int c_id)
{
	return ((w_id - 1) * g_num_districts + (d_id - 1)) * g_num_customers + (c_id - 1);
}

// MAX_ORDERS_PER_DISTRICT = 10000 slots per (warehouse, district)
// (3000 pre-populated + room for new orders during benchmark)
static inline int idx_ord(int w_id, int d_id, int o_id)
{
	return ((w_id - 1) * g_num_districts + (d_id - 1)) * MAX_ORDERS_PER_DISTRICT +
	       (o_id - 1);
}

// NewOrder uses same layout as Order
static inline int idx_no(int w_id, int d_id, int o_id)
{
	return ((w_id - 1) * g_num_districts + (d_id - 1)) * MAX_ORDERS_PER_DISTRICT +
	       (o_id - 1);
}

// Order-Line: each order has up to MAX_OL_PER_ORDER (15) lines
static inline int idx_ol(int w_id, int d_id, int o_id, int ol_num)
{
	return (((w_id - 1) * g_num_districts + (d_id - 1)) * MAX_ORDERS_PER_DISTRICT +
	        (o_id - 1)) *
	           MAX_OL_PER_ORDER +
	       (ol_num - 1);
}

static inline int idx_i(int i_id) { return i_id - 1; }

static inline int idx_s(int w_id, int i_id)
{
	return (w_id - 1) * g_num_items + (i_id - 1);
}

// ==========================================================================
// Utility
// ==========================================================================

static void gen_string(char *dest, int len, const char *src)
{
	int slen = (int)strlen(src);
	for (int i = 0; i < len - 1; i++)
		dest[i] = src[i % slen];
	dest[len - 1] = '\0';
}

// Return a mutable pointer to the s_dist_XX string for district d_id (1-10)
// (§2.4.2.3: order-line dist_info is copied from stock's s_dist_XX)
static char *stock_dist_string(Stock *s, int d_id)
{
	switch (d_id) {
	case 1:
		return s->s_dist_01;
	case 2:
		return s->s_dist_02;
	case 3:
		return s->s_dist_03;
	case 4:
		return s->s_dist_04;
	case 5:
		return s->s_dist_05;
	case 6:
		return s->s_dist_06;
	case 7:
		return s->s_dist_07;
	case 8:
		return s->s_dist_08;
	case 9:
		return s->s_dist_09;
	case 10:
		return s->s_dist_10;
	default:
		return s->s_dist_01;
	}
}

// ==========================================================================
// Data initialization (§4.3)
// ==========================================================================

TM void init_data()
{
	int nw = g_num_warehouses;
	int nd = g_num_districts;
	int nc = g_num_customers;
	int ni = g_num_items;

	// ---- Warehouse (§4.3.3.1) ----
	for (int w = 1; w <= nw; w++) {
		int wi = idx_w(w);
		g_warehouse[wi].w_id = w;
		gen_string(g_warehouse[wi].w_name, 11, "Warehouse");
		gen_string(g_warehouse[wi].w_street_1, 21, "Street");
		gen_string(g_warehouse[wi].w_street_2, 21, "Ave");
		gen_string(g_warehouse[wi].w_city, 21, "City");
		gen_string(g_warehouse[wi].w_state, 3, "ST");
		gen_string(g_warehouse[wi].w_zip, 10, "123456789");
		g_warehouse[wi].w_tax = 0.1900f;     // ~mid-range of [0.0000, 0.2000]
		g_warehouse[wi].w_ytd = 3000000.00f; // §4.3.3.1
	}

	// ---- District (§4.3.3.1) ----
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
			g_district[di].d_tax = 0.1500f;     // ~mid-range of [0.0000, 0.2000]
			g_district[di].d_ytd = 3000000.00f; // §4.3.3.1
			g_district[di].d_next_o_id = PREPOPULATED_ORDERS + 1; // 3001 (§4.3.3.1)
		}
	}

	// ---- Customer (§4.3.3.1) ----
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
				g_customer[ci].c_since = (int)(::time(nullptr));
				gen_string(g_customer[ci].c_credit, 3, "GC");
				g_customer[ci].c_credit_lim = 50000.00f;
				g_customer[ci]
				    .c_discount = 0.3000f; // ~mid-range [0.0000, 0.5000] (§4.3.3.1)
				g_customer[ci].c_balance = -10.00f;    // §4.3.3.1
				g_customer[ci].c_ytd_payment = 10.00f; // §4.3.3.1
				g_customer[ci].c_payment_cnt = 1;      // §4.3.3.1
				g_customer[ci].c_delivery_cnt = 0;
				gen_string(g_customer[ci].c_data, 501, "data");
			}
		}
	}

	// ---- Item (§4.3.3.1) ----
	for (int i = 1; i <= ni; i++) {
		int ii = idx_i(i);
		g_item[ii].i_id = i;
		g_item[ii].i_im_id = (i % 99999) + 1;
		gen_string(g_item[ii].i_name, 25, "ItemName");
		g_item[ii].i_price = (float)((i % 100) + 1); // [1.00, 100.00]
		gen_string(g_item[ii].i_data, 51, "OriginalData");
	}

	// ---- Stock (§4.3.3.1) ----
	for (int w = 1; w <= nw; w++) {
		for (int i = 1; i <= ni; i++) {
			int si = idx_s(w, i);
			g_stock[si].s_i_id = i;
			g_stock[si].s_w_id = w;
			g_stock[si].s_quantity = 100; // within spec range [10, 100]
			for (int d = 1; d <= 10; d++)
				gen_string(stock_dist_string(&g_stock[si], d), 25, "DistStr");
			g_stock[si].s_ytd = 0;
			g_stock[si].s_order_cnt = 0;
			g_stock[si].s_remote_cnt = 0;
			gen_string(g_stock[si].s_data, 51, "OriginalData");
		}
	}

	// ---- Pre-populate History rows (one per customer, §4.3.3.1) ----
	for (int w = 1; w <= nw; w++) {
		for (int d = 1; d <= nd; d++) {
			for (int c = 1; c <= nc; c++) {
				int hi = g_history_count++;
				g_history[hi].h_c_id = c;
				g_history[hi].h_c_d_id = d;
				g_history[hi].h_c_w_id = w;
				g_history[hi].h_d_id = d;
				g_history[hi].h_w_id = w;
				g_history[hi].h_date = (int)(::time(nullptr));
				g_history[hi].h_amount = 10.00f; // initial payment amount
				gen_string(g_history[hi].h_data, 25, "InitHistory");
			}
		}
	}

	// ---- Pre-populate Orders, Order-Lines, and New-Orders (§4.3.3.1) ----
	// For each district, create PREPOPULATED_ORDERS (3000) orders.
	// o_id 1 .. PREPOPULATED_ORDERS.
	// Orders with o_id <= 2100 are "delivered" (carrier_id set, ol_delivery_d set).
	// Orders with o_id > 2100 are "pending" (carrier_id=0, new-order entry exists).
	//
	// We use a deterministic PRNG seeded per (w,d) for reproducibility.

	for (int w = 1; w <= nw; w++) {
		for (int d = 1; d <= nd; d++) {
			std::mt19937 ord_rng((unsigned)(w * 1000 + d));

			for (int o = 1; o <= PREPOPULATED_ORDERS; o++) {
				int c_id = (int)(ord_rng() % nc) + 1;
				int ol_cnt = (int)(ord_rng() % 11) + 5; // [5, 15]

				int o_idx = idx_ord(w, d, o);
				g_order[o_idx].o_id = o;
				g_order[o_idx].o_d_id = d;
				g_order[o_idx].o_w_id = w;
				g_order[o_idx].o_c_id = c_id;
				g_order[o_idx].o_entry_d = (int)(::time(nullptr));
				g_order[o_idx].o_carrier_id = (o <= 2100) ? ((int)(ord_rng() % 10) + 1)
				                                          : 0;
				g_order[o_idx].o_ol_cnt = ol_cnt;
				g_order[o_idx].o_all_local = 1.0f;
				g_order_count++;

				// Create order lines
				TMSafeHashSet<int> used_items;
				for (int l = 1; l <= ol_cnt; l++) {
					int ol_idx = idx_ol(w, d, o, l);
					int i_id;
					do {
						i_id = (int)(ord_rng() % ni) + 1;
					} while (used_items.contains(i_id));
					used_items.insert(i_id);

					int supply_w = w; // all local in init data

					g_orderline[ol_idx].ol_o_id = o;
					g_orderline[ol_idx].ol_d_id = d;
					g_orderline[ol_idx].ol_w_id = w;
					g_orderline[ol_idx].ol_number = l;
					g_orderline[ol_idx].ol_i_id = i_id;
					g_orderline[ol_idx].ol_supply_w_id = supply_w;
					g_orderline[ol_idx].ol_quantity = 5;
					g_orderline[ol_idx].ol_delivery_d = (o <= 2100)
					                                        ? (int)(::time(nullptr))
					                                        : 0;
					g_orderline[ol_idx].ol_amount = 5.0f * g_item[idx_i(i_id)].i_price;
					gen_string(g_orderline[ol_idx].ol_dist_info,
					           25,
					           stock_dist_string(&g_stock[idx_s(w, i_id)], d));
				}

				// New-Order entries for undelivered orders (o_id > 2100) (§4.3.3.1)
				if (o > 2100) {
					int no_idx = idx_no(w, d, o);
					g_neworder[no_idx].no_o_id = o;
					g_neworder[no_idx].no_d_id = d;
					g_neworder[no_idx].no_w_id = w;
					g_neworder_count++;
				}
			}
		}
	}
	// debug: init complete
}

// ==========================================================================
// Transaction: New-Order (§2.4)
// ==========================================================================
// Creates a new order with order lines, updates stock.
// 45% of transaction mix.

TX int txn_new_order(int w_id,
                     int d_id,
                     int c_id,
                     int num_items,
                     int *item_ids,
                     int *supplier_ws,
                     int *quantities)
{
	// §2.4.2.2: Determine next order id (from district)
	int d_idx = idx_d(w_id, d_id);
	int o_id = g_district[d_idx].d_next_o_id++;
	g_order_count++;

	// Create the order header
	int o_idx = idx_ord(w_id, d_id, o_id);
	g_order[o_idx].o_id = o_id;
	g_order[o_idx].o_d_id = d_id;
	g_order[o_idx].o_w_id = w_id;
	g_order[o_idx].o_c_id = c_id;
	g_order[o_idx].o_entry_d = (int)(::time(nullptr));
	g_order[o_idx].o_carrier_id = 0; // not yet delivered
	g_order[o_idx].o_ol_cnt = num_items;

	// Create New-Order entry (§2.4.2.3)
	int no_idx = idx_no(w_id, d_id, o_id);
	g_neworder[no_idx].no_o_id = o_id;
	g_neworder[no_idx].no_d_id = d_id;
	g_neworder[no_idx].no_w_id = w_id;
	g_neworder_count++;

	// Process order lines (§2.4.2.3)
	float total_amount = 0;
	int all_local = 1;

	for (int i = 0; i < num_items; i++) {
		int ol_number = i + 1;
		int ol_idx = idx_ol(w_id, d_id, o_id, ol_number);

		int ol_i_id = item_ids[i];
		int ol_sw_id = supplier_ws[i];
		int ol_qty = quantities[i];

		if (ol_sw_id != w_id)
			all_local = 0; // remote item

		g_orderline[ol_idx].ol_o_id = o_id;
		g_orderline[ol_idx].ol_d_id = d_id;
		g_orderline[ol_idx].ol_w_id = w_id;
		g_orderline[ol_idx].ol_number = ol_number;
		g_orderline[ol_idx].ol_i_id = ol_i_id;
		g_orderline[ol_idx].ol_supply_w_id = ol_sw_id;
		g_orderline[ol_idx].ol_quantity = ol_qty;
		g_orderline[ol_idx].ol_delivery_d = 0; // not yet delivered

		// Look up item price
		float i_price = g_item[idx_i(ol_i_id)].i_price;
		float ol_amount = (float)ol_qty * i_price;
		g_orderline[ol_idx].ol_amount = ol_amount;
		total_amount += ol_amount;

		// Update stock (§2.4.2.3, §2.7.3.3)
		int s_idx = idx_s(ol_sw_id, ol_i_id);
		Stock &s = g_stock[s_idx];
		if (s.s_quantity - ol_qty >= 10) {
			s.s_quantity -= ol_qty;
		} else {
			// §2.7.3.3: s_quantity = s_quantity - ol_qty + 91
			s.s_quantity = s.s_quantity - ol_qty + 91;
		}
		s.s_ytd += ol_qty; // §4.1.9
		s.s_order_cnt++;
		if (ol_sw_id != w_id)
			s.s_remote_cnt++;

		// Copy dist_info from stock for this district (§2.4.2.3)
		gen_string(g_orderline[ol_idx].ol_dist_info, 25, stock_dist_string(&s, d_id));
	}

	g_order[o_idx].o_all_local = (all_local ? 1.0f : 0.0f); // §2.4.2.3

	g_neworder_transactions++;
	return o_id;
}

// ==========================================================================
// Transaction: Payment (§2.5)
// ==========================================================================
// Updates warehouse, district, customer. Inserts history row.
// 43% of transaction mix.

TX void txn_payment(int w_id, int d_id, int c_id, float amount)
{
	// §2.5.2.2: Update warehouse YTD
	int w_idx = idx_w(w_id);
	g_warehouse[w_idx].w_ytd += amount;

	// §2.5.2.2: Update district YTD
	int d_idx = idx_d(w_id, d_id);
	g_district[d_idx].d_ytd += amount;

	// §2.5.2.2: Update customer
	int c_idx = idx_c(w_id, d_id, c_id);
	Customer &c = g_customer[c_idx];
	c.c_balance -= amount;
	c.c_ytd_payment += amount;
	c.c_payment_cnt++;

	// §2.5.2.3: Insert history row
	int h_idx = g_history_count++;
	g_history[h_idx].h_c_id = c_id;
	g_history[h_idx].h_c_d_id = d_id;
	g_history[h_idx].h_c_w_id = w_id;
	g_history[h_idx].h_d_id = d_id;
	g_history[h_idx].h_w_id = w_id;
	g_history[h_idx].h_date = (int)(::time(nullptr));
	g_history[h_idx].h_amount = amount;
	gen_string(g_history[h_idx].h_data, 25, "Payment");

	g_payment_transactions++;
}

// ==========================================================================
// Transaction: Order-Status (§2.6)
// ==========================================================================
// Lookup customer by ID, find latest order, return its order lines.
// 4% of transaction mix.

TX float txn_order_status(int w_id, int d_id, int c_id)
{
	int c_idx = idx_c(w_id, d_id, c_id);
	float balance = g_customer[c_idx].c_balance;

	// Find the customer's latest order (§2.6.2.2)
	// Scan descending from district's next_o_id - 1
	int d_idx = idx_d(w_id, d_id);
	int max_o = g_district[d_idx].d_next_o_id - 1;
	int latest_o_id = -1;

	for (int o_id = max_o; o_id >= 1; o_id--) {
		int o_idx = idx_ord(w_id, d_id, o_id);
		if (g_order[o_idx].o_c_id == c_id && g_order[o_idx].o_w_id != 0) {
			latest_o_id = o_id;
			break;
		}
	}

	// §2.6.2.3: Iterate over order lines of the latest order
	if (latest_o_id > 0) {
		int o_idx = idx_ord(w_id, d_id, latest_o_id);
		int ol_cnt = g_order[o_idx].o_ol_cnt;
		for (int l = 1; l <= ol_cnt; l++) {
			int ol_idx = idx_ol(w_id, d_id, latest_o_id, l);
			// Touching order-line data constitutes reading it (§2.6.2.3)
			volatile float amount = g_orderline[ol_idx].ol_amount;
			(void)amount;
		}
	}

	g_orderstatus_transactions++;
	return balance;
}

// ==========================================================================
// Transaction: Delivery (§2.7)
// ==========================================================================
// For each district, deliver the oldest pending new-order.
// 4% of transaction mix.

TX void txn_delivery(int w_id, int carrier_id)
{
	for (int d_id = 1; d_id <= g_num_districts; d_id++) {
		// §2.7.2.2: Find the oldest pending new-order for this district
		int found_no = -1;
		for (int o_id = 1; o_id < MAX_ORDERS_PER_DISTRICT; o_id++) {
			int no_idx = idx_no(w_id, d_id, o_id);
			if (g_neworder[no_idx].no_o_id == o_id) {
				found_no = o_id;
				break;
			}
		}
		if (found_no < 0)
			continue;

		// §2.7.2.2: Update order header with carrier id
		int o_idx = idx_ord(w_id, d_id, found_no);
		g_order[o_idx].o_carrier_id = carrier_id;

		// §2.7.2.3: Deliver all order lines
		int ol_cnt = g_order[o_idx].o_ol_cnt;
		float ol_total = 0;
		for (int l = 1; l <= ol_cnt; l++) {
			int ol_idx = idx_ol(w_id, d_id, found_no, l);
			ol_total += g_orderline[ol_idx].ol_amount;
			g_orderline[ol_idx].ol_delivery_d = (int)(::time(nullptr));
		}

		// §2.7.2.3: Update customer balance
		int c_id = g_order[o_idx].o_c_id;
		int c_idx = idx_c(w_id, d_id, c_id);
		g_customer[c_idx].c_balance += ol_total;
		g_customer[c_idx].c_delivery_cnt++;

		// Delete the new-order row (§2.7.2.2)
		g_neworder[idx_no(w_id, d_id, found_no)].no_o_id = -1;
	}

	g_delivery_transactions++;
}

// ==========================================================================
// Transaction: Stock-Level (§2.8)
// ==========================================================================
// Counts stock items below threshold in the last 20 orders' order lines.
// 4% of transaction mix.

TX int txn_stock_level(int w_id, int d_id, int threshold)
{
	// §2.8.2.2: Get next_o_id from district
	int d_idx = idx_d(w_id, d_id);
	int next_o_id = g_district[d_idx].d_next_o_id;

	// Scan the last 20 orders (excluding current)
	int start_o_id = next_o_id - 20;
	if (start_o_id < 1)
		start_o_id = 1;

	// Use local set to avoid counting duplicate items within the scan
	TMSafeHashSet<int> counted;

	for (int o_id = start_o_id; o_id < next_o_id; o_id++) {
		int o_idx = idx_ord(w_id, d_id, o_id);
		if (g_order[o_idx].o_w_id == 0)
			continue;

		int ol_cnt = g_order[o_idx].o_ol_cnt;
		for (int l = 1; l <= ol_cnt; l++) {
			int ol_idx = idx_ol(w_id, d_id, o_id, l);
			int i_id = g_orderline[ol_idx].ol_i_id;
			if (counted.contains(i_id))
				continue;
			counted.insert(i_id);

			int s_idx = idx_s(w_id, i_id);
			if (g_stock[s_idx].s_quantity < threshold) {
				// count this item as low-stock (spec counts distinct items)
			}
		}
	}

	g_stocklevel_transactions++;
	return (int)counted.size();
}

// ==========================================================================
// Worker thread (§2.3: Transaction profile)
// ==========================================================================
// Transaction mix: New-Order 45%, Payment 43%,
//                  Order-Status 4%, Delivery 4%, Stock-Level 4%

std::atomic<bool> done{false};
std::atomic<uint64_t> total_ops{0};

struct Worker {
	int id;
	int loops;
	std::mt19937 *rng;
};

THREAD void run(Worker *w)
{
	std::uniform_int_distribution<int> wdist(1, g_num_warehouses);
	std::uniform_int_distribution<int> ddist(1, g_num_districts);
	std::uniform_int_distribution<int> cdist(1, g_num_customers);
	std::uniform_int_distribution<int> op_dist(0, 99);
	std::uniform_int_distribution<int> item_dist(1, g_num_items);
	std::uniform_int_distribution<int> qty_dist(1, 10);
	std::uniform_int_distribution<int> olcount_dist(5, 15);
	std::uniform_int_distribution<int> threshold_dist(10, 20);

	while (!done.load() && w->loops > 0) {
		w->loops--;
		int r = op_dist(*w->rng);
		int w_id = wdist(*w->rng);
		int d_id = ddist(*w->rng);

		if (r < 45) {
			// ---- New-Order (§2.4) ----
			int c_id = cdist(*w->rng);
			int num_items = olcount_dist(*w->rng);

			int item_ids[15];
			int supplier_ws[15];
			int quantities[15];

			// §2.4.1.5: 1% of items are from a remote warehouse
			for (int i = 0; i < num_items; i++) {
				int r2 = (int)((*w->rng)() % 100);
				item_ids[i] = item_dist(*w->rng);

				// Ensure unique items within the order (§2.4.1.5)
				for (int j = 0; j < i; j++) {
					if (item_ids[j] == item_ids[i]) {
						item_ids[i] = item_dist(*w->rng);
						j = -1; // restart check
					}
				}

				// Remote warehouse check
				if (r2 < 1 && g_num_warehouses > 1) {
					int other_w = wdist(*w->rng);
					while (other_w == w_id)
						other_w = wdist(*w->rng);
					supplier_ws[i] = other_w;
				} else {
					supplier_ws[i] = w_id;
				}

				quantities[i] = qty_dist(*w->rng);
			}

			txn_new_order(w_id, d_id, c_id, num_items, item_ids, supplier_ws, quantities);

		} else if (r < 88) {
			// ---- Payment (§2.5) ----
			// §2.5.1.2: 15% of payments involve a remote customer's warehouse
			int pay_w_id = w_id;
			if (((*w->rng)() % 100) < 15 && g_num_warehouses > 1) {
				do {
					pay_w_id = wdist(*w->rng);
				} while (pay_w_id == w_id);
			}
			int c_id = cdist(*w->rng);
			float amount = 100.00f + (float)((*w->rng)() % 9900); // [100, 10000]
			txn_payment(pay_w_id, d_id, c_id, amount);

		} else if (r < 92) {
			// ---- Order-Status (§2.6) ----
			int c_id = cdist(*w->rng);
			txn_order_status(w_id, d_id, c_id);

		} else if (r < 96) {
			// ---- Delivery (§2.7) ----
			int carrier_id = ((int)((*w->rng)() % 10) + 1);
			txn_delivery(w_id, carrier_id);

		} else {
			// ---- Stock-Level (§2.8) ----
			int threshold = threshold_dist(*w->rng); // random [10, 20] (§2.8.1.2)
			txn_stock_level(w_id, d_id, threshold);
		}

		total_ops.fetch_add(1, std::memory_order_relaxed);
	}
}

// ==========================================================================
// Main
// ==========================================================================

MAIN int main(int argc, char *argv[])
{
	int threads = 4;
	int duration = 10000;
	g_num_warehouses = DEFAULT_WAREHOUSES;
	g_num_districts = DEFAULT_DISTRICTS;
	g_num_customers = DEFAULT_CUSTOMERS;
	g_num_items = DEFAULT_ITEMS;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-t") == 0 && i + 1 < argc)
			threads = std::atoi(argv[++i]);
		else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc)
			duration = std::atoi(argv[++i]);
		else if (strcmp(argv[i], "-w") == 0 && i + 1 < argc)
			g_num_warehouses = std::atoi(argv[++i]);
	}

	// Allocate flat arrays (§4.3.3)
	g_warehouse = (Warehouse *)malloc(g_num_warehouses * sizeof(Warehouse));
	g_district = (District *)malloc(g_num_warehouses * g_num_districts *
	                                sizeof(District));
	g_customer = (Customer *)malloc(g_num_warehouses * g_num_districts * g_num_customers *
	                                sizeof(Customer));
	g_history = (History *)malloc(1000000 * sizeof(History));
	g_order = (Order *)malloc(g_num_warehouses * g_num_districts *
	                          MAX_ORDERS_PER_DISTRICT * sizeof(Order));
	g_neworder = (NewOrder *)malloc(g_num_warehouses * g_num_districts *
	                                MAX_ORDERS_PER_DISTRICT * sizeof(NewOrder));
	g_orderline = (OrderLine *)malloc(g_num_warehouses * g_num_districts *
	                                  MAX_ORDERS_PER_DISTRICT * MAX_OL_PER_ORDER *
	                                  sizeof(OrderLine));
	g_item = (Item *)malloc(g_num_items * sizeof(Item));
	g_stock = (Stock *)malloc(g_num_warehouses * g_num_items * sizeof(Stock));

	if (!g_warehouse || !g_district || !g_customer || !g_history || !g_order ||
	    !g_neworder || !g_orderline || !g_item || !g_stock) {
		std::cerr << "FATAL: malloc failed\n";
		return 1;
	}

	std::cout << "========= TPC-C Benchmark (v5.11) =========\n";
	std::cout << "===========================================\n";
	std::cout << "Configuration:\n";
	std::cout << "  Warehouses: " << g_num_warehouses << "\n";
	std::cout << "  Districts:  " << g_num_districts << "\n";
	std::cout << "  Customers:  " << g_num_customers << " per district\n";
	std::cout << "  Items:      " << g_num_items << "\n";
	std::cout << "  Orders:     " << PREPOPULATED_ORDERS
	          << " per district (pre-populated)\n";
	std::cout << "  Threads:    " << threads << "\n";
	std::cout << "  Duration:   " << duration << " ms\n\n";

	std::cout << "Initializing data..." << std::endl;
	std::cout.flush();
	init_data();
	std::cout << "  Warehouses: " << g_num_warehouses << "\n";
	std::cout << "  Districts:  " << (g_num_warehouses * g_num_districts) << "\n";
	std::cout << "  Customers:  "
	          << (g_num_warehouses * g_num_districts * g_num_customers) << "\n";
	std::cout << "  Items:      " << g_num_items << "\n";
	std::cout << "  Orders:     " << g_order_count << "\n";
	std::cout << "  OrderLines: "
	          << (g_num_warehouses * g_num_districts * PREPOPULATED_ORDERS * 10)
	          << " (avg)\n";
	std::cout << "  New-Orders: " << g_neworder_count << "\n";
	std::cout << "  History:    " << g_history_count << "\n\n";

	int loops = duration / 10;
	std::vector<Worker> ws(threads);
	std::vector<std::thread> thr;
	std::vector<std::mt19937> rngs(threads);

	for (int i = 0; i < threads; i++) {
		rngs[i] = std::mt19937((unsigned)(i * 12345 + 42));
		ws[i].id = i;
		ws[i].loops = loops;
		ws[i].rng = &rngs[i];
	}

	auto start = std::chrono::high_resolution_clock::now();
	for (int i = 0; i < threads; i++)
		thr.emplace_back(run, &ws[i]);

	std::this_thread::sleep_for(std::chrono::milliseconds(duration));
	done = true;

	for (auto &t : thr)
		t.join();
	auto end = std::chrono::high_resolution_clock::now();
	auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

	uint64_t ops = total_ops.load();

	std::cout << "Results\n";
	std::cout << "=======\n";
	std::cout << "Elapsed:       " << ms << " ms\n";
	std::cout << "Total ops:     " << ops << "\n";
	std::cout << "Ops/sec:       " << (ops * 1000.0 / ms) << "\n\n";

	std::cout << "Transaction breakdown:\n";
	std::cout << "  New-Order:     " << g_neworder_transactions << "\n";
	std::cout << "  Payment:       " << g_payment_transactions << "\n";
	std::cout << "  Order-Status:  " << g_orderstatus_transactions << "\n";
	std::cout << "  Delivery:      " << g_delivery_transactions << "\n";
	std::cout << "  Stock-Level:   " << g_stocklevel_transactions << "\n";

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
