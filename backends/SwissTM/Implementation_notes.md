# SwissTM

SwissTM is a lock-based STM that uses invisible reads and counter based heuristics (the same as in TinySTM and TL2). It features eager write/write and lazy read/write conflict detection, as well asa two-phase contention manager with random linear back-off. The API of SwissTM is word-based, as it enables transactional access to arbitrary memory words. SwissTM uses a redo-logging scheme (partially to support its conflict detection scheme).

## Programming model

Similarly to most other STM libraries, SwissTM guarantees opacity [17]. Opacity is similar to serializability in database systems [30]. The main difference is that all transactions always observe consistent states of the system. This means that transactions cannot, e.g., use stale values, and that they do not require periodic validation or sandboxing to prevent infinite loops or crashes due to accesses to inconsistent memory states.

SwissTM is a weakly atomic STM, i.e., it does not provide any guarantees for the code that accesses the same data from both inside and outside of transactions. SwissTM is not privatization safe [38]. This could make programming with SwissTM slightly more difficult in certain cases, but did not affect us, as none of the benchmarks we use requires privatization-safe STM.

When programming with SwissTM, programmers have to replace all memory references to shared data from inside transactions with SwissTM calls for reading and writing memory words. The programming model can be improved by using an STM compiler (as in e.g. [21, 2, 14, 29]). While the compiler instrumentation can degrade performance due to over-instrumentation [42] and possibly even change the characteristics of the workload slightly (e.g. numbers and ratio of transactional read and write operations), the compiler instrumentation remains a largely orthogonal issue to the performance of an STM library.

Other three STMs we compare to in our experiments provide the same semantical guarantees as SwissTM. Also, strengthening the guarantees (as described in Section 6) would have a similar performance impact on all STMs we use.

## Algorithm

We give the pseudo-code of SwissTM in Algorithm 1. The algorithm invokes contention manager functions ( cm_* ), which are defined in Algorithm 2 and described below. All transactions share a global commit counter commit_ts incremented by every non-read-only transaction upon commit. Each memory word m is mapped to a pair of locks in a global lock table : r_lock (read) and w_lock (write). Lock w_lock is acquired by a writer T of m (eagerly) to prevent other transactions from writing to m .Lock r_lock is acquired by T at commit time to prevent other transactions from reading word m and, as a result, observing inconsistent states of words written by T. In addition, when r_lock is unlocked, it contains the version number of m. Every transaction T has a transaction descriptor tx that contains (among other data): (1) the value of commit_ts read at the start or subsequent validation of T , and (2) read and write logs of T.

Transaction start. Every transaction T , upon its start, reads the global counter commit_ts and stores its value in tx.valid_ts (line 2).

Reading. When reading location addr , transaction T first reads the value of w_lock to detect possible read-after-write cases. If T is the owner of w_lock ,then T can return the value from its write log immediately, which is the last value T has written to addr (line 6). Otherwise, i.e., when some other transaction owns w_lock or when w_lock is unlocked, T reads the value of r_lock , then the value of addr , and then again the value of r_lock. Transaction T repeats these three reads until (1) two values of r_lock are the same, meaning that T has read consistent values of r_lock and addr ,and(2) r_lock is unlocked (lines 8–15). When r_lock is unlocked, it contains the current version v of addr .If v is lower or equal to the validation timestamp tx.valid_ts of T (which means that addr has not changed since T ’s last validation or start), T returns the value at addr read in line 18. Otherwise, T revalidates its read set. If the revalidation does not succeed, T rolls back (line 17). If it succeeds, the read operation returns and T extends its validation timestamp tx.valid_ts to the current value of commit_ts (line 56).

Writing. Whenever some transaction T writes to a memory location addr , T first checks if T is the owner of the lock w_lock corresponding to addr .Ifitis, T updates the value of addr in its write log and returns (lines 21–23). Otherwise, T tries to acquire w_lock by atomically replacing, using a compare-and-swap (CAS) operation, valueunlockedwith the pointer to the T ’s write log entry that contains the new value of addr (line 29). If CAS does not succeed, T asks the contention manager whether to rollback and retry or wait for the current owner of the lock to finish (line 26). In order to guarantee opacity, T has to revalidate its read set if the current version of addr (contained in r_lock ) is higher than its validity timestamp tx.valid_ts (lines 31–32).

Validation. To validate itself, T compares the versions of all
memory locations read so far to their versions at the point they
were initially read by T (lines 51–52). These versions are stored in
T ’s read log. If there is a mismatch between any version numbers,
the validation fails (line 52).

Commit. A read-only transaction T can commit immediately, as its read log is guaranteed to be consistent (line 35). A transaction T that is not read-only first locks all read locks of memory locations T has written to (line 36). Then, T increments commit_ts (line 37) and re-validates its read log. If the validation does not succeed, T roll-backs and restarts (lines 38–41). Upon successful validation, T traverses its write set, updates values of all written memory locations, and releases the corresponding read and write locks (lines 42–45). When releasing read locks, T writes the new value of commit_ts to those locks.

Algorithm 1 : Pseudo-code representation of SwissTM.

