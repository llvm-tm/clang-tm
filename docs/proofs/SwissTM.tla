------------------------- MODULE SwissTM -------------------------
(*
 * SwissTM — ORec-based Eager Writes + Lazy Read Validation (PlusCal)
 *
 * ORec (Ownership Record) per address: <<r_lock, w_lock, r_ver, w_owner>>
 *   - r_lock: 0=free, 1=read-locked (during commit)
 *   - w_lock: 0=free, 1=write-locked (eager, at write time)
 *   - r_ver: version number for read-set validation
 *   - w_owner: thread holding the write lock
 *
 * Labels:
 *   L_idle          — begin or terminate
 *   L_begin         — clear state, start new transaction
 *   L_active        — non-deterministic: read, write (eager w_lock),
 *                     write conflict, or commit
 *   L_commit_rlock  — acquire read-locks on read-set
 *   L_commit        — increment ts + validate
 *   L_commit_wb     — write-back + release locks with new version
 *   L_abort         — clean up (write conflict, no locks held)
 *   L_done          — termination
 *)

EXTENDS Naturals, FiniteSets, TLC

CONSTANTS
    Thread,                (* Set of thread IDs *)
    Addr,                  (* Set of addresses *)
    Data,                  (* Set of possible data values *)
    MaxCommits             (* Max commits per thread *)

ASSUME Thread \subseteq Nat \ {0}
ASSUME Addr \subseteq Nat
ASSUME Data \subseteq Nat
ASSUME MaxCommits \in Nat \ {0}

(* ORec helpers *)
OREC_RLOCK(o) == o[1]
OREC_WLOCK(o) == o[2]
OREC_RVER(o) == o[3]
OREC_WOWNER(o) == o[4]
MAKE_OREC(rl, wl, rv, wo) == <<rl, wl, rv, wo>>

(* NOTE: lastFence[t] fence tracking exists only in the TLA+ translation below.
   Running pcal.trans will regenerate TLA+ and LOSE all fence tracking.
   Re-add lastFence variable + EXCEPT updates manually after translation. *)
