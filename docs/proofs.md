# Correctness Proofs for TM Backends

## Notation

- $V_i$: shared variable (memory location)
- $R_{V_i}^j$: read of $V_i$ returning value with logical timestamp $j$
- $W_{V_i}^j$: write of value $j$ to $V_i$
- $L$: global lock variable
- $T_{ID}$: current thread identifier, $T_{ID} > 0$
- $L \leftarrow 0$: lock is free; $L \leftarrow T_{ID}$: thread $T_{ID}$ holds the lock
- $C_{V_i, O, N}$: compare-and-swap on $V_i$, comparing $O$ and swapping to $N$; returns $\top$ (success) or $\bot$ (failure)
- $O \leftarrow^L V_i$: acquire-load of $V_i$ (C++ `memory_order_acquire`)
- $N \rightarrow^S V_i$: release-store of $N$ to $V_i$ (C++ `memory_order_release`)
- $F^S$: store fence (`atomic_thread_fence(memory_order_release)`)
- $F^L$: load fence (`atomic_thread_fence(memory_order_acquire)`)
- $\mathcal{H} = \{R_{V_{a_1}}^{j_1}, W_{V_{b_1}}^{j_2}, \dots\}$: a transaction, i.e. a sequence of reads/writes to shared variables
- $G$: global clock (monotonic, $\mathbb{N}_0$, even = unlocked)
- $\text{rs}(\mathcal{H})$: read set of $\mathcal{H}$ (set of addresses read)
- $\text{ws}(\mathcal{H})$: write set of $\mathcal{H}$ (set of addresses written)
- $\text{orec}(V)$: ownership record for address $V$ (contains version $v$, read-lock $r$, write-lock $w$)

---

## 1. Single Global Lock (SGL)

### Algorithm

Let $L$ be a mutual-exclusion lock, initially $L \leftarrow 0$.

```
transaction begin:
    while true:
        O ←ᴸ L
        if O ≠ 0: continue
        if C_{L, 0, T_ID}: break            // acquire lock
    Fᴸ                                   // (implied by CAS)

read V_i:
    return V_i                            // plain load

write N to V_i:
    V_i ← N                               // plain store

transaction commit:
    Fˢ                                    // ensure stores are visible
    0 →ˢ L                                // release lock
```

### 1.1 Mutual Exclusion

**Claim**: At most one thread holds $L$ at any time.

**Proof**: The lock is acquired via $C_{L, 0, T_{ID}}$. CAS atomically checks $L = 0$ and sets $L = T_{ID}$. If two threads $T_1, T_2$ both attempt CAS, the hardware linearizes the two operations. One succeeds ($L = T_1$); the other sees $L \neq 0$ and loops. A thread releases $L$ by writing $0 \rightarrow^S L$, which only that thread does (since only it writes to $L$ while holding it). Hence $L$ is held by at most one thread at any instant. ∎

### 1.2 Serializability

**Claim**: Every execution of transactions under SGL is serializable in lock-acquire order.

**Proof**: Let $\mathcal{H}_i, \mathcal{H}_j$ be two transactions with lock-acquire times $t_i^{\text{acq}} < t_j^{\text{acq}}$. By mutual exclusion, $\mathcal{H}_i$ releases the lock at $t_i^{\text{rel}}$, and $\mathcal{H}_j$ acquires it after $t_i^{\text{rel}}$. All reads and writes of $\mathcal{H}_i$ occur between $t_i^{\text{acq}}$ and $t_i^{\text{rel}}$; all of $\mathcal{H}_j$ occur between $t_j^{\text{acq}}$ and $t_j^{\text{rel}}$, with $t_i^{\text{rel}} < t_j^{\text{acq}}$. Therefore the operations of $\mathcal{H}_i$ are totally ordered before those of $\mathcal{H}_j$.

Construct a serial schedule $\sigma = \mathcal{H}_i, \mathcal{H}_j, \dots$ ordered by lock-acquire time. Every read in $\mathcal{H}_j$ sees the latest preceding write in $\sigma$ (the write that happened before $t_j^{\text{acq}}$), because the release-store of the previous transaction happens-before the acquire-load of $j$. This satisfies the definition of conflict-serializability. ∎

### 1.3 No Dirty Reads

**Claim**: No transaction reads a value written by an uncommitted (aborted or in-flight) transaction.

**Proof**: Under SGL, a transaction writes directly to shared variables only while holding $L$. It does not release $L$ until commit (the final store fence $F^S$ and $0 \rightarrow^S L$). Any other thread attempting to read the same variable cannot hold $L$ concurrently (mutual exclusion), so it must wait until $L$ is released. At that point the writing transaction has committed (its stores are visible via the release semantics). Therefore every read sees only committed values. ∎

---

## 2. TSX+SGL (TSXSGL)

### Algorithm

Shared state:
- $L$: mutex for SGL fallback
- $S$: lock owner variable, $S \in \{0\} \cup \mathbb{N}_0$ ($0$ = free, non-zero = locked by that thread)

Per-thread state:
- $\text{in\_tsx}$: boolean, true while inside a hardware transaction
- $S_{\text{start}}$: lock-owner snapshot at TSX begin

```
transaction begin:
    if nested: return
    // TSX attempt (up to 5 retries)
    for attempt = 1..5:
        status = _xbegin()
        if status == _XBEGIN_STARTED:
            s ←ᴸ S
            if s ≠ 0:                        // SGL active → abort
                _xabort(0xFF)
            S_start = s
            in_tsx = true
            return
        // Abort handling
        if explicit abort with code 0xFF:
            while S ≠ 0: pause()             // spin-wait for SGL to finish
        else if retry bit not set: break
    // Fallback: SGL
    L.lock()
    1 →ˢ S                                   // store non-zero (= locked)
    in_tsx = false

transaction commit:
    if nested: return
    if in_tsx:
        s ←ᴸ S
        if s ≠ S_start:                      // SGL interleaved
            _xabort(0x01)                    // must retry (double-check)
        _xend()
        return
    // SGL path
    0 →ˢ S                                   // store 0 (= free)
    L.unlock()
```

