---------------------- MODULE PersistentSGL -----------------------
(*
 * PersistentSGL — SGL with NVM Durability Barriers (PlusCal)
 *
 * Algorithm: Single global lock with durable commit.
 * After releasing the lock, the thread flushes written cache
 * lines to NVM (clwb + sfence).  On recovery, the durable
 * state is loaded from NVM.
 *
 * Invariants:
 *   LockExclusion: At most one thread holds the lock.
 *   LockHolderActive: Lock holder is in active or flushing state.
 *   RecoveryConsistency: After recovery, mem = nvm.
 *)

EXTENDS Naturals, FiniteSets, Sequences, TLC

CONSTANTS Thread, Addr, Data, MaxCommits
ASSUME Thread \subseteq Nat \ {0}
ASSUME Addr \subseteq Nat
ASSUME Data \subseteq Nat
ASSUME MaxCommits \in Nat

(* --algorithm PersistentSGL

variables
    lock = 0,
    mem = [a \in Addr |-> 0],
    nvm = [a \in Addr |-> 0],
    state = [t \in Thread |-> "idle"],
    durable_log = [t \in Thread |-> {}],
    version = 0,
    committed = [t \in Thread |-> 0],
    crashed = FALSE,
    recovered = FALSE;

process ThreadProc \in Thread
begin

L_idle:
    if committed[self] < MaxCommits /\ ~crashed /\ lock = 0 then
        lock := self;
        state[self] := "active";
        durable_log[self] := {};
        goto L_active;
    elsif committed[self] >= MaxCommits then
        goto L_done;
    else
        goto L_idle;
    end if;

L_active:
    either \* Read
        with a \in Addr do
            skip;
        end with;
        goto L_active;
    or \* Write (buffer in durable log)
        with a \in Addr, v \in Data do
            mem[a] := v;
            durable_log[self] := durable_log[self] \union {<<a, v>>};
        end with;
        goto L_active;
    or \* Flush to NVM
        nvm := [a \in Addr |->
            IF \E <<a2, v2>> \in durable_log[self] : a2 = a
            THEN
                LET WrittenValues ==
                    {entry[2] : entry \in {x \in durable_log[self] : x[1] = a}}
                IN CHOOSE v \in WrittenValues : TRUE
            ELSE nvm[a]];
        goto L_complete;
    end either;

L_complete:
    \* Release lock and commit
    lock := 0;
    version := version + 1;
    committed[self] := committed[self] + 1;
    durable_log[self] := {};
    state[self] := "idle";
    goto L_idle;

L_done:
    skip;

end process;

process System = 0
begin

L_sys:
    either \* Crash
        await ~crashed;
        crashed := TRUE;
        goto L_sys;
    or \* Recover
        await crashed /\ ~recovered;
        mem := nvm;
        lock := 0;
        state := [t \in Thread |-> "idle"];
        durable_log := [t \in Thread |-> {}];
        recovered := TRUE;
        goto L_sys;
    end either;

end process;

end algorithm; *)
\* BEGIN TRANSLATION (chksum(pcal) = "e03b7ccd" /\ chksum(tla) = "dec22ba9")
VARIABLES pc, lock, mem, nvm, state, durable_log, version, committed, crashed, 
          recovered

vars == << pc, lock, mem, nvm, state, durable_log, version, committed, 
           crashed, recovered >>

ProcSet == (Thread) \cup {0}

Init == (* Global variables *)
        /\ lock = 0
        /\ mem = [a \in Addr |-> 0]
        /\ nvm = [a \in Addr |-> 0]
        /\ state = [t \in Thread |-> "idle"]
        /\ durable_log = [t \in Thread |-> {}]
        /\ version = 0
        /\ committed = [t \in Thread |-> 0]
        /\ crashed = FALSE
        /\ recovered = FALSE
        /\ pc = [self \in ProcSet |-> CASE self \in Thread -> "L_idle"
                                        [] self = 0 -> "L_sys"]

L_idle(self) == /\ pc[self] = "L_idle"
                /\ IF committed[self] < MaxCommits /\ ~crashed /\ lock = 0
                      THEN /\ lock' = self
                           /\ state' = [state EXCEPT ![self] = "active"]
                           /\ durable_log' = [durable_log EXCEPT ![self] = {}]
                           /\ pc' = [pc EXCEPT ![self] = "L_active"]
                      ELSE /\ IF committed[self] >= MaxCommits
                                 THEN /\ pc' = [pc EXCEPT ![self] = "L_done"]
                                 ELSE /\ pc' = [pc EXCEPT ![self] = "L_idle"]
                           /\ UNCHANGED << lock, state, durable_log >>
                /\ UNCHANGED << mem, nvm, version, committed, crashed, 
                                recovered >>

