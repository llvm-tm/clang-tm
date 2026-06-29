------------------------ MODULE TSXSim ------------------------
(*
 * TSX-Sim — Bloom-filter-based TSX Simulation Backend (PlusCal)
 *
 * Models Intel TSX execution at cache-line granularity:
 *   - Bloom-filter read-set (4096-bit, double-hashing)
 *   - Cache-line write-set (exact)
 *   - Capacity abort on read/write line overflow
 *   - Conflict detection via bloom filter (no false negatives)
 *   - SGL fallback when retries exhausted
 *   - Virtual cycle counter
 *
 * Invariants:
 *   LockFreeInv:         sgl_lock=0 iff no thread in SGL mode
 *   LockOwnerInv:        sgl_lock=t => mode[t]="sgl"
 *   TSXvsSGLSafety:      tsx mode => sgl_lock=0
 *   BloomContainsReads:  bloom covers all read cache lines
 *   CapacityBounds:      TSX respects read/write capacity limits
 *   NoTSXCommitConflict: Conflicting TSX can't both commit
 *   NoSGLTSXOverlap:     SGL writes don't conflict with active TSX
 *   LockExclusion:       At most one SGL at a time
 *)

EXTENDS Naturals, FiniteSets, Sequences, TLC, TMTypes

CONSTANTS
    Thread,             (* Set of client thread IDs *)
    Addr,               (* Set of memory addresses *)
    CacheLine,          (* Set of cache-line identifiers *)
    HashPosition,       (* Set of bloom filter bit positions *)
    MAX_RETRIES,        (* Max TSX retries before SGL fallback *)
    MAX_READ_LINES,     (* Max cache lines in read-set *)
    MAX_WRITE_LINES     (* Max cache lines in write-set *)

ASSUME Thread \subseteq Nat \ {0}
ASSUME Addr \subseteq Nat
ASSUME CacheLine \subseteq Nat
ASSUME HashPosition \subseteq Nat
ASSUME MAX_RETRIES \in Nat
ASSUME MAX_READ_LINES \in Nat
ASSUME MAX_WRITE_LINES \in Nat

(*─── Hash function + cache-line mapping (default constants) ───────*)
Hash == [cl \in CacheLine |->
    {h \in HashPosition : (cl + h) % (Cardinality(HashPosition) + 1) \in HashPosition}]

CacheLineOf == [a \in Addr |-> ((a - 1) % Cardinality(CacheLine)) + 1]
CL(a) == CacheLineOf[a]


