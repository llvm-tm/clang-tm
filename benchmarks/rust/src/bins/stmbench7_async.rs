//! STMbench7 — Async (queue-manual) dispatch in Rust.
//!
//! Uses a channel-based work queue instead of the LLVM plugin.
//! Workers call tm_init_thread() on startup, then loop dequeueing tasks.
//! Main thread enqueues batches and waits for each batch to complete
//! via an atomic completion counter (analogous to tm_wait_prev_tx()).
//!
//! Run:
//!   cargo run --release --bin stmbench7_async -- -t 4 -d 5000
//!
//! Backend selection (default: wbctl):
//!   cargo run --release --no-default-features --features tl2 --bin stmbench7_async -- -t 4 -d 5000

use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::mpsc::Sender;
use std::sync::Arc;
use std::thread::{self, JoinHandle};
use tm::{TmCell, Transaction, transaction, tm_init, tm_exit, tm_init_thread, tm_exit_thread};

// ══════════════════════════════════════════════════════════════════════
// Constants, data structures, operations — copied from stmbench7.rs
// ══════════════════════════════════════════════════════════════════════

const FANOUT: usize = 3;
const TREE_LEVELS: usize = 6;
const MAX_CP: usize = 500;
const AP_PER_CP: usize = 200;
const CONN_PER_AP: usize = 3;
const MAX_BA: usize = 729;
const MAX_CA: usize = 364;
const MAX_AP: usize = MAX_CP * AP_PER_CP;
const MAX_CONN: usize = MAX_AP * CONN_PER_AP;
const MAX_DOCS: usize = MAX_CP;
const MAX_CP_BA_BAG: usize = 5;

struct Document { id: i32, doc_type: TmCell<i32>, build_date: TmCell<i32>, composite_part_id: i32 }
struct Connection { id: i32, from_atomic_part_id: i32, to_atomic_part_id: i32, conn_type: TmCell<i32>, valid: TmCell<i32> }
struct AtomicPart { id: i32, x: TmCell<i32>, y: TmCell<i32>, z: TmCell<i32>, build_date: TmCell<i32>, weight: TmCell<i32>, composite_part_id: i32, connection_ids: Vec<usize>, valid: TmCell<i32> }
struct CompositePart { id: i32, build_date: TmCell<i32>, document_id: i32, root_atomic_part_id: i32, atomic_part_ids: Vec<usize>, base_assembly_ids: Vec<usize>, valid: TmCell<i32>, ap_count: TmCell<i32> }
struct BaseAssembly { id: i32, parent_assembly_id: i32, build_date: TmCell<i32>, composite_part_ids: Vec<usize>, valid: TmCell<i32> }
struct ComplexAssembly { id: i32, level: i32, parent_id: i32, child_assembly_ids: Vec<usize>, child_base_assembly_ids: Vec<usize>, build_date: TmCell<i32> }
struct Module { id: i32, root_assembly_id: i32 }

struct Xsrng(u64);
impl Xsrng {
    fn new(seed: u64) -> Self { Self(seed.wrapping_mul(6364136223846793005).wrapping_add(1442695040888963407)) }
    fn next(&mut self) -> u64 { self.0 = self.0.wrapping_mul(6364136223846793005).wrapping_add(1442695040888963407); self.0 >> 33 }
    fn range(&mut self, lo: usize, hi: usize) -> usize { lo + (self.next() as usize) % (hi - lo) }
    fn range_i32(&mut self, lo: i32, hi: i32) -> i32 { lo + (self.next() as i32) % (hi - lo) }
}

struct Database {
    modules: Vec<Module>,
    complex_assemblies: Vec<ComplexAssembly>,
    base_assemblies: Vec<BaseAssembly>,
    composite_parts: Vec<CompositePart>,
    atomic_parts: Vec<AtomicPart>,
    connections: Vec<Connection>,
    documents: Vec<Document>,
    cp_by_date: Vec<(i32, usize)>,
    next_cp_slot: AtomicUsize,
    next_ap_slot: AtomicUsize,
    next_conn_slot: AtomicUsize,
    next_ba_slot: AtomicUsize,
}

