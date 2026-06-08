use std::sync::atomic::{AtomicBool, AtomicU64};
use benchmarks::stamp::Config;

fn parse_args() -> Config {
    let args: Vec<String> = std::env::args().collect();
    let mut c = Config::default();
    let mut i = 1;
    while i < args.len() {
        match args[i].as_str() {
            "-p" if i + 1 < args.len() => { c.threads = args[i + 1].parse().unwrap_or(4); i += 2; }
            "-d" if i + 1 < args.len() => { c.duration = args[i + 1].parse().unwrap_or(10000); i += 2; }
            "-a" if i + 1 < args.len() => { c.num_atoms = args[i + 1].parse().unwrap_or(10); i += 2; }
            "-l" if i + 1 < args.len() => { c.max_length = args[i + 1].parse().unwrap_or(128); i += 2; }
            "-n" if i + 1 < args.len() => { c.num_packets = args[i + 1].parse().unwrap_or(1048576); i += 2; }
            _ => i += 1,
        }
    }
    c
}

fn main() {
    let config = parse_args();
    println!("========= STAMP Intruder =========");
    println!("Threads: {}  Duration: {}ms", config.threads, config.duration);
    println!("Atoms: {}  Max length: {}  Packets: {}", config.num_atoms, config.max_length, config.num_packets);
    println!();

    tm::tm_init();
    let stop = AtomicBool::new(false);
    let ops = AtomicU64::new(0);
    benchmarks::stamp::intruder::run(&config, &stop, &ops);
    println!("Total ops: {}", ops.load(std::sync::atomic::Ordering::Relaxed));
    tm::tm_exit();
}