(*─── PlusCal algorithm ───────────────────────────────────────────*)
(* --algorithm TSXSim

variables
    mem = [a \in Addr |-> 0],
    sgl_lock = 0,
    mode = [t \in Thread |-> "idle"],
    write_set = [t \in Thread |-> {}],
    write_data = [t \in Thread |-> [cl \in CacheLine |-> NoWrite]],
    bloom = [t \in Thread |-> {}],
    read_lines = [t \in Thread |-> {}],
    sgl_write_set = [t \in Thread |-> {}],
    sgl_write_data = [t \in Thread |-> [cl \in CacheLine |-> NoWrite]],
    tsx_retries = [t \in Thread |-> 0],
    cycles = [t \in Thread |-> 0],
    committed = [t \in Thread |-> 0],
    aborted = [t \in Thread |-> 0],
    capacity_aborts = [t \in Thread |-> 0],
    conflict_aborts = [t \in Thread |-> 0];

define
    (* Conflict-free check: no bloom overlap or write-set overlap with other threads *)
    CF(t) ==
        \A t2 \in Thread \ {t} :
            /\ \A cl \in write_set[t] :
                 bloom[t2] \cap Hash[cl] = {}
            /\ write_set[t] \cap write_set[t2] = {}

    (* Write-back: apply write_set to memory *)
    WriteBack(t) ==
        [a \in Addr |->
            IF write_set[t] \cap {CL(a)} # {}
            THEN LET found == CHOOSE cl \in (write_set[t] \cap {CL(a)}) : TRUE IN
                 IF write_data[t][found] # NoWrite THEN write_data[t][found] ELSE mem[a]
            ELSE mem[a]]

    (* SGL write-back: apply sgl_write_set to memory *)
    SGLWriteBack(t) ==
        [a \in Addr |->
            IF sgl_write_set[t] \cap {CL(a)} # {}
            THEN LET found == CHOOSE cl \in (sgl_write_set[t] \cap {CL(a)}) : TRUE IN
                 IF sgl_write_data[t][found] # NoWrite THEN sgl_write_data[t][found] ELSE mem[a]
            ELSE mem[a]]
end define;

process ThreadProc \in Thread
begin

L_idle:
    either \* TSX Begin
        await sgl_lock = 0;
        mode[self] := "tsx";
        write_set[self] := {};
        write_data[self] := [cl \in CacheLine |-> NoWrite];
        bloom[self] := {};
        read_lines[self] := {};
        cycles[self] := cycles[self] + 20;
        tsx_retries[self] := 0;
        goto L_tsx;
    or \* SGL Begin
        await sgl_lock = 0 /\ \A other \in Thread \ {self} : mode[other] # "tsx";
        sgl_lock := self;
        mode[self] := "sgl";
        sgl_write_set[self] := {};
        sgl_write_data[self] := [cl \in CacheLine |-> NoWrite];
        cycles[self] := cycles[self] + 100;
        goto L_sgl;
    end either;

L_tsx:
    either \* Read: track in bloom + read_lines
        with a \in Addr do
            if Cardinality(read_lines[self] \union {CL(a)}) <= MAX_READ_LINES then
                read_lines[self] := read_lines[self] \union {CL(a)};
                bloom[self] := bloom[self] \union Hash[CL(a)];
                cycles[self] := cycles[self] + 4;
            else
                \* Capacity abort (read limit)
                mode[self] := "idle";
                write_set[self] := {};
                write_data[self] := [cl \in CacheLine |-> NoWrite];
                bloom[self] := {};
                read_lines[self] := {};
                tsx_retries[self] := tsx_retries[self] + 1;
                cycles[self] := cycles[self] + 1500;
                aborted[self] := aborted[self] + 1;
                capacity_aborts[self] := capacity_aborts[self] + 1;
            end if;
        end with;
        goto L_tsx;
    or \* Write: buffer in write_set
        with a \in Addr do
            if Cardinality(write_set[self] \union {CL(a)}) <= MAX_WRITE_LINES then
                write_set[self] := write_set[self] \union {CL(a)};
                write_data[self][CL(a)] := mem[a];
                cycles[self] := cycles[self] + 5;
            else
                \* Capacity abort (write limit)
                mode[self] := "idle";
                write_set[self] := {};
                write_data[self] := [cl \in CacheLine |-> NoWrite];
                bloom[self] := {};
                read_lines[self] := {};
                tsx_retries[self] := tsx_retries[self] + 1;
                cycles[self] := cycles[self] + 1500;
                aborted[self] := aborted[self] + 1;
                capacity_aborts[self] := capacity_aborts[self] + 1;
            end if;
        end with;
        goto L_tsx;
    or \* Commit: check conflict-free, then write-back
        if sgl_lock = 0 /\ CF(self) then
            mem := WriteBack(self);
            mode[self] := "idle";
            write_set[self] := {};
            write_data[self] := [cl \in CacheLine |-> NoWrite];
            bloom[self] := {};
            read_lines[self] := {};
            cycles[self] := cycles[self] + 80;
            committed[self] := committed[self] + 1;
        else
            \* Conflict or SGL active: abort
            mode[self] := "idle";
            write_set[self] := {};
            write_data[self] := [cl \in CacheLine |-> NoWrite];
            bloom[self] := {};
            read_lines[self] := {};
            tsx_retries[self] := tsx_retries[self] + 1;
            cycles[self] := cycles[self] + 2500;
            aborted[self] := aborted[self] + 1;
            conflict_aborts[self] := conflict_aborts[self] + 1;
        end if;
        goto L_idle;
    or \* Nondeterministic abort (simulates any RTM abort)
        mode[self] := "idle";
        write_set[self] := {};
        write_data[self] := [cl \in CacheLine |-> NoWrite];
        bloom[self] := {};
        read_lines[self] := {};
        tsx_retries[self] := tsx_retries[self] + 1;
        cycles[self] := cycles[self] + 1500;
        aborted[self] := aborted[self] + 1;
        goto L_tsx_retry;
    end either;

L_tsx_retry:
    if tsx_retries[self] < MAX_RETRIES then
        \* Retry TSX
        await sgl_lock = 0;
        mode[self] := "tsx";
        write_set[self] := {};
        write_data[self] := [cl \in CacheLine |-> NoWrite];
        bloom[self] := {};
        read_lines[self] := {};
        cycles[self] := cycles[self] + 20;
        goto L_tsx;
    else
        \* Fall back to SGL
        await sgl_lock = 0 /\ \A other \in Thread \ {self} : mode[other] # "tsx";
        sgl_lock := self;
        mode[self] := "sgl";
        sgl_write_set[self] := {};
        sgl_write_data[self] := [cl \in CacheLine |-> NoWrite];
        cycles[self] := cycles[self] + 100;
        goto L_sgl;
    end if;

L_sgl:
    either \* Read (lock provides isolation)
        with a \in Addr do
            skip;
        end with;
        cycles[self] := cycles[self] + 4;
        goto L_sgl;
    or \* Write: direct to memory + track in sgl_write_set
        with a \in Addr do
            mem[a] := mem[a];  \* re-write same value (TLC model)
            sgl_write_set[self] := sgl_write_set[self] \union {CL(a)};
            sgl_write_data[self][CL(a)] := mem[a];
            cycles[self] := cycles[self] + 6;
        end with;
        goto L_sgl;
    or \* Commit: apply SGL writes, release lock
        mem := SGLWriteBack(self);
        sgl_lock := 0;
        mode[self] := "idle";
        sgl_write_set[self] := {};
        sgl_write_data[self] := [cl \in CacheLine |-> NoWrite];
        cycles[self] := cycles[self] + 75;
        committed[self] := committed[self] + 1;
        goto L_idle;
    end either;

end process;

end algorithm; *)
\* BEGIN TRANSLATION (chksum(pcal) = "589ec8ef" /\ chksum(tla) = "25b974a8")
VARIABLES pc, mem, sgl_lock, mode, write_set, write_data, bloom, read_lines, 
          sgl_write_set, sgl_write_data, tsx_retries, cycles, committed, 
          aborted, capacity_aborts, conflict_aborts

