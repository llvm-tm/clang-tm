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
    recovered = FALSE,
    \* Dual-write intermediate state: thread has written mem but not yet nvm.
    \* A crash during this window leaves nvm with stale data (no fence between 
    \* *addr=val and memcpy in C++).  Recovery restores mem from nvm, losing
    \* the mem-only write.
    pending_nvm = [t \in Thread |-> FALSE],
    pending_addr = [t \in Thread |-> 0],
    pending_val = [t \in Thread |-> 0];

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
    await ~crashed;
    either \* Read (no-op in model)
        with a \in Addr do
            skip;
        end with;
        goto L_active;
    or \* Write mem (plain store — no fence before nvm write)
        with a \in Addr, v \in Data do
            mem[a] := v;
            pending_nvm[self] := TRUE;
            pending_addr[self] := a;
            pending_val[self] := v;
        end with;
        goto L_write_nvm;
    or \* Commit (release lock)
        lock := 0;
        version := version + 1;
        committed[self] := committed[self] + 1;
        state[self] := "idle";
        goto L_idle;
    end either;

L_write_nvm:
    \* Write nvm (plain memcpy to mmap — no fence after mem write)
    nvm[pending_addr[self]] := pending_val[self];
    pending_nvm[self] := FALSE;
    pending_addr[self] := 0;
    pending_val[self] := 0;
    goto L_active;

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
    or \* Recover (nvm may have stale data from partial dual-write)
        \* Reset all threads to idle, matching re-initialization in C++
        await crashed /\ ~recovered;
        mem := nvm;
        lock := 0;
        state := [t \in Thread |-> "idle"];
        recovered := TRUE;
        \* Clear any in-flight dual-write state
        pending_nvm := [t \in Thread |-> FALSE];
        pending_addr := [t \in Thread |-> 0];
        pending_val := [t \in Thread |-> 0];
        pc := [t \in ProcSet |-> IF t \in Thread THEN "L_idle" ELSE pc[t]];
        goto L_sys;
    end either;

end process;

end algorithm; *)
\* BEGIN TRANSLATION (chksum(pcal) = "b87e1f34" /\ chksum(tla) = "2a9c5e01")
VARIABLES lock, mem, nvm, state, version, committed, crashed, recovered,
          pending_nvm, pending_addr, pending_val, pc

vars == << lock, mem, nvm, state, version, committed, crashed, recovered,
           pending_nvm, pending_addr, pending_val, pc >>

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
        /\ pending_nvm = [t \in Thread |-> FALSE]
        /\ pending_addr = [t \in Thread |-> 0]
        /\ pending_val = [t \in Thread |-> 0]
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
                                recovered, pending_nvm, pending_addr,
                                pending_val >>

L_active(self) == /\ pc[self] = "L_active"
                  /\ ~crashed
                  /\ \/ /\ \E a \in Addr:
                             TRUE
                        /\ pc' = [pc EXCEPT ![self] = "L_active"]
                        /\ UNCHANGED <<lock, mem, nvm, state, version,
                                       committed, pending_nvm, pending_addr,
                                       pending_val>>
                     \/ \* Write mem (plain store, no fence before nvm)
                        /\ \E a \in Addr:
                              \E v \in Data:
                                /\ mem' = [mem EXCEPT ![a] = v]
                                /\ pending_nvm' = [pending_nvm EXCEPT ![self] = TRUE]
                                /\ pending_addr' = [pending_addr EXCEPT ![self] = a]
                                /\ pending_val' = [pending_val EXCEPT ![self] = v]
                        /\ pc' = [pc EXCEPT ![self] = "L_write_nvm"]
                        /\ UNCHANGED <<lock, nvm, state, version, committed>>
                     \/ \* Commit (release lock)
                        /\ lock' = 0
                        /\ version' = version + 1
                        /\ committed' = [committed EXCEPT ![self] = committed[self] + 1]
                        /\ state' = [state EXCEPT ![self] = "idle"]
                        /\ pc' = [pc EXCEPT ![self] = "L_idle"]
                        /\ UNCHANGED <<mem, nvm, pending_nvm, pending_addr,
                                       pending_val>>
                  /\ UNCHANGED << crashed, recovered >>

L_write_nvm(self) == /\ pc[self] = "L_write_nvm"
                     /\ nvm' = [nvm EXCEPT ![pending_addr[self]] = pending_val[self]]
                     /\ pending_nvm' = [pending_nvm EXCEPT ![self] = FALSE]
                     /\ pending_addr' = [pending_addr EXCEPT ![self] = 0]
                     /\ pending_val' = [pending_val EXCEPT ![self] = 0]
                     /\ pc' = [pc EXCEPT ![self] = "L_active"]
                     /\ UNCHANGED << lock, mem, state, version, committed,
                                     crashed, recovered >>

L_done(self) == /\ pc[self] = "L_done"
                /\ TRUE
                /\ pc' = [pc EXCEPT ![self] = "Done"]
                /\ UNCHANGED << lock, mem, nvm, state, version, committed, 
                                crashed, recovered, pending_nvm, pending_addr,
                                pending_val >>

ThreadProc(self) == L_idle(self) \/ L_active(self) \/ L_write_nvm(self)
                       \/ L_done(self)

L_sys == /\ pc[0] = "L_sys"
         /\ \/ /\ ~crashed
               /\ crashed' = TRUE
               /\ pc' = [pc EXCEPT ![0] = "L_sys"]
               /\ UNCHANGED <<lock, mem, state, recovered, pending_nvm,
                               pending_addr, pending_val>>
             \/ /\ crashed /\ ~recovered
                /\ mem' = nvm
                /\ lock' = 0
                /\ state' = [t \in Thread |-> "idle"]
                /\ recovered' = TRUE
                /\ pending_nvm' = [t \in Thread |-> FALSE]
                /\ pending_addr' = [t \in Thread |-> 0]
                /\ pending_val' = [t \in Thread |-> 0]
                /\ pc' = [t \in ProcSet |-> IF t \in Thread THEN "L_idle"
                                               ELSE pc[t]]
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

(*── I4: NVM equals mem when no dual-write is in-flight (non-crash) ──*)
(* The dual-write is split into L_active (mem write) and L_write_nvm (nvm   *)
(* write) with zero ordering. A crash between the two leaves nvm stale;     *)
(* recovery restores mem from nvm, losing the write.  This invariant checks *)
(* that mem and nvm are consistent when the system is not crashed and no    *)
(* thread is mid-dual-write.                                                *)
NVMAgreesWithMem ==
    ~crashed /\ (\A t \in Thread : ~pending_nvm[t])
        => (\A a \in Addr : mem[a] = nvm[a])

(*====================================================================*)
(* Fairness and Liveness                                              *)
(*====================================================================*)

(* Weak fairness on each thread process and the System process *)
(* System WF ensures recovery eventually happens after crash, so stuck *)
(* threads in L_active (blocked by ~crashed guard) can reach L_idle.   *)
Spec_WF == Spec
              /\ WF_vars(System)
              /\ \A self \in Thread : WF_vars(ThreadProc(self))

(* Strong fairness on each thread process *)
Spec_SF == Spec /\ \A self \in Thread : SF_vars(ThreadProc(self))

(* Liveness: every active thread eventually becomes idle *)
ProgressProperty ==
    \A self \in Thread : (pc[self] = "L_active" ~> pc[self] \in {"L_idle", "L_done", "Done"})

=======================================================================
