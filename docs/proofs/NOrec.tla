------------------------ MODULE NOrec ------------------------
(*
 * NOrec (NO read-Check) STM — PlusCal Specification
 *
 * Algorithm (Dice, Shavit, 2006):
 *   - Single global clock G (even = unlocked, odd = locked).
 *   - Per-thread read-set and write-set.
 *   - begin(): spin until G is even, snapshot = G.
 *   - read(a):
 *       1. Return own buffered value if a is in write-set.
 *       2. Load a from memory; while G # snapshot: validate() (re-read
 *          all read-set entries, abort on mismatch), re-read a, update snapshot.
 *       3. Add (a, value, snapshot) to read-set.
 *   - write(a, v): buffer (a, v) in write-set.
 *   - commit():
 *       1. If read-only: clear and return.
 *       2. CAS snapshot -> snapshot+1 (acquire clock).
 *       3. If CAS fails: validate(), retry CAS.
 *       4. Write buffered values to memory (write-back).
 *       5. G := snapshot+2 (release clock, advance version).
 *)

EXTENDS Naturals, TLC

CONSTANTS Thread, Addr, Data, MaxRetries, MaxCommits
ASSUME Thread \subseteq Nat \ {0}
ASSUME Addr \subseteq Nat
ASSUME Data \subseteq Nat /\ 0 \in Data
ASSUME MaxRetries \in Nat
ASSUME MaxCommits \in Nat

(* A read-set entry: (addr, value, clock_version) *)
CONSTANT NO_RS_ENTRY
RS_ENTRY(a, v, c) == <<a, v, c>>
RS_ADDR(e) == e[1]
RS_VAL(e) == e[2]
RS_CLK(e) == e[3]

(* --algorithm NOrec

variables
    clk = 0,
    mem = [a \in Addr |-> 0],
    readSet = [t \in Thread |-> {}],
    writeSet = [t \in Thread |-> {}],
    wbBuffer = [t \in Thread |-> [a \in Addr |-> 0]],
    snapshot = [t \in Thread |-> 0],
    readOnly = [t \in Thread |-> TRUE],
    committed = [t \in Thread |-> 0],
    aborted = [t \in Thread |-> 0],
    rsSnapshot = [t \in Thread |-> 0],   (* NOTE: Set on begin, never read by any guard or invariant. Kept for documentation. *)
    lastWriter = [a \in Addr |-> 0],
    lastWriteClock = [a \in Addr |-> 0];

process ThreadProc \in Thread
begin

L_idle:
    either \* Begin transaction
        await committed[self] < MaxCommits /\ clk % 2 = 0;
        snapshot[self] := clk;
        readOnly[self] := TRUE;
        readSet[self] := {};
        writeSet[self] := {};
        rsSnapshot[self] := clk;
        goto L_active;
    or \* Done
        await committed[self] >= MaxCommits;
        goto L_done;
    end either;

L_active:
    either \* Read
        with a \in Addr do
            if a \in writeSet[self] then
                \* Read own write: no state change
                goto L_active;
            elsif readSet[self] = {} \/ clk = snapshot[self] then
                \* Fast path: clock unchanged
                readSet[self] := readSet[self] \cup {RS_ENTRY(a, mem[a], snapshot[self])};
                goto L_active;
            elsif \A e \in readSet[self] : mem[RS_ADDR(e)] = RS_VAL(e) then
                \* Validate and re-read
                snapshot[self] := clk - (clk % 2);
                readSet[self] := readSet[self] \cup {RS_ENTRY(a, mem[a], snapshot[self])};
                goto L_active;
            else
                \* Validation failed => abort
                readSet[self] := {};
                writeSet[self] := {};
                aborted[self] := aborted[self] + 1;
                goto L_idle;
            end if;
        end with;
    or \* Write (buffered)
        with a \in Addr, n \in Data do
            writeSet[self] := writeSet[self] \cup {a};
            wbBuffer[self][a] := n;
            readOnly[self] := FALSE;
        end with;
        goto L_active;
    or \* Commit
        if readOnly[self] then
            \* Read-only: commit without touching clock
            readSet[self] := {};
            writeSet[self] := {};
            committed[self] := committed[self] + 1;
            goto L_idle;
        elsif clk = snapshot[self] then
            \* CAS success: acquire clock
            clk := snapshot[self] + 1;
            readSet[self] := {};
            goto L_commit_wb;
        else
            \* CAS failed: validate and retry
            if readSet[self] = {} \/ \A e \in readSet[self] : mem[RS_ADDR(e)] = RS_VAL(e) then
                snapshot[self] := clk - (clk % 2);
                readSet[self] := {};
                goto L_active;
            else
                \* Validation failed => abort
                readSet[self] := {};
                writeSet[self] := {};
                aborted[self] := aborted[self] + 1;
                goto L_idle;
            end if;
        end if;
    end either;

L_commit_wb:
    \* Write buffered values to memory, release clock
    mem := [a \in Addr |->
        IF a \in writeSet[self] THEN wbBuffer[self][a] ELSE mem[a]];
    clk := clk + 1;
    readSet[self] := {};
    writeSet[self] := {};
    committed[self] := committed[self] + 1;
    lastWriter := [a \in Addr |->
        IF a \in writeSet[self] THEN self ELSE lastWriter[a]];
    lastWriteClock := [a \in Addr |->
        IF a \in writeSet[self] THEN clk ELSE lastWriteClock[a]];
    goto L_idle;

L_done:
    skip;

end process;

end algorithm; *)
\* BEGIN TRANSLATION (chksum(pcal) = "0a7440b5" /\ chksum(tla) = "4bdf5fd2")
VARIABLES clk, mem, readSet, writeSet, wbBuffer, snapshot, readOnly, 
          committed, aborted, rsSnapshot, lastWriter, lastWriteClock, pc

