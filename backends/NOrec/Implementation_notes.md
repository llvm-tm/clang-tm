# A Fast TM

NOrec combines three key ideas: (1) a single global sequence lock---shared with our simpler Transactional Mutex Lock (TML) system [36]; (2) an indexed write set, as in our work on contention management for word-based, blocking TMs [35]; and (3) value-based conflict detection, as in the JudoSTM of Olszewski et al. [29]. The resulting TM is both fast at low thread counts and surprisingly scalable---even on multi-chip machines.

## Design

In a future where hardware TM is common on many-core chips, STM will be important primarily in smaller and legacy systems, and as a software fall-back when hardware resources are exhausted. In these contexts an algorithm’s instrumentation overhead is at least as important as its scalability. We describe the design of NOrec by starting with the lowest overhead STM algorithm we know of, and adding the minimum overhead needed for satisfactory scalability.

**Single Global Sequence Lock:**
Our minimum overhead algorithm is the Transactional Mutex Lock (TML) [36], which uses a global sequence lock [17] to serialize writer transactions. A sequence lock resembles a reader-writer lock in which a reader can upgrade to writer status at any time, but must be prepared to restart its critical section if one of its peers upgrades.

The primary advantage of a sequence lock over a traditional reader-writer lock is that readers are invisible, and need not induce the coherence overhead associated with updating the lock datastructure. The primary disadvantage is that doomed readers may be active at the same time as a writer, and may read inconsistent values from memory, leading to potentially erroneous behavior, especially in pointer-based algorithms. Some problems can be avoided by (typically manual) sandboxing, but using the lock in this way is error prone, and still does not solve problems related to memory deallocation. Sequence locks are used in the Linux kernel to protect read-mostly structures where dynamic memory allocation is not required; traditional reader-writer locks and read-copy-update (RCU) [24] are used in other cases.

TML integrates the concept of a single global sequence lock with eager conflict detection and in-place updates. The STM write barrier acquires the lock for writing, the read barrier checks the lock version to ensure consistency, and the STM’s built-in memory management system handles any dangerous deallocation situations. The result is a very low overhead STM that is highly scalable in read-mostly workloads, as seen in our performance results (Section 4).

The simplicity of the resulting algorithm facilitates compiler optimization [36], but the results presented here do not consider these. A side effect of having a single lock is that the resulting algorithm, while blocking, is clearly livelock free. This property is preserved through the following modifications, which we make to improve scalability.

There are two impediments to scaling in TML. First, the eager, in-place nature of the algorithm, when combined with its single
lock, means that only one writer can ever be active at a time. Second, invisible readers must be extremely conservative and assume that they may have been invalidated by any writer.

**Lazy/Redo:**
Our first extension is to use lazy conflict detection and a redo log, for concurrent speculative writers. Updates are buffered in a write log, which we must search on each read to satisfy possible read-after-write hazards. We use a linear write log indexed by a linear-probed hash table with versioned buckets to support O(1) clearing---a structure that was shown to scale well in our work on word-based contention management [35]. Writing transactions do not attempt to acquire the sequence lock until their commit points, allowing speculative readers and writers to proceed concurrently.

The primary benefit of this extension is to shrink the period of time that a writer holds the lock, increasing the likelihood that concurrent read-only transactions will commit.

**Value-Based Validation:**
We would also like to allow transactions, both readers and writers, to detect if they have actually been invalidated by a committing writer, rather than making a conservative assumption. The typical detection mechanism associates transactional metadata with each data location: word-based systems usually use a table of ownership records (orecs). Most of the complexity in traditional STMs is in correctly and efficiently maintaining these orecs. The read barrier in a typical "invisible" reader system inspects both the location read and its associated orec, storing the location and possibly information from its orec in a read set data structure. The transaction then re-checks the orecs during validation, possibly comparing to previously saved values, to see whether all its reads remain mutually consistent (i.e., could have occurred at the same instant in time).

