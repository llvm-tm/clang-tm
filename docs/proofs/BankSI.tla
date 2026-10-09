------------------------------ MODULE BankSI ------------------------------
(*
 * BankSI — snapshot isolation, the anomaly, and why serializability is
 * stronger, for Book Ch. 7 (isolation menu).  Companion mechanisms in
 * the repo: the multi-version readers of backends/tm_impl/jvstm and
 * mvlog, and the snapshot threshold of GPU CSMV / GUST (Ch. 18).
 *
 * Two transactions withdraw from two accounts.  The bank rule couples
 * the two accounts: "the combined reserve must stay >= RESERVE".  Under
 * SERIALIZABILITY the second transaction reads the first's effect and
 * the rule holds; under SNAPSHOT ISOLATION both read the same snapshot,
 * each rule-check passes locally, and neither write conflicts with the
 * other (disjoint write-sets) — both commit and the reserve collapses.
 * This is the write-skew anomaly (P4 in the Berenson taxonomy): legal
 * under SI, illegal under any serial execution.
 *
 * The invariant Reserve IS violated by design — `make check-teaching`
 * asserts TLC finds the counterexample, which the book replays.
 *)

EXTENDS Integers

CONSTANTS START, DRAW, RESERVE, SERIALIZABLE

ASSUME START \in Int
ASSUME DRAW \in Int
ASSUME RESERVE \in Int
ASSUME SERIALIZABLE \in BOOLEAN

(* SERIALIZABLE = FALSE: reads never validate (snapshot isolation);
   disjoint write-sets always commit — the anomaly is admitted.
   SERIALIZABLE = TRUE: commit revalidates the whole read-set against
   memory (optimistic serializability, the NOrec/TL2 rule); a stale
   read aborts instead of committing, and the reserve is preserved. *)

VARIABLES pc1, pc2, x, y, sx1, sy1, sx2, sy2

vars == <<pc1, pc2, x, y, sx1, sy1, sx2, sy2>>

Init ==
    /\ pc1 = "I"   /\ pc2 = "I"
    /\ x = START  /\ y = START
    /\ sx1 = 0 /\ sy1 = 0 /\ sx2 = 0 /\ sy2 = 0

(* Transaction 1: snapshot read (sx1, sy1), then conditional withdraw
   from x.  Reads never block and never validate: that is SI's promise
   and its trap.  Writes take first-committer rule; disjoint write-sets
   never conflict, so both commits below always succeed. *)
T1Snap ==
    /\ pc1 = "I"
    /\ sx1' = x  /\ sy1' = y
    /\ pc1' = "decide"
    /\ UNCHANGED <<pc2, x, y, sx2, sy2>>

T1Decide ==
    /\ pc1 = "decide"
    /\ IF sx1 + sy1 - DRAW >= RESERVE
     THEN /\ pc1' = "commit"
     ELSE /\ pc1' = "done"
    /\ UNCHANGED <<pc2, x, y, sx1, sy1, sx2, sy2>>

T1Commit ==
    /\ pc1 = "commit"
    /\ IF ~ SERIALIZABLE
     THEN /\ x' = x - DRAW
          /\ pc1' = "done"
     ELSE /\ IF x = sx1 /\ y = sy1                 \* read-set still valid?
           THEN /\ x' = x - DRAW
                /\ pc1' = "done"
           ELSE /\ pc1' = "aborted"                \* abort: no write applied
                /\ UNCHANGED x
    /\ UNCHANGED <<pc2, y, sx1, sy1, sx2, sy2>>

T2Snap ==
    /\ pc2 = "I"
    /\ sx2' = x  /\ sy2' = y
    /\ pc2' = "decide"
    /\ UNCHANGED <<pc1, x, y, sx1, sy1>>

T2Decide ==
    /\ pc2 = "decide"
    /\ IF sx2 + sy2 - DRAW >= RESERVE
     THEN /\ pc2' = "commit"
     ELSE /\ pc2' = "done"
    /\ UNCHANGED <<pc1, x, y, sx1, sy1, sx2, sy2>>

T2Commit ==
    /\ pc2 = "commit"
    /\ IF ~ SERIALIZABLE
     THEN /\ y' = y - DRAW
          /\ pc2' = "done"
     ELSE /\ IF x = sx2 /\ y = sy2
           THEN /\ y' = y - DRAW
                /\ pc2' = "done"
           ELSE /\ pc2' = "aborted"
                /\ UNCHANGED y
    /\ UNCHANGED <<pc1, x, sx1, sy1, sx2, sy2>>

Next == T1Snap \/ T1Decide \/ T1Commit
     \/ T2Snap \/ T2Decide \/ T2Commit

Spec == Init /\ [][Next]_vars

TypeOK ==
    /\ pc1 \in {"I", "decide", "commit", "done", "aborted"}
    /\ pc2 \in {"I", "decide", "commit", "done", "aborted"}
    /\ x \in Int  /\ y \in Int
    /\ sx1 \in Int /\ sy1 \in Int /\ sx2 \in Int /\ sy2 \in Int

(* The coupling rule: serial executions preserve it; SI does not. *)
Reserve == x + y >= RESERVE

==========================================================================
