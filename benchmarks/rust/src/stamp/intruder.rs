use std::mem::size_of;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::Arc;
use tm::transaction;
use crate::Rng;
use super::Config;

const INTRUDER_MAX_DATA: usize = 256;
const INTRUDER_MAX_PACKETS: usize = 128;

// ── POD data structures (C layout) ──────────────────────────────────
#[derive(Clone)]
#[repr(C)]
struct Packet {
    flow_id: i64,
    fragment_id: i32,
    num_fragments: i32,
    length: i32,
    data: [i8; INTRUDER_MAX_DATA],
}

#[derive(Clone)]
#[repr(C)]
struct DecodedFlow {
    flow_id: i64,
    data_len: i32,
    data: [i8; INTRUDER_MAX_DATA * 2],
}

impl Default for Packet {
    fn default() -> Self {
        Packet {
            flow_id: -1,
            fragment_id: 0,
            num_fragments: 0,
            length: 0,
            data: [0i8; INTRUDER_MAX_DATA],
        }
    }
}

impl Default for DecodedFlow {
    fn default() -> Self {
        DecodedFlow {
            flow_id: -1,
            data_len: 0,
            data: [0i8; INTRUDER_MAX_DATA * 2],
        }
    }
}

// ── Shared TM-allocated state ──────────────────────────────────────
struct IntruderData {
    packet_queue: *mut u8,
    packet_q_head: *mut i32,
    packet_q_tail: *mut i32,
    #[allow(dead_code)]
    packet_q_capacity: i32,
    decoder_flows: *mut u8,
    fragment_storage: *mut i8,
    fragment_counts: *mut i32,
    decoded_queue: *mut u8,
    decoded_q_head: *mut i32,
    decoded_q_tail: *mut i32,
    decoded_q_capacity: i32,
    dictionary: Vec<String>,
    ops: &'static AtomicU64,
}

unsafe impl Send for IntruderData {}
unsafe impl Sync for IntruderData {}

// ── TX helpers ─────────────────────────────────────────────────────
fn get_packet(data: &IntruderData) -> Packet {
    transaction(|_tx| unsafe {
        let head = tm::tm_read_i32(data.packet_q_head);
        let tail = tm::tm_read_i32(data.packet_q_tail);
        if head < tail {
            let p = data.packet_queue.add(head as usize * size_of::<Packet>()) as *mut Packet;
            let pkt = Packet {
                flow_id: tm::tm_read_i64(std::ptr::addr_of_mut!((*p).flow_id)),
                fragment_id: tm::tm_read_i32(std::ptr::addr_of_mut!((*p).fragment_id)),
                num_fragments: tm::tm_read_i32(std::ptr::addr_of_mut!((*p).num_fragments)),
                length: tm::tm_read_i32(std::ptr::addr_of_mut!((*p).length)),
                data: {
                    let mut buf = [0i8; INTRUDER_MAX_DATA];
                    for i in 0..INTRUDER_MAX_DATA {
                        buf[i] = tm::tm_read_i8(std::ptr::addr_of_mut!((*p).data[i]));
                    }
                    buf
                },
            };
            tm::tm_write_i32(data.packet_q_head, head + 1);
            pkt
        } else {
            Packet::default()
        }
    })
}

fn process_decoder(data: &IntruderData, pkt: &Packet) {
    if pkt.flow_id < 0 { return; }
    transaction(|_tx| unsafe {
        let fid = pkt.flow_id;
        let idx = (fid - 1) as usize;
        let storage_off = (fid - 1) as usize * INTRUDER_MAX_PACKETS * INTRUDER_MAX_DATA
                          + pkt.fragment_id as usize * INTRUDER_MAX_DATA;

        for i in 0..(pkt.length as usize).min(INTRUDER_MAX_DATA) {
            tm::tm_write_i8(data.fragment_storage.add(storage_off + i), pkt.data[i]);
        }

        let prev = tm::tm_read_i32(data.fragment_counts.add(idx));
        tm::tm_write_i32(data.fragment_counts.add(idx), prev + 1);

        if prev + 1 == pkt.num_fragments {
            let df_ptr = data.decoder_flows.add(idx * size_of::<DecodedFlow>()) as *mut DecodedFlow;
            let mut total: usize = 0;
            for f in 0..pkt.num_fragments as usize {
                let off = (fid - 1) as usize * INTRUDER_MAX_PACKETS * INTRUDER_MAX_DATA
                         + f * INTRUDER_MAX_DATA;
                let flen = pkt.length as usize;
                for i in 0..flen.min(INTRUDER_MAX_DATA) {
                    if total >= INTRUDER_MAX_DATA * 2 { break; }
                    let byte = tm::tm_read_i8(data.fragment_storage.add(off + i));
                    tm::tm_write_i8(std::ptr::addr_of_mut!((*df_ptr).data[total]), byte);
                    total += 1;
                }
            }

            tm::tm_write_i32(std::ptr::addr_of_mut!((*df_ptr).data_len), total as i32);
            tm::tm_write_i64(std::ptr::addr_of_mut!((*df_ptr).flow_id), fid);

            let qtail = tm::tm_read_i32(data.decoded_q_tail);
            if qtail < data.decoded_q_capacity {
                let dst_ptr = data.decoded_queue.add(qtail as usize * size_of::<DecodedFlow>()) as *mut DecodedFlow;
                tm::tm_write_i64(std::ptr::addr_of_mut!((*dst_ptr).flow_id), fid);
                tm::tm_write_i32(std::ptr::addr_of_mut!((*dst_ptr).data_len), total as i32);
                for i in 0..total {
                    let byte = tm::tm_read_i8(std::ptr::addr_of_mut!((*df_ptr).data[i]));
                    tm::tm_write_i8(std::ptr::addr_of_mut!((*dst_ptr).data[i]), byte);
                }
                tm::tm_write_i32(data.decoded_q_tail, qtail + 1);
            }
            tm::tm_write_i32(data.fragment_counts.add(idx), 0);
        }
    });
}

