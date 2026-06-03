// Linked-list stress benchmark for the TM region allocator.
//
// Mirrors `backends/tests/test_region_stress.cpp`.

use std::alloc::{alloc, dealloc, Layout};
use std::time::Instant;
use std::{thread, ptr};

// ── Linked list node (16 bytes, maps to size class 0) ────────
#[repr(C)]
struct Node {
    next: *mut Node,
    value: i64,
}

// ── Allocator helpers ─────────────────────────────────────────
fn node_alloc_tm(value: i64) -> *mut Node {
    let p = addrspace::tm_region_malloc(size_of::<Node>());
    if p.is_null() {
        return ptr::null_mut();
    }
    unsafe {
        (*p.cast::<Node>()).next = ptr::null_mut();
        (*p.cast::<Node>()).value = value;
    }
    p.cast::<Node>()
}

fn list_free_tm(mut head: *mut Node) {
    while !head.is_null() {
        let next = unsafe { (*head).next };
        addrspace::tm_region_free(head.cast::<u8>());
        head = next;
    }
}

fn node_alloc_malloc(value: i64) -> *mut Node {
    let layout = Layout::new::<Node>();
    let p = unsafe { alloc(layout) };
    if p.is_null() {
        return ptr::null_mut();
    }
    unsafe {
        (*p.cast::<Node>()).next = ptr::null_mut();
        (*p.cast::<Node>()).value = value;
    }
    p.cast::<Node>()
}

fn list_free_malloc(mut head: *mut Node) {
    let layout = Layout::new::<Node>();
    while !head.is_null() {
        let next = unsafe { (*head).next };
        unsafe { dealloc(head.cast::<u8>(), layout) };
        head = next;
    }
}

// ── List operations ───────────────────────────────────────────
fn list_prepend(head: *mut Node, n: *mut Node) -> *mut Node {
    unsafe { (*n).next = head };
    n
}

fn list_sum(head: *const Node) -> i64 {
    let mut s: i64 = 0;
    let mut cur = head;
    while !cur.is_null() {
        s += unsafe { (*cur).value };
        cur = unsafe { (*cur).next };
    }
    s
}

// ── Benchmark helpers ─────────────────────────────────────────
#[allow(dead_code)]
struct BenchResult {
    name: &'static str,
    nodes: i64,
    sum: i64,
    secs: f64,
    allocs_per_sec: i64,
}

fn bench_single(name: &'static str, alloc: fn(i64) -> *mut Node, free_fn: fn(*mut Node), n: i64) -> BenchResult {
    let start = Instant::now();
    let mut head: *mut Node = ptr::null_mut();
    for i in 0..n {
        let node = alloc(i);
        head = list_prepend(head, node);
    }
    let ms = start.elapsed().as_millis() as i64;
    let sum = list_sum(head);
    let secs = ms as f64 / 1000.0;
    free_fn(head);
    BenchResult {
        name,
        nodes: n,
        sum,
        secs,
        allocs_per_sec: if secs > 0.0 { (n as f64 / secs) as i64 } else { 0 },
    }
}

fn bench_mt(name: &'static str, alloc: fn(i64) -> *mut Node, free_fn: fn(*mut Node), n: i64, n_threads: usize) -> BenchResult {
    let start = Instant::now();
    let _sum: i64 = (0..n_threads)
        .map(|ti| {
            thread::spawn(move || {
                let mut head: *mut Node = ptr::null_mut();
                for i in 0..n {
                    let node = alloc((ti as i64) * n + i);
                    head = list_prepend(head, node);
                }
                let s = list_sum(head);
                free_fn(head);
                s
            })
        })
        .collect::<Vec<_>>()
        .into_iter()
        .map(|h| h.join().unwrap())
        .sum();
    let ms = start.elapsed().as_millis() as i64;
    let secs = ms as f64 / 1000.0;
    let total = n * n_threads as i64;
    BenchResult {
        name,
        nodes: total,
        sum: 0,
        secs,
        allocs_per_sec: if secs > 0.0 { (total as f64 / secs) as i64 } else { 0 },
    }
}

