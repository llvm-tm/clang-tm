----------------------- MODULE DESEngine -----------------------
(*
 * DESEngine — Cross-LP Conflict Resolution Protocol
 *
 * NOTE: This spec models the pure DES engine (SimState in engine.rs),
 * NOT the real-backend replay engine (SimEngine in sim_engine.rs).
 * See docs/audits/simengine.md for the naming mismatch.
 *
 * Algorithm (from simulator/src/engine.rs, AGENTS.md 2026-06-22):
 *
 *   The DES engine's SimState tracks per-LP in-flight writes/reads:
 *     - in_flight_writes:  set of (lp, addr) tuples
 *     - in_flight_reads:   set of (lp, addr) tuples
 *
 *   Conflict detection on each operation:
 *     - Write(addr) by LP t:  check RAW → if any other LP is reading addr,
 *       that other LP is aborted (older reader loses to newer writer).
 *     - Read(addr) by LP t:   check WAR → if any other LP is writing addr,
 *       that other LP is aborted (older writer loses to newer reader).
 *
 *   Resolution rule: the OLDER in-flight operation is always aborted
 *   (the conflict is detected when the NEWER operation dispatches, and
 *   it aborts the already-in-flight older LP).  This is equivalent to
 *   "newest operation wins" / "older transaction is sacrificed."
 *
 *   SGL fallback mode:
 *     When sgl_mode[t] = TRUE, no other LP can have conflicting addresses.
 *     SGL provides mutual exclusion on all addresses.
 *
 * Invariants (for TLC model checking):
 *   NoConcurrentWrites:    After resolution, at most one LP holds a write on any addr.
 *   SGLMutex:              At most one LP in SGL mode at any time.
 *   SGLIsolation:          If sgl_mode[t] = TRUE, no other LP has in-flight
 *                          writes or reads on any address.
 *   NoOrphanedOps:         If not in a transaction, no in-flight ops.
 *   SGLStateConsistent:    SGL mode implies lp_state = "sgl".
 *   WriteTrackingConsistent: In-flight writes imply active TX.
 *   ReadTrackingConsistent:  In-flight reads imply active TX.
 *   NoSelfConflict:        No LP writes and reads the same address.
 *)

EXTENDS Naturals, FiniteSets, TLC

CONSTANTS
    LP,                 (* Set of LP (thread) IDs *)
    Addr                (* Set of memory addresses *)

ASSUME LP \subseteq Nat
ASSUME Addr \subseteq Nat