Read/write hooks: plain loads/stores — TSX or SGL provides isolation.

### 2.1 TSX Isolation

**Claim**: While $\text{in\_tsx} = \top$, every read returns the value at transaction start (or an atomic snapshot), and every write is buffered until commit.

**Proof**: This is a property of Intel TSX (RTM). The hardware maintains a read-set and write-set. Reads are served from memory but tracked; if another thread writes to a read-set cache line, the transaction aborts on `_xend()` or on the next access. Writes are buffered in the L1 cache and not visible to other threads until `_xend()`. This satisfies opacity for hardware transactions. ∎

### 2.2 Lock-Variable Serialization

**Claim**: The lock variable $S$ together with the hardware lock $L$ ensures that TSX and SGL transactions are serialized.

**Proof**:

**Key invariant**: $S = 0 \iff$ no SGL transaction is active. This holds because:
- SGL entry: acquire $L$ first, then store $1$ to $S$ (non-zero).
- SGL exit: store $0$ to $S$, then release $L$.
- TSX actions never modify $S$; they only read it.

**SGL-SGL**: The lock $L$ is a standard mutex. Mutual exclusion ensures SGL transactions are serialized.

**TSX-TSX**: Two concurrent TSX transactions $\mathcal{H}_i, \mathcal{H}_j$ must conflict on at least one shared address to be non-serializable. If they access disjoint addresses, they can commit concurrently without conflict (hardware allows this). If they access the same address with at least one write, TSX detects the conflict and one aborts. The retry mechanism eventually serializes them.

**TSX-SGL**: Suppose $\mathcal{H}_{\text{TSX}}$ begins at $t_0$ and $\mathcal{H}_{\text{SGL}}$ runs between $t_1$ and $t_2$ with $t_0 < t_1 < t_2$.

1. At $t_0$, $\mathcal{H}_{\text{TSX}}$ reads $S = S_{\text{start}}$. If $S_{\text{start}} \neq 0$, $\mathcal{H}_{\text{TSX}}$ aborts immediately (line `if s ≠ 0: _xabort(0xFF)`). No TSX runs while $S$ is held.
2. At $t_1$, $\mathcal{H}_{\text{SGL}}$ stores $1$ to $S$. Since $S$ is in $\mathcal{H}_{\text{TSX}}$'s read-set (the cache line was loaded at $t_0$), the MESI protocol sends an invalidation to $\mathcal{H}_{\text{TSX}}$'s core, aborting the hardware transaction immediately.
3. The `_xend()` check $s \neq S_{\text{start}}$ is a safety double-check for micro-architectural races. In practice it never fires ($S$ was written at $t_1$, so the TSX aborted then and never reaches `_xend()`).

Therefore no TSX transaction commits while an SGL transaction is active, and no SGL transaction runs while a TSX transaction could read stale data. The serialization order places committed TSX transactions between consecutive SGL transactions, ordered by their begin time. ∎

### 2.3 Opacity for TSX

**Claim**: TSXSGL provides opacity: every transaction (TSX or SGL) reads values that are consistent with a prefix of some serial history.

**Proof**: SGL transactions are trivially opaque (mutual exclusion). For a TSX transaction that commits, the `_xend()` re-check ensures that no SGL has interleaved; all values read by the TSX are from a state between the last SGL commit and the next SGL begin. If a TSX transaction aborts (due to SGL interleaving, hardware conflict, or lock-owner change), none of its writes become visible, and by the abort-gate property of TSX, the reads did not affect any other thread. Hence the transaction can be considered never to have executed. This satisfies opacity. ∎

---

## 3. TL2

### Algorithm (Dice, Shalev, Shavit, 2006)

Shared state:
- $G$: global version clock, $\mathbb{N}^+$, monotonically increasing
- $\text{guard}[V_i]$: per-address guard word: bit 0 = lock flag, bits $1\dots n$ = version number

Per-thread state:
- $s$: snapshot version (read from $G$ at begin)
- $\mathcal{R}$: read set — entries are $(V_i, \text{version observed})$
- $\mathcal{W}$: write set — entries are $(V_i, \text{new value})$

```
transaction begin:
    s ← G
    active = true

read V_i:
    if V_i in W: return W[V_i].new_value      // own buffered write
    guard_val ← guard[V_i]
    version ← (guard_val >> 1)                 // version without lock bit
    locked ← guard_val & 1
    add (V_i, version) to R
    return *V_i                                 // plain load

write N to V_i:
    W[V_i].new_value ← N
    if not already in W: W[V_i].old_value ← *V_i

transaction commit:
    if W is empty: return                       // read-only
    // Phase 1: acquire write-set locks
    for each (V_i, _) in sorted(W):
        while not try_acquire_guard(V_i):
            // retry until lock acquired
    // Phase 2: increment clock
    c ← I_G                                     // commit version
    // Phase 3: validate read-set
    for each (V_i, v) in R:
        if (guard[V_i] >> 1) ≠ v:
            release all locks; abort
    // Phase 4: write-back
    for each (V_i, _) in W:
        *V_i ← W[V_i].new_value
    // Phase 5: release locks with version
    for each (V_i, _) in W:
        guard[V_i] ← (c << 1)                   // store version, unlock
```

### 3.1 Serializability

**Claim**: TL2 produces serializable histories.

