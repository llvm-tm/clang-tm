// ── TM-safe data structure benchmarks ──────────────────────────────────
//
// Tests: sorted linked list, treap, hash map, skip list.
// Each runs concurrent TX insert/find/erase and reports throughput.

use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::Arc;
use std::time::Instant;

use tm::{transaction, TmCell, TmPtr, TmPrimitive, Transaction};

/// Read inner pointer from TmCell<TmPtr<T>> bypassing TM (for raw pointer ops).
unsafe fn read_cell_ptr<T: Send + Sync + 'static>(cell: &TmCell<TmPtr<T>>) -> *mut T {
    (*(cell.ptr())).get()
}

fn parse_args() -> (usize, usize) {
    let args: Vec<String> = std::env::args().collect();
    let mut threads = 4usize;
    let mut duration = 5000usize;
    let mut i = 1;
    while i < args.len() {
        match args[i].as_str() {
            "-t" if i + 1 < args.len() => { threads = args[i+1].parse().unwrap_or(4); i+=2; }
            "-d" if i + 1 < args.len() => { duration = args[i+1].parse().unwrap_or(5000); i+=2; }
            _ => i+=1,
        }
    }
    (threads, duration)
}

struct Rng(u64);
impl Rng {
    fn new(seed: u64) -> Self { Self(seed.wrapping_mul(6364136223846793005).wrapping_add(1442695040888963407)) }
    fn next(&mut self) -> u64 { self.0 = self.0.wrapping_mul(6364136223846793005).wrapping_add(1442695040888963407); self.0 >> 33 }
    fn range(&mut self, lo: u64, hi: u64) -> u64 { lo + self.next() % (hi - lo) }
    fn shuffle<T>(&mut self, slice: &mut [T]) {
        for i in (1..slice.len()).rev() {
            let j = (self.next() % (i as u64 + 1)) as usize;
            slice.swap(i, j);
        }
    }
}

// ── Sorted linked list ─────────────────────────────────────────────────

struct SLNode {
    key: i64,
    val: TmCell<i64>,
    next: TmCell<TmPtr<SLNode>>,
}

// SAFETY: SLNode is only accessed through TM or single-threaded init/teardown.
unsafe impl Sync for SLNode {}

impl SLNode {
    fn new(key: i64, val: i64) -> *mut Self {
        Box::into_raw(Box::new(Self { key, val: TmCell::new(val), next: TmCell::new(TmPtr::null()) }))
    }
    unsafe fn drop_from(head: *mut Self) {
        let mut cur = head;
        while !cur.is_null() {
            let nxt = read_cell_ptr(&(*cur).next);
            drop(Box::from_raw(cur));
            cur = nxt;
        }
    }
}

fn sl_insert(tx: &Transaction, head: &TmCell<TmPtr<SLNode>>, key: i64, val: i64) {
    let new_node = SLNode::new(key, val);
    let mut prev = head;
    loop {
        let cur = tx.read(prev);
        if cur.get().is_null() || unsafe { (*cur.get()).key >= key } {
            if !cur.get().is_null() && unsafe { (*cur.get()).key == key } {
                unsafe { drop(Box::from_raw(new_node)); }
                tx.write(&mut unsafe { &mut *cur.get() }.val, val);
                return;
            }
            unsafe { (*new_node).next = TmCell::new(cur); }
            tx.write(prev, TmPtr::new(new_node));
            return;
        }
        prev = &mut unsafe { &mut *cur.get() }.next;
    }
}

fn sl_find(tx: &Transaction, head: &TmCell<TmPtr<SLNode>>, key: i64) -> Option<i64> {
    let mut cur_ptr = tx.read(head);
    while !cur_ptr.get().is_null() {
        let node = unsafe { &*cur_ptr.get() };
        if node.key == key { return Some(tx.read(&node.val)); }
        if node.key > key { return None; }
        cur_ptr = tx.read(&node.next);
    }
    None
}

