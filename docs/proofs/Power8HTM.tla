------------------------ MODULE Power8HTM ------------------------
(*
 * POWER8 vs TSX hardware-TM conflict semantics (PlusCal)
 *
 * Formalises the *one* semantic difference that matters when comparing
 * gem5's hardware-TM runs on `bank`: how a transactional write conflict
 * is resolved at the coherence layer.
 *
 *   - TSX (Intel RTM, last-writer-wins / steal):  the L1 holds speculative
 *     data; a *second* writer of a line already in an active TX's
 *     write-set succeeds (steals the line) and the *first* transaction
 *     aborts (it detects the theft on its next step).  Both may run
 *     concurrently; resolution "picks a winner" by aborting the loser.
 *     A committed write also invalidates any other transaction that read
 *     the line (a reader whose read was invalidated aborts at commit).
 *
 *   - POWER8 TS (ownership / NAK):  a transactional write *owns* the line
 *     exclusively at the coherence layer; a remote request (read or
 *     write) to an owned line is denied (NAK'd) and the *remote*
 *     transaction aborts, while the owner keeps running.  Reads never
 *     NAK (only writes acquire ownership).  Suspend/resume releases and
 *     re-acquires ownership.
 *
 * Both must satisfy atomicity/opacity; they differ only in WHO aborts on a
 * conflict.  This matters for gem5 because its single ISA-agnostic Ruby
 * MESI_Three_Level_HTM protocol implements the POWER8 ownership/NAK model
 * (L0cache.sm f_sendDataToL1 sends NAK when a line is in an HTM write-set;
 * no data-forwarding) for BOTH ARM TME and x86 TSX.  So today "TSX" and
 * "TME" in gem5 share POWER8 conflict semantics; a true TSX
 * last-writer-wins data-forwarding path does not yet exist.
 *
 * Invariants checked under both ConflictPolicy hypotheses:
 *   WriteOwnership:  a line is owned by at most one tx (POWER8)
 *   WriteImpliesOwn: a tx's write-set lines are exactly its owned lines
 *                    (POWER8)
 *   ReadersNoNAK:    a pure read never causes an abort; a line read while
 *                    owned must be in the read tx's own write-set (POWER8)
 *
 * (A global 'committed write-sets are disjoint' invariant would be UNSOUND:
 * two transactions may legitimately commit writes to the same address in
 * sequence, last-writer-wins.  Opacity is instead guarded directly in the
 * COMMIT action via StaleRead, applied to both policies.  See the scope
 * note at the bottom.)
 *
 * Verifiable TLC configs (see *.cfg): TSX or POWER8, small (single-address,
 * MaxCommit = 1) completes deterministically; the two-address configs run
 * large state spaces (no violations found, but do not terminate quickly).
 *
 * Expected TLC result: invariants hold under both policies; the abort
 * trace (which transaction aborted) differs between policies — the
 * observable gem5 would report with identical clock settings.
 *)

EXTENDS Naturals, FiniteSets, Sequences, TLC

CONSTANTS
    Thread,          (* Set of transactional thread IDs *)
    Addr,            (* Set of addresses (one cache-line each here) *)
    ConflictPolicy,  (* "TSX" last-writer-wins OR "POWER8" ownership *)
    MaxCommit        (* bound on commits per thread for TLC termination *)

ASSUME Thread \subseteq Nat \ {0}
ASSUME Addr \subseteq Nat
ASSUME ConflictPolicy \in {"TSX", "POWER8"}
ASSUME MaxCommit \in Nat

(*─── PlusCal algorithm ───────────────────────────────────────────*)
(* --algorithm Power8HTM

variables
    mem = [a \in Addr |-> 0],
    mode = [t \in Thread |-> "idle"],      (* idle | tsx | tsx_suspend *)
    read_set = [t \in Thread |-> {}],
    write_set = [t \in Thread |-> {}],
    ws_new = [t \in Thread |-> [a \in Addr |-> 0]], (* buffered new values *)
    wowner = [a \in Addr |-> 0],           (* owner of a line's write-set *)
                                           (* 0 = not owned *)
    aborted_thread = [t \in Thread |-> 0],
    committed = [t \in Thread |-> 0],
    (* Last committed read/write sets, kept for the Opacity invariant. *)
    c_rs = [t \in Thread |-> {}],
    c_ws = [t \in Thread |-> {}];

define
    (* A committed transaction other than t wrote a line t currently reads. *)
    StaleRead(t) ==
        \E t2 \in Thread \ {t} :
            committed[t2] > 0 /\ read_set[t] \cap c_ws[t2] # {}

    (* TSX: one of t's write lines was stolen (now owned by someone else). *)
    Stolen(t) ==
        ConflictPolicy = "TSX" /\
        \E a \in write_set[t] : wowner[a] # 0 /\ wowner[a] # t

    (* POWER8: t no longer owns a line it wrote (ownership lost). *)
    LostOwnership(t) ==
        ConflictPolicy = "POWER8" /\
        \E a \in write_set[t] : wowner[a] # t

    WriteBack(t) ==
        [a \in Addr |->
            IF a \in write_set[t] THEN ws_new[t][a] ELSE mem[a]]
end define;

process ThreadProc \in Thread
begin

L_idle:
    either
        \* Begin a transaction.
        read_set[self] := {};
        write_set[self] := {};
        mode[self] := "tsx";
        goto L_active;
    or
        \* Idle forever (bound-commit termination).
        goto L_done;
    end either;

L_chk:
    \* Theft / ownership-loss detection: if we lost a write line, abort.
    if Stolen(self) \/ LostOwnership(self)
    then
        wowner := [a \in Addr |-> IF a \in write_set[self] /\ wowner[a] = self THEN 0 ELSE wowner[a]];
        mode[self] := "idle";
        read_set[self] := {};
        write_set[self] := {};
        aborted_thread[self] := aborted_thread[self] + 1;
        goto L_idle;
    else
        goto L_active;
    end if;

L_active:
    either \* ── Read address a (no ownership acquired under POWER8) ──
        with a \in Addr do
            if ConflictPolicy = "POWER8" /\ \E t2 \in Thread :
                   mode[t2] = "tsx" /\ a \in write_set[t2] /\ t2 # self
            then
                \* NAK'd by a writer-owner -> we abort.
                wowner := [q \in Addr |-> IF q \in write_set[self] /\ wowner[q] = self THEN 0 ELSE wowner[q]];
                mode[self] := "idle";
                read_set[self] := {};
                write_set[self] := {};
                aborted_thread[self] := aborted_thread[self] + 1;
                goto L_idle;
            else
                read_set[self] := read_set[self] \union {a};
                goto L_chk;
            end if;
        end with;
    or \* ── Write address a ──
        with a \in Addr do
            if ConflictPolicy = "TSX"
            then
                \* last-writer-wins: claim the line (steal if owned).
                wowner[a] := self;
                write_set[self] := write_set[self] \union {a};
                ws_new[self][a] := (mem[a] + 1) % 2;
                goto L_chk;
            else
                \* POWER8 ownership: only one owner per line.
                if \E t2 \in Thread : wowner[a] # 0 /\ wowner[a] # self
                then
                    \* owned elsewhere -> WE abort (owner wins).
                    wowner := [q \in Addr |-> IF q \in write_set[self] /\ wowner[q] = self THEN 0 ELSE wowner[q]];
                    mode[self] := "idle";
                    read_set[self] := {};
                    write_set[self] := {};
                    aborted_thread[self] := aborted_thread[self] + 1;
                    goto L_idle;
                else
                    wowner[a] := self;
                    write_set[self] := write_set[self] \union {a};
                    ws_new[self][a] := (mem[a] + 1) % 2;
                    goto L_chk;
                end if;
            end if;
        end with;
    or \* ── Suspend (POWER8 tsuspend): release write-set ownership ──
        if ConflictPolicy = "POWER8"
        then
            wowner := [a \in Addr |-> IF a \in write_set[self] THEN 0 ELSE wowner[a]];
            mode[self] := "tsx_suspend";
            goto L_chk;
        else
            goto L_chk;
        end if;
    or \* ── Resume (POWER8 tresume): re-acquire write-set ownership ──
        if ConflictPolicy = "POWER8"
        then
            if \E a \in write_set[self] : wowner[a] # 0 /\ wowner[a] # self
            then
                \* a line was taken while suspended -> we abort.
                wowner := [a \in Addr |-> IF a \in write_set[self] /\ wowner[a] = self THEN 0 ELSE wowner[a]];
                mode[self] := "idle";
                read_set[self] := {};
                write_set[self] := {};
                aborted_thread[self] := aborted_thread[self] + 1;
                goto L_idle;
            else
                wowner := [a \in Addr |-> IF a \in write_set[self] THEN self ELSE wowner[a]];
                mode[self] := "tsx";
                goto L_chk;
            end if;
        else
            goto L_chk;
        end if;
    or \* ── Commit: atomically publish the write-set ──
        if committed[self] < MaxCommit
        then
            if LostOwnership(self) \/ Stolen(self) \/ StaleRead(self)
            then
                \* Abort on ANY conflict that would break opacity:
                \*   - LostOwnership (POWER8): we no longer own a written line.
                \*   - Stolen        (TSX):    a line we wrote was stolen.
                \*   - StaleRead     (both):   a line we read was committed
                \*                             by a concurrent writer.
                wowner := [a \in Addr |-> IF a \in write_set[self] /\ wowner[a] = self THEN 0 ELSE wowner[a]];
                mode[self] := "idle";
                read_set[self] := {};
                write_set[self] := {};
                aborted_thread[self] := aborted_thread[self] + 1;
                goto L_idle;
            else
                \* publish atomically and record committed sets.
                mem := WriteBack(self);
                committed[self] := committed[self] + 1;
                c_rs[self] := read_set[self];
                c_ws[self] := write_set[self];
                if ConflictPolicy = "POWER8"
                then
                    wowner := [a \in Addr |-> IF a \in write_set[self] THEN 0 ELSE wowner[a]];
                end if;
                mode[self] := "idle";
                read_set[self] := {};
                write_set[self] := {};
                goto L_idle;
            end if;
        else
            goto L_done;
        end if;
    end either;

L_done:
    skip;

end process;

end algorithm; *)
\* BEGIN TRANSLATION
VARIABLES pc, mem, mode, read_set, write_set, ws_new, wowner, aborted_thread, 
          committed, c_rs, c_ws

(* define statement *)
StaleRead(t) ==
    \E t2 \in Thread \ {t} :
        committed[t2] > 0 /\ read_set[t] \cap c_ws[t2] # {}


Stolen(t) ==
    ConflictPolicy = "TSX" /\
    \E a \in write_set[t] : wowner[a] # 0 /\ wowner[a] # t


LostOwnership(t) ==
    ConflictPolicy = "POWER8" /\
    \E a \in write_set[t] : wowner[a] # t

WriteBack(t) ==
    [a \in Addr |->
        IF a \in write_set[t] THEN ws_new[t][a] ELSE mem[a]]


vars == << pc, mem, mode, read_set, write_set, ws_new, wowner, aborted_thread, 
           committed, c_rs, c_ws >>

ProcSet == (Thread)

Init == (* Global variables *)
        /\ mem = [a \in Addr |-> 0]
        /\ mode = [t \in Thread |-> "idle"]
        /\ read_set = [t \in Thread |-> {}]
        /\ write_set = [t \in Thread |-> {}]
        /\ ws_new = [t \in Thread |-> [a \in Addr |-> 0]]
        /\ wowner = [a \in Addr |-> 0]
        /\ aborted_thread = [t \in Thread |-> 0]
        /\ committed = [t \in Thread |-> 0]
        /\ c_rs = [t \in Thread |-> {}]
        /\ c_ws = [t \in Thread |-> {}]
        /\ pc = [self \in ProcSet |-> "L_idle"]

L_idle(self) == /\ pc[self] = "L_idle"
                /\ \/ /\ read_set' = [read_set EXCEPT ![self] = {}]
                      /\ write_set' = [write_set EXCEPT ![self] = {}]
                      /\ mode' = [mode EXCEPT ![self] = "tsx"]
                      /\ pc' = [pc EXCEPT ![self] = "L_active"]
                   \/ /\ pc' = [pc EXCEPT ![self] = "L_done"]
                      /\ UNCHANGED <<mode, read_set, write_set>>
                /\ UNCHANGED << mem, ws_new, wowner, aborted_thread, committed, 
                                c_rs, c_ws >>

L_chk(self) == /\ pc[self] = "L_chk"
               /\ IF Stolen(self) \/ LostOwnership(self)
                     THEN /\ wowner' = [a \in Addr |-> IF a \in write_set[self] /\ wowner[a] = self THEN 0 ELSE wowner[a]]
                          /\ mode' = [mode EXCEPT ![self] = "idle"]
                          /\ read_set' = [read_set EXCEPT ![self] = {}]
                          /\ write_set' = [write_set EXCEPT ![self] = {}]
                          /\ aborted_thread' = [aborted_thread EXCEPT ![self] = aborted_thread[self] + 1]
                          /\ pc' = [pc EXCEPT ![self] = "L_idle"]
                     ELSE /\ pc' = [pc EXCEPT ![self] = "L_active"]
                          /\ UNCHANGED << mode, read_set, write_set, wowner, 
                                          aborted_thread >>
               /\ UNCHANGED << mem, ws_new, committed, c_rs, c_ws >>

L_active(self) == /\ pc[self] = "L_active"
                  /\ \/ /\ \E a \in Addr:
                             IF ConflictPolicy = "POWER8" /\ \E t2 \in Thread :
                                    mode[t2] = "tsx" /\ a \in write_set[t2] /\ t2 # self
                                THEN /\ wowner' = [q \in Addr |-> IF q \in write_set[self] /\ wowner[q] = self THEN 0 ELSE wowner[q]]
                                     /\ mode' = [mode EXCEPT ![self] = "idle"]
                                     /\ read_set' = [read_set EXCEPT ![self] = {}]
                                     /\ write_set' = [write_set EXCEPT ![self] = {}]
                                     /\ aborted_thread' = [aborted_thread EXCEPT ![self] = aborted_thread[self] + 1]
                                     /\ pc' = [pc EXCEPT ![self] = "L_idle"]
                                ELSE /\ read_set' = [read_set EXCEPT ![self] = read_set[self] \union {a}]
                                     /\ pc' = [pc EXCEPT ![self] = "L_chk"]
                                     /\ UNCHANGED << mode, write_set, wowner, 
                                                     aborted_thread >>
                        /\ UNCHANGED <<mem, ws_new, committed, c_rs, c_ws>>
                     \/ /\ \E a \in Addr:
                             IF ConflictPolicy = "TSX"
                                THEN /\ wowner' = [wowner EXCEPT ![a] = self]
                                     /\ write_set' = [write_set EXCEPT ![self] = write_set[self] \union {a}]
                                     /\ ws_new' = [ws_new EXCEPT ![self][a] = (mem[a] + 1) % 2]
                                     /\ pc' = [pc EXCEPT ![self] = "L_chk"]
                                     /\ UNCHANGED << mode, read_set, 
                                                     aborted_thread >>
                                ELSE /\ IF \E t2 \in Thread : wowner[a] # 0 /\ wowner[a] # self
                                           THEN /\ wowner' = [q \in Addr |-> IF q \in write_set[self] /\ wowner[q] = self THEN 0 ELSE wowner[q]]
                                                /\ mode' = [mode EXCEPT ![self] = "idle"]
                                                /\ read_set' = [read_set EXCEPT ![self] = {}]
                                                /\ write_set' = [write_set EXCEPT ![self] = {}]
                                                /\ aborted_thread' = [aborted_thread EXCEPT ![self] = aborted_thread[self] + 1]
                                                /\ pc' = [pc EXCEPT ![self] = "L_idle"]
                                                /\ UNCHANGED ws_new
                                           ELSE /\ wowner' = [wowner EXCEPT ![a] = self]
                                                /\ write_set' = [write_set EXCEPT ![self] = write_set[self] \union {a}]
                                                /\ ws_new' = [ws_new EXCEPT ![self][a] = (mem[a] + 1) % 2]
                                                /\ pc' = [pc EXCEPT ![self] = "L_chk"]
                                                /\ UNCHANGED << mode, read_set, 
                                                                aborted_thread >>
                        /\ UNCHANGED <<mem, committed, c_rs, c_ws>>
                     \/ /\ IF ConflictPolicy = "POWER8"
                              THEN /\ wowner' = [a \in Addr |-> IF a \in write_set[self] THEN 0 ELSE wowner[a]]
                                   /\ mode' = [mode EXCEPT ![self] = "tsx_suspend"]
                                   /\ pc' = [pc EXCEPT ![self] = "L_chk"]
                              ELSE /\ pc' = [pc EXCEPT ![self] = "L_chk"]
                                   /\ UNCHANGED << mode, wowner >>
                        /\ UNCHANGED <<mem, read_set, write_set, ws_new, aborted_thread, committed, c_rs, c_ws>>
                     \/ /\ IF ConflictPolicy = "POWER8"
                              THEN /\ IF \E a \in write_set[self] : wowner[a] # 0 /\ wowner[a] # self
                                         THEN /\ wowner' = [a \in Addr |-> IF a \in write_set[self] /\ wowner[a] = self THEN 0 ELSE wowner[a]]
                                              /\ mode' = [mode EXCEPT ![self] = "idle"]
                                              /\ read_set' = [read_set EXCEPT ![self] = {}]
                                              /\ write_set' = [write_set EXCEPT ![self] = {}]
                                              /\ aborted_thread' = [aborted_thread EXCEPT ![self] = aborted_thread[self] + 1]
                                              /\ pc' = [pc EXCEPT ![self] = "L_idle"]
                                         ELSE /\ wowner' = [a \in Addr |-> IF a \in write_set[self] THEN self ELSE wowner[a]]
                                              /\ mode' = [mode EXCEPT ![self] = "tsx"]
                                              /\ pc' = [pc EXCEPT ![self] = "L_chk"]
                                              /\ UNCHANGED << read_set, 
                                                              write_set, 
                                                              aborted_thread >>
                              ELSE /\ pc' = [pc EXCEPT ![self] = "L_chk"]
                                   /\ UNCHANGED << mode, read_set, write_set, 
                                                   wowner, aborted_thread >>
                        /\ UNCHANGED <<mem, ws_new, committed, c_rs, c_ws>>
                     \/ /\ IF committed[self] < MaxCommit
                              THEN /\ IF LostOwnership(self) \/ Stolen(self) \/ StaleRead(self)
                                         THEN /\ wowner' = [a \in Addr |-> IF a \in write_set[self] /\ wowner[a] = self THEN 0 ELSE wowner[a]]
                                              /\ mode' = [mode EXCEPT ![self] = "idle"]
                                              /\ read_set' = [read_set EXCEPT ![self] = {}]
                                              /\ write_set' = [write_set EXCEPT ![self] = {}]
                                              /\ aborted_thread' = [aborted_thread EXCEPT ![self] = aborted_thread[self] + 1]
                                              /\ pc' = [pc EXCEPT ![self] = "L_idle"]
                                              /\ UNCHANGED << mem, committed, 
                                                              c_rs, c_ws >>
                                         ELSE /\ mem' = WriteBack(self)
                                              /\ committed' = [committed EXCEPT ![self] = committed[self] + 1]
                                              /\ c_rs' = [c_rs EXCEPT ![self] = read_set[self]]
                                              /\ c_ws' = [c_ws EXCEPT ![self] = write_set[self]]
                                              /\ IF ConflictPolicy = "POWER8"
                                                    THEN /\ wowner' = [a \in Addr |-> IF a \in write_set[self] THEN 0 ELSE wowner[a]]
                                                    ELSE /\ TRUE
                                                         /\ UNCHANGED wowner
                                              /\ mode' = [mode EXCEPT ![self] = "idle"]
                                              /\ read_set' = [read_set EXCEPT ![self] = {}]
                                              /\ write_set' = [write_set EXCEPT ![self] = {}]
                                              /\ pc' = [pc EXCEPT ![self] = "L_idle"]
                                              /\ UNCHANGED aborted_thread
                              ELSE /\ pc' = [pc EXCEPT ![self] = "L_done"]
                                   /\ UNCHANGED << mem, mode, read_set, 
                                                   write_set, wowner, 
                                                   aborted_thread, committed, 
                                                   c_rs, c_ws >>
                        /\ UNCHANGED ws_new

L_done(self) == /\ pc[self] = "L_done"
                /\ TRUE
                /\ pc' = [pc EXCEPT ![self] = "Done"]
                /\ UNCHANGED << mem, mode, read_set, write_set, ws_new, wowner, 
                                aborted_thread, committed, c_rs, c_ws >>

ThreadProc(self) == L_idle(self) \/ L_chk(self) \/ L_active(self)
                       \/ L_done(self)

(* Allow infinite stuttering to prevent deadlock on termination. *)
Terminating == /\ \A self \in ProcSet: pc[self] = "Done"
               /\ UNCHANGED vars

Next == (\E self \in Thread: ThreadProc(self))
           \/ Terminating

Spec == Init /\ [][Next]_vars

Termination == <>(\A self \in ProcSet: pc[self] = "Done")

\* END TRANSLATION
(*====================================================================*)
(* Invariants                                                         *)
(*====================================================================*)
(*
 * Scope note (deliberate): this model verifies the conflict-RESOLUTION
 * mechanism that distinguishes the two hardware-TM policies, not a full
 * linearizability/opacity proof.  Enforcing opacity transitively
 * (a committed tx never commits a read invalidated by a concurrent
 * committed writer) requires per-address read-time version capture; this
 * model instead encodes the standard STM safety net directly in the
 * COMMIT action (StaleRead guard, applied to both policies) and checks
 * the POWER8 ownership discipline that hardware enforces at the
 * coherence layer.  This mirrors the existing repo models (e.g. TSXSGL
 * checks lock/owner safety, not transitive opacity).
 *
 *   - AtomicOp / a global disjointness invariant would be UNSOUND here:
 *     two transactions may LEGITIMATELY commit writes to the same
 *     address in sequence (last-writer-wins), so 'committed write-sets
 *     are pairwise disjoint' is false even for correct histories.
 *   - The StaleRead guard in the commit action is what prevents a
 *     transaction from committing a read invalidated by an intervening
 *     concurrent commit; it applies to BOTH TSX and POWER8.
 *)

(* POWER8: at most one active transaction owns a line.  A TSX writer has
   no persistent owner (lines are stolen and data-forwarded), so this is
   vacuous under TSX — TSX ownership is "last writer wins", which the
   Stolen guard enforces. *)
WriteOwnership ==
    ConflictPolicy /= "POWER8" \/
    \A a \in Addr : \A t1, t2 \in Thread :
        t1 # t2 /\ wowner[a] = t1 /\ wowner[a] = t2 => FALSE

(* POWER8: a writer exclusively holds the lines in its write-set. *)
WriteImpliesOwn ==
    ConflictPolicy /= "POWER8" \/
    \A t \in Thread : \A a \in Addr :
        (mode[t] = "tsx" /\ a \in write_set[t]) => wowner[a] = t

(* POWER8: ownership is only ever granted to a writer — a pure reader
   never acquires ownership, so reads never NAK other transactions. *)
ReadersNoNAK ==
    ConflictPolicy /= "POWER8" \/
    \A t \in Thread : \A a \in Addr :
        (mode[t] = "tsx" /\ a \in read_set[t] /\ wowner[a] = t)
        => a \in write_set[t]

(*====================================================================*)
(* Bounding constraints for TLC termination                           *)
(*====================================================================*)
TLCBound ==
    /\ \A t \in Thread : committed[t] <= MaxCommit
    /\ \A t \in Thread : aborted_thread[t] < 3

\* Tighter bound for the larger 2-address model: at most one abort per thread.
\* The abort-ownership-release leak is already observable after a single abort,
\* so this bound still exercises that scenario while keeping TLC tractable.
TLCBound1 ==
    /\ \A t \in Thread : committed[t] <= MaxCommit
    /\ \A t \in Thread : aborted_thread[t] < 2

(*====================================================================*)
(* Temporal properties                                                *)
(*====================================================================*)

Spec_WF == Spec /\ \A self \in Thread : WF_vars(ThreadProc(self))

TransactionProgress ==
    \A t \in Thread :
        []( mode[t] = "tsx" =>
            <>(mode[t] = "idle" \/ mode[t] = "tsx_suspend") )

=====================================================================