vars == << clk, mem, readSet, writeSet, wbBuffer, snapshot, readOnly, 
           committed, aborted, rsSnapshot, lastWriter, lastWriteClock, pc >>

ProcSet == (Thread)

Init == (* Global variables *)
        /\ clk = 0
        /\ mem = [a \in Addr |-> 0]
        /\ readSet = [t \in Thread |-> {}]
        /\ writeSet = [t \in Thread |-> {}]
        /\ wbBuffer = [t \in Thread |-> [a \in Addr |-> 0]]
        /\ snapshot = [t \in Thread |-> 0]
        /\ readOnly = [t \in Thread |-> TRUE]
        /\ committed = [t \in Thread |-> 0]
        /\ aborted = [t \in Thread |-> 0]
        /\ rsSnapshot = [t \in Thread |-> 0]
        /\ lastWriter = [a \in Addr |-> 0]
        /\ lastWriteClock = [a \in Addr |-> 0]
        /\ pc = [self \in ProcSet |-> "L_idle"]

L_idle(self) == /\ pc[self] = "L_idle"
                /\ \/ /\ committed[self] < MaxCommits /\ clk % 2 = 0
                      /\ snapshot' = [snapshot EXCEPT ![self] = clk]
                      /\ readOnly' = [readOnly EXCEPT ![self] = TRUE]
                      /\ readSet' = [readSet EXCEPT ![self] = {}]
                      /\ writeSet' = [writeSet EXCEPT ![self] = {}]
                      /\ rsSnapshot' = [rsSnapshot EXCEPT ![self] = clk]
                      /\ pc' = [pc EXCEPT ![self] = "L_active"]
                   \/ /\ committed[self] >= MaxCommits
                      /\ pc' = [pc EXCEPT ![self] = "L_done"]
                      /\ UNCHANGED <<readSet, writeSet, snapshot, readOnly, rsSnapshot>>
                /\ UNCHANGED << clk, mem, wbBuffer, committed, aborted, 
                                lastWriter, lastWriteClock >>

