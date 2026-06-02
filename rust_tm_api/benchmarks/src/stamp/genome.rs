use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::Arc;
use std::cell::RefCell;
use tm::{transaction, TmCell};
use crate::Rng;
use super::Config;

pub fn run(config: &Config, stop: &AtomicBool, ops: &AtomicU64) {
    println!("\n=== Genome ===");
    println!("  Gene length: {}  Segments: {}  Segment length: {}",
             config.gene_length, config.num_segments, config.segment_length);

    let hash_size: usize = 1 << 18;
    let hash_table: Vec<TmCell<u32>> = (0..hash_size).map(|_| TmCell::new(u32::MAX)).collect();
    let gene_len = config.gene_length.min(1 << 20);
    let gene: Vec<u8> = (0..gene_len).map(|i| (i.wrapping_mul(6364136223846793005) >> 40 & 0x3) as u8).collect();
    let table = Arc::new(hash_table);
    let dur = std::time::Duration::from_millis(config.duration as u64);
    let num_seg = config.num_segments.min(1 << 20);
    let g_matches = AtomicU64::new(0);
    let g_reads = AtomicU64::new(0);

    let t0 = std::time::Instant::now();
    std::thread::scope(|s| {
        for tid in 0..config.threads {
            let ht = table.clone();
            let g = gene.clone();
            let sc = stop;
            let so = ops.clone();
            let gm = &g_matches;
            let gr = &g_reads;
            s.spawn(move || {
                let rng = RefCell::new(Rng::new(tid as u64 * 12345 + 42));
                let chunk = num_seg / config.threads;
                let start = tid * chunk;
                let end = if tid == config.threads - 1 { num_seg } else { start + chunk };

                while !sc.load(Ordering::Relaxed) {
                    for _ in start..end {
                        let pos = (rng.borrow_mut().next() as usize) % (gene_len.saturating_sub(config.segment_length));
                        let mut hash_val = 0u32;
                        for j in 0..config.segment_length {
                            hash_val = hash_val.wrapping_mul(31).wrapping_add(g.get(pos + j).copied().unwrap_or(0) as u32);
                        }
                        let bucket = (hash_val as usize) % hash_size;

                        let new_match = transaction(|tx| {
                            let cur = tx.read(&ht[bucket]);
                            if cur == u32::MAX || cur == hash_val {
                                tx.write(&ht[bucket], hash_val);
                                cur == u32::MAX
                            } else {
                                false
                            }
                        });

                        if new_match { gm.fetch_add(1, Ordering::Relaxed); }
                        gr.fetch_add(1, Ordering::Relaxed);
                    }
                    so.fetch_add(1, Ordering::Relaxed);
                }
            });
        }
        std::thread::sleep(dur);
        stop.store(true, Ordering::Relaxed);
    });
    let elapsed = t0.elapsed().as_millis() as u64;
    let matches = g_matches.load(Ordering::Relaxed);
    let reads = g_reads.load(Ordering::Relaxed);
    println!("  Matches: {}  Reads: {}  Elapsed: {} ms", matches, reads, elapsed);
}