impl Database {
    fn new() -> Self {
        let _rng = Xsrng::new(42);
        let mut modules = Vec::new();
        modules.push(Module { id: 0, root_assembly_id: 0 });
        let mut complex_assemblies = Vec::with_capacity(MAX_CA);
        let mut base_assemblies = Vec::with_capacity(MAX_BA * 2);
        let mut composite_parts = Vec::with_capacity(MAX_CP * 2);
        let mut atomic_parts = Vec::with_capacity(MAX_AP * 2);
        let mut connections = Vec::with_capacity(MAX_CONN * 2);
        let mut documents = Vec::with_capacity(MAX_DOCS + MAX_CP);

        let mut level_sizes = [0usize; TREE_LEVELS];
        let mut level_offsets = [0usize; TREE_LEVELS];
        let mut off = 0usize;
        let mut sz = 1usize;
        for l in 0..TREE_LEVELS {
            level_sizes[l] = sz;
            level_offsets[l] = off;
            for j in 0..sz {
                complex_assemblies.push(ComplexAssembly {
                    id: (off + j) as i32, level: l as i32, parent_id: -1,
                    child_assembly_ids: Vec::with_capacity(FANOUT),
                    child_base_assembly_ids: Vec::with_capacity(FANOUT),
                    build_date: TmCell::new(1000 + (l as i32 * 100 + j as i32) % 365),
                });
            }
            off += sz; sz *= FANOUT;
        }
        for l in 1..TREE_LEVELS {
            let parent_off = level_offsets[l - 1];
            let child_off = level_offsets[l];
            for p in 0..level_sizes[l - 1] {
                let parent_idx = parent_off + p;
                for c in 0..FANOUT {
                    let child_idx = child_off + p * FANOUT + c;
                    complex_assemblies[parent_idx].child_assembly_ids.push(child_idx);
                    complex_assemblies[child_idx].parent_id = parent_idx as i32;
                }
            }
        }
        let ba_parent_off = level_offsets[TREE_LEVELS - 1];
        let mut ba_id = 0i32;
        for p in 0..level_sizes[TREE_LEVELS - 1] {
            let parent_idx = ba_parent_off + p;
            for _c in 0..FANOUT {
                base_assemblies.push(BaseAssembly {
                    id: ba_id, parent_assembly_id: parent_idx as i32,
                    build_date: TmCell::new(1000 + (ba_id as usize % 365) as i32),
                    composite_part_ids: Vec::new(), valid: TmCell::new(1),
                });
                complex_assemblies[parent_idx].child_base_assembly_ids.push(ba_id as usize);
                ba_id += 1;
            }
        }
        for cp_idx in 0..MAX_CP {
            let cd = 1000 + (cp_idx % 365) as i32;
            composite_parts.push(CompositePart {
                id: cp_idx as i32, build_date: TmCell::new(cd), document_id: cp_idx as i32,
                root_atomic_part_id: -1, atomic_part_ids: Vec::with_capacity(AP_PER_CP * 2),
                base_assembly_ids: Vec::with_capacity(MAX_CP_BA_BAG * 2), valid: TmCell::new(1),
                ap_count: TmCell::new(0),
            });
            documents.push(Document { id: cp_idx as i32, doc_type: TmCell::new((cp_idx % 3) as i32), build_date: TmCell::new(cd), composite_part_id: cp_idx as i32 });
        }
        {   let mut bag_rng = Xsrng::new(42);
            for ci in 0..MAX_CP {
                let num = 1 + bag_rng.range(0, MAX_CP_BA_BAG - 1);
                for _ in 0..num { let bi = bag_rng.range(0, MAX_BA); composite_parts[ci].base_assembly_ids.push(bi); base_assemblies[bi].composite_part_ids.push(ci); }
            }
        }
        let mut raw_idx: Vec<(i32, usize)> = composite_parts.iter().enumerate().map(|(i, cp)| (unsafe { *cp.build_date.ptr() }, i)).collect();
        raw_idx.sort_by(|a, b| a.0.cmp(&b.0));
        let cp_by_date = raw_idx;

        {   let mut ap_rng = Xsrng::new(99);
            for ci in 0..MAX_CP {
                let first = ci * AP_PER_CP;
                composite_parts[ci].root_atomic_part_id = first as i32;
                for j in 0..AP_PER_CP {
                    let ap_id = first + j;
                    atomic_parts.push(AtomicPart {
                        id: ap_id as i32, x: TmCell::new((j % 100) as i32), y: TmCell::new(((j / 100) % 100) as i32),
                        z: TmCell::new((j / 10000) as i32), build_date: TmCell::new((1000 + (ci * AP_PER_CP + j) % 365) as i32),
                        weight: TmCell::new(((j % 50) + 1) as i32), composite_part_id: ci as i32,
                        connection_ids: Vec::with_capacity(CONN_PER_AP * 4), valid: TmCell::new(1),
                    });
                    composite_parts[ci].atomic_part_ids.push(first + j);
                }
                for j in 0..AP_PER_CP {
                    let a = first + j;
                    let t1 = first + (j + 1) % AP_PER_CP;
                    let c_id1 = connections.len();
                    connections.push(Connection { id: c_id1 as i32, from_atomic_part_id: a as i32, to_atomic_part_id: t1 as i32, conn_type: TmCell::new((j % 3) as i32), valid: TmCell::new(1) });
                    atomic_parts[a].connection_ids.push(c_id1);
                    let t2 = first + (j + 2) % AP_PER_CP;
                    let c_id2 = connections.len();
                    connections.push(Connection { id: c_id2 as i32, from_atomic_part_id: a as i32, to_atomic_part_id: t2 as i32, conn_type: TmCell::new(((j + 1) % 3) as i32), valid: TmCell::new(1) });
                    atomic_parts[a].connection_ids.push(c_id2);
                    for _k in 0..(CONN_PER_AP - 2) {
                        let mut t3 = first + ap_rng.range(0, AP_PER_CP);
                        if t3 == a { t3 = first + (j + 3) % AP_PER_CP; }
                        connections.push(Connection { id: connections.len() as i32, from_atomic_part_id: a as i32, to_atomic_part_id: t3 as i32, conn_type: TmCell::new(((j + _k) % 3) as i32), valid: TmCell::new(1) });
                        atomic_parts[a].connection_ids.push(connections.len() - 1);
                    }
                }
            }
        }
        while base_assemblies.len() < MAX_BA * 2 { base_assemblies.push(BaseAssembly { id: base_assemblies.len() as i32, parent_assembly_id: -1, build_date: TmCell::new(0), composite_part_ids: Vec::new(), valid: TmCell::new(0) }); }
        while composite_parts.len() < MAX_CP * 2 { composite_parts.push(CompositePart { id: composite_parts.len() as i32, build_date: TmCell::new(0), document_id: -1, root_atomic_part_id: -1, atomic_part_ids: Vec::new(), base_assembly_ids: Vec::new(), valid: TmCell::new(0), ap_count: TmCell::new(0) }); }
        while atomic_parts.len() < MAX_AP * 2 { atomic_parts.push(AtomicPart { id: atomic_parts.len() as i32, x: TmCell::new(0), y: TmCell::new(0), z: TmCell::new(0), build_date: TmCell::new(0), weight: TmCell::new(0), composite_part_id: -1, connection_ids: Vec::new(), valid: TmCell::new(0) }); }
        while connections.len() < MAX_CONN * 2 { connections.push(Connection { id: connections.len() as i32, from_atomic_part_id: -1, to_atomic_part_id: -1, conn_type: TmCell::new(0), valid: TmCell::new(0) }); }
        while documents.len() < MAX_DOCS + MAX_CP { documents.push(Document { id: documents.len() as i32, doc_type: TmCell::new(0), build_date: TmCell::new(0), composite_part_id: -1 }); }

        eprintln!("Init: {} CA, {} BA, {} CP, {} AP, {} conn, {} docs",
            complex_assemblies.len(), base_assemblies.len(), composite_parts.len(),
            atomic_parts.len(), connections.len(), documents.len());

        Database { modules, complex_assemblies, base_assemblies, composite_parts, atomic_parts,
            connections, documents, cp_by_date, next_cp_slot: AtomicUsize::new(MAX_CP),
            next_ap_slot: AtomicUsize::new(MAX_AP), next_conn_slot: AtomicUsize::new(MAX_CONN),
            next_ba_slot: AtomicUsize::new(MAX_BA) }
    }
}

