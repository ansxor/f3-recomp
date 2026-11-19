/*
 * Lazy-CCR liveness: is eliding a flag producer sound, and does the local
 * certificate checker accept exactly the sound elisions?
 *
 * A Node is one emitted instruction. `succ` is the static graph the recompiler
 * builds (fallthrough, direct branches, a direct call flowing into its callee).
 * A Boundary node may transfer control somewhere the graph does not name: rts,
 * rte, computed jmp/jsr, a successor that is not emitted natively, a fallback or
 * hook PC. After a boundary, unknown code may read every flag.
 *
 * Messy-assembly features are not special cases here; they are graph shapes the
 * solver enumerates: shared tails (two nodes with one successor), overlapping
 * decodes and aliased entry points (nodes with no graph predecessor, entered
 * only from a boundary), flags returned across rts (callee writes, rts is a
 * boundary), loops, nodes that are both readers and writers.
 *
 * Interrupts are the one edge the Python rule ignores: an IRQ can be taken after
 * any instruction (native code yields at every instruction boundary once the
 * deadline passes). The handler starts with the interrupted CCR and the frame
 * stacks it, so `Irq.observes` is what handlers read before writing plus what
 * any code later reads back out of a stacked frame.
 *
 * `writes` is a must-write set: a conditional write is modelled as a read.
 */
module liveness

abstract sig Flag {}
one sig X, N, Z, V, C extends Flag {}

sig Node {
  reads: set Flag,
  writes: set Flag,
  succ: set Node
}
sig Boundary in Node {}

one sig Irq { observes: set Flag }

-- Python's rule (recomp/liveness.py): some fixpoint of the dataflow equations.
one sig Py { out: Node -> Flag }
-- The proposed rule adds the interrupt observer to every node.
one sig Ocaml { out: Node -> Flag }
-- An arbitrary candidate certificate handed to the checker.
one sig Cert { out: Node -> Flag }

fun liveIn[o: Node -> Flag, s: Node]: set Flag { s.reads + (s.o - s.writes) }

pred pythonFixpoint {
  all n: Node | n.(Py.out) =
    (n in Boundary implies Flag else none) + { f: Flag | some s: n.succ | f in liveIn[Py.out, s] }
}
pred ocamlFixpoint {
  all n: Node | n.(Ocaml.out) =
    (n in Boundary implies Flag else none) + Irq.observes +
    { f: Flag | some s: n.succ | f in liveIn[Ocaml.out, s] }
}

-- The certificate checker: local post-fixpoint inequalities only, no solving.
pred accepted[o: Node -> Flag] {
  all n: Node {
    n in Boundary implies Flag in n.o
    Irq.observes in n.o
    all s: n.succ | liveIn[o, s] in n.o
  }
}

-- Ground truth over executions. After n, f is observed if an IRQ handler or a
-- frame reader sees it, if n leaves the graph, or if some path through nodes
-- that do not must-write f reaches a reader of f or a boundary that passes it.
fun passes[f: Flag]: set Node { { m: Node | f not in m.writes } }
fun observers[f: Flag]: set Node {
  { m: Node | f in m.reads or (m in Boundary and f not in m.writes) }
}
pred trulyLive[n: Node, f: Flag] {
  n in Boundary
  or f in Irq.observes
  or some (n.succ.*(passes[f] <: succ) & observers[f])
}

-- Elision: n writes f and the certificate says f is dead after n.
pred unsoundElision[o: Node -> Flag] {
  some n: Node, f: n.writes | f not in n.o and trulyLive[n, f]
}

-- 1. Counterexample expected: Python's fixpoint ignores interrupt observers.
PythonRuleSound: check { pythonFixpoint implies not unsoundElision[Py.out] } for 5 Node

-- 1b. With no interrupt observer, Python's rule is sound (all shapes, scope 6).
PythonRuleSoundWithoutIrqObservers: check {
  (pythonFixpoint and no Irq.observes) implies not unsoundElision[Py.out]
} for 6 Node

-- 2. The proposed rule is sound for every graph shape.
OcamlRuleSound: check { ocamlFixpoint implies not unsoundElision[Ocaml.out] } for 6 Node

-- 3. Checker soundness: anything it accepts elides only dead writes.
CheckerSound: check { accepted[Cert.out] implies not unsoundElision[Cert.out] } for 6 Node

-- 4. Checker completeness: the exact truth (the least solution) is accepted,
--    so the checker never rejects the most precise sound result.
CheckerAcceptsTruth: check {
  (all n: Node, f: Flag | f in n.(Cert.out) iff trulyLive[n, f]) implies accepted[Cert.out]
} for 6 Node

-- 5. Any sound elision set is certifiable: keeping more flags live in a node's
--    own write set never breaks acceptance (so "elide less" is always checkable).
WideningOwnWritesStaysAccepted: check {
  all n: Node, f: n.writes |
    accepted[Cert.out] implies accepted[Cert.out + n -> f]
} for 6 Node

-- 6. The solver result the checker sees is accepted (solver/checker agree).
OcamlFixpointAccepted: check { ocamlFixpoint implies accepted[Ocaml.out] } for 6 Node

-- Witness runs for the shapes named in the header (all must be satisfiable).
SharedTail: run {
  ocamlFixpoint
  some disj a, b, t: Node | t in a.succ and t in b.succ and a not in Boundary and b not in Boundary
} for 4 Node
FlagsAcrossRts: run {
  ocamlFixpoint
  some disj call, callee, rts: Node |
    call.succ = callee and rts in callee.*succ and rts in Boundary and some callee.writes
} for 4 Node
AliasedEntry: run {
  ocamlFixpoint
  some e: Node | no succ.e and e not in Boundary and some e.writes and some e.succ
} for 4 Node
