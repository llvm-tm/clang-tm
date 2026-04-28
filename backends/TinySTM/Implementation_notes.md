# REAL-TIME LAZY SNAPSHOT ALGORITHM

In what follows, we introduce the LSA-RT algorithm, an extension of LSA [9], that does not assume a global shared counter as time base.

## Time Bases

In this paper, we use two time bases: (1) perfectly synchronized clocks and (2) externally synchronized clocks. Perfectly synchronized clocks give (conceptually) all threads access to one global clock without any reading error. The reading error is the diﬀerence between the value read and the correct value. Typically, such a perfectly synchronized clock would need to be implemented in hardware. An externally synchronized clock also provides access to a globalclock but with some reading error that might vary and might not be bounded.

In both cases, the global clock does not actually need to be a real-time clock, i.e., neither its speed nor its value needs to be approximately synchronized with real-time. However, having a global real-time clock typically simplifies the implementation of an externally synchronized clock (because local clocks with a bounded drift rate can be used to approximate real-time). In particular, this reduces the overhead and error if the synchronization is done in software.

Algorithm 1 Generic utility functions
1: use function getTime()    ⊳ Get current timestamp (module-specific)
2: use function getNewTS()   ⊳ Get strictly greater timestamp (module-specific)
3: use function  ≽(t1, t2)   ⊳ Guaranteed later than or equal (module-specific)
4: function ≿(t1, t2)        ⊳ Possibly later than
5:     return t2 ⋡ t1
6: end function
7: use function max(t1, t2)  ⊳ Maximum (module-specific)
8: use function min(t1, t2)  ⊳ Minimum (module-specific)

We use several utility functions whose implementation depends on the time base that is used (see Algorithm 1): get-Time, getNewTS, t1 ≽ t2 (guaranteed later than or equal),t1 ≿ t2 (possibly later than), max and min. We first focus on the semantics of these utility functions. Their implementation will be described later together with their respective time base.

Function getTime returns the current time. We assume that the timestamps that a thread is reading are monotonic, i.e., if a thread reads first t1 and then t2, then we know that t2 is guaranteed to be later or equal to t1. In our terminology, we will denote this by t2 ≽ t1. We do not require t2 to be strictly later than t1 because we want to support clocks that tick rarely, e.g., only when a transaction commits. If a thread needs to read such a fresh timestamp, it needs to call function getNewTS. This function ensures that the value returned to a thread is strictly greater than any timestamp that has so far been returned to this thread by getNewTS or getTime. Note that the timestamps returned by getTime and getNewTS are not necessarily unique: other threads might read the same timestamps.

We assume that getTime and getNewTS return a clock value that was read instantaneously at some point in realtime between the time getTime/getNewTS was called andthe time it returned. If two timestamps are read by different threads, it might not be possible to say which timestamp wasread later or earlier (e.g., because there is a non-zero clock reading error). To cope with this uncertainty, we say that t2 ≽ t1 iff it is guaranteed that t1 was read no later than t2. Sometimes we might only be able to say that t2 was possibly read at a later point than t1. We denote this by t2 ≿ t1. Note that t2 ≽ t1 always implies t1 ≵ t2 and t2 ≿ t1 implies t1 ⋡ t2.

The utility functions max(t1, t2) and min(t1, t2) have the following semantics. For any timestamp t3, if t3 is guaranteed to be later than max(t1, t2) then t3 is guaranteed to be later than both t1 and t2. Similarly, for any timestamp t3 that is guaranteed to be earlier than min(t1, t2), then t3 is guaranteed to be earlier than both t1 and t2.

## Snapshot Construction

