----------------------------- MODULE Dispatch -----------------------------
(***************************************************************************)
(* What the recompiler's output must respect at run time: the dispatcher,  *)
(* chained native transfers, step deadlines, IRQs, idle yields and RAM     *)
(* code whose bytes may change (runtime/cpu_abi.cpp f3_dispatch,           *)
(* runtime/machine.cpp Machine::boundary/next_event, recomp/cpu_ops.h).    *)
(*                                                                         *)
(* Guest program (one main loop, one RAM routine, one IRQ handler):       *)
(*   work : may rewrite the RAM routine's bytes (copy loop, patch)        *)
(*   call : jsr m0                       (ROM -> RAM)                     *)
(*   m0   : may write its own payload    (RAM, self-modifying if allowed) *)
(*   m1   : rts                          (RAM -> ROM poll)                *)
(*   poll : tst.w flag; beq poll         (idle-loop head)                 *)
(*   h0   : flag := 1 ; h1 : rte         (IRQ handler)                    *)
(*                                                                         *)
(* Design switches model the generated-code rules the plan fixes:         *)
(*   ChainIntoRam    a native transfer may enter RAM code without dispatch *)
(*   RamBlockLen     RAM instructions per native function (1 or 2)        *)
(*   GuardBackEdges  every chained transfer tests steps < deadline         *)
(*   IdleSkip        an idle yield is planted at poll                      *)
(***************************************************************************)
EXTENDS Naturals, Sequences

CONSTANTS ChainIntoRam, RamBlockLen, GuardBackEdges, IdleSkip,
          RamSelfWrite, SliceSteps, MaxEvents, MaxSteps

Ram == {"m0", "m1"}
Rom == {"work", "call", "poll", "h0", "h1"}

\* Native function owning each PC (one function per RAM instruction unless RamBlockLen = 2).
Owner(p) == CASE p \in {"work", "call", "poll"} -> "main"
              [] p \in {"h0", "h1"} -> "irq"
              [] p = "m0" -> "ram0"
              [] p = "m1" -> IF RamBlockLen = 2 THEN "ram0" ELSE "ram1"

VARIABLES pc, mode, steps, deadline, sliceEnd, irqPending, masked, stack,
          flag, payload, translated, idle, events, fault

vars == <<pc, mode, steps, deadline, sliceEnd, irqPending, masked, stack,
          flag, payload, translated, idle, events, fault>>

Init ==
    /\ pc = "work"
    /\ mode = "dispatch"
    /\ steps = 0
    /\ deadline = 0
    /\ sliceEnd = SliceSteps
    /\ irqPending = FALSE
    /\ masked = FALSE
    /\ stack = <<>>
    /\ flag = FALSE
    /\ payload = "v1"          \* RAM bytes currently in memory
    /\ translated = "v1"       \* RAM bytes the recompiler translated
    /\ idle = FALSE
    /\ events = 0
    /\ fault = "none"

\* The chain guard the generated C emits before continuing at `to`.
\* Within one function a guarded goto; across functions F3_CHAIN/F3_DISPATCH_PC.
Chainable(from, to) ==
    \/ to \in Rom
    \/ (to \in Ram /\ ChainIntoRam)
    \/ (to \in Ram /\ from \in Ram /\ Owner(from) = Owner(to))
Continue(from, to, guarded) ==
    IF Chainable(from, to) /\ (~guarded \/ steps + 1 < deadline)
    THEN mode' = "native" ELSE mode' = "dispatch"

\* Executing a RAM instruction natively whose translated bytes are stale.
Stale(p) == p \in Ram /\ payload # translated

Exec ==
    /\ mode = "native"
    /\ steps < MaxSteps
    /\ steps' = steps + 1
    /\ fault' = IF fault = "none" /\ Stale(pc) THEN "stale RAM code executed" ELSE fault
    /\ CASE pc = "work" ->
              /\ payload' \in {"v1", "v2"}          \* rewrite the routine (same or other bytes)
              /\ pc' = "call"
              /\ Continue("work", "call", TRUE)
              /\ UNCHANGED <<stack, flag, idle, deadline, masked>>
         [] pc = "call" ->
              /\ pc' = "m0" /\ stack' = Append(stack, "poll")
              /\ Continue("call", "m0", TRUE)
              /\ UNCHANGED <<payload, flag, idle, deadline, masked>>
         [] pc = "m0" ->
              /\ payload' \in (IF RamSelfWrite THEN {"v1", "v2"} ELSE {payload})
              /\ pc' = "m1"
              /\ Continue("m0", "m1", TRUE)
              /\ UNCHANGED <<stack, flag, idle, deadline, masked>>
         [] pc = "m1" ->
              /\ pc' = stack[Len(stack)] /\ stack' = SubSeq(stack, 1, Len(stack) - 1)
              /\ Continue("m1", stack[Len(stack)], TRUE)     \* F3_DISPATCH_PC
              /\ UNCHANGED <<payload, flag, idle, deadline, masked>>
         [] pc = "poll" ->
              IF flag
              THEN /\ flag' = FALSE /\ pc' = "work"
                   /\ Continue("poll", "work", TRUE)
                   /\ UNCHANGED <<payload, stack, idle, deadline, masked>>
              ELSE /\ pc' = "poll"
                   /\ idle' = IdleSkip                         \* F3_IDLE_POLL_BCC
                   /\ deadline' = IF IdleSkip THEN 0 ELSE deadline
                   /\ IF IdleSkip
                      THEN mode' = "dispatch"                  \* guard sees deadline 0
                      ELSE Continue("poll", "poll", GuardBackEdges)
                   /\ UNCHANGED <<payload, stack, flag, masked>>
         [] pc = "h0" ->
              /\ flag' = TRUE /\ pc' = "h1"
              /\ Continue("h0", "h1", TRUE)
              /\ UNCHANGED <<payload, stack, idle, deadline, masked>>
         [] pc = "h1" ->                                       \* rte always returns
              /\ pc' = stack[Len(stack)] /\ stack' = SubSeq(stack, 1, Len(stack) - 1)
              /\ masked' = FALSE /\ mode' = "dispatch"
              /\ UNCHANGED <<payload, flag, idle, deadline>>
    /\ UNCHANGED <<sliceEnd, irqPending, events, translated>>

\* f3_dispatch: f3_boundary, then lookup (RAM entries re-verify their bytes).
Dispatch ==
    /\ mode = "dispatch"
    /\ fault = "none"
    /\ IF irqPending /\ ~masked THEN                         \* take the IRQ
            /\ irqPending' = FALSE /\ masked' = TRUE
            /\ stack' = Append(stack, pc) /\ pc' = "h0"
            /\ deadline' = 0 /\ idle' = FALSE
            /\ UNCHANGED <<mode, steps, sliceEnd, flag, payload, events, fault>>
       ELSE IF idle \/ steps >= sliceEnd THEN                 \* next_event
            /\ events < MaxEvents
            /\ events' = events + 1 /\ irqPending' = TRUE
            /\ sliceEnd' = steps + SliceSteps
            /\ deadline' = 0 /\ idle' = FALSE
            /\ UNCHANGED <<pc, mode, steps, masked, stack, flag, payload, fault>>
       ELSE IF pc \in Ram /\ payload # translated THEN        \* fallback: fatal, but detected
            /\ fault' = "detected stale RAM code"
            /\ UNCHANGED <<pc, mode, steps, deadline, sliceEnd, irqPending, masked, stack,
                           flag, payload, idle, events>>
       ELSE /\ deadline' = sliceEnd /\ mode' = "native"
            /\ UNCHANGED <<pc, steps, sliceEnd, irqPending, masked, stack, flag, payload,
                           idle, events, fault>>
    /\ UNCHANGED translated

Next == Exec \/ Dispatch

Spec == Init /\ [][Next]_vars /\ WF_vars(Next)

\* Native code never runs RAM instructions whose bytes changed since translation.
RamCodeFresh == fault # "stale RAM code executed"
\* Native code never runs past the scheduling slice: device events are not delayed.
NoOverrun == steps <= sliceEnd
\* An unmasked pending IRQ is eventually taken (absent a detected fatal stop).
IrqTaken == (irqPending /\ ~masked) ~> (pc = "h0" \/ fault # "none" \/ steps = MaxSteps)

\* Witnesses: each is expected to be violated, proving the path is reachable.
IdleNeverYields == ~idle
RamNeverRunsNatively == ~(pc \in Ram /\ mode = "native")
HandlerNeverRuns == pc # "h1"
=============================================================================