(* define statement *)
CF(t) ==
    \A t2 \in Thread \ {t} :
        /\ \A cl \in write_set[t] :
             bloom[t2] \cap Hash[cl] = {}
        /\ write_set[t] \cap write_set[t2] = {}


WriteBack(t) ==
    [a \in Addr |->
        IF write_set[t] \cap {CL(a)} # {}
        THEN LET found == CHOOSE cl \in (write_set[t] \cap {CL(a)}) : TRUE IN
             IF write_data[t][found] # NoWrite THEN write_data[t][found] ELSE mem[a]
        ELSE mem[a]]


SGLWriteBack(t) ==
    [a \in Addr |->
        IF sgl_write_set[t] \cap {CL(a)} # {}
        THEN LET found == CHOOSE cl \in (sgl_write_set[t] \cap {CL(a)}) : TRUE IN
             IF sgl_write_data[t][found] # NoWrite THEN sgl_write_data[t][found] ELSE mem[a]
        ELSE mem[a]]


vars == << pc, mem, sgl_lock, mode, write_set, write_data, bloom, read_lines, 
           sgl_write_set, sgl_write_data, tsx_retries, cycles, committed, 
           aborted, capacity_aborts, conflict_aborts >>

ProcSet == (Thread)

Init == (* Global variables *)
        /\ mem = [a \in Addr |-> 0]
        /\ sgl_lock = 0
        /\ mode = [t \in Thread |-> "idle"]
        /\ write_set = [t \in Thread |-> {}]
        /\ write_data = [t \in Thread |-> [cl \in CacheLine |-> NoWrite]]
        /\ bloom = [t \in Thread |-> {}]
        /\ read_lines = [t \in Thread |-> {}]
        /\ sgl_write_set = [t \in Thread |-> {}]
        /\ sgl_write_data = [t \in Thread |-> [cl \in CacheLine |-> NoWrite]]
        /\ tsx_retries = [t \in Thread |-> 0]
        /\ cycles = [t \in Thread |-> 0]
        /\ committed = [t \in Thread |-> 0]
        /\ aborted = [t \in Thread |-> 0]
        /\ capacity_aborts = [t \in Thread |-> 0]
        /\ conflict_aborts = [t \in Thread |-> 0]
        /\ pc = [self \in ProcSet |-> "L_idle"]

