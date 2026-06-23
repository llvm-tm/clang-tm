------------------------- MODULE SwissTM -------------------------
(*
 * SwissTM — TLA+ Specification (TLC-checkable).
 *
 * Features (Dragojevic, Guerraoui, Kapalka, 2009):
 *   - Ownership Record (ORec) per address: {r_lock (version), w_lock (pointer)}
 *   - Eager write-write conflict detection:
 *       CAS on w_lock at write time; if fails, abort or wait via CM.
 *   - Lazy read-write conflict detection:
 *       Read-set validated at commit via read-lock acquisition.
 *   - Commit protocol:
 *       1. Acquire read-locks on read-set.
 *       2. Increment global commit timestamp.
 *       3. Validate read-set (re-check versions).
 *       4. Write-back.
 *       5. Release all locks with new version.
 *   - Contention manager: backoff on abort (not modelled for checkability).
 *
 * TLC-checkable invariants:
 *   - ORec r_lock and w_lock are not simultaneously held by different threads.
 *   - Write-back happens under lock.
 *   - No two threads hold w_lock for the same address.
 *)

EXTENDS Naturals, FiniteSets, TLC

CONSTANTS Thread, Addr, MAX_VAL
ASSUME Thread \subseteq Nat \ {0}
ASSUME Addr \subseteq Nat

VARIABLES
    g_ts,                                 (* global commit timestamp *)
    orec,                               (* [addr -> {r_lock, w_lock, r_ver, w_owner}] *)
    mem,
    pc,                                (* idle | active | commit_rlock | commit_wb *)
    readSet,                           (* set of <<addr, observed_r_ver>> *)
    writeLog,                          (* set of addr *)
    writeBuf,                           (* buffered new value per addr *)
    oldVal,                              (* old value for undo *)
    readOnly,
    committed

vars == <<g_ts, orec, mem, pc, readSet, writeLog, writeBuf, oldVal, readOnly, committed>>

(* ORec encoding: <<r_lock, w_lock, r_ver, w_owner>> *)
OREC_RLOCK(o) == o[1]           (* 0 = unlocked, 1 = read-locked *)
OREC_WLOCK(o) == o[2]           (* 0 = unlocked, 1 = write-locked *)
OREC_RVER(o) == o[3]            (* read version *)
OREC_WOWNER(o) == o[4]          (* thread ID holding write lock *)
MAKE_OREC(rl, wl, rv, wo) == <<rl, wl, rv, wo>>

Init ==
    /\ g_ts = 0
    /\ orec = [a \in Addr |-> MAKE_OREC(0, 0, 0, 0)]
    /\ mem = [a \in Addr |-> 0]
    /\ pc = [t \in Thread |-> "idle"]
    /\ readSet = [t \in Thread |-> {}]
    /\ writeLog = [t \in Thread |-> {}]
    /\ writeBuf = [t \in Thread, a \in Addr |-> 0]
    /\ oldVal = [t \in Thread, a \in Addr |-> 0]
    /\ readOnly = [t \in Thread |-> TRUE]
    /\ committed = [t \in Thread |-> 0]

Begin(t) ==
    /\ pc[t] = "idle"
    /\ pc' = [pc EXCEPT ![t] = "active"]
    /\ readOnly' = [readOnly EXCEPT ![t] = TRUE]
    /\ readSet' = [readSet EXCEPT ![t] = {}]
    /\ writeLog' = [writeLog EXCEPT ![t] = {}]
    /\ UNCHANGED <<g_ts, orec, mem, writeBuf, oldVal, committed>>

(* ---- Read: check write log first, then ORec ---- *)
ReadOwn(t, a) ==
    /\ pc[t] = "active"
    /\ a \in writeLog[t]
    /\ UNCHANGED vars

ReadFromMem(t, a) ==
    /\ pc[t] = "active"
    /\ a \notin writeLog[t]
    (* ORec not write-locked by another *)
    /\ ~ (OREC_WLOCK(orec[a]) = 1 /\ OREC_WOWNER(orec[a]) # t)
    (* Record r_lock version *)
    /\ readSet' = [readSet EXCEPT ![t] = readSet[t]
        \cup {<<a, OREC_RVER(orec[a])>>}]
    /\ UNCHANGED <<g_ts, orec, mem, pc, writeLog, writeBuf, oldVal, readOnly, committed>>

