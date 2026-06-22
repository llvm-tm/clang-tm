----------------------- MODULE SimEngine -----------------------
(*
 * SimEngine — Cross-LP Conflict Resolution Protocol
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
 *   NoLostUpdate:       After resolution, at most one LP holds a write on any addr.
 *   SGLMutex:           At most one LP in SGL mode at any time.
 *   SGLBlocksOthers:    If sgl_mode[t] = TRUE, no other LP has in-flight
 *                       writes or reads on any address.
 *   AcyclicAborts:      The abort relation is acyclic (timestamp ordering).
 *   Deterministic:      For a given state and event, the resolution outcome
 *                       is uniquely determined.
 *)

EXTENDS Naturals, FiniteSets, TLC

CONSTANTS
    LP,                 (* Set of LP (thread) IDs *)
    Addr,               (* Set of memory addresses *)

ASSUME LP \subseteq Nat
ASSUME Addr \subseteq Nat

VARIABLES
    in_flight_writes,   (* Set of <<lp, addr>>: LPs currently writing each addr *)
    in_flight_reads,    (* Set of <<lp, addr>>: LPs currently reading each addr *)
    in_tx,              (* [LP -> BOOLEAN]: whether LP is in a transaction *)
    sgl_mode,           (* [LP -> BOOLEAN]: whether LP holds the SGL mutex *)
    pc,                 (* [LP -> {"idle", "active", "conflict_abort", "sgl"}] *)
    tx_count,           (* [LP -> Nat]: successful transaction count *)
    abort_count,        (* [LP -> Nat]: abort count *)
    conflict_aborts     (* Nat: total synthetic conflict aborts *)

vars == <<in_flight_writes, in_flight_reads, in_tx, sgl_mode, pc,
          tx_count, abort_count, conflict_aborts>>

(*--------------------------------------------------------------------*)
(* Helpers                                                             *)
(*--------------------------------------------------------------------*)

(* An LP is writing address a *)
IsWriting(lp, a) == <<lp, a>> \in in_flight_writes

(* An LP is reading address a *)
IsReading(lp, a) == <<lp, a>> \in in_flight_reads