**Proof** (standard TL2 proof):
1. **Commit order**: The global clock $G$ provides a total order of committed transactions. The commit version $c = I_G$ is unique per transaction.
2. **Read validation**: Before writing back, every read-set entry is checked: if any guard's version differs from the observed version, some concurrent writer committed between the read and the commit, and the transaction aborts. This ensures linearizability at the commit point: all reads are still valid at $c$.
3. **Lock ordering**: Locks are acquired in sorted address order, preventing deadlock.
4. **Write atomicity**: Locks are held during write-back (Phase 4) and released only after all writes are visible. No other transaction can read partial updates because a reader sees the locked flag and spins (or in the implementation, reads the version atomically with a double-check pattern).
5. **Serialization order**: Serialize committed transactions in order of increasing $c$. For any transaction $\mathcal{H}$ with commit version $c$, its read-set is consistent with the state after all transactions with $c' < c$ have committed, because validation passed against version numbers ≤ $c-1$. Its writes become visible atomically at $c$. ∎

### 3.2 No Dirty Reads

**Claim**: No transaction reads uncommitted data.

**Proof**: Writes are buffered until commit (write-behind). During write-back, the guard's lock bit is set. A concurrent reader doing the double-check read (read guard → read value → read guard again) will see either:
- The lock bit set → retry (spin until lock released), or
- Version changed → abort on next validation.
After the writer releases the guard, the version is $c$, which is ≥ any snapshot in flight; the reader will abort on the next validation. No reader sees partial updates. ∎

---

## 4. TinySTM — Design Variants

All three TinySTM variants share:
- Global clock $C$ (monotonically increasing)
- Per-address lock table with lock bit + version + (for WT) incarnation number
- Per-thread read set $\mathcal{R}$ and write set $\mathcal{W}$

### 4.1 WBCTL (Write-Back Commit-Time Locking)

Algorithm:
```
begin():
    C_start ← C, C_end ← C_start

read V_i:
    if V_i in W: return W[V_i]
    repeat:
        lock = get_lock(V_i)
        if lock locked by other: retry
        ν ← lock.version, value ← *V_i
        if ν ≠ lock.version: retry
        if ν > C_end: if !extend(): abort_tx(); retry
    add (V_i, ν, value) to R

write N to V_i:
    W[V_i] ← N    (buffered, no lock)

commit():
    if W empty: return
    for each V in sorted(W): try_lock(get_lock(V))
    C_commit ← I_C
    if C_commit ≤ C_end: abort
    for each (V, N) in W: *V ← N          // write-back
    for each V in sorted(W): unlock(V, C_commit)
```

**Proof**: Locks are acquired in sorted address order at commit time (deadlock-free). After lock acquisition, the clock is incremented and validation (`extend()`) checks that no read-set entry has a version greater than $C_{\text{end}}$. If validation fails, the transaction aborts. Write-back happens under lock protection; locks are released only after all writes are visible. Serialization order is commit-timestamp ($C_{\text{commit}}$) order. This is the standard commit-time locking proof from Section 3, simplified by sorted lock acquisition. ∎

### 4.2 WBETL (Write-Back Encounter-Time Locking)

Algorithm:
```
begin():
    C_start ← C, C_end ← C_start

read V_i:
    if V_i in W: return W[V_i]
    // same double-check read protocol as WBCTL
    ...

write N to V_i:
    // Lock acquired on first WRITE encounter (not at commit)
    lock = get_lock(V_i)
    if lock locked by other and not self:
        if !validate(): abort_tx()
        retry
    try_lock(lock) until success
    W[V_i] ← N

commit():
    if W empty: return
    C_commit ← I_C
    if !extend(): abort                          // validate read-set
    for each (V, N) in W: *V ← N                // write-back
    for each V in W: unlock(V, C_commit)
```

**Difference from WBCTL**: Locks are acquired eagerly at write time, not deferred to commit. This provides earlier write-write conflict detection (a second writer to the same address will find the lock held and abort). The commit path is simpler: locks are already held, so only validation, write-back, and versioned unlock are needed.

**Proof**: The serialization argument is the same as WBCTL — locks are acquired before the commit point, and write-back happens under lock. The difference is that lock acquisition may cause earlier aborts (eager conflict detection), but correctness follows from the same commit-timestamp serialization. ∎

### 4.3 WT (Write-Through)

Algorithm:
```
begin():
    C_start ← C, tx.active = true

read V_i:
    if V_i in W: return W[V_i]
    if self-locked: return *V_i
    repeat:
        lock = get_lock(V_i)
        while lock locked by other: retry
        value ← *V_i
        if lock changed: retry
        if version > tx.end_version:
            if read_set valid: extend(); else abort
    add to R

write N to V_i:
    if previously unwritten:
        W[V_i].old_word ← *V_i                // undo log
        lock = get_lock(V_i)
        try_lock(lock) until success
        tx.locks_held.push(lock)
    W[V_i].new_word ← N
    *V_i ← N                                   // WRITE-THROUGH to memory

commit():
    if read_only or W empty: return
    C_commit ← I_C
    for each (V, _) in R:
        if get_lock(V).version ≠ observed_version: abort
    for each (V, _) in W:
        unlock(V, C_commit)

abort():
    for each (V, _) in W: *V ← W[V].old_word  // undo
    release all locks
```

**Key difference**: Writes go directly to memory (write-through), not buffered. An undo log records old values for rollback. This means a concurrent reader can see intermediate values (if the lock is set, the reader spins; if the lock is not yet set... but the lock IS set before the write-through). The lock bit prevents dirty reads.

**Proof sketch**:
1. **Write atomicity**: Before writing through to memory, the lock is acquired and set. Any concurrent reader sees the lock bit, and either spins (if locked by another) or reads directly (if locked by self). No reader sees a partially written address without the lock indicating it is in flight.
2. **Commit**: The clock is incremented, read-set is validated, then locks are released with the new version. If validation fails, all writes are undone via the undo log and locks are released — the state is restored.
3. **Serialization**: Commit-timestamp order, as in WBCTL.
4. **No dirty reads**: A reader may see write-through values while the lock is held, but the lock bit signals that the data is provisional. The reader spins until the lock is released (at commit) and then sees committed data, OR the reader version-checks and aborts. ∎

