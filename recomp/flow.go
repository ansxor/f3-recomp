package main

import (
	"strings"
)

// StaticFlow lists the successor PCs an instruction can reach, for block chaining.
//
// Targets are non-sequential successors (branch/jump destinations) that are
// statically known from the opcode. FallsThrough reports whether the
// sequential next instruction is also a possible successor. An indirect
// transfer reports neither, which keeps it out of any chain. Escapes is set
// when control may also reach an address that is not named here (computed jump
// or call, return, malformed branch operands).
type StaticFlow struct {
	Targets      []int
	FallsThrough bool
	Escapes      bool
}

func staticFlow(targets []int, falls, escapes bool) StaticFlow {
	masked := make([]int, len(targets))
	for i, t := range targets {
		masked[i] = t & 0xFFFFFFFF
	}
	return StaticFlow{Targets: masked, FallsThrough: falls, Escapes: escapes}
}

func staticEATarget(insn *Insn, op *Operand) (target int, ok bool) {
	switch op.AddressMode {
	case AMAbsoluteDataShort:
		return int(int16(op.Imm)) & 0xFFFFFFFF, true
	case AMAbsoluteDataLong:
		return int(op.Imm) & 0xFFFFFFFF, true
	case AMPCIDisp:
		return (insn.PCBase() + int(op.Mem.Disp)) & 0xFFFFFFFF, true
	}
	return 0, false
}

func (i *Insn) Flow() StaticFlow {
	mnem := i.Base()
	if i.ID == 0 {
		return staticFlow(nil, true, true)
	}
	ops := i.Operands
	branchTarget := func(n int) int { return int(i.Address) + 2 + int(ops[n].BrDisp.Disp) }
	if mnem == "bra" || mnem == "bsr" {
		if len(ops) > 0 && ops[0].Type == OpBrDisp {
			return staticFlow([]int{branchTarget(0)}, false, false)
		}
		return staticFlow(nil, false, true)
	}
	if rest, ok := strings.CutPrefix(mnem, "b"); ok {
		if _, isCond := CondMap[rest]; isCond {
			if len(ops) > 0 && ops[0].Type == OpBrDisp {
				return staticFlow([]int{branchTarget(0)}, true, false)
			}
			return staticFlow(nil, true, true)
		}
	}
	if condName, ok := strings.CutPrefix(mnem, "db"); ok {
		if _, isCond := CondMap[condName]; isCond && len(ops) >= 2 && ops[1].Type == OpBrDisp {
			return staticFlow([]int{branchTarget(1)}, true, false)
		}
		return staticFlow(nil, true, true)
	}
	switch mnem {
	case "jmp", "jsr":
		if len(ops) > 0 {
			if target, ok := staticEATarget(i, &ops[0]); ok {
				return staticFlow([]int{target}, false, false)
			}
		}
		return staticFlow(nil, false, true)
	case "rts", "rtd", "rtr", "rte":
		return staticFlow(nil, false, true)
	}
	return staticFlow(nil, true, false)
}
