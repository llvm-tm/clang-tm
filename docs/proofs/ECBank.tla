--------------------------- MODULE ECBank ---------------------------
(* Eventual-consistency bank with automated compensating transactions
   (companion to research/ec-bank/).

   Scenario: two replicas ("the views of two threads"). Two candidate
   transfers, both 1->3 of 8, are created on replica 1 and broadcast to both
   replicas by an asynchronous causal broadcast (a compensation op is
   delivered only after its base transaction). Replica views apply ops as a
   grow-only, idempotent, commutative op sets; balances are folded locally.

   A transaction commits with NO coordination, so a replica may transiently
   show a negative balance (the non-negative invariant is broken *locally* by
   concurrent debits). Each delivery atomically enqueues an automated
   "negative" transaction that undoes the maximal-by-id, still-uncompensated
   transfer contributing to the violated account (deterministic culprit rule,
   so replicas converge to the same repaired view).

   Checked exhaustively:
     - MoneyConserved  : every replica's total == initial total
     - SEC             : equal applied sets => equal views
     - QuiescentSafe   : nothing in flight => no negative balance anywhere
     - CausalPending   : no compensation in flight before its base is applied
*)

EXTENDS Naturals, FiniteSets, TLC

REPLICAS == { 1, 2 }
ACCOUNTS == { 1, 3 }

BalInit(a) == IF a = 1 THEN 10 ELSE 0

Txns == { [id |-> 1, src |-> 1, dst |-> 3, amt |-> 8],
          [id |-> 2, src |-> 1, dst |-> 3, amt |-> 8] }

IsComp(o) == "compOf" \in DOMAIN o

CompOf(o) == [id |-> 10 + o.id, src |-> o.dst, dst |-> o.src,
              amt |-> o.amt, compOf |-> o.id]

ParentOf(o) == IF IsComp(o) THEN o.compOf ELSE o.id

Comps == { CompOf(t) : t \in Txns }

AllOps == Txns \cup Comps

Dval(o, a) == IF o.src = a THEN 0 - o.amt
               ELSE IF o.dst = a THEN o.amt
               ELSE 0

(* ---------------- state --------------------------------------------------- *)

(* applied : grow-only views (the CRDT);  bal : local balances;
   msgs   : in-flight broadcast (AllOps \times REPLICAS);  created : SUBSET Txns *)
VARIABLES applied, bal, msgs, created

vars == <<applied, bal, msgs, created>>

Init ==
    /\ applied = [r \in REPLICAS |-> {}]
    /\ bal = [r \in REPLICAS |-> [a \in ACCOUNTS |-> BalInit(a)]]
    /\ msgs = { <<t, r>> : t \in Txns, r \in REPLICAS }
    /\ created = Txns

(* o is a live (uncompensated, non-comp) transfer debiting account a *)
Contrib(o, A, a) ==
    /\ ~ IsComp(o)
    /\ o.src = a
    /\ ~ \E c \in A : IsComp(c) /\ c.compOf = o.id

Culprits(A, a) == { o \in A : Contrib(o, A, a) }

MaxById(S) == CHOOSE x \in S : \A y \in S : x.id >= y.id

(* compensation message to enqueue after applying o at r breaks acct a *)
Repair(o, r, a) ==
    LET balAfter == bal[r][a] + Dval(o, a) IN
        IF balAfter < 0
        THEN { <<CompOf(MaxById(Culprits(applied[r] \cup {o}, a))), r>> }
        ELSE {}

Next ==
    \E o \in AllOps, r \in REPLICAS :
        /\ <<o, r>> \in msgs
        /\ ( ~ IsComp(o) \/ \E p \in applied[r] : p.id = ParentOf(o) )   \* causal gate
        /\ applied' = [applied EXCEPT ![r] = applied[r] \cup {o}]
        /\ bal' = [bal EXCEPT ![r] = [a \in ACCOUNTS |-> bal[r][a] + Dval(o, a)]]
        /\ msgs' = (msgs \ {<<o, r>>})
                    \cup (UNION { Repair(o, r, a) : a \in ACCOUNTS })
        /\ created' = created
    \/ UNCHANGED vars

(* ---------------- invariants ---------------------------------------------- *)

TypeOK ==
    /\ applied \in [REPLICAS -> SUBSET AllOps]
    /\ msgs \subseteq (AllOps \times REPLICAS)
    /\ created \subseteq Txns

MoneyConserved ==
    \A r \in REPLICAS :
        bal[r][1] + bal[r][3] = BalInit(1) + BalInit(3)

SEC ==
    \A r, s \in REPLICAS :
        applied[r] = applied[s]
            => (bal[r][1] = bal[s][1]) /\ (bal[r][3] = bal[s][3])

QuiescentSafe ==
    msgs = {}
        => \A r \in REPLICAS : (bal[r][1] >= 0) /\ (bal[r][3] >= 0)

CausalPending ==
    \A <<o, r>> \in msgs : ~ IsComp(o) \/ \E p \in applied[r] : p.id = ParentOf(o)

===============================================================================
