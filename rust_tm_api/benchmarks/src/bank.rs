use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::time::Duration;
use tm::{TmCell, Transaction, transaction, tm_init, tm_exit};

// ── Configuration ───────────────────────────────────────
const DEFAULT_DURATION_MS: u64 = 10000;
const DEFAULT_NB_ACCOUNTS: usize = 1024;
const DEFAULT_NB_THREADS: usize = 4;
const DEFAULT_READ_ALL: u32 = 20;
const DEFAULT_INITIAL_BALANCE: i32 = 1000;

// ── Account & Bank ──────────────────────────────────────
struct Account {
    _number: i32,
    balance: TmCell<i32>,
}

impl Account {
    fn new(number: i32) -> Self {
        Account {
            _number: number,
            balance: TmCell::new(DEFAULT_INITIAL_BALANCE),
        }
    }
}

struct Bank {
    accounts: Vec<Account>,
}

impl Bank {
    fn new(size: usize) -> Self {
        let mut accounts = Vec::with_capacity(size);
        for i in 0..size {
            accounts.push(Account::new(i as i32));
        }
        Bank { accounts }
    }

    fn size(&self) -> usize {
        self.accounts.len()
    }
}

// ── TX operations ───────────────────────────────────────
fn transfer(tx: &Transaction, bank: &Bank, src: usize, dst: usize, amount: i32) {
    let b = tx.read(&bank.accounts[src].balance);
    tx.write(&bank.accounts[src].balance, b - amount);

    let b = tx.read(&bank.accounts[dst].balance);
    tx.write(&bank.accounts[dst].balance, b + amount);
}

fn total_transactional(tx: &Transaction, bank: &Bank) -> i32 {
    let mut total = 0i32;
    for account in &bank.accounts {
        total += tx.read(&account.balance);
    }
    total
}

fn total_non_transactional(bank: &Bank) -> i32 {
    // Direct access without TM — only called outside any transaction
    let mut total = 0i32;
    // SAFETY: We read via raw pointer to bypass TM (peek equivalent)
    for account in &bank.accounts {
        unsafe {
            total += *account.balance.ptr();
        }
    }
    total
}

// ── Main ────────────────────────────────────────────────
fn main() {
    let args: Vec<String> = std::env::args().collect();
    let mut duration_ms = DEFAULT_DURATION_MS;
    let mut nb_accounts = DEFAULT_NB_ACCOUNTS;
    let mut nb_threads = DEFAULT_NB_THREADS;
    let mut read_all_pct = DEFAULT_READ_ALL;

    let mut i = 1;
    while i < args.len() {
        match args[i].as_str() {
            "-d" if i + 1 < args.len() => {
                i += 1;
                duration_ms = args[i].parse().unwrap_or(DEFAULT_DURATION_MS);
            }
            "-a" if i + 1 < args.len() => {
                i += 1;
                nb_accounts = args[i].parse().unwrap_or(DEFAULT_NB_ACCOUNTS);
            }
            "-t" if i + 1 < args.len() => {
                i += 1;
                nb_threads = args[i].parse().unwrap_or(DEFAULT_NB_THREADS);
            }
            "-r" if i + 1 < args.len() => {
                i += 1;
                read_all_pct = args[i].parse().unwrap_or(DEFAULT_READ_ALL);
            }
            "-h" | "--help" => {
                println!("Usage: bank [-d ms] [-a n] [-t n] [-r pct]");
                return;
            }
            _ => {}
        }
        i += 1;
    }

    println!("Bank Benchmark — Rust TM API");
    println!(
        "Duration: {} ms  Accounts: {}  Threads: {}",
        duration_ms, nb_accounts, nb_threads
    );

    tm_init();

    let bank = Arc::new(Bank::new(nb_accounts));
    let expected_total = (nb_accounts as i32) * DEFAULT_INITIAL_BALANCE;
    let initial_total = total_non_transactional(&bank);
    println!("Initial total: {}  Expected: {}", initial_total, expected_total);
    assert_eq!(initial_total, expected_total, "Initial total mismatch");

    let stop = Arc::new(AtomicBool::new(false));

    let mut handles = Vec::with_capacity(nb_threads);
    for tid in 0..nb_threads {
        let bank = Arc::clone(&bank);
        let stop = Arc::clone(&stop);
        handles.push(std::thread::spawn(move || {
            let mut rng = fastrand::Rng::new();
            rng.seed(tid as u64 + 1234);
            let n = bank.size();
            let mut txfer = 0u64;
            let mut reads = 0u64;

            while !stop.load(Ordering::Relaxed) {
                let roll = rng.f64() * 100.0;
                if roll < read_all_pct as f64 {
                    transaction(|tx| {
                        total_transactional(tx, &bank);
                    });
                    reads += 1;
                } else {
                    let src = rng.usize(0..n);
                    let mut dst = rng.usize(0..n);
                    if dst == src {
                        dst = (src + 1) % n;
                    }
                    transaction(|tx| {
                        transfer(tx, &bank, src, dst, 1);
                    });
                    txfer += 1;
                }
            }
            (txfer, reads)
        }));
    }

    std::thread::sleep(Duration::from_millis(duration_ms));
    stop.store(true, Ordering::Relaxed);

    let mut total_txfer = 0u64;
    let mut total_reads = 0u64;
    for h in handles {
        let (t, r) = h.join().unwrap();
        total_txfer += t;
        total_reads += r;
    }

    let final_total = total_non_transactional(&bank);
    let total_txns = total_txfer + total_reads;

    let aborts = tm::tm_abort_count();
    println!(
        "\nResults:\n  Final total: {}  Expected: {}\n  Transfers: {}  Read-all: {}\n  Total txns: {}\n  TM aborts: {}",
        final_total, expected_total, total_txfer, total_reads, total_txns, aborts
    );

    tm_exit();

    if final_total == expected_total {
        println!("PASS: Money conserved");
    } else {
        eprintln!(
            "FAIL: Money {} by {}",
            if final_total > expected_total { "created" } else { "destroyed" },
            if final_total > expected_total {
                final_total - expected_total
            } else {
                expected_total - final_total
            }
        );
        std::process::exit(1);
    }
}