Algorithm 2 Real-Time Lazy Snapshot Alg. (LSA-RT)
1:  procedure Start( T )      ⊳ Initialize transaction attributes
2:      T.CT ← 0              ⊳ T ’s commit time
3:      T.R ← [getTime(), ∞]  ⊳ T ’s validity range
4:      T.O ← ∅               ⊳ Set of objects versions accessed by T
5:      T.update ← false      ⊳ T starts as a read-only transaction
6:      T.status ← active     ⊳ T is active
7:  end procedure
8:  procedure Open(T, o, m)   ⊳ Opens o in mode m (read/write). To simplify, we assume an object is opened at most once per T
9:      if m = write then
10:         T.update ← true
11:         repeat
12:             vc ← getVersion(T, o, [⌊T.R⌋, ∞]) ⊳ Get latest committed version
13:             v ← clone(vc )                     ⊳ Create new copy for writing
14:             v.T ← T                            ⊳ Current transaction is writer
15:             Tw ← o.writer
16:             if Tw ≠ null ∧ Tw.status ∉ {aborted, committed} then
17:                 solveConflict(o, T, Tw)         ⊳ Contention manager...
18:             else                                ⊳ ...arbitrates and aborts the loser
19:                 CAS(o.writer, T.w, T)           ⊳ Try registering as writer
20:             end if
21:         until o.writer = T
22:         if ⌊v.R⌋ ≿ ⌈T.R⌉ then                  ⊳ Is the version too recent?
23:         Extend(T)                               ⊳ Extend as much as possible
24:     end if
25:     else
26:         v ← getVersion(T, o, T.R)               ⊳ Get latest committed version in interval
27:     end if
28:     ⌊T.R⌋ ← max(⌊T.R⌋, ⌊v.R⌋)
29:     ⌈T.R⌉ ← min(⌈T.R⌉, getPrelimUB(T, o, v, ⌈T.R⌉))
30:     if ⌊T.R⌋ ≿ ⌈T.R⌉ then                         ⊳ Possibly inconsistent?
31:         Abort(T)                                   ⊳ Yes: abort (and terminate execution)
32:     end if
33:     T.O ← T.O ∪ {(o, v)}                           ⊳ Access object versions
34: end procedure
35: procedure Commit(T)                                ⊳ Try to commit transaction
36:     if ¬T.update then
37:         CAS(T.status, active, committed)           ⊳ Validation not necessary
38:     else
39:     CAS(T.status, active, committing)              ⊳ Start committing
40:     if T.status = committing then
41:         t ← getNewTS()                             ⊳ Tentative commit time (may not be unique)
42:         CAS(T.CT, 0, t)                            ⊳ Try imposing our timestamp
43:         for all (o, v) ∈ T.O do                    ⊳ Are versions still valid at t?
44:             ub ← getPrelimUB(T, o, v, T.CT )
45:             if T.CT ≿ ub then
46:                 Abort(T)                           ⊳ No: abort (and terminate execution)
47:             end if
48:         end for
49:         CAS(T.status, committing, committed)       ⊳ Yes: commit
50:     end if
51: end if
52: end procedure
53: procedure Abort(T)                                 ⊳ Abort transaction (unless committed)
54:     if ¬CAS(T.status, active, aborted) then        ⊳ Still active?
55:         CAS(T.status, committing, aborted)         ⊳ Committing?
56:     end if
57:     if T.status = aborted then                     ⊳ Aborted?
58:         throw AbortedException in T                ⊳ Terminate execution
59:     end if
60: end procedure

The main idea of LSA-RT (see Algorithm 2) is to construct consistent snapshots on the fly during the execution of a transaction and to lazily extend the validity range on demand. By this, we can reach two goals. First, transactions working on a consistent snapshot always read consistent data. Second, verifying that there is an overlap between the snapshot’s validity range and the commit time of a transaction can ensure linearizability, if so desired.

The set of objects being accessed by a transaction and their specific versions are determined during the execution of a transaction. The validity range T.R is therefore constructed incrementally. When a transaction T is started, the lower bound of its validity range is set to the current time (line 3), i.e., the transaction cannot execute in the past. The getTime function returns the current time—as observed by the current thread—according to the time basebeing used. The timestamps returned by the function to any single thread are guaranteed to be monotonically increasing, but not strictly (a thread may read more than once the sametimestamp).

When accessing the most recent version of an object o, it is not yet known when this version will be replaced by a new version. We therefore obtain an approximate validity ranger by obtaining the latest version of o (line 12) and computing a lower bound on its maximum validity range. We call this the preliminary upper bound on the validity range (see line 29). Note that we use the GetPrelimUB function to recompute the preliminary upper bound of an object version according to the current thread’s time reference. During theexecution of a transaction, time might advance and thus the preliminary validity ranges might get longer. We can try to extend T.R by recomputing its lower bound (line 23 of Algorithm 2 and lines 1–6 in Algorithm 3). Extensions are not required for correctness, but they increase the chance that a suitable object version is available. To avoid unnecessary ex-tensions, we mark a transaction as closed as soon as extenddetects that T has read an object version that has in mean-time be replaced by a new version, i.e., no further extensionof the validity interval T.R is possible. For simplicity, wehave not included this optimization in the pseudo-code.


