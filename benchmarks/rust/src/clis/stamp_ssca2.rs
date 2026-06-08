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
            "-s" if i + 1 < args.len() => { c.scale = args[i + 1].parse().unwrap_or(13); i += 2; }
            "-m" if i + 1 < args.len() => { c.min_degree = args[i + 1].parse().unwrap_or(3); i += 2; }
            "-l" if i + 1 < args.len() => { c.max_degree = args[i + 1].parse().unwrap_or(3); i += 2; }
            _ => i += 1,
        }
    }
    c
}

fn main() {
    let config = parse_args();
    println!("========= STAMP SSCA2 =========");
    println!("Threads: {}  Duration: {}ms", config.threads, config.duration);
    println!("Scale: {}  Min degree: {}  Max degree: {}", config.scale, config.min_degree, config.max_degree);
    println!();

    tm::tm_init();
    let stop = AtomicBool::new(false);
    let ops = AtomicU64::new(0);
    benchmarks::stamp::ssca2::run(&config, &stop, &ops);
    println!("Total ops: {}", ops.load(std::sync::atomic::Ordering::Relaxed));
    tm::tm_exit();
}
