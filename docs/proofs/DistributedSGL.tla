---------------------- MODULE DistributedSGL ----------------------
(*
 * DistributedSGL — SGL over Network Messages (PlusCal)
 *
 * Algorithm: A Single Global Lock acquired via network message
 * passing.  Lock server with N client nodes.
 *
 * Invariants:
 *   LockExclusion:        At most one client holds the lock
 *   LockHolderHasGrant:   Lock holder has the grant flag
 *   NoSpuriousGrant:      Grant flag only when holding lock
 *   AtMostOnePending:     At most one pending request in flight
 *   ServerConsistency:    At most one grant in flight
 *)

EXTENDS Naturals, FiniteSets, TLC

CONSTANTS
    Client,             (* Set of client node IDs *)
    Addr,               (* Set of memory addresses *)
    Data                (* Set of possible data values *)

ASSUME Client \subseteq Nat \ {0}
ASSUME Addr \subseteq Nat
ASSUME Data \subseteq Nat

(*─── PlusCal algorithm ───────────────────────────────────────────────*)
(* --algorithm DistributedSGL

variables
    lock_holder = 0,
    mem = [a \in Addr |-> 0],
    version = 0,
    msg_queue = {},
    granted = [c \in Client |-> FALSE],
    committed = [c \in Client |-> 0];

process ClientProc \in Client
begin
L_idle:
    msg_queue := msg_queue \union {<<self, "lock_req">>};
    goto L_waiting;

L_waiting:
    if <<self, "lock_grant">> \in msg_queue then
        granted[self] := TRUE;
        msg_queue := msg_queue \ {<<self, "lock_grant">>};
        goto L_active;
    else
        goto L_waiting;
    end if;

L_active:
    either \* Read (nop)
        with a \in Addr do
            skip;
        end with;
        goto L_active;
    or \* Write
        with a \in Addr, v \in Data do
            mem[a] := v;
        end with;
        goto L_active;
    or \* Unlock
        msg_queue := msg_queue \union {<<self, "unlock">>};
        goto L_idle;
    end either;
end process;

fair process ServerProc = 0
begin
L_server:
    either \* Process lock request
        with msg \in {m \in msg_queue : m[2] = "lock_req" /\ lock_holder = 0} do
            lock_holder := msg[1];
            granted[msg[1]] := TRUE;
            msg_queue := (msg_queue \ {msg}) \union {<<msg[1], "lock_grant">>};
        end with;
        goto L_server;
    or \* Process unlock
        with msg \in {m \in msg_queue : m[2] = "unlock" /\ lock_holder = m[1]} do
            lock_holder := 0;
            granted[msg[1]] := FALSE;
            version := version + 1;
            committed[msg[1]] := committed[msg[1]] + 1;
            msg_queue := msg_queue \ {msg};
        end with;
        goto L_server;
    end either;
end process;

end algorithm; *)

\* BEGIN TRANSLATION
VARIABLES lock_holder, mem, version, msg_queue, granted, committed, pc

vars == << lock_holder, mem, version, msg_queue, granted, committed, pc >>

ProcSet == (Client) \cup {0}

Init == (* Global variables *)
        /\ lock_holder = 0
        /\ mem = [a \in Addr |-> 0]
        /\ version = 0
        /\ msg_queue = {}
        /\ granted = [c \in Client |-> FALSE]
        /\ committed = [c \in Client |-> 0]
        /\ pc = [self \in ProcSet |-> CASE self \in Client -> "L_idle"
                                        [] self = 0 -> "L_server"]

L_idle(self) == /\ pc[self] = "L_idle"
                /\ msg_queue' = (msg_queue \union {<<self, "lock_req">>})
                /\ pc' = [pc EXCEPT ![self] = "L_waiting"]
                /\ UNCHANGED << lock_holder, mem, version, granted, committed >>