Algorithm 3 Helper functions
1:  procedure Extend(T)                    ⊳ Try to extend T ’s validity range to at least t
2:      ⌈T.R⌉ ← getTime()
3:      for all (o, v) ∈ T.O do            ⊳ Recompute the upper bound on validity range
4:          ⌈T.R⌉ ← min(⌈T.R⌉, gePrelimtUB(o, v, ⌈T.R⌉))
5:      end for
6:  end procedure
7:  function getVersion(T, o, R)           ⊳ Get latest version of o overlapping R
8:      loop
9:          v ← latest version of o s.t. ⌈v.R⌉ ≽ ⌊R⌋ ∧ ⌈R⌉ ≽ ⌊v.R⌋ ∧ (v.T = null ∨ v.T.status ∈ {committing, committed})
10:         if v = null then                       ⊳ Any valid version?
11:             Abort(T)                           ⊳ No: abort (and terminate execution)
12:         else if v.T ≠ null ∧ v.T.status = committing then
13:             Commit(v.T)                        ⊳ Help committing transaction to complete
14:         else
15:             return v                           ⊳ Always return a commited version
16:         end if
17:     end loop
18: end function
19: function getPrelimUB(T, o, v, t)        ⊳ Get conservative estimate on v.R
20:     Tw ← o.writer
21:     if ⌈v.R⌉ ≠ ∞ then                   ⊳ Still open?
22:         return ⌈v.R⌉                    ⊳ No: return version upper bound
23:     else if Tw ≠ null then              ⊳ Yes: only Tw may set UB before t
24:         if Tw.status ∈ {committing, committed} then
25:             if Tw.CT > 0 then
26:                 if Tw = T then
27:                     return Tw.CT        ⊳ Oﬀ by 1 but simplifies Commit
28:                 else
29:                     return Tw.CT − 1    ⊳ Version valid at least until then
30:                 end if
31:             end if
32:         end if
33:     end if
34:     return t                       ⊳ Return caller’s timestamp (getTime() ≽ t)
35: end function


If the validity range r of the latest version of o does notintersect with T.R and the transaction is read-only, we can look for an older version whose range overlaps with T.R (the algorithm requests the most recent among the valid overlapping versions, but any of them would do). The newvalue of T.R is computed as the intersection of the previousvalue and the validity range of the version being accessed (lines 28–29). The transaction must abort if no suitable version can be found (line 31 of Algorithm 2 and line 11 of Algorithm 3).

By construction of T.R, LSA-RT guarantees that a transaction started at time t has a snapshot that is valid at or after the transaction started, i.e., ⌊T.R⌋ ≽ t. Hence, a read-only transaction can commit iff it has used a consistent snapshot, i.e., T.R is non-empty.

## Update Transactions

An update transaction T can only commit if it can extendits validity range up to and including its commit time. This ensures that at the time T commits no other transaction has modified any of these objects including at the commit time. Note that in this way, we permit multiple transactions to commit at the same time as long as they are not in conflict with each other. The preliminary upper bound of an object version written to by T is overestimated by 1 (line 27 in Algorithm 3) to simplify the test in Commit(): we know that T will try to commit a new version o at T.CT but, more importantly, we also know that no other transactioncan commit a new version of o until T.CT+1 if T can indeed commit.

The commit of an update transaction (lines 35–52) is a two-phase process. The transaction first enters the committing state before determining whether it can commit or must abort. The reason for keeping track of the transaction’s status and updating it using a CAS operation is that another thread can help the transaction to commit or force it to abort, as will be discussed shortly.

A committing thread will try to set the timestamp obtained from its local time reference as the commit time of the transaction. If it fails, i.e., another thread has set the commit time beforehand, then the current thread uses that previously set commit time T.CT. The thread will then check whether the upper bound of the validity range of the transaction can be extended to include T.CT. The trans-action can only commit if this succeeds because otherwisesome objects accessed by T might have been modified byanother transaction that committed before T.CT.