// ── Operations (copied from stmbench7.rs) ───────────────────────────

fn op_lt1(db: &Database, tx: &Transaction) -> i64 {
    let mut sum = 0i64;
    for ca in &db.complex_assemblies { sum += ca.id as i64 + ca.level as i64 + tx.read(&ca.build_date) as i64; }
    for ba in &db.base_assemblies { if tx.read(&ba.valid) != 0 { sum += ba.id as i64 + tx.read(&ba.build_date) as i64; } }
    sum
}
fn op_lt2(db: &Database, tx: &Transaction) {
    for ca in &db.complex_assemblies { let d = tx.read(&ca.build_date); tx.write(&ca.build_date, (d + 1) % 365 + 1000); }
    for ba in &db.base_assemblies { if tx.read(&ba.valid) != 0 { let d = tx.read(&ba.build_date); tx.write(&ba.build_date, (d + 1) % 365 + 1000); } }
}
fn op_lt3(db: &Database, tx: &Transaction) -> i64 {
    let mut sum = 0i64;
    for cp in &db.composite_parts { if tx.read(&cp.valid) != 0 { sum += cp.id as i64 + tx.read(&cp.build_date) as i64; } }
    sum
}
fn op_lt4(db: &Database, tx: &Transaction) -> i64 {
    let mut sum = 0i64;
    for ap in &db.atomic_parts { if tx.read(&ap.valid) != 0 { let w = (tx.read(&ap.weight) % 50) + 1; tx.write(&ap.weight, w); sum += w as i64; } }
    sum
}
fn op_lt5(db: &Database, tx: &Transaction) -> i64 {
    let mut sum = 0i64;
    for c in &db.connections { if tx.read(&c.valid) != 0 { sum += c.id as i64 + c.from_atomic_part_id as i64 + c.to_atomic_part_id as i64 + tx.read(&c.conn_type) as i64; } }
    sum
}