fn main() {
    let n_nodes: i64 = std::env::args().nth(1).and_then(|s| s.parse().ok()).unwrap_or(500_000);
    let n_threads: usize = std::env::args().nth(2).and_then(|s| s.parse().ok()).unwrap_or(4);
    let hw = thread::available_parallelism().map(|x| x.get()).unwrap_or(4);
    let n_threads = n_threads.min(hw);

    println!("\n═══ TM Region Allocator Stress Test (Rust) ═══\n");
    println!("Linked list: {} nodes/thread, {} threads\n", n_nodes, n_threads);

    // ── Phase 1: init ──────────────────────────────────────────
    println!("Phase 1: Initialise TM region");
    let rc = addrspace::tm_region_init();
    assert_eq!(rc, 0, "tm_region_init failed");
    if let Some(stats) = addrspace::tm_region_stats() {
        println!("  {} slabs × {} B each ({} total), {} B/slab",
                 stats.slab_count, stats.slab_size,
                 stats.slab_count * stats.slab_size,
                 stats.slab_size);
    }
    println!();

    // ── Phase 2: correctness ──────────────────────────────────
    println!("Phase 2: Correctness");
    {
        let mut head: *mut Node = ptr::null_mut();
        for i in 0..n_nodes {
            let n = node_alloc_tm(i);
            assert!(!n.is_null());
            head = list_prepend(head, n);
        }
        let sum = list_sum(head);
        let expected = n_nodes * (n_nodes - 1) / 2;
        let ok = sum == expected;
        println!("  sum = {}, expected = {}  {}", sum, expected, if ok { "PASS" } else { "FAIL" });
        assert!(ok, "data corruption");
        list_free_tm(head);
    }

    // ── Phase 3: single-threaded throughput ───────────────────
    println!("\nPhase 3: Single-threaded throughput");
    println!("  {:<14} {:>10} {:>14}", "Allocator", "Time (ms)", "Allocs/sec");
    println!("  ────────────── ────────── ──────────────");

    let r_tm  = bench_single("TM region",   node_alloc_tm,   list_free_tm,   n_nodes);
    let r_mal = bench_single("std::alloc",  node_alloc_malloc, list_free_malloc, n_nodes);

    println!("  {:<14} {:>10} {:>14}", r_tm.name,  (r_tm.secs * 1000.0) as i64, r_tm.allocs_per_sec);
    println!("  {:<14} {:>10} {:>14}", r_mal.name, (r_mal.secs * 1000.0) as i64, r_mal.allocs_per_sec);

    let ratio_st = r_tm.allocs_per_sec as f64 / r_mal.allocs_per_sec.max(1) as f64;
    println!("\n  Speedup: {:.1}× (TM region vs std::alloc)", ratio_st);

    // ── Phase 4: multi-threaded throughput ────────────────────
    println!("\nPhase 4: Multi-threaded throughput ({} threads)", n_threads);
    println!("  {:<14} {:>10} {:>14}", "Allocator", "Time (ms)", "Allocs/sec");
    println!("  ────────────── ────────── ──────────────");

    let per = n_nodes / n_threads as i64;
    let r_tm_mt  = bench_mt("TM region",   node_alloc_tm,   list_free_tm,   per, n_threads);
    let r_mal_mt = bench_mt("std::alloc",  node_alloc_malloc, list_free_malloc, per, n_threads);

    println!("  {:<14} {:>10} {:>14}", r_tm_mt.name,  (r_tm_mt.secs * 1000.0) as i64, r_tm_mt.allocs_per_sec);
    println!("  {:<14} {:>10} {:>14}", r_mal_mt.name, (r_mal_mt.secs * 1000.0) as i64, r_mal_mt.allocs_per_sec);

    let ratio_mt = r_tm_mt.allocs_per_sec as f64 / r_mal_mt.allocs_per_sec.max(1) as f64;
    println!("\n  Speedup: {:.1}× (TM region vs std::alloc)", ratio_mt);

    // ── Phase 5: scalability ──────────────────────────────────
    println!("\nPhase 5: Scalability (TM allocator)");
    println!("  {:>7} {:>10} {:>14}", "Threads", "Time (ms)", "Allocs/sec");
    println!("  ─────── ────────── ──────────────");

    for tc in [1, 2, 4] {
        if tc > hw { continue; }
        let p = n_nodes / tc as i64;
        let r = bench_mt("", node_alloc_tm, list_free_tm, p, tc);
        println!("  {:>7} {:>10} {:>14}", tc, (r.secs * 1000.0) as i64, r.allocs_per_sec);
    }

    // ── Phase 6: free/reuse correctness ───────────────────────
    println!("\nPhase 6: Free/Reuse correctness");
    {
        const N: usize = 10000;
        let mut ptrs = [ptr::null_mut::<Node>(); N];
        for i in 0..N { ptrs[i] = node_alloc_tm(i as i64); }
        for i in 0..N { addrspace::tm_region_free(ptrs[i].cast::<u8>()); }
        for i in 0..N { ptrs[i] = node_alloc_tm((i + N) as i64); }
        let ok = (0..N).all(|i| {
            unsafe { ptrs[i].is_null() || (*ptrs[i]).value == (i + N) as i64 }
        });
        println!("  free+reuse: {}", if ok { "PASS" } else { "FAIL" });
        assert!(ok, "free/reuse corruption");
        for i in 0..N { addrspace::tm_region_free(ptrs[i].cast::<u8>()); }
    }

    // ── Phase 7: alloc/free exceeding 64 GB total ─────────────
    println!("\nPhase 7: Alloc/free exceeding 64 GB total");
    {
        let total_gb = 2;
        let total_bytes = (total_gb as i64) * 1024 * 1024 * 1024;
        let iterations = total_bytes / size_of::<Node>() as i64;

        let start = Instant::now();
        for i in 0..iterations {
            let n = node_alloc_tm(i);
            addrspace::tm_region_free(n.cast::<u8>());
        }
        let ms = start.elapsed().as_millis() as i64;
        let secs = ms as f64 / 1000.0;
        let rate = if secs > 0.0 { (iterations as f64 / secs) as i64 } else { 0 };

        let pass = if let Some(st) = addrspace::tm_region_stats() {
            // slabs used * slab_size should be < TOTAL_GB if recycling worked
            (st.next_slab_idx * st.slab_size) < (total_gb as usize) * 1024 * 1024 * 1024
        } else {
            false
        };
        println!("  {} iters in {:.1}s  ({} allocs/sec)  {}",
                 iterations, secs, rate, if pass { "PASS" } else { "FAIL" });
        assert!(pass, "alloc/free stress — region exhausted");
    }

    // ── Phase 8: cleanup ──────────────────────────────────────
    println!("\nPhase 8: Destroy");
    addrspace::tm_region_destroy();

    let speedup = if ratio_st > ratio_mt { ratio_st } else { ratio_mt };
    println!("\n═══ {} ({:.1}× speedup over std::alloc) ═══\n",
             if speedup >= 1.0 { "TM ALLOCATOR FASTER" } else { "MALLOC FASTER" },
             speedup);
}
