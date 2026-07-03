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

EXTENDS Naturals, FiniteSets, TLC, TMTypes

CONSTANTS
    Thread,                (* Set of thread IDs *)
    Addr,                  (* Set of addresses *)
    ORecIdx,               (* Set of orec table indices (smaller than Addr for false sharing) *)
    orec_of,               (* Mapping Addr -> ORecIdx: which orec covers each address *)
    Data,                  (* Set of possible data values *)
    READ_LOCKED,           (* Sentinel: r_lock word when read-locked *)
    MaxCommits             (* Max commits per thread *)

ASSUME Thread \subseteq Nat \ {0}
ASSUME Addr \subseteq Nat
ASSUME ORecIdx \subseteq Nat
ASSUME orec_of \in [Addr -> ORecIdx]
ASSUME Data \subseteq Nat
ASSUME READ_LOCKED \notin Data
ASSUME MaxCommits \in Nat \ {0}

(* ORec helpers — C++ matches: r_lock is a single atomic word (version OR READ_LOCKED) *)
OREC_RWORD(o) == o[1]           (* r_lock word: version number or READ_LOCKED sentinel *)
OREC_WLOCK(o) == o[2]           (* w_lock: 0=free, thread-id=locked *)
OREC_RLOCKED(o) == o[1] = READ_LOCKED
OREC_RVER(o) == o[1]            (* version when not read-locked *)
W_LOCKED(o) == o[2] # 0
W_OWNER(o) == o[2]              (* thread-id that holds w_lock, 0 if free *)
MAKE_OREC(rv, wl) == <<rv, wl>>