fn op_st1(db: &Database, tx: &Transaction) -> i64 {
    let mut sum = 0i64;
    if db.modules.is_empty() { return 0; }
    let mut ci = db.modules[0].root_assembly_id as usize;
    if ci < db.complex_assemblies.len() { sum += db.complex_assemblies[ci].id as i64; }
    for _l in 0..TREE_LEVELS - 1 {
        if ci >= db.complex_assemblies.len() || db.complex_assemblies[ci].child_assembly_ids.is_empty() { break; }
        ci = db.complex_assemblies[ci].child_assembly_ids[0]; sum += db.complex_assemblies[ci].id as i64;
    }
    if ci < db.complex_assemblies.len() && !db.complex_assemblies[ci].child_base_assembly_ids.is_empty() {
        let bi = db.complex_assemblies[ci].child_base_assembly_ids[0];
        if bi < db.base_assemblies.len() && tx.read(&db.base_assemblies[bi].valid) != 0 {
            sum += tx.read(&db.base_assemblies[bi].build_date) as i64;
            if !db.base_assemblies[bi].composite_part_ids.is_empty() {
                let cpi = db.base_assemblies[bi].composite_part_ids[0];
                if cpi < db.composite_parts.len() && tx.read(&db.composite_parts[cpi].valid) != 0 { sum += tx.read(&db.composite_parts[cpi].build_date) as i64; }
            }
        }
    }
    sum
}
fn op_st2(db: &Database, tx: &Transaction, doc_idx: usize) -> i64 {
    if doc_idx >= db.documents.len() { return 0; }
    let mut sum = tx.read(&db.documents[doc_idx].build_date) as i64 + tx.read(&db.documents[doc_idx].doc_type) as i64;
    let ci = db.documents[doc_idx].composite_part_id as usize;
    if ci < db.composite_parts.len() && tx.read(&db.composite_parts[ci].valid) != 0 {
        for &ai in &db.composite_parts[ci].atomic_part_ids { if ai < db.atomic_parts.len() && tx.read(&db.atomic_parts[ai].valid) != 0 { sum += tx.read(&db.atomic_parts[ai].weight) as i64; } }
    }
    sum
}
fn op_st3(db: &Database, tx: &Transaction, ap_idx: usize) -> i64 {
    if ap_idx >= db.atomic_parts.len() { return 0; }
    let mut sum = 0i64;
    let ap = &db.atomic_parts[ap_idx];
    sum += tx.read(&ap.x) as i64 + tx.read(&ap.y) as i64 + tx.read(&ap.z) as i64;
    for &ci in &ap.connection_ids {
        if ci < db.connections.len() && tx.read(&db.connections[ci].valid) != 0 {
            let nb = if db.connections[ci].from_atomic_part_id as usize == ap_idx { db.connections[ci].to_atomic_part_id as usize } else { db.connections[ci].from_atomic_part_id as usize };
            if nb < db.atomic_parts.len() && tx.read(&db.atomic_parts[nb].valid) != 0 { sum += tx.read(&db.atomic_parts[nb].weight) as i64; }
        }
    }
    sum
}
fn op_st4(db: &Database, tx: &Transaction, ca_idx: usize) {
    if ca_idx < db.complex_assemblies.len() {
        let d = tx.read(&db.complex_assemblies[ca_idx].build_date);
        tx.write(&db.complex_assemblies[ca_idx].build_date, (d + 1) % 365 + 1000);
    }
}
fn op_st5(db: &Database, tx: &Transaction, low: i32, high: i32) -> i64 {
    let mut count = 0i64;
    for &(d, i) in &db.cp_by_date {
        if d < low { continue; }
        if d > high { break; }
        if i < db.composite_parts.len() && tx.read(&db.composite_parts[i].valid) != 0 { count += 1; }
    }
    count
}
fn op_st6(db: &Database, tx: &Transaction, ap_idx: usize) {
    if ap_idx < db.atomic_parts.len() {
        tx.write(&db.atomic_parts[ap_idx].x, (tx.read(&db.atomic_parts[ap_idx].x) + 1) % 100);
        tx.write(&db.atomic_parts[ap_idx].y, (tx.read(&db.atomic_parts[ap_idx].y) + 1) % 100);
    }
}
fn op_st7(db: &Database, tx: &Transaction, cp_idx: usize) -> i64 {
    if cp_idx >= db.composite_parts.len() || tx.read(&db.composite_parts[cp_idx].valid) == 0 { return 0; }
    let mut max_w = 0i64;
    for &ai in &db.composite_parts[cp_idx].atomic_part_ids { if ai < db.atomic_parts.len() && tx.read(&db.atomic_parts[ai].valid) != 0 { max_w = max_w.max(tx.read(&db.atomic_parts[ai].weight) as i64); } }
    max_w
}
fn op_st8(db: &Database, tx: &Transaction, ba_idx: usize) {
    if ba_idx < db.base_assemblies.len() && tx.read(&db.base_assemblies[ba_idx].valid) != 0 {
        let d = tx.read(&db.base_assemblies[ba_idx].build_date);
        tx.write(&db.base_assemblies[ba_idx].build_date, (d + 1) % 365 + 1000);
    }
}
fn op_st9(db: &Database, tx: &Transaction, cp_idx: usize) -> i64 {
    if cp_idx >= db.composite_parts.len() || tx.read(&db.composite_parts[cp_idx].valid) == 0 { return 0; }
    let mut sum = 0i64;
    for &ai in &db.composite_parts[cp_idx].atomic_part_ids { if ai < db.atomic_parts.len() && tx.read(&db.atomic_parts[ai].valid) != 0 { sum += tx.read(&db.atomic_parts[ai].weight) as i64; } }
    sum
}
fn op_st10(db: &Database, tx: &Transaction, cp_idx: usize) {
    if cp_idx < db.composite_parts.len() && tx.read(&db.composite_parts[cp_idx].valid) != 0 {
        let di = db.composite_parts[cp_idx].document_id as usize;
        if di < db.documents.len() { let d = tx.read(&db.documents[di].build_date); tx.write(&db.documents[di].build_date, (d + 1) % 365 + 1000); }
    }
}