L_active(self) == /\ pc[self] = "L_active"
                  /\ \/ /\ \E a \in Addr:
                             TRUE
                        /\ pc' = [pc EXCEPT ![self] = "L_active"]
                        /\ UNCHANGED <<mem, nvm, durable_log>>
                     \/ /\ \E a \in Addr:
                             \E v \in Data:
                               /\ mem' = [mem EXCEPT ![a] = v]
                               /\ durable_log' = [durable_log EXCEPT ![self] = durable_log[self] \union {<<a, v>>}]
                        /\ pc' = [pc EXCEPT ![self] = "L_active"]
                        /\ nvm' = nvm
                     \/ /\ nvm' =    [a \in Addr |->
                                  IF \E <<a2, v2>> \in durable_log[self] : a2 = a
                                  THEN
                                      LET WrittenValues ==
                                          {entry[2] : entry \in {x \in durable_log[self] : x[1] = a}}
                                      IN CHOOSE v \in WrittenValues : TRUE
                                  ELSE nvm[a]]
                        /\ pc' = [pc EXCEPT ![self] = "L_complete"]
                        /\ UNCHANGED <<mem, durable_log>>
                  /\ UNCHANGED << lock, state, version, committed, crashed, 
                                  recovered >>

L_complete(self) == /\ pc[self] = "L_complete"
                    /\ lock' = 0
                    /\ version' = version + 1
                    /\ committed' = [committed EXCEPT ![self] = committed[self] + 1]
                    /\ durable_log' = [durable_log EXCEPT ![self] = {}]
                    /\ state' = [state EXCEPT ![self] = "idle"]
                    /\ pc' = [pc EXCEPT ![self] = "L_idle"]
                    /\ UNCHANGED << mem, nvm, crashed, recovered >>

L_done(self) == /\ pc[self] = "L_done"
                /\ TRUE
                /\ pc' = [pc EXCEPT ![self] = "Done"]
                /\ UNCHANGED << lock, mem, nvm, state, durable_log, version, 
                                committed, crashed, recovered >>

ThreadProc(self) == L_idle(self) \/ L_active(self) \/ L_complete(self)
                       \/ L_done(self)

L_sys == /\ pc[0] = "L_sys"
         /\ \/ /\ ~crashed
               /\ crashed' = TRUE
               /\ pc' = [pc EXCEPT ![0] = "L_sys"]
               /\ UNCHANGED <<lock, mem, state, durable_log, recovered>>
            \/ /\ crashed /\ ~recovered
               /\ mem' = nvm
               /\ lock' = 0
               /\ state' = [t \in Thread |-> "idle"]
               /\ durable_log' = [t \in Thread |-> {}]
               /\ recovered' = TRUE
               /\ pc' = [pc EXCEPT ![0] = "L_sys"]
               /\ UNCHANGED crashed
         /\ UNCHANGED << nvm, version, committed >>

System == L_sys

(* Allow infinite stuttering to prevent deadlock on termination. *)
Terminating == /\ \A self \in ProcSet: pc[self] = "Done"
               /\ UNCHANGED vars

Next == System
           \/ (\E self \in Thread: ThreadProc(self))
           \/ Terminating

Spec == Init /\ [][Next]_vars

Termination == <>(\A self \in ProcSet: pc[self] = "Done")

\* END TRANSLATION

(*====================================================================*)
(* Bounds for model checking                                          *)
(*====================================================================*)
ModelBound ==
    /\ version < 4
    /\ \A t \in Thread : committed[t] <= MaxCommits

(*====================================================================*)
(* Invariants                                                         *)
(*====================================================================*)

(*── I1: Mutual exclusion on the SGL ───────────────────────────────*)
LockExclusion ==
    \A t1, t2 \in Thread :
        (lock = t1 /\ lock = t2) => t1 = t2

(*── I2: Lock holder is in active or flushing state ────────────────*)
LockHolderActive ==
    \A t \in Thread :
        lock = t => state[t] \in {"active", "flushing"}

(*── I3: After crash and recovery, mem = nvm ──────────────────────*)
RecoveryConsistency ==
    recovered = TRUE => mem = nvm

(*── I4: All durable writes in nvm are a superset of committed writes ─*)
NVMContainsCommitted ==
    \A a \in Addr :
        \/ nvm[a] = mem[a]
        \/ \E t \in Thread :
            \E <<a2, v>> \in durable_log[t] : a2 = a /\ nvm[a] = v

=======================================================================