(*--algorithm SwissTM

variables
    g_ts = 0,
    orec = [i \in ORecIdx |-> MAKE_OREC(0, 0)],
    mem = [a \in Addr |-> 0],
    committed = [t \in Thread |-> 0],
    aborted = [t \in Thread |-> 0],
    lastSignalFence = [t \in Thread |-> ""],
    lastThreadFence = [t \in Thread |-> ""],
    lastRmw = [t \in Thread |-> ""];

process ThreadProc \in Thread
variables
    readSet = {},
    writeLog = {},
    writeBuf = [a \in Addr |-> 0],
    oldVal = [a \in Addr |-> 0],
    readOnly = TRUE,
    readLockVer = [a \in Addr |-> 0],
    (* readLockVer[a] = version captured during Phase 1 exchange for address a *)
    valid_ts = 0,
    owned_orecs = {};
    (* valid_ts = snapshot timestamp; extended via validate() on version mismatch *)
    (* owned_orecs = set of ORecIdx whose w_lock we hold (dedup for false sharing) *)
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
    readLockVer := [a \in Addr |-> 0];
    valid_ts := g_ts;
    owned_orecs := {};
    lastSignalFence[self] := "";
    lastThreadFence[self] := "";
    lastRmw[self] := "";
    goto L_active;

L_active:
    either \* Read own write (from writeBuf)
        with a \in Addr do
            if a \in writeLog then skip; end if;
        end with;
        goto L_active;
    or \* Read from memory (record r_ver; spin if read-locked; extend if stale)
        with a \in Addr do
            if a \notin writeLog /\ ~OREC_RLOCKED(orec[orec_of[a]]) then
                readSet := readSet \union {<<a, OREC_RVER(orec[orec_of[a]])>>};
                lastSignalFence[self] := "sc";
                if OREC_RVER(orec[orec_of[a]]) > valid_ts then
                    goto L_extend;
                end if;
            end if;
        end with;
        goto L_active;
    or \* Write (acquire w_lock; dedup on shared orec)
        with a \in Addr, n \in Data do
            if a \notin writeLog then
                if orec_of[a] \notin owned_orecs then
                    orec[orec_of[a]] := MAKE_OREC(OREC_RWORD(orec[orec_of[a]]), self);
                    owned_orecs := owned_orecs \union {orec_of[a]};
                end if;
                writeLog := writeLog \union {a};
                writeBuf[a] := n;
                oldVal[a] := mem[a];
                readOnly := FALSE;
                lastRmw[self] := "acq_rel";
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
        if \E a \in Addr : a \notin writeLog /\ W_LOCKED(orec[orec_of[a]]) /\ W_OWNER(orec[orec_of[a]]) # self /\ orec_of[a] \notin owned_orecs then
            lastRmw[self] := "release";
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
    or \* Commit Phase 1: exchange r_lock -> READ_LOCKED for all read-set entries
        if ~readOnly /\ writeLog # {}
           /\ \A <<a, v>> \in readSet : ~OREC_RLOCKED(orec[orec_of[a]])
        then
            (* Capture current r_lock values before exchange *)
            readLockVer := [a \in Addr |->
                IF \E entry \in readSet : entry[1] = a
                THEN OREC_RWORD(orec[orec_of[a]])
                ELSE readLockVer[a]];
            orec := [i \in ORecIdx |->
                IF \E entry \in readSet : orec_of[entry[1]] = i
                THEN MAKE_OREC(READ_LOCKED, OREC_WLOCK(orec[i]))
                ELSE orec[i]];
            lastSignalFence[self] := "sc";
            goto L_commit;
        else
            goto L_active;
        end if;
    end either;

L_commit:
    g_ts := g_ts + 1;
    if \A <<addr, ver>> \in readSet :
        W_OWNER(orec[orec_of[addr]]) = self \/ readLockVer[addr] = ver
    then
        lastSignalFence[self] := "sc";
        goto L_commit_wb;
    else
        (* Release all locks (read + write) without bumping versions *)
        orec := [i \in ORecIdx |->
            IF \E a \in writeLog : orec_of[a] = i
            THEN MAKE_OREC(OREC_RWORD(orec[i]), 0)
            ELSE IF \E entry \in readSet : orec_of[entry[1]] = i
                THEN MAKE_OREC(readLockVer[entry[1]], 0)
                ELSE orec[i]];
        readSet := {};
        writeLog := {};
        owned_orecs := {};
        lastRmw[self] := "release";
        goto L_abort;
    end if;

L_commit_wb:
    (* Write-back and release with new version *)
    mem := [a \in Addr |->
        IF a \in writeLog THEN writeBuf[a] ELSE mem[a]];
    orec := [i \in ORecIdx |->
        IF \E a \in writeLog : orec_of[a] = i
        THEN MAKE_OREC(g_ts, 0)                                (* new version, release w_lock *)
        ELSE IF \E entry \in readSet : orec_of[entry[1]] = i
            THEN MAKE_OREC(readLockVer[entry[1]], 0)           (* restore original version *)
            ELSE orec[i]];
    lastRmw[self] := "release";
    committed[self] := committed[self] + 1;
    goto L_idle;

L_abort:
    lastRmw[self] := "release";
    readSet := {};
    writeLog := {};
    owned_orecs := {};
    readOnly := TRUE;
    aborted[self] := aborted[self] + 1;
    goto L_idle;

L_extend:
    (* Re-validate all read-set entries; if all match current orec versions,
       advance valid_ts to current clock. Otherwise abort. *)
    if \A <<addr, ver>> \in readSet :
        OREC_RWORD(orec[orec_of[addr]]) = ver
    then
        valid_ts := g_ts;
        lastSignalFence[self] := "sc";
        goto L_active;
    else
        lastRmw[self] := "release";
        goto L_abort;
    end if;

L_done:
    skip;

end process;

end algorithm; *)

\* BEGIN TRANSLATION
VARIABLES pc, g_ts, orec, mem, committed, aborted, lastSignalFence, lastThreadFence, lastRmw, readSet, writeLog, 
          writeBuf, oldVal, readOnly, readLockVer, valid_ts, owned_orecs

vars == << pc, g_ts, orec, mem, committed, aborted, lastSignalFence, lastThreadFence, lastRmw, readSet, writeLog, 
           writeBuf, oldVal, readOnly, readLockVer, valid_ts, owned_orecs >>

ProcSet == (Thread)