(*--algorithm SwissTM

variables
    g_ts = 0,
    orec = [a \in Addr |-> MAKE_OREC(0, 0, 0, 0)],
    mem = [a \in Addr |-> 0],
    committed = [t \in Thread |-> 0],
    aborted = [t \in Thread |-> 0];

process ThreadProc \in Thread
variables
    readSet = {},
    writeLog = {},
    writeBuf = [a \in Addr |-> 0],
    oldVal = [a \in Addr |-> 0],
    readOnly = TRUE;
begin

L_idle:
    if committed[self] >= MaxCommits then
        goto L_done;
    else
        goto L_begin;
    end if;

L_begin:
    readSet := {};
    writeLog := {};
    readOnly := TRUE;
    goto L_active;

L_active:
    either \* Read own write (from writeBuf)
        with a \in Addr do
            if a \in writeLog then skip; end if;
        end with;
        goto L_active;
    or \* Read from memory (record r_ver)
        with a \in Addr do
            if a \notin writeLog /\ ~(OREC_WLOCK(orec[a]) = 1 /\ OREC_WOWNER(orec[a]) # self) then
                readSet := readSet \union {<<a, OREC_RVER(orec[a])>>};
            end if;
        end with;
        goto L_active;
    or \* Write (acquire w_lock)
        with a \in Addr, n \in Data do
            if a \notin writeLog /\ OREC_WLOCK(orec[a]) = 0 then
                orec[a] := MAKE_OREC(OREC_RLOCK(orec[a]), 1, OREC_RVER(orec[a]), self);
                writeLog := writeLog \union {a};
                writeBuf[a] := n;
                oldVal[a] := mem[a];
                readOnly := FALSE;
            end if;
        end with;
        goto L_active;
    or \* Write (already in writeLog — update)
        with a \in Addr, n \in Data do
            if a \in writeLog then
                writeBuf[a] := n;
            end if;
        end with;
        goto L_active;
    or \* Write (conflict — w_lock held by another)
        if \E a \in Addr : a \notin writeLog /\ OREC_WLOCK(orec[a]) = 1 /\ OREC_WOWNER(orec[a]) # self then
            goto L_abort;
        else
            goto L_active;
        end if;
    or \* Commit read-only
        if readOnly then
            committed[self] := committed[self] + 1;
            goto L_idle;
        else
            goto L_active;
        end if;
    or \* Commit (acquire read-locks first)
        if ~readOnly /\ writeLog # {}
           /\ \A <<a, v>> \in readSet : OREC_RLOCK(orec[a]) = 0
        then
            orec := [a \in Addr |->
                IF \E entry \in readSet : entry[1] = a
                THEN MAKE_OREC(1, OREC_WLOCK(orec[a]), OREC_RVER(orec[a]), OREC_WOWNER(orec[a]))
                ELSE orec[a]];
            goto L_commit;
        else
            goto L_active;
        end if;
    end either;

L_commit:
    g_ts := g_ts + 1;
    if \A <<addr, ver>> \in readSet :
        OREC_WOWNER(orec[addr]) = self \/ OREC_RVER(orec[addr]) = ver
    then
        goto L_commit_wb;
    else
        (* Release all locks (read + write) without bumping versions *)
        orec := [a \in Addr |->
            IF a \in writeLog
            THEN MAKE_OREC(0, 0, OREC_RVER(orec[a]), 0)
            ELSE IF \E entry \in readSet : entry[1] = a
                THEN MAKE_OREC(0, OREC_WLOCK(orec[a]), OREC_RVER(orec[a]), OREC_WOWNER(orec[a]))
                ELSE orec[a]];
        readSet := {};
        writeLog := {};
        goto L_abort;
    end if;

L_commit_wb:
    (* Write-back and release with new version *)
    mem := [a \in Addr |->
        IF a \in writeLog THEN writeBuf[a] ELSE mem[a]];
    orec := [a \in Addr |->
        IF a \in writeLog
        THEN MAKE_OREC(0, 0, g_ts, 0)
        ELSE IF \E entry \in readSet : entry[1] = a
            THEN MAKE_OREC(0, OREC_WLOCK(orec[a]), OREC_RVER(orec[a]), OREC_WOWNER(orec[a]))
            ELSE orec[a]];
    committed[self] := committed[self] + 1;
    goto L_idle;

L_abort:
    readSet := {};
    writeLog := {};
    readOnly := TRUE;
    aborted[self] := aborted[self] + 1;
    goto L_idle;

L_done:
    skip;

end process;

end algorithm; *)

\* BEGIN TRANSLATION
VARIABLES pc, g_ts, orec, mem, committed, aborted, lastFence, readSet, writeLog, 
          writeBuf, oldVal, readOnly

vars == << pc, g_ts, orec, mem, committed, aborted, lastFence, readSet, writeLog, 
           writeBuf, oldVal, readOnly >>

ProcSet == (Thread)

Init == (* Global variables *)
        /\ g_ts = 0
        /\ orec = [a \in Addr |-> MAKE_OREC(0, 0, 0, 0)]
        /\ mem = [a \in Addr |-> 0]
        /\ committed = [t \in Thread |-> 0]
        /\ aborted = [t \in Thread |-> 0]
        /\ lastFence = [t \in Thread |-> ""]
        (* Process ThreadProc *)
        /\ readSet = [self \in Thread |-> {}]
        /\ writeLog = [self \in Thread |-> {}]
        /\ writeBuf = [self \in Thread |-> [a \in Addr |-> 0]]
        /\ oldVal = [self \in Thread |-> [a \in Addr |-> 0]]
        /\ readOnly = [self \in Thread |-> TRUE]
        /\ pc = [self \in ProcSet |-> "L_idle"]

L_idle(self) == /\ pc[self] = "L_idle"
                /\ IF committed[self] >= MaxCommits
                      THEN /\ pc' = [pc EXCEPT ![self] = "L_done"]
                      ELSE /\ pc' = [pc EXCEPT ![self] = "L_begin"]
                /\ UNCHANGED << g_ts, orec, mem, committed, aborted, lastFence, readSet, 
                                writeLog, writeBuf, oldVal, readOnly >>

L_begin(self) == /\ pc[self] = "L_begin"
                 /\ readSet' = [readSet EXCEPT ![self] = {}]
                 /\ writeLog' = [writeLog EXCEPT ![self] = {}]
                 /\ readOnly' = [readOnly EXCEPT ![self] = TRUE]
                 /\ lastFence' = [lastFence EXCEPT ![self] = ""]
                 /\ pc' = [pc EXCEPT ![self] = "L_active"]
                 /\ UNCHANGED << g_ts, orec, mem, committed, aborted, writeBuf, 
                                 oldVal >>

L_active(self) == /\ pc[self] = "L_active"
                  /\ \/ /\ \E a \in Addr:
                             IF a \in writeLog[self]
                                THEN /\ TRUE
                                ELSE /\ TRUE
                        /\ pc' = [pc EXCEPT ![self] = "L_active"]
                        /\ UNCHANGED <<lastFence, orec, committed, readSet, writeLog, writeBuf, oldVal, readOnly>>
                     \/ /\ \E a \in Addr:
                             IF a \notin writeLog[self] /\ ~(OREC_WLOCK(orec[a]) = 1 /\ OREC_WOWNER(orec[a]) # self)
                                THEN /\ readSet' = [readSet EXCEPT ![self] = readSet[self] \union {<<a, OREC_RVER(orec[a])>>}]
                                ELSE /\ TRUE
                                     /\ UNCHANGED readSet
                        /\ lastFence' = [lastFence EXCEPT ![self] = "sc"]
                        /\ pc' = [pc EXCEPT ![self] = "L_active"]
                        /\ UNCHANGED <<orec, committed, writeLog, writeBuf, oldVal, readOnly>>
                     \/ /\ \E a \in Addr:
                             \E n \in Data:
                               IF a \notin writeLog[self] /\ OREC_WLOCK(orec[a]) = 0
                                  THEN /\ orec' = [orec EXCEPT ![a] = MAKE_OREC(OREC_RLOCK(orec[a]), 1, OREC_RVER(orec[a]), self)]
                                       /\ writeLog' = [writeLog EXCEPT ![self] = writeLog[self] \union {a}]
                                       /\ writeBuf' = [writeBuf EXCEPT ![self][a] = n]
                                       /\ oldVal' = [oldVal EXCEPT ![self][a] = mem[a]]
                                       /\ readOnly' = [readOnly EXCEPT ![self] = FALSE]
                                  ELSE /\ TRUE
                                       /\ UNCHANGED << orec, writeLog, 
                                                       writeBuf, oldVal, 
                                                       readOnly >>
                        /\ lastFence' = [lastFence EXCEPT ![self] = "acq"]
                        /\ pc' = [pc EXCEPT ![self] = "L_active"]
                        /\ UNCHANGED <<committed, readSet>>
                     \/ /\ \E a \in Addr:
                             \E n \in Data:
                               IF a \in writeLog[self]
                                  THEN /\ writeBuf' = [writeBuf EXCEPT ![self][a] = n]
                                  ELSE /\ TRUE
                                       /\ UNCHANGED writeBuf
                        /\ pc' = [pc EXCEPT ![self] = "L_active"]
                        /\ UNCHANGED <<lastFence, orec, committed, readSet, writeLog, oldVal, readOnly>>
                     \/ /\ IF \E a \in Addr : a \notin writeLog[self] /\ OREC_WLOCK(orec[a]) = 1 /\ OREC_WOWNER(orec[a]) # self
                              THEN /\ lastFence' = [lastFence EXCEPT ![self] = "rel"]
                                   /\ pc' = [pc EXCEPT ![self] = "L_abort"]
                              ELSE /\ UNCHANGED lastFence
                                   /\ pc' = [pc EXCEPT ![self] = "L_active"]
                        /\ UNCHANGED <<orec, committed, readSet, writeLog, writeBuf, oldVal, readOnly>>
                     \/ /\ IF readOnly[self]
                              THEN /\ committed' = [committed EXCEPT ![self] = committed[self] + 1]
                                   /\ pc' = [pc EXCEPT ![self] = "L_idle"]
                              ELSE /\ pc' = [pc EXCEPT ![self] = "L_active"]
                                   /\ UNCHANGED committed
                        /\ UNCHANGED <<lastFence, orec, readSet, writeLog, writeBuf, oldVal, readOnly>>
                     \/ /\ IF ~readOnly[self] /\ writeLog[self] # {}
                              /\ \A <<a, v>> \in readSet[self] : OREC_RLOCK(orec[a]) = 0
                              THEN /\ orec' =     [a \in Addr |->
                                              IF \E entry \in readSet[self] : entry[1] = a
                                              THEN MAKE_OREC(1, OREC_WLOCK(orec[a]), OREC_RVER(orec[a]), OREC_WOWNER(orec[a]))
                                              ELSE orec[a]]
                                    /\ lastFence' = [lastFence EXCEPT ![self] = "sc"]
                                    /\ pc' = [pc EXCEPT ![self] = "L_commit"]
                              ELSE /\ UNCHANGED lastFence
                                   /\ pc' = [pc EXCEPT ![self] = "L_active"]
                                   /\ orec' = orec
                        /\ UNCHANGED <<committed, readSet, writeLog, writeBuf, oldVal, readOnly>>
                  /\ UNCHANGED << g_ts, mem, aborted >>

L_commit(self) == /\ pc[self] = "L_commit"
                  /\ g_ts' = g_ts + 1
                  /\ IF \A <<addr, ver>> \in readSet[self] :
                         OREC_WOWNER(orec[addr]) = self \/ OREC_RVER(orec[addr]) = ver
                        THEN /\ lastFence' = [lastFence EXCEPT ![self] = "sc"]
                             /\ pc' = [pc EXCEPT ![self] = "L_commit_wb"]
                             /\ UNCHANGED << orec, readSet, writeLog >>
                        ELSE /\ lastFence' = [lastFence EXCEPT ![self] = "rel"]
                             /\ orec' =     [a \in Addr |->
                                        IF a \in writeLog[self]
                                        THEN MAKE_OREC(0, 0, OREC_RVER(orec[a]), 0)
                                        ELSE IF \E entry \in readSet[self] : entry[1] = a
                                            THEN MAKE_OREC(0, OREC_WLOCK(orec[a]), OREC_RVER(orec[a]), OREC_WOWNER(orec[a]))
                                            ELSE orec[a]]
                             /\ readSet' = [readSet EXCEPT ![self] = {}]
                             /\ writeLog' = [writeLog EXCEPT ![self] = {}]
                             /\ pc' = [pc EXCEPT ![self] = "L_abort"]
                  /\ UNCHANGED << mem, committed, aborted, writeBuf, oldVal, 
                                  readOnly >>

