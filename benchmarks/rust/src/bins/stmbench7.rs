use std::cell::RefCell;
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::sync::Arc;
use std::time::Duration;
use tm::{TmCell, Transaction, transaction, tm_init, tm_exit, tm_init_thread, tm_exit_thread};

// ── Spec constants (§2: medium OO7 size) ─────────────────────────
const FANOUT: usize = 3;
const TREE_LEVELS: usize = 6;
const MAX_CP: usize = 500;
const AP_PER_CP: usize = 200;
const CONN_PER_AP: usize = 3;
const MAX_BA: usize = 729;
const MAX_CA: usize = 364;
const MAX_AP: usize = MAX_CP * AP_PER_CP;  // 100,000
const MAX_CONN: usize = MAX_AP * CONN_PER_AP;  // 300,000
const MAX_DOCS: usize = MAX_CP;
const MAX_CP_BA_BAG: usize = 5;

// ── Data structures ─────────────────────────────────────────────
struct Document {
    id: i32,
    doc_type: TmCell<i32>,
    build_date: TmCell<i32>,
    composite_part_id: i32,
}

struct Connection {
    id: i32,
    from_atomic_part_id: i32,
    to_atomic_part_id: i32,
    conn_type: TmCell<i32>,
    valid: TmCell<i32>,
}

struct AtomicPart {
    id: i32,
    x: TmCell<i32>,
    y: TmCell<i32>,
    z: TmCell<i32>,
    build_date: TmCell<i32>,
    weight: TmCell<i32>,
    composite_part_id: i32,
    connection_ids: Vec<usize>,
    valid: TmCell<i32>,
}

struct CompositePart {
    id: i32,
    build_date: TmCell<i32>,
    document_id: i32,
    root_atomic_part_id: i32,
    atomic_part_ids: Vec<usize>,
    base_assembly_ids: Vec<usize>,
    valid: TmCell<i32>,
    ap_count: TmCell<i32>,
}

struct BaseAssembly {
    id: i32,
    parent_assembly_id: i32,
    build_date: TmCell<i32>,
    composite_part_ids: Vec<usize>,
    valid: TmCell<i32>,
}

struct ComplexAssembly {
    id: i32,
    level: i32,
    parent_id: i32,
    child_assembly_ids: Vec<usize>,
    child_base_assembly_ids: Vec<usize>,
    build_date: TmCell<i32>,
}

struct Module {
    id: i32,
    root_assembly_id: i32,
}

// ── RNG ──────────────────────────────────────────────────────────
struct Xsrng(u64);
impl Xsrng {
    fn new(seed: u64) -> Self {
        Self(seed.wrapping_mul(6364136223846793005).wrapping_add(1442695040888963407))
    }
    fn next(&mut self) -> u64 {
        self.0 = self.0.wrapping_mul(6364136223846793005).wrapping_add(1442695040888963407);
        self.0 >> 33
    }
    fn range(&mut self, lo: usize, hi: usize) -> usize {
        lo + (self.next() as usize) % (hi - lo)
    }
    fn range_i32(&mut self, lo: i32, hi: i32) -> i32 {
        lo + (self.next() as i32) % (hi - lo)
    }
}

// ── Database ────────────────────────────────────────────────────
struct Database {
    modules: Vec<Module>,
    complex_assemblies: Vec<ComplexAssembly>,
    base_assemblies: Vec<BaseAssembly>,
    composite_parts: Vec<CompositePart>,
    atomic_parts: Vec<AtomicPart>,
    connections: Vec<Connection>,
    documents: Vec<Document>,
    cp_by_date: Vec<(i32, usize)>,

    // Slot allocators for structure modifications (SM ops)
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

        // Pre-allocate assembly tree
        let capacity_ca = MAX_CA;
        let mut complex_assemblies = Vec::with_capacity(capacity_ca);

        let capacity_ba = MAX_BA * 2;
        let mut base_assemblies = Vec::with_capacity(capacity_ba);

        let capacity_cp = MAX_CP * 2;
        let mut composite_parts = Vec::with_capacity(capacity_cp);

        let capacity_ap = MAX_AP * 2;
        let mut atomic_parts = Vec::with_capacity(capacity_ap);

        let capacity_conn = MAX_CONN * 2;
        let mut connections = Vec::with_capacity(capacity_conn);

        let capacity_doc = MAX_DOCS + MAX_CP;
        let mut documents = Vec::with_capacity(capacity_doc);

