------------------------------ MODULE BankWeak ------------------------------
(*
 * BankWeak — three weak-isolation anomalies on one bank, for Book Ch. 7
 * (companion of BankSI.tla, which handles write skew, and of the C++ demo
 * benchmarks/cpp/sidemo/bank_si.cpp, which runs the same scenarios live).
 *
 * One account x = 0.  T1 is a risky deposit: it reads x, prepares
 * x + D1, and may COMMIT or ABORT (nondeterministically — the point is
 * that RC and above must stay safe whichever happens).  T2 is a reader
 * that acts: it reads x TWICE (r1, r2), then commits x <- r1 + D2,
 * basing its write on its FIRST read.
 *
 * The constant LEVEL selects the isolation semantics:
 *   "RU": reads may return T1's pending (uncommitted) value
 *         -> the CommittedReads invariant fails: DIRTY READ.
 *   "RC": reads take the fresh committed value, but each read picks
 *         its own snapshot -> T1 can commit between r1 and r2:
 *         NON-REPEATABLE READ; and acting on a stale r1 while T1
 *         committed in between loses T1's update: LOST UPDATE.
 *   "SI": reads come from one snapshot taken at begin (r1 = r2
 *         structurally), and commit applies first-committer-wins
 *         (a concurrent write to x since the snapshot aborts), so all
 *         three invariants hold.
 *
 * The anomaly configs (BankWeak-ru.cfg, BankWeak-rc.cfg) are INTENTIONALLY
 * violated; `make -C docs/proofs check-teaching` asserts TLC finds exactly
 * those counterexamples.  BankWeak-si.cfg must pass.
 *)

EXTENDS Integers

CONSTANTS D1, D2, LEVEL

ASSUME D1 \in Int
ASSUME D2 \in Int
ASSUME LEVEL \in {"RU", "RC", "SI"}

VARIABLES pc1, pc2, x, haspending, pending, v1, sx1, sx2, r1, r2

vars == <<pc1, pc2, x, haspending, pending, v1, sx1, sx2, r1, r2>>

Init ==
    /\ pc1 = "I"   /\ pc2 = "I"
    /\ x = 0
    /\ haspending = FALSE
    /\ pending = 0
    /\ v1 = 0 /\ sx1 = 0 /\ sx2 = 0 /\ r1 = 0 /\ r2 = 0

(* ---------- T1: risky deposit, may commit or abort ---------- *)

T1Begin ==
    /\ pc1 = "I"
    /\ sx1' = x
    /\ pc1' = "rd"
    /\ UNCHANGED <<pc2, x, haspending, pending, v1, sx2, r1, r2>>

(* One step for the read and the sandboxed write: the read value lands
   in v1, the new balance lands in `pending`, which only RU exposes. *)
T1RW ==
    /\ pc1 = "rd"
    /\ v1' = IF LEVEL = "SI" THEN sx1 ELSE x
    /\ pending' = (IF LEVEL = "SI" THEN sx1 ELSE x) + D1
    /\ haspending' = TRUE
    /\ pc1' = "pend"
    /\ UNCHANGED <<pc2, x, sx1, sx2, r1, r2>>

(* The write is sandboxed: it lands in `pending`, visible to other
   transactions only if LEVEL = "RU" (see TRead below). *)
T1Commit ==
    /\ pc1 = "pend"
    /\ IF LEVEL = "SI" /\ x # sx1
      THEN /\ pc1' = "aborted"              \* WW conflict: first committer won
           /\ haspending' = FALSE
           /\ UNCHANGED <<x, pending>>
      ELSE /\ x' = pending
           /\ haspending' = FALSE
           /\ pc1' = "done"
           /\ UNCHANGED pending
    /\ UNCHANGED <<pc2, v1, sx1, sx2, r1, r2>>

T1Abort ==
    /\ pc1 = "pend"
    /\ haspending' = FALSE
    /\ UNCHANGED <<pc2, x, pending, v1, sx1, sx2, r1, r2>>
    /\ pc1' = "aborted"

(* ---------- T2: read twice, then act on the first read ---------- *)

(* The read function of the isolation level: snapshot, dirty, or fresh. *)
TRead ==
    IF LEVEL = "SI" THEN sx2
    ELSE IF /\ LEVEL = "RU"
            /\ haspending
         THEN pending
         ELSE x

T2Begin ==
    /\ pc2 = "I"
    /\ sx2' = x
    /\ pc2' = "rd1"
    /\ UNCHANGED <<pc1, x, haspending, pending, v1, sx1, r1, r2>>

T2Read1 ==
    /\ pc2 = "rd1"
    /\ r1' = TRead
    /\ pc2' = "rd2"
    /\ UNCHANGED <<pc1, x, haspending, pending, v1, sx1, sx2, r2>>

T2Read2 ==
    /\ pc2 = "rd2"
    /\ r2' = TRead
    /\ pc2' = "wr"
    /\ UNCHANGED <<pc1, x, haspending, pending, v1, sx1, sx2, r1>>

T2Commit ==
    /\ pc2 = "wr"
    /\ IF LEVEL = "SI" /\ x # sx2
      THEN /\ pc2' = "aborted"              \* WW conflict
           /\ UNCHANGED x
      ELSE /\ x' = r1 + D2                  \* acts on the FIRST read
           /\ pc2' = "done"
    /\ UNCHANGED <<pc1, haspending, pending, v1, sx1, sx2, r1, r2>>

Next == T1Begin \/ T1RW \/ T1Commit \/ T1Abort
     \/ T2Begin \/ T2Read1 \/ T2Read2 \/ T2Commit

Spec == Init /\ [][Next]_vars

(* ---------- type invariant ---------- *)

TypeOK ==
    /\ pc1 \in {"I", "rd", "pend", "done", "aborted"}
    /\ pc2 \in {"I", "rd1", "rd2", "wr", "done", "aborted"}
    /\ x \in Int
    /\ haspending \in BOOLEAN
    /\ pending \in Int
    /\ v1 \in Int /\ sx1 \in Int /\ sx2 \in Int
    /\ r1 \in Int /\ r2 \in Int

(* ---------- the three anomaly predicates ---------- *)

(* No read may equal a value that is still only pending: the dirty-read
   predicate.  A match of a recorded read with the live pending value is
   exactly a read-from-above. *)
CommittedReads ==
    ~ (  haspending
      /\ (r1 = pending \/ r2 = pending) )

(* Both reads of T2 agree: the repeatable-read predicate. *)
Repeatable ==
    pc2 \in {"wr", "done", "aborted"} => r1 = r2

(* If both transactions committed, both deposits are in the balance:
   the lost-update predicate. *)
NoLostUpdate ==
    (pc1 = "done" /\ pc2 = "done") => x = D1 + D2

==========================================================================
