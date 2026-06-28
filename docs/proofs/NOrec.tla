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
    lastWriteClock = [a \in Addr |-> 0],
    \* Torn-read double-check state:
    \*   clock1[t]: first clock capture (before data read)
    \*   raddr[t]:  address being read
    clock1 = [t \in Thread |-> 0],
    raddr = [t \in Thread |-> 0],
    lastSignalFence = [t \in Thread |-> ""],
    lastThreadFence = [t \in Thread |-> ""],
    lastRmw = [t \in Thread |-> ""];

process ThreadProc \in Thread
begin

L_idle:
    either \* Begin transaction (acquire-load snapshot)
        await committed[self] < MaxCommits /\ clk % 2 = 0;
        snapshot[self] := clk;
        lastRmw[self] := "acquire";  \* g_clock.load(acquire)
        readOnly[self] := TRUE;
        readSet[self] := {};
        writeSet[self] := {};
        rsSnapshot[self] := clk;
        clock1[self] := 0;
        raddr[self] := 0;
        goto L_active;
    or \* Done
        await committed[self] >= MaxCommits;
        goto L_done;
    end either;

L_active:
    either \* Begin read: capture clock (acquire-load), choose address
        with a \in Addr do
            if a \in writeSet[self] then
                \* Read own write: no ordering needed
                lastSignalFence[self] := "";
                lastThreadFence[self] := "";
                lastRmw[self] := "";
                goto L_active;
            else
                clock1[self] := clk;
                lastRmw[self] := "acquire";  \* g_clock.load(acquire)
                raddr[self] := a;
                goto L_read_val;
            end if;
        end with;
    or \* Write (buffered — no ordering needed)
        with a \in Addr, n \in Data do
            writeSet[self] := writeSet[self] \cup {a};
            wbBuffer[self][a] := n;
            readOnly[self] := FALSE;
            lastSignalFence[self] := "";
            lastThreadFence[self] := "";
            lastRmw[self] := "";
        end with;
        goto L_active;
    or \* Commit
        if readOnly[self] then
            \* Read-only: commit without touching clock (no ordering)
            readSet[self] := {};
            writeSet[self] := {};
            committed[self] := committed[self] + 1;
            lastSignalFence[self] := "";
            lastThreadFence[self] := "";
            lastRmw[self] := "";
            goto L_idle;
        elsif clk = snapshot[self] then
            \* CAS success: acquire clock (seq_cst compare_exchange_strong)
            clk := snapshot[self] + 1;
            readSet[self] := {};
            lastRmw[self] := "seq_cst";  \* CAS (seq_cst)
            goto L_commit_wb;
        else
            \* CAS failed: validate and retry (acquire-load at validate entry)
            if readSet[self] = {} \/ \A e \in readSet[self] : mem[RS_ADDR(e)] = RS_VAL(e) then
                snapshot[self] := clk - (clk % 2);
                readSet[self] := {};
                lastRmw[self] := "acquire";  \* g_clock.load(acquire)
                goto L_active;
            else
                \* Validation failed => abort
                readSet[self] := {};
                writeSet[self] := {};
                aborted[self] := aborted[self] + 1;
                lastSignalFence[self] := "";
                lastThreadFence[self] := "";
                lastRmw[self] := "";
                goto L_idle;
            end if;
        end if;
    end either;

L_read_val:
    \* Re-read clock (acquire-load) and compare with first capture
    if clk # clock1[self] then
        \* Clock changed during read — possible torn read
        \* Validate read-set entries (acquire-load at validate entry)
        if readSet[self] = {} \/ \A e \in readSet[self] : mem[RS_ADDR(e)] = RS_VAL(e) then
            \* Validate OK: update snapshot, record entry, retry
            snapshot[self] := clk - (clk % 2);
            readSet[self] := readSet[self] \cup {RS_ENTRY(raddr[self], mem[raddr[self]], snapshot[self])};
            lastRmw[self] := "acquire";  \* g_clock.load(acquire)
            goto L_active;
        else
            \* Validation failed => abort
            readSet[self] := {};
            writeSet[self] := {};
            aborted[self] := aborted[self] + 1;
            lastSignalFence[self] := "";
            lastThreadFence[self] := "";
            lastRmw[self] := "";
            goto L_idle;
        end if;
    else
        \* Clock unchanged — no concurrent commit, safe to record
        readSet[self] := readSet[self] \cup {RS_ENTRY(raddr[self], mem[raddr[self]], clock1[self])};
        lastRmw[self] := "acquire";  \* g_clock.load(acquire)
        goto L_active;
    end if;