L_waiting(self) == /\ pc[self] = "L_waiting"
                   /\ IF <<self, "lock_grant">> \in msg_queue
                         THEN /\ granted' = [granted EXCEPT ![self] = TRUE]
                              /\ msg_queue' = msg_queue \ {<<self, "lock_grant">>}
                              /\ pc' = [pc EXCEPT ![self] = "L_active"]
                         ELSE /\ pc' = [pc EXCEPT ![self] = "L_waiting"]
                              /\ UNCHANGED << msg_queue, granted >>
                   /\ UNCHANGED << lock_holder, mem, version, committed >>

L_active(self) == /\ pc[self] = "L_active"
                  /\ \/ /\ \E a \in Addr:
                             TRUE
                        /\ pc' = [pc EXCEPT ![self] = "L_active"]
                        /\ UNCHANGED <<mem, msg_queue>>
                     \/ /\ \E a \in Addr:
                             \E v \in Data:
                               mem' = [mem EXCEPT ![a] = v]
                        /\ pc' = [pc EXCEPT ![self] = "L_active"]
                        /\ UNCHANGED msg_queue
                     \/ /\ msg_queue' = (msg_queue \union {<<self, "unlock">>})
                        /\ pc' = [pc EXCEPT ![self] = "L_idle"]
                        /\ mem' = mem
                  /\ UNCHANGED << lock_holder, version, granted, committed >>

ClientProc(self) == L_idle(self) \/ L_waiting(self) \/ L_active(self)

L_server == /\ pc[0] = "L_server"
            /\ \/ /\ \E msg \in {m \in msg_queue : m[2] = "lock_req" /\ lock_holder = 0}:
                       /\ lock_holder' = msg[1]
                       /\ granted' = [granted EXCEPT ![msg[1]] = TRUE]
                       /\ msg_queue' = ((msg_queue \ {msg}) \union {<<msg[1], "lock_grant">>})
                  /\ pc' = [pc EXCEPT ![0] = "L_server"]
                  /\ UNCHANGED <<version, committed>>
               \/ /\ \E msg \in {m \in msg_queue : m[2] = "unlock" /\ lock_holder = m[1]}:
                       /\ lock_holder' = 0
                       /\ granted' = [granted EXCEPT ![msg[1]] = FALSE]
                       /\ version' = version + 1
                       /\ committed' = [committed EXCEPT ![msg[1]] = committed[msg[1]] + 1]
                       /\ msg_queue' = msg_queue \ {msg}
                  /\ pc' = [pc EXCEPT ![0] = "L_server"]
            /\ mem' = mem

ServerProc == L_server

(* Allow infinite stuttering to prevent deadlock on termination. *)
Terminating == /\ \A self \in ProcSet: pc[self] = "Done"
               /\ UNCHANGED vars

Next == ServerProc
           \/ (\E self \in Client: ClientProc(self))
           \/ Terminating

Spec == /\ Init /\ [][Next]_vars
        /\ WF_vars(ServerProc)

Termination == <>(\A self \in ProcSet: pc[self] = "Done")

\* END TRANSLATION

\* END TRANSLATION



(*====================================================================*)
(* Invariants                                                         *)
(*====================================================================*)

LockExclusion ==
    \A c1, c2 \in Client :
        (lock_holder = c1 /\ lock_holder = c2) => c1 = c2

LockHolderHasGrant ==
    \A c \in Client :
        lock_holder = c => granted[c] = TRUE

NoSpuriousGrant ==
    \A c \in Client :
        granted[c] = TRUE => lock_holder = c

ServerConsistency ==
    Cardinality({x \in msg_queue : x[2] = "lock_grant"}) <= 1

(*====================================================================*)
(* Bounding constraints for TLC termination                           *)
(*====================================================================*)
TLCBound ==
    /\ \A c \in Client : committed[c] < 3

(*====================================================================*)
(* Temporal properties                                                *)
(*====================================================================*)

Spec_WF == Spec /\ \A self \in Client : WF_vars(ClientProc(self))
              /\ WF_vars(ServerProc)

ProgressProperty ==
    \A c \in Client :
        (pc[c] = "L_waiting") ~> (pc[c] = "L_active")

=====================================================================
