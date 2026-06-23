---------------------- MODULE DistributedSGL ----------------------
(*
 * DistributedSGL — SGL over Network Messages
 *
 * Algorithm: A Single Global Lock acquired via network message
 * passing instead of shared memory.  Each node sends a LOCK_REQ
 * message, receives LOCK_GRANT, executes the transaction, then
 * sends UNLOCK.  The lock server grants to one requester at a time.
 *
 * This is a trivial extension of SGL.tla with explicit message
 * passing between a lock server and N client nodes.
 *
 * Invariants (matching SGL.tla):
 *   LockExclusion: At most one client holds the lock at any time.
 *   LockServerConsistency: The server grants the lock to at most
 *     one client at a time.
 *)

EXTENDS Naturals, FiniteSets, TLC

CONSTANTS
    Client,             (* Set of client node IDs *)
    Addr,               (* Set of memory addresses *)
    Data                (* Set of possible data values *)

ASSUME Client \subseteq Nat \ {0}
ASSUME Addr \subseteq Nat
ASSUME Data \subseteq Nat

VARIABLES
    lock_holder,        (* Client \cup {0}: who holds the lock *)
    pending_req,        (* Client \cup {0}: pending request being processed *)
    mem,                (* [Addr -> Nat]: shared memory *)
    pc,                 (* [Client -> {"idle", "waiting", "active", "done"}] *)
    granted,            (* [Client -> BOOLEAN]: whether client thinks it has lock *)
    version,            (* Nat: global version counter *)
    committed,          (* [Client -> Nat] *)
    msg_queue           (* Set of <<src, type>>: messages in flight *)

vars == <<lock_holder, pending_req, mem, pc, granted, version,
          committed, msg_queue>>

MSG_LOCK_REQ == "lock_req"
MSG_LOCK_GRANT == "lock_grant"
MSG_UNLOCK == "unlock"

(*--------------------------------------------------------------------*)
(* Init                                                                *)
(*--------------------------------------------------------------------*)

Init ==
    /\ lock_holder = 0
    /\ pending_req = 0
    /\ mem = [a \in Addr |-> 0]
    /\ pc = [c \in Client |-> "idle"]
    /\ granted = [c \in Client |-> FALSE]
    /\ version = 0
    /\ committed = [c \in Client |-> 0]
    /\ msg_queue = {}

(*--------------------------------------------------------------------*)
(* Client Actions                                                      *)
(*--------------------------------------------------------------------*)

(* Client sends lock request *)
SendLockReq(c) ==
    /\ pc[c] = "idle"
    /\ msg_queue' = msg_queue \cup {<<c, MSG_LOCK_REQ>>}
    /\ pc' = [pc EXCEPT ![c] = "waiting"]
    /\ UNCHANGED <<lock_holder, pending_req, mem, granted, version,
                   committed>>

(* Server processes lock request: grant if available *)
ProcessLockReq ==
    /\ \E msg \in msg_queue :
        /\ msg[1] \in Client
        /\ msg[2] = MSG_LOCK_REQ
        /\ lock_holder = 0
        /\ pending_req = 0
        /\ lock_holder' = msg[1]
        /\ pending_req' = msg[1]
        /\ msg_queue' = msg_queue \ {msg}
        /\ msg_queue' = msg_queue' \cup {<<msg[1], MSG_LOCK_GRANT>>}
    /\ UNCHANGED <<mem, pc, granted, version, committed>>

(* Client receives lock grant *)
RecvLockGrant(c) ==
    /\ <<c, MSG_LOCK_GRANT>> \in msg_queue
    /\ pc[c] = "waiting"
    /\ granted' = [granted EXCEPT ![c] = TRUE]
    /\ pc' = [pc EXCEPT ![c] = "active"]
    /\ msg_queue' = msg_queue \ {<<c, MSG_LOCK_GRANT>>}
    /\ pending_req' = 0
    /\ UNCHANGED <<lock_holder, mem, version, committed>>

(* Client read *)
ClientRead(c, a) ==
    /\ pc[c] = "active"
    /\ granted[c] = TRUE
    /\ a \in Addr
    /\ UNCHANGED vars

(* Client write *)
ClientWrite(c, a, v) ==
    /\ pc[c] = "active"
    /\ granted[c] = TRUE
    /\ a \in Addr
    /\ v \in Data
    /\ mem' = [mem EXCEPT ![a] = v]
    /\ UNCHANGED <<lock_holder, pending_req, pc, granted, version,
                   committed, msg_queue>>

(* Client releases lock *)
SendUnlock(c) ==
    /\ pc[c] = "active"
    /\ granted[c] = TRUE
    /\ msg_queue' = msg_queue \cup {<<c, MSG_UNLOCK>>}
    /\ pc' = [pc EXCEPT ![c] = "done"]
    /\ UNCHANGED <<lock_holder, pending_req, mem, granted, version,
                   committed>>

(* Server processes unlock *)
ProcessUnlock ==
    /\ \E msg \in msg_queue :
        /\ msg[1] \in Client
        /\ msg[2] = MSG_UNLOCK
        /\ lock_holder = msg[1]
        /\ lock_holder' = 0
        /\ granted' = [granted EXCEPT ![msg[1]] = FALSE]
        /\ version' = version + 1
        /\ committed' = [committed EXCEPT ![msg[1]] = committed[msg[1]] + 1]
        /\ pc' = [pc EXCEPT ![msg[1]] = "idle"]
        /\ msg_queue' = msg_queue \ {msg}
    /\ UNCHANGED <<pending_req, mem>>

(*--------------------------------------------------------------------*)
(* Next-state relation                                                *)
(*--------------------------------------------------------------------*)

Next ==
    \/ \E c \in Client : SendLockReq(c)
    \/ ProcessLockReq
    \/ \E c \in Client : RecvLockGrant(c)
    \/ \E c \in Client : \E a \in Addr : ClientRead(c, a)
    \/ \E c \in Client : \E a \in Addr : \E v \in Data : ClientWrite(c, a, v)
    \/ \E c \in Client : SendUnlock(c)
    \/ ProcessUnlock

Spec == Init /\ [][Next]_vars

(*====================================================================*)
(* Invariants                                                         *)
(*====================================================================*)

(*── I1: At most one client holds the lock ──────────────────────────*)
LockExclusion ==
    \A c1, c2 \in Client :
        (lock_holder = c1 /\ lock_holder = c2) => c1 = c2

(*── I2: Lock holder has granted flag ───────────────────────────────*)
LockHolderHasGrant ==
    \A c \in Client :
        lock_holder = c => granted[c] = TRUE

(*── I3: No client has grant without holding the lock ──────────────*)
NoSpuriousGrant ==
    \A c \in Client :
        granted[c] = TRUE => lock_holder = c

(*── I4: At most one pending request at a time ─────────────────────*)
AtMostOnePending ==
    /\ Cardinality({x \in msg_queue : x[2] = MSG_LOCK_REQ}) <= 1
    /\ pending_req = 0 \/ \E c \in Client : <<c, MSG_LOCK_GRANT>> \notin msg_queue

(*── I5: Server consistency — only one grant in flight ─────────────*)
ServerConsistency ==
    Cardinality({x \in msg_queue : x[2] = MSG_LOCK_GRANT}) <= 1

====