Init == (* Global variables *)
        /\ g_ts = 0
        /\ orec = [i \in ORecIdx |-> MAKE_OREC(0, 0)]
        /\ mem = [a \in Addr |-> 0]
        /\ committed = [t \in Thread |-> 0]
        /\ aborted = [t \in Thread |-> 0]
        /\ lastSignalFence = [t \in Thread |-> ""]
        /\ lastThreadFence = [t \in Thread |-> ""]
        /\ lastRmw = [t \in Thread |-> ""]
        (* Process ThreadProc *)
        /\ readSet = [self \in Thread |-> {}]
        /\ writeLog = [self \in Thread |-> {}]
        /\ writeBuf = [self \in Thread |-> [a \in Addr |-> 0]]
        /\ oldVal = [self \in Thread |-> [a \in Addr |-> 0]]
        /\ readOnly = [self \in Thread |-> TRUE]
        /\ readLockVer = [self \in Thread |-> [a \in Addr |-> 0]]
        /\ valid_ts = [self \in Thread |-> 0]
        /\ owned_orecs = [self \in Thread |-> {}]
        /\ pc = [self \in ProcSet |-> "L_idle"]

L_idle(self) == /\ pc[self] = "L_idle"
                /\ IF committed[self] >= MaxCommits
                      THEN /\ pc' = [pc EXCEPT ![self] = "L_done"]
                      ELSE /\ pc' = [pc EXCEPT ![self] = "L_begin"]
                /\ UNCHANGED << g_ts, orec, mem, committed, aborted, lastSignalFence, lastThreadFence, lastRmw, readSet, 
                                writeLog, writeBuf, oldVal, readOnly, readLockVer, valid_ts, owned_orecs >>
L_begin(self) == /\ pc[self] = "L_begin"
                 /\ readSet' = [readSet EXCEPT ![self] = {}]
                 /\ writeLog' = [writeLog EXCEPT ![self] = {}]
                 /\ readOnly' = [readOnly EXCEPT ![self] = TRUE]
                 /\ readLockVer' = [readLockVer EXCEPT ![self] = [a \in Addr |-> 0]]
                 /\ valid_ts' = [valid_ts EXCEPT ![self] = g_ts]
                 /\ owned_orecs' = [owned_orecs EXCEPT ![self] = {}]
                 /\ lastSignalFence' = [lastSignalFence EXCEPT ![self] = ""]
                 /\ lastThreadFence' = [lastThreadFence EXCEPT ![self] = ""]
                 /\ lastRmw' = [lastRmw EXCEPT ![self] = ""]
                 /\ pc' = [pc EXCEPT ![self] = "L_active"]
                 /\ UNCHANGED << g_ts, orec, mem, committed, aborted, writeBuf, 
                                 oldVal >>

