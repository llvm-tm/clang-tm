use clap::Parser;
use std::collections::HashMap;
use std::io::BufRead;
use tm_des::event::{Event, EventKind};
use tm_des::trace::Trace;

#[derive(Parser, Debug)]
#[command(name = "tm-trace2jsonl", about = "Convert raw TM trace to JSONL for simulator")]
struct Cli {
    #[arg(short, long, default_value = "-")]
    input: String,
    #[arg(short, long, default_value = "-")]
    output: String,
}

struct RawEntry {
    timestamp: u64,
    thread_id: u32,
    type_code: u32,
    addr: u64,
    width: u64,
    value: u64,
}

fn parse_trace(input: &str) -> Result<Vec<RawEntry>, String> {
    let reader: Box<dyn std::io::BufRead> = if input == "-" {
        Box::new(std::io::BufReader::new(std::io::stdin()))
    } else {
        let f = std::fs::File::open(input).map_err(|e| format!("open {}: {}", input, e))?;
        Box::new(std::io::BufReader::new(f))
    };

    let mut entries = Vec::new();
    for (line_no, line) in reader.lines().enumerate() {
        let line = line.map_err(|e| format!("line {}: {}", line_no + 1, e))?;
        let line = line.trim();
        if line.is_empty() || line.starts_with('#') {
            continue;
        }
        let parts: Vec<&str> = line.split_whitespace().collect();
        if parts.len() < 4 {
            return Err(format!("line {}: need at least 4 columns", line_no + 1));
        }
        let timestamp: u64 = parts[0].parse()
            .map_err(|e| format!("line {} timestamp: {}", line_no + 1, e))?;
        let thread_id: u32 = parts[1].parse()
            .map_err(|e| format!("line {} thread_id: {}", line_no + 1, e))?;
        let type_code: u32 = parts[2].parse()
            .map_err(|e| format!("line {} type_code: {}", line_no + 1, e))?;

        // Two trace formats are in use:
        //
        //   Old (6-field):  ts tid type 0x<addr> <width> 0x<value>
        //   Extended (8+):  ts tid type <txid> 0x<addr> <width> 0x<value> <cont> [extra...]
        //
        // Distinguish by checking whether parts[3] starts with "0x".
        // In the old format parts[3] is the address (always 0x-prefixed).
        // In the extended format parts[3] is the TX id (decimal integer).
        let (addr, width, value) = if parts[3].starts_with("0x") {
            // Old legacy format (6 fields)
            let addr_str = parts[3].trim_start_matches("0x");
            let addr = u64::from_str_radix(addr_str, 16)
                .map_err(|e| format!("line {} addr: {}", line_no + 1, e))?;
            let width = if parts.len() > 4 {
                parts[4].parse().unwrap_or(8)
            } else { 8 };
            let value_str = if parts.len() > 5 {
                parts[5].trim_start_matches("0x")
            } else { "0" };
            let value = u64::from_str_radix(value_str, 16).unwrap_or(0);
            (addr, width, value)
        } else {
            // Extended format (8+ fields): ts tid type txid 0x<addr> width 0x<value> cont
            if parts.len() < 7 {
                return Err(format!("line {}: extended format needs at least 7 columns", line_no + 1));
            }
            let addr_str = parts[4].trim_start_matches("0x");
            let addr = u64::from_str_radix(addr_str, 16)
                .map_err(|e| format!("line {} addr: {}", line_no + 1, e))?;
            let width: u64 = parts[5].parse()
                .map_err(|e| format!("line {} width: {}", line_no + 1, e))?;
            let value_str = parts[6].trim_start_matches("0x");
            let value = u64::from_str_radix(value_str, 16).unwrap_or(0);
            (addr, width, value)
        };

        entries.push(RawEntry { timestamp, thread_id, type_code, addr, width, value });
    }
    Ok(entries)
}

fn infer_events(entries: &[RawEntry]) -> Vec<Event> {
    let mut by_thread: HashMap<u32, Vec<&RawEntry>> = HashMap::new();
    for e in entries {
        by_thread.entry(e.thread_id).or_default().push(e);
    }

    let mut events: Vec<Event> = Vec::new();
    let mut next_seq = 0u64;

    for (&tid, thread_entries) in &by_thread {
        let mut sorted = thread_entries.clone();
        sorted.sort_by_key(|e| e.timestamp);

        for entry in sorted {
            let kind = match entry.type_code {
                0 => EventKind::Read { addr: entry.addr, width: entry.width as u8 },
                1 => EventKind::Write { addr: entry.addr, width: entry.width as u8, val: entry.value },
                2 => EventKind::TxBegin,
                3 => EventKind::TxEnd,
                4 => EventKind::Alloc { addr: entry.addr, size: entry.width },
                5 => EventKind::Free { addr: entry.addr },
                6 => EventKind::Abort { reason: entry.value },
                code => {
                    eprintln!("warning: unsupported type_code {} at line (ts={}, tid={}) — skipped",
                              code, entry.timestamp, entry.thread_id);
                    continue;
                }
            };
            events.push(Event::new(entry.timestamp, tid, next_seq, kind));
            next_seq += 1;
        }
    }

    events.sort_by_key(|e| e.timestamp);
    events
}

fn main() {
    let cli = Cli::parse();
    let entries = parse_trace(&cli.input).unwrap_or_else(|e| {
        eprintln!("Parse error: {}", e);
        std::process::exit(1);
    });

    let events = infer_events(&entries);
    let trace = Trace { events };

    let mut out: Box<dyn std::io::Write> = if cli.output == "-" {
        Box::new(std::io::stdout())
    } else {
        Box::new(std::fs::File::create(&cli.output).unwrap())
    };

    trace.to_jsonl(&mut out).unwrap();
    eprintln!("Converted {} raw entries → {} JSONL events", entries.len(), trace.events.len());
}
