use std::sync::atomic::{AtomicBool, AtomicU64};
use benchmarks::stamp::{Config, Bench};

fn parse_args() -> Config {
    let args: Vec<String> = std::env::args().collect();
    let mut c = Config::default();
    let mut i = 1;
    while i < args.len() {
        match args[i].as_str() {
            "-t" if i+1<args.len() => { c.threads = args[i+1].parse().unwrap_or(4); i+=2; }
            "-d" if i+1<args.len() => { c.duration = args[i+1].parse().unwrap_or(10000); i+=2; }
            "-b" if i+1<args.len() => {
                c.bench = match args[i+1].to_lowercase().as_str() {
                    "kmeans" => Bench::KMeans, "labyrinth" => Bench::Labyrinth,
                    "vacation" => Bench::Vacation, "genome" => Bench::Genome,
                    "intruder" => Bench::Intruder, "ssca2" => Bench::SSCA2,
                    "bayes" => Bench::Bayes, "yada" => Bench::Yada,
                    _ => Bench::All,
                };
                i+=2;
            }
            _ => i+=1,
        }
    }
    c
}

fn main() {
    tm::tm_init();
    let config = parse_args();

    let bench_name = match config.bench {
        Bench::All => "all", Bench::KMeans => "kmeans",
        Bench::Labyrinth => "labyrinth", Bench::Vacation => "vacation",
        Bench::Genome => "genome", Bench::Intruder => "intruder",
        Bench::SSCA2 => "ssca2", Bench::Bayes => "bayes", Bench::Yada => "yada",
    };
    println!("========= STAMP Benchmarks =========");
    println!("Bench: {bench_name}  Threads: {t}  Duration: {d}ms",
             t=config.threads, d=config.duration);
    println!();

    if config.bench == Bench::All || config.bench == Bench::KMeans {
        let stop = AtomicBool::new(false);
        let ops = AtomicU64::new(0);
        benchmarks::stamp::kmeans::run(&config, &stop, &ops);
    }
    if config.bench == Bench::All || config.bench == Bench::Labyrinth {
        let stop = AtomicBool::new(false);
        let ops = AtomicU64::new(0);
        benchmarks::stamp::labyrinth::run(&config, &stop, &ops);
    }
    if config.bench == Bench::All || config.bench == Bench::Vacation {
        let stop = AtomicBool::new(false);
        let ops = AtomicU64::new(0);
        benchmarks::stamp::vacation::run(&config, &stop, &ops);
    }
    if config.bench == Bench::All || config.bench == Bench::Genome {
        let stop = AtomicBool::new(false);
        let ops = AtomicU64::new(0);
        benchmarks::stamp::genome::run(&config, &stop, &ops);
    }
    if config.bench == Bench::All || config.bench == Bench::Intruder {
        let stop = AtomicBool::new(false);
        let ops = AtomicU64::new(0);
        benchmarks::stamp::intruder::run(&config, &stop, &ops);
    }
    if config.bench == Bench::All || config.bench == Bench::SSCA2 {
        let stop = AtomicBool::new(false);
        let ops = AtomicU64::new(0);
        benchmarks::stamp::ssca2::run(&config, &stop, &ops);
    }
    if config.bench == Bench::All || config.bench == Bench::Bayes {
        let stop = AtomicBool::new(false);
        let ops = AtomicU64::new(0);
        benchmarks::stamp::bayes::run(&config, &stop, &ops);
    }
    if config.bench == Bench::All || config.bench == Bench::Yada {
        let stop = AtomicBool::new(false);
        let ops = AtomicU64::new(0);
        benchmarks::stamp::yada::run(&config, &stop, &ops);
    }

    println!("\nTM aborts: {}", tm::tm_abort_count());
    tm::tm_exit();
}