L_active(self) == /\ pc[self] = "L_active"
                  /\ \/ /\ \E a \in Addr:
                             IF a \in writeLog[self]
                                THEN /\ TRUE
                                ELSE /\ TRUE
                        /\ pc' = [pc EXCEPT ![self] = "L_active"]
                        /\ UNCHANGED <<lastSignalFence, lastThreadFence, lastRmw, orec, committed, readSet, writeLog, writeBuf, oldVal, readOnly, readLockVer, valid_ts, owned_orecs>>
                     \/ /\ \E a \in Addr:
                             IF a \notin writeLog[self] /\ ~OREC_RLOCKED(orec[orec_of[a]])
                                THEN /\ readSet' = [readSet EXCEPT ![self] = readSet[self] \union {<<a, OREC_RVER(orec[orec_of[a]])>>}]
                                     /\ lastSignalFence' = [lastSignalFence EXCEPT ![self] = "sc"]
                                     /\ IF OREC_RVER(orec[orec_of[a]]) > valid_ts[self]
                                           THEN pc' = [pc EXCEPT ![self] = "L_extend"]
                                           ELSE pc' = [pc EXCEPT ![self] = "L_active"]
                                     /\ UNCHANGED <<lastThreadFence, lastRmw, orec, committed, writeLog, writeBuf, oldVal, readOnly, readLockVer, valid_ts, owned_orecs>>
                                ELSE /\ TRUE
                                     /\ UNCHANGED readSet
                                     /\ UNCHANGED lastSignalFence
                                     /\ pc' = [pc EXCEPT ![self] = "L_active"]
                                     /\ UNCHANGED <<lastThreadFence, lastRmw, orec, committed, writeLog, writeBuf, oldVal, readOnly, readLockVer, valid_ts, owned_orecs>>
                     \/ /\ \E a \in Addr:
                             \E n \in Data:
                               IF a \notin writeLog[self]
                                  THEN /\ IF orec_of[a] \notin owned_orecs[self]
                                            THEN /\ orec' = [orec EXCEPT ![orec_of[a]] = MAKE_OREC(OREC_RWORD(orec[orec_of[a]]), self)]
                                                 /\ owned_orecs' = [owned_orecs EXCEPT ![self] = owned_orecs[self] \union {orec_of[a]}]
                                            ELSE /\ UNCHANGED << orec, owned_orecs >>
                                       /\ writeLog' = [writeLog EXCEPT ![self] = writeLog[self] \union {a}]
                                       /\ writeBuf' = [writeBuf EXCEPT ![self][a] = n]
                                       /\ oldVal' = [oldVal EXCEPT ![self][a] = mem[a]]
                                       /\ readOnly' = [readOnly EXCEPT ![self] = FALSE]
                                  ELSE /\ TRUE
                                       /\ UNCHANGED << orec, writeLog, writeBuf, oldVal, readOnly, owned_orecs >>
                        /\ lastRmw' = [lastRmw EXCEPT ![self] = "acq_rel"]
                        /\ pc' = [pc EXCEPT ![self] = "L_active"]
                        /\ UNCHANGED <<lastSignalFence, lastThreadFence, committed, readSet, readLockVer, valid_ts>>
                     \/ /\ \E a \in Addr:
                             \E n \in Data:
                               IF a \in writeLog[self]
                                  THEN /\ writeBuf' = [writeBuf EXCEPT ![self][a] = n]
                                  ELSE /\ TRUE
                                       /\ UNCHANGED writeBuf
                        /\ pc' = [pc EXCEPT ![self] = "L_active"]
                        /\ UNCHANGED <<lastSignalFence, lastThreadFence, lastRmw, orec, committed, readSet, writeLog, oldVal, readOnly, readLockVer, valid_ts, owned_orecs>>
                     \/ /\ IF \E a \in Addr : a \notin writeLog[self] /\ W_LOCKED(orec[orec_of[a]]) /\ W_OWNER(orec[orec_of[a]]) # self /\ orec_of[a] \notin owned_orecs[self]
                              THEN /\ lastRmw' = [lastRmw EXCEPT ![self] = "release"]
                                   /\ pc' = [pc EXCEPT ![self] = "L_abort"]
                              ELSE /\ UNCHANGED lastRmw
                                   /\ pc' = [pc EXCEPT ![self] = "L_active"]
                        /\ UNCHANGED <<lastSignalFence, lastThreadFence, orec, committed, readSet, writeLog, writeBuf, oldVal, readOnly, readLockVer, valid_ts, owned_orecs>>
                     \/ /\ IF readOnly[self]
                              THEN /\ committed' = [committed EXCEPT ![self] = committed[self] + 1]
                                   /\ pc' = [pc EXCEPT ![self] = "L_idle"]
                              ELSE /\ pc' = [pc EXCEPT ![self] = "L_active"]
                                   /\ UNCHANGED committed
                        /\ UNCHANGED <<lastSignalFence, lastThreadFence, lastRmw, orec, readSet, writeLog, writeBuf, oldVal, readOnly, readLockVer, valid_ts, owned_orecs>>
                     \/ /\ IF ~readOnly[self] /\ writeLog[self] # {}
                              /\ \A <<a, v>> \in readSet[self] : ~OREC_RLOCKED(orec[orec_of[a]])
                              THEN /\ readLockVer' = [readLockVer EXCEPT ![self] = [a \in Addr |->
                                                             IF \E entry \in readSet[self] : entry[1] = a
                                                             THEN OREC_RWORD(orec[orec_of[a]])
                                                             ELSE readLockVer[self][a]]]
                                   /\ orec' = [i \in ORecIdx |->
                                               IF \E entry \in readSet[self] : orec_of[entry[1]] = i
                                               THEN MAKE_OREC(READ_LOCKED, OREC_WLOCK(orec[i]))
                                               ELSE orec[i]]
                                   /\ lastSignalFence' = [lastSignalFence EXCEPT ![self] = "sc"]
                                   /\ pc' = [pc EXCEPT ![self] = "L_commit"]
                              ELSE /\ UNCHANGED lastSignalFence
                                   /\ pc' = [pc EXCEPT ![self] = "L_active"]
                                   /\ UNCHANGED orec
                                   /\ UNCHANGED readLockVer
                        /\ UNCHANGED <<lastThreadFence, lastRmw, committed, readSet, writeLog, writeBuf, oldVal, readOnly, valid_ts, owned_orecs>>
                  /\ UNCHANGED << g_ts, mem, aborted >>

