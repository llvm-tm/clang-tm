use std::sync::atomic::{AtomicBool, AtomicU64};
use benchmarks::stamp::Config;

fn parse_args() -> Config {
    let args: Vec<String> = std::env::args().collect();
    let mut c = Config::default();
    let mut i = 1;
    while i < args.len() {
        match args[i].as_str() {
            "-p" if i + 1 < args.len() => { c.threads = args[i + 1].parse().unwrap_or(4); i += 2; }
            "-a" if i + 1 < args.len() => { c.percent_attack = args[i + 1].parse().unwrap_or(10); i += 2; }
            "-l" if i + 1 < args.len() => { c.max_length = args[i + 1].parse().unwrap_or(128); i += 2; }
            "-n" if i + 1 < args.len() => { c.num_packets = args[i + 1].parse().unwrap_or(1048576); i += 2; }
            "-s" if i + 1 < args.len() => { c.seed = args[i + 1].parse().unwrap_or(1); i += 2; }
            "-h" => {
                eprintln!("Usage: {} [-p threads] [-a attack_pct] [-l max_len] [-n flows] [-s seed]", args[0]);
                std::process::exit(0);
            }
            _ => i += 1,
        }
    }
    c
}

fn main() {
    let config = parse_args();
    println!("========= STAMP Intruder =========");
    println!("Threads: {}  Attack%: {}%  Max len: {}  Flows: {}  Seed: {}",
             config.threads, config.percent_attack, config.max_length,
             config.num_packets, config.seed);
    println!();

    tm::tm_init();
    let stop = AtomicBool::new(false);
    let ops = AtomicU64::new(0);
    benchmarks::stamp::intruder::run(&config, &stop, &ops);
    println!("Total ops: {}", ops.load(std::sync::atomic::Ordering::Relaxed));
    tm::tm_exit();
}
