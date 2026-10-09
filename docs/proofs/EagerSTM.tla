---------------------------- MODULE EagerSTM ----------------------------
(*
 * EagerSTM — opacity made formal, for Book Ch. 2 (and the companion
 * backends/tm_impl/eager_broken + benchmarks/cpp/opaquedemo).
 *
 * This models the TRIVIAL EAGER publish/undo protocol exactly as the
 * teaching backend implements it, with the demo's three roles:
 *
 *   W (writer) : a transaction that publishes an inventory slot and the
 *                count eagerly (their effects are visible NOW), records
 *                its read of `seed`, and validates `seed` at commit.
 *                On mismatch it undoes both writes (slot back to NULL,
 *                count back to 0).  The committed projection is clean;
 *                the mid-flight view is not.
 *   S (spoiler): a transaction that commits seed := 2, guaranteeing the
 *                writer aborts.
 *   O (observer): NOT in a transaction — the privatized consumer.  It
 *                latches (count, slot), then later rescans the slot it
 *                latched "exists" — exactly the demo's second phase.
 *
 * Opacity demands that every transaction — committed or aborted — see a
 * state that could have been produced by some serial order.  Here that
 * is violated: O latches count = 1 from a transaction that then ABORTS,
 * and the rescan observes the undone NULL slot through the latched
 * count.  The model records that as the program counter reaching
 * "crash" (a NULL dereference, i.e. the SIGSEGV of bin/opaquedemo).
 *
 * The invariant NoCrash IS violated by design.  `make check-teaching`
 * in docs/proofs asserts TLC finds the counterexample.  The opaque
 * contrast model (sandboxed writes) is NOrec.tla, whose opacity-style
 * invariants pass; the demo shows both from one binary.
 *)

EXTENDS Naturals

CONSTANTS P   (* the address of the allocated item, modelled atomically *)

ASSUME P # 0

VARIABLES pcW, pcS, pcO,        (* program counters *)
          inv, cnt, seed,        (* shared memory *)
          obsSeed,               (* writer's read-set value *)
          latchedN,              (* observer's latched count *)
          latchedP               (* observer's latched slot   *)

vars == <<pcW, pcS, pcO, inv, cnt, seed, obsSeed, latchedN, latchedP>>

Init ==
    /\ pcW = "I"  /\ pcS = "I"  /\ pcO = "I"
    /\ inv = 0    /\ cnt = 0    /\ seed = 1
    /\ obsSeed = 0
    /\ latchedN = 0 /\ latchedP = 0

(* ---- writer transaction: eager publish, validate seed at commit ---- *)
WAlloc ==
    /\ pcW = "I"
    /\ obsSeed' = seed          \* tm_read(&seed), recorded for validation
    /\ pcW' = "publish"
    /\ UNCHANGED <<pcS, pcO, inv, cnt, seed, latchedN, latchedP>>

WPublishInv ==
    /\ pcW = "publish"
    /\ inv' = P                 \* tm_write(&inventory[0], it): VISIBLE NOW
    /\ pcW' = "publishCount"
    /\ UNCHANGED <<pcS, pcO, cnt, seed, obsSeed, latchedN, latchedP>>

WPublishCount ==
    /\ pcW = "publishCount"
    /\ cnt' = 1                 \* tm_write(&count, 1): VISIBLE NOW
    /\ pcW' = "validate"
    /\ UNCHANGED <<pcS, pcO, inv, seed, obsSeed, latchedN, latchedP>>

WValidate ==
    /\ pcW = "validate"
    /\ IF seed = obsSeed
     THEN /\ pcW' = "committed"                       (* keep eager writes *)
          /\ UNCHANGED <<inv, cnt>>
     ELSE /\ inv' = 0                                  (* undo: slot -> NULL *)
          /\ cnt' = 0                                  (* undo: count -> 0 *)
          /\ pcW' = "aborted"                          (* then siglongjmp  *)
    /\ UNCHANGED <<pcS, pcO, seed, obsSeed, latchedN, latchedP>>

(* ---- spoiler: a committed transaction that forces the abort ---------- *)
SSpoil ==
    /\ pcS = "I"
    /\ seed' = 2
    /\ pcS' = "done"
    /\ UNCHANGED <<pcW, pcO, inv, cnt, obsSeed, latchedN, latchedP>>

(* ---- observer: the non-transactional consumer (privatization) -------- *)
OLatch ==
    /\ pcO = "I"
    /\ latchedN' = cnt          \* mid-flight: sees count = 1, slot = P
    /\ latchedP' = inv          \* the phantom item is fully readable here
    /\ pcO' = "rescan"
    /\ UNCHANGED <<pcW, pcS, inv, cnt, seed, obsSeed>>

ORescan ==
    /\ pcO = "rescan"
    /\ IF latchedN > 0 /\ inv = 0    \* latched "one item", but the abort
     THEN /\ pcO' = "crash"          \* undid the slot -> NULL dereference
     ELSE /\ pcO' = "ok"
    /\ UNCHANGED <<pcW, pcS, inv, cnt, seed, obsSeed, latchedN, latchedP>>

Next ==
    WAlloc \/ WPublishInv \/ WPublishCount \/ WValidate
    \/ SSpoil \/ OLatch \/ ORescan

Spec == Init /\ [][Next]_vars

(* -------------------------- invariants ------------------------------- *)
TypeOK ==
    /\ pcW \in {"I", "publish", "publishCount", "validate", "committed",
                "aborted"}
    /\ pcS \in {"I", "done"}
    /\ pcO \in {"I", "rescan", "ok", "crash"}
    /\ inv \in {0, P}
    /\ cnt \in {0, 1}
    /\ seed \in {1, 2}
    /\ latchedN \in {0, 1}
    /\ latchedP \in {0, P}

(* THE invariant: the application must never crash.  Opacity is exactly
   the condition that makes NoCrash hold for every program that is
   correct under serial execution; without it, this model reaches crash. *)
NoCrash == pcO # "crash"

==========================================================================