1  function start ( tx )
2      tx.valid_ts := commit_ts ;
3      cm_start ( tx );
4  function read-word ( tx, addr )
5      ( r_lock , w_lock ) := map_addr_to_locks ( addr );
6      if is_locked_by ( w_lock, tx ) then return get-value ( w_lock, addr );
7      version := read ( r_lock );
8      while true do
9          if version == locked then
10             version := read ( r_lock );
11             continue ;
12         value := read ( addr );
13         version2 := read ( r_lock );
14         if version = version2 then break ;
15         version2 := version ;
16     add_to_read_log ( tx , r_lock , version );
17     if version > tx.valid_ts and not extend ( tx ) then rollback ( tx );
18     return value ;
19 function write-word ( tx, addr, value )
20     ( r_lock , w_lock ) := map_addr_to_locks ( addr );
21     if is_locked_by ( w_lock, tx ) then
22         update_log_entry ( w_lock , addr , value );
23         return ;
24     while true do
25         if is_locked ( w_lock ) then
26             if cm_should-abort ( tx, w_lock ) then rollback ( tx );
27             else continue ;
28         log_entry := add_to_write_log ( tx , w_lock , addr , value );
29         if CAS ( w_lock, unlocked, log_entry ) then
30             break ;
31     if read ( r_lock ) > tx.valid_ts and not extend ( tx ) then
32         rollback ( tx );
33     cm_on_write ( tx );
34 function commit ( tx )
35     if is_read_only ( tx ) then return ;
36     for log_entry in tx.read_log do write ( log_entry.r_lock ,locked);
37     ts := atomic_add_fetch ( commit_ts );
38     if ts > tx.valid_ts + 1 and not validate ( tx ) then
39         for log_entry in tx.read_log do
40             write ( log_entry.r_lock , log_entry.version );
41         rollback ( tx );
42     for log_entry in tx.write_log do
43         write ( log_entry.addr , log_entry.value );
44         write ( log_entry.r_lock , ts );
45         write ( log_entry.w_lock ,unlocked);
46 function rollback ( tx )
47     for log_entry in tx.write_log do
48         write ( log_entry.w_lock ,unlocked);
49     cm_on_rollback ( tx );
50 function validate ( tx )
51     for log_entry in tx.read_log do
52         if log_entry.version != read ( log_entry.r_lock ) and not is_locked_by ( log_entry.r_lock, tx ) then return false ;
53     return true ;
54 function extend ( tx )
55     ts := read ( commit_ts );
56     if validate ( tx ) then tx.valid_ts := ts ; return true ;
57     return false ;

Algorithm 2 : Pseudo-code of the two-phase contention manager ( Wn is a constant)

1  function cm_start ( tx )
2      if not_restart ( tx ) then tx.cm_ts := ∞;
3  function cm_on_write ( tx )
4      if tx.cm_ts == ∞ and size ( tx.write_log )= Wn then tx.cm_ts := atomic_add_fetch ( greedy_ts );
5  function cm_should-abort ( tx, w_lock )
6      if tx.cm_ts == ∞ then return true ;
7      lock_owner = owner ( w_lock );
8      if lock_owner. cm_ts < tx.cm_ts then return true ;
9      else abort ( lock_owner ); return false ;
10 function cm_on_rollback ( tx )
11     wait_random ( tx.succ_abort_count );

Rollback. On rollback, transaction T releases all write locks it
holds (lines 47–48), and then restarts itself.

Contention management. We give the pseudo-code of our two-phase contention manager in Algorithm 2. The contention manager gets invoked by Algorithm 1 (1) at transaction start ( cm_start in line 3), (2) on a write/write conflict ( cm_should-abort in line 26), (3) after a successful write ( cm_on_write in line 33), and (4) after restart ( cm_on_rollback in line 49). Every transaction, upon executing its Wn thwrite (where we set Wn to 10), increments global counter greedy_ts and stores its value in tx.cm_ts (line 4). Hence, short transactions (those that execute less than Wn writes) do not access greedy_ts that would otherwise become a memory hot spot—this reduces contention and the number of cache misses. Transactions that have already incremented greedy_ts are in the second phase of the contention management scheme, and others are in the first phase. Upon a conflict, a transaction that is still in the first phase gets restarted immediately (line 6). If both conflicting transactions are already in the second phase, the transaction with the higher value of cm_ts is restarted (lines 8–9). This prioritizes ransactions that have performed more work. Conceptually, transactions in the first phase have an infinite value of cm_ts (set in line 2). This means that (longer) transactions, which are in the second phase, have higher priority than (short) transactions that are in the first phase. After restarting, transactions are delayed using a randomized back-off scheme (line 11). This reduces probability of having some transaction aborted many times repeatedly because of the same conflict.

## Implementation Highlights

We implemented SwissTM in C++ (g++ 4.0.1 compiler). We used the (fairly portable) atomicops library [5] for atomic operations implementation. Currently, SwissTM works on 32-bit x86 Linux 2.6.x and OS X 10.5 platforms (64-bit port is in progress).

Lock table. To map memory word m to a lock table entry, we take the address a of m , shift it to the right by 4 (it would be 5 with 64-bit words). This makes each lock map to consecutive four memory words (we empirically selected this value, as explained in Section 5). Then, we set all high order bits to zero. As the lock table contains 2^22 entries in our implementation, we just perform logical AND operation between shifted address and 2^22 −1 to get the index into the table. Figure 1 depicts the mapping scheme. Having multiple consecutive memory words mapped to the same lock table entry can result in false conflict, when unrelated memory words get locked together, but this does not cause any problems in practice.