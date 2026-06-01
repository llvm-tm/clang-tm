// ── TPC-C Benchmark v5.11 (Rust/TM port) ───────────────────────────────
//
// 9 tables, 5 transaction types. Based on TPC-C spec v5.11.
//
// Tables: Warehouse, District, Customer, History, Order, NewOrder,
//         OrderLine, Item, Stock.

use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::Arc;

use tm::{transaction, TmCell, Transaction};

// ── Constants ─────────────────────────────────────────────────────────

const DEFAULT_WAREHOUSES: usize = 1;
const DEFAULT_DISTRICTS: usize = 10;
const DEFAULT_CUSTOMERS: usize = 3000;
const DEFAULT_ITEMS: usize = 100000;
const MAX_ORDERS_PER_DISTRICT: usize = 10000;
const MAX_OL_PER_ORDER: usize = 15;

// ── Table structs (mutable fields are TmCell<T>) ─────────────────────

struct Warehouse {
    w_id: i32,
    w_tax: TmCell<f64>,
    w_ytd: TmCell<f64>,
}

struct District {
    d_id: i32, d_w_id: i32,
    d_tax: TmCell<f64>,
    d_ytd: TmCell<f64>,
    d_next_o_id: TmCell<i32>,
}

struct Customer {
    c_id: i32, c_d_id: i32, c_w_id: i32,
    c_credit_lim: TmCell<f64>,
    c_discount: TmCell<f64>,
    c_balance: TmCell<f64>,
    c_ytd_payment: TmCell<f64>,
    c_payment_cnt: TmCell<i32>,
    c_delivery_cnt: TmCell<i32>,
}

// History: single amount field tracked per entry
struct History {
    h_amount: TmCell<f64>,
}

// Order header — immutable after insert except carrier_id
struct Order {
    o_id: i32, o_d_id: i32, o_w_id: i32, o_c_id: i32,
    o_carrier_id: TmCell<i32>,
    o_ol_cnt: i32,
}

// New-Order: all three immutable, but created/deleted atomically via row presence
struct NewOrder {
    no_o_id: TmCell<i32>,
    no_d_id: TmCell<i32>,
    no_w_id: TmCell<i32>,
}

// Order-Line — mutable: ol_amount, ol_delivery_d
struct OrderLine {
    ol_o_id: i32, ol_d_id: i32, ol_w_id: i32,
    ol_number: i32, ol_i_id: i32,
    ol_amount: TmCell<f64>,
    ol_delivery_d: TmCell<i32>,
}

// Item — read-only after load (price immutable)
struct Item {
    i_id: i32,
    i_price: f64,
}

// Stock — mutable: quantity, ytd, order_cnt, remote_cnt
struct Stock {
    s_i_id: i32, s_w_id: i32,
    s_quantity: TmCell<i32>,
    s_ytd: TmCell<i32>,
    s_order_cnt: TmCell<i32>,
    s_remote_cnt: TmCell<i32>,
}

// ── Database ──────────────────────────────────────────────────────────

struct TpccDatabase {
    warehouse: Vec<Warehouse>,
    district: Vec<District>,
    customer: Vec<Customer>,
    history: Vec<History>,
    order: Vec<Order>,
    neworder: Vec<NewOrder>,
    orderline: Vec<OrderLine>,
    item: Vec<Item>,
    stock: Vec<Stock>,

    num_w: usize,
    num_d: usize,
    num_c: usize,
    num_i: usize,
    no_ptr: AtomicU64,
    ol_ptr: AtomicU64,
}

