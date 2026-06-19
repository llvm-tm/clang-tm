use std::cell::RefCell;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::Arc;
use tm::{transaction, TmCell};
use crate::Rng;
use super::Config;

fn sqrt_approx(x: f64) -> f64 {
    if x <= 0.0 { return 0.0; }
    let mut s = x;
    for _ in 0..25 {
        let ns = (s + x / s) * 0.5;
        if (ns - s).abs() < 1e-15 { break; }
        s = ns;
    }
    s
}

struct KMeansData {
    npoints: usize,
    ndims: usize,
    nclusters: usize,
    threshold: f64,
    points: Vec<TmCell<f64>>,
    centroids: Vec<TmCell<f64>>,
    assignments: Vec<TmCell<i32>>,
    new_centers_sum: Vec<TmCell<f64>>,
    new_centers_count: Vec<TmCell<i32>>,
}

fn accumulate(tid: usize, num_threads: usize, d: &KMeansData,
              local_sum: &RefCell<Vec<f64>>, local_count: &RefCell<Vec<i32>>) {
    let chunk = (d.npoints + num_threads - 1) / num_threads;
    let start = tid * chunk;
    let end = (start + chunk).min(d.npoints);

    transaction(|tx| {
        let mut ls = local_sum.borrow_mut();
        let mut lc = local_count.borrow_mut();
        for i in start..end {
            let mut best = -1i32;
            let mut best_dist = f64::MAX;
            for c in 0..d.nclusters {
                let mut dist = 0.0;
                for dim in 0..d.ndims {
                    let diff = tx.read(&d.points[i * d.ndims + dim])
                            - tx.read(&d.centroids[c * d.ndims + dim]);
                    dist += diff * diff;
                }
                if dist < best_dist { best_dist = dist; best = c as i32; }
            }
            tx.write(&d.assignments[i], best);
            lc[best as usize] += 1;
            for dim in 0..d.ndims {
                ls[best as usize * d.ndims + dim] += tx.read(&d.points[i * d.ndims + dim]);
            }
        }
    });
}

pub fn run(config: &Config, _stop: &AtomicBool, _ops: &AtomicU64) {
    println!("\n=== KMeans ===");
    let npoints = config.points.max(1);
    let nclusters = config.clusters.max(1);
    let ndims = config.dims.max(1);
    let threshold = 0.00001;
    println!("  Points: {}  Dims: {}  Clusters: {}  Threshold: {}",
             npoints, ndims, nclusters, threshold);

    let mut d = KMeansData {
        npoints, ndims, nclusters, threshold,
        points: (0..npoints * ndims).map(|_| TmCell::new(0.0)).collect(),
        centroids: (0..nclusters * ndims).map(|_| TmCell::new(0.0)).collect(),
        assignments: (0..npoints).map(|_| TmCell::new(-1)).collect(),
        new_centers_sum: (0..nclusters * ndims).map(|_| TmCell::new(0.0)).collect(),
        new_centers_count: (0..nclusters).map(|_| TmCell::new(0)).collect(),
    };

    let mut rng = Rng::new(42);
    for i in 0..npoints {
        let cluster = (i % nclusters) as f64;
        for dim in 0..ndims {
            let u = rng.uniform();
            let val = (-10.0 + u * 20.0) + cluster * 5.0;
            unsafe { *d.points[i * ndims + dim].ptr() = val; }
        }
    }
    for c in 0..nclusters {
        for dim in 0..ndims {
            let u = rng.uniform();
            let val = -10.0 + u * 20.0;
            unsafe { *d.centroids[c * ndims + dim].ptr() = val; }
        }
    }

    let data = Arc::new(d);
    let converged = AtomicBool::new(false);
    let g_ops = AtomicU64::new(0);
    let t0 = std::time::Instant::now();

    std::thread::scope(|s| {
        for tid in 0..config.threads {
            let d = data.clone();
            let cv = &converged;
            let go = &g_ops;
            s.spawn(move || {
                let local_sum = RefCell::new(vec![0.0f64; nclusters * ndims]);
                let local_count = RefCell::new(vec![0i32; nclusters]);
                let mut max_iters = 100i32;

                while !cv.load(Ordering::Relaxed) && max_iters > 0 {
                    max_iters -= 1;
                    local_sum.borrow_mut().fill(0.0);
                    local_count.borrow_mut().fill(0);

                    accumulate(tid, config.threads, &d, &local_sum, &local_count);

                    // Merge into globals (non-TX, matches C++ explicit API)
                    {
                        let ls = local_sum.borrow();
                        let lc = local_count.borrow();
                        for c in 0..nclusters {
                            unsafe {
                                *d.new_centers_count[c].ptr() += lc[c];
                                for dim in 0..ndims {
                                    *d.new_centers_sum[c * ndims + dim].ptr() += ls[c * ndims + dim];
                                }
                            }
                        }
                    }

                    // Update centroids (all threads, matches C++ semantics)
                    let mut delta = 0.0;
                    for c in 0..nclusters {
                        let cnt = unsafe { *d.new_centers_count[c].ptr() };
                        if cnt > 0 {
                            for dim in 0..ndims {
                                let sum = unsafe { *d.new_centers_sum[c * ndims + dim].ptr() };
                                let new_val = sum / cnt as f64;
                                let old_val = unsafe { *d.centroids[c * ndims + dim].ptr() };
                                let diff = old_val - new_val;
                                delta += diff * diff;
                                unsafe { *d.centroids[c * ndims + dim].ptr() = new_val; }
                                unsafe { *d.new_centers_sum[c * ndims + dim].ptr() = 0.0; }
                            }
                        }
                        unsafe { *d.new_centers_count[c].ptr() = 0; }
                    }
                    delta = sqrt_approx(delta / (nclusters * ndims) as f64);
                    if delta < threshold {
                        cv.store(true, Ordering::Relaxed);
                    }
                    go.fetch_add(npoints as u64, Ordering::Relaxed);
                }
            });
        }
    });

    let elapsed = t0.elapsed().as_millis() as u64;
    let iter_ops = g_ops.load(Ordering::Relaxed);
    println!("  Operations: {}  Elapsed: {} ms  Rate: {} ops/s",
             iter_ops, elapsed, if elapsed > 0 { iter_ops * 1000 / elapsed } else { 0 });

    println!("  Centroids:");
    let d = &*data;
    for c in 0..nclusters.min(10) {
        print!("    [{c}] ");
        for dim in 0..ndims.min(5) {
            if dim > 0 { print!(", "); }
            print!("{:.6}", unsafe { *d.centroids[c * ndims + dim].ptr() });
        }
        if ndims > 5 { print!(", ..."); }
        println!();
    }
}
