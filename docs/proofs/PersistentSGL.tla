---------------------- MODULE PersistentSGL -----------------------
(*
 * PersistentSGL — SGL with NVM Dual-Write (PlusCal)
 *
 * Algorithm: Single global lock with persistent dual-write.
 * Every write updates both mem and nvm simultaneously (clwb + sfence
 * per write).  On recovery, the durable state is loaded from nvm.
 *
 * Invariants:
 *   LockExclusion: At most one thread holds the lock.
 *   LockHolderActive: Lock holder is in active state.
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
    or \* Write (dual-write: mem + nvm simultaneously)
        with a \in Addr, v \in Data do
            mem[a] := v;
            nvm[a] := v;
        end with;
        goto L_active;
    or \* Commit (release lock)
        lock := 0;
        version := version + 1;
        committed[self] := committed[self] + 1;
        state[self] := "idle";
        goto L_idle;
    end either;

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
    or \* Recover (nvm is always consistent)
        await crashed /\ ~recovered;
        mem := nvm;
        lock := 0;
        state := [t \in Thread |-> "idle"];
        recovered := TRUE;
        goto L_sys;
    end either;

end process;

end algorithm; *)
\* BEGIN TRANSLATION (chksum(pcal) = "a4102c63" /\ chksum(tla) = "07755a91")
VARIABLES lock, mem, nvm, state, version, committed, crashed, recovered, pc

vars == << lock, mem, nvm, state, version, committed, crashed, recovered, pc
        >>

ProcSet == (Thread) \cup {0}

Init == (* Global variables *)
        /\ lock = 0
        /\ mem = [a \in Addr |-> 0]
        /\ nvm = [a \in Addr |-> 0]
        /\ state = [t \in Thread |-> "idle"]
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
                           /\ pc' = [pc EXCEPT ![self] = "L_active"]
                      ELSE /\ IF committed[self] >= MaxCommits
                                 THEN /\ pc' = [pc EXCEPT ![self] = "L_done"]
                                 ELSE /\ pc' = [pc EXCEPT ![self] = "L_idle"]
                           /\ UNCHANGED << lock, state >>
                /\ UNCHANGED << mem, nvm, version, committed, crashed, 
                                recovered >>

L_active(self) == /\ pc[self] = "L_active"
                  /\ \/ /\ \E a \in Addr:
                             TRUE
                        /\ pc' = [pc EXCEPT ![self] = "L_active"]
                        /\ UNCHANGED <<lock, mem, nvm, state, version, committed>>
                     \/ /\ \E a \in Addr:
                             \E v \in Data:
                               /\ mem' = [mem EXCEPT ![a] = v]
                               /\ nvm' = [nvm EXCEPT ![a] = v]
                        /\ pc' = [pc EXCEPT ![self] = "L_active"]
                        /\ UNCHANGED <<lock, state, version, committed>>
                     \/ /\ lock' = 0
                        /\ version' = version + 1
                        /\ committed' = [committed EXCEPT ![self] = committed[self] + 1]
                        /\ state' = [state EXCEPT ![self] = "idle"]
                        /\ pc' = [pc EXCEPT ![self] = "L_idle"]
                        /\ UNCHANGED <<mem, nvm>>
                  /\ UNCHANGED << crashed, recovered >>

L_done(self) == /\ pc[self] = "L_done"
                /\ TRUE
                /\ pc' = [pc EXCEPT ![self] = "Done"]
                /\ UNCHANGED << lock, mem, nvm, state, version, committed, 
                                crashed, recovered >>

ThreadProc(self) == L_idle(self) \/ L_active(self) \/ L_done(self)

L_sys == /\ pc[0] = "L_sys"
         /\ \/ /\ ~crashed
               /\ crashed' = TRUE
               /\ pc' = [pc EXCEPT ![0] = "L_sys"]
               /\ UNCHANGED <<lock, mem, state, recovered>>
            \/ /\ crashed /\ ~recovered
               /\ mem' = nvm
               /\ lock' = 0
               /\ state' = [t \in Thread |-> "idle"]
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
(* NOTE: Tautology — (lock=t1 /\ lock=t2) => t1=t2 holds trivially. *)
(* Kept for documentation clarity; excluded from TLC-checked Inv.   *)
LockExclusion ==
    \A t1, t2 \in Thread :
        (lock = t1 /\ lock = t2) => t1 = t2

(*── I2: Lock holder is in active state ────────────────────────────*)
LockHolderActive ==
    \A t \in Thread :
        lock = t => state[t] = "active"

(*── I3: After crash and recovery, mem = nvm ──────────────────────*)
RecoveryConsistency ==
    recovered = TRUE => mem = nvm

(*── I4: NVM always equals mem for committed writes (dual-write) ──*)
NVMAgreesWithMem ==
    \A a \in Addr :
        mem[a] = nvm[a]

(*====================================================================*)
(* Fairness and Liveness                                              *)
(*====================================================================*)

(* Weak fairness on each thread process *)
Spec_WF == Spec /\ \A self \in Thread : WF_vars(ThreadProc(self))

(* Strong fairness on each thread process *)
Spec_SF == Spec /\ \A self \in Thread : SF_vars(ThreadProc(self))

(* Liveness: every active thread eventually becomes idle *)
ProgressProperty ==
    \A self \in Thread : (pc[self] = "L_active" ~> pc[self] \in {"L_idle", "L_done", "Done"})

=======================================================================