(*--------------------------------------------------------------------*)
(* PlusCal algorithm                                                   *)
(*--------------------------------------------------------------------*)
(* --algorithm DESEngine

variables
    in_flight_writes = {},
    in_flight_reads = {},
    in_tx = [lp \in LP |-> FALSE],
    sgl_mode = [lp \in LP |-> FALSE],
    lp_state = [lp \in LP |-> "idle"],
    tx_count = [lp \in LP |-> 0],
    abort_count = [lp \in LP |-> 0],
    conflict_aborts = 0;

fair process Proc \in LP
    variables
        conflicting_writers = {},
        conflicting_readers = {};
begin
L_entry:
    either
        (* BeginTx *)
        when lp_state[self] = "idle" /\ ~in_tx[self]
             /\ \A other \in LP : sgl_mode[other] = FALSE;
        in_tx[self] := TRUE;
        lp_state[self] := "active";
    or
        (* ReadAddr *)
        when lp_state[self] = "active" /\ in_tx[self] = TRUE;
        with a \in Addr do
            conflicting_writers := {w \in LP : <<w, a>> \in in_flight_writes /\ w # self};
            if (conflicting_writers /= {}) then
                in_tx := [w \in LP |-> IF w \in conflicting_writers THEN FALSE ELSE in_tx[w]];
                lp_state := [w \in LP |-> IF w \in conflicting_writers THEN "idle" ELSE lp_state[w]];
                abort_count := [w \in LP |-> IF w \in conflicting_writers THEN abort_count[w] + 1 ELSE abort_count[w]];
                in_flight_writes := in_flight_writes \ {<<w, a2>> : w \in conflicting_writers, a2 \in Addr};
                in_flight_reads := in_flight_reads \ {<<w, a2>> : w \in conflicting_writers, a2 \in Addr};
                conflict_aborts := conflict_aborts + Cardinality(conflicting_writers);
            else
                in_flight_reads := in_flight_reads \cup {<<self, a>>};
            end if;
        end with;
    or
        (* WriteAddr *)
        when lp_state[self] = "active" /\ in_tx[self] = TRUE;
        with a \in Addr do
            conflicting_readers := {r \in LP : <<r, a>> \in in_flight_reads /\ r # self};
            conflicting_writers := {w \in LP \ {self} : <<w, a>> \in in_flight_writes};
            if (conflicting_readers /= {} \/ conflicting_writers /= {}) then
                in_tx := [w \in LP |-> IF w \in conflicting_readers \cup conflicting_writers THEN FALSE ELSE in_tx[w]];
                lp_state := [w \in LP |-> IF w \in conflicting_readers \cup conflicting_writers THEN "idle" ELSE lp_state[w]];
                abort_count := [w \in LP |-> IF w \in conflicting_readers THEN abort_count[w] + 1
                                             ELSE IF w \in conflicting_writers THEN abort_count[w] + 1
                                             ELSE abort_count[w]];
                in_flight_reads := in_flight_reads \ {<<rp, a2>> : rp \in conflicting_readers \union conflicting_writers, a2 \in Addr};
                in_flight_writes := in_flight_writes \ {<<wp, a2>> : wp \in conflicting_readers \union conflicting_writers, a2 \in Addr};
                conflict_aborts := conflict_aborts + Cardinality(conflicting_readers) + Cardinality(conflicting_writers);
            else
                in_flight_writes := in_flight_writes \cup {<<self, a>>};
            end if;
        end with;
    or
        (* CommitTx *)
        when lp_state[self] = "active" /\ in_tx[self] = TRUE;
        in_tx[self] := FALSE;
        in_flight_writes := in_flight_writes \ {<<self, a>> : a \in Addr};
        in_flight_reads := in_flight_reads \ {<<self, a>> : a \in Addr};
        tx_count[self] := tx_count[self] + 1;
        lp_state[self] := "idle";
    or
        (* AbortTx *)
        when lp_state[self] = "active" /\ in_tx[self] = TRUE;
        in_tx[self] := FALSE;
        in_flight_writes := in_flight_writes \ {<<self, a>> : a \in Addr};
        in_flight_reads := in_flight_reads \ {<<self, a>> : a \in Addr};
        abort_count[self] := abort_count[self] + 1;
        lp_state[self] := "idle";
    or
        (* EnterSGL *)
        when \A other \in LP :
            other = self \/
            (sgl_mode[other] = FALSE /\ in_tx[other] = FALSE
             /\ \A a \in Addr : ~(<<other, a>> \in in_flight_writes) /\ ~(<<other, a>> \in in_flight_reads));
        sgl_mode[self] := TRUE;
        lp_state[self] := "sgl";
    or
        (* ExitSGL *)
        when lp_state[self] = "sgl" /\ sgl_mode[self] = TRUE;
        sgl_mode[self] := FALSE;
        in_flight_writes := in_flight_writes \ {<<self, a>> : a \in Addr};
        in_flight_reads := in_flight_reads \ {<<self, a>> : a \in Addr};
        in_tx[self] := FALSE;
        lp_state[self] := "idle";
    or
        (* ConflictAbort *)
        when lp_state[self] = "active" /\ in_tx[self] = TRUE;
        in_tx[self] := FALSE;
        in_flight_writes := in_flight_writes \ {<<self, a>> : a \in Addr};
        in_flight_reads := in_flight_reads \ {<<self, a>> : a \in Addr};
        abort_count[self] := abort_count[self] + 1;
        conflict_aborts := conflict_aborts + 1;
        lp_state[self] := "idle";
    end either;
    goto L_entry;
end process;

end algorithm; *)
\* BEGIN TRANSLATION
VARIABLES in_flight_writes, in_flight_reads, in_tx, sgl_mode, lp_state, 
          tx_count, abort_count, conflict_aborts, pc, conflicting_writers, 
          conflicting_readers

vars == << in_flight_writes, in_flight_reads, in_tx, sgl_mode, lp_state, 
           tx_count, abort_count, conflict_aborts, pc, conflicting_writers, 
           conflicting_readers >>

ProcSet == (LP)

Init == (* Global variables *)
        /\ in_flight_writes = {}
        /\ in_flight_reads = {}
        /\ in_tx = [lp \in LP |-> FALSE]
        /\ sgl_mode = [lp \in LP |-> FALSE]
        /\ lp_state = [lp \in LP |-> "idle"]
        /\ tx_count = [lp \in LP |-> 0]
        /\ abort_count = [lp \in LP |-> 0]
        /\ conflict_aborts = 0
        (* Process Proc *)
        /\ conflicting_writers = [self \in LP |-> {}]
        /\ conflicting_readers = [self \in LP |-> {}]
        /\ pc = [self \in ProcSet |-> "L_entry"]

L_entry(self) == /\ pc[self] = "L_entry"
                 /\ \/ /\ lp_state[self] = "idle" /\ ~in_tx[self]
                          /\ \A other \in LP : sgl_mode[other] = FALSE
                       /\ in_tx' = [in_tx EXCEPT ![self] = TRUE]
                       /\ lp_state' = [lp_state EXCEPT ![self] = "active"]
                       /\ UNCHANGED <<in_flight_writes, in_flight_reads, sgl_mode, tx_count, abort_count, conflict_aborts, conflicting_writers, conflicting_readers>>
                    \/ /\ lp_state[self] = "active" /\ in_tx[self] = TRUE
                       /\ \E a \in Addr:
                            /\ conflicting_writers' = [conflicting_writers EXCEPT ![self] = {w \in LP : <<w, a>> \in in_flight_writes /\ w # self}]
                            /\ IF (conflicting_writers'[self] /= {})
                                  THEN /\ in_tx' = [w \in LP |-> IF w \in conflicting_writers'[self] THEN FALSE ELSE in_tx[w]]
                                       /\ lp_state' = [w \in LP |-> IF w \in conflicting_writers'[self] THEN "idle" ELSE lp_state[w]]
                                       /\ abort_count' = [w \in LP |-> IF w \in conflicting_writers'[self] THEN abort_count[w] + 1 ELSE abort_count[w]]
                                       /\ in_flight_writes' = in_flight_writes \ {<<w, a2>> : w \in conflicting_writers'[self], a2 \in Addr}
                                       /\ in_flight_reads' = in_flight_reads \ {<<w, a2>> : w \in conflicting_writers'[self], a2 \in Addr}
                                       /\ conflict_aborts' = conflict_aborts + Cardinality(conflicting_writers'[self])
                                  ELSE /\ in_flight_reads' = (in_flight_reads \cup {<<self, a>>})
                                       /\ UNCHANGED << in_flight_writes, in_tx, 
                                                       lp_state, abort_count, 
                                                       conflict_aborts >>
                       /\ UNCHANGED <<sgl_mode, tx_count, conflicting_readers>>
                    \/ /\ lp_state[self] = "active" /\ in_tx[self] = TRUE
                       /\ \E a \in Addr:
                            /\ conflicting_readers' = [conflicting_readers EXCEPT ![self] = {r \in LP : <<r, a>> \in in_flight_reads /\ r # self}]
                            /\ conflicting_writers' = [conflicting_writers EXCEPT ![self] = {w \in LP \ {self} : <<w, a>> \in in_flight_writes}]
                            /\ IF (conflicting_readers'[self] /= {} \/ conflicting_writers'[self] /= {})
                                  THEN /\ in_tx' = [w \in LP |-> IF w \in conflicting_readers'[self] \cup conflicting_writers'[self] THEN FALSE ELSE in_tx[w]]
                                       /\ lp_state' = [w \in LP |-> IF w \in conflicting_readers'[self] \cup conflicting_writers'[self] THEN "idle" ELSE lp_state[w]]
                                       /\ abort_count' = [w \in LP |-> IF w \in conflicting_readers'[self] THEN abort_count[w] + 1
                                                                       ELSE IF w \in conflicting_writers'[self] THEN abort_count[w] + 1
                                                                       ELSE abort_count[w]]
                                       /\ in_flight_reads' = in_flight_reads \ {<<rp, a2>> : rp \in conflicting_readers'[self] \union conflicting_writers'[self], a2 \in Addr}
                                       /\ in_flight_writes' = in_flight_writes \ {<<wp, a2>> : wp \in conflicting_readers'[self] \union conflicting_writers'[self], a2 \in Addr}
                                       /\ conflict_aborts' = conflict_aborts + Cardinality(conflicting_readers'[self]) + Cardinality(conflicting_writers'[self])
                                  ELSE /\ in_flight_writes' = (in_flight_writes \cup {<<self, a>>})
                                       /\ UNCHANGED << in_flight_reads, in_tx, 
                                                       lp_state, abort_count, 
                                                       conflict_aborts >>
                       /\ UNCHANGED <<sgl_mode, tx_count>>
                    \/ /\ lp_state[self] = "active" /\ in_tx[self] = TRUE
                       /\ in_tx' = [in_tx EXCEPT ![self] = FALSE]
                       /\ in_flight_writes' = in_flight_writes \ {<<self, a>> : a \in Addr}
                       /\ in_flight_reads' = in_flight_reads \ {<<self, a>> : a \in Addr}
                       /\ tx_count' = [tx_count EXCEPT ![self] = tx_count[self] + 1]
                       /\ lp_state' = [lp_state EXCEPT ![self] = "idle"]
                       /\ UNCHANGED <<sgl_mode, abort_count, conflict_aborts, conflicting_writers, conflicting_readers>>
                    \/ /\ lp_state[self] = "active" /\ in_tx[self] = TRUE
                       /\ in_tx' = [in_tx EXCEPT ![self] = FALSE]
                       /\ in_flight_writes' = in_flight_writes \ {<<self, a>> : a \in Addr}
                       /\ in_flight_reads' = in_flight_reads \ {<<self, a>> : a \in Addr}
                       /\ abort_count' = [abort_count EXCEPT ![self] = abort_count[self] + 1]
                       /\ lp_state' = [lp_state EXCEPT ![self] = "idle"]
                       /\ UNCHANGED <<sgl_mode, tx_count, conflict_aborts, conflicting_writers, conflicting_readers>>
                    \/ /\  \A other \in LP :
                          other = self \/
                          (sgl_mode[other] = FALSE /\ in_tx[other] = FALSE
                           /\ \A a \in Addr : ~(<<other, a>> \in in_flight_writes) /\ ~(<<other, a>> \in in_flight_reads))
                       /\ sgl_mode' = [sgl_mode EXCEPT ![self] = TRUE]
                       /\ lp_state' = [lp_state EXCEPT ![self] = "sgl"]
                       /\ UNCHANGED <<in_flight_writes, in_flight_reads, in_tx, tx_count, abort_count, conflict_aborts, conflicting_writers, conflicting_readers>>
                    \/ /\ lp_state[self] = "sgl" /\ sgl_mode[self] = TRUE
                       /\ sgl_mode' = [sgl_mode EXCEPT ![self] = FALSE]
                       /\ in_flight_writes' = in_flight_writes \ {<<self, a>> : a \in Addr}
                       /\ in_flight_reads' = in_flight_reads \ {<<self, a>> : a \in Addr}
                       /\ in_tx' = [in_tx EXCEPT ![self] = FALSE]
                       /\ lp_state' = [lp_state EXCEPT ![self] = "idle"]
                       /\ UNCHANGED <<tx_count, abort_count, conflict_aborts, conflicting_writers, conflicting_readers>>
                    \/ /\ lp_state[self] = "active" /\ in_tx[self] = TRUE
                       /\ in_tx' = [in_tx EXCEPT ![self] = FALSE]
                       /\ in_flight_writes' = in_flight_writes \ {<<self, a>> : a \in Addr}
                       /\ in_flight_reads' = in_flight_reads \ {<<self, a>> : a \in Addr}
                       /\ abort_count' = [abort_count EXCEPT ![self] = abort_count[self] + 1]
                       /\ conflict_aborts' = conflict_aborts + 1
                       /\ lp_state' = [lp_state EXCEPT ![self] = "idle"]
                       /\ UNCHANGED <<sgl_mode, tx_count, conflicting_writers, conflicting_readers>>
                 /\ pc' = [pc EXCEPT ![self] = "L_entry"]

Proc(self) == L_entry(self)

(* Allow infinite stuttering to prevent deadlock on termination. *)
Terminating == /\ \A self \in ProcSet: pc[self] = "Done"
               /\ UNCHANGED vars

Next == (\E self \in LP: Proc(self))
           \/ Terminating

Spec == /\ Init /\ [][Next]_vars
        /\ \A self \in LP : WF_vars(Proc(self))

Termination == <>(\A self \in ProcSet: pc[self] = "Done")

\* END TRANSLATION

(*--------------------------------------------------------------------*)
(* Helpers (used by invariants and temporal properties below)         *)
(*--------------------------------------------------------------------*)

(* An LP is writing address a *)
IsWriting(lp, a) == <<lp, a>> \in in_flight_writes

(* An LP is reading address a *)
IsReading(lp, a) == <<lp, a>> \in in_flight_reads

(* Find another LP reading the same address (RAW conflict check) *)
FindReader(lp, a) ==
    {r \in LP : IsReading(r, a) /\ r # lp}

(* Find another LP writing the same address (WAR conflict check) *)
FindWriter(lp, a) ==
    {w \in LP : IsWriting(w, a) /\ w # lp}

(*====================================================================*)
(* Invariants                                                         *)
(*====================================================================*)

(*── I1: An address can be written by at most one LP at a time ──────*)
NoConcurrentWrites ==
    \A a \in Addr :
        Cardinality({lp \in LP : IsWriting(lp, a)}) <= 1

(*── I2: At most one LP in SGL mode at any time ─────────────────────*)
SGLMutex ==
    \A lp1, lp2 \in LP :
        (sgl_mode[lp1] = TRUE /\ sgl_mode[lp2] = TRUE) => lp1 = lp2

(*── I3: If an LP is in SGL mode, no other LP has in-flight ops ────*)
SGLIsolation ==
    \A lp \in LP :
        sgl_mode[lp] = TRUE =>
            \A other \in LP \ {lp} :
                /\ \A a \in Addr : ~IsWriting(other, a)
                /\ \A a \in Addr : ~IsReading(other, a)

(*── I4: If an LP is not in a transaction, it has no in-flight ops ──*)
NoOrphanedOps ==
    \A lp \in LP :
        ~in_tx[lp] =>
            (\A a \in Addr : ~IsWriting(lp, a) /\ ~IsReading(lp, a))

(*── I5: SGL mode implies lp_state is "sgl" ─────────────────────────*)
SGLStateConsistent ==
    \A lp \in LP :
        sgl_mode[lp] = TRUE => lp_state[lp] = "sgl"

(*── I6: An LP with in-flight writes is active and in a TX ──────────*)
WriteTrackingConsistent ==
    \A lp \in LP :
        (\E a \in Addr : IsWriting(lp, a)) =>
            (in_tx[lp] = TRUE /\ lp_state[lp] \in {"active", "sgl"})

(*── I7: An LP with in-flight reads is active and in a TX ───────────*)
ReadTrackingConsistent ==
    \A lp \in LP :
        (\E a \in Addr : IsReading(lp, a)) =>
            (in_tx[lp] = TRUE /\ lp_state[lp] \in {"active", "sgl"})

(*── I8: No LP can conflict with itself ─────────────────────────────*)
NoSelfConflict ==
    \A lp \in LP, a \in Addr :
        ~ (IsWriting(lp, a) /\ IsReading(lp, a))

(*====================================================================*)
(* Fence (memory ordering) annotations                                *)
(*                                                                    *)
(* DESEngine models the cross-LP conflict resolution protocol in      *)
(* the discrete-event simulator (engine.rs).  There are no shared     *)
(* memory fences in the model because the DES engine does not         *)
(* execute on real hardware — it is a software simulation with        *)
(* sequential event processing.  Memory ordering is irrelevant.       *)
(*                                                                    *)
(* Score: 2/5 — simulation model; fences not applicable.              *)
(*====================================================================*)

(*====================================================================*)
(* Temporal properties                                                *)
(*====================================================================*)

(* Every started transaction eventually completes *)
Progress ==
    \A lp \in LP :
        (lp_state[lp] = "active") ~> (lp_state[lp] = "idle")

(* No livelock: abort count can increase but total transactions
   (committed + aborted) is bounded per step — we just require
   that the set of aborted LPs is not all LPs forever. *)
NotAllAborted ==
    ~ [](\A lp \in LP : abort_count[lp] > tx_count[lp])

(*====================================================================*)
(* Model parameters                                                   *)
(*====================================================================*)

(* Default: LP = {0, 1}; Addr = {0, 1} *)

====