If it is possible to update a most recent version (i.e., T.R remains non-empty), LSA-RT atomically marks the object o that it is writing (visible write) by registering itself in o.writer. When another transaction tries to write the same object, it will see the mark and detect a conflict (lines 16-17). In that case, one of the transactions might need to wait or be aborted. This task is typically delegated to a contention manager [7], a configurable module whose role is to determine which transaction is allowed to progress upon conflict; the other transaction will be aborted.

Setting the transaction’s state atomically commits—or discards in case of an abort—all object versions written by the transaction and removes the write markers on all written objects (as in DSTM [7]).

## Use of Real-Time Clocks

The function getNewTS is actually required to return a timestamp that is larger than the time at which the function got invoked. For time bases that can tick on demand (e.g.,counters), this condition is easily satisfiable. However, if a clock ticks independently and rarely (e.g., a slow real-timeclock), the committing transaction T would have to wait for a new timestamp. If reading the time takes always longer than the time between two ticks of the time base (which isthe case in our system), then this requirement is trivially satisfied.

The reason for this requirement is that threads need to agree on the validity ranges of object versions. Informally, we have to avoid a situation where one transaction draws conclusions about the state at time t and later another transaction modifies state at t. We ensure this by first putting an update transaction T into the committing state, which will get visible to other transactions at some time tc when theCAS returns. getNewTS then sets T.CT to a value larger than tc (see above). Because transactions always read the time before they start to access objects (see Algorithms 2 and 3), it is guaranteed that if a transaction Ta accesses a version at time t, all transactions T that could commit a change to the object at T.CT = t are already in the committing state. Ta sees this state indirectly in all possibly updated objects, and will either not access the version at time t or wait for all T to commit or abort.



# A Lightweight STM Design

TINYSTM is a word-based STM implementation that uses locks to protect shared memory locations. Its name stems from the simplicity and performance of our design. TINYSTM uses a single-version, word-based variant of our LSA algorithm [10] (which is very similar to TL2’s algorithm [3]) and uses a time-based design. The notion of time to guarantee consistency in STMs was first proposed in [11] and then simultaneously exploited by LSA and TL2 for object-based and word-based designs, respectively. TINYSTM shares many properties with other word-based STMs: in particular with TL2 (several aspects of our STM library were directly inspired by TL2’s reference implementation) but also with Ennals’ [4] and Sahaet al.’s [13, 16] designs. However, TINYSTM follows different design strategies on some key aspects.

Unlike TL2’s reference implementation—but like Ennals and Sahaet al.’s algorithms—TINYSTM uses encounter-time locking. Yet, like LSA and TL2, our STM is time-based and guarantees that transactions always read consistent memory states. The reason why we opted for encounter-time locking is twofold:
  - First, our empirical observations appear to indicate that detecting conflicts early often increases the transaction throughput because transactions do not perform useless work. Commit-time locking may help avoid some read-write conflicts, but in general conflicts discovered at commit time cannot be solved without aborting at least one transaction.
  - Second, encounter-time locking allows us to efficiently handle reads-after-writes without requiring expensive or complex mechanisms. This feature is especially valuable when write sets have non-negligible size.

In addition, we have implemented two strategies for accesses to memory, each with its unique advantages and limitations: with write-throughaccess, transactions directly write to memory and revert their updates in case they need to abort; with write-back access, transactions delay their updates to memory until commit time. We shall discuss both strategies shortly.

For the sake of simplicity, TINYSTM has been designed in such a way that a transaction never needs to access another transaction’s private memory (besides the shared data structures used for concurrency control). Atomic operations and memory barriers are implemented using Hans Boehm’s atomic ops library [1]. These design choices make it straightforward to compile TINYSTM on any 32- and 64-bit architecture supported by atomic ops.

## 3.1 Basic Algorithm

Locks and Versions: As most word-based STM designs, TINYSTM relies upon a shared array oflocksto manage concurrent accesses to memory (see Figure 1). Each lock covers a portion of the address space. In our implementation, we use a per-stripe mapping where addresses are mapped to locks based on a hash function.