L_commit_wb(self) == /\ pc[self] = "L_commit_wb"
                     /\ mem' =    [a \in Addr |->
                               IF a \in writeLog[self] THEN writeBuf[self][a] ELSE mem[a]]
                     /\ orec' =     [a \in Addr |->
                                IF a \in writeLog[self]
                                THEN MAKE_OREC(0, 0, g_ts, 0)
                                ELSE IF \E entry \in readSet[self] : entry[1] = a
                                    THEN MAKE_OREC(0, OREC_WLOCK(orec[a]), OREC_RVER(orec[a]), OREC_WOWNER(orec[a]))
                                    ELSE orec[a]]
                     /\ committed' = [committed EXCEPT ![self] = committed[self] + 1]
                     /\ lastFence' = [lastFence EXCEPT ![self] = "rel"]
                     /\ pc' = [pc EXCEPT ![self] = "L_idle"]
                     /\ UNCHANGED << g_ts, aborted, readSet, writeLog, 
                                     writeBuf, oldVal, readOnly >>

L_abort(self) == /\ pc[self] = "L_abort"
                  /\ readSet' = [readSet EXCEPT ![self] = {}]
                  /\ writeLog' = [writeLog EXCEPT ![self] = {}]
                  /\ readOnly' = [readOnly EXCEPT ![self] = TRUE]
                  /\ aborted' = [aborted EXCEPT ![self] = aborted[self] + 1]
                  /\ lastFence' = [lastFence EXCEPT ![self] = "rel"]
                  /\ pc' = [pc EXCEPT ![self] = "L_idle"]
                  /\ UNCHANGED << g_ts, orec, mem, committed, writeBuf, oldVal >>

