---------------------------- MODULE LazyFlags ----------------------------
(***************************************************************************)
(* Refinement of eager CCR semantics by the lazy representation of         *)
(* include/f3rt/cpu_abi.h (cpu->sr plus one pending cc_op), under flag     *)
(* elision driven by a liveness certificate.                               *)
(*                                                                         *)
(* The program is generated online. Each instruction carries the live-out *)
(* set its certificate claims; the previous instruction's claim must cover *)
(* reads(next) + (claim(next) - writes(next)) (+ the IRQ observer set when *)
(* the proposed rule is on). Those are exactly the local inequalities of   *)
(* the certificate checker, so every behaviour is "a program plus a        *)
(* certificate the checker accepts". Behaviours whose claims are not       *)
(* accepted are simply not generated.                                      *)
(*                                                                         *)
(* Flags are abstracted to X, Z, C: Z stands for N/Z (set from a result),  *)
(* C for V/C (cleared by LOGIC, computed by ADD/SUB/CMP).                  *)
(*                                                                         *)
(* Eager and lazy machines run in lock step. Every flag read, every IRQ   *)
(* handler read of inherited flags, and the final boundary compare the    *)
(* lazy machine's flushed flags with the eager machine's.                 *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets

CONSTANTS
    MaxLength,          \* longest generated straight-line program
    IrqObserves,        \* flags an IRQ handler reads before writing them
    RuleCoversIrq,      \* proposed rule: every live-out includes IrqObserves
    ClearStaleOnXOnly,  \* ADD/SUB with only X live clears the pending op
    RepresentationInvariant \* entry states satisfy pending-ARITH X = sr.X

Flags == {"X", "Z", "C"}
ZC == {"Z", "C"}
Ops == {"NONE", "LOGIC", "ARITH", "CMP"}
Kinds == {"LOGIC", "ARITH", "CMP", "ZONLY", "SETCCR", "READ", "NOP"}
ReadSets == {{"X"}, {"Z"}, {"C"}, {"X", "Z"}}

Writes(k) == CASE k = "LOGIC" -> ZC [] k = "ARITH" -> Flags [] k = "CMP" -> ZC
             [] k = "ZONLY" -> {"Z"} [] k = "SETCCR" -> Flags
             [] OTHER -> {}

Bits == [Flags -> BOOLEAN]

\* f3_cc_flush: materialise the pending op over sr.
Flush(sr, p) ==
    CASE p.op = "NONE"  -> sr
      [] p.op = "LOGIC" -> [f \in Flags |-> IF f = "X" THEN sr.X ELSE IF f = "Z" THEN p.v.Z ELSE FALSE]
      [] p.op = "ARITH" -> p.v
      [] p.op = "CMP"   -> [f \in Flags |-> IF f = "X" THEN sr.X ELSE p.v[f]]

None == [op |-> "NONE", v |-> [f \in Flags |-> FALSE]]

VARIABLES pos, claim, eager, sr, pend, violation, frameDiffers

vars == <<pos, claim, eager, sr, pend, violation, frameDiffers>>

Init ==
    /\ pos = 0
    /\ claim = Flags                      \* entered from the dispatcher, a boundary
    /\ sr \in Bits
    /\ pend \in {None} \cup [op : Ops \ {"NONE"}, v : Bits]
    \* Representation invariant at every dispatcher entry that follows a real
    \* boundary: a pending ADD/SUB carries the X already stored in sr, because X
    \* was live (claim = all flags) when it was produced. TLC found the
    \* counterexample without this conjunct (see README): LOGIC replaces the
    \* pending ADD and the X that lived only in the pending op is lost.
    /\ RepresentationInvariant => (pend.op = "ARITH" => pend.v.X = sr.X)
    /\ eager = Flush(sr, pend)            \* arbitrary pending state on entry
    /\ violation = "none"
    /\ frameDiffers = FALSE

Accepted(k, reads, next) ==
    /\ reads \cup (next \ Writes(k)) \subseteq claim
    /\ RuleCoversIrq => IrqObserves \subseteq next

Lazy == Flush(sr, pend)

\* One instruction of kind k with result bits val and certificate live-out next.
Step(k, reads, val, next) ==
    /\ pos < MaxLength
    /\ Accepted(k, reads, next)
    /\ pos' = pos + 1
    /\ claim' = next
    /\ eager' = CASE k = "LOGIC" -> [f \in Flags |-> IF f = "X" THEN eager.X ELSE IF f = "Z" THEN val.Z ELSE FALSE]
                  [] k = "ARITH" -> val
                  [] k = "CMP" -> [f \in Flags |-> IF f = "X" THEN eager.X ELSE val[f]]
                  [] k = "ZONLY" -> [eager EXCEPT !.Z = val.Z]
                  [] k = "SETCCR" -> val
                  [] OTHER -> eager
    \* What the generated C does (recomp/emitter.py _cc_logic/_cc_cmp/_cc_arith and flush users).
    /\ CASE k = "LOGIC" ->
              /\ sr' = sr
              /\ pend' = IF next \cap ZC # {} THEN [op |-> "LOGIC", v |-> val] ELSE pend
         [] k = "CMP" ->
              /\ sr' = sr
              /\ pend' = IF next \cap ZC # {} THEN [op |-> "CMP", v |-> val] ELSE pend
         [] k = "ARITH" ->
              /\ sr' = IF "X" \in next THEN [sr EXCEPT !.X = val.X] ELSE sr
              /\ pend' = IF next \cap ZC # {} THEN [op |-> "ARITH", v |-> val]
                         ELSE IF "X" \in next /\ ClearStaleOnXOnly THEN None
                         ELSE pend
         [] k = "ZONLY" ->                  \* helper flushes, then changes Z only
              /\ sr' = [Lazy EXCEPT !.Z = val.Z]
              /\ pend' = None
         [] k = "SETCCR" ->
              /\ sr' = val
              /\ pend' = None
         [] OTHER ->
              /\ sr' = sr
              /\ pend' = pend
    /\ violation' = IF violation = "none" /\ k = "READ" /\ \E f \in reads : Lazy[f] # eager[f]
                    THEN "flag read" ELSE violation
    /\ UNCHANGED frameDiffers

Instruction ==
    \E k \in Kinds, val \in Bits, next \in SUBSET Flags :
        IF k = "READ" THEN \E reads \in ReadSets : Step(k, reads, val, next)
        ELSE Step(k, {}, val, next)

\* f3_boundary takes an IRQ: flush, stack SR, handler runs, rte restores the flushed SR.
Interrupt ==
    /\ violation' = IF violation = "none" /\ \E f \in IrqObserves : Lazy[f] # eager[f]
                    THEN "irq handler read" ELSE violation
    /\ frameDiffers' = (frameDiffers \/ Lazy # eager)
    /\ sr' = Lazy
    /\ pend' = None
    /\ UNCHANGED <<pos, claim, eager>>

\* Leaving the graph (rts, computed jump, end of block run): every flag is observable.
Exit ==
    /\ claim = Flags
    /\ pos < MaxLength + 1
    /\ violation' = IF violation = "none" /\ Lazy # eager THEN "boundary" ELSE violation
    /\ pos' = MaxLength + 1
    /\ UNCHANGED <<claim, eager, sr, pend, frameDiffers>>

Next == Instruction \/ Interrupt \/ Exit \/ (pos = MaxLength + 1 /\ UNCHANGED vars)

Spec == Init /\ [][Next]_vars

NoObservableDifference == violation = "none"
\* Stacked IRQ frames carry exactly the eager CCR (expected to fail: elided
\* flags leave stale, dead bits in the stacked SR).
FramesMatchEager == ~frameDiffers
=============================================================================