fn get_complete(data: &IntruderData) -> DecodedFlow {
    transaction(|_tx| unsafe {
        let head = tm::tm_read_i32(data.decoded_q_head);
        let tail = tm::tm_read_i32(data.decoded_q_tail);
        if head < tail {
            let src = data.decoded_queue.add(head as usize * size_of::<DecodedFlow>()) as *mut DecodedFlow;
            let data_len = tm::tm_read_i32(std::ptr::addr_of_mut!((*src).data_len)) as usize;
            let df = DecodedFlow {
                flow_id: tm::tm_read_i64(std::ptr::addr_of_mut!((*src).flow_id)),
                data_len: data_len as i32,
                data: {
                    let mut buf = [0i8; INTRUDER_MAX_DATA * 2];
                    for i in 0..data_len.min(INTRUDER_MAX_DATA * 2) {
                        buf[i] = tm::tm_read_i8(std::ptr::addr_of_mut!((*src).data[i]));
                    }
                    buf
                },
            };
            tm::tm_write_i32(data.decoded_q_head, head + 1);
            df
        } else {
            DecodedFlow::default()
        }
    })
}

// ── Attack detection (non-TX) ──────────────────────────────────────
fn detect_attack(data: &[i8], data_len: usize, dictionary: &[String]) -> bool {
    let len = data_len.min(INTRUDER_MAX_DATA * 2);
    let mut lower = String::with_capacity(len);
    for &c in data.iter().take(len) {
        let bc = c as u8;
        if bc >= b'A' && bc <= b'Z' {
            lower.push((bc - b'A' + b'a') as char);
        } else {
            lower.push(bc as char);
        }
    }
    dictionary.iter().any(|word| lower.contains(word))
}

// ── Worker thread ──────────────────────────────────────────────────
fn worker(data: &IntruderData) {
    tm::tm_init_thread();
    loop {
        let pkt = get_packet(data);
        if pkt.flow_id >= 0 {
            process_decoder(data, &pkt);
        }
        let df = get_complete(data);
        if df.flow_id >= 0 {
            let _attack = detect_attack(&df.data, df.data_len as usize, &data.dictionary);
            data.ops.fetch_add(1, Ordering::Relaxed);
        } else if pkt.flow_id < 0 {
            break;
        }
    }
    tm::tm_exit_thread();
}

// ── Public entry point ─────────────────────────────────────────────
pub fn test() -> i32 {
    let mut fails = 0;
    // Test detect_attack with dictionary
    let dict: Vec<String> = vec![
        "attack".to_string(), "virus".to_string(), "malware".to_string(),
    ];
    let mut data = [0i8; 256];
    let msg = b"this is an attack message";
    for (i, &c) in msg.iter().enumerate() { data[i] = c as i8; }
    if !detect_attack(&data, msg.len(), &dict) {
        eprintln!("FAIL: 'attack' not detected"); fails += 1;
    }
    let safe = b"hello world this is fine";
    for (i, &c) in safe.iter().enumerate() { data[i] = c as i8; }
    if detect_attack(&data, safe.len(), &dict) {
        eprintln!("FAIL: false positive for safe message"); fails += 1;
    }
    // Test case-insensitive matching
    let mixed = b"Virus found here";
    for (i, &c) in mixed.iter().enumerate() { data[i] = c as i8; }
    if !detect_attack(&data, mixed.len(), &dict) {
        eprintln!("FAIL: case-insensitive 'Virus' not detected"); fails += 1;
    }
    if fails > 0 { eprintln!("intruder: {} test(s) failed", fails); }
    fails
}