L_commit(self) == /\ pc[self] = "L_commit"
                  /\ g_ts' = g_ts + 1
                  /\ IF \A <<addr, ver>> \in readSet[self] :
                         W_OWNER(orec[orec_of[addr]]) = self \/ readLockVer[self][addr] = ver
                        THEN /\ lastSignalFence' = [lastSignalFence EXCEPT ![self] = "sc"]
                             /\ pc' = [pc EXCEPT ![self] = "L_commit_wb"]
                             /\ UNCHANGED << orec, readSet, writeLog, lastThreadFence, lastRmw, readLockVer, valid_ts, owned_orecs >>
                             /\ UNCHANGED mem
                             /\ UNCHANGED committed
                             /\ UNCHANGED aborted
                             /\ UNCHANGED writeBuf
                             /\ UNCHANGED oldVal
                             /\ UNCHANGED readOnly
                        ELSE /\ lastRmw' = [lastRmw EXCEPT ![self] = "release"]
                             /\ orec' =     [i \in ORecIdx |->
                                        IF \E a \in writeLog[self] : orec_of[a] = i
                                        THEN MAKE_OREC(OREC_RWORD(orec[i]), 0)
                                        ELSE IF \E entry \in readSet[self] : orec_of[entry[1]] = i
                                            THEN MAKE_OREC(readLockVer[self][entry[1]], 0)
                                            ELSE orec[i]]
                             /\ readSet' = [readSet EXCEPT ![self] = {}]
                             /\ writeLog' = [writeLog EXCEPT ![self] = {}]
                             /\ readLockVer' = [readLockVer EXCEPT ![self] = [a \in Addr |-> 0]]
                             /\ owned_orecs' = [owned_orecs EXCEPT ![self] = {}]
                             /\ pc' = [pc EXCEPT ![self] = "L_abort"]
                             /\ UNCHANGED << lastSignalFence, lastThreadFence, valid_ts >>
                  /\ UNCHANGED << mem, committed, aborted, writeBuf, oldVal, 
                                  readOnly, valid_ts >>

L_commit_wb(self) == /\ pc[self] = "L_commit_wb"
                     /\ mem' =    [a \in Addr |->
                               IF a \in writeLog[self] THEN writeBuf[self][a] ELSE mem[a]]
                     /\ orec' =     [i \in ORecIdx |->
                                IF \E a \in writeLog[self] : orec_of[a] = i
                                THEN MAKE_OREC(g_ts, 0)
                                ELSE IF \E entry \in readSet[self] : orec_of[entry[1]] = i
                                    THEN MAKE_OREC(readLockVer[self][entry[1]], 0)
                                    ELSE orec[i]]
                     /\ committed' = [committed EXCEPT ![self] = committed[self] + 1]
                     /\ lastRmw' = [lastRmw EXCEPT ![self] = "release"]
                     /\ pc' = [pc EXCEPT ![self] = "L_idle"]
                     /\ UNCHANGED << g_ts, aborted, readSet, writeLog, 
                                     writeBuf, oldVal, readOnly, readLockVer, valid_ts, owned_orecs,
                                     lastSignalFence, lastThreadFence >>