fn op_op1(db: &Database, tx: &Transaction, id: usize) -> i64 {
    if id < db.atomic_parts.len() && tx.read(&db.atomic_parts[id].valid) != 0 { let ap = &db.atomic_parts[id]; return (tx.read(&ap.x) + tx.read(&ap.y) + tx.read(&ap.z)) as i64; }
    0
}
fn op_op2(db: &Database, tx: &Transaction, id: usize) -> i64 {
    if id < db.composite_parts.len() && tx.read(&db.composite_parts[id].valid) != 0 { return tx.read(&db.composite_parts[id].build_date) as i64; }
    0
}
fn op_op3(db: &Database, tx: &Transaction, doc_idx: usize) -> i64 {
    if doc_idx < db.documents.len() { return (tx.read(&db.documents[doc_idx].build_date) + tx.read(&db.documents[doc_idx].doc_type)) as i64; }
    0
}
fn op_op4(db: &Database, tx: &Transaction, ba_idx: usize) -> i64 {
    if ba_idx < db.base_assemblies.len() && tx.read(&db.base_assemblies[ba_idx].valid) != 0 {
        let ba = &db.base_assemblies[ba_idx];
        return ba.composite_part_ids.iter().map(|&ci| if ci < db.composite_parts.len() && tx.read(&db.composite_parts[ci].valid) != 0 { tx.read(&db.composite_parts[ci].build_date) as i64 } else { 0 }).sum();
    }
    0
}
fn op_op5(db: &Database, tx: &Transaction, ca_idx: usize) -> i64 {
    if ca_idx < db.complex_assemblies.len() {
        let ca = &db.complex_assemblies[ca_idx];
        return ca.child_assembly_ids.iter().map(|&ci| if ci < db.complex_assemblies.len() { tx.read(&db.complex_assemblies[ci].build_date) as i64 } else { 0 }).sum::<i64>()
            + ca.child_base_assembly_ids.iter().map(|&bi| if bi < db.base_assemblies.len() && tx.read(&db.base_assemblies[bi].valid) != 0 { tx.read(&db.base_assemblies[bi].build_date) as i64 } else { 0 }).sum::<i64>();
    }
    0
}
fn op_op6(db: &Database, tx: &Transaction, ap_idx: usize) -> i64 {
    if ap_idx < db.atomic_parts.len() && tx.read(&db.atomic_parts[ap_idx].valid) != 0 {
        return db.atomic_parts[ap_idx].connection_ids.iter().map(|&ci| if ci < db.connections.len() && tx.read(&db.connections[ci].valid) != 0 { tx.read(&db.connections[ci].conn_type) as i64 } else { 0 }).sum();
    }
    0
}
fn op_op7(db: &Database, tx: &Transaction, cp_idx: usize) -> i64 {
    if cp_idx < db.composite_parts.len() && tx.read(&db.composite_parts[cp_idx].valid) != 0 { return tx.read(&db.composite_parts[cp_idx].ap_count) as i64; }
    0
}
fn op_op8(db: &Database, tx: &Transaction, cp_idx: usize) -> i64 {
    if cp_idx < db.composite_parts.len() && tx.read(&db.composite_parts[cp_idx].valid) != 0 { return db.composite_parts[cp_idx].base_assembly_ids.len() as i64; }
    0
}
fn op_op9(db: &Database, tx: &Transaction, cp_idx: usize) -> i64 {
    if cp_idx < db.composite_parts.len() && tx.read(&db.composite_parts[cp_idx].valid) != 0 {
        let mut oldest = 999999i64;
        for &bi in &db.composite_parts[cp_idx].base_assembly_ids { if bi < db.base_assemblies.len() && tx.read(&db.base_assemblies[bi].valid) != 0 { oldest = oldest.min(tx.read(&db.base_assemblies[bi].build_date) as i64); } }
        return oldest;
    }
    999999
}
fn op_op10(db: &Database, tx: &Transaction, ap_idx: usize) -> i64 {
    if ap_idx < db.atomic_parts.len() && tx.read(&db.atomic_parts[ap_idx].valid) != 0 {
        return db.atomic_parts[ap_idx].connection_ids.iter().map(|&ci| {
            if ci < db.connections.len() && tx.read(&db.connections[ci].valid) != 0 {
                let nb = if db.connections[ci].from_atomic_part_id as usize == ap_idx { db.connections[ci].to_atomic_part_id as usize } else { db.connections[ci].from_atomic_part_id as usize };
                if nb < db.atomic_parts.len() && tx.read(&db.atomic_parts[nb].valid) != 0 { return tx.read(&db.atomic_parts[nb].weight) as i64; }
            }
            0i64
        }).sum();
    }
    0
}
fn op_op11(db: &Database, tx: &Transaction, ap_idx: usize, new_x: i32, new_y: i32) {
    if ap_idx < db.atomic_parts.len() && tx.read(&db.atomic_parts[ap_idx].valid) != 0 { tx.write(&db.atomic_parts[ap_idx].x, new_x); tx.write(&db.atomic_parts[ap_idx].y, new_y); }
}
fn op_op12(db: &Database, tx: &Transaction, ap_idx: usize, new_w: i32) {
    if ap_idx < db.atomic_parts.len() && tx.read(&db.atomic_parts[ap_idx].valid) != 0 { tx.write(&db.atomic_parts[ap_idx].weight, new_w); }
}
fn op_op13(db: &Database, tx: &Transaction, doc_idx: usize, new_date: i32) {
    if doc_idx < db.documents.len() { tx.write(&db.documents[doc_idx].build_date, new_date); }
}
fn op_op14(db: &Database, tx: &Transaction, cp_idx: usize, new_date: i32) {
    if cp_idx < db.composite_parts.len() && tx.read(&db.composite_parts[cp_idx].valid) != 0 { tx.write(&db.composite_parts[cp_idx].build_date, new_date); }
}
fn op_op15(db: &Database, tx: &Transaction, ba_idx: usize, new_date: i32) {
    if ba_idx < db.base_assemblies.len() && tx.read(&db.base_assemblies[ba_idx].valid) != 0 { tx.write(&db.base_assemblies[ba_idx].build_date, new_date); }
}