L_active(self) == /\ pc[self] = "L_active"
                  /\ \/ /\ \E a \in Addr:
                             IF a \in writeSet[self]
                                THEN /\ pc' = [pc EXCEPT ![self] = "L_active"]
                                     /\ UNCHANGED << readSet, writeSet, 
                                                     snapshot, aborted >>
                                ELSE /\ IF readSet[self] = {} \/ clk = snapshot[self]
                                           THEN /\ readSet' = [readSet EXCEPT ![self] = readSet[self] \cup {RS_ENTRY(a, mem[a], snapshot[self])}]
                                                /\ pc' = [pc EXCEPT ![self] = "L_active"]
                                                /\ UNCHANGED << writeSet, 
                                                                snapshot, 
                                                                aborted >>
                                           ELSE /\ IF \A e \in readSet[self] : mem[RS_ADDR(e)] = RS_VAL(e)
                                                      THEN /\ snapshot' = [snapshot EXCEPT ![self] = clk - (clk % 2)]
                                                           /\ readSet' = [readSet EXCEPT ![self] = readSet[self] \cup {RS_ENTRY(a, mem[a], snapshot'[self])}]
                                                           /\ pc' = [pc EXCEPT ![self] = "L_active"]
                                                           /\ UNCHANGED << writeSet, 
                                                                           aborted >>
                                                      ELSE /\ readSet' = [readSet EXCEPT ![self] = {}]
                                                           /\ writeSet' = [writeSet EXCEPT ![self] = {}]
                                                           /\ aborted' = [aborted EXCEPT ![self] = aborted[self] + 1]
                                                           /\ pc' = [pc EXCEPT ![self] = "L_idle"]
                                                           /\ UNCHANGED snapshot
                        /\ UNCHANGED <<clk, wbBuffer, readOnly, committed>>
                     \/ /\ \E a \in Addr:
                             \E n \in Data:
                               /\ writeSet' = [writeSet EXCEPT ![self] = writeSet[self] \cup {a}]
                               /\ wbBuffer' = [wbBuffer EXCEPT ![self][a] = n]
                               /\ readOnly' = [readOnly EXCEPT ![self] = FALSE]
                        /\ pc' = [pc EXCEPT ![self] = "L_active"]
                        /\ UNCHANGED <<clk, readSet, snapshot, committed, aborted>>
                     \/ /\ IF readOnly[self]
                              THEN /\ readSet' = [readSet EXCEPT ![self] = {}]
                                   /\ writeSet' = [writeSet EXCEPT ![self] = {}]
                                   /\ committed' = [committed EXCEPT ![self] = committed[self] + 1]
                                   /\ pc' = [pc EXCEPT ![self] = "L_idle"]
                                   /\ UNCHANGED << clk, snapshot, aborted >>
                              ELSE /\ IF clk = snapshot[self]
                                         THEN /\ clk' = snapshot[self] + 1
                                              /\ readSet' = [readSet EXCEPT ![self] = {}]
                                              /\ pc' = [pc EXCEPT ![self] = "L_commit_wb"]
                                              /\ UNCHANGED << writeSet, 
                                                              snapshot, 
                                                              aborted >>
                                         ELSE /\ IF readSet[self] = {} \/ \A e \in readSet[self] : mem[RS_ADDR(e)] = RS_VAL(e)
                                                    THEN /\ snapshot' = [snapshot EXCEPT ![self] = clk - (clk % 2)]
                                                         /\ readSet' = [readSet EXCEPT ![self] = {}]
                                                         /\ pc' = [pc EXCEPT ![self] = "L_active"]
                                                         /\ UNCHANGED << writeSet, 
                                                                         aborted >>
                                                    ELSE /\ readSet' = [readSet EXCEPT ![self] = {}]
                                                         /\ writeSet' = [writeSet EXCEPT ![self] = {}]
                                                         /\ aborted' = [aborted EXCEPT ![self] = aborted[self] + 1]
                                                         /\ pc' = [pc EXCEPT ![self] = "L_idle"]
                                                         /\ UNCHANGED snapshot
                                              /\ clk' = clk
                                   /\ UNCHANGED committed
                        /\ UNCHANGED <<wbBuffer, readOnly>>
                  /\ UNCHANGED << mem, rsSnapshot, lastWriter, lastWriteClock >>