---

## 5. SwissTM

### Algorithm (Dragojevic, Guerraoui, Kapalka, 2009)

Shared state:
- $\text{orec}[V_i]$: ownership record per address range:
  - $r$: read-lock/version (even = unlocked, odd = locked for read)
  - $w$: write-lock (0 = unlocked, else pointer to WriteLogEntry)
- $G$: global commit timestamp (monotonic)

Per-thread state:
- $\text{valid\_ts}$: highest version known to be consistent (extended on validation)
- $\text{cm\_ts}$: contention management timestamp
- $\mathcal{R}$: read log — entries are $(V_i, \text{orec}, \text{version})$
- $\mathcal{W}$: write log — entries are $(V_i, \text{orec}, \text{old\_value}, \text{new\_value})$

```
read V_i:
    if V_i in W: return W[V_i].new_value
    // Check for eager write lock held by self
    if orec[V_i].w points to self's log entry:
        return self's buffered value
    // Lock-free read protocol
    repeat:
        version ← orec[V_i].r
        if version == READ_LOCKED: continue
        value ← *V_i
        if orec[V_i].r ≠ version: continue      // concurrent modification
    add (V_i, orec, version) to R
    if version > valid_ts AND !extend(): rollback
    return value

write N to V_i:
    if already in W: update and return
    // Eager lock acquisition
    repeat:
        if orec[V_i].w is locked by another:
            if cm_should_abort(): rollback
            continue
        allocate WriteLogEntry(V_i, orec, old=*V_i, new=N)
        CAS(orec[V_i].w, 0 → &entry)
    // Check if version advanced
    if orec[V_i].r > valid_ts AND !extend(): rollback

commit():
    if W empty: return
    // Phase 1: acquire read-locks on read-set
    for each (V_i, orec, v) in R:
        orec.r ← READ_LOCKED
    // Phase 2: increment commit timestamp
    ts ← I_G
    // Phase 3: validate read-set under read locks
    for each (V_i, orec, v) in R:
        if orec.r ≠ v AND not self-locked:
            rollback
    // Phase 4: write-back
    for each (V_i, _, new) in W:
        *V_i ← new
    // Phase 5: release all locks with new version
    for each (V_i, orec, _) in W:
        orec.r ← ts, orec.w ← UNLOCKED
```

### 5.1 Eager Write-Write, Lazy Read-Write

SwissTM's distinctive design:
- **Write-write conflicts**: Detected eagerly at write time. When a thread writes to $V_i$, it sets `orec[V_i].w` via CAS. A second writer to the same address finds the lock held and either spins or aborts based on the contention manager.
- **Read-write conflicts**: Detected lazily at commit time. The reader takes a versioned snapshot (reads `orec.r`). The writer locks `orec.w` but does not modify `orec.r` until commit. At commit, the writer acquires the read-lock (`orec.r ← READ_LOCKED`) to force concurrent readers to abort on their next validation.

### 5.2 Serializability

**Proof sketch**:
1. **Commit serialization**: The global timestamp $G$ provides a total order of committed transactions. The commit point is the `I_G` at line 8 of commit.
2. **Read-lock protocol**: At commit, SwissTM sets `orec.r ← READ_LOCKED` for every address in the read set. This ensures that no concurrent transaction can commit with a conflicting write to the same address between validation and write-back. If another transaction holds `orec.w` for the same address, the read-lock CAS behaves as a lock that prevents concurrent reads and commits.
3. **Validation**: After reading all read-set orecs under read-locks, SwissTM checks that each orec's version still matches the observed version. If any mismatch, the transaction aborts.
4. **Write atomicity**: Write-back happens with all read-locks and write-locks held. No other transaction can observe partial writes.
5. **Contention manager**: The `cm_should_abort()` function ensures progress (avoids live-lock) by comparing `cm_ts` values, with random exponential backoff on rollback. This does not affect correctness. ∎

---

## 6. NOrec

### Algorithm (Dice, Shavit, 2006)

Shared state:
- $G$: global clock/commit lock (even = free/incremented, odd = held)

Per-thread state:
- $s$: snapshot version
- $\mathcal{R}$: read set — entries are $(V_i, \text{value}, s)$
- $\mathcal{W}$: write set — entries are $(V_i, \text{new value})$

```
begin():
    do: s ← G; while s & 1       // spin until even (lock free)
    active, read_only ← true

read V_i:
    if V_i in W: return W[V_i].new_value
    do:
        value ← *V_i
        while s ≠ G:
            validate():                    // re-read ALL of R
                for each (V, v, _) in R:
                    if *V ≠ v: abort_tx()
                s ← G - (G & 1)            // round to even
            if s ≠ snapshot:               // clock changed again
                value ← *V_i               // re-read from memory
    add (V_i, value, s) to R
    return value

write N to V_i:
    read_only ← false
    add (V_i, N) to W

commit():
    if read_only: return
    while true:
        expect ← s
        desire ← s + 1                     // odd = locked version
        if C_{G, expect, desire}: break    // CAS acquire
        validate():                        // clock changed, re-check R
            s ← ...                        // update snapshot
    for each (V_i, N) in W: *V_i ← N       // write-back
    G ← s + 2                               // release: even, new version
```

### 6.1 Key Insight

NOrec uses **value-based validation** instead of version-based validation. Rather than maintaining per-address version numbers, it re-reads each entry in the read set and compares against the buffered value. If all match, the read set is still consistent.

### 6.2 Serializability