        // Build assembly tree
        let mut level_sizes = [0usize; TREE_LEVELS];
        let mut level_offsets = [0usize; TREE_LEVELS];
        let mut off = 0usize;
        let mut sz = 1usize;

        for l in 0..TREE_LEVELS {
            level_sizes[l] = sz;
            level_offsets[l] = off;
            for j in 0..sz {
                complex_assemblies.push(ComplexAssembly {
                    id: (off + j) as i32,
                    level: l as i32,
                    parent_id: -1,
                    child_assembly_ids: Vec::with_capacity(FANOUT),
                    child_base_assembly_ids: Vec::with_capacity(FANOUT),
                    build_date: TmCell::new(1000 + (l as i32 * 100 + j as i32) % 365),
                });
            }
            off += sz;
            sz *= FANOUT;
        }

        // Wire parent/child among CAs
        for l in 1..TREE_LEVELS {
            let parent_off = level_offsets[l - 1];
            let parent_sz = level_sizes[l - 1];
            let child_off = level_offsets[l];
            for p in 0..parent_sz {
                let parent_idx = parent_off + p;
                for c in 0..FANOUT {
                    let child_idx = child_off + p * FANOUT + c;
                    complex_assemblies[parent_idx].child_assembly_ids.push(child_idx);
                    complex_assemblies[child_idx].parent_id = parent_idx as i32;
                }
            }
        }

        // BA leaves
        let ba_parent_off = level_offsets[TREE_LEVELS - 1];
        let ba_parent_sz = level_sizes[TREE_LEVELS - 1];
        let mut ba_id = 0i32;
        for p in 0..ba_parent_sz {
            let parent_idx = ba_parent_off + p;
            for _c in 0..FANOUT {
                base_assemblies.push(BaseAssembly {
                    id: ba_id,
                    parent_assembly_id: parent_idx as i32,
                    build_date: TmCell::new(1000 + (ba_id as usize % 365) as i32),
                    composite_part_ids: Vec::new(),
                    valid: TmCell::new(1),
                });
                complex_assemblies[parent_idx].child_base_assembly_ids.push(ba_id as usize);
                ba_id += 1;
            }
        }

        // CPs + Documents
        for cp_idx in 0..MAX_CP {
            let cd = 1000 + (cp_idx % 365) as i32;
            composite_parts.push(CompositePart {
                id: cp_idx as i32,
                build_date: TmCell::new(cd),
                document_id: cp_idx as i32,
                root_atomic_part_id: -1,
                atomic_part_ids: Vec::with_capacity(AP_PER_CP * 2),
                base_assembly_ids: Vec::with_capacity(MAX_CP_BA_BAG * 2),
                valid: TmCell::new(1),
                ap_count: TmCell::new(0),
            });
            documents.push(Document {
                id: cp_idx as i32,
                doc_type: TmCell::new((cp_idx % 3) as i32),
                build_date: TmCell::new(cd),
                composite_part_id: cp_idx as i32,
            });
        }

        // CP-BA bags (no duplicate avoidance in Rust for simplicity)
        {
            let mut bag_rng = Xsrng::new(42);
            for ci in 0..MAX_CP {
                let num = 1 + bag_rng.range(0, MAX_CP_BA_BAG - 1);
                for _ in 0..num {
                    let bi = bag_rng.range(0, MAX_BA);
                    composite_parts[ci].base_assembly_ids.push(bi);
                    base_assemblies[bi].composite_part_ids.push(ci);
                }
            }
        }

        // Date index for CP (sorted for binary search)
        let mut raw_idx: Vec<(i32, usize)> = composite_parts.iter().enumerate()
            .map(|(i, cp)| (unsafe { *cp.build_date.ptr() }, i))
            .collect();
        raw_idx.sort_by(|a, b| a.0.cmp(&b.0));
        let cp_by_date = raw_idx;