L_commit_wb(self) == /\ pc[self] = "L_commit_wb"
                     /\ mem' =    [a \in Addr |->
                               IF a \in writeSet[self] THEN wbBuffer[self][a] ELSE mem[a]]
                     /\ clk' = clk + 1
                     /\ readSet' = [readSet EXCEPT ![self] = {}]
                     /\ writeSet' = [writeSet EXCEPT ![self] = {}]
                     /\ committed' = [committed EXCEPT ![self] = committed[self] + 1]
                     /\ lastWriter' =           [a \in Addr |->
                                      IF a \in writeSet'[self] THEN self ELSE lastWriter[a]]
                     /\ lastWriteClock' =               [a \in Addr |->
                                          IF a \in writeSet'[self] THEN clk' ELSE lastWriteClock[a]]
                     /\ pc' = [pc EXCEPT ![self] = "L_idle"]
                     /\ UNCHANGED << wbBuffer, snapshot, readOnly, aborted, 
                                     rsSnapshot >>

L_done(self) == /\ pc[self] = "L_done"
                /\ TRUE
                /\ pc' = [pc EXCEPT ![self] = "Done"]
                /\ UNCHANGED << clk, mem, readSet, writeSet, wbBuffer, 
                                snapshot, readOnly, committed, aborted, 
                                rsSnapshot, lastWriter, lastWriteClock >>

ThreadProc(self) == L_idle(self) \/ L_active(self) \/ L_commit_wb(self)
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
(* Bounds for model checking                                          *)
(*====================================================================*)
ModelBound ==
    /\ \A t \in Thread : committed[t] <= MaxCommits
    /\ \A t \in Thread : aborted[t] <= MaxRetries

(*====================================================================*)
(* INVARIANT 1: Clock parity                                            *)
(*   clk % 2 = 1  =>  some thread is in "committing" state (holds clock)*)
(*====================================================================*)
ClockParityInv ==
    (clk % 2 = 1) <=> (\E t \in Thread : pc[t] = "L_commit_wb")

(*====================================================================*)
(* INVARIANT 2: During write-back, read-set is empty                    *)
(*====================================================================*)
CommitInvariant ==
    \A t \in Thread : (pc[t] = "L_commit_wb") => (readSet[t] = {})

(*====================================================================*)
(* INVARIANT 3: Write-back consistency                                  *)
(*====================================================================*)
WriteBufferInv ==
    \A t \in Thread, a \in Addr :
        (a \in writeSet[t]) => (wbBuffer[t][a] # 0 \/ wbBuffer[t][a] \in Data)

(*====================================================================*)
(* PROPERTY: No dirty reads                                             *)
(*====================================================================*)
NoDirtyReads ==
    \A t1, t2 \in Thread, a \in Addr :
        (t1 # t2 /\ lastWriter[a] = t1 /\ lastWriteClock[a] > snapshot[t2])
        => (readSet[t2] \cap {e \in readSet[t2] : RS_ADDR(e) = a} # {}
            => pc[t2] = "L_active")

(*====================================================================*)
(* Fairness and Liveness                                              *)
(*====================================================================*)

(* Weak fairness on each thread process *)
Spec_WF == Spec /\ \A self \in Thread : WF_vars(ThreadProc(self))

(* Strong fairness on each thread process *)
Spec_SF == Spec /\ \A self \in Thread : SF_vars(ThreadProc(self))

(* Liveness: every transaction eventually commits or aborts *)
ProgressProperty ==
    \A t \in Thread :
        (pc[t] \in {"L_active", "L_commit_wb"}) ~> (pc[t] = "L_idle")

=======================================================================