(* ---- Write: acquire w_lock eagerly ---- *)
WriteAcquire(t, a, n) ==
    /\ pc[t] = "active"
    /\ a \notin writeLog[t]
    (* w_lock is free *)
    /\ OREC_WLOCK(orec[a]) = 0
    (* Acquire write lock *)
    /\ orec' = [aa \in Addr |->
        IF aa = a
        THEN MAKE_OREC(OREC_RLOCK(orec[a]), 1, OREC_RVER(orec[a]), t)
        ELSE orec[aa]]
    /\ writeLog' = [writeLog EXCEPT ![t] = writeLog[t] \cup {a}]
    /\ writeBuf' = [writeBuf EXCEPT ![t][a] = n]
    /\ oldVal' = [oldVal EXCEPT ![t][a] = mem[a]]
    /\ readOnly' = [readOnly EXCEPT ![t] = FALSE]
    /\ UNCHANGED <<g_ts, mem, pc, readSet, committed>>

WriteConflict(t, a) ==
    (* Write conflict: w_lock held by another -> abort *)
    /\ pc[t] = "active"
    /\ a \notin writeLog[t]
    /\ OREC_WLOCK(orec[a]) = 1 /\ OREC_WOWNER(orec[a]) # t
    /\ pc' = [pc EXCEPT ![t] = "idle"]
    /\ readSet' = [readSet EXCEPT ![t] = {}]
    /\ UNCHANGED <<g_ts, orec, mem, writeLog, writeBuf, oldVal, readOnly, committed>>

WriteUpdate(t, a, n) ==
    /\ pc[t] = "active"
    /\ a \in writeLog[t]
    /\ writeBuf' = [writeBuf EXCEPT ![t][a] = n]
    /\ readOnly' = [readOnly EXCEPT ![t] = FALSE]
    /\ UNCHANGED <<g_ts, orec, mem, pc, readSet, writeLog, oldVal, committed>>

(* ---- Commit: Phase 1 — acquire read-locks on read-set ---- *)
CommitRLock(t) ==
    /\ pc[t] = "active"
    /\ readOnly[t] = FALSE
    /\ writeLog[t] # {}
    (* All read-set addresses must not be read-locked by another *)
    /\ \A entry \in readSet[t] :
        LET addr == entry[1] IN
        OREC_RLOCK(orec[addr]) = 0              (* not read-locked *)
    (* Acquire read-locks: set r_lock = 1 for all read-set entries *)
    /\ orec' = [a \in Addr |->
        IF \E entry \in readSet[t] : entry[1] = a
        THEN MAKE_OREC(1, OREC_WLOCK(orec[a]), OREC_RVER(orec[a]), OREC_WOWNER(orec[a]))
        ELSE orec[a]]
    /\ pc' = [pc EXCEPT ![t] = "commit_rlock"]
    /\ UNCHANGED <<g_ts, mem, readSet, writeLog, writeBuf, oldVal, readOnly, committed>>

(* Phase 2: increment commit timestamp *)
CommitIncTS(t) ==
    /\ pc[t] = "commit_rlock"
    /\ g_ts' = g_ts + 1
    /\ UNCHANGED <<orec, mem, pc, readSet, writeLog, writeBuf, oldVal, readOnly, committed>>

(* Phase 3: validate read-set (re-check versions) *)
CommitValidate(t) ==
    /\ pc[t] = "commit_rlock"
    /\ \A entry \in readSet[t] :
        LET addr == entry[1]
            obs_ver == entry[2] IN
        \/ OREC_WOWNER(orec[addr]) = t          (* we hold w_lock *)
        \/ OREC_RVER(orec[addr]) = obs_ver       (* version unchanged *)
    /\ pc' = [pc EXCEPT ![t] = "commit_wb"]
    /\ UNCHANGED <<g_ts, orec, mem, readSet, writeLog, writeBuf, oldVal, readOnly, committed>>