fn op_sm1(db: &Database, tx: &Transaction, new_id: i32) {
    let slot = db.next_cp_slot.fetch_add(1, Ordering::Relaxed);
    let doc_slot = slot;
    if slot >= db.composite_parts.len() { return; }
    let cp = &db.composite_parts[slot];
    tx.write(&cp.valid, 1);
    tx.write(&cp.build_date, 2000);
    let first_ap = slot * AP_PER_CP;
    for j in 0..AP_PER_CP {
        let ap_idx = first_ap + j;
        if ap_idx < db.atomic_parts.len() {
            tx.write(&db.atomic_parts[ap_idx].valid, 1);
            tx.write(&db.atomic_parts[ap_idx].x, (j % 100) as i32);
            tx.write(&db.atomic_parts[ap_idx].y, ((j / 100) % 100) as i32);
            tx.write(&db.atomic_parts[ap_idx].z, (j / 10000) as i32);
            tx.write(&db.atomic_parts[ap_idx].build_date, 2000);
            tx.write(&db.atomic_parts[ap_idx].weight, 10);
        }
    }
    if doc_slot < db.documents.len() {
        tx.write(&db.documents[doc_slot].build_date, 2000);
        tx.write(&db.documents[doc_slot].doc_type, new_id % 3);
    }
}
fn op_sm2(db: &Database, tx: &Transaction, ci: usize) {
    if ci < db.composite_parts.len() { tx.write(&db.composite_parts[ci].valid, 0); }
}
fn op_sm3(db: &Database, tx: &Transaction, cp_idx: usize) {
    if cp_idx >= db.composite_parts.len() { return; }
    if tx.read(&db.composite_parts[cp_idx].valid) == 0 { return; }
    let slot = db.next_ap_slot.fetch_add(1, Ordering::Relaxed);
    if slot >= db.atomic_parts.len() { return; }
    tx.write(&db.atomic_parts[slot].valid, 1);
    tx.write(&db.atomic_parts[slot].x, 0);
    tx.write(&db.atomic_parts[slot].y, 0);
    tx.write(&db.atomic_parts[slot].z, 0);
    tx.write(&db.atomic_parts[slot].build_date, 2000);
    tx.write(&db.atomic_parts[slot].weight, 5);
}
fn op_sm4(db: &Database, tx: &Transaction, ai: usize) {
    if ai < db.atomic_parts.len() { tx.write(&db.atomic_parts[ai].valid, 0); }
}
fn op_sm5(db: &Database, tx: &Transaction, from_ap: usize, to_ap: usize, typ: i32) {
    if from_ap >= db.atomic_parts.len() || to_ap >= db.atomic_parts.len() { return; }
    let slot = db.next_conn_slot.fetch_add(1, Ordering::Relaxed);
    if slot >= db.connections.len() { return; }
    tx.write(&db.connections[slot].valid, 1);
    tx.write(&db.connections[slot].conn_type, typ);
}
fn op_sm6(db: &Database, tx: &Transaction, ci: usize) {
    if ci < db.connections.len() { tx.write(&db.connections[ci].valid, 0); }
}
fn op_sm7(db: &Database, tx: &Transaction, parent_ca_idx: usize) {
    if parent_ca_idx >= db.complex_assemblies.len() { return; }
    if db.complex_assemblies[parent_ca_idx].level as usize != TREE_LEVELS - 1 { return; }
    let slot = db.next_ba_slot.fetch_add(1, Ordering::Relaxed);
    if slot >= db.base_assemblies.len() { return; }
    tx.write(&db.base_assemblies[slot].valid, 1);
    tx.write(&db.base_assemblies[slot].build_date, 2000);
}
fn op_sm8(db: &Database, tx: &Transaction, bi: usize) {
    if bi < db.base_assemblies.len() { tx.write(&db.base_assemblies[bi].valid, 0); }
}