The alternative to metadata-based validation is value-based validation (VBV), used by Harris & Fraser as a "second chance" validation scheme in their work on revocable locks [15], by Ding et al. in their work on speculative parallelization using process-level virtual memory [10], and by Olszewski et al. in JudoSTM [29]. Rather than logging the address of an ownership recodr, a VBV read barrier logs the address of the location and the value read. Validation consists of re-reading the addresses and verifying that there exists a time (namely now) at which all of the transaction’s reads could have occurred atomically.

VBV employs no shared metadata, issues no atomic read-modify-write instructions, and introduces no false conflicts above the level of a word. Fortuitously, our global sequence lock provides a natural "consistent snapshot" capability for validation.

Lazy conflict detection, buffered updates, and VBV allow active transactions to "survive" through a nonconflicting writer’s commit. This adds significant scalability to NOrec in workloads where writers are common or transactions are long. The resulting algorithm has one main scalability bottleneck remaining: the sequence lock provides for only a single active committing writer at a time. This is the limitation that would likely make it unsuitable as the primary TM mechanism on a machine with hundreds of cores. To minimize this commit bottleneck, we arrange for validation to occur before lock acquisition, a technique pioneered in RingSTM [34]. Details appear in the following section.


**Listing 1:** NOrec Metadata
```
1: volatile unsigned global lock
2: local unsigned lock snapshot
3: local List <Address, Value> reads
4: local Hash<Address, Value> writes
```


## Implementation

Metadata NOrec requires little shared metadata, and very little metadata overall (Listing 1). The sequence lock is simply a shared unsigned integer. Each transaction maintains a thread local snapshot of the lock, as well as a list of address/value pairs for a read log, and a hashtable representation of a write set. As is standard, the implementation stores these thread locals, along with a few others (e.g., jump buffers used during aborts), as part of a transaction Descriptor, which is then an explicit parameter to the STM API calls.

Our current implementation logs values as unsigned words. Clients of the TM interface are responsible for appropriate alignment and type modifications, and for splitting operations on larger types into multiple calls to TXRead or TXWrite.


**Listing 2:** NOrec Validation
```
unsigned Validate ()
1:  while ( true )
2:      time = global_lock
3:      if ((time & 1) != 0)
4:          continue
5:
6:      for each (addr, val ) in reads
7:          if (*addr != val)
8:              TXAbort() // abort will longjmp
9:
10:         if (time == global_lock)
11:             return time
```


**Validation:**
Validation is a simple consistent snapshot algorithm (Listing 2). We start by reading the global lock’s version number in line 2, spinning if there is a writer currently holding the lock. Lines 6--8 loop through the read log, verifying that locations still contain the values seen by earlier reads. Lines 10 and 11 verify that validation occurred without interference by a committing writer, restarting the validation if this is not true. The Validate routine returns the time at which the validation succeeded. This time is used by the calling transaction to update its snapshot value. This mechanism resembles the extendable timestamps of Riegel et al. [30], with important differences that we cover in Section 3.2.


**Listing 3:** Transaction Begin
```
void TXBegin()
1: do
2:     snapshot = global_lock
3: while ((snapshot & 1) != 0)
```


**TXBegin:**
Beginning a transaction in NOrec simply entails reading the sequence lock, spinning if it is currently held by a committing writer. This snapshot value indicates the most recent time at which the transaction was known to be consistent.


**Listing 4:** Read Barrier
```
Value TXRead(Address addr)
1:  if (writes.contains (addr))
2:      return writes [addr]
3:
4:  val = *addr
5:  while (snapshot != global lock)
6:      snapshot = Validate()
7:      val = *addr
8:
9:  reads.append(address, value)
10: return val
```


**TXRead:**
Given lazy conflict detection and buffered updates, the read barrier first checks if we have already written this location (lines 1 and 2). If not, we read a value from memory (line 4). Lines 5--7 provide opacity [14] via post-validation. Opacity is crucial for unmanaged (non-sandboxed) languages: it guarantees that even a speculative transaction will never see inconsistent state.