impl TpccDatabase {
    fn new(num_w: usize) -> Self {
        let num_d = DEFAULT_DISTRICTS;
        let num_c = DEFAULT_CUSTOMERS;
        let num_i = DEFAULT_ITEMS;
        let total_orders = num_w * num_d * MAX_ORDERS_PER_DISTRICT;
        let total_ol = total_orders * MAX_OL_PER_ORDER;

        let warehouse = (0..num_w).map(|i| Warehouse {
            w_id: (i+1) as i32, w_tax: TmCell::new(0.19), w_ytd: TmCell::new(3000000.0),
        }).collect();

        let district = (0..num_w * num_d).map(|i| District {
            d_id: ((i % num_d) + 1) as i32, d_w_id: ((i / num_d) + 1) as i32,
            d_tax: TmCell::new(0.15), d_ytd: TmCell::new(3000000.0),
            d_next_o_id: TmCell::new((/* prepopulated */ 2101) as i32),
        }).collect();

        // Customers: prepopulate 3000 per district
        let customer = (0..num_w * num_d * num_c).enumerate().map(|(idx, _)| {
            let c_id = (idx % num_c + 1) as i32;
            Customer {
                c_id, c_d_id: ((idx / num_c) % num_d + 1) as i32,
                c_w_id: (idx / (num_c * num_d) + 1) as i32,
                c_credit_lim: TmCell::new(50000.0), c_discount: TmCell::new(0.3),
                c_balance: TmCell::new(-10.0), c_ytd_payment: TmCell::new(10.0),
                c_payment_cnt: TmCell::new(1), c_delivery_cnt: TmCell::new(0),
            }
        }).collect();

        let history = (0..total_orders.max(100000)).map(|_| History {
            h_amount: TmCell::new(10.0),
        }).collect();

        // Pre-populate 2100 orders per district with carrier_id set (delivered)
        let order = (0..total_orders).map(|idx| {
            let o_id = (idx % MAX_ORDERS_PER_DISTRICT + 1) as i32;
            let carrier = if o_id <= 2100 { (o_id % 10 + 1) } else { 0 };
            Order {
                o_id, o_carrier_id: TmCell::new(carrier),
                o_ol_cnt: (o_id % 11 + 5) as i32,
                o_d_id: ((idx / MAX_ORDERS_PER_DISTRICT) % num_d + 1) as i32,
                o_w_id: (idx / (MAX_ORDERS_PER_DISTRICT * num_d) + 1) as i32,
                o_c_id: (o_id % 3000 + 1) as i32,
            }
        }).collect();

        // NewOrder: prepopulate 900 entries (o_id 2101-3000 pending)
        let neworder = (0..total_orders).map(|idx| {
            let o_id = (idx % MAX_ORDERS_PER_DISTRICT + 1) as i32;
            if o_id > 2100 {
                NewOrder {
                    no_o_id: TmCell::new(o_id),
                    no_d_id: TmCell::new(((idx / MAX_ORDERS_PER_DISTRICT) % num_d + 1) as i32),
                    no_w_id: TmCell::new((idx / (MAX_ORDERS_PER_DISTRICT * num_d) + 1) as i32),
                }
            } else {
                NewOrder {
                    no_o_id: TmCell::new(-1), no_d_id: TmCell::new(-1), no_w_id: TmCell::new(-1),
                }
            }
        }).collect();

        // Order-Line: prepopulate for first 2100 orders
        let orderline = (0..total_ol).map(|idx| {
            let o_idx = idx / MAX_OL_PER_ORDER;
            let ol_num = (idx % MAX_OL_PER_ORDER + 1) as i32;
            let o_id = (o_idx % MAX_ORDERS_PER_DISTRICT + 1) as i32;
            let carrier = if o_id <= 2100 { 1000 } else { 0 };
            OrderLine {
                ol_o_id: o_id,
                ol_d_id: ((o_idx / MAX_ORDERS_PER_DISTRICT) % num_d + 1) as i32,
                ol_w_id: (o_idx / (MAX_ORDERS_PER_DISTRICT * num_d) + 1) as i32,
                ol_number: ol_num, ol_i_id: (o_idx % num_i + 1) as i32,
                ol_amount: TmCell::new(o_id as f64 * 5.0),
                ol_delivery_d: TmCell::new(carrier),
            }
        }).collect();

        let item = (0..num_i).map(|i| Item {
            i_id: (i+1) as i32, i_price: ((i % 100) + 1) as f64,
        }).collect();

        let stock = (0..num_w * num_i).map(|i| Stock {
            s_i_id: ((i % num_i) + 1) as i32, s_w_id: ((i / num_i) + 1) as i32,
            s_quantity: TmCell::new(100), s_ytd: TmCell::new(0),
            s_order_cnt: TmCell::new(0), s_remote_cnt: TmCell::new(0),
        }).collect();

        TpccDatabase {
            warehouse, district, customer, history, order, neworder, orderline, item, stock,
            num_w, num_d, num_c, num_i,
            no_ptr: AtomicU64::new(0), ol_ptr: AtomicU64::new(0),
        }
    }

