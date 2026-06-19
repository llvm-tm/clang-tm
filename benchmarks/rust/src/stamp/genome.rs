use std::collections::{HashMap, HashSet};
use std::sync::atomic::{AtomicBool, AtomicU64};
use crate::Rng;
use super::Config;

fn str_hash(s: &[u8], start: usize, len: usize) -> u64 {
    let mut h = 0u64;
    for i in start..start + len {
        h = h.wrapping_mul(131).wrapping_add(s[i] as u64);
    }
    h
}

pub fn test() -> i32 {
    let mut fails = 0;
    let mut rng = Rng::new(42);
    let bases = [b'a', b'c', b'g', b't'];
    let gene_len = 100; let seg_len = 10;
    let gene: Vec<u8> = (0..gene_len).map(|_| bases[(rng.next() % 4) as usize]).collect();
    if gene.len() != 100 { eprintln!("FAIL: gene length {}", gene.len()); fails += 1; }
    for _ in 0..50 {
        let start = (rng.next() as usize) % (gene_len - seg_len);
        let seg = &gene[start..start + seg_len];
        if seg.len() != seg_len { eprintln!("FAIL: segment length"); fails += 1; }
        if seg.len() > gene.len() { eprintln!("FAIL: segment > gene"); fails += 1; }
    }
    // Test str_hash
    let h1 = str_hash(&gene, 0, 5);
    let h2 = str_hash(&gene, 0, 5);
    if h1 != h2 { eprintln!("FAIL: hash not deterministic"); fails += 1; }
    if fails > 0 { eprintln!("genome: {} test(s) failed", fails); }
    fails
}

pub fn run(config: &Config, _stop: &AtomicBool, _ops: &AtomicU64) {
    println!("\n=== Genome ===");
    let gene_len = config.gene_length.max(1).min(1 << 20);
    let seg_len = config.segment_length.max(1).min(gene_len - 1);
    let num_seg = config.num_segments.max(1).min(1 << 20);
    println!("  Gene length: {}  Segment length: {}  Segments: {}",
             gene_len, seg_len, num_seg);

    let mut rng = Rng::new(42);
    let bases = [b'a', b'c', b'g', b't'];

    // Generate gene
    let gene: Vec<u8> = (0..gene_len).map(|_| bases[(rng.next() % 4) as usize]).collect();

    // Generate segments
    let segments: Vec<Vec<u8>> = (0..num_seg)
        .map(|_| {
            let start = (rng.next() as usize) % (gene_len - seg_len);
            gene[start..start + seg_len].to_vec()
        })
        .collect();

    // Dedup (sequential, no TM — matches C++ single-threaded approach)
    let unique: HashSet<Vec<u8>> = segments.into_iter().collect();
    let unique_segs: Vec<Vec<u8>> = unique.into_iter().collect();
    println!("  Unique segments: {}", unique_segs.len());

    // Match: find overlapping suffix/prefix matches
    let mut hash_table: HashMap<u64, Vec<usize>> = HashMap::new();
    for (idx, s) in unique_segs.iter().enumerate() {
        if s.len() > 1 {
            let h = str_hash(s, 0, s.len() - 1);
            hash_table.entry(h).or_default().push(idx);
        }
    }

    let mut matches = 0u64;
    for j in (1..seg_len).rev() {
        for (idx, s) in unique_segs.iter().enumerate() {
            if s.len() <= j { continue; }
            let end_h = str_hash(s, s.len() - j, j);
            if let Some(candidates) = hash_table.get(&end_h) {
                for &cidx in candidates {
                    if cidx == idx { continue; }
                    let cs = &unique_segs[cidx];
                    if cs.len() < j { continue; }
                    if s[s.len() - j..s.len()] == cs[0..j] {
                        matches += 1;
                    }
                }
            }
        }
    }
    println!("  Overlapping matches: {}", matches);
}