L_done(self) == /\ pc[self] = "L_done"
                /\ TRUE
                /\ pc' = [pc EXCEPT ![self] = "Done"]
                /\ UNCHANGED << g_ts, orec, mem, committed, aborted, lastFence, readSet, 
                                writeLog, writeBuf, oldVal, readOnly >>

ThreadProc(self) == L_idle(self) \/ L_begin(self) \/ L_active(self)
                       \/ L_commit(self) \/ L_commit_wb(self)
                       \/ L_abort(self) \/ L_done(self)

(* Allow infinite stuttering to prevent deadlock on termination. *)
Terminating == /\ \A self \in ProcSet: pc[self] = "Done"
               /\ UNCHANGED vars

Next == (\E self \in Thread: ThreadProc(self))
           \/ Terminating

Spec == Init /\ [][Next]_vars

Termination == <>(\A self \in ProcSet: pc[self] = "Done")

\* END TRANSLATION

(*====================================================================*)
(* Invariants                                                          *)
(*====================================================================*)

(* I1: No two threads hold w_lock for the same address *)
MutexWriteLock ==
    \A a \in Addr :
        OREC_WLOCK(orec[a]) = 1 =>
            \E t \in Thread : OREC_WOWNER(orec[a]) = t

(* I2: w_lock owner is in writeLog *)
WriteOwnerInv ==
    \A a \in Addr :
        OREC_WLOCK(orec[a]) = 1 =>
            \E t \in Thread :
                OREC_WOWNER(orec[a]) = t /\ a \in writeLog[t]