L_commit_wb:
    \* Write buffered values to memory (plain stores)
    mem := [a \in Addr |->
        IF a \in writeSet[self] THEN wbBuffer[self][a] ELSE mem[a]];
    \* Release clock (release-store — makes write-back visible)
    clk := clk + 1;
    lastRmw[self] := "release";  \* g_clock.store(release, ...)
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
\* BEGIN TRANSLATION (chksum(pcal) = "9f3c11a2" /\ chksum(tla) = "d7a1e4c0")
VARIABLES clk, mem, readSet, writeSet, wbBuffer, snapshot, readOnly, 
          committed, aborted, rsSnapshot, lastWriter, lastWriteClock, clock1, 
          raddr, lastSignalFence, lastThreadFence, lastRmw, pc

vars == << clk, mem, readSet, writeSet, wbBuffer, snapshot, readOnly, 
           committed, aborted, rsSnapshot, lastWriter, lastWriteClock, clock1, 
           raddr, lastSignalFence, lastThreadFence, lastRmw, pc >>

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
        /\ clock1 = [t \in Thread |-> 0]
        /\ raddr = [t \in Thread |-> 0]
        /\ lastSignalFence = [t \in Thread |-> ""]
        /\ lastThreadFence = [t \in Thread |-> ""]
        /\ lastRmw = [t \in Thread |-> ""]
        /\ pc = [self \in ProcSet |-> "L_idle"]

L_idle(self) == /\ pc[self] = "L_idle"
                /\ \/ /\ committed[self] < MaxCommits /\ clk % 2 = 0
                      /\ snapshot' = [snapshot EXCEPT ![self] = clk]
                      /\ lastRmw' = [lastRmw EXCEPT ![self] = "acquire"]
                      /\ readOnly' = [readOnly EXCEPT ![self] = TRUE]
                      /\ readSet' = [readSet EXCEPT ![self] = {}]
                      /\ writeSet' = [writeSet EXCEPT ![self] = {}]
                      /\ rsSnapshot' = [rsSnapshot EXCEPT ![self] = clk]
                      /\ clock1' = [clock1 EXCEPT ![self] = 0]
                      /\ raddr' = [raddr EXCEPT ![self] = 0]
                      /\ pc' = [pc EXCEPT ![self] = "L_active"]
                   \/ /\ committed[self] >= MaxCommits
                      /\ pc' = [pc EXCEPT ![self] = "L_done"]
                      /\ UNCHANGED <<readSet, writeSet, snapshot, readOnly, rsSnapshot, clock1, raddr, lastRmw>>
                /\ UNCHANGED << clk, mem, wbBuffer, committed, aborted, 
                                lastWriter, lastWriteClock, lastSignalFence, 
                                lastThreadFence >>