        // APs + connections
        {
            let mut ap_rng = Xsrng::new(99);

            for ci in 0..MAX_CP {
                let first = ci * AP_PER_CP;
                composite_parts[ci].root_atomic_part_id = first as i32;

                for j in 0..AP_PER_CP {
                    let ap_id = first + j;
                    atomic_parts.push(AtomicPart {
                        id: ap_id as i32,
                        x: TmCell::new((j % 100) as i32),
                        y: TmCell::new(((j / 100) % 100) as i32),
                        z: TmCell::new((j / 10000) as i32),
                        build_date: TmCell::new((1000 + (ci * AP_PER_CP + j) % 365) as i32),
                        weight: TmCell::new(((j % 50) + 1) as i32),
                        composite_part_id: ci as i32,
                        connection_ids: Vec::with_capacity(CONN_PER_AP * 4),
                        valid: TmCell::new(1),
                    });
                    composite_parts[ci].atomic_part_ids.push(first + j);
                }

                // Connections: ring + chord + random
                for j in 0..AP_PER_CP {
                    let a = first + j;
                    // Ring: j → (j+1)%n
                    let t1 = first + (j + 1) % AP_PER_CP;
                    let c_id1 = connections.len();
                    connections.push(Connection {
                        id: c_id1 as i32,
                        from_atomic_part_id: a as i32,
                        to_atomic_part_id: t1 as i32,
                        conn_type: TmCell::new((j % 3) as i32),
                        valid: TmCell::new(1),
                    });
                    atomic_parts[a].connection_ids.push(c_id1);

                    // Chord: j → (j+2)%n
                    let t2 = first + (j + 2) % AP_PER_CP;
                    let c_id2 = connections.len();
                    connections.push(Connection {
                        id: c_id2 as i32,
                        from_atomic_part_id: a as i32,
                        to_atomic_part_id: t2 as i32,
                        conn_type: TmCell::new(((j + 1) % 3) as i32),
                        valid: TmCell::new(1),
                    });
                    atomic_parts[a].connection_ids.push(c_id2);

                    // Random extra edges to reach CONN_PER_AP
                    for _k in 0..(CONN_PER_AP - 2) {
                        let mut t3 = first + ap_rng.range(0, AP_PER_CP);
                        if t3 == a {
                            // pick again differently
                            t3 = first + (j + 3) % AP_PER_CP;
                        }
                        let c_id3 = connections.len();
                        connections.push(Connection {
                            id: c_id3 as i32,
                            from_atomic_part_id: a as i32,
                            to_atomic_part_id: t3 as i32,
                            conn_type: TmCell::new(((j + _k) % 3) as i32),
                            valid: TmCell::new(1),
                        });
                        atomic_parts[a].connection_ids.push(c_id3);
                    }
                }
            }
        }

        // Fill remaining capacity with dummy elements for SM operations
        let capacity_ba = MAX_BA * 2;
        while base_assemblies.len() < capacity_ba {
            base_assemblies.push(BaseAssembly {
                id: base_assemblies.len() as i32,
                parent_assembly_id: -1,
                build_date: TmCell::new(0),
                composite_part_ids: Vec::new(),
                valid: TmCell::new(0),
            });
        }
        let capacity_cp = MAX_CP * 2;
        while composite_parts.len() < capacity_cp {
            composite_parts.push(CompositePart {
                id: composite_parts.len() as i32,
                build_date: TmCell::new(0),
                document_id: -1,
                root_atomic_part_id: -1,
                atomic_part_ids: Vec::new(),
                base_assembly_ids: Vec::new(),
                valid: TmCell::new(0),
                ap_count: TmCell::new(0),
            });
        }
        let capacity_ap = MAX_AP * 2;
        while atomic_parts.len() < capacity_ap {
            atomic_parts.push(AtomicPart {
                id: atomic_parts.len() as i32,
                x: TmCell::new(0), y: TmCell::new(0), z: TmCell::new(0),
                build_date: TmCell::new(0), weight: TmCell::new(0),
                composite_part_id: -1, connection_ids: Vec::new(),
                valid: TmCell::new(0),
            });
        }
        let capacity_conn = MAX_CONN * 2;
        while connections.len() < capacity_conn {
            connections.push(Connection {
                id: connections.len() as i32,
                from_atomic_part_id: -1, to_atomic_part_id: -1,
                conn_type: TmCell::new(0), valid: TmCell::new(0),
            });
        }
        while documents.len() < MAX_DOCS + MAX_CP {
            documents.push(Document {
                id: documents.len() as i32,
                doc_type: TmCell::new(0), build_date: TmCell::new(0),
                composite_part_id: -1,
            });
        }

        eprintln!(
            "Init: {} CA, {} BA, {} CP, {} AP, {} conn, {} docs",
            complex_assemblies.len(),
            base_assemblies.len(),
            composite_parts.len(),
            atomic_parts.len(),
            connections.len(),
            documents.len(),
        );

