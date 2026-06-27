use clap::Parser;
use std::collections::HashMap;
use std::io::BufRead;
use tm_des::event::{Event, EventKind};
use tm_des::trace::Trace;

#[derive(Parser, Debug)]
#[command(name = "tm-fix-trace", about = "Fix trace BEGIN/END mismatches from siglongjmp retry")]
struct Cli {
    #[arg(short, long, default_value = "-")]
    input: String,
    #[arg(short, long, default_value = "-")]
    output: String,
}

fn main() {
    let cli = Cli::parse();

    let reader: Box<dyn BufRead> = if cli.input == "-" {
        Box::new(std::io::BufReader::new(std::io::stdin()))
    } else {
        let f = std::fs::File::open(&cli.input)
            .unwrap_or_else(|e| { eprintln!("Error: open {}: {}", cli.input, e); std::process::exit(1); });
        Box::new(std::io::BufReader::new(f))
    };

    let trace: Trace = Trace::from_jsonl(reader)
        .unwrap_or_else(|e| { eprintln!("Error: parse JSONL: {}", e); std::process::exit(1); });

    // Per-thread tx depth tracker
    let mut tx_depth: HashMap<u32, u32> = HashMap::new();
    let mut fixed_events: Vec<Event> = Vec::with_capacity(trace.events.len());
    let mut inserted_count = 0u64;

    for event in &trace.events {
        let tid = event.thread_id;
        let depth = tx_depth.entry(tid).or_insert(0);

        match &event.kind {
            EventKind::TxBegin => {
                if *depth > 0 {
                    // siglongjmp retry: no matching TxEnd/Abort was written.
                    // Insert synthetic Abort event before this Begin.
                    fixed_events.push(Event::new(
                        event.timestamp,
                        tid,
                        event.seq,
                        EventKind::Abort { reason: 0 },
                    ));
                    *depth = depth.saturating_sub(1);
                    inserted_count += 1;
                }
                *depth += 1;
                fixed_events.push(event.clone());
            }
            EventKind::TxEnd | EventKind::Abort { .. } => {
                *depth = depth.saturating_sub(1);
                fixed_events.push(event.clone());
            }
            _ => {
                fixed_events.push(event.clone());
            }
        }
    }

    let fixed_trace = Trace { events: fixed_events };

    let mut out: Box<dyn std::io::Write> = if cli.output == "-" {
        Box::new(std::io::stdout())
    } else {
        Box::new(std::fs::File::create(&cli.output).unwrap())
    };

    fixed_trace.to_jsonl(&mut out).unwrap();
    eprintln!(
        "Fixed {} events → {} events (inserted {} synthetic Abort events for unmatched BEGINs)",
        trace.events.len(),
        fixed_trace.events.len(),
        inserted_count,
    );
}