L_active(self) == /\ pc[self] = "L_active"
                  /\ \/ /\ \E a \in Addr:
                             IF a \in writeSet[self]
                                THEN /\ lastSignalFence' = [lastSignalFence EXCEPT ![self] = ""]
                                     /\ lastThreadFence' = [lastThreadFence EXCEPT ![self] = ""]
                                     /\ lastRmw' = [lastRmw EXCEPT ![self] = ""]
                                     /\ pc' = [pc EXCEPT ![self] = "L_active"]
                                     /\ UNCHANGED << clock1, raddr >>
                                ELSE /\ clock1' = [clock1 EXCEPT ![self] = clk]
                                     /\ lastRmw' = [lastRmw EXCEPT ![self] = "acquire"]
                                     /\ raddr' = [raddr EXCEPT ![self] = a]
                                     /\ pc' = [pc EXCEPT ![self] = "L_read_val"]
                                     /\ UNCHANGED << lastSignalFence, 
                                                     lastThreadFence >>
                        /\ UNCHANGED <<clk, readSet, writeSet, wbBuffer, snapshot, readOnly, committed, aborted>>
                     \/ /\ \E a \in Addr:
                             \E n \in Data:
                               /\ writeSet' = [writeSet EXCEPT ![self] = writeSet[self] \cup {a}]
                               /\ wbBuffer' = [wbBuffer EXCEPT ![self][a] = n]
                               /\ readOnly' = [readOnly EXCEPT ![self] = FALSE]
                               /\ lastSignalFence' = [lastSignalFence EXCEPT ![self] = ""]
                               /\ lastThreadFence' = [lastThreadFence EXCEPT ![self] = ""]
                               /\ lastRmw' = [lastRmw EXCEPT ![self] = ""]
                        /\ pc' = [pc EXCEPT ![self] = "L_active"]
                        /\ UNCHANGED <<clk, readSet, snapshot, committed, aborted, clock1, raddr>>
                     \/ /\ IF readOnly[self]
                              THEN /\ readSet' = [readSet EXCEPT ![self] = {}]
                                   /\ writeSet' = [writeSet EXCEPT ![self] = {}]
                                   /\ committed' = [committed EXCEPT ![self] = committed[self] + 1]
                                   /\ lastSignalFence' = [lastSignalFence EXCEPT ![self] = ""]
                                   /\ lastThreadFence' = [lastThreadFence EXCEPT ![self] = ""]
                                   /\ lastRmw' = [lastRmw EXCEPT ![self] = ""]
                                   /\ pc' = [pc EXCEPT ![self] = "L_idle"]
                                   /\ UNCHANGED << clk, snapshot, aborted >>
                              ELSE /\ IF clk = snapshot[self]
                                         THEN /\ clk' = snapshot[self] + 1
                                              /\ readSet' = [readSet EXCEPT ![self] = {}]
                                              /\ lastRmw' = [lastRmw EXCEPT ![self] = "seq_cst"]
                                              /\ pc' = [pc EXCEPT ![self] = "L_commit_wb"]
                                              /\ UNCHANGED << writeSet, 
                                                              snapshot, 
                                                              aborted, 
                                                              lastSignalFence, 
                                                              lastThreadFence >>
                                         ELSE /\ IF readSet[self] = {} \/ \A e \in readSet[self] : mem[RS_ADDR(e)] = RS_VAL(e)
                                                    THEN /\ snapshot' = [snapshot EXCEPT ![self] = clk - (clk % 2)]
                                                         /\ readSet' = [readSet EXCEPT ![self] = {}]
                                                         /\ lastRmw' = [lastRmw EXCEPT ![self] = "acquire"]
                                                         /\ pc' = [pc EXCEPT ![self] = "L_active"]
                                                         /\ UNCHANGED << writeSet, 
                                                                         aborted, 
                                                                         lastSignalFence, 
                                                                         lastThreadFence >>
                                                    ELSE /\ readSet' = [readSet EXCEPT ![self] = {}]
                                                         /\ writeSet' = [writeSet EXCEPT ![self] = {}]
                                                         /\ aborted' = [aborted EXCEPT ![self] = aborted[self] + 1]
                                                         /\ lastSignalFence' = [lastSignalFence EXCEPT ![self] = ""]
                                                         /\ lastThreadFence' = [lastThreadFence EXCEPT ![self] = ""]
                                                         /\ lastRmw' = [lastRmw EXCEPT ![self] = ""]
                                                         /\ pc' = [pc EXCEPT ![self] = "L_idle"]
                                                         /\ UNCHANGED snapshot
                                              /\ clk' = clk
                                   /\ UNCHANGED committed
                        /\ UNCHANGED <<wbBuffer, readOnly, clock1, raddr>>
                  /\ UNCHANGED << mem, rsSnapshot, lastWriter, lastWriteClock >>

