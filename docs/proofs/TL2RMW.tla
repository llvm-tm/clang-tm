-------------------------- MODULE TL2RMW ---------------------------
(*
 * TL2RMW — Serializability / lost-update check for the TL2 read+commit
 * protocol AS IMPLEMENTED in backends/tm_impl/tl2/tl2.hpp.
 *
 * Unlike TL2.tla (which only checks guard bookkeeping and collapses the
 * read into one atomic step), this model ties a READ VALUE into a later
 * WRITE (a true read-modify-write: buf := val + 1) so that a LOST UPDATE
 * is observable.  It models the implementation's read as TWO separate
 * memory accesses (observe guard, then load *addr) with NO re-check of
 * the guard after loading the value — exactly the "missing double-check
 * read" the prose in docs/proofs.md §3.2 assumes is present.
 *
 * If the commit-time version validation still guarantees the counter
 * invariant, the two-phase read is serializable and the missing
 * double-check is NOT the cause of the bank_tl2 money drift (B-21).
 *
 * Protocol modeled (faithful to tl2.hpp):
 *   begin        : pc I -> RO
 *   read obs     : observed := VersionOf(guard)   (atomic guard load;
 *                  records version regardless of lock bit — see :615)
 *   read value   : val := mem                      (plain load, SEPARATE
 *                  step; a concurrent commit may interleave here)
 *   write        : buf := val + 1
 *   lock         : spin until LockBit(guard)=0 then set it (:332 CAS)
 *   validate     : if VersionOf(guard) = observed commit else abort
 *                  (abort clears lock bit, keeps version — :745)
 *   write-back   : mem := buf
 *   release      : guard := MakeEntry(clock+1); clock++ ; commits++
 *)

EXTENDS Naturals

CONSTANTS Thread, MaxCommits

ASSUME Thread \subseteq Nat
ASSUME MaxCommits \in Nat

(* version/lock encoding: bit0 = lock, rest = version (tl2.hpp:76-77) *)
LockBit(e)  == e % 2
VersionOf(e) == e \div 2
MakeEntry(v) == v * 2

VARIABLES pc, clock, guard, mem, observed, val, buf, commits, ctr

vars == <<pc, clock, guard, mem, observed, val, buf, commits, ctr>>

Init ==
    /\ clock = 0
    /\ guard = 0
    /\ mem = 0
    /\ commits = 0
    /\ observed = [t \in Thread |-> 0]
    /\ val      = [t \in Thread |-> 0]
    /\ buf      = [t \in Thread |-> 0]
    /\ ctr      = [t \in Thread |-> 0]
    /\ pc       = [t \in Thread |-> "I"]

Begin(t) ==
    /\ pc[t] = "I"
    /\ pc' = [pc EXCEPT ![t] = "RO"]
    /\ UNCHANGED <<clock, guard, mem, observed, val, buf, commits, ctr>>

ReadObs(t) ==
    /\ pc[t] = "RO"
    /\ observed' = [observed EXCEPT ![t] = VersionOf(guard)]
    /\ pc' = [pc EXCEPT ![t] = "RV"]
    /\ UNCHANGED <<clock, guard, mem, val, buf, commits, ctr>>

ReadVal(t) ==
    /\ pc[t] = "RV"
    /\ val' = [val EXCEPT ![t] = mem]
    /\ pc' = [pc EXCEPT ![t] = "WR"]
    /\ UNCHANGED <<clock, guard, mem, observed, buf, commits, ctr>>

Write(t) ==
    /\ pc[t] = "WR"
    /\ buf' = [buf EXCEPT ![t] = val[t] + 1]
    /\ pc' = [pc EXCEPT ![t] = "LK"]
    /\ UNCHANGED <<clock, guard, mem, observed, val, commits, ctr>>

Lock(t) ==
    /\ pc[t] = "LK"
    /\ LockBit(guard) = 0
    /\ guard' = guard + 1
    /\ pc' = [pc EXCEPT ![t] = "VC"]
    /\ UNCHANGED <<clock, mem, observed, val, buf, commits, ctr>>

ValidateOK(t) ==
    /\ pc[t] = "VC"
    /\ VersionOf(guard) = observed[t]
    /\ pc' = [pc EXCEPT ![t] = "WB"]
    /\ UNCHANGED <<clock, guard, mem, observed, val, buf, commits, ctr>>

ValidateAbort(t) ==
    /\ pc[t] = "VC"
    /\ VersionOf(guard) # observed[t]
    /\ guard' = MakeEntry(VersionOf(guard))
    /\ pc' = [pc EXCEPT ![t] = "I"]
    /\ UNCHANGED <<clock, mem, observed, val, buf, commits, ctr>>

(* Atomic commit: write-back + version release happen together while the
   guard is held (tl2.hpp step 7 :760-790).  Modeling them as one step keeps
   mem and the commit counter consistent for other lock-aware threads; the
   impl guarantees this atomicity because the guard is held across both. *)
Commit(t) ==
    /\ pc[t] = "WB"
    /\ mem' = buf[t]
    /\ guard' = MakeEntry(clock + 1)
    /\ clock' = clock + 1
    /\ commits' = commits + 1
    /\ ctr' = [ctr EXCEPT ![t] = ctr[t] + 1]
    /\ pc' = [pc EXCEPT ![t] = IF ctr[t] + 1 >= MaxCommits THEN "D" ELSE "I"]
    /\ UNCHANGED <<observed, val, buf>>

Done(t) ==
    /\ pc[t] = "D"
    /\ UNCHANGED vars

Nxt(t) ==
    \/ Begin(t)
    \/ ReadObs(t)
    \/ ReadVal(t)
    \/ Write(t)
    \/ Lock(t)
    \/ ValidateOK(t)
    \/ ValidateAbort(t)
    \/ Commit(t)
    \/ Done(t)

Next == \E t \in Thread : Nxt(t)

Spec == Init /\ [][Next]_vars

(* ---- Invariants ------------------------------------------------ *)

(* No lost update: applied increments equal successful commits *)
LostUpdate == mem = commits

(* Lock bookkeeping: guard is locked iff some thread owns it mid-commit *)
LockInv ==
    LockBit(guard) = 1 <=> \E t \in Thread : pc[t] \in {"VC", "WB"}

(* A transaction never increments past MaxCommits *)
Bounded == \A t \in Thread : ctr[t] <= MaxCommits

Inv == /\ LostUpdate
       /\ LockInv
       /\ Bounded

THEOREM Spec => []Inv

======================================================================
