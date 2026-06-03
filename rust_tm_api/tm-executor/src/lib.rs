//! Execution-strategy abstraction for TM transactions.
//!
//! Separates *where* a TX runs from the retry loop.
//! Users write:
//!
//! ```ignore
//! let exec = InlineExecutor;
//! tx_execute(&exec, |tx| { balance.write(tx, new_val); });
//! ```
//!
//! For queue mode:
//! ```ignore
//! let exec = QueueExecutor::new(4, 4);
//! tx_execute(&exec, |tx| { balance.write(tx, new_val); });
//! ```

use std::collections::VecDeque;
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::sync::{Arc, Condvar, Mutex};
use std::thread::{self, JoinHandle};

// ── TxExecutor trait ───────────────────────────────────────────────

/// Execution-strategy abstraction.
///
/// `execute` runs `body` synchronously (blocks until the TX commits).
/// `execute_async` runs `body` asynchronously, returning a `JoinHandle`.
pub trait TxExecutor: Send + Sync {
    fn execute(&self, body: Box<dyn FnOnce() + Send>);
    fn execute_async(&self, body: Box<dyn FnOnce() + Send>) -> JoinHandle<()>;
}

// ── InlineExecutor ─────────────────────────────────────────────────

/// Runs all TXes on the caller's thread immediately.
pub struct InlineExecutor;

impl TxExecutor for InlineExecutor {
    fn execute(&self, body: Box<dyn FnOnce() + Send>) {
        body();
    }

    fn execute_async(&self, body: Box<dyn FnOnce() + Send>) -> JoinHandle<()> {
        thread::spawn(body)
    }
}

// ── QueueExecutor ──────────────────────────────────────────────────

/// Thread-pool executor with round-robin queue distribution,
/// work stealing, and spin-wait synchronous execution.
pub struct QueueExecutor {
    /// Shared state (queues, CVs, shutdown flag).
    inner: Arc<QueueInner>,
    /// Owned join handles (must outlive the Arc).
    workers: Vec<JoinHandle<()>>,
}

struct QueueInner {
    num_q: usize,
    shutdown: AtomicBool,
    next_q: AtomicUsize,
    next_wq: AtomicUsize,
    queues: Vec<Mutex<VecDeque<Box<dyn FnOnce() + Send>>>>,
    cvs: Vec<Condvar>,
}

impl QueueExecutor {
    pub fn new(num_workers: usize, num_queues: usize) -> Self {
        let nq = num_queues.max(1);
        let nw = num_workers.max(1);
        let inner = Arc::new(QueueInner {
            num_q: nq,
            shutdown: AtomicBool::new(false),
            next_q: AtomicUsize::new(0),
            next_wq: AtomicUsize::new(0),
            queues: (0..nq).map(|_| Mutex::new(VecDeque::new())).collect(),
            cvs: (0..nq).map(|_| Condvar::new()).collect(),
        });

        let mut workers = Vec::with_capacity(nw);
        for _ in 0..nw {
            let s = Arc::clone(&inner);
            workers.push(thread::spawn(move || Self::worker_loop(&s)));
        }

        QueueExecutor { inner, workers }
    }

    fn worker_loop(inner: &QueueInner) {
        loop {
            let start_q =
                inner.next_wq.fetch_add(1, Ordering::Relaxed) % inner.num_q;

            let task: Option<Box<dyn FnOnce() + Send>>;
            let mut waited = false;

            'search: loop {
                for a in 0..inner.num_q {
                    let q = (start_q + a) % inner.num_q;
                    let mut guard = inner.queues[q].lock().unwrap();
                    if let Some(f) = guard.pop_front() {
                        task = Some(f);
                        break 'search;
                    }
                    if !waited && a == 0 {
                        // Block on our starting queue
                        guard = inner.cvs[q]
                            .wait_while(guard, |q| {
                                q.is_empty()
                                    && !inner.shutdown.load(Ordering::Relaxed)
                            })
                            .unwrap();
                        waited = true;
                        // Re-check after wake
                        if let Some(f) = guard.pop_front() {
                            task = Some(f);
                            break 'search;
                        }
                    }
                }
                // All queues empty — yield before spinning again
                if inner.shutdown.load(Ordering::Relaxed) {
                    return;
                }
                task = None;
                break;
            }

            match task {
                Some(f) => f(),
                None => {
                    if inner.shutdown.load(Ordering::Relaxed) {
                        return;
                    }
                    thread::yield_now();
                }
            }
        }
    }

    fn enqueue(&self, task: Box<dyn FnOnce() + Send>) {
        let qidx =
            self.inner.next_q.fetch_add(1, Ordering::Relaxed) % self.inner.num_q;
        {
            let mut lock = self.inner.queues[qidx].lock().unwrap();
            lock.push_back(task);
        }
        self.inner.cvs[qidx].notify_one();
    }
}