// ── Pick operation (determines category + read/write) ──────────────

struct OpDesc { cat: u8, is_read: bool }

fn pick_operation(rng: &mut Xsrng, write_percent: i32) -> OpDesc {
    let r = rng.range(0, 100);
    let cat = if r < 5 { 0 } else if r < 45 { 1 } else if r < 90 { 2 } else { 3 };
    if cat == 3 { return OpDesc { cat, is_read: false }; }
    let want_read = (rng.range(0, 100) as i32) < (100 - write_percent);
    OpDesc { cat, is_read: want_read }
}

// ── Generate a task closure for a given operation ──────────────────
// Returns a Box<dyn Fn(&Transaction)> that captures the db Arc
// and all needed args.  tm::transaction takes Fn (not FnOnce)
// because it may retry on abort.

type Task = Box<dyn Fn(&Transaction) + Send + 'static>;

fn make_task(db: &Arc<Database>, desc: &OpDesc, rng: &mut Xsrng) -> Task {
    let db = Arc::clone(db);
    match (desc.cat, desc.is_read, rng.range(0, 20)) {
        (0, true, _) => Box::new(move |tx| { op_lt1(&db, tx); }),
        (0, false, _) => Box::new(move |tx| { op_lt2(&db, tx); }),
        (1, true, s) if s < 4 => { let di = rng.range(0, MAX_DOCS); Box::new(move |tx| { op_st2(&db, tx, di); }) }
        (1, true, s) if s < 8 => { let ai = rng.range(0, MAX_AP); Box::new(move |tx| { op_st3(&db, tx, ai); }) }
        (1, true, s) if s < 12 => { let lo = 1000 + rng.range(0, 100) as i32; let hi = 1000 + 200 + rng.range(0, 100) as i32; Box::new(move |tx| { op_st5(&db, tx, lo, hi); }) }
        (1, true, s) if s < 16 => { let ci = rng.range(0, MAX_CP); Box::new(move |tx| { op_st7(&db, tx, ci); }) }
        (1, true, _) => { let ci = rng.range(0, MAX_CP); Box::new(move |tx| { op_st9(&db, tx, ci); }) }
        (1, false, s) if s < 4 => { let ci = rng.range(0, MAX_CA); Box::new(move |tx| { op_st4(&db, tx, ci); }) }
        (1, false, s) if s < 8 => { let ai = rng.range(0, MAX_AP); Box::new(move |tx| { op_st6(&db, tx, ai); }) }
        (1, false, s) if s < 12 => { let bi = rng.range(0, MAX_BA); Box::new(move |tx| { op_st8(&db, tx, bi); }) }
        (1, false, s) if s < 16 => { let ci = rng.range(0, MAX_CP); Box::new(move |tx| { op_st10(&db, tx, ci); }) }
        (1, false, _) => Box::new(move |tx| { op_st1(&db, tx); }),
        (2, true, s) if s < 10 => { let id = rng.range(0, MAX_AP); Box::new(move |tx| { op_op1(&db, tx, id); }) }
        (2, true, _) => { let ci = rng.range(0, MAX_CP); Box::new(move |tx| { op_op9(&db, tx, ci); }) }
        (2, false, s) if s < 4 => { let ai = rng.range(0, MAX_AP); let nx = rng.range_i32(0, 100); let ny = rng.range_i32(0, 100); Box::new(move |tx| { op_op11(&db, tx, ai, nx, ny); }) }
        (2, false, _) => { let ai = rng.range(0, MAX_AP); let nw = rng.range_i32(1, 51); Box::new(move |tx| { op_op12(&db, tx, ai, nw); }) }
        (3, _, s) if s < 3 => { let nid = (MAX_CP + rng.range(0, 1000)) as i32; Box::new(move |tx| { op_sm1(&db, tx, nid); }) }
        (3, _, s) if s < 7 => { let ci = rng.range(0, MAX_CP); Box::new(move |tx| { op_sm2(&db, tx, ci); }) }
        (3, _, s) if s < 10 => { let ci = rng.range(0, MAX_CP); Box::new(move |tx| { op_sm3(&db, tx, ci); }) }
        (3, _, s) if s < 13 => { let ai = rng.range(0, MAX_AP); Box::new(move |tx| { op_sm4(&db, tx, ai); }) }
        (3, _, s) if s < 17 => { let a = rng.range(0, MAX_AP); let b = rng.range(0, MAX_AP); let t = rng.range_i32(0, 3); Box::new(move |tx| { op_sm5(&db, tx, a, b, t); }) }
        (3, _, _) => { let ci = rng.range(0, MAX_CONN); Box::new(move |tx| { op_sm6(&db, tx, ci); }) }
        _ => Box::new(move |_tx| {}),
    }
}