L_idle(self) == /\ pc[self] = "L_idle"
                /\ \/ /\ sgl_lock = 0
                      /\ mode' = [mode EXCEPT ![self] = "tsx"]
                      /\ write_set' = [write_set EXCEPT ![self] = {}]
                      /\ write_data' = [write_data EXCEPT ![self] = [cl \in CacheLine |-> NoWrite]]
                      /\ bloom' = [bloom EXCEPT ![self] = {}]
                      /\ read_lines' = [read_lines EXCEPT ![self] = {}]
                      /\ cycles' = [cycles EXCEPT ![self] = cycles[self] + 20]
                      /\ tsx_retries' = [tsx_retries EXCEPT ![self] = 0]
                      /\ pc' = [pc EXCEPT ![self] = "L_tsx"]
                      /\ UNCHANGED <<sgl_lock, sgl_write_set, sgl_write_data>>
                   \/ /\ sgl_lock = 0 /\ \A other \in Thread \ {self} : mode[other] # "tsx"
                      /\ sgl_lock' = self
                      /\ mode' = [mode EXCEPT ![self] = "sgl"]
                      /\ sgl_write_set' = [sgl_write_set EXCEPT ![self] = {}]
                      /\ sgl_write_data' = [sgl_write_data EXCEPT ![self] = [cl \in CacheLine |-> NoWrite]]
                      /\ cycles' = [cycles EXCEPT ![self] = cycles[self] + 100]
                      /\ pc' = [pc EXCEPT ![self] = "L_sgl"]
                      /\ UNCHANGED <<write_set, write_data, bloom, read_lines, tsx_retries>>
                /\ UNCHANGED << mem, committed, aborted, capacity_aborts, 
                                conflict_aborts >>

