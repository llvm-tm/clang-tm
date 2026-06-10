use clap::Parser;
use tm_des::bank_sim::{self, generate_all_scenarios};
use tm_des::trace::Trace;

#[derive(Parser, Debug)]
#[command(name = "tm-gen", about = "Generate TM simulation traces for bank benchmark")]
struct Cli {
    /// Output file (use '-' for stdout).
    #[arg(short, long, default_value = "-")]
    output: String,

    /// Base address for bank accounts.
    #[arg(long, default_value = "0x7f0000001000")]
    base: String,

    /// Generate all scenarios (default) or a specific scenario.
    #[arg(long)]
    scenario: Option<String>,
}

fn parse_addr(s: &str) -> u64 {
    if s.starts_with("0x") || s.starts_with("0X") {
        u64::from_str_radix(&s[2..], 16).unwrap()
    } else {
        s.parse().unwrap()
    }
}

fn main() {
    let cli = Cli::parse();
    let base = parse_addr(&cli.base);

    let events = match cli.scenario.as_deref() {
        Some("simple") => bank_sim::scenario_simple_transfer(base),
        Some("scan") => bank_sim::scenario_read_only_scan(base),
        Some("conflict") => bank_sim::scenario_same_account_conflict(base),
        Some("disjoint") => bank_sim::scenario_disjoint_transfers(base),
        Some("lost-update") => bank_sim::scenario_lost_update(base),
        Some("write-skew") => bank_sim::scenario_write_skew(base),
        Some("all") | None => generate_all_scenarios(base),
        Some(s) => {
            eprintln!("Unknown scenario: {}", s);
            std::process::exit(1);
        }
    };

    let trace = Trace { events };
    let mut out: Box<dyn std::io::Write> = if cli.output == "-" {
        Box::new(std::io::stdout())
    } else {
        Box::new(std::fs::File::create(&cli.output).unwrap())
    };

    trace.to_jsonl(&mut out).unwrap();
    eprintln!("Generated {} events", trace.events.len());
}