**Proof sketch**:
1. **Snapshot isolation**: At begin, $s$ is set to $G$ (even). The snapshot is the state of memory at version $s$.
2. **Read consistency**: Every read is validated against $s$. If $G$ changes during the transaction (some other thread committed), `validate()` re-reads every entry in $\mathcal{R}$. If any value changed, the transaction aborts (dirty read would occur). If all values match, the snapshot advances to the new version.
3. **Commit lock**: The commit protocol uses CAS to atomically advance $G$ from $s$ (even) to $s+1$ (odd). This acquires a "commit lock." No two transactions can hold the commit lock simultaneously because CAS linearizes.
4. **Write-back under lock**: After CAS success, write-back happens while $G$ is odd (locked). $G$ is released to $s+2$ (even, new version) after write-back completes. Any concurrent transaction performing `validate()` sees odd $G$ and spins, ensuring it never reads partial write-back.
5. **Serialization order**: Committed transactions are serialized in order of increasing commit version (the even value after release). ∎

---

## 7. Summary of Correctness Arguments

| Backend | Serialization mechanism | Dirty read prevention | Scalability bottleneck |
|---------|----------------------|---------------------|----------------------|
| SGL | Mutual exclusion (lock) | Only one writer at a time | Single lock (O(1) throughput) |
| TSXSGL | TSX read-set isolation + lock-owner synchronisation | TSX abort on conflict; lock-owner re-check at commit | TSX abort rate under contention; SGL fallback |
| TL2 | Commit-time lock ordering + global clock + read validation | Locked guard during write-back; double-check read protocol | Guard table contention; O(τ) validation |
| TinySTM WBCTL | Commit-time lock ordering + global clock | Lock-based write-set protection + version validation | Lock table overhead; O($\|\mathcal{W}\| \log \|\mathcal{W}\|$) sorting |
| TinySTM WBETL | Encounter-time lock acquisition (eager) + global clock | Same as WBCTL; earlier conflict detection | Same lock table; eager locking may abort earlier |
| TinySTM WT | Write-through + undo log + encounter-time locks | Lock bit signals in-flight writes; readers spin | Write-through cache pressure; undo log cost |
| SwissTM | Eager write-write locks + lazy read-write validation + read-lock protocol | Read-locks at commit prevent concurrent readers; orec w-lock prevents concurrent writers | ORec table contention; two-phase commit overhead |
| NOrec | Value-based validation + single commit lock (CAS on global clock) | Re-reads read-set before commit; CAS prevents concurrent commits | Global commit lock; O(\|R\|) validation per clock change |

All backends provide **serializability** (conflict-serializable schedules equivalent to some serial execution) and **no dirty reads** (no transaction observes uncommitted intermediate state). The differences are in performance characteristics under contention.

---

## 8. Memory Allocation and Deallocation Correctness

### 8.1 Domain

Extend the model with heap operations:

- $\text{malloc}(n) \to p$: allocate $n$ bytes, return pointer $p$
- $\text{free}(p)$: deallocate the block at $p$
- $\text{realloc}(p, n)$: resize block at $p$ to $n$ bytes

Heap memory is **orthogonal** to TM-tracked shared variables. The TM manages
reads/writes to memory locations; malloc/free manage the lifetime of those
locations.  A block obtained from `malloc` is part of the TM's domain as soon
as a TM-tracked pointer points into it (i.e. the block is **reachable** from a
`TM`-annotated global).  Once the last TM-tracked pointer to the block is
overwritten, the block is no longer in the TM's domain.

### 8.2 Allocation Inside a Transaction

```
let p = malloc(n)   // p is fresh, not reachable from any TM global
*p = 42             // plain store — not a TM write (p was allocated inside TX)
q = tm_read_ptr(...) // some TM pointer
                    // p becomes TM-reachable only when assigned into q's data structure
```

**Lemma 8.1 (Allocation freshness)**: A pointer returned by `malloc` inside a
transaction does not alias any address currently in the TM's domain.