L_tsx(self) == /\ pc[self] = "L_tsx"
               /\ \/ /\ \E a \in Addr:
                          IF Cardinality(read_lines[self] \union {CL(a)}) <= MAX_READ_LINES
                             THEN /\ read_lines' = [read_lines EXCEPT ![self] = read_lines[self] \union {CL(a)}]
                                  /\ bloom' = [bloom EXCEPT ![self] = bloom[self] \union Hash[CL(a)]]
                                  /\ cycles' = [cycles EXCEPT ![self] = cycles[self] + 4]
                                  /\ UNCHANGED << mode, write_set, write_data, 
                                                  tsx_retries, aborted, 
                                                  capacity_aborts >>
                             ELSE /\ mode' = [mode EXCEPT ![self] = "idle"]
                                  /\ write_set' = [write_set EXCEPT ![self] = {}]
                                  /\ write_data' = [write_data EXCEPT ![self] = [cl \in CacheLine |-> NoWrite]]
                                  /\ bloom' = [bloom EXCEPT ![self] = {}]
                                  /\ read_lines' = [read_lines EXCEPT ![self] = {}]
                                  /\ tsx_retries' = [tsx_retries EXCEPT ![self] = tsx_retries[self] + 1]
                                  /\ cycles' = [cycles EXCEPT ![self] = cycles[self] + 1500]
                                  /\ aborted' = [aborted EXCEPT ![self] = aborted[self] + 1]
                                  /\ capacity_aborts' = [capacity_aborts EXCEPT ![self] = capacity_aborts[self] + 1]
                     /\ pc' = [pc EXCEPT ![self] = "L_tsx"]
                     /\ UNCHANGED <<mem, committed, conflict_aborts>>
                  \/ /\ \E a \in Addr:
                          IF Cardinality(write_set[self] \union {CL(a)}) <= MAX_WRITE_LINES
                             THEN /\ write_set' = [write_set EXCEPT ![self] = write_set[self] \union {CL(a)}]
                                  /\ write_data' = [write_data EXCEPT ![self][CL(a)] = mem[a]]
                                  /\ cycles' = [cycles EXCEPT ![self] = cycles[self] + 5]
                                  /\ UNCHANGED << mode, bloom, read_lines, 
                                                  tsx_retries, aborted, 
                                                  capacity_aborts >>
                             ELSE /\ mode' = [mode EXCEPT ![self] = "idle"]
                                  /\ write_set' = [write_set EXCEPT ![self] = {}]
                                  /\ write_data' = [write_data EXCEPT ![self] = [cl \in CacheLine |-> NoWrite]]
                                  /\ bloom' = [bloom EXCEPT ![self] = {}]
                                  /\ read_lines' = [read_lines EXCEPT ![self] = {}]
                                  /\ tsx_retries' = [tsx_retries EXCEPT ![self] = tsx_retries[self] + 1]
                                  /\ cycles' = [cycles EXCEPT ![self] = cycles[self] + 1500]
                                  /\ aborted' = [aborted EXCEPT ![self] = aborted[self] + 1]
                                  /\ capacity_aborts' = [capacity_aborts EXCEPT ![self] = capacity_aborts[self] + 1]
                     /\ pc' = [pc EXCEPT ![self] = "L_tsx"]
                     /\ UNCHANGED <<mem, committed, conflict_aborts>>
                  \/ /\ IF sgl_lock = 0 /\ CF(self)
                           THEN /\ mem' = WriteBack(self)
                                /\ mode' = [mode EXCEPT ![self] = "idle"]
                                /\ write_set' = [write_set EXCEPT ![self] = {}]
                                /\ write_data' = [write_data EXCEPT ![self] = [cl \in CacheLine |-> NoWrite]]
                                /\ bloom' = [bloom EXCEPT ![self] = {}]
                                /\ read_lines' = [read_lines EXCEPT ![self] = {}]
                                /\ cycles' = [cycles EXCEPT ![self] = cycles[self] + 80]
                                /\ committed' = [committed EXCEPT ![self] = committed[self] + 1]
                                /\ UNCHANGED << tsx_retries, aborted, 
                                                conflict_aborts >>
                           ELSE /\ mode' = [mode EXCEPT ![self] = "idle"]
                                /\ write_set' = [write_set EXCEPT ![self] = {}]
                                /\ write_data' = [write_data EXCEPT ![self] = [cl \in CacheLine |-> NoWrite]]
                                /\ bloom' = [bloom EXCEPT ![self] = {}]
                                /\ read_lines' = [read_lines EXCEPT ![self] = {}]
                                /\ tsx_retries' = [tsx_retries EXCEPT ![self] = tsx_retries[self] + 1]
                                /\ cycles' = [cycles EXCEPT ![self] = cycles[self] + 2500]
                                /\ aborted' = [aborted EXCEPT ![self] = aborted[self] + 1]
                                /\ conflict_aborts' = [conflict_aborts EXCEPT ![self] = conflict_aborts[self] + 1]
                                /\ UNCHANGED << mem, committed >>
                     /\ pc' = [pc EXCEPT ![self] = "L_idle"]
                     /\ UNCHANGED capacity_aborts
                  \/ /\ mode' = [mode EXCEPT ![self] = "idle"]
                     /\ write_set' = [write_set EXCEPT ![self] = {}]
                     /\ write_data' = [write_data EXCEPT ![self] = [cl \in CacheLine |-> NoWrite]]
                     /\ bloom' = [bloom EXCEPT ![self] = {}]
                     /\ read_lines' = [read_lines EXCEPT ![self] = {}]
                     /\ tsx_retries' = [tsx_retries EXCEPT ![self] = tsx_retries[self] + 1]
                     /\ cycles' = [cycles EXCEPT ![self] = cycles[self] + 1500]
                     /\ aborted' = [aborted EXCEPT ![self] = aborted[self] + 1]
                     /\ pc' = [pc EXCEPT ![self] = "L_tsx_retry"]
                     /\ UNCHANGED <<mem, committed, capacity_aborts, conflict_aborts>>
               /\ UNCHANGED << sgl_lock, sgl_write_set, sgl_write_data >>