Each lock is the size of an address on the target architecture. Its least significant bit is used to indicate whether the lock is owned. If it is not owned, we store in the remaining bits a version number that corresponds to the commit timestamp of the transaction that last wrote to one of the memory locations covered by the lock.

If the lock is owned, we store in the remaining bits an address to either the owner transaction (when using write-through), or an entry in the write set of the owner transaction (when using write-back). In both cases, addresses point to structures that are word-aligned and their least significant bit is always zero; hence it can be safely used as lock bit.

When using the write-back design, the address stored in the owned lock allows a transaction to quickly locate in its write set the updated memory locations covered by the lock, in case they are accessed again by the same transaction. In contrast, TL2 must check upon access to a memory location whether the current transaction did not yet write to this address, which may be costly when write sets grow large (TL2 uses Bloom filters to avoid unnecessary write set traversals). Read-after-write is not a problem when using the write-through design because the memory always contains the latest value written by the active transaction.

Reads and Writes: When writing to a memory location, a transaction first identifies the lock entry that covers the memory address and atomically reads its value. If the lock bit is set, the transaction checks if it is the owner of the lock using the address stored in the remaining bits of the entry. In that case, it simply writes the new value and returns. Otherwise, the transaction can try to wait for some time (note that the transaction must not wait indefinitely as this might lead to deadlocks) or abort immediately. We use the latter option in our implementation.

If the lock bit is not set, the transaction tries to acquire the lock by writing a new value in the entry using an atomic “compare-and-swap” operation. Failure indicates that another transaction has acquired the lock in the meantime and the whole procedure is restarted.

When reading a memory location, a transaction must verify that the lock is not owed nor updated concurrently. To that end, the transaction reads the lock, then the memory location, and finally the lock again (obviously, appropriate memory barriers are used to ensure correct ordering of accesses). If the lock is not owned and its value (i.e., version number) did not change between both reads, then the value read is consistent. There is a subtle problem when using the write-through design. Consider a transaction reading the lock; then, another transactions grabs the lock and writes a new value to memory that is read by the first transaction; the second transaction aborts and restores the initial value of the lock; finally, the first transaction reads the lock again and does not detect a concurrent access, although it has read an inconsistent value. To solve that issue, we additionally store in the lock anincarnation numberthat is incremented each time a transaction aborts and allows us to detect concurrent accesses in such scenarios. We use three bits for the incarnation number in our implementation; in the unlikely event that it overflows, we simply obtain a new version number from the global clock. Note that this problem does not apply to the write-back design.

Once a value has been read, we check if it can be used to construct a consistent snapshot. As with LSA, if the version is more recent than the current validity range of the transaction’s snapshot, we try to “extend” the snapshot. This consists in verifying that every address in the read set is still valid and not locked by another transaction. If extension succeeds, we can update the end of the snapshot’s validity range up to the value of the clock right before extension. Otherwise, the transaction aborts (when keeping multiple versions of every memory location, we could use an old value valid at the time of the snapshot. We do not use this approach in our implementation because, without hardware support, the memory and processing overheads of multi-version designs overcome their benefits).

Read-only transactions are particularly efficient because we incrementally construct a snapshot that is valid at all times. No validation is necessary at commit time and, hence, we do not need to maintain a read set.

Write-through vs. Write-back: The write-through and write-back designs differ in the way updates are written to memory. With write-through access, updates are written directly to memory and previous values are stored in anundo log to be reinstated upon abort. With write-back access, updates are stored in awrite log and written to memory upon commit. Write-through has lower commit-time overhead and faster read-after-write/write-after-write handling; further, it enables various interesting compiler optimizations, e.g., accesses to a previously locked memory location do not
need to go through the STM at all. On the other hand, write-back
has lower abort overhead and does not require incarnation numbers
to guarantee consistent reads, as discussed above.

Memory Management: Using dynamic memory within transactions is not trivial with unmanaged languages. Consider the case of a transaction that inserts an element in a dynamic data structure such as a linked list. If memory is allocated but the transaction fails, it might not be properly reclaimed, which results in memory leaks. Similarly, one cannot free memory in a transaction unless one can guarantee that it will not abort. Dealing explicitly with such situations leads to intricate code.

TINYSTM provides memory-management functions that allow transactional code to use dynamic memory. Transactions keep track of memory allocated or freed: allocated memory is automatically disposed of upon abort, and freed memory is not disposed of until commit. Further, a transaction can only free memory after it has acquired all the locks covering it as a free is semantically equivalent to an update.

