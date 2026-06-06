use std::collections::{HashMap, HashSet};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Instant;
use tm::*;
use benchmarks::Rng;

fn str_hash_range(s: &[u8], start: usize, len: usize) -> u64 {
    let mut h: u64 = 0;
    for i in start..start + len {
        h = h.wrapping_mul(131).wrapping_add(s[i] as u64);
    }
    h
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let mut num_threads = 4;
    let mut gene_length = 16384;
    let mut segment_length = 64;
    let mut num_segments = 1_000_000;

    let mut i = 1;
    while i < args.len() {
        match args[i].as_str() {
            "-p" => { i += 1; num_threads = args[i].parse().unwrap(); }
            "-g" => { i += 1; gene_length = args[i].parse().unwrap(); }
            "-s" => { i += 1; segment_length = args[i].parse().unwrap(); }
            "-n" => { i += 1; num_segments = args[i].parse().unwrap(); }
            _ => {}
        }
        i += 1;
    }

    let bases = [b'a', b'c', b'g', b't'];

    println!("Creating gene and segments... done.");
    println!("Gene length     = {}", gene_length);
    println!("Segment length  = {}", segment_length);
    println!("Number segments = {}", num_segments);
    println!("Sequencing gene...");

    tm_init();

    // Generate gene
    let mut rng = Rng::new(42);
    let mut gene_bytes = Vec::with_capacity(gene_length);
    for _ in 0..gene_length {
        gene_bytes.push(bases[rng.next() as usize % 4]);
    }
    let gene = unsafe { String::from_utf8_unchecked(gene_bytes) };

    // Generate segments
    let segments: Vec<String> = (0..num_segments)
        .map(|_| {
            let start = rng.next() as usize % (gene_length - segment_length);
            gene[start..start + segment_length].to_string()
        })
        .collect();

    let unique_segments = Arc::new(Mutex::new(HashSet::new()));
    let total_ops = Arc::new(AtomicU64::new(0));

    let start = Instant::now();

    // Phase 1: Dedup (parallel with mutex)
    std::thread::scope(|s| {
        for tid in 0..num_threads {
            let segments = &segments;
            let unique_segments = unique_segments.clone();

            s.spawn(move || {
                tm_init_thread();

                let chunk = (num_segments + num_threads - 1) / num_threads;
                let start_idx = tid * chunk;
                let end_idx = std::cmp::min(start_idx + chunk, num_segments);

                for i in start_idx..end_idx {
                    let mut u = unique_segments.lock().unwrap();
                    u.insert(segments[i].clone());
                }

                tm_exit_thread();
            });
        }
    });

    // Phase 2: Match
    let unique: Vec<String> = unique_segments.lock().unwrap().iter().cloned().collect();
    let num_unique = unique.len();

    // Build hash table: map suffix hash -> indices into `unique`
    let mut ht: HashMap<u64, Vec<usize>> = HashMap::new();
    for (idx, s) in unique.iter().enumerate() {
        if s.len() > 1 {
            let h = str_hash_range(s.as_bytes(), 0, s.len() - 1);
            ht.entry(h).or_default().push(idx);
        }
    }

    let unique = Arc::new(unique);
    let ht = Arc::new(ht);
    let reconstructed = Arc::new(Mutex::new(Vec::new()));

    std::thread::scope(|s| {
        for tid in 0..num_threads {
            let unique = unique.clone();
            let ht = ht.clone();
            let total_ops = total_ops.clone();
            let reconstructed = reconstructed.clone();

            s.spawn(move || {
                tm_init_thread();

                let chunk = (num_unique + num_threads - 1) / num_threads;
                let start_idx = tid * chunk;
                let end_idx = std::cmp::min(start_idx + chunk, num_unique);

                for j in (1..segment_length).rev() {
                    for idx in start_idx..end_idx {
                        let seg = &unique[idx];
                        if seg.len() <= j { continue; }
                        let end_h = str_hash_range(seg.as_bytes(), seg.len() - j, j);
                        if let Some(candidates) = ht.get(&end_h) {
                            for &cidx in candidates {
                                if cidx == idx { continue; }
                                let candidate = &unique[cidx];
                                if candidate.len() < j { continue; }
                                let seg_suffix = &seg[seg.len() - j..];
                                let cand_prefix = &candidate[..j];
                                if seg_suffix == cand_prefix {
                                    let mut r = reconstructed.lock().unwrap();
                                    r.push(format!("{}{}", seg, &candidate[j..]));
                                    total_ops.fetch_add(1, Ordering::Relaxed);
                                    break;
                                }
                            }
                        }
                    }
                }

                tm_exit_thread();
            });
        }
    });

    let elapsed = start.elapsed();
    let ops = total_ops.load(Ordering::Relaxed);

    println!("    Time = {} ms", elapsed.as_millis());
    println!("    Unique segments = {}", num_unique);
    println!("    Reconstructed = {}", ops);

    tm_exit();
}