Line 5 compares the sequence lock to the local snapshot. If the snapshot is out of date, line 6 validates to confirm that the transaction is still consistent, capturing the returned time as the new local snapshot. Line 7 rereads the memory location and returns to line 5 to try again.

Lines 9 and 10 log the address/value pair for future validation, and return the value read. The reads log is currently an append-only list, allowing us to detect inconsistent reads during validation. An address may appear multiple times in the list. In a data-race-free program, post-validation for opacity guarantees that all entries for a given location contain the same value. Programs with data races can result in entries with different values---in which case subsequent
validation will fail, as it should.

An alternative would be to store the read set in a hashed structure as we do the write set, guaranteeing a unique value for each address. The read barrier would then have two options: (1) always look up the address in the read set first, returning the found value if one exists, or (2) always read the actual location first, and abort if an inconsistent value exists in the read set. The first option can be considered optimistic, in the sense that at the time of the read a conflict may exist that will be “fixed" by some future committing transaction that restores the earlier value. The optimistic option can tolerate this temporary conflict and commit successfully, where the second, pessimistic option must abort.

We expect that the lower constant overhead of the list combined with its simpler read barrier logic will result in better performance in most applications. It also greatly simplifies closed nesting implementations (Section 5.1). The most compelling reason to use a hashed set is to accommodate programs in which locations are reread frequently and validation is also frequent; here the list has validation cost proportional to the number of reads performed, while the hash is proportional to the number of locations read.


**Listing 5:** Write Barrier
```
void TXWrite(Address addr, Value val)
1: writes [addr] = val
```


TXWrite The write barrier (Listing 5) simply logs the value writ-
ten using a simple hash-based set. Details of the set implementation
are given elsewhere [31, 35].


**Listing 6:** Transaction Commit
```
void TXCommit()
1:  if (read−only transaction )
2:      return
3:
4:  while (! CAS(&global lock, snapshot, snapshot + 1))
5:      snapshot = Validate()
6:
7:  for each (addr, val ) in writes
8:      *addr = val
9:
10: global lock = snapshot + 2 // one more than CAS above
```


**TXCommit:**
All transactions enter their commit protocol (Listing 6) with a snapshot of the sequence lock, and are guaranteed, due to post-validation in the read barrier (Listing 4, Lines 5-7), to have been consistent as of that snapshot. We exploit this property in both the read-only and writer commit protocol.

A read-only transaction linearizes at the last time that it was proven consistent, i.e., snapshot time. No additional work is required at commit for such transactions (lines 1 and 2 of Listing 6).

A writer transaction will attempt to atomically increment the sequence lock using a compare-and-swap (CAS) instruction, using its snapshot time as the expected prior value. If this CAS succeeds, then the writer cannot have been invalidated by a second writer: no further validation is required. A failed CAS indicates a need for validation because a concurrent writer committed. Line 5 in Listing 6 performs this validation and moves the snapshot forward, preparing the writer for another commit attempt. As in RingSTM, transactions never hold the commit lock while validating, minimizing the underlying serial bottleneck of single-writer commit.


**Listing 7:** Publication
```
initially p == null, published == false
   T1:                       |  T2:
1: p = new foo()             |  atomic {
2: atomic {                  |      if ( published )
3:     published = true      |          val = p−>x
4: }                         |  }
```


**Listing 8:** Privatization
```
initially p != null , published == true
   T1:                       |  T2:
1: atomic {                  |  atomic {
2:     published = false     |      if ( published )
3: }                         |          val = p−>x
4: p = null                  |      }
```


**Listing 9:** Publication via empty transaction
```
initially n == 0, published == false
   T1:                       |  T2:
1: n = 1                     |  atomic {
2: atomic { }                |      v = n
3: published = true          |      f = published
4:                           |  }
```