    // Index helpers
    fn idx_w(&self, w: usize) -> usize { w - 1 }
    fn idx_d(&self, w: usize, d: usize) -> usize { (w - 1) * self.num_d + (d - 1) }
    fn idx_c(&self, w: usize, d: usize, c: usize) -> usize {
        ((w - 1) * self.num_d + (d - 1)) * self.num_c + (c - 1)
    }
    fn idx_ord(&self, w: usize, d: usize, o: usize) -> usize {
        ((w - 1) * self.num_d + (d - 1)) * MAX_ORDERS_PER_DISTRICT + (o - 1)
    }
    fn idx_no(&self, w: usize, d: usize, o: usize) -> usize {
        self.idx_ord(w, d, o)
    }
    fn idx_ol(&self, w: usize, d: usize, o: usize, l: usize) -> usize {
        (((w - 1) * self.num_d + (d - 1)) * MAX_ORDERS_PER_DISTRICT + (o - 1))
            * MAX_OL_PER_ORDER + (l - 1)
    }
    fn idx_i(&self, i: usize) -> usize { i - 1 }
    fn idx_s(&self, w: usize, i: usize) -> usize { (w - 1) * self.num_i + (i - 1) }
}

// ── RNG ───────────────────────────────────────────────────────────────

struct Rng(u64);
impl Rng {
    fn new(seed: u64) -> Self { Self(seed.wrapping_mul(6364136223846793005).wrapping_add(1442695040888963407)) }
    fn next(&mut self) -> u64 { self.0 = self.0.wrapping_mul(6364136223846793005).wrapping_add(1442695040888963407); self.0 >> 33 }
    fn range(&mut self, lo: u64, hi: u64) -> u64 { lo + self.next() % (hi - lo) }
}

// ── Transaction types ─────────────────────────────────────────────────

fn txn_new_order(db: &TpccDatabase, tx: &Transaction, w_id: usize, d_id: usize, _c_id: usize, rng: &mut Rng) -> i32 {
    let d_idx = db.idx_d(w_id, d_id);
    let next_o_id = tx.read(&db.district[d_idx].d_next_o_id);
    tx.write(&db.district[d_idx].d_next_o_id, next_o_id + 1);
    let o_id = next_o_id as usize;

    let num_items = (rng.range(5, 16)) as usize;
    let o_idx = db.idx_ord(w_id, d_id, o_id);

    // Set order carrier_id = 0 (undelivered)
    tx.write(&db.order[o_idx].o_carrier_id, 0);

    // Create new-order entry
    let no_idx = db.idx_no(w_id, d_id, o_id);
    tx.write(&db.neworder[no_idx].no_o_id, o_id as i32);
    tx.write(&db.neworder[no_idx].no_d_id, d_id as i32);
    tx.write(&db.neworder[no_idx].no_w_id, w_id as i32);

    for ol_num in 1..=num_items {
        let i_id = (rng.range(1, db.num_i as u64 + 1)) as usize;
        let ol_idx = db.idx_ol(w_id, d_id, o_id, ol_num);
        let amount = db.item[db.idx_i(i_id)].i_price * 5.0;

        // Update stock
        let s_idx = db.idx_s(w_id, i_id);
        let qty = tx.read(&db.stock[s_idx].s_quantity);
        tx.write(&db.stock[s_idx].s_quantity, if qty >= 5 { qty - 5 } else { qty - 5 + 91 });
        tx.write(&db.stock[s_idx].s_ytd, tx.read(&db.stock[s_idx].s_ytd) + 5);
        tx.write(&db.stock[s_idx].s_order_cnt, tx.read(&db.stock[s_idx].s_order_cnt) + 1);

        // Write order-line
        tx.write(&db.orderline[ol_idx].ol_amount, amount);
        tx.write(&db.orderline[ol_idx].ol_delivery_d, 0);
    }

    next_o_id
}

fn txn_payment(db: &TpccDatabase, tx: &Transaction, w_id: usize, d_id: usize, c_id: usize, amount: f64) {
    let w_idx = db.idx_w(w_id);
    tx.write(&db.warehouse[w_idx].w_ytd, tx.read(&db.warehouse[w_idx].w_ytd) + amount);

    let d_idx = db.idx_d(w_id, d_id);
    tx.write(&db.district[d_idx].d_ytd, tx.read(&db.district[d_idx].d_ytd) + amount);

    let c_idx = db.idx_c(w_id, d_id, c_id);
    tx.write(&db.customer[c_idx].c_balance, tx.read(&db.customer[c_idx].c_balance) - amount);
    tx.write(&db.customer[c_idx].c_ytd_payment, tx.read(&db.customer[c_idx].c_ytd_payment) + amount);
    tx.write(&db.customer[c_idx].c_payment_cnt, tx.read(&db.customer[c_idx].c_payment_cnt) + 1);
}