pub fn run(config: &Config, _stop: &AtomicBool, ops: &AtomicU64) {
    let max_flows = config.num_packets;
    let max_packets_per_flow = config.max_length.min(INTRUDER_MAX_PACKETS);

    // ── Build dictionary ──
    let wordlist: &[&str] = &[
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
    let dictionary: Vec<String> = wordlist.iter().map(|s| s.to_string()).collect();

    // ── Allocate TM-safe flat arrays ──
    let packet_q_capacity = (max_flows * max_packets_per_flow + 1024) as i32;
    let packet_queue = addrspace::tm_region_calloc(packet_q_capacity as usize * size_of::<Packet>(), 1);
    let packet_q_head = addrspace::tm_region_calloc(size_of::<i32>(), 1) as *mut i32;
    let packet_q_tail = addrspace::tm_region_calloc(size_of::<i32>(), 1) as *mut i32;

    let decoder_flows = addrspace::tm_region_calloc(max_flows * size_of::<DecodedFlow>(), 1);
    let fragment_storage = addrspace::tm_region_calloc(max_flows * INTRUDER_MAX_PACKETS * INTRUDER_MAX_DATA, 1) as *mut i8;
    let fragment_counts = addrspace::tm_region_calloc(max_flows * size_of::<i32>(), 1) as *mut i32;

    let decoded_q_capacity = max_flows as i32;
    let decoded_queue = addrspace::tm_region_calloc(decoded_q_capacity as usize * size_of::<DecodedFlow>(), 1);
    let decoded_q_head = addrspace::tm_region_calloc(size_of::<i32>(), 1) as *mut i32;
    let decoded_q_tail = addrspace::tm_region_calloc(size_of::<i32>(), 1) as *mut i32;

    // ── Generate packets (single-threaded, no TM) ──
    let queue_ptr = packet_queue as *mut Packet;
    let mut total_packets: usize = 0;
    unsafe {
        let tail = &mut *packet_q_tail;
        let mut rng = Rng::new(config.seed);

        for flow in 1..=max_flows {
            let is_attack = (rng.next() % 100) < config.percent_attack as u64;

            let (payload, payload_len): ([i8; INTRUDER_MAX_DATA * 2], usize) = if is_attack {
                let sig_idx = (rng.next() % dictionary.len() as u64) as usize;
                let sig = dictionary[sig_idx].as_bytes();
                let slen = sig.len().min(INTRUDER_MAX_DATA * 2);
                let mut buf = [0i8; INTRUDER_MAX_DATA * 2];
                for i in 0..slen { buf[i] = sig[i] as i8; }
                (buf, slen)
            } else {
                let plen = ((rng.next() % config.max_length as u64) + 1) as usize;
                let plen = plen.min(INTRUDER_MAX_DATA * 2);
                let mut buf = [0i8; INTRUDER_MAX_DATA * 2];
                for i in 0..plen { buf[i] = (32 + (rng.next() % 95)) as i8; }
                (buf, plen)
            };

            let num_frags = ((rng.next() % payload_len as u64) + 1) as usize;
            let num_frags = num_frags.max(1).min(INTRUDER_MAX_PACKETS);

            let base_len = payload_len / num_frags;
            let rem = payload_len % num_frags;
            let mut offset = 0;

            for f in 0..num_frags {
                let pkt = &mut *queue_ptr.add(*tail as usize + total_packets + f);
                pkt.flow_id = flow as i64;
                pkt.fragment_id = f as i32;
                pkt.num_fragments = num_frags as i32;
                let this_len = base_len + if f < rem { 1 } else { 0 };
                pkt.length = this_len as i32;
                for i in 0..this_len.min(INTRUDER_MAX_DATA) {
                    pkt.data[i] = payload[offset + i];
                }
                offset += this_len;
            }
            total_packets += num_frags;
        }
        *tail += total_packets as i32;
    }

    println!("Total packets generated = {}", total_packets);
    println!("Flows = {}", max_flows);
    println!("Max length = {}", config.max_length);
    println!("Attack % = {}%", config.percent_attack);
    println!("Seed = {}", config.seed);
    println!();

    // ── Run workers ──
    let data = Arc::new(IntruderData {
        packet_queue,
        packet_q_head,
        packet_q_tail,
        packet_q_capacity,
        decoder_flows,
        fragment_storage,
        fragment_counts,
        decoded_queue,
        decoded_q_head,
        decoded_q_tail,
        decoded_q_capacity,
        dictionary,
        ops: unsafe { &*(ops as *const AtomicU64) },
    });

    let start = std::time::Instant::now();
    std::thread::scope(|s| {
        for _ in 0..config.threads {
            let d = Arc::clone(&data);
            s.spawn(move || worker(&d));
        }
    });
    let elapsed = start.elapsed().as_millis();

    let total_ops = ops.load(Ordering::Relaxed);
    println!("Elapsed time = {}.{:03} seconds", elapsed / 1000, elapsed % 1000);
    println!("Num found = {}", total_ops);

    // ── Cleanup ──
    addrspace::tm_region_free(packet_queue);
    addrspace::tm_region_free(packet_q_head as *mut u8);
    addrspace::tm_region_free(packet_q_tail as *mut u8);
    addrspace::tm_region_free(decoder_flows);
    addrspace::tm_region_free(fragment_storage as *mut u8);
    addrspace::tm_region_free(fragment_counts as *mut u8);
    addrspace::tm_region_free(decoded_queue);
    addrspace::tm_region_free(decoded_q_head as *mut u8);
    addrspace::tm_region_free(decoded_q_tail as *mut u8);
}
