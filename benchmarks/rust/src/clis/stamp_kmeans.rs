use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Barrier};
use std::time::Instant;
use tm::*;
use benchmarks::Rng;

fn euclidean_dist_sq(a: &[f64], b: &[f64]) -> f64 {
    a.iter().zip(b.iter()).map(|(x, y)| (x - y) * (x - y)).sum()
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let mut npoints = 200;
    let mut nclusters = 8;
    let mut ndims = 2;
    let mut num_threads = 2;
    let mut max_iter = 100;

    let mut i = 1;
    while i < args.len() {
        match args[i].as_str() {
            "-p" => { i += 1; num_threads = args[i].parse().unwrap(); }
            "-n" => { i += 1; npoints = args[i].parse().unwrap(); }
            "-c" => { i += 1; nclusters = args[i].parse().unwrap(); }
            "-d" => { i += 1; ndims = args[i].parse().unwrap(); }
            "-i" => { i += 1; max_iter = args[i].parse().unwrap(); }
            "-h" => {
                println!("Usage: {} -p <threads> -n <points> -c <clusters> -d <dims> -i <max_iter>", args[0]);
                return;
            }
            _ => {}
        }
        i += 1;
    }

    println!("Points    = {}", npoints);
    println!("Clusters  = {}", nclusters);
    println!("Dims      = {}", ndims);
    println!("Max iter  = {}", max_iter);
    println!("Threads   = {}", num_threads);

    tm_init();

    // Generate random points and cluster centers (same algorithm as C++ kmeans.cpp)
    let mut rng = Rng::new(42);
    let mut points = Vec::with_capacity(npoints * ndims);
    for i in 0..npoints {
        let cluster = i % nclusters;
        for _d in 0..ndims {
            let u = rng.uniform();
            points.push(TmCell::new((-10.0 + u * 20.0) + cluster as f64 * 5.0));
        }
    }
    let clusters: Vec<TmCell<f64>> = (0..nclusters * ndims).map(|_| {
        let u = rng.uniform();
        TmCell::new(-10.0 + u * 20.0)
    }).collect();
    let assignments: Vec<TmCell<i32>> = (0..npoints).map(|_| TmCell::new(0)).collect();
    let _membership: Vec<TmCell<i32>> = (0..npoints).map(|_| TmCell::new(0)).collect();

    let points = Arc::new(points);
    let clusters = Arc::new(clusters);
    let assignments = Arc::new(assignments);
    let _membership = Arc::new(_membership);
    let converged = Arc::new(AtomicBool::new(false));
    let barrier = Arc::new(Barrier::new(num_threads));

    let start = Instant::now();

    std::thread::scope(|s| {
        for tid in 0..num_threads {
            let points = points.clone();
            let clusters = clusters.clone();
            let assignments = assignments.clone();
            let _membership = _membership.clone();
            let converged = converged.clone();
            let barrier = barrier.clone();

            s.spawn(move || {
                tm_init_thread();
                let chunk = (npoints + num_threads - 1) / num_threads;
                let start_pt = tid * chunk;
                let end_pt = std::cmp::min(start_pt + chunk, npoints);

                for _iter in 0..max_iter {
                    if converged.load(Ordering::Acquire) { break; }

                    // Phase 1: assign each point to nearest cluster
                    for p in start_pt..end_pt {
                        transaction(|tx| {
                            let mut best = -1i32;
                            let mut best_dist = f64::MAX;
                            for c in 0..nclusters {
                                let mut dist = 0.0f64;
                                for d in 0..ndims {
                                    let diff = tx.read(&points[p * ndims + d])
                                        - tx.read(&clusters[c * ndims + d]);
                                    dist += diff * diff;
                                }
                                if dist < best_dist {
                                    best_dist = dist;
                                    best = c as i32;
                                }
                            }
                            tx.write(&assignments[p], best);
                        });
                    }

                    barrier.wait();

                    // Phase 2: centroid update (thread 0 only)
                    if tid == 0 {
                        for c in 0..nclusters {
                            let mut sum = vec![0.0f64; ndims];
                            let mut count = 0i32;
                            for p in 0..npoints {
                                let a = transaction(|tx| tx.read(&assignments[p]));
                                if a == c as i32 {
                                    for d in 0..ndims {
                                        sum[d] += unsafe { *points[p * ndims + d].ptr() };
                                    }
                                    count += 1;
                                }
                            }
                            let cvd = transaction(|tx| {
                                let mut delta = 0.0f64;
                                for d in 0..ndims {
                                    let new_val = sum[d] / count as f64;
                                    let old_val = tx.read(&clusters[c * ndims + d]);
                                    delta += (new_val - old_val) * (new_val - old_val);
                                    tx.write(&clusters[c * ndims + d], new_val);
                                }
                                delta.sqrt() < 0.001
                            });
                            if cvd { converged.store(true, Ordering::Release); }
                        }
                    }

                    barrier.wait();
                }

                tm_exit_thread();
            });
        }
    });

    let elapsed = start.elapsed();
    println!("    Time = {} ms", elapsed.as_millis());

    // Print final centroids for verification
    println!("  Centroids:");
    for c in 0..nclusters {
        print!("    [{}] ", c);
        for d in 0..ndims {
            let v = unsafe { *clusters[c * ndims + d].ptr() };
            if d > 0 { print!(", "); }
            print!("{:.6}", v);
        }
        println!();
    }

    tm_exit();
}

#[cfg(test)]
mod tests {
    use super::*;
    use benchmarks::Rng;

    #[test]
    fn test_rng_determinism() {
        let mut a = Rng::new(42);
        let mut b = Rng::new(42);
        for _ in 0..1000 {
            assert_eq!(a.next(), b.next());
        }
    }

    #[test]
    fn test_euclidean_dist_sq() {
        let a = [0.0, 0.0, 0.0];
        let b = [1.0, 2.0, 3.0];
        let dist = euclidean_dist_sq(&a, &b);
        assert!((dist - 14.0).abs() < 1e-10, "dist = {}", dist);
    }

    #[test]
    fn test_euclidean_dist_sq_same_point() {
        let a = [3.5, 7.2, 1.0];
        let dist = euclidean_dist_sq(&a, &a);
        assert!((dist - 0.0).abs() < 1e-10, "dist = {}", dist);
    }

    #[test]
    fn test_euclidean_dist_sq_2d() {
        let a = [0.0, 0.0];
        let b = [3.0, 4.0];
        let dist = euclidean_dist_sq(&a, &b);
        assert!((dist - 25.0).abs() < 1e-10, "dist = {}", dist);
    }
}
