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
            "-g" if i + 1 < args.len() => { c.gene_length = args[i + 1].parse().unwrap_or(16384); i += 2; }
            "-s" if i + 1 < args.len() => { c.segment_length = args[i + 1].parse().unwrap_or(64); i += 2; }
            "-n" if i + 1 < args.len() => { c.num_segments = args[i + 1].parse().unwrap_or(16777216); i += 2; }
            _ => i += 1,
        }
    }
    c
}

fn main() {
    let config = parse_args();
    println!("========= STAMP Genome =========");
    println!("Threads: {}  Duration: {}ms", config.threads, config.duration);
    println!("Gene length: {}  Segment length: {}  Segments: {}", config.gene_length, config.segment_length, config.num_segments);
    println!();

    tm::tm_init();
    let stop = AtomicBool::new(false);
    let ops = AtomicU64::new(0);
    benchmarks::stamp::genome::run(&config, &stop, &ops);
    println!("Total ops: {}", ops.load(std::sync::atomic::Ordering::Relaxed));
    tm::tm_exit();
}