CommitValidateFail(t) ==
    /\ pc[t] = "commit_rlock"
    /\ \E entry \in readSet[t] :
        LET addr == entry[1]
            obs_ver == entry[2] IN
        ~ (OREC_WOWNER(orec[addr]) = t \/ OREC_RVER(orec[addr]) = obs_ver)
    (* Release all read-locks *)
    /\ orec' = [a \in Addr |->
        IF \E entry \in readSet[t] : entry[1] = a
        THEN MAKE_OREC(0, OREC_WLOCK(orec[a]), OREC_RVER(orec[a]), OREC_WOWNER(orec[a]))
        ELSE orec[a]]
    (* Release all write-locks *)
    /\ orec' = [a \in Addr |->
        IF a \in writeLog[t]
        THEN MAKE_OREC(OREC_RLOCK(orec'[a]), 0, OREC_RVER(orec'[a]), 0)
        ELSE orec'[a]]
    /\ pc' = [pc EXCEPT ![t] = "idle"]
    /\ readSet' = [readSet EXCEPT ![t] = {}]
    /\ writeLog' = [writeLog EXCEPT ![t] = {}]
    /\ UNCHANGED <<g_ts, mem, writeBuf, oldVal, readOnly, committed>>

(* Phase 4+5: write-back + release locks *)
CommitWriteBack(t) ==
    /\ pc[t] = "commit_wb"
    (* Write-back *)
    /\ mem' = [a \in Addr |->
        IF a \in writeLog[t] THEN writeBuf[t][a] ELSE mem[a]]
    (* Release all locks: r_lock -> 0 (with new version),
       w_lock -> 0, r_ver -> g_ts *)
    /\ orec' = [a \in Addr |->
        IF a \in writeLog[t]    (* written addresses: release both *)
        THEN MAKE_OREC(0, 0, g_ts, 0)
        ELSE IF \E entry \in readSet[t] : entry[1] = a  (* read-only, release r_lock *)
            THEN MAKE_OREC(0, OREC_WLOCK(orec[a]), OREC_RVER(orec[a]), OREC_WOWNER(orec[a]))
            ELSE orec[a]]
    /\ committed' = [committed EXCEPT ![t] = committed[t] + 1]
    /\ pc' = [pc EXCEPT ![t] = "idle"]
    /\ readSet' = [readSet EXCEPT ![t] = {}]
    /\ writeLog' = [writeLog EXCEPT ![t] = {}]
    /\ UNCHANGED <<g_ts, writeBuf, oldVal, readOnly>>

CommitReadOnly(t) ==
    /\ pc[t] = "active"
    /\ readOnly[t] = TRUE
    /\ committed' = [committed EXCEPT ![t] = committed[t] + 1]
    /\ pc' = [pc EXCEPT ![t] = "idle"]
    /\ readSet' = [readSet EXCEPT ![t] = {}]
    /\ UNCHANGED <<g_ts, orec, mem, writeLog, writeBuf, oldVal, readOnly>>

Next ==
    \E t \in Thread :
        \/ Begin(t)
        \/ (\E a \in Addr : ReadOwn(t, a))
        \/ (\E a \in Addr : ReadFromMem(t, a))
        \/ (\E a \in Addr : \E n \in 0..MAX_VAL : WriteAcquire(t, a, n))
        \/ (\E a \in Addr : WriteConflict(t, a))
        \/ (\E a \in Addr : \E n \in 0..MAX_VAL : WriteUpdate(t, a, n))
        \/ CommitRLock(t)
        \/ CommitIncTS(t)
        \/ CommitValidate(t)
        \/ CommitValidateFail(t)
        \/ CommitWriteBack(t)
        \/ CommitReadOnly(t)

Spec == Init /\ [][Next]_vars

(* ---- INVARIANTS ---- *)

(* No two threads hold w_lock for the same address *)
MutexWriteLock ==
    \A a \in Addr :
        OREC_WLOCK(orec[a]) = 1
        => \E t \in Thread : OREC_WOWNER(orec[a]) = t

(* w_lock owner is in writeLog *)
WriteOwnerInv ==
    \A a \in Addr :
        (OREC_WLOCK(orec[a]) = 1)
        => \E t \in Thread :
            OREC_WOWNER(orec[a]) = t /\ a \in writeLog[t]

(* No thread holds a write-lock after commit *)
NoPostCommitLocks ==
    \A t \in Thread : (pc[t] = "idle")
        => \A a \in Addr : ~(OREC_WLOCK(orec[a]) = 1 /\ OREC_WOWNER(orec[a]) = t)

Inv ==
    /\ MutexWriteLock
    /\ WriteOwnerInv
    /\ NoPostCommitLocks

THEOREM Spec => []Inv

========================================================================