impl TxExecutor for QueueExecutor {
    fn execute(&self, body: Box<dyn FnOnce() + Send>) {
        let done = Arc::new(AtomicBool::new(false));
        let done_c = Arc::clone(&done);

        self.enqueue(Box::new(move || {
            body();
            done_c.store(true, Ordering::Release);
        }));

        // Spin-wait with CPU relax
        while !done.load(Ordering::Acquire) {
            cpu_relax();
        }
    }

    fn execute_async(&self, body: Box<dyn FnOnce() + Send>) -> JoinHandle<()> {
        let (tx, rx) = std::sync::mpsc::channel::<()>();
        self.enqueue(Box::new(move || {
            body();
            let _ = tx.send(());
        }));
        thread::spawn(move || {
            let _ = rx.recv();
        })
    }
}

impl Drop for QueueExecutor {
    fn drop(&mut self) {
        self.inner.shutdown.store(true, Ordering::Release);
        for cv in &self.inner.cvs {
            cv.notify_all();
        }
        for w in self.workers.drain(..) {
            let _ = w.join();
        }
    }
}

// ── CPU relax ──────────────────────────────────────────────────────

#[inline]
fn cpu_relax() {
    std::hint::spin_loop();
}

// ── Convenience: tx_execute ────────────────────────────────────────

/// Execute a transaction body through the given executor.
///
/// The body receives a `&Transaction` reference (same as `tm::transaction()`)
/// and the executor handles where/how the retry loop runs.
///
/// `F` must be `Fn` (not `FnOnce`) because the retry loop may call it multiple
/// times on abort.  This matches `tm::transaction()`'s signature.
pub fn tx_execute<F>(exec: &dyn TxExecutor, f: F)
where
    F: Fn(&tm::Transaction) + Send + 'static,
{
    exec.execute(Box::new(move || {
        tm::transaction(&f);
    }));
}

// ── Tests ──────────────────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn inline_execute_does_not_crash() {
        let exec = InlineExecutor;
        exec.execute(Box::new(|| {}));
    }

    #[test]
    fn queue_execute_sync_works() {
        let exec = QueueExecutor::new(2, 2);
        let flag = Arc::new(AtomicBool::new(false));
        let flag_c = Arc::clone(&flag);
        exec.execute(Box::new(move || {
            flag_c.store(true, Ordering::Release);
        }));
        assert!(flag.load(Ordering::Acquire));
        // Drop joins workers
    }

    #[test]
    fn queue_execute_async_works() {
        let exec = QueueExecutor::new(2, 2);
        let flag = Arc::new(AtomicBool::new(false));
        let flag_c = Arc::clone(&flag);
        let h = exec.execute_async(Box::new(move || {
            flag_c.store(true, Ordering::Release);
        }));
        h.join().unwrap();
        assert!(flag.load(Ordering::Acquire));
    }

    #[test]
    fn tx_execute_compiles() {
        use tm::TmCell;
        let cell = TmCell::new(42u64);
        let exec = InlineExecutor;
        tx_execute(&exec, move |tx| {
            let v = tx.read(&cell);
            tx.write(&cell, v + 1);
        });
    }
}
