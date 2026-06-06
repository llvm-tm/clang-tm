use tm::{tm_init, transaction, TmCell};
use std::sync::Arc;

struct DB {
    warehouses: Vec<Warehouse>,
    districts: Vec<District>,
    customers: Vec<Customer>,
}

struct Warehouse {
    w_id: i32, w_tax: TmCell<f64>, w_ytd: TmCell<f64>,
}

struct District {
    d_id: i32, d_w_id: i32,
    d_tax: TmCell<f64>, d_ytd: TmCell<f64>, d_next_o_id: TmCell<i32>,
}

struct Customer {
    c_id: i32, c_d_id: i32, c_w_id: i32,
    c_credit_lim: TmCell<f64>, c_discount: TmCell<f64>,
    c_balance: TmCell<f64>, c_ytd_payment: TmCell<f64>,
    c_payment_cnt: TmCell<i32>, c_delivery_cnt: TmCell<i32>,
}

fn main() {
    tm_init();
    
    let num_w = 1usize;
    let num_d = 10usize;
    let num_c = 10usize;
    
    let db = Arc::new(DB {
        warehouses: (0..num_w).map(|i| Warehouse {
            w_id: (i+1) as i32, w_tax: TmCell::new(0.19), w_ytd: TmCell::new(3000000.0),
        }).collect(),
        districts: (0..num_w * num_d).map(|i| District {
            d_id: ((i % num_d) + 1) as i32, d_w_id: ((i / num_d) + 1) as i32,
            d_tax: TmCell::new(0.15), d_ytd: TmCell::new(3000000.0),
            d_next_o_id: TmCell::new(2101),
        }).collect(),
        customers: (0..num_w * num_d * num_c).enumerate().map(|(idx, _)| {
            let c_id = (idx % num_c + 1) as i32;
            Customer {
                c_id, c_d_id: ((idx / num_c) % num_d + 1) as i32,
                c_w_id: (idx / (num_c * num_d) + 1) as i32,
                c_credit_lim: TmCell::new(50000.0), c_discount: TmCell::new(0.3),
                c_balance: TmCell::new(-10.0), c_ytd_payment: TmCell::new(10.0),
                c_payment_cnt: TmCell::new(1), c_delivery_cnt: TmCell::new(0),
            }
        }).collect(),
    });
    
    println!("DB created. Running single new-order transaction...");
    
    let w_id = 1usize;
    let d_id = 1usize;
    let c_id = 1usize;
    
    transaction(|tx| {
        let d = &db.districts[(w_id - 1) * num_d + (d_id - 1)];
        let c = &db.customers[(w_id - 1) * num_d * num_c + (d_id - 1) * num_c + (c_id - 1)];
        let w = &db.warehouses[w_id - 1];
        
        let o_id = tx.read(&d.d_next_o_id);
        tx.write(&d.d_next_o_id, o_id + 1);
        
        let w_tax = tx.read(&w.w_tax);
        let d_tax = tx.read(&d.d_tax);
        let c_discount = tx.read(&c.c_discount);
        let _c_last = c_id as f64;
        let _credit_lim = tx.read(&c.c_credit_lim);
        
        // Simulate order line
        let _ol_amount = 100.0;
        
        // Update customer balance
        let bal = tx.read(&c.c_balance);
        tx.write(&c.c_balance, bal - _ol_amount);
        
        // Update warehouse ytd
        let ytd = tx.read(&w.w_ytd);
        tx.write(&w.w_ytd, ytd + _ol_amount);
        
        println!("  New order {o_id}: tax={:.2}/{:.2}, discount={:.2}, ytd={:.2}", 
                 w_tax, d_tax, c_discount, ytd);
    });
    
    println!("First transaction PASS");
    
    // Run many transactions
    println!("\nRunning 100 transactions...");
    for i in 0..100 {
        transaction(|tx| {
            let d = &db.districts[0];
            let o_id = tx.read(&d.d_next_o_id);
            tx.write(&d.d_next_o_id, o_id + 1);
        });
    }
    println!("All PASS");
}