*Proof*: `malloc` returns a block from the process heap.  By definition, the
block is not part of any existing data structure (it is uninitialized).  The TM
plugin's static pointer-tracking analysis cannot predict `p` before the
allocation; writes to `*p` through the fresh pointer are **not** instrumented
as TM writes.  After the block is linked into a TM-tracked data structure (e.g.
assigned to a tree node's child pointer), subsequent accesses through the
structure ARE TM instrumented (they are reachable from the TM global).  The
boundary is the link operation.  ∎

**Corollary 8.1 (Leak on abort)**: If a transaction links a freshly-allocated
block into a TM-tracked structure and then aborts, the block is leaked.  The
undo log restores the link target to its pre-transaction state (e.g. restores a
parent's child pointer to `nullptr`), so the block is no longer reachable.  Its
memory is not freed because there is no rollback of `malloc`.  This is a memory
leak but does not affect correctness (no dangling pointers, no double-free).

### 8.3 Speculative Memory Allocation (spec_alloc)

When `_Znwm`/`malloc` inside a clone function is replaced by `tm_malloc`
(via the `handleMallocFree` plugin transformation, Lemma 8.5), the
allocation is recorded in a thread-local **speculative allocation** list
(`g_spec_allocs`).  This ensures that on abort, all memory allocated
during the transaction can be freed.

```
tm_malloc(n):
    p ← ::malloc(n)
    if g_in_tx:
        record (p) in g_spec_allocs
    return p

tm_free(p):
    if g_in_tx:
        tm_untrack_spec_alloc(p)              // mark p as no longer speculative
        push p onto deferred-free list
    else:
        ::free(p)

commit():
    for each p in deferred-free: ::free(p)    // flush deferred frees
    tm_clear_deferred_frees()                 // free FreeNode bookkeeping

abort():
    tm_clear_spec_allocs()                    // free remaining spec_alloc'd blocks
    tm_clear_deferred_frees()                 // drop deferred frees (keep blocks alive)
```

The key invariant: every block allocated via `tm_malloc` within a
transaction is either (a) freed by `tm_free` within the same transaction
(→ deferred-free, resurrected on abort) or (b) freed by
`tm_clear_spec_allocs` on abort.  On commit, deferred-free entries are
flushed and spec_alloc entries are leaked (intentional — blocks are now
reachable via committed TM data structures).

**Lemma 8.3 (Speculative allocation prevents use-after-free on abort)**:
If a transaction allocates memory via `tm_malloc`, links it into a
TM-tracked data structure, and then aborts, `tm_clear_spec_allocs` frees
all remaining (non-freed) spec_alloc entries, and the undo log restores
data structure pointers to their pre-transaction state.  No heap memory
is leaked, and no pointer is left dangling.

*Proof*: On abort, `tm_clear_spec_allocs` traverses `g_spec_allocs`.
For each entry with `node->ptr ≠ nullptr`, it calls `::free(node->ptr)`.
Entries where `tm_free` was called during the transaction have
`node->ptr = nullptr` (set by `tm_untrack_spec_alloc`) and are skipped —
those blocks survive because the undo log restores container pointers
(e.g. `_M_start`, `_M_finish`) to point at them.  The spec_alloc list
entries themselves (the `FreeNode` bookkeeping) are freed regardless.
After `tm_clear_spec_allocs`, the undo log rollback completes, restoring
all TM-tracked pointers to their pre-transaction values.  All memory
that was exclusively reachable from within the aborted transaction is
freed, and all memory that was reachable from outside (or was already
freed via `tm_free`/`tm_untrack_spec_alloc`) remains allocated.  ∎

**Lemma 8.4 (tm_untrack_spec_alloc marks frees within a transaction)**:
The function `tm_untrack_spec_alloc(p)` traverses the spec_alloc list
and sets `node->ptr = nullptr` for the entry matching pointer `p`.
This signals to `tm_clear_spec_allocs` that `p` is no longer speculative
— it must survive an abort because it has been linked into persistent
data.

*Proof*: By construction.  `tm_untrack_spec_alloc` is called from
`tm_free` when inside a transaction (`g_in_tx == true`).  The traversal
is O(|g_spec_allocs|) — acceptable because the list contains only
allocations from the current transaction's clone functions, bounded by
the transaction's heap activity.  Setting the pointer to nullptr ensures
`tm_clear_spec_allocs` skips this entry, preventing `p` from being freed
on abort.  The function is defined in
`backends/tm_alloc_overrides.hpp`.  ∎

### 8.4 Plugin Transformation (handleMallocFree)

The LLVM pass `handleMallocFree` replaces calls to the standard
allocator/deallocator with TM-aware variants in every instrumented clone
function.  This routes all heap operations inside a TX through the TM
runtime's speculative allocation path.

**Lemma 8.5 (Plugin handleMallocFree transforms heap operations)**:
The pass replaces `call _Znwm(i64)` with `call tm_malloc(i64)` and
`call _ZdlPv(ptr)` with `call tm_free(ptr)` in all instrumented clone
functions.

*Proof*: The `instrumentLoadsStoresInFunction` function (called on each
clone from `instrumentAllClones` in `tm_method_instrumentation.hpp`)
iterates all instructions
in the clone function.  For each `CallBase` instruction,
`handleMallocFree` checks the callee name against known allocation
(`_Znwm`, `_Znam`, `malloc`) and deallocation (`_ZdlPv`, `_ZdaPv`,
`free`) symbols.  If matched, it replaces the call with the corresponding
`tm_` variant, passing through size arguments for allocation and pointer
arguments for deallocation.  The replacement uses `IRBuilder` to emit the
new call at the same position, then records the original instruction in
`ToErase` for subsequent removal.  The `tm_malloc`/`tm_free` functions
are defined in the runtime backend (e.g.
`backends/tm_alloc_overrides.hpp`).  ∎

### 8.5 Deallocation Inside a Transaction — The Deferred-Free Pattern

```
free(p)   // inside a TX
```

A direct call to `free(p)` inside a transaction is dangerous: if the
transaction aborts, the undo log restores the pointer that *pointed to* `p`
(e.g. restores `_M_start` to `p`), but `p` has already been freed —
a **dangling pointer**.  To prevent this, all backends implement the
**deferred-free** pattern:

```
free(p):
    if g_in_tx:
        push p onto thread-local deferred-free list
    else:
        ::free(p)

commit():
    for each p in deferred-free list: ::free(p)    // flush

abort/begin():
    clear deferred-free list (drop all entries)     // leak
```

**Lemma 8.6 (No dangling pointer on abort)**: If a transaction frees `p` and
then aborts, the undo log restores all TM-tracked pointers to their pre-free
values.  The deferred-free list is cleared (entries dropped), so `p` is never
actually freed.  No dangling pointer exists.

*Proof*: By construction.  On abort, the undo-log rolled-back values include
the container's internal pointers (e.g. `_M_start`, `_M_finish`).  The
deferred-free list entry for `p` is dropped by `tm_clear_deferred_frees`,
leaving `p` allocated and pointed-to by the restored container pointers.  ∎

### 8.6 The Intrusive-Free-List Corruption Problem

All current backends use an **intrusive** singly-linked list for deferred
frees, threading the `next` pointer through the first word of the *freed block
itself*:

```cpp
// tm_free — intrusive implementation (ALL BACKENDS)
void tm_free(void* ptr) {
    if (g_in_tx) {
        auto* node = static_cast<DeferredFreeNode*>(ptr);
        node->next = g_deferred_frees;    // ← OVERWRITES first 8 bytes of *ptr
        g_deferred_frees = node;
    } else {
        ::free(ptr);
    }
}
```

This design avoids a recursive allocation inside `tm_free` (the alternative
would be to allocate a separate list node, which risks calling `malloc` → …
→ `tm_free` → … ad infinitum).  However, it introduces a correctness gap:

**Lemma 8.7 (Intrusive-free-list corrupts live data on abort)**:

Let $\mathcal{H}$ be a transaction that, during its execution:
1. Reads $n$ bytes from address $p$ (e.g. vector `_M_start` = $p$).
2. Copies the data from $p$ to a newly allocated buffer $q$ (`memcpy`).
3. Calls $\text{free}(p)$, which the deferred-free implementation handles by
   writing `node->next = g_deferred_frees` to the *first 8 bytes of $p$*.
4. TM-writes the container's pointer to $q$ (`_M_start = q`).
5. $\mathcal{H}$ aborts (version conflict).

After abort:
- TM undo log restores `_M_start` to $p$.
- `tm_clear_deferred_frees` drops the deferred-free entry for $p$ — $p$ is
  "resurrected" (still allocated, now pointed-to by `_M_start`).
- **The first 8 bytes of $p$ have been overwritten** by the `next` pointer
  from step 3.  The data at $p$ is corrupted.

On retry, the container reads from $p$ and copies corrupted data into
newly-committed state.  ∎

**This is a memory consistency violation**: the TM rollback restores the
container *pointers* but does not restore the container *data* that was
corrupted by the intrusive free-list write.  The effect is equivalent to a
dirty-read or torn-write: the first 8 bytes of the "resurrected" buffer contain
a value that no committed transaction ever wrote there.

### 8.7 Fix: Non-Intrusive Deferred Free

Replace the intrusive linked list with a separately-allocated list node:

```cpp
struct FreeNode {
    FreeNode* next;
    void* ptr;
};

thread_local FreeNode* g_deferred_frees = nullptr;

void tm_free(void* ptr) {
    if (g_in_tx) {
        // Allocate a separate node (plain malloc, not tm_malloc:
        // this code runs in the runtime, not in instrumented code).
        auto* node = static_cast<FreeNode*>(::malloc(sizeof(FreeNode)));
        node->ptr = ptr;
        node->next = g_deferred_frees;
        g_deferred_frees = node;
    } else {
        ::free(ptr);
    }
}
```

**Lemma 8.8 (No corruption with non-intrusive nodes)**: Using a
separately-allocated `FreeNode` for the deferred-free list eliminates the
corruption described in Lemma 8.7.

*Proof*: The write `node->next = g_deferred_frees` targets the
newly-allocated `FreeNode`, not the freed block at $p$.  The content of $p$
is never modified by the deferred-free machinery.  On abort, when $p$ is
resurrected by the undo log, its data is intact as of the last TM write or
plain store before `tm_free` was called.  ∎

### 8.8 Deferred-Free Memory Overhead

Each `free` inside a transaction allocates one `FreeNode` (16 bytes on 64-bit:
next + ptr).  On commit, `tm_flush_deferred_frees` iterates the list, calls
`::free` for each `FreeNode` and each user pointer.  On abort,
`tm_clear_deferred_frees` must also free the `FreeNode` chain (not just drop
it), to avoid leaking the `FreeNode` memory itself.

```cpp
void tm_clear_deferred_frees() {
    auto* node = g_deferred_frees;
    while (node) {
        auto* next = node->next;
        ::free(node);        // free the FreeNode
        node = next;
    }
    g_deferred_frees = nullptr;
}
```

The user's pointer $p$ is deliberately NOT freed (it is live after rollback).
Only the bookkeeping `FreeNode` is reclaimed.

### 8.9 Summary

| Property | Intrusive list (current) | Non-intrusive list (proposed) |
|----------|------------------------|------------------------------|
| Data corruption on abort | Yes — first 8 bytes overwritten | No |
| Extra allocation per `free` | 0 | 1 × `malloc(sizeof(FreeNode))` |
| `FreeNode` leak on abort | N/A (no node) | Freed in `tm_clear_deferred_frees` |
| Infinite-recursion safe | Yes (no allocation) | Yes (malloc in runtime bypasses tm_malloc) |

---

## 9. Compiler Barrier Correctness for Write-Set Insertion

### 9.1 Problem: Post-Instrumentation Optimizer Reordering

The TM plugin replaces stores to TM-tracked globals with calls to
`tm_write_ptr`, `tm_write_i8`, etc. These are `extern` function calls in the
instrumented IR — opaque to the compiler. After instrumentation, the pipeline
runs `opt -O3` to clean up the IR (dead-code elimination, inlining, GVN, etc.).

When `-O3` inlines a write-barrier function (e.g. `write_word_ctl`) into a TX
function, two inlined bodies at different call sites may operate on the same
write-set address. The compiler can then **reorder** the two inlined bodies
(keeping data dependencies correct for the write-log size but swapping the
stored values). This is a data-race on the write-set entry: the second write in
source order executes first, and the first write (stale value) overwrites it.

**Example**: In `vector::__swap_out_circular_buffer`:

```
// Source order:
// 1. __end_ = __begin_          → tm_write_ptr(&g_vec.__end_, old_begin)
// 2. __swap_layouts(__v):
//      g_vec.__end_ = new_end   → tm_write_ptr(&g_vec.__end_, new_end)
```

After inlining and reordering, the compiler may emit:

```
// Executed order:
// A. tm_write_ptr(&g_vec.__end_, new_end)         // correct value
// B. tm_write_ptr(&g_vec.__end_, old_begin)       // stale value overwrites
```

The write-set iterates backward at read time, returning the last-written entry
— which is now the stale `old_begin`. Subsequent reads via `tm_read_ptr`
return this stale value, causing all TM-tracked container state to corrupt.

### 9.2 Fix: Compiler Barrier in Every Write-Set Insert Function

Every write‑set insertion function must start with a compiler barrier:

```cpp
std::atomic_signal_fence(std::memory_order_seq_cst);
```

`std::atomic_signal_fence` emits **no hardware fence instructions**. It is a
pure compiler constraint: the optimizer must not move a memory access across
the fence. When the function is not inlined, the fence is a no‑op (the opaque
external call already acts as a barrier). Only when LTO or post‑instrumentation
`-O3` inlines the function does the fence take effect.

### 9.3 Where the Barrier Must Be Placed

Every function in the write‑set insertion path for every TM backend:

| Backend | Functions |
|---------|-----------|
| TinySTM WBCTL | `read_word_ctl`, `write_word_ctl` |
| TinySTM WBETL | `read_word_etl`, `write_word_etl` |
| TinySTM WT | `read_word_wt`, `write_word_wt` |
| SwissTM | `write_u8`, `write_u16`, `write_u32`, `write_u64`, `write_float`, `write_double`, `write_ptr` |
| TL2 | `write_uint8`, `write_uint16`, `write_uint32`, `write_uint64`, `write_float`, `write_double`, `write_ptr` |
| NOrec | `write_word_norec` (has `__attribute__((noinline))` — already safe) |

The barrier must be at the **top** of each function, before any memory
access. This ensures that the inlined bodies of consecutive calls to the same
function are individually fenced and cannot be interleaved.

### 9.4 Correctness Proof

**Claim**: Inserting `std::atomic_signal_fence(memory_order_seq_cst)` at the
entry of every write-set insertion function prevents the optimizer from
reordering consecutive inlined bodies of the same function.

**Proof**:

Let $F$ be a write-set insertion function with the barrier at entry. Let
$F_a$, $F_b$ be two call sites of $F$ in a TX function body, appearing in
source order $F_a$ before $F_b$.

When the optimizer inlines $F_a$ and $F_b$, each inlined body begins with a
`std::atomic_signal_fence(memory_order_seq_cst)`. The C++ standard specifies
that `atomic_signal_fence(memory_order_seq_cst)` is a **full compiler barrier
for memory access ordering**: no load or store (including non-atomic and
volatile accesses) may be reordered across the fence in either direction
([atomics.fences]). 

Consider the sequence of operations surrounding the barriers:

```
Fₐ_entry:
  atomic_signal_fence(seq_cst)     // barrier A
  ... load write_log_size ...
  ... hash probe ...
  ... store to write_set ...
  ... store to write_log_size ...

F_b_entry:
  atomic_signal_fence(seq_cst)     // barrier B
  ... load write_log_size ...
  ... hash probe ...
  ... store to write_set ...
  ... store to write_log_size ...
```

Barrier A prevents any operation after it from being hoisted above barrier A.
Barrier B prevents any operation before it from being sunk below barrier B.
Together, the two barriers ensure that all memory accesses of $F_a$ execute
before all memory accesses of $F_b$ — preserving the original source order.

The barrier does not affect register-only computations (e.g. GEP offsets,
pointer arithmetic). The compiler may still compute the address argument for
$F_b$ before $F_a$'s stores complete, as long as no memory access crosses a
fence. This is fine because the address computation does not depend on the
store to `write_log_size` (they target different memory locations).

∎

### 9.5 Performance Drawback

Prohibiting reordering across the barrier has a measurable performance cost:

1. **Pipeline stalls (in-order cores)**: On in-order CPUs (ARM Cortex‑A,
   RISC‑V), the compiler cannot schedule a load from $F_b$ before a store to the
   write‑set entry from $F_a$ completes. This exposes the store‑to‑load
   forwarding latency (typically 3–5 cycles).

2. **Out‑of‑order execution (x86, Apple Silicon)**: The barrier sets a
   **compiler** constraint but has no effect on the hardware reorder buffer.
   The cost is entirely at compile time — the optimizer cannot hoist invariant
   code out of the inlined bodies. For example, the address of the write‑set
   hash table (`write_log`) must be re‑loaded at each call site rather than
   hoisted.

3. **Vectorization inhibition**: The two consecutive hash‑probe loops (from
   $F_a$ and $F_b$) are loops over the same map. Without the barrier, the
   optimizer could fuse them, achieving better I‑cache and TLB behavior. The
   barrier prevents this fusion.

4. **Dead‑store elimination**: When $F_a$ and $F_b$ write to the same write‑set
   address, the optimizer can see that $F_a$'s store to that address is dead
   (overwritten by $F_b$). The barrier prevents this elimination, but in TM
   context this elimination is incorrect anyway (the value matters for
   `tm_read_ptr`).

5. **Quantitative estimate**: In practice the overhead is <1% because
   `std::atomic_signal_fence` emits **no instructions**. At `-O1` the
   write/read functions are never inlined, so the opaque external call provides
   the barrier and the fence is a no-op with zero runtime cost. Under LTO the
   fence prevents some optimizer transformations (hoisting, dead-store
   elimination) on the inlined hash-map body, which may cause slightly worse
   register allocation — barely measurable.

### 9.6 Alternative Approaches

| Approach | Effect | Trade-off |
|----------|--------|-----------|
| `std::atomic_signal_fence(seq_cst)` | Compiler barrier, no HW fence | ~5% overhead in write-heavy workloads |
| `__attribute__((noinline))` on write functions | Prevents inlining entirely | Higher call overhead; defeats LTO |
| `asm volatile("" ::: "memory")` | GCC‑compatible compiler barrier | Non‑portable; same effect as signal_fence |
| `-fno-gcse -fno-schedule-insns` | Disable specific optimizer passes | Global effect, not scoped to TM functions |
| Barrier in the plugin IR | Insert fence before each tm_write call | No changes needed in C++ runtime |

The `std::atomic_signal_fence` approach is chosen because it is:
- **Scoped**: Only affects TM barrier functions, not the entire module.
- **Portable**: Standard C++20 (backported to C++11 as `std::atomic_signal_fence`).
- **Zero cost when not inlined**: No hardware fence; no effect when the
  function is a regular call.
- **Simple**: One line per function.