        Database {
            modules,
            complex_assemblies,
            base_assemblies,
            composite_parts,
            atomic_parts,
            connections,
            documents,
            cp_by_date,
            next_cp_slot: AtomicUsize::new(MAX_CP),
            next_ap_slot: AtomicUsize::new(MAX_AP),
            next_conn_slot: AtomicUsize::new(MAX_CONN),
            next_ba_slot: AtomicUsize::new(MAX_BA),
        }
    }
}

// ==================================================================
// OPERATIONS
// ==================================================================

// ── Long traversals (§3) ────────────────────────────────────────

fn op_lt1(db: &Database, tx: &Transaction) -> i64 {
    let mut sum = 0i64;
    for ca in &db.complex_assemblies {
        sum += ca.id as i64 + ca.level as i64 + tx.read(&ca.build_date) as i64;
    }
    for ba in &db.base_assemblies {
        if tx.read(&ba.valid) != 0 {
            sum += ba.id as i64 + tx.read(&ba.build_date) as i64;
        }
    }
    sum
}

fn op_lt2(db: &Database, tx: &Transaction) {
    for ca in &db.complex_assemblies {
        let d = tx.read(&ca.build_date);
        tx.write(&ca.build_date, (d + 1) % 365 + 1000);
    }
    for ba in &db.base_assemblies {
        if tx.read(&ba.valid) != 0 {
            let d = tx.read(&ba.build_date);
            tx.write(&ba.build_date, (d + 1) % 365 + 1000);
        }
    }
}

fn op_lt3(db: &Database, tx: &Transaction) -> i64 {
    let mut sum = 0i64;
    for cp in &db.composite_parts {
        if tx.read(&cp.valid) != 0 {
            sum += cp.id as i64 + tx.read(&cp.build_date) as i64;
        }
    }
    sum
}

fn op_lt4(db: &Database, tx: &Transaction) -> i64 {
    let mut sum = 0i64;
    for ap in &db.atomic_parts {
        if tx.read(&ap.valid) != 0 {
            let w = (tx.read(&ap.weight) % 50) + 1;
            tx.write(&ap.weight, w);
            sum += w as i64;
        }
    }
    sum
}

fn op_lt5(db: &Database, tx: &Transaction) -> i64 {
    let mut sum = 0i64;
    for c in &db.connections {
        if tx.read(&c.valid) != 0 {
            sum += c.id as i64 + c.from_atomic_part_id as i64 + c.to_atomic_part_id as i64 + tx.read(&c.conn_type) as i64;
        }
    }
    sum
}

// ── Short traversals (§3) ───────────────────────────────────────