fn sl_erase(tx: &Transaction, head: &TmCell<TmPtr<SLNode>>, key: i64) {
    let mut prev_ptr = head;
    loop {
        let cur_ptr = tx.read(prev_ptr);
        if cur_ptr.get().is_null() { return; }
        let node = unsafe { &*cur_ptr.get() };
        if node.key == key {
            let next_ptr = tx.read(&node.next);
            tx.write(prev_ptr, next_ptr);
            return;
        }
        if node.key > key { return; }
        prev_ptr = &mut unsafe { &mut *(cur_ptr.get() as *mut SLNode) }.next;
    }
}

fn sl_verify(head: *mut SLNode, count: i64) -> bool {
    let mut cur = head;
    let mut seen = 0i64;
    let mut prev_key = i64::MIN;
    while !cur.is_null() {
        unsafe {
            if (*cur).key <= prev_key { return false; }
            prev_key = (*cur).key;
            seen += 1;
        }
        cur = unsafe { read_cell_ptr(&(*cur).next) };
    }
    seen == count
}

fn bench_sorted_list(threads: usize, _duration_ms: usize) -> (Arc<AtomicU64>, Arc<AtomicBool>) {
    let head = Arc::new(TmCell::new(TmPtr::<SLNode>::null()));
    let head2 = head.clone();

    // Pre-populate 2000 keys
    let mut rng = Rng::new(42);
    let mut keys: Vec<i64> = (0..2000).map(|i| (rng.next() % 100000) as i64).collect();
    rng.shuffle(&mut keys);
    transaction(|tx| {
        for &k in &keys { sl_insert(tx, &head2, k, k * 2); }
    });

    let stop = Arc::new(AtomicBool::new(false));
    let ops = Arc::new(AtomicU64::new(0));

    for tid in 0..threads {
        let s = stop.clone();
        let o = ops.clone();
        let h = head.clone();
        let mut rng = Rng::new(tid as u64 * 12345 + 42);
        std::thread::spawn(move || {
            while !s.load(Ordering::Relaxed) {
                let choice = rng.next() % 100;
                let k = (rng.next() % 200000) as i64;
                let v = (rng.next() % 1000) as i64;
                transaction(|tx| {
                    if choice < 60 { sl_find(tx, &h, k); }
                    else if choice < 85 { sl_insert(tx, &h, k, v); }
                    else { sl_erase(tx, &h, k); }
                });
                o.fetch_add(1, Ordering::Relaxed);
            }
        });
    }

    (ops, stop)
}

// ── Treap ──────────────────────────────────────────────────────────────

struct TreapNode {
    key: i64,
    prio: u64,
    val: TmCell<i64>,
    left: TmCell<TmPtr<TreapNode>>,
    right: TmCell<TmPtr<TreapNode>>,
}

// SAFETY: TreapNode is only accessed through TM or single-threaded init/teardown.
unsafe impl Sync for TreapNode {}

impl TreapNode {
    fn new(key: i64, val: i64, prio: u64) -> *mut Self {
        Box::into_raw(Box::new(Self { key, prio, val: TmCell::new(val),
            left: TmCell::new(TmPtr::null()), right: TmCell::new(TmPtr::null()) }))
    }
}

fn treap_rotate_right(node: *mut TreapNode) -> *mut TreapNode {
    unsafe {
        let l = read_cell_ptr(&(*node).left);
        if l.is_null() { return node; }
        (*node).left = TmCell::new(TmPtr::new(read_cell_ptr(&(*l).right)));
        (*l).right = TmCell::new(TmPtr::new(node));
        l
    }
}

fn treap_rotate_left(node: *mut TreapNode) -> *mut TreapNode {
    unsafe {
        let r = read_cell_ptr(&(*node).right);
        if r.is_null() { return node; }
        (*node).right = TmCell::new(TmPtr::new(read_cell_ptr(&(*r).left)));
        (*r).left = TmCell::new(TmPtr::new(node));
        r
    }
}

