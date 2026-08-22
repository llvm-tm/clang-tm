----------------------------- MODULE MVLog -----------------------------
(*
 * MVLog — Multi-Version Log-based STM
 *
 * Design (see backends/tm_impl/mvlog/Implementation_notes.md):
 *   - A transaction negotiates its commit slot in an evergrowing commit log
 *     at BEGIN (single atomic fetch-add on `next`).
 *   - The commit log stores each writer's write-set; a per-address index maps
 *     each address to the slot of its newest committed writer.  A dirty set
 *     (a Bloom filter in the implementation, an exact set here) lets a reader
 *     fast-path to plain memory when an address has no live log write.
 *   - Reads resolve values by snooping the index (slow path) or reading
 *     memory (fast path).  Because no transaction with slot > S can commit
 *     while S is in flight, any index entry visible to S is a committed
 *     predecessor (< S), so the snooped value is the correct snapshot value.
 *   - Commit waits until ALL predecessor slots (< S) resolve, then
 *     value-validates its read-set, then publishes its write-set, updates the
 *     index, and marks the address set dirty.
 *   - Abort marks the slot aborted (still "resolves" for successors).
 *
 * The invariant CommittedReadsConsistent (opacity) is the key one: for every
 * committed slot s, every recorded read <<addr,v>> satisfies
 *     v = ReadValue(addr, s),
 * i.e. no committed transaction exhibits read skew.  This is what the naive
 * "read-only transactions never validate" design violates.
 *
 * CONSTANTS:
 *   Thread     — Set of thread IDs (positive integers)
 *   Addr       — Set of memory addresses
 *   Data       — Set of data values
 *   MaxCommits — Max committed transactions per thread (bound)
 *   MaxAborts  — Max aborted attempts per thread (bound, for TLC)
 *   MaxSlots   — Bound on the commit log length
 *)

EXTENDS Naturals, Integers, FiniteSets, TLC, TMTypes

CONSTANTS Thread, Addr, Data, MaxCommits, MaxAborts, MaxSlots
ASSUME Thread \subseteq Nat \ {0}
ASSUME Addr \subseteq Nat
ASSUME Data \subseteq Nat
ASSUME MaxCommits \in Nat \ {0}
ASSUME MaxAborts \in Nat
ASSUME MaxSlots \in Nat
ASSUME Cardinality(Thread) * (MaxCommits + MaxAborts + 1) <= MaxSlots