fn op_st1(db: &Database, tx: &Transaction) -> i64 {
    let mut sum = 0i64;
    if db.modules.is_empty() { return 0; }
    let mut ci = db.modules[0].root_assembly_id as usize;
    if ci < db.complex_assemblies.len() {
        sum += db.complex_assemblies[ci].id as i64;
    }
    for _l in 0..TREE_LEVELS - 1 {
        if ci >= db.complex_assemblies.len() || db.complex_assemblies[ci].child_assembly_ids.is_empty() { break; }
        ci = db.complex_assemblies[ci].child_assembly_ids[0];
        sum += db.complex_assemblies[ci].id as i64;
    }
    if ci < db.complex_assemblies.len() && !db.complex_assemblies[ci].child_base_assembly_ids.is_empty() {
        let bi = db.complex_assemblies[ci].child_base_assembly_ids[0];
        if bi < db.base_assemblies.len() && tx.read(&db.base_assemblies[bi].valid) != 0 {
            sum += tx.read(&db.base_assemblies[bi].build_date) as i64;
            if !db.base_assemblies[bi].composite_part_ids.is_empty() {
                let cpi = db.base_assemblies[bi].composite_part_ids[0];
                if cpi < db.composite_parts.len() && tx.read(&db.composite_parts[cpi].valid) != 0 {
                    sum += tx.read(&db.composite_parts[cpi].build_date) as i64;
                }
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
        for &ai in &db.composite_parts[ci].atomic_part_ids {
            if ai < db.atomic_parts.len() && tx.read(&db.atomic_parts[ai].valid) != 0 {
                sum += tx.read(&db.atomic_parts[ai].weight) as i64;
            }
        }
    }
    sum
}

fn op_st3(db: &Database, tx: &Transaction, ap_idx: usize) -> i64 {
    if ap_idx >= db.atomic_parts.len() { return 0; }
    if tx.read(&db.atomic_parts[ap_idx].valid) == 0 { return 0; }
    let mut sum = (tx.read(&db.atomic_parts[ap_idx].x) + tx.read(&db.atomic_parts[ap_idx].y) + tx.read(&db.atomic_parts[ap_idx].z)) as i64;
    for &cid in &db.atomic_parts[ap_idx].connection_ids {
        if cid < db.connections.len() && tx.read(&db.connections[cid].valid) != 0 {
            let nb = if db.connections[cid].from_atomic_part_id == ap_idx as i32 {
                db.connections[cid].to_atomic_part_id as usize
            } else {
                db.connections[cid].from_atomic_part_id as usize
            };
            if nb < db.atomic_parts.len() && tx.read(&db.atomic_parts[nb].valid) != 0 {
                sum += tx.read(&db.atomic_parts[nb].weight) as i64;
            }
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
    let mut cnt = 0i64;
    for &(d, _ci) in &db.cp_by_date {
        if d >= low && d <= high {
            cnt += 1;
        }
    }
    cnt
}

fn op_st6(db: &Database, tx: &Transaction, ap_idx: usize) {
    if ap_idx < db.atomic_parts.len() && tx.read(&db.atomic_parts[ap_idx].valid) != 0 {
        let x = tx.read(&db.atomic_parts[ap_idx].x);
        tx.write(&db.atomic_parts[ap_idx].x, (x + 1) % 100);
        let y = tx.read(&db.atomic_parts[ap_idx].y);
        tx.write(&db.atomic_parts[ap_idx].y, (y + 1) % 100);
    }
}

fn op_st7(db: &Database, tx: &Transaction, cp_idx: usize) -> i32 {
    if cp_idx >= db.composite_parts.len() || tx.read(&db.composite_parts[cp_idx].valid) == 0 { return 0; }
    let mut max_w = 0i32;
    for &ai in &db.composite_parts[cp_idx].atomic_part_ids {
        if ai < db.atomic_parts.len() && tx.read(&db.atomic_parts[ai].valid) != 0 {
            max_w = max_w.max(tx.read(&db.atomic_parts[ai].weight));
        }
    }
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
    for &ai in &db.composite_parts[cp_idx].atomic_part_ids {
        if ai < db.atomic_parts.len() && tx.read(&db.atomic_parts[ai].valid) != 0 {
            sum += tx.read(&db.atomic_parts[ai].weight) as i64;
        }
    }
    sum
}

fn op_st10(db: &Database, tx: &Transaction, cp_idx: usize) {
    if cp_idx >= db.composite_parts.len() || tx.read(&db.composite_parts[cp_idx].valid) == 0 { return; }
    let di = db.composite_parts[cp_idx].document_id as usize;
    if di < db.documents.len() {
        let d = tx.read(&db.documents[di].build_date);
        tx.write(&db.documents[di].build_date, (d + 1) % 365 + 1000);
    }
}

// ── Short operations (§3) ───────────────────────────────────────

fn op_op1(db: &Database, tx: &Transaction, id: usize) -> i64 {
    if id < db.atomic_parts.len() && tx.read(&db.atomic_parts[id].valid) != 0 {
        (tx.read(&db.atomic_parts[id].x) + tx.read(&db.atomic_parts[id].y) + tx.read(&db.atomic_parts[id].z)) as i64
    } else { 0 }
}

fn op_op2(db: &Database, tx: &Transaction, id: usize) -> i64 {
    if id < db.composite_parts.len() && tx.read(&db.composite_parts[id].valid) != 0 {
        tx.read(&db.composite_parts[id].build_date) as i64
    } else { 0 }
}

fn op_op3(db: &Database, tx: &Transaction, id: usize) -> i64 {
    if id < db.documents.len() {
        (tx.read(&db.documents[id].build_date) + tx.read(&db.documents[id].doc_type)) as i64
    } else { 0 }
}

fn op_op4(db: &Database, tx: &Transaction, id: usize) -> i64 {
    if id < db.base_assemblies.len() && tx.read(&db.base_assemblies[id].valid) != 0 {
        tx.read(&db.base_assemblies[id].build_date) as i64
    } else { 0 }
}

fn op_op5(db: &Database, tx: &Transaction, id: usize) -> i64 {
    if id < db.complex_assemblies.len() {
        tx.read(&db.complex_assemblies[id].build_date) as i64
    } else { 0 }
}

fn op_op6(db: &Database, tx: &Transaction, ap_idx: usize) -> i64 {
    if ap_idx < db.atomic_parts.len() && tx.read(&db.atomic_parts[ap_idx].valid) != 0 {
        (tx.read(&db.atomic_parts[ap_idx].x) + tx.read(&db.atomic_parts[ap_idx].y) + tx.read(&db.atomic_parts[ap_idx].z)) as i64
    } else { 0 }
}

fn op_op7(db: &Database, tx: &Transaction, cp_idx: usize) -> i64 {
    if cp_idx < db.composite_parts.len() && tx.read(&db.composite_parts[cp_idx].valid) != 0 {
        tx.read(&db.composite_parts[cp_idx].build_date) as i64
    } else { 0 }
}

fn op_op8(db: &Database, _tx: &Transaction, _cp_idx: usize) -> i64 { 1 }

fn op_op9(db: &Database, tx: &Transaction, cp_idx: usize) -> i64 {
    if cp_idx >= db.composite_parts.len() || tx.read(&db.composite_parts[cp_idx].valid) == 0 { return 0; }
    let mut sum = 0i64;
    for &ai in &db.composite_parts[cp_idx].atomic_part_ids {
        if ai < db.atomic_parts.len() && tx.read(&db.atomic_parts[ai].valid) != 0 {
            sum += tx.read(&db.atomic_parts[ai].weight) as i64;
        }
    }
    sum
}

fn op_op10(db: &Database, tx: &Transaction, ap_idx: usize) -> usize {
    if ap_idx < db.atomic_parts.len() && tx.read(&db.atomic_parts[ap_idx].valid) != 0 {
        db.atomic_parts[ap_idx].connection_ids.len()
    } else { 0 }
}

fn op_op11(db: &Database, tx: &Transaction, ap_idx: usize, nx: i32, ny: i32) {
    if ap_idx < db.atomic_parts.len() && tx.read(&db.atomic_parts[ap_idx].valid) != 0 {
        tx.write(&db.atomic_parts[ap_idx].x, nx);
        tx.write(&db.atomic_parts[ap_idx].y, ny);
    }
}

fn op_op12(db: &Database, tx: &Transaction, ap_idx: usize, nw: i32) {
    if ap_idx < db.atomic_parts.len() && tx.read(&db.atomic_parts[ap_idx].valid) != 0 {
        tx.write(&db.atomic_parts[ap_idx].weight, nw);
    }
}

fn op_op13(db: &Database, tx: &Transaction, doc_idx: usize, nd: i32) {
    if doc_idx < db.documents.len() {
        tx.write(&db.documents[doc_idx].build_date, nd);
    }
}

fn op_op14(db: &Database, tx: &Transaction, cp_idx: usize, nd: i32) {
    if cp_idx < db.composite_parts.len() && tx.read(&db.composite_parts[cp_idx].valid) != 0 {
        tx.write(&db.composite_parts[cp_idx].build_date, nd);
    }
}

fn op_op15(db: &Database, tx: &Transaction, ba_idx: usize, nd: i32) {
    if ba_idx < db.base_assemblies.len() && tx.read(&db.base_assemblies[ba_idx].valid) != 0 {
        tx.write(&db.base_assemblies[ba_idx].build_date, nd);
    }
}

// ── Structure modifications (§3) ────────────────────────────────

fn op_sm1(db: &Database, tx: &Transaction, new_id: i32) {
    let slot = db.next_cp_slot.fetch_add(1, Ordering::Relaxed);
    let doc_slot = slot; // one doc per CP
    if slot >= db.composite_parts.len() { return; }
    let cp = &db.composite_parts[slot];
    tx.write(&cp.valid, 1);
    tx.write(&cp.build_date, 2000);
    // Create 200 APs + connections (simplified: write counters)
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
    if ci < db.composite_parts.len() {
        tx.write(&db.composite_parts[ci].valid, 0);
    }
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
    if ai < db.atomic_parts.len() {
        tx.write(&db.atomic_parts[ai].valid, 0);
    }
}

fn op_sm5(db: &Database, tx: &Transaction, from_ap: usize, to_ap: usize, typ: i32) {
    if from_ap >= db.atomic_parts.len() || to_ap >= db.atomic_parts.len() { return; }
    let slot = db.next_conn_slot.fetch_add(1, Ordering::Relaxed);
    if slot >= db.connections.len() { return; }
    tx.write(&db.connections[slot].valid, 1);
    tx.write(&db.connections[slot].conn_type, typ);
}

fn op_sm6(db: &Database, tx: &Transaction, ci: usize) {
    if ci < db.connections.len() {
        tx.write(&db.connections[ci].valid, 0);
    }
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
    if bi < db.base_assemblies.len() {
        tx.write(&db.base_assemblies[bi].valid, 0);
    }
}

// ── Operation dispatch ──────────────────────────────────────────

enum OpClass { LongTrav, ShortTrav, ShortOp, StructMod }

struct OpDesc {
    cat: OpClass,
    is_read: bool,
}

fn pick_operation(rng: &mut Xsrng, write_percent: i32) -> OpDesc {
    let r = rng.range(0, 100);
    let cat = if r < 5 { OpClass::LongTrav }
    else if r < 45 { OpClass::ShortTrav }
    else if r < 90 { OpClass::ShortOp }
    else { OpClass::StructMod };

    match cat {
        OpClass::StructMod => OpDesc { cat, is_read: false },
        _ => {
            let want_read = (rng.range(0, 100) as i32) < (100 - write_percent);
            OpDesc { cat, is_read: want_read }
        }
    }
}

fn execute_op(db: &Database, tx: &Transaction, desc: &OpDesc, rng: &mut Xsrng) {
    match desc.cat {
        OpClass::LongTrav => {
            if desc.is_read {
                match rng.range(0, 3) {
                    0 => { op_lt1(db, tx); }
                    1 => { op_lt3(db, tx); }
                    _ => { op_lt5(db, tx); }
                }
            } else {
                match rng.range(0, 2) {
                    0 => { op_lt2(db, tx); }
                    _ => { op_lt4(db, tx); }
                }
            }
        }
        OpClass::ShortTrav => {
            if desc.is_read {
                match rng.range(0, 5) {
                    0 => { let di = rng.range(0, db.documents.len()); op_st2(db, tx, di); }
                    1 => { let ai = rng.range(0, db.atomic_parts.len()); op_st3(db, tx, ai); }
                    2 => {
                        let lo = 1000 + rng.range(0, 100) as i32;
                        let hi = 1000 + 200 + rng.range(0, 100) as i32;
                        op_st5(db, tx, lo, hi);
                    }
                    3 => { let ci = rng.range(0, db.composite_parts.len()); op_st7(db, tx, ci); }
                    _ => { let ci = rng.range(0, db.composite_parts.len()); op_st9(db, tx, ci); }
                }
            } else {
                match rng.range(0, 5) {
                    0 => { let ci = rng.range(0, db.complex_assemblies.len()); op_st4(db, tx, ci); }
                    1 => { let ai = rng.range(0, db.atomic_parts.len()); op_st6(db, tx, ai); }
                    2 => { let bi = rng.range(0, db.base_assemblies.len()); op_st8(db, tx, bi); }
                    3 => { let ci = rng.range(0, db.composite_parts.len()); op_st10(db, tx, ci); }
                    _ => { op_st1(db, tx); }
                }
            }
        }
        OpClass::ShortOp => {
            if desc.is_read {
                match rng.range(0, 10) {
                    0 => { op_op1(db, tx, rng.range(0, MAX_AP)); }
                    1 => { op_op2(db, tx, rng.range(0, MAX_CP)); }
                    2 => { op_op3(db, tx, rng.range(0, db.documents.len())); }
                    3 => { op_op4(db, tx, rng.range(0, MAX_BA)); }
                    4 => { op_op5(db, tx, rng.range(0, MAX_CA)); }
                    5 => { op_op6(db, tx, rng.range(0, db.atomic_parts.len())); }
                    6 => { op_op7(db, tx, rng.range(0, db.composite_parts.len())); }
                    7 => { op_op8(db, tx, rng.range(0, db.composite_parts.len())); }
                    8 => { op_op9(db, tx, rng.range(0, db.composite_parts.len())); }
                    _ => { op_op10(db, tx, rng.range(0, db.atomic_parts.len())); }
                }
            } else {
                match rng.range(0, 5) {
                    0 => {
                        let ai = rng.range(0, db.atomic_parts.len());
                        op_op11(db, tx, ai, rng.range_i32(0, 100), rng.range_i32(0, 100));
                    }
                    1 => {
                        let ai = rng.range(0, db.atomic_parts.len());
                        op_op12(db, tx, ai, rng.range_i32(1, 51));
                    }
                    2 => {
                        let di = rng.range(0, db.documents.len());
                        op_op13(db, tx, di, 1000 + rng.range_i32(0, 365));
                    }
                    3 => {
                        let ci = rng.range(0, db.composite_parts.len());
                        op_op14(db, tx, ci, 1000 + rng.range_i32(0, 365));
                    }
                    _ => {
                        let bi = rng.range(0, db.base_assemblies.len());
                        op_op15(db, tx, bi, 1000 + rng.range_i32(0, 365));
                    }
                }
            }
        }
        OpClass::StructMod => {
            match rng.range(0, 8) {
                0 => { let nid = (MAX_CP + rng.range(0, 1000)) as i32; op_sm1(db, tx, nid); }
                1 => { let ci = rng.range(0, db.composite_parts.len()); op_sm2(db, tx, ci); }
                2 => { let ci = rng.range(0, db.composite_parts.len()); op_sm3(db, tx, ci); }
                3 => { let ai = rng.range(0, db.atomic_parts.len()); op_sm4(db, tx, ai); }
                4 => {
                    let a = rng.range(0, db.atomic_parts.len());
                    let b = rng.range(0, db.atomic_parts.len());
                    op_sm5(db, tx, a, b, rng.range_i32(0, 3));
                }
                5 => { let ci = rng.range(0, db.connections.len()); op_sm6(db, tx, ci); }
                6 => {
                    // Find a leaf CA (level TREE_LEVELS-1)
                    for _ in 0..100 {
                        let idx = rng.range(0, db.complex_assemblies.len());
                        if idx < db.complex_assemblies.len() && db.complex_assemblies[idx].level as usize == TREE_LEVELS - 1 {
                            op_sm7(db, tx, idx);
                            return;
                        }
                    }
                }
                _ => { let bi = rng.range(0, db.base_assemblies.len()); op_sm8(db, tx, bi); }
            }
        }
    }
}

// ── Worker ──────────────────────────────────────────────────────

fn worker(tid: usize, db: &Database, stop: &AtomicBool,
          total_ops: &AtomicUsize, write_percent: i32) {
    let rng_cell = RefCell::new(Xsrng::new((tid as u64) * 12345 + 42 + 1));

    while !stop.load(Ordering::Relaxed) {
        let desc = pick_operation(&mut *rng_cell.borrow_mut(), write_percent);
        transaction(|tx| {
            execute_op(db, tx, &desc, &mut *rng_cell.borrow_mut());
        });
        total_ops.fetch_add(1, Ordering::Relaxed);
    }
}

// ── Main ────────────────────────────────────────────────────────

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let mut duration_ms = 10000u64;
    let mut nb_threads = 4usize;
    let mut workload = 1i32; // 1=read-dom, 2=read-write, 3=write-dom

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

    let write_percent = match workload {
        2 => 40,
        3 => 90,
        _ => 10,
    };

    println!("STMbench7 — Rust TM API (full 45-op spec)");
    println!("Workload: {} ({}% read, {}% write)", workload, 100 - write_percent, write_percent);
    println!("Duration: {} ms  Threads: {}", duration_ms, nb_threads);

    tm_init();

    let db = Arc::new(Database::new());
    println!("  CA: {}  BA: {}  CP: {}  AP: {}  Conn: {}  Docs: {}",
             db.complex_assemblies.len(), db.base_assemblies.len(),
             db.composite_parts.len(), db.atomic_parts.len(),
             db.connections.len(), db.documents.len());

    let stop = Arc::new(AtomicBool::new(false));
    let total_ops = Arc::new(AtomicUsize::new(0));

    let mut handles = Vec::with_capacity(nb_threads);
    for tid in 0..nb_threads {
        let db = Arc::clone(&db);
        let stop = Arc::clone(&stop);
        let ops = Arc::clone(&total_ops);
        handles.push(std::thread::spawn(move || {
            tm_init_thread();
            worker(tid, &db, &stop, &ops, write_percent);
            tm_exit_thread();
        }));
    }

    std::thread::sleep(Duration::from_millis(duration_ms));
    stop.store(true, Ordering::Relaxed);

    for h in handles { h.join().unwrap(); }

    let total = total_ops.load(Ordering::Relaxed);
    let elapsed = duration_ms as f64 / 1000.0;

    let aborts = tm::tm_abort_count();

    println!("\nResults");
    println!("=======");
    println!("Elapsed:   {:.1}s", elapsed);
    println!("Total ops: {}", total);
    println!("Throughput: {:.0} txns/sec", total as f64 / elapsed);
    println!("TM aborts: {} ({:.1}%)", aborts, if total > 0 { aborts as f64 / total as f64 * 100.0 } else { 0.0 });

    tm_exit();
    println!("PASS");
}
