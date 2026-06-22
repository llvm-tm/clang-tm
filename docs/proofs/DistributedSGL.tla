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
    Addr                (* Set of memory addresses *)

ASSUME Client \subseteq Nat \ {0}
ASSUME Addr \subseteq Nat

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
    /\ \E <<c, MSG_LOCK_REQ>> \in msg_queue :
        /\ lock_holder = 0
        /\ pending_req = 0
        /\ lock_holder' = c
        /\ pending_req' = c
        /\ msg_queue' = msg_queue \ {<<c, MSG_LOCK_REQ>>}
        /\ msg_queue' = msg_queue' \cup {<<c, MSG_LOCK_GRANT>>}
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
    /\ v \in Nat
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
    /\ \E <<c, MSG_UNLOCK>> \in msg_queue :
        /\ lock_holder = c
        /\ lock_holder' = 0
        /\ granted' = [granted EXCEPT ![c] = FALSE]
        /\ version' = version + 1
        /\ committed' = [committed EXCEPT ![c] = committed[c] + 1]
        /\ pc' = [pc EXCEPT ![c] = "idle"]
        /\ msg_queue' = msg_queue \ {<<c, MSG_UNLOCK>>}
    /\ UNCHANGED <<pending_req, mem>>

(*--------------------------------------------------------------------*)
(* Next-state relation                                                *)
(*--------------------------------------------------------------------*)

Next ==
    \/ \E c \in Client : SendLockReq(c)
    \/ ProcessLockReq
    \/ \E c \in Client : RecvLockGrant(c)
    \/ \E c \in Client : \E a \in Addr : ClientRead(c, a)
    \/ \E c \in Client : \E a \in Addr : \E v \in Nat : ClientWrite(c, a, v)
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
    /\ Cardinality({<<c, MSG_LOCK_REQ>> \in msg_queue}) <= 1
    /\ pending_req = 0 \/ \E c \in Client : <<c, MSG_LOCK_GRANT>> \notin msg_queue

(*── I5: Server consistency — only one grant in flight ─────────────*)
ServerConsistency ==
    Cardinality({<<c, MSG_LOCK_GRANT>> \in msg_queue}) <= 1

====