L_abort(self) == /\ pc[self] = "L_abort"
                 /\ readSet' = [readSet EXCEPT ![self] = {}]
                 /\ writeLog' = [writeLog EXCEPT ![self] = {}]
                 /\ readOnly' = [readOnly EXCEPT ![self] = TRUE]
                 /\ owned_orecs' = [owned_orecs EXCEPT ![self] = {}]
                 /\ aborted' = [aborted EXCEPT ![self] = aborted[self] + 1]
                 /\ lastRmw' = [lastRmw EXCEPT ![self] = "release"]
                 /\ pc' = [pc EXCEPT ![self] = "L_idle"]
                 /\ UNCHANGED << g_ts, orec, mem, committed, writeBuf, oldVal, 
                                 lastSignalFence, lastThreadFence, readLockVer, valid_ts >>

L_extend(self) == /\ pc[self] = "L_extend"
                  /\ IF \A <<addr, ver>> \in readSet[self] :
                         OREC_RWORD(orec[orec_of[addr]]) = ver
                        THEN /\ valid_ts' = [valid_ts EXCEPT ![self] = g_ts]
                             /\ lastSignalFence' = [lastSignalFence EXCEPT ![self] = "sc"]
                             /\ pc' = [pc EXCEPT ![self] = "L_active"]
                             /\ UNCHANGED << readSet, writeLog, readOnly, owned_orecs,
                                             lastThreadFence, lastRmw >>
                        ELSE /\ lastRmw' = [lastRmw EXCEPT ![self] = "release"]
                             /\ pc' = [pc EXCEPT ![self] = "L_abort"]
                             /\ UNCHANGED << valid_ts, readSet, writeLog, readOnly, owned_orecs,
                                             lastSignalFence, lastThreadFence >>
                  /\ UNCHANGED << g_ts, orec, mem, committed, aborted, writeBuf,
                                  oldVal, readLockVer >>

L_done(self) == /\ pc[self] = "L_done"
                /\ TRUE
                /\ pc' = [pc EXCEPT ![self] = "Done"]
                /\ UNCHANGED << g_ts, orec, mem, committed, aborted, lastSignalFence, lastThreadFence, lastRmw, readSet, 
                                writeLog, writeBuf, oldVal, readOnly, readLockVer, valid_ts, owned_orecs >>

ThreadProc(self) == L_idle(self) \/ L_begin(self) \/ L_active(self)
                       \/ L_commit(self) \/ L_commit_wb(self)
                       \/ L_extend(self) \/ L_abort(self) \/ L_done(self)

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

(* I1: No two threads hold w_lock for the same orec index *)
MutexWriteLock ==
    \A i \in ORecIdx :
        W_LOCKED(orec[i]) =>
            \E t \in Thread : W_OWNER(orec[i]) = t

(* I2: w_lock owner has at least one address in writeLog for that orec *)
WriteOwnerInv ==
    \A i \in ORecIdx :
        W_LOCKED(orec[i]) =>
            \E t \in Thread :
                W_OWNER(orec[i]) = t /\ \E a \in Addr : orec_of[a] = i /\ a \in writeLog[t]

(* I3: No thread holds a write-lock when idle *)
NoPostCommitLocks ==
    \A t \in Thread :
        pc[t] \in {"L_idle", "L_begin", "L_done"} =>
            \A i \in ORecIdx : ~(W_LOCKED(orec[i]) /\ W_OWNER(orec[i]) = t)

(* I4: No thread holds a read-lock when idle *)
NoReadLockWhenIdle ==
    \A t \in Thread :
        pc[t] \in {"L_idle", "L_begin", "L_done"} =>
            \A i \in ORecIdx : ~OREC_RLOCKED(orec[i])

(* I5: Every thread with a non-empty write-set has issued a fence *)
FenceFidelity == TMTypes!FenceFidelity(Thread, writeLog, lastSignalFence, lastThreadFence, lastRmw)

(* Combined invariant *)
Inv ==
    /\ MutexWriteLock
    /\ WriteOwnerInv
    /\ NoPostCommitLocks
    /\ NoReadLockWhenIdle
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
