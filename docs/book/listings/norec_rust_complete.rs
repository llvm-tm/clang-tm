// A complete, compilable NOrec-style STM in safe Rust (std only).
// Book listing source: verified with rustc 1.96 before inclusion.
use std::collections::HashMap;
use std::marker::PhantomData;
use std::sync::atomic::{AtomicU64, Ordering as O};
use std::sync::{Arc, atomic::AtomicBool};
use std::thread;

// ---------- 1. Word-encodable cell contents ----------

pub trait Word: Copy {
    fn pack(self) -> u64;
    fn unpack(raw: u64) -> Self;
}
impl Word for u64 {
    fn pack(self) -> u64 { self }
    fn unpack(raw: u64) -> u64 { raw }
}
impl Word for i64 {
    fn pack(self) -> u64 { self as u64 }
    fn unpack(raw: u64) -> i64 { raw as i64 }
}

// ---------- 2. Shared region: one clock + a word-addressed heap ----------

pub struct Region {
    clock: AtomicU64,      // even = version v; odd = version v, commit lock held
    mem: Vec<AtomicU64>,   // the TM heap: one machine word per cell
}

#[derive(Clone, Copy)]
pub struct TmCell<T> {
    slot: usize,
    _pd: PhantomData<T>,
}

/// Zero-sized signal: "this transaction attempt made no sense; retry".
pub struct Abort;

/// Per-thread transaction state. Created fresh by `transaction`.
pub struct Tx<'r> {
    region: &'r Region,
    rv: u64,                    // snapshot version taken at begin
    ws: HashMap<usize, u64>,    // write-set: slot -> buffered value
    rs: Vec<(usize, u64)>,      // read-set: slot -> value observed
}

impl Region {
    pub fn new() -> Self {
        Region { clock: AtomicU64::new(0), mem: Vec::new() }
    }

    /// Allocate a cell. Call only during single-threaded setup:
    /// the heap does not grow once threads share the region.
    pub fn alloc<T: Word>(&mut self, init: T) -> TmCell<T> {
        self.mem.push(AtomicU64::new(init.pack()));
        TmCell { slot: self.mem.len() - 1, _pd: PhantomData }
    }

    /// Run `body` as an atomic action, retrying until it commits.
    pub fn transaction<R>(
        &self,
        body: impl Fn(&mut Tx<'_>) -> Result<R, Abort>,
    ) -> R {
        let mut tx = Tx { region: self, rv: 0, ws: HashMap::new(), rs: Vec::new() };
        loop {
            tx.begin();                       // sample clock, clear logs
            match body(&mut tx) {
                Ok(value) => match tx.commit() {
                    Ok(()) => return value,   // committed
                    Err(Abort) => {}          // lost the race: retry
                },
                Err(Abort) => {}              // invalid snapshot: retry
            }
        }
    }
}

// ---------- 3. Begin and the read path ----------

impl<'r> Tx<'r> {
    fn begin(&mut self) {
        self.ws.clear();
        self.rs.clear();
        // Sample the clock; wait out any in-flight commit (bit0 set).
        self.rv = loop {
            let s = self.region.clock.load(O::Acquire);
            if s & 1 == 0 { break s; }
        };
    }

    /// Transactional read: write-set first, then capture-read-recheck
    /// against the *begin* version. Every successful read re-validates
    /// the entire snapshot implicitly: any commit since begin would have
    /// moved the clock away from `rv`.
    pub fn read<T: Word>(&mut self, cell: &TmCell<T>) -> Result<T, Abort> {
        let slot = cell.slot;
        if let Some(&v) = self.ws.get(&slot) {
            return Ok(T::unpack(v));          // read own buffered write
        }
        let s1 = self.region.clock.load(O::Acquire);   // capture
        if s1 != self.rv {
            return Err(Abort);                // snapshot is stale already
        }
        let raw = self.region.mem[slot].load(O::Relaxed); // read data
        let s2 = self.region.clock.load(O::Acquire);   // re-check
        if s1 != s2 {
            return Err(Abort);                // a commit raced the read
        }
        self.rs.push((slot, raw));
        Ok(T::unpack(raw))
    }

    /// Transactional write: buffer only; visible at commit time.
    pub fn write<T: Word>(&mut self, cell: &TmCell<T>, value: T) {
        self.ws.insert(cell.slot, value.pack());
    }

    // ---------- 4. Commit protocol ----------

    fn commit(&mut self) -> Result<(), Abort> {
        if self.ws.is_empty() {
            return Ok(());  // read-only: reads validated continuously
        }
        let rv = self.rv;
        // Acquire the single commit lock by setting bit0 of the clock.
        if self.region.clock
            .compare_exchange(rv, rv | 1, O::Acquire, O::Relaxed)
            .is_err()
        {
            return Err(Abort);                // another writer went first
        }
        // Lock held: no other writer can touch memory now, so value
        // comparisons against our recorded reads are meaningful.
        for &(slot, observed) in &self.rs {
            if self.region.mem[slot].load(O::Relaxed) != observed {
                self.region.clock.store(rv, O::Release); // unlock, same version
                return Err(Abort);
            }
        }
        // Publish: write back, then bump the clock (unlock + new version).
        // The Release store makes every buffered write visible to every
        // thread whose Acquire load observes the new version.
        for (&slot, &v) in &self.ws {
            self.region.mem[slot].store(v, O::Relaxed);
        }
        self.region.clock.store(rv + 2, O::Release);
        Ok(())
    }
}

// ---------- 5. The running example: bank transfer ----------

const THREADS: usize = 8;
const ITERS: usize = 25_000;
const START: i64 = 1_000_000;

fn main() {
    let mut region = Region::new();
    let a = region.alloc(START);
    let b = region.alloc(START);
    let region = Arc::new(region);

    let done = Arc::new(AtomicBool::new(false));

    // Mover threads: transfer 10 between A and B, honouring a floor of 10.
    let mut handles = Vec::new();
    for t in 0..THREADS {
        let region = Arc::clone(&region);
        let done = Arc::clone(&done);
        handles.push(thread::spawn(move || {
            let (src, dst) = if t % 2 == 0 { (a, b) } else { (b, a) };
            let (mut moved, mut rejected) = (0u64, 0u64);
            while !done.load(O::Relaxed) && moved < ITERS as u64 {
                let ok = region.transaction(|tx| {
                    let s = tx.read(&src)?;
                    let d = tx.read(&dst)?;
                    if s < 10 { return Ok(false); }   // floor: a real outcome
                    tx.write(&src, s - 10);
                    tx.write(&dst, d + 10);
                    Ok(true)
                });
                if ok { moved += 1 } else { rejected += 1 }
            }
            (moved, rejected)
        }));
    }

    // Checker thread: every committed snapshot must conserve money.
    let region2 = Arc::clone(&region);
    let done2 = Arc::clone(&done);
    let checker = thread::spawn(move || {
        let mut checks = 0u64;
        while !done2.load(O::Relaxed) {
            let total = region2.transaction(|tx| {
                Ok(tx.read(&a)? + tx.read(&b)?)
            });
            assert_eq!(total, 2 * START, "invariant violated");
            checks += 1;
        }
        checks
    });

    for h in handles { h.join().unwrap(); }
    done.store(true, O::Relaxed);
    let checks = checker.join().unwrap();

    let total = region.transaction(|tx| Ok(tx.read(&a)? + tx.read(&b)?));
    println!("final total = {}", total);
    println!("snapshot checks passed: {}", checks);
    assert_eq!(total, 2 * START);
}