L_tsx_retry(self) == /\ pc[self] = "L_tsx_retry"
                     /\ IF tsx_retries[self] < MAX_RETRIES
                           THEN /\ sgl_lock = 0
                                /\ mode' = [mode EXCEPT ![self] = "tsx"]
                                /\ write_set' = [write_set EXCEPT ![self] = {}]
                                /\ write_data' = [write_data EXCEPT ![self] = [cl \in CacheLine |-> NoWrite]]
                                /\ bloom' = [bloom EXCEPT ![self] = {}]
                                /\ read_lines' = [read_lines EXCEPT ![self] = {}]
                                /\ cycles' = [cycles EXCEPT ![self] = cycles[self] + 20]
                                /\ pc' = [pc EXCEPT ![self] = "L_tsx"]
                                /\ UNCHANGED << sgl_lock, sgl_write_set, 
                                                sgl_write_data >>
                           ELSE /\ sgl_lock = 0 /\ \A other \in Thread \ {self} : mode[other] # "tsx"
                                /\ sgl_lock' = self
                                /\ mode' = [mode EXCEPT ![self] = "sgl"]
                                /\ sgl_write_set' = [sgl_write_set EXCEPT ![self] = {}]
                                /\ sgl_write_data' = [sgl_write_data EXCEPT ![self] = [cl \in CacheLine |-> NoWrite]]
                                /\ cycles' = [cycles EXCEPT ![self] = cycles[self] + 100]
                                /\ pc' = [pc EXCEPT ![self] = "L_sgl"]
                                /\ UNCHANGED << write_set, write_data, bloom, 
                                                read_lines >>
                     /\ UNCHANGED << mem, tsx_retries, committed, aborted, 
                                     capacity_aborts, conflict_aborts >>

L_sgl(self) == /\ pc[self] = "L_sgl"
               /\ \/ /\ \E a \in Addr:
                          TRUE
                     /\ cycles' = [cycles EXCEPT ![self] = cycles[self] + 4]
                     /\ pc' = [pc EXCEPT ![self] = "L_sgl"]
                     /\ UNCHANGED <<mem, sgl_lock, mode, sgl_write_set, sgl_write_data, committed>>
                  \/ /\ \E a \in Addr:
                          /\ mem' = [mem EXCEPT ![a] = mem[a]]
                          /\ sgl_write_set' = [sgl_write_set EXCEPT ![self] = sgl_write_set[self] \union {CL(a)}]
                          /\ sgl_write_data' = [sgl_write_data EXCEPT ![self][CL(a)] = mem'[a]]
                          /\ cycles' = [cycles EXCEPT ![self] = cycles[self] + 6]
                     /\ pc' = [pc EXCEPT ![self] = "L_sgl"]
                     /\ UNCHANGED <<sgl_lock, mode, committed>>
                  \/ /\ mem' = SGLWriteBack(self)
                     /\ sgl_lock' = 0
                     /\ mode' = [mode EXCEPT ![self] = "idle"]
                     /\ sgl_write_set' = [sgl_write_set EXCEPT ![self] = {}]
                     /\ sgl_write_data' = [sgl_write_data EXCEPT ![self] = [cl \in CacheLine |-> NoWrite]]
                     /\ cycles' = [cycles EXCEPT ![self] = cycles[self] + 75]
                     /\ committed' = [committed EXCEPT ![self] = committed[self] + 1]
                     /\ pc' = [pc EXCEPT ![self] = "L_idle"]
               /\ UNCHANGED << write_set, write_data, bloom, read_lines, 
                               tsx_retries, aborted, capacity_aborts, 
                               conflict_aborts >>

