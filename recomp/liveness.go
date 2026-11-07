package main

import (
	"maps"
	"slices"
)

// Liveness is per-instruction CCR liveness over a set of emitted instruction
// PCs. LiveOut[pc] is what Lower needs. Effects, Succs and Preds expose
// the graph so an emitter pass can also ask which producers reach a consumer
// (ReachingWriters). Succs is in edge order (static targets, then fallthrough).
type Liveness struct {
	Effects map[int]FlagEffects
	Succs   map[int][]int
	Preds   map[int][]int
	LiveIn  map[int]int
	LiveOut map[int]int
}

// AnalyseLiveness solves flag liveness for the given emitted instruction PCs;
// pinned PCs read every flag (emit-unit hooks and fallback stubs observe a
// flushed sr).
func (a *Analysis) AnalyseLiveness(nodes, pinned []int) *Liveness {
	liveness := NewLiveness()
	liveness.Analyse(a.Instructions, nodes, pinned)
	return liveness
}

// ReachingWriters returns the instructions (ascending) whose write of any of
// flags may reach pc.
//
// It walks predecessors until an instruction that must-write flags is found on
// every path. The flag is open (second result true) if some path reaches an
// instruction with no known predecessor or one that does not write every flag
// asked for: the value then comes from a state this graph cannot see (entry
// from the dispatcher, a return, an unknown caller).
func (l *Liveness) ReachingWriters(pc, flags int) ([]int, bool) {
	type visit struct{ node, wanted int }
	start := l.Preds[pc]
	if len(start) == 0 {
		return nil, true
	}
	writers := map[int]struct{}{}
	open := false
	seen := map[visit]struct{}{}
	stack := make([]visit, 0, len(start))
	for _, pred := range start {
		stack = append(stack, visit{pred, flags})
	}
	for len(stack) > 0 {
		v := stack[len(stack)-1]
		stack = stack[:len(stack)-1]
		if _, ok := seen[v]; ok {
			continue
		}
		seen[v] = struct{}{}
		writes := l.Effects[v.node].Writes
		if writes&v.wanted != 0 {
			writers[v.node] = struct{}{}
		}
		wanted := v.wanted &^ writes
		if wanted == 0 {
			continue
		}
		preds := l.Preds[v.node]
		if len(preds) == 0 {
			open = true
			continue
		}
		for _, pred := range preds {
			stack = append(stack, visit{pred, wanted})
		}
	}
	return slices.Sorted(maps.Keys(writers)), open
}

func NewLiveness() *Liveness { return &Liveness{} }

func (l *Liveness) Analyse(instructions map[int]*Insn, nodes []int, pinned []int) {
	var order []int
	if nodes == nil {
		order = make([]int, 0, len(instructions))
		for pc := range instructions {
			order = append(order, pc)
		}
	} else {
		order = slices.Clone(nodes)
	}
	slices.Sort(order)
	order = slices.Compact(order)
	nodeSet := make(map[int]struct{}, len(order))
	for _, pc := range order {
		nodeSet[pc] = struct{}{}
	}
	pinSet := make(map[int]struct{}, len(pinned))
	for _, pc := range pinned {
		pinSet[pc] = struct{}{}
	}

	*l = Liveness{
		Effects: make(map[int]FlagEffects, len(order)),
		Succs:   make(map[int][]int, len(order)),
		Preds:   make(map[int][]int, len(order)),
		LiveIn:  make(map[int]int, len(order)),
		LiveOut: make(map[int]int, len(order)),
	}
	for _, pc := range order {
		l.Preds[pc] = []int{}
	}
	baseOut := make(map[int]int, len(order))
	for _, pc := range order {
		insn := instructions[pc]
		effect := insn.FlagEffects()
		if _, ok := pinSet[pc]; ok {
			effect.Reads = CCRAll
		}
		l.Effects[pc] = effect
		flow := insn.Flow()
		boundary := flow.Escapes || effect.Opaque
		candidates := slices.Clone(flow.Targets)
		if flow.FallsThrough {
			candidates = append(candidates, pc+int(insn.Size))
		}
		var targets []int
		for _, target := range candidates {
			if _, ok := nodeSet[target]; !ok {
				boundary = true
			} else if !slices.Contains(targets, target) {
				targets = append(targets, target)
			}
		}
		l.Succs[pc] = targets
		if boundary {
			baseOut[pc] = CCRAll
		}
		for _, target := range targets {
			l.Preds[target] = append(l.Preds[target], pc)
		}
	}

	for _, pc := range order {
		l.LiveIn[pc] = l.Effects[pc].Reads
		l.LiveOut[pc] = baseOut[pc]
	}
	work := make([]int, len(order))
	for i, pc := range order {
		work[len(order)-1-i] = pc
	}
	queued := make(map[int]struct{}, len(order))
	for _, pc := range order {
		queued[pc] = struct{}{}
	}
	for len(work) > 0 {
		pc := work[0]
		work = work[1:]
		delete(queued, pc)
		out := baseOut[pc]
		for _, target := range l.Succs[pc] {
			out |= l.LiveIn[target]
		}
		l.LiveOut[pc] = out
		effect := l.Effects[pc]
		newIn := effect.Reads | (out &^ effect.Writes)
		if newIn != l.LiveIn[pc] {
			l.LiveIn[pc] = newIn
			for _, pred := range l.Preds[pc] {
				if _, ok := queued[pred]; !ok {
					queued[pred] = struct{}{}
					work = append(work, pred)
				}
			}
		}
	}
}