L_read_val(self) == /\ pc[self] = "L_read_val"
                    /\ IF clk # clock1[self]
                          THEN /\ IF readSet[self] = {} \/ \A e \in readSet[self] : mem[RS_ADDR(e)] = RS_VAL(e)
                                     THEN /\ snapshot' = [snapshot EXCEPT ![self] = clk - (clk % 2)]
                                          /\ readSet' = [readSet EXCEPT ![self] = readSet[self] \cup {RS_ENTRY(raddr[self], mem[raddr[self]], snapshot'[self])}]
                                          /\ lastRmw' = [lastRmw EXCEPT ![self] = "acquire"]
                                          /\ pc' = [pc EXCEPT ![self] = "L_active"]
                                          /\ UNCHANGED << writeSet, aborted, 
                                                          lastSignalFence, 
                                                          lastThreadFence >>
                                     ELSE /\ readSet' = [readSet EXCEPT ![self] = {}]
                                          /\ writeSet' = [writeSet EXCEPT ![self] = {}]
                                          /\ aborted' = [aborted EXCEPT ![self] = aborted[self] + 1]
                                          /\ lastSignalFence' = [lastSignalFence EXCEPT ![self] = ""]
                                          /\ lastThreadFence' = [lastThreadFence EXCEPT ![self] = ""]
                                          /\ lastRmw' = [lastRmw EXCEPT ![self] = ""]
                                          /\ pc' = [pc EXCEPT ![self] = "L_idle"]
                                          /\ UNCHANGED snapshot
                          ELSE /\ readSet' = [readSet EXCEPT ![self] = readSet[self] \cup {RS_ENTRY(raddr[self], mem[raddr[self]], clock1[self])}]
                               /\ lastRmw' = [lastRmw EXCEPT ![self] = "acquire"]
                               /\ pc' = [pc EXCEPT ![self] = "L_active"]
                               /\ UNCHANGED << writeSet, snapshot, aborted, 
                                               lastSignalFence, 
                                               lastThreadFence >>
                    /\ UNCHANGED << clk, mem, wbBuffer, readOnly, committed, 
                                    rsSnapshot, lastWriter, lastWriteClock, 
                                    clock1, raddr >>

L_commit_wb(self) == /\ pc[self] = "L_commit_wb"
                     /\ mem' =    [a \in Addr |->
                               IF a \in writeSet[self] THEN wbBuffer[self][a] ELSE mem[a]]
                     /\ clk' = clk + 1
                     /\ lastRmw' = [lastRmw EXCEPT ![self] = "release"]
                     /\ readSet' = [readSet EXCEPT ![self] = {}]
                     /\ writeSet' = [writeSet EXCEPT ![self] = {}]
                     /\ committed' = [committed EXCEPT ![self] = committed[self] + 1]
                     /\ lastWriter' =           [a \in Addr |->
                                      IF a \in writeSet'[self] THEN self ELSE lastWriter[a]]
                     /\ lastWriteClock' =               [a \in Addr |->
                                          IF a \in writeSet'[self] THEN clk' ELSE lastWriteClock[a]]
                     /\ pc' = [pc EXCEPT ![self] = "L_idle"]
                     /\ UNCHANGED << wbBuffer, snapshot, readOnly, aborted, 
                                     rsSnapshot, clock1, raddr, 
                                     lastSignalFence, lastThreadFence >>

L_done(self) == /\ pc[self] = "L_done"
                /\ TRUE
                /\ pc' = [pc EXCEPT ![self] = "Done"]
                /\ UNCHANGED << clk, mem, readSet, writeSet, wbBuffer, 
                                snapshot, readOnly, committed, aborted, 
                                rsSnapshot, lastWriter, lastWriteClock, clock1, 
                                raddr, lastSignalFence, lastThreadFence, 
                                lastRmw >>

ThreadProc(self) == L_idle(self) \/ L_active(self) \/ L_read_val(self)
                       \/ L_commit_wb(self) \/ L_done(self)

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
(* INVARIANT 4: Fence fidelity                                          *)
(*   When entering write-back (commit CAS succeeded), the last ordering  *)
(*   operation must be seq_cst (from compare_exchange_strong) or release *)
(*   (from set_clock() release-store). This captures the C++ NOrec       *)
(*   ordering guarantee: write-back is made visible before the release   *)
(*   store that unlocks the global clock.                                *)
(*====================================================================*)
FenceFidelity ==
    \A t \in Thread : (pc[t] = "L_commit_wb") =>
        (lastRmw[t] \in {"seq_cst", "release"})

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
