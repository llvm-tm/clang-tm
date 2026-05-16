# Correctness Proofs for TM Backends

## Notation

- $V_i$: shared variable (memory location)
- $R_{V_i}^j$: read of $V_i$ returning value with logical timestamp $j$
- $W_{V_i}^j$: write of value $j$ to $V_i$
- $L$: global lock variable
- $T_{ID}$: current thread identifier, $T_{ID} > 0$
- $L \leftarrow 0$: lock is free; $L \leftarrow T_{ID}$: thread $T_{ID}$ holds the lock
- $I_{V_i}$: atomic increment of $V_i$ by 1
- $I_{V_i,N}$: atomic increment of $V_i$ by $N$
- $C_{V_i, O, N}$: compare-and-swap on $V_i$, comparing $O$ and swapping to $N$; returns $\top$ (success) or $\bot$ (failure)
- $O \leftarrow^L V_i$: acquire-load of $V_i$ (C++ `memory_order_acquire`)
- $N \rightarrow^S V_i$: release-store of $N$ to $V_i$ (C++ `memory_order_release`)
- $F^S$: store fence (`atomic_thread_fence(memory_order_release)`)
- $F^L$: load fence (`atomic_thread_fence(memory_order_acquire)`)
- $\mathcal{H} = \{R_{V_{a_1}}^{j_1}, W_{V_{b_1}}^{j_2}, \dots\}$: a transaction, i.e. a sequence of reads/writes to shared variables
- $G$: global clock (monotonic, $\mathbb{N}_0$, even = unlocked)
- $E$: global epoch counter (monotonic, $\mathbb{N}_0$)
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
- $E$: monotonic global epoch counter, initially $0$
- $L$: mutex for SGL fallback

Per-thread state:
- $\text{in\_tsx}$: boolean, true while inside a hardware transaction
- $E_{\text{start}}$: epoch snapshot at TSX begin

```
transaction begin:
    if nested: return
    // TSX attempt (up to 5 retries)
    for attempt = 1..5:
        status = _xbegin()
        if status == _XBEGIN_STARTED:
            e ←ᴸ E
            if e & 1:                      // SGL active → abort
                _xabort(0xFF)
            E_start = e
            in_tsx = true
            return
        // Abort handling
        if explicit abort with code 0xFF:
            while E & 1: pause()           // spin-wait for SGL to finish
        else if retry bit not set: break
    // Fallback: SGL
    L.lock()
    I_E                                     // make epoch odd
    in_tsx = false

transaction commit:
    if nested: return
    if in_tsx:
        e ←ᴸ E
        if e ≠ E_start:                     // SGL interleaved
            _xabort(0x01)                   // must retry
        _xend()
        return
    // SGL path
    I_E                                     // make epoch even FIRST
    L.unlock()                              // then release lock
```

Read/write hooks: plain loads/stores — TSX or SGL provides isolation.

### 2.1 TSX Isolation

**Claim**: While $\text{in\_tsx} = \top$, every read returns the value at transaction start (or an atomic snapshot), and every write is buffered until commit.

**Proof**: This is a property of Intel TSX (RTM). The hardware maintains a read-set and write-set. Reads are served from memory but tracked; if another thread writes to a read-set cache line, the transaction aborts on `_xend()` or on the next access. Writes are buffered in the L1 cache and not visible to other threads until `_xend()`. This satisfies opacity for hardware transactions. ∎

### 2.2 Epoch-Based Serialization

**Claim**: The epoch $E$ together with the lock $L$ ensures that TSX and SGL transactions are serialized.

**Proof**: Let $E_t$ be the epoch value at logical time $t$, with $E_0 = 0$.

**Key invariant**: $E$ is odd $\iff$ an SGL transaction holds $L$. This holds because:
- SGL entry: acquire $L$ first, then $I_E$ (odd).
- SGL exit: $I_E$ (even) first, then release $L$.
- No other action changes $E$.