(* I3: No thread holds a write-lock when idle *)
NoPostCommitLocks ==
    \A t \in Thread :
        pc[t] \in {"L_idle", "L_begin", "L_done"} =>
            \A a \in Addr : ~(OREC_WLOCK(orec[a]) = 1 /\ OREC_WOWNER(orec[a]) = t)

(* I4: Every thread with a non-empty write-set has issued a fence *)
FenceFidelity ==
    \A t \in Thread : writeLog[t] # {} => lastFence[t] # ""

(* Combined invariant *)
Inv ==
    /\ MutexWriteLock
    /\ WriteOwnerInv
    /\ NoPostCommitLocks
    /\ FenceFidelity

(* Constraint for bounded model checking *)
ModelBound == g_ts <= 5 /\ \A t \in Thread : aborted[t] <= MaxCommits * 2

(*====================================================================*)
(* Fairness and Liveness                                              *)
(*====================================================================*)

(* Weak fairness on each thread process *)
Spec_WF == Spec /\ \A self \in Thread : WF_vars(ThreadProc(self))

(* Strong fairness on each thread process *)
Spec_SF == Spec /\ \A self \in Thread : SF_vars(ThreadProc(self))

(* Liveness: every active thread eventually becomes idle *)
ProgressProperty ==
    \A self \in Thread : (pc[self] = "L_active" ~> pc[self] \in {"L_idle", "L_begin", "L_done"})

=====
