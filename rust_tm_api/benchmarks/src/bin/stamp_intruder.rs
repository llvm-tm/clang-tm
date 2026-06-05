use std::collections::{HashMap, VecDeque};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Instant;
use tm::*;

#[derive(Clone)]
struct Packet {
    flow_id: i64,
    fragment_id: i32,
    num_fragments: i32,
    data: String,
}

#[derive(Clone)]
#[allow(dead_code)]
struct DecodedFlow {
    flow_id: i64,
    data: String,
}

struct SharedData {
    packet_queue: VecDeque<Packet>,
    decoder_map: HashMap<i64, DecodedFlowInternal>,
    decoded_queue: VecDeque<DecodedFlow>,
}

#[allow(dead_code)]
struct DecodedFlowInternal {
    flow_id: i64,
    num_fragments_received: i32,
    fragments: Vec<String>,
}

static DICT: &[&str] = &[
    "about", "attack", "back", "root", "system", "access",
    "all", "after", "also", "and", "any", "are", "but",
    "can", "come", "could", "did", "do", "each", "find",
    "first", "for", "from", "get", "go", "has", "have",
    "her", "here", "him", "his", "how", "into", "its",
    "just", "know", "like", "look", "make", "man", "may",
    "more", "most", "must", "new", "no", "not", "now",
    "old", "one", "only", "other", "our", "out", "over",
    "own", "part", "people", "said", "say", "see", "she",
    "shell", "should", "site", "some", "such", "take",
    "than", "that", "their", "them", "then", "there",
];

fn detect_attack(data: &str) -> bool {
    let lower: String = data.chars()
        .map(|c| if c >= 'A' && c <= 'Z' { (c as u8 - b'A' + b'a') as char } else { c })
        .collect();
    for sig in DICT {
        if lower.contains(sig) {
            return true;
        }
    }
    false
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let mut num_threads = 4;
    let mut percent_attack = 10;
    let mut max_data_length = 128;
    let mut num_flows = 1_000_000;
    let mut seed = 1;

    let mut i = 1;
    while i < args.len() {
        match args[i].as_str() {
            "-p" => { i += 1; num_threads = args[i].parse().unwrap(); }
            "-a" => { i += 1; percent_attack = args[i].parse().unwrap(); }
            "-l" => { i += 1; max_data_length = args[i].parse().unwrap(); }
            "-n" => { i += 1; num_flows = args[i].parse().unwrap(); }
            "-s" => { i += 1; seed = args[i].parse().unwrap(); }
            _ => {}
        }
        i += 1;
    }

    tm_init();

    let mut data = SharedData {
        packet_queue: VecDeque::new(),
        decoder_map: HashMap::new(),
        decoded_queue: VecDeque::new(),
    };

    let mut rng = fastrand::Rng::with_seed(seed as u64);
    let mut total_attacks = 0;

    for flow in 1..=num_flows as i64 {
        let is_attack = rng.u32(..) % 100 < percent_attack as u32;
        let payload: String;

        if is_attack {
            let sig_idx = rng.usize(0..DICT.len());
            payload = DICT[sig_idx].to_string();
            total_attacks += 1;
        } else {
            let len = (rng.u32(..) as usize % max_data_length as usize) + 1;
            payload = (0..len).map(|_| (32u8 + (rng.u32(..) as u8 % 95)) as char).collect();
        }

        let num_packets = if payload.is_empty() {
            1
        } else {
            (rng.u32(..) as usize % payload.len()) + 1
        };

        let base_len = payload.len() / num_packets;
        let rem = payload.len() % num_packets;

        let mut offset = 0;
        for f in 0..num_packets {
            let this_len = base_len + if f < rem { 1 } else { 0 };
            let frag_data = &payload[offset..offset + this_len];
            data.packet_queue.push_back(Packet {
                flow_id: flow,
                fragment_id: f as i32,
                num_fragments: num_packets as i32,
                data: frag_data.to_string(),
            });
            offset += this_len;
        }
    }

    println!("Percent attack  = {}", percent_attack);
    println!("Max data length = {}", max_data_length);
    println!("Num flow        = {}", num_flows);
    println!("Random seed     = {}", seed);
    println!("Num attack      = {}", total_attacks);

    let data = Arc::new(Mutex::new(data));
    let total_ops = Arc::new(AtomicU64::new(0));

    let start = Instant::now();

    std::thread::scope(|s| {
        for _ in 0..num_threads {
            let data = data.clone();
            let total_ops = total_ops.clone();

            s.spawn(move || {
                tm_init_thread();

                loop {
                    let mut had_packet = false;

                    // Get packet and process decoder
                    let pkt = {
                        let mut d = data.lock().unwrap();
                        d.packet_queue.pop_front()
                    };

                    if let Some(pkt) = pkt {
                        had_packet = true;
                        let mut d = data.lock().unwrap();
                        let it = d.decoder_map.entry(pkt.flow_id).or_insert_with(|| {
                            DecodedFlowInternal {
                                flow_id: pkt.flow_id,
                                num_fragments_received: 0,
                                fragments: vec![String::new(); pkt.num_fragments as usize],
                            }
                        });
                        it.fragments[pkt.fragment_id as usize] = pkt.data;
                        it.num_fragments_received += 1;

                        if it.num_fragments_received == pkt.num_fragments {
                            let full = it.fragments.concat();
                            d.decoded_queue.push_back(DecodedFlow {
                                flow_id: pkt.flow_id,
                                data: full,
                            });
                            d.decoder_map.remove(&pkt.flow_id);
                        }
                    }

                    // Get complete decoded flow
                    let df = {
                        let mut d = data.lock().unwrap();
                        d.decoded_queue.pop_front()
                    };

                    if let Some(df) = df {
                        let _attack = detect_attack(&df.data);
                        total_ops.fetch_add(1, Ordering::Relaxed);
                    } else if !had_packet {
                        // No packets left and no decoded flows ready → done
                        break;
                    }
                }

                tm_exit_thread();
            });
        }
    });

    let elapsed = start.elapsed();
    let ops = total_ops.load(Ordering::Relaxed);

    println!("    Time = {} ms", elapsed.as_millis());
    println!("    Completed flows = {} / {}", ops, num_flows);

    tm_exit();
}