// ── Async queue infrastructure ────────────────────────────────────

struct AsyncQueue {
    sender: Sender<Task>,
    completed: Arc<AtomicUsize>,
    _handles: Vec<JoinHandle<()>>,
}

impl AsyncQueue {
    fn new(num_workers: usize, db: Arc<Database>) -> Self {
        let (task_sender, task_receiver) = std::sync::mpsc::channel::<Task>();
        let rx = Arc::new(std::sync::Mutex::new(task_receiver));
        let completed = Arc::new(AtomicUsize::new(0));
        let mut handles = Vec::with_capacity(num_workers);

        for _tid in 0..num_workers {
            let rx = Arc::clone(&rx);
            let completed = Arc::clone(&completed);

            handles.push(thread::spawn(move || {
                tm_init_thread();
                loop {
                    let task = {
                        let lock = rx.lock().unwrap();
                        lock.recv()
                    };
                    match task {
                        Ok(task) => {
                            transaction(|tx| task(tx));
                            completed.fetch_add(1, Ordering::Release);
                        }
                        Err(_) => break,
                    }
                }
                tm_exit_thread();
            }));
        }

        AsyncQueue { sender: task_sender, completed, _handles: handles }
    }

    fn enqueue(&self, task: Task) {
        self.sender.send(task).unwrap();
    }

    fn completed_count(&self) -> usize {
        self.completed.load(Ordering::Acquire)
    }

    fn wait_for(&self, target: usize) {
        while self.completed.load(Ordering::Acquire) < target {
            std::hint::spin_loop();
        }
    }
}

// ── Main ──────────────────────────────────────────────────────────

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let mut duration_ms = 10000u64;
    let mut nb_threads = 4usize;
    let mut workload = 1i32;
    let batch_size = 1000usize;

    let mut i = 1;
    while i < args.len() {
        match args[i].as_str() {
            "-d" if i + 1 < args.len() => { i += 1; duration_ms = args[i].parse().unwrap_or(10000); }
            "-t" if i + 1 < args.len() => { i += 1; nb_threads = args[i].parse().unwrap_or(4); }
            "-w" if i + 1 < args.len() => { i += 1; workload = args[i].parse().unwrap_or(1); }
            _ => {}
        }
        i += 1;
    }

    let write_percent = match workload { 2 => 40, 3 => 90, _ => 10 };

    println!("STMbench7 — Rust async (queue-manual) dispatch");
    println!("Workload: {} ({}% read, {}% write)", workload, 100 - write_percent, write_percent);
    println!("Duration: {} ms  Workers: {}  Batch size: {}", duration_ms, nb_threads, batch_size);

    tm_init();
    let db = Arc::new(Database::new());
    println!("  CA: {}  BA: {}  CP: {}  AP: {}  Conn: {}  Docs: {}",
             db.complex_assemblies.len(), db.base_assemblies.len(),
             db.composite_parts.len(), db.atomic_parts.len(),
             db.connections.len(), db.documents.len());

    let queue = AsyncQueue::new(nb_threads, db.clone());
    let mut total_ops: usize = 0;
    let mut batch_no: usize = 0;
    let start_time = std::time::Instant::now();

    loop {
        let elapsed = start_time.elapsed().as_secs_f64();
        if (elapsed * 1000.0) >= duration_ms as f64 { break; }

        let mut rng = Xsrng::new(42 + batch_no as u64 * 12345);
        let batch_start = queue.completed_count();

        for _bi in 0..batch_size {
            let desc = pick_operation(&mut rng, write_percent);
            let task = make_task(&db, &desc, &mut rng);
            queue.enqueue(task);
        }

        total_ops += batch_size;
        batch_no += 1;

        // Barrier: wait for this batch to complete (like tm_wait_prev_tx)
        queue.wait_for(batch_start + batch_size);

        if batch_no % 10 == 0 || batch_no == 1 {
            let elapsed = start_time.elapsed().as_secs_f64();
            println!("  [batch {}] {} ops, {:.0} ops/s", batch_no, total_ops,
                     if elapsed > 0.0 { total_ops as f64 / elapsed } else { 0.0 });
        }
    }

    let elapsed = start_time.elapsed().as_secs_f64();
    let completed = queue.completed_count();

    println!("\nResults");
    println!("=======");
    println!("Elapsed:    {:.3}s", elapsed);
    println!("Batches:    {}", batch_no);
    println!("Enqueued:   {}", total_ops);
    println!("Completed:  {}", completed);
    println!("Throughput: {:.0} txns/sec", if elapsed > 0.0 { total_ops as f64 / elapsed } else { 0.0 });

    tm_exit();
    println!("PASS");
}