fn txn_order_status(db: &TpccDatabase, tx: &Transaction, w_id: usize, d_id: usize, c_id: usize) -> f64 {
    let c_idx = db.idx_c(w_id, d_id, c_id);
    tx.read(&db.customer[c_idx].c_balance)
}

fn txn_delivery(db: &TpccDatabase, tx: &Transaction, w_id: usize, carrier_id: i32) {
    for d_id in 1..=db.num_d {
        // Find oldest pending new-order for this district
        let mut found_o_id = None;
        for o_id in 2101..MAX_ORDERS_PER_DISTRICT {
            let no_idx = db.idx_no(w_id, d_id, o_id);
            let no_o_id = tx.read(&db.neworder[no_idx].no_o_id);
            if no_o_id == o_id as i32 {
                found_o_id = Some(o_id);
                break;
            }
        }
        let o_id = match found_o_id { Some(id) => id, None => continue };

        let o_idx = db.idx_ord(w_id, d_id, o_id);
        tx.write(&db.order[o_idx].o_carrier_id, carrier_id);

        let ol_cnt = db.order[o_idx].o_ol_cnt as usize;
        let mut ol_total = 0.0;
        for l in 1..=ol_cnt {
            let ol_idx = db.idx_ol(w_id, d_id, o_id, l);
            ol_total += tx.read(&db.orderline[ol_idx].ol_amount);
            tx.write(&db.orderline[ol_idx].ol_delivery_d, 1000);
        }

        let c_id = db.order[o_idx].o_c_id as usize;
        let c_idx = db.idx_c(w_id, d_id, c_id);
        tx.write(&db.customer[c_idx].c_balance, tx.read(&db.customer[c_idx].c_balance) + ol_total);
        tx.write(&db.customer[c_idx].c_delivery_cnt, tx.read(&db.customer[c_idx].c_delivery_cnt) + 1);

        // Delete new-order (set to -1)
        tx.write(&db.neworder[db.idx_no(w_id, d_id, o_id)].no_o_id, -1);
    }
}

fn txn_stock_level(db: &TpccDatabase, tx: &Transaction, w_id: usize, d_id: usize, threshold: i32) -> usize {
    let d_idx = db.idx_d(w_id, d_id);
    let next_o_id = tx.read(&db.district[d_idx].d_next_o_id) as usize;
    let start = if next_o_id > 20 { next_o_id - 20 } else { 1 };

    let mut below_threshold = 0;
    let mut seen = std::collections::HashSet::new();
    for o_id in start..next_o_id {
        let o_idx = db.idx_ord(w_id, d_id, o_id);
        let ol_cnt = db.order[o_idx].o_ol_cnt as usize;
        for l in 1..=ol_cnt {
            let ol_idx = db.idx_ol(w_id, d_id, o_id, l);
            let i_id = db.orderline[ol_idx].ol_i_id as usize;
            if seen.insert(i_id) {
                let qty = tx.read(&db.stock[db.idx_s(w_id, i_id)].s_quantity);
                if qty < threshold { below_threshold += 1; }
            }
        }
    }
    below_threshold
}

// ── Worker ─────────────────────────────────────────────────────────────

#[repr(C)]
struct TxCounts {
    c0: AtomicU64, c1: AtomicU64, c2: AtomicU64, c3: AtomicU64, c4: AtomicU64,
}

impl TxCounts {
    const fn new() -> Self {
        TxCounts {
            c0: AtomicU64::new(0), c1: AtomicU64::new(0), c2: AtomicU64::new(0),
            c3: AtomicU64::new(0), c4: AtomicU64::new(0),
        }
    }
}