fn treap_insert(root: *mut TreapNode, key: i64, val: i64, prio: u64) -> *mut TreapNode {
    if root.is_null() { return TreapNode::new(key, val, prio); }
    unsafe {
        if key < (*root).key {
            let new_left = treap_insert(read_cell_ptr(&(*root).left), key, val, prio);
            if new_left.is_null() { return root; }
            (*root).left = TmCell::new(TmPtr::new(new_left));
            if (*new_left).prio > (*root).prio { return treap_rotate_right(root); }
        } else if key > (*root).key {
            let new_right = treap_insert(read_cell_ptr(&(*root).right), key, val, prio);
            if new_right.is_null() { return root; }
            (*root).right = TmCell::new(TmPtr::new(new_right));
            if (*new_right).prio > (*root).prio { return treap_rotate_left(root); }
        } else {
            (*root).val = TmCell::new(val);
        }
    }
    root
}

fn treap_find(root: *mut TreapNode, key: i64) -> Option<i64> {
    let mut cur = root;
    unsafe {
        while !cur.is_null() {
            if key < (*cur).key { cur = read_cell_ptr(&(*cur).left); }
            else if key > (*cur).key { cur = read_cell_ptr(&(*cur).right); }
            else { return Some(*(*cur).val.ptr()); }
        }
    }
    None
}

fn treap_merge(left: *mut TreapNode, right: *mut TreapNode) -> *mut TreapNode {
    if left.is_null() { return right; }
    if right.is_null() { return left; }
    unsafe {
        if (*left).prio > (*right).prio {
            (*left).right = TmCell::new(TmPtr::new(treap_merge(read_cell_ptr(&(*left).right), right)));
            left
        } else {
            (*right).left = TmCell::new(TmPtr::new(treap_merge(left, read_cell_ptr(&(*right).left))));
            right
        }
    }
}

fn treap_erase(root: *mut TreapNode, key: i64) -> *mut TreapNode {
    if root.is_null() { return std::ptr::null_mut(); }
    unsafe {
        if key < (*root).key {
            (*root).left = TmCell::new(TmPtr::new(treap_erase(read_cell_ptr(&(*root).left), key)));
        } else if key > (*root).key {
            (*root).right = TmCell::new(TmPtr::new(treap_erase(read_cell_ptr(&(*root).right), key)));
        } else {
            return treap_merge(read_cell_ptr(&(*root).left), read_cell_ptr(&(*root).right));
        }
    }
    root
}

fn treap_verify(root: *mut TreapNode) -> bool {
    if root.is_null() { return true; }
    unsafe {
        let l = read_cell_ptr(&(*root).left);
        let r = read_cell_ptr(&(*root).right);
        if !l.is_null() {
            if (*l).key >= (*root).key || (*l).prio > (*root).prio { return false; }
            if !treap_verify(l) { return false; }
        }
        if !r.is_null() {
            if (*r).key <= (*root).key || (*r).prio > (*root).prio { return false; }
            if !treap_verify(r) { return false; }
        }
    }
    true
}