Clock Management: TINYSTM uses a shared counter as clock, like LSA and TL2. This approach is both simple and sufficiently efficient on SMP architectures. In case the contention on this global counter becomes a bottleneck in large systems, we can use more scalable time bases such as an external clock or multiple synchronized physical clocks [12].

The maximal value of the clock is 2^31 on a 32-bit architecture, and 2^63 on a 64-bit architecture (with the write-through design, maximal values are 2^28 and 2^60 as three bits are used for incarnation numbers).

In 32-bit systems with frequent commits, this value can be quickly reached. Therefore, TINYSTM provides a simple clock roll-over mechanism: when a transaction detects that the maximal clock value has been reached (transactions read the current time when they start. Update transactions additionally obtain the current time when they try to commit), it aborts and waits on a barrier until all active transactions have completed their execution. Then, we reset the clock and all version numbers. While this procedure unnecessarily aborts some non-conflicting transactions and prevents progress for a short period of time, its overhead is negligible as it is executed only rarely.

## Hierarchical Locking

Transactions that are identified as read-only do not need to keep a read set as the LSA algorithm guarantees that we incrementally construct a consistent snapshot. Update transactions do, however, need to validate their read sets at commit time. This implies that they must verify that all the addresses they have read are still valid, i.e., they are not locked by another transaction and still have the same version number. A notable exception is when the commit time of the transaction is equal to its start time plus one: in that case, validation is not necessary as we know that no other transaction has concurrently written to memory.

Depending on the size of the read set, validation may be costly. A transaction reading a large chunk of memory (e.g., a large matrix) needs to validate every single address read. To speed up validation of large read sets, one can reduce the number of locks, i.e., each lock will cover a larger number of memory locations. However, this can increase the abort rate significantly due to memory operations mistakenly identified as conflicting. To solve this problem, we propose using a hierarchical locking strategy.


In addition to the shared array of l locks, we maintain a smaller hierarchical array of h << l counters (typically 4 to 16) as depicted in Figure 1. Memory addresses are mapped to the counters using a hash function that is consistent with that of the lock array: two memory locations that are mapped to the same lock are also mapped to the same counter. In other words, a counter covers multiple locks and the associated memory addresses. When choosing l as a multiple of h, typically l = 2^i, h = 2^j, i > j, we can compute the lock index as (hash(addr) mod l) and the counter index as (hash(addr) mod h).

Each transaction additionally maintains two private data structures: a read mask and a write mask of h bits each. Finally, read sets are partitioned into h independent parts.

When reading or writing a memory location, a transaction will first determine to which shared counter i in the hierarchical array it maps. If the corresponding i-th bit in the read mask is zero, then we set it and store locally the current value of the counter. If the memory access is a write, we check the corresponding i-th bit in the write mask: if zero, we set it and atomically increment the shared counter. If the memory access is a read and the transaction maintains a read set, we create the new entry in the corresponding i-th part of the read set.

Upon validation, we check for every counteriwhose corresponding i-th bit is set in the read mask if (1) the current value of the counter is equal to the previously stored value, or (2) the current value of the counter is one more than the stored value and the corresponding i-th bit in the write mask is set. In either case, we know that no concurrent transaction has locked an address that maps to counteriand we can skip validation of the i-th part of the read set (i.e., we can use the validation fast path). By doing so, we are essentially partitioning the locks so that validation can apply to only a portion of the memory locations read by a transaction.

Note that a transaction writing to many locations might need to increment at most h counters. As atomic operations are costly on most architectures, the size of the hierarchical array must be chosen with care: larger h values reduce the validation overhead but may require more atomic operations.

Strictly speaking, the role of the hierarchical array is not to lock memory locations; hence the term hierarchical “locking” is not perfectly accurate. The array does, however, allow transactions to determine whether locks have been acquired. This scheme can be generalized “hierarchically” to multiple levels of nesting.

Obviously, hierarchical locking provides performance benefits only if (1) read set validation is expensive, i.e., update transactions read many memory locations, and (2) there are few writes from competing transactions. However, we believe that these conditions are encountered often enough in real applications for this optimization to be useful.