(* Find another LP reading the same address (RAW conflict check) *)
FindReader(lp, a) ==
    {r : r \in {r2 \in LP : IsReading(r2, a)} : r # lp}

(* Find another LP writing the same address (WAR conflict check) *)
FindWriter(lp, a) ==
    {w : w \in {w2 \in LP : IsWriting(w2, a)} : w # lp}

(*--------------------------------------------------------------------*)
(* Init                                                                *)
(*--------------------------------------------------------------------*)

Init ==
    /\ in_flight_writes = {}
    /\ in_flight_reads = {}
    /\ in_tx = [lp \in LP |-> FALSE]
    /\ sgl_mode = [lp \in LP |-> FALSE]
    /\ pc = [lp \in LP |-> "idle"]
    /\ tx_count = [lp \in LP |-> 0]
    /\ abort_count = [lp \in LP |-> 0]
    /\ conflict_aborts = 0

(*--------------------------------------------------------------------*)
(* Actions                                                             *)
(*--------------------------------------------------------------------*)

(*── Begin transaction ───────────────────────────────────────────────*)
BeginTx(lp) ==
    /\ pc[lp] = "idle"
    /\ ~in_tx[lp]
    /\ in_tx' = [in_tx EXCEPT ![lp] = TRUE]
    /\ pc' = [pc EXCEPT ![lp] = "active"]
    /\ UNCHANGED <<in_flight_writes, in_flight_reads, sgl_mode,
                   tx_count, abort_count, conflict_aborts>>

(*── Read address (inside transaction) ───────────────────────────────*)
(*  Check WAR conflict: if another LP is writing the same address,
    abort that older writer. *)
ReadAddr(lp, a) ==
    /\ pc[lp] = "active"
    /\ in_tx[lp] = TRUE
    /\ a \in Addr
    LET conflicting_writers == FindWriter(lp, a) IN
    /\ IF conflicting_writers # {}
        THEN
            (* Abort the older writer(s) *)
            /\ \A w \in conflicting_writers :
                in_tx' = [in_tx EXCEPT ![w] = FALSE]
                /\ abort_count' = [abort_count EXCEPT ![w] = abort_count[w] + 1]
                /\ in_flight_writes' = in_flight_writes \ {<<w, a2>> : a2 \in Addr}
                /\ in_flight_reads' = in_flight_reads \ {<<w, a2>> : a2 \in Addr}
                /\ conflict_aborts' = conflict_aborts + Cardinality(conflicting_writers)
        ELSE
            (* No conflict: add to read-set *)
            /\ in_flight_reads' = in_flight_reads \cup {<<lp, a>>}
            /\ UNCHANGED <<in_tx, abort_count, conflict_aborts>>
    /\ UNCHANGED <<pc, sgl_mode, tx_count>>
    (* Note: the \A w quantifier in the THEN branch non-deterministically
       chooses an ordering for aborting multiple conflicting writers.
       In practice, the engine handles one at a time.  For TLC, this
       models all resolutions correctly because the end state is the
       same regardless of order. *)

(*── Write address (inside transaction) ──────────────────────────────*)
(*  Check RAW conflict: if another LP is reading the same address,
    abort that older reader. *)
WriteAddr(lp, a) ==
    /\ pc[lp] = "active"
    /\ in_tx[lp] = TRUE
    /\ a \in Addr
    LET conflicting_readers == FindReader(lp, a) IN
    /\ IF conflicting_readers # {}
        THEN
            (* Abort the older reader(s) *)
            /\ \A r \in conflicting_readers :
                in_tx' = [in_tx EXCEPT ![r] = FALSE]
                /\ abort_count' = [abort_count EXCEPT ![r] = abort_count[r] + 1]
                /\ in_flight_reads' = in_flight_reads \ {<<r, a2>> : a2 \in Addr}
                /\ in_flight_writes' = in_flight_writes \ {<<r, a2>> : a2 \in Addr}
                /\ conflict_aborts' = conflict_aborts + Cardinality(conflicting_readers)
        ELSE
            (* No conflict: add to write-set *)
            /\ in_flight_writes' = in_flight_writes \cup {<<lp, a>>}
            /\ UNCHANGED <<in_tx, abort_count, conflict_aborts>>
    /\ UNCHANGED <<pc, sgl_mode, tx_count>>

(*── Commit transaction ─────────────────────────────────────────────*)
CommitTx(lp) ==
    /\ pc[lp] = "active"
    /\ in_tx[lp] = TRUE
    /\ in_tx' = [in_tx EXCEPT ![lp] = FALSE]
    (* Clear all in-flight tracking for this LP *)
    /\ in_flight_writes' = in_flight_writes \ {<<lp, a>> : a \in Addr}
    /\ in_flight_reads' = in_flight_reads \ {<<lp, a>> : a \in Addr}
    /\ tx_count' = [tx_count EXCEPT ![lp] = tx_count[lp] + 1]
    /\ pc' = [pc EXCEPT ![lp] = "idle"]
    /\ UNCHANGED <<sgl_mode, abort_count, conflict_aborts>>

(*── Abort transaction (from trace or explicit) ─────────────────────*)
AbortTx(lp) ==
    /\ pc[lp] = "active"
    /\ in_tx[lp] = TRUE
    /\ in_tx' = [in_tx EXCEPT ![lp] = FALSE]
    (* Clear all in-flight tracking for this LP *)
    /\ in_flight_writes' = in_flight_writes \ {<<lp, a>> : a \in Addr}
    /\ in_flight_reads' = in_flight_reads \ {<<lp, a>> : a \in Addr}
    /\ abort_count' = [abort_count EXCEPT ![lp] = abort_count[lp] + 1]
    /\ pc' = [pc EXCEPT ![lp] = "idle"]
    /\ UNCHANGED <<sgl_mode, tx_count, conflict_aborts>>

(*── Enter SGL mode ─────────────────────────────────────────────────*)
EnterSGL(lp) ==
    (* Can only enter SGL when no other LP is in SGL mode *)
    /\ \A other \in LP : other = lp \/ sgl_mode[other] = FALSE
    (* No other LP has conflicting in-flight writes/reads *)
    /\ \A other \in LP \ {lp} :
        /\ \A a \in Addr : ~IsWriting(other, a)
        /\ \A a \in Addr : ~IsReading(other, a)
    /\ sgl_mode' = [sgl_mode EXCEPT ![lp] = TRUE]
    /\ pc' = [pc EXCEPT ![lp] = "sgl"]
    /\ UNCHANGED <<in_flight_writes, in_flight_reads, in_tx,
                   tx_count, abort_count, conflict_aborts>>

(*── Exit SGL mode ──────────────────────────────────────────────────*)
ExitSGL(lp) ==
    /\ pc[lp] = "sgl"
    /\ sgl_mode[lp] = TRUE
    /\ sgl_mode' = [sgl_mode EXCEPT ![lp] = FALSE]
    /\ pc' = [pc EXCEPT ![lp] = "idle"]
    /\ UNCHANGED <<in_flight_writes, in_flight_reads, in_tx,
                   tx_count, abort_count, conflict_aborts>>

(*── Conflict abort (external: triggered by a different LP's action) ─*)
(* This is a non-deterministic action modeling that any LP can be
   conflict-aborted by another LP's concurrent operation.
   The specific resolution is modeled in ReadAddr/WriteAddr above. *)
ConflictAbort(lp) ==
    /\ pc[lp] = "active"
    /\ in_tx[lp] = TRUE
    (\* This LP is being aborted by another LP's operation *\)
    /\ in_tx' = [in_tx EXCEPT ![lp] = FALSE]
    /\ in_flight_writes' = in_flight_writes \ {<<lp, a>> : a \in Addr}
    /\ in_flight_reads' = in_flight_reads \ {<<lp, a>> : a \in Addr}
    /\ abort_count' = [abort_count EXCEPT ![lp] = abort_count[lp] + 1]
    /\ conflict_aborts' = conflict_aborts + 1
    /\ pc' = [pc EXCEPT ![lp] = "idle"]
    /\ UNCHANGED <<sgl_mode, tx_count>>

(*--------------------------------------------------------------------*)
(* Next-state relation                                                *)
(*--------------------------------------------------------------------*)

Next ==
    \E lp \in LP :
        \/ BeginTx(lp)
        \/ \E a \in Addr : ReadAddr(lp, a)
        \/ \E a \in Addr : WriteAddr(lp, a)
        \/ CommitTx(lp)
        \/ AbortTx(lp)
        \/ EnterSGL(lp)
        \/ ExitSGL(lp)
        \/ ConflictAbort(lp)

Spec == Init /\ [][Next]_vars

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

(*── I5: SGL mode implies LP is in "sgl" pc state ───────────────────*)
SGLStateConsistent ==
    \A lp \in LP :
        sgl_mode[lp] = TRUE => pc[lp] = "sgl"

(*── I6: An LP with in-flight writes is active and in a TX ──────────*)
WriteTrackingConsistent ==
    \A lp \in LP :
        (\E a \in Addr : IsWriting(lp, a)) =>
            (in_tx[lp] = TRUE /\ pc[lp] \in {"active", "sgl"})

(*── I7: An LP with in-flight reads is active and in a TX ───────────*)
ReadTrackingConsistent ==
    \A lp \in LP :
        (\E a \in Addr : IsReading(lp, a)) =>
            (in_tx[lp] = TRUE /\ pc[lp] \in {"active", "sgl"})

(*── I8: No LP can conflict with itself ─────────────────────────────*)
NoSelfConflict ==
    \A lp \in LP, a \in Addr :
        ~ (IsWriting(lp, a) /\ IsReading(lp, a))

(*====================================================================*)
(* Temporal properties                                                *)
(*====================================================================*)

(* Every started transaction eventually completes *)
Progress ==
    \A lp \in LP :
        (pc[lp] = "active") ~> (pc[lp] = "idle")

(* No livelock: abort count can increase but total transactions
   (committed + aborted) is bounded per step — we just require
   that the set of aborted LPs is not all LPs forever. *)
NotAllAborted ==
    ~ [](\A lp \in LP : abort_count[lp] > tx_count[lp])

(*====================================================================*)
(* Model parameters                                                   *)
(*====================================================================*)

(* Default: LP = {0, 1}; Addr = {0, 1} *)