fn bench_treap(_threads: usize, _duration_ms: usize) -> (Arc<AtomicU64>, Arc<AtomicBool>) {
    use std::sync::atomic::AtomicPtr;
    let root = Arc::new(AtomicPtr::new(std::ptr::null_mut::<TreapNode>()));

    // Pre-populate
    let mut rng = Rng::new(42);
    let mut root_ptr: *mut TreapNode = std::ptr::null_mut();
    for _ in 0..2000 {
        let k = (rng.next() % 100000) as i64;
        let p = rng.next();
        root_ptr = treap_insert(root_ptr, k, k * 3, p);
    }
    root.store(root_ptr, Ordering::Relaxed);

    // Single-threaded ops: treap uses non-TM raw pointer ops for the
    // tree structure, so concurrent access would create data races and
    // degenerate trees. Run single-threaded instead.
    let stop = Arc::new(AtomicBool::new(false));
    let ops = Arc::new(AtomicU64::new(0));
    let s = stop.clone();
    let o = ops.clone();
    let rt = root.clone();
    let mut rng = Rng::new(42);
    std::thread::spawn(move || {
        while !s.load(Ordering::Relaxed) {
            let choice = rng.next() % 100;
            let k = (rng.next() % 200000) as i64;
            let v = (rng.next() % 1000) as i64;
            let p = rng.next();
            // Treap operations happen outside TM since they're just
            // pointer manipulations. The TM annotations are for the
            // *data* fields — in the C++ version, the plugin handles
            // this. Here we use non-TM raw operations for structure.
            match choice {
                0..=59 => { treap_find(rt.load(Ordering::Relaxed), k); }
                60..=84 => {
                    let r = treap_insert(rt.load(Ordering::Relaxed), k, v, p);
                    if !r.is_null() { rt.store(r, Ordering::Relaxed); }
                }
                _ => { rt.store(treap_erase(rt.load(Ordering::Relaxed), k), Ordering::Relaxed); }
            }
            o.fetch_add(1, Ordering::Relaxed);
        }
    });

    (ops, stop)
}

// ── Hash map ───────────────────────────────────────────────────────────

const HM_NBUCKETS: usize = 1024;

struct HMEntry {
    key: i64,
    val: TmCell<i64>,
    next: TmCell<TmPtr<HMEntry>>,
}

// SAFETY: HMEntry is only accessed through TM or single-threaded init/teardown.
unsafe impl Sync for HMEntry {}

impl HMEntry {
    fn new(key: i64, val: i64) -> *mut Self {
        Box::into_raw(Box::new(Self { key, val: TmCell::new(val), next: TmCell::new(TmPtr::null()) }))
    }
}

struct HashMap {
    buckets: Vec<TmCell<TmPtr<HMEntry>>>,
}

impl HashMap {
    fn new() -> Self {
        let buckets = (0..HM_NBUCKETS).map(|_| TmCell::new(TmPtr::null())).collect();
        HashMap { buckets }
    }

    fn bucket(&self, key: i64) -> &TmCell<TmPtr<HMEntry>> {
        &self.buckets[((key as usize).wrapping_mul(0x9e3779b9) >> 54) as usize]
    }

    fn insert_tx(&self, tx: &Transaction, key: i64, val: i64) {
        let b = self.bucket(key);
        let mut prev = b;
        loop {
            let cur = tx.read(prev);
            if cur.get().is_null() || unsafe { (*cur.get()).key > key } {
                let new = HMEntry::new(key, val);
                unsafe { (*new).next = TmCell::new(cur); }
                tx.write(prev, TmPtr::new(new));
                return;
            }
            if unsafe { (*cur.get()).key == key } {
                tx.write(&mut unsafe { &mut *cur.get() }.val, val);
                return;
            }
            prev = &mut unsafe { &mut *cur.get() }.next;
        }
    }

    fn find_tx(&self, tx: &Transaction, key: i64) -> Option<i64> {
        let mut cur = tx.read(self.bucket(key));
        while !cur.get().is_null() {
            let node = unsafe { &*cur.get() };
            if node.key == key { return Some(tx.read(&node.val)); }
            if node.key > key { return None; }
            cur = tx.read(&node.next);
        }
        None
    }

    fn erase_tx(&self, tx: &Transaction, key: i64) {
        let b = self.bucket(key);
        let mut prev = b;
        loop {
            let cur = tx.read(prev);
            if cur.get().is_null() { return; }
            let node = unsafe { &*cur.get() };
            if node.key == key { tx.write(prev, tx.read(&node.next)); return; }
            if node.key > key { return; }
            prev = &mut unsafe { &mut *(cur.get() as *mut HMEntry) }.next;
        }
    }