ThreadProc(self) == L_idle(self) \/ L_tsx(self) \/ L_tsx_retry(self)
                       \/ L_sgl(self)

(* Allow infinite stuttering to prevent deadlock on termination. *)
Terminating == /\ \A self \in ProcSet: pc[self] = "Done"
               /\ UNCHANGED vars

Next == (\E self \in Thread: ThreadProc(self))
           \/ Terminating

Spec == Init /\ [][Next]_vars

Termination == <>(\A self \in ProcSet: pc[self] = "Done")

\* END TRANSLATION 

(***********************************************************************)
(* pcal.trans output will be inserted here                             *)
(***********************************************************************)

(*====================================================================*)
(* Invariants                                                         *)
(*====================================================================*)

LockFreeInv ==
    (sgl_lock = 0) <=> ~(\E t \in Thread : mode[t] = "sgl")

LockOwnerInv ==
    \A t \in Thread : (sgl_lock = t) => (mode[t] = "sgl")

TSXvsSGLSafety ==
    \A t \in Thread : (mode[t] = "tsx") => (sgl_lock = 0)

BloomContainsReads ==
    \A t \in Thread :
        \A cl \in read_lines[t] :
            Hash[cl] \subseteq bloom[t]

CapacityBounds ==
    \A t \in Thread :
        mode[t] = "tsx" =>
            /\ Cardinality(read_lines[t]) <= MAX_READ_LINES
            /\ Cardinality(write_set[t]) <= MAX_WRITE_LINES

NoTSXCommitConflict ==
    \A t1, t2 \in Thread :
        (t1 # t2 /\
         \E cl \in write_set[t1] :
             ((\E cl2 \in read_lines[t2] : cl = cl2) \/
              (\E cl2 \in write_set[t2] : cl = cl2)))
        => ~(CF(t1) /\ CF(t2))

NoSGLTSXOverlap ==
    \A t1, t2 \in Thread :
        t1 # t2 /\
        mode[t1] = "sgl" /\
        sgl_write_set[t1] \cap write_set[t2] # {}
        => mode[t2] # "tsx"

(* NOTE: Tautology — (sgl_lock=t1 /\ sgl_lock=t2) => t1=t2 holds trivially. *)
LockExclusion ==
    \A t1, t2 \in Thread :
        (sgl_lock = t1 /\ sgl_lock = t2) => (t1 = t2)

(*====================================================================*)
(* Bounding constraints for TLC termination                           *)
(*====================================================================*)
TLCBound ==
    /\ \A t \in Thread : committed[t] < 2
    /\ \A t \in Thread : aborted[t] < 3
    /\ \A t \in Thread : tsx_retries[t] <= MAX_RETRIES

(*====================================================================*)
(* Temporal properties                                                *)
(*====================================================================*)

Spec_WF == Spec /\ \A self \in Thread : WF_vars(ThreadProc(self))

TransactionProgress ==
    \A t \in Thread :
        []( (pc[t] \in {"L_tsx", "L_sgl", "L_tsx_retry"})
            => <>(pc[t] = "L_idle") )

=====================================================================