**SGL-SGL**: The lock $L$ is a standard mutex. Mutual exclusion ensures SGL transactions are serialized.

**TSX-TSX**: Two concurrent TSX transactions $\mathcal{H}_i, \mathcal{H}_j$ must conflict on at least one shared address to be non-serializable. If they access disjoint addresses, they can commit concurrently without conflict (hardware allows this). If they access the same address with at least one write, TSX detects the conflict and one aborts. The retry mechanism eventually serializes them.

**TSX-SGL**: Suppose $\mathcal{H}_{\text{TSX}}$ begins at $t_0$ and $\mathcal{H}_{\text{SGL}}$ runs between $t_1$ and $t_2$ with $t_0 < t_1 < t_2$.

1. At $t_0$, $\mathcal{H}_{\text{TSX}}$ reads $E = E_{\text{start}}$. If $E_{\text{start}}$ is odd, $\mathcal{H}_{\text{TSX}}$ aborts immediately (line `if e & 1: _xabort(0xFF)`). No TSX runs while $E$ is odd.
2. At $t_1$, $\mathcal{H}_{\text{SGL}}$ writes to $E$ (`I_E`). Since $E$ is in $\mathcal{H}_{\text{TSX}}$'s read set, this invalidates the cache line, causing $\mathcal{H}_{\text{TSX}}$ to abort at `_xend()` or on the next access.
3. Even if the cache line invalidation does not immediately abort (e.g. the TSX commit check runs after SGL finishes), the epoch re-check at commit catches it: $\mathcal{H}_{\text{TSX}}$ reads $E$ again; if $E \neq E_{\text{start}}$, it calls `_xabort(0x01)`.

Therefore no TSX transaction commits while an SGL transaction is active, and no SGL transaction runs while a TSX transaction could read stale data. The serialization order places committed TSX transactions between consecutive SGL transactions, ordered by their epoch-snapshot time. ∎

### 2.3 Opacity for TSX

**Claim**: TSXSGL provides opacity: every transaction (TSX or SGL) reads values that are consistent with a prefix of some serial history.

**Proof**: SGL transactions are trivially opaque (mutual exclusion). For a TSX transaction that commits, the epoch re-check at commit ensures that no SGL has interleaved; all values read by the TSX are from a state between the last SGL commit and the next SGL begin. If a TSX transaction aborts (due to SGL interleaving, hardware conflict, or epoch change), none of its writes become visible, and by the abort-gate property of TSX, the reads did not affect any other thread. Hence the transaction can be considered never to have executed. This satisfies opacity. ∎

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
| TSXSGL | TSX read-set isolation + epoch synchronisation | TSX abort on conflict; epoch re-check at commit | TSX abort rate under contention; SGL fallback |
| TL2 | Commit-time lock ordering + global clock + read validation | Locked guard during write-back; double-check read protocol | Guard table contention; O(τ) validation |
| TinySTM WBCTL | Commit-time lock ordering + global clock | Lock-based write-set protection + version validation | Lock table overhead; O($\|\mathcal{W}\| \log \|\mathcal{W}\|$) sorting |
| TinySTM WBETL | Encounter-time lock acquisition (eager) + global clock | Same as WBCTL; earlier conflict detection | Same lock table; eager locking may abort earlier |
| TinySTM WT | Write-through + undo log + encounter-time locks | Lock bit signals in-flight writes; readers spin | Write-through cache pressure; undo log cost |
| SwissTM | Eager write-write locks + lazy read-write validation + read-lock protocol | Read-locks at commit prevent concurrent readers; orec w-lock prevents concurrent writers | ORec table contention; two-phase commit overhead |
| NOrec | Value-based validation + single commit lock (CAS on global clock) | Re-reads read-set before commit; CAS prevents concurrent commits | Global commit lock; O(\|R\|) validation per clock change |

All backends provide **serializability** (conflict-serializable schedules equivalent to some serial execution) and **no dirty reads** (no transaction observes uncommitted intermediate state). The differences are in performance characteristics under contention.