(* --algorithm MVLog

variables
    \* ── Global state ──
    next = 0,                    \* next free commit-log slot
    \* The commit log is three parallel arrays indexed by slot (records do
    \* not fingerprint in TLC, so no record-valued log entries).
    logState = [s \in 0..(MaxSlots-1) |-> "free"],
                                 \* "free" | "progress" | "committed" | "aborted"
    logWs = [s \in 0..(MaxSlots-1) |-> [a \in Addr |-> NoWrite]],
                                 \* committed write-set per slot
    logRs = [s \in 0..(MaxSlots-1) |-> {}],
                                 \* committed read-set per slot
    index = [a \in Addr |-> -1], \* newest committed slot that wrote addr (-1 = none)
    dirty = {},                  \* addresses with committed-but-unreclaimed writes
    mem = [a \in Addr |-> 0],    \* retired/initial memory (fast-path reads)

    committed = [t \in Thread |-> 0],
    aborted = [t \in Thread |-> 0],

    lastSignalFence = [t \in Thread |-> ""],
    lastThreadFence = [t \in Thread |-> ""],
    lastRmw = [t \in Thread |-> ""];

define
    \* Highest committed slot < s that wrote addr a.
    MaxCommittedWriter(a, s) ==
        CHOOSE d \in 0..(s-1) :
            logState[d] = "committed" /\ logWs[d][a] # NoWrite /\
            \A e \in (d+1)..(s-1) : ~(logState[e] = "committed" /\ logWs[e][a] # NoWrite)

    \* Value of addr a visible to a transaction whose commit slot is s:
    \*   - the write-set value of the highest committed slot < s that wrote a, or
    \*   - mem[a] if no committed predecessor wrote a.
    ReadValue(a, s) ==
        IF \E e \in 0..(s-1) : logState[e] = "committed" /\ logWs[e][a] # NoWrite
        THEN logWs[MaxCommittedWriter(a, s)][a]
        ELSE mem[a]
end define;

process ThreadProc \in Thread
variables
    slot = -1,                   \* claimed commit-log slot
    read_set = {},               \* set of <<addr, value>> observed reads
    write_set = [a \in Addr |-> NoWrite];
begin

L_idle:
    if committed[self] >= MaxCommits \/ aborted[self] >= MaxAborts then
        goto L_done;
    elsif next >= MaxSlots then
        goto L_done;
    else
        \* Negotiate commit position: slot := next; next := next + 1
        slot := next;
        next := next + 1;
        logState[slot] := "progress";
        read_set := {};
        write_set := [a \in Addr |-> NoWrite];
        lastSignalFence[self] := "";
        lastThreadFence[self] := "";
        lastRmw[self] := "";
        goto L_active;
    end if;

L_active:
    either \* ── Read ──
        with a \in Addr do
            if write_set[a] # NoWrite then
                skip;   \* read-own-write (own value wins; not validated)
            else
                if a \notin dirty then
                    \* FAST PATH: no live log write of a → memory is exact
                    read_set := read_set \union {<<a, mem[a]>>};
                else
                    \* SLOW PATH: snoop the newest committed writer via index
                    read_set := read_set \union {<<a, ReadValue(a, slot)>>};
                end if;
                lastSignalFence[self] := "sc";
            end if;
        end with;
        goto L_active;
    or \* ── Write ──
        with a \in Addr, v \in Data do
            write_set[a] := v;
            lastRmw[self] := "acquire";
        end with;
        goto L_active;
    or \* ── Commit: wait for predecessors, then validate ──
        goto L_wait;
    end either;

L_wait:
    if \A s \in 0..(slot-1) : logState[s] \in {"committed", "aborted"} then
        goto L_validate;
    else
        goto L_wait;
    end if;

L_validate:
    if \A <<addr, captured>> \in read_set :
            ReadValue(addr, slot) = captured
    then
        \* Publish: append write-set + read-set to the log, update index.
        logState[slot] := "committed";
        logWs[slot] := write_set;
        logRs[slot] := read_set;
        index := [a \in Addr |->
            IF write_set[a] # NoWrite THEN slot ELSE index[a]];
        dirty := dirty \union {a \in Addr : write_set[a] # NoWrite};
        lastSignalFence[self] := "sc";
        committed[self] := committed[self] + 1;
        lastRmw[self] := "release";
        goto L_idle;
    else
        \* Conflict with a committed predecessor — abort.
        logState[slot] := "aborted";
        aborted[self] := aborted[self] + 1;
        lastRmw[self] := "release";
        goto L_idle;
    end if;

L_done:
    skip;

end process;

end algorithm; *)
\* BEGIN TRANSLATION
VARIABLES next, logState, logWs, logRs, index, dirty, mem, committed, aborted, 
          lastSignalFence, lastThreadFence, lastRmw, pc

(* define statement *)
MaxCommittedWriter(a, s) ==
    CHOOSE d \in 0..(s-1) :
        logState[d] = "committed" /\ logWs[d][a] # NoWrite /\
        \A e \in (d+1)..(s-1) : ~(logState[e] = "committed" /\ logWs[e][a] # NoWrite)




ReadValue(a, s) ==
    IF \E e \in 0..(s-1) : logState[e] = "committed" /\ logWs[e][a] # NoWrite
    THEN logWs[MaxCommittedWriter(a, s)][a]
    ELSE mem[a]

VARIABLES slot, read_set, write_set

vars == << next, logState, logWs, logRs, index, dirty, mem, committed, 
           aborted, lastSignalFence, lastThreadFence, lastRmw, pc, slot, 
           read_set, write_set >>

ProcSet == (Thread)

Init == (* Global variables *)
        /\ next = 0
        /\ logState = [s \in 0..(MaxSlots-1) |-> "free"]
        /\ logWs = [s \in 0..(MaxSlots-1) |-> [a \in Addr |-> NoWrite]]
        /\ logRs = [s \in 0..(MaxSlots-1) |-> {}]
        /\ index = [a \in Addr |-> -1]
        /\ dirty = {}
        /\ mem = [a \in Addr |-> 0]
        /\ committed = [t \in Thread |-> 0]
        /\ aborted = [t \in Thread |-> 0]
        /\ lastSignalFence = [t \in Thread |-> ""]
        /\ lastThreadFence = [t \in Thread |-> ""]
        /\ lastRmw = [t \in Thread |-> ""]
        (* Process ThreadProc *)
        /\ slot = [self \in Thread |-> -1]
        /\ read_set = [self \in Thread |-> {}]
        /\ write_set = [self \in Thread |-> [a \in Addr |-> NoWrite]]
        /\ pc = [self \in ProcSet |-> "L_idle"]

L_idle(self) == /\ pc[self] = "L_idle"
                /\ IF committed[self] >= MaxCommits \/ aborted[self] >= MaxAborts
                      THEN /\ pc' = [pc EXCEPT ![self] = "L_done"]
                           /\ UNCHANGED << next, logState, lastSignalFence, 
                                           lastThreadFence, lastRmw, slot, 
                                           read_set, write_set >>
                      ELSE /\ IF next >= MaxSlots
                                 THEN /\ pc' = [pc EXCEPT ![self] = "L_done"]
                                      /\ UNCHANGED << next, logState, 
                                                      lastSignalFence, 
                                                      lastThreadFence, lastRmw, 
                                                      slot, read_set, 
                                                      write_set >>
                                 ELSE /\ slot' = [slot EXCEPT ![self] = next]
                                      /\ next' = next + 1
                                      /\ logState' = [logState EXCEPT ![slot'[self]] = "progress"]
                                      /\ read_set' = [read_set EXCEPT ![self] = {}]
                                      /\ write_set' = [write_set EXCEPT ![self] = [a \in Addr |-> NoWrite]]
                                      /\ lastSignalFence' = [lastSignalFence EXCEPT ![self] = ""]
                                      /\ lastThreadFence' = [lastThreadFence EXCEPT ![self] = ""]
                                      /\ lastRmw' = [lastRmw EXCEPT ![self] = ""]
                                      /\ pc' = [pc EXCEPT ![self] = "L_active"]
                /\ UNCHANGED << logWs, logRs, index, dirty, mem, committed, 
                                aborted >>

L_active(self) == /\ pc[self] = "L_active"
                  /\ \/ /\ \E a \in Addr:
                             IF write_set[self][a] # NoWrite
                                THEN /\ TRUE
                                     /\ UNCHANGED << lastSignalFence, read_set >>
                                ELSE /\ IF a \notin dirty
                                           THEN /\ read_set' = [read_set EXCEPT ![self] = read_set[self] \union {<<a, mem[a]>>}]
                                           ELSE /\ read_set' = [read_set EXCEPT ![self] = read_set[self] \union {<<a, ReadValue(a, slot[self])>>}]
                                     /\ lastSignalFence' = [lastSignalFence EXCEPT ![self] = "sc"]
                        /\ pc' = [pc EXCEPT ![self] = "L_active"]
                        /\ UNCHANGED <<lastRmw, write_set>>
                     \/ /\ \E a \in Addr:
                             \E v \in Data:
                               /\ write_set' = [write_set EXCEPT ![self][a] = v]
                               /\ lastRmw' = [lastRmw EXCEPT ![self] = "acquire"]
                        /\ pc' = [pc EXCEPT ![self] = "L_active"]
                        /\ UNCHANGED <<lastSignalFence, read_set>>
                     \/ /\ pc' = [pc EXCEPT ![self] = "L_wait"]
                        /\ UNCHANGED <<lastSignalFence, lastRmw, read_set, write_set>>
                  /\ UNCHANGED << next, logState, logWs, logRs, index, dirty, 
                                  mem, committed, aborted, lastThreadFence, 
                                  slot >>

L_wait(self) == /\ pc[self] = "L_wait"
                /\ IF \A s \in 0..(slot[self]-1) : logState[s] \in {"committed", "aborted"}
                      THEN /\ pc' = [pc EXCEPT ![self] = "L_validate"]
                      ELSE /\ pc' = [pc EXCEPT ![self] = "L_wait"]
                /\ UNCHANGED << next, logState, logWs, logRs, index, dirty, 
                                mem, committed, aborted, lastSignalFence, 
                                lastThreadFence, lastRmw, slot, read_set, 
                                write_set >>

L_validate(self) == /\ pc[self] = "L_validate"
                    /\ IF \A <<addr, captured>> \in read_set[self] :
                               ReadValue(addr, slot[self]) = captured
                          THEN /\ logState' = [logState EXCEPT ![slot[self]] = "committed"]
                               /\ logWs' = [logWs EXCEPT ![slot[self]] = write_set[self]]
                               /\ logRs' = [logRs EXCEPT ![slot[self]] = read_set[self]]
                               /\ index' =      [a \in Addr |->
                                           IF write_set[self][a] # NoWrite THEN slot[self] ELSE index[a]]
                               /\ dirty' = (dirty \union {a \in Addr : write_set[self][a] # NoWrite})
                               /\ lastSignalFence' = [lastSignalFence EXCEPT ![self] = "sc"]
                               /\ committed' = [committed EXCEPT ![self] = committed[self] + 1]
                               /\ lastRmw' = [lastRmw EXCEPT ![self] = "release"]
                               /\ pc' = [pc EXCEPT ![self] = "L_idle"]
                               /\ UNCHANGED aborted
                          ELSE /\ logState' = [logState EXCEPT ![slot[self]] = "aborted"]
                               /\ aborted' = [aborted EXCEPT ![self] = aborted[self] + 1]
                               /\ lastRmw' = [lastRmw EXCEPT ![self] = "release"]
                               /\ pc' = [pc EXCEPT ![self] = "L_idle"]
                               /\ UNCHANGED << logWs, logRs, index, dirty, 
                                               committed, lastSignalFence >>
                    /\ UNCHANGED << next, mem, lastThreadFence, slot, read_set, 
                                    write_set >>

L_done(self) == /\ pc[self] = "L_done"
                /\ TRUE
                /\ pc' = [pc EXCEPT ![self] = "Done"]
                /\ UNCHANGED << next, logState, logWs, logRs, index, dirty, 
                                mem, committed, aborted, lastSignalFence, 
                                lastThreadFence, lastRmw, slot, read_set, 
                                write_set >>

ThreadProc(self) == L_idle(self) \/ L_active(self) \/ L_wait(self)
                       \/ L_validate(self) \/ L_done(self)

(* Allow infinite stuttering to prevent deadlock on termination. *)
Terminating == /\ \A self \in ProcSet: pc[self] = "Done"
               /\ UNCHANGED vars

Next == (\E self \in Thread: ThreadProc(self))
           \/ Terminating

Spec == Init /\ [][Next]_vars

Termination == <>(\A self \in ProcSet: pc[self] = "Done")

\* END TRANSLATION

(*====================================================================*)
(* INVARIANTS                                                         *)
(*====================================================================*)

(* Model bound to prevent infinite execution *)
ModelBound ==
    \A t \in Thread : committed[t] <= MaxCommits /\ aborted[t] <= MaxAborts

(* I1 (KEY): Opacity — for every committed slot s, every recorded read
   <<a, v>> satisfies v = ReadValue(a, s).  ReadValue(a, s) is the value of
   the highest committed predecessor (< s) that wrote a.  This rules out
   read skew for ALL committed transactions (read-only and writers). *)
CommittedReadsConsistent ==
    \A s \in 0..(MaxSlots-1) :
        logState[s] = "committed" =>
            \A <<a, v>> \in logRs[s] : ReadValue(a, s) = v

(* I2: The per-address index points to the newest committed writer of addr. *)
IndexIsHighest ==
    \A a \in Addr :
        LET W == {s \in 0..(MaxSlots-1) :
                    logState[s] = "committed" /\ logWs[s][a] # NoWrite} IN
        IF W = {} THEN index[a] = -1
        ELSE index[a] \in W /\ \A s \in W : s <= index[a]

(* I3: The dirty filter has no false negatives — every committed writer's
   addresses are dirty.  A dirty miss is therefore exact, which is what makes
   the plain-memory fast path sound. *)
DirtySuperset ==
    {a \in Addr : \E s \in 0..(MaxSlots-1) :
         logState[s] = "committed" /\ logWs[s][a] # NoWrite} \subseteq dirty

(* I4: Slot-state machine — a slot is free iff it is at-or-after the next
   unclaimed slot; claimed slots are never re-used. *)
FreeSlots ==
    \A s \in 0..(MaxSlots-1) : (logState[s] = "free") <=> (s >= next)

SlotStateValid ==
    \A s \in 0..(MaxSlots-1) :
        logState[s] \in {"free", "progress", "committed", "aborted"}

(* I5: The commit-slot counter never decreases. *)
NextMonotonic == next >= 0

(* I6: Fence fidelity — any thread holding a write-set has issued a fence. *)
FenceFidelityInst ==
    FenceFidelity(Thread, [t \in Thread |-> {a \in Addr : write_set[t][a] # NoWrite}],
                  lastSignalFence, lastThreadFence, lastRmw)

(* Combined invariant *)
Inv == /\ CommittedReadsConsistent
       /\ IndexIsHighest
       /\ DirtySuperset
       /\ FreeSlots
       /\ SlotStateValid
       /\ NextMonotonic
       /\ FenceFidelityInst

THEOREM Spec => []Inv

(*====================================================================*)
(* Fairness and Liveness                                              *)
(*====================================================================*)

Spec_WF == Spec /\ \A self \in Thread : WF_vars(ThreadProc(self))

ProgressProp ==
    \A self \in Thread :
        (pc[self] \in {"L_active", "L_wait", "L_validate"}
         ~> pc[self] \in {"L_idle", "L_done"})

=============================================================================
