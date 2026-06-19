use std::sync::atomic::{AtomicBool, AtomicU64};
use benchmarks::stamp::{Config, Bench};

fn parse_args() -> Config {
    let args: Vec<String> = std::env::args().collect();
    let mut c = Config::default();
    let mut i = 1;
    while i < args.len() {
        match args[i].as_str() {
            // Threads (C++ uses -p, accept both)
            "-p" | "-t" if i+1<args.len() => { c.threads = args[i+1].parse().unwrap_or(4); i+=2; }
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
            "-k" if i+1<args.len() => { c.clusters = args[i+1].parse().unwrap_or(5); i+=2; }
            "-n" if i+1<args.len() => { c.points = args[i+1].parse().unwrap_or(10000); i+=2; }
            "-g" if i+1<args.len() => { c.gene_length = args[i+1].parse().unwrap_or(16384); i+=2; }
            "-s" if i+1<args.len() => { c.segment_length = args[i+1].parse().unwrap_or(64); i+=2; }
            "-x" if i+1<args.len() => { c.grid_x = args[i+1].parse().unwrap_or(5); i+=2; }
            "-y" if i+1<args.len() => { c.grid_y = args[i+1].parse().unwrap_or(5); i+=2; }
            "-z" if i+1<args.len() => { c.grid_z = args[i+1].parse().unwrap_or(5); i+=2; }
            "-r" if i+1<args.len() => { c.num_rooms = args[i+1].parse().unwrap_or(10); i+=2; }
            "-c" if i+1<args.len() => { c.num_customers = args[i+1].parse().unwrap_or(100); i+=2; }
            "-a" if i+1<args.len() => { c.angle = args[i+1].parse().unwrap_or(20.0); i+=2; }
            "-j" if i+1<args.len() => { c.jitter = args[i+1].parse().unwrap_or(0.5); i+=2; }
            "-u" if i+1<args.len() => { c.prob_unidirectional = args[i+1].parse().unwrap_or(0.5); i+=2; }
            "-l" if i+1<args.len() => { c.subgr_edge_length = args[i+1].parse().unwrap_or(3); i+=2; }
            "-i" if i+1<args.len() => { c.iterations = args[i+1].parse().unwrap_or(10); i+=2; }
            "-m" if i+1<args.len() => { c.max_length = args[i+1].parse().unwrap_or(128); i+=2; }
            "--points" if i+1<args.len() => { c.points = args[i+1].parse().unwrap_or(10000); i+=2; }
            "--test" => { c.bench = Bench::All; return c; }
            "--help" => {
                println!("Usage: stamp [options]");
                println!("  -p/-t N   Threads (default: 4)");
                println!("  -d N      Duration ms (default: 10000)");
                println!("  -b NAME   Benchmark: kmeans|labyrinth|vacation|genome|intruder|ssca2|bayes|yada|all");
                println!("  -k N      Kmeans clusters");
                println!("  -n N      Kmeans points / genome segments");
                println!("  -g N      Gene length");
                println!("  -s N      Segment length");
                println!("  -x -y -z  Labyrinth grid dims");
                println!("  -r N      Vacation rooms");
                println!("  -c N      Vacation customers");
                println!("  -a N      Yada angle constraint");
                println!("  -j N      Yada jitter");
                println!("  -u N      SSCA2 prob unidirectional");
                println!("  -l N      SSCA2 subgraph edge length");
                println!("  -i N      SSCA2 iterations");
                println!("  --test    Run self-tests");
                std::process::exit(0);
            }
            _ => i+=1,
        }
    }
    c
}

fn run_benchmarks(config: &Config) {
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
        benchmarks::stamp::kmeans::run(config, &stop, &ops);
    }
    if config.bench == Bench::All || config.bench == Bench::Labyrinth {
        let stop = AtomicBool::new(false);
        let ops = AtomicU64::new(0);
        benchmarks::stamp::labyrinth::run(config, &stop, &ops);
    }
    if config.bench == Bench::All || config.bench == Bench::Vacation {
        let stop = AtomicBool::new(false);
        let ops = AtomicU64::new(0);
        benchmarks::stamp::vacation::run(config, &stop, &ops);
    }
    if config.bench == Bench::All || config.bench == Bench::Genome {
        let stop = AtomicBool::new(false);
        let ops = AtomicU64::new(0);
        benchmarks::stamp::genome::run(config, &stop, &ops);
    }
    if config.bench == Bench::All || config.bench == Bench::Intruder {
        let stop = AtomicBool::new(false);
        let ops = AtomicU64::new(0);
        benchmarks::stamp::intruder::run(config, &stop, &ops);
    }
    if config.bench == Bench::All || config.bench == Bench::SSCA2 {
        let stop = AtomicBool::new(false);
        let ops = AtomicU64::new(0);
        benchmarks::stamp::ssca2::run(config, &stop, &ops);
    }
    if config.bench == Bench::All || config.bench == Bench::Bayes {
        let stop = AtomicBool::new(false);
        let ops = AtomicU64::new(0);
        benchmarks::stamp::bayes::run(config, &stop, &ops);
    }
    if config.bench == Bench::All || config.bench == Bench::Yada {
        let stop = AtomicBool::new(false);
        let ops = AtomicU64::new(0);
        benchmarks::stamp::yada::run(config, &stop, &ops);
    }
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    if args.len() > 1 && (args[1] == "--test" || args[1] == "-T") {
        tm::tm_init();
        let mut config = Config::default();
        let mut i = 2;
        while i < args.len() {
            if args[i] == "-b" && i + 1 < args.len() {
                config.bench = match args[i + 1].to_lowercase().as_str() {
                    "kmeans" => Bench::KMeans, "labyrinth" => Bench::Labyrinth,
                    "vacation" => Bench::Vacation, "genome" => Bench::Genome,
                    "intruder" => Bench::Intruder, "ssca2" => Bench::SSCA2,
                    "bayes" => Bench::Bayes, "yada" => Bench::Yada,
                    _ => Bench::All,
                };
                i += 2;
            } else { i += 1; }
        }
        let bench_name = match config.bench {
            Bench::All => "all",
            Bench::KMeans => "kmeans", Bench::Labyrinth => "labyrinth",
            Bench::Vacation => "vacation", Bench::Genome => "genome",
            Bench::Intruder => "intruder", Bench::SSCA2 => "ssca2",
            Bench::Bayes => "bayes", Bench::Yada => "yada",
        };
        println!("Self-tests: {bench_name}");
        let fails = benchmarks::stamp::run_test(config.bench);
        println!("{} tests failed.", fails);
        tm::tm_exit();
        std::process::exit(if fails > 0 { 1 } else { 0 });
    }

    tm::tm_init();
    let config = parse_args();
    run_benchmarks(&config);
    tm::tm_exit();
}