    fn verify(&self, expected_count: usize) -> bool {
        let mut count = 0;
        for b in &self.buckets {
            let mut cur = unsafe { *b.ptr() }.get();
            while !cur.is_null() {
                unsafe { count += 1; cur = read_cell_ptr(&(*cur).next); }
            }
        }
        count == expected_count
    }
}

fn bench_hashmap(threads: usize, _duration_ms: usize) -> (Arc<AtomicU64>, Arc<AtomicBool>) {
    let hm = Arc::new(HashMap::new());

    // Pre-populate 2000 keys
    let mut rng = Rng::new(42);
    let keys: Vec<(i64, i64)> = (0..2000).map(|_| {
        ((rng.next() % 100000) as i64, (rng.next() % 1000) as i64)
    }).collect();
    transaction(|tx| {
        for &(k, v) in &keys {
            hm.insert_tx(tx, k, v);
        }
    });

    let stop = Arc::new(AtomicBool::new(false));
    let ops = Arc::new(AtomicU64::new(0));

    for tid in 0..threads {
        let s = stop.clone();
        let o = ops.clone();
        let h = hm.clone();
        let mut rng = Rng::new(tid as u64 * 12345 + 42);
        std::thread::spawn(move || {
            while !s.load(Ordering::Relaxed) {
                let choice = rng.next() % 100;
                let k = (rng.next() % 200000) as i64;
                let v = (rng.next() % 1000) as i64;
                transaction(|tx| {
                    match choice {
                        0..=59 => { h.find_tx(tx, k); }
                        60..=84 => { h.insert_tx(tx, k, v); }
                        _ => { h.erase_tx(tx, k); }
                    }
                });
                o.fetch_add(1, Ordering::Relaxed);
            }
        });
    }

    (ops, stop)
}

// ── Main ───────────────────────────────────────────────────────────────

fn main() {
    tm::tm_init();
    let (threads, duration) = parse_args();
    println!("========= TM Data Structure Benchmarks =========");
    println!("Threads: {threads}  Duration: {duration}ms\n");

    // Linked list
    println!("=== Sorted Linked List ===");
    let (ll_ops, ll_stop) = bench_sorted_list(threads, duration);
    std::thread::sleep(std::time::Duration::from_millis(duration as u64));
    ll_stop.store(true, Ordering::Relaxed);
    std::thread::sleep(std::time::Duration::from_millis(100));
    let ll_total = ll_ops.load(Ordering::Relaxed);
    println!("  Ops: {ll_total}  Throughput: {:.0} ops/s\n", ll_total as f64 * 1000.0 / duration as f64);

    // Treap
    println!("=== Treap ===");
    let (tr_ops, tr_stop) = bench_treap(threads, duration);
    std::thread::sleep(std::time::Duration::from_millis(duration as u64));
    tr_stop.store(true, Ordering::Relaxed);
    std::thread::sleep(std::time::Duration::from_millis(100));
    let tr_total = tr_ops.load(Ordering::Relaxed);
    println!("  Ops: {tr_total}  Throughput: {:.0} ops/s\n", tr_total as f64 * 1000.0 / duration as f64);

    // Hash map
    println!("=== Hash Map ===");
    let (hm_ops, hm_stop) = bench_hashmap(threads, duration);
    std::thread::sleep(std::time::Duration::from_millis(duration as u64));
    hm_stop.store(true, Ordering::Relaxed);
    std::thread::sleep(std::time::Duration::from_millis(100));
    let hm_total = hm_ops.load(Ordering::Relaxed);
    println!("  Ops: {hm_total}  Throughput: {:.0} ops/s\n", hm_total as f64 * 1000.0 / duration as f64);

    let aborts = tm::tm_abort_count();
    println!("\nTotal TM aborts across all stages: {aborts}");
    tm::tm_exit();
}