fn worker(tid: usize, db: &TpccDatabase, stop: &AtomicBool, total_ops: &AtomicU64,
          tx_counts: &TxCounts) {
    let rng = std::cell::RefCell::new(Rng::new(tid as u64 * 12345 + 42));

    while !stop.load(Ordering::Relaxed) {
        let r = rng.borrow_mut().next() % 100;
        let w_id = (rng.borrow_mut().range(1, db.num_w as u64 + 1)) as usize;
        let d_id = (rng.borrow_mut().range(1, db.num_d as u64 + 1)) as usize;
        let c_id = (rng.borrow_mut().range(1, db.num_c as u64 + 1)) as usize;

        if r < 45 {
            transaction(|tx| {
                let mut rng = rng.borrow_mut();
                txn_new_order(db, tx, w_id, d_id, c_id, &mut *rng);
            });
            tx_counts.c0.fetch_add(1, Ordering::Relaxed);
        } else if r < 88 {
            let amount = 100.0 + (rng.borrow_mut().next() % 9900) as f64;
            transaction(|tx| { txn_payment(db, tx, w_id, d_id, c_id, amount); });
            tx_counts.c1.fetch_add(1, Ordering::Relaxed);
        } else if r < 92 {
            transaction(|tx| { txn_order_status(db, tx, w_id, d_id, c_id); });
            tx_counts.c2.fetch_add(1, Ordering::Relaxed);
        } else if r < 96 {
            let carrier = ((rng.borrow_mut().next() % 10) + 1) as i32;
            transaction(|tx| { txn_delivery(db, tx, w_id, carrier); });
            tx_counts.c3.fetch_add(1, Ordering::Relaxed);
        } else {
            let threshold = (rng.borrow_mut().range(10, 21)) as i32;
            transaction(|tx| { txn_stock_level(db, tx, w_id, d_id, threshold); });
            tx_counts.c4.fetch_add(1, Ordering::Relaxed);
        }
        total_ops.fetch_add(1, Ordering::Relaxed);
    }
}

// ── Main ───────────────────────────────────────────────────────────────

fn main() {
    tm::tm_init();

    let args: Vec<String> = std::env::args().collect();
    let mut threads = 4usize;
    let mut duration = 10000usize;
    let mut warehouses = DEFAULT_WAREHOUSES;
    let mut i = 1;
    while i < args.len() {
        match args[i].as_str() {
            "-t" if i+1<args.len() => { threads = args[i+1].parse().unwrap_or(4); i+=2; }
            "-d" if i+1<args.len() => { duration = args[i+1].parse().unwrap_or(10000); i+=2; }
            "-w" if i+1<args.len() => { warehouses = args[i+1].parse().unwrap_or(1); i+=2; }
            _ => i+=1,
        }
    }

    println!("========= TPC-C Benchmark (v5.11) =========");
    println!("============================================");
    println!("Warehouses: {warehouses}");
    println!("Districts:  {}", warehouses * DEFAULT_DISTRICTS);
    println!("Customers:  {}", warehouses * DEFAULT_DISTRICTS * DEFAULT_CUSTOMERS);
    println!("Items:      {DEFAULT_ITEMS}");
    println!("Threads:    {threads}");
    println!("Duration:   {duration}ms\n");

    let db = Arc::new(TpccDatabase::new(warehouses));

    let stop = Arc::new(AtomicBool::new(false));
    let total_ops = Arc::new(AtomicU64::new(0));
    let tx_counts = Arc::new(TxCounts::new());

    let mut handles = Vec::new();
    for tid in 0..threads {
        let db = Arc::clone(&db);
        let stop = Arc::clone(&stop);
        let ops = Arc::clone(&total_ops);
        let tc = Arc::clone(&tx_counts);
        handles.push(std::thread::spawn(move || {
            worker(tid, &db, &stop, &ops, &tc);
        }));
    }

    std::thread::sleep(std::time::Duration::from_millis(duration as u64));
    stop.store(true, Ordering::Relaxed);
    for h in handles { h.join().unwrap(); }

    let ops = total_ops.load(Ordering::Relaxed);
    let elapsed = duration as f64 / 1000.0;

    println!("\nResults");
    println!("=======");
    println!("Elapsed: {:.1}s", elapsed);
    println!("Total ops: {ops}");
    println!("Throughput: {:.0} txn/s", ops as f64 / elapsed);
    println!();
    println!("Transaction breakdown:");
    println!("  New-Order:    {}", tx_counts.c0.load(Ordering::Relaxed));
    println!("  Payment:      {}", tx_counts.c1.load(Ordering::Relaxed));
    println!("  Order-Status: {}", tx_counts.c2.load(Ordering::Relaxed));
    println!("  Delivery:     {}", tx_counts.c3.load(Ordering::Relaxed));
    println!("  Stock-Level:  {}", tx_counts.c4.load(Ordering::Relaxed));

    tm::tm_exit();
}
