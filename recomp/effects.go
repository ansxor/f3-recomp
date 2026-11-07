package main

import (
	"strings"
)

// FlagEffects is the CCR flags one instruction reads and unconditionally
// writes. Opaque marks instructions whose control flow is not described by
// FlowOf (unknown mnemonic): they end the analysed graph.
type FlagEffects struct {
	Reads  int
	Writes int
	Opaque bool
}

var (
	noFlags  = FlagEffects{}
	readsAll = FlagEffects{Reads: CCRAll}
	opaque   = FlagEffects{Reads: CCRAll, Opaque: true}
	logic    = FlagEffects{Writes: CCRNZVC}
	arith    = FlagEffects{Writes: CCRAll}
	extended = FlagEffects{Reads: CCRX | CCRZ, Writes: CCRAll}
)

var condFlags = [16]int{
	0, 0,
	CCRC | CCRZ, CCRC | CCRZ,
	CCRC, CCRC,
	CCRZ, CCRZ,
	CCRV, CCRV,
	CCRN, CCRN,
	CCRN | CCRV, CCRN | CCRV,
	CCRN | CCRV | CCRZ, CCRN | CCRV | CCRZ,
}

func set(names ...string) map[string]struct{} {
	s := make(map[string]struct{}, len(names))
	for _, n := range names {
		s[n] = struct{}{}
	}
	return s
}

var noFlagMnemonics = set(
	"nop", "bra", "bsr", "jmp", "jsr", "rts", "rtd", "lea", "pea", "link",
	"unlk", "movea", "adda", "suba", "exg", "movem", "movep",
)

var readsAllMnemonics = set(
	"reset", "chk", "chk2", "cmp2", "trapv", "trap", "moves", "movec", "rte",
	"rtr", "stop", "divu", "divs", "illegal",
)

var logicMnemonics = set(
	"moveq", "ext", "extb", "swap", "clr", "tst", "not", "tas", "mulu", "muls",
	"cas", "cas2", "cmp", "cmpa", "cmpi", "cmpm", "bftst", "bfextu", "bfchg",
	"bfexts", "bfclr", "bfffo", "bfset", "bfins",
)

var (
	bitOps      = set("btst", "bset", "bclr", "bchg")
	arithDstAn  = set("add", "addi", "addq", "sub", "subi", "subq")
	logicImm    = set("and", "andi", "or", "ori", "eor", "eori")
	shiftNoX    = set("asl", "asr", "lsl", "lsr")
	rotate      = set("rol", "ror")
	rotateX     = set("roxl", "roxr")
	extendedOps = set("negx", "addx", "subx", "nbcd")
	bcdOps      = set("abcd", "sbcd", "pack", "unpk")
)

func in(s map[string]struct{}, k string) bool {
	_, ok := s[k]
	return ok
}

func ConditionFlags(cond int) int {
	return condFlags[cond&15]
}

func isAReg(op *Operand) bool {
	return op.Type == OpReg && RegA0 <= op.Reg && op.Reg <= RegA7
}

func shiftEffects(mnem string, ops []Operand) FlagEffects {
	counted := len(ops) == 1 || (ops[0].Type == OpImm && ops[0].Imm&63 != 0)
	switch {
	case in(rotate, mnem):
		return logic
	case in(rotateX, mnem):
		if counted {
			return FlagEffects{Reads: CCRX, Writes: CCRAll}
		}
		return FlagEffects{Reads: CCRX, Writes: CCRNZVC}
	}
	if counted {
		return arith
	}
	return FlagEffects{Reads: CCRX, Writes: CCRNZVC}
}

func moveEffects(ops []Operand) FlagEffects {
	if len(ops) < 2 {
		return opaque
	}
	var kinds [2]string
	for i := range kinds {
		op := &ops[i]
		if op.Type != OpReg {
			kinds[i] = "ea"
			continue
		}
		switch {
		case RegD0 <= op.Reg && op.Reg <= RegD7:
			kinds[i] = "d"
		case isAReg(op):
			kinds[i] = "a"
		case op.Reg == RegCCR:
			kinds[i] = "ccr"
		case op.Reg == RegSR:
			kinds[i] = "sr"
		case op.Reg == RegUSP:
			kinds[i] = "usp"
		default:
			return opaque
		}
	}
	source, destination := kinds[0], kinds[1]
	switch {
	case source == "ccr" || source == "sr" || source == "usp" || destination == "sr" || destination == "usp":
		return readsAll
	case destination == "ccr":
		return FlagEffects{Writes: CCRAll}
	case destination == "a":
		return noFlags
	}
	return logic
}

func logicEffects(mnem string, ops []Operand) FlagEffects {
	if len(ops) < 2 {
		return opaque
	}
	destination := &ops[1]
	if destination.Type == OpReg && (destination.Reg == RegCCR || destination.Reg == RegSR) {
		if destination.Reg == RegSR {
			return readsAll
		}
		imm := int(ops[0].Imm) & CCRAll
		switch {
		case strings.Contains(mnem, "eor"):
			return FlagEffects{Reads: imm}
		case strings.Contains(mnem, "and"):
			return FlagEffects{Reads: imm, Writes: ^imm & CCRAll}
		}
		return FlagEffects{Reads: ^imm & CCRAll, Writes: imm}
	}
	return logic
}

func bcdEffects(opcode int) FlagEffects {
	switch opcode & 0xF1F0 {
	case 0xC100, 0x8100:
		return extended
	case 0x8140, 0x8180:
		return noFlags
	}
	return opaque
}

func isCondition(name string) bool {
	_, ok := CondMap[name]
	return ok
}

func condEffects(name string) (FlagEffects, bool) {
	cond, ok := CondMap[name]
	if !ok {
		return FlagEffects{}, false
	}
	return FlagEffects{Reads: condFlags[cond]}, true
}

func (i *Insn) FlagEffects() FlagEffects {
	var opcode int
	for _, b := range i.Bytes[:min(2, len(i.Bytes))] {
		opcode = opcode<<8 | int(b)
	}
	if top := opcode >> 12; top == 10 || top == 15 || opcode == 0x4AFC || opcode&0xFFF8 == 0x4848 {
		return readsAll
	}
	if i.ID == 0 {
		return opaque
	}
	ops := i.Operands
	mnem := i.Base()

	switch {
	case in(noFlagMnemonics, mnem):
		return noFlags
	case in(readsAllMnemonics, mnem):
		return readsAll
	case strings.HasPrefix(mnem, "trap") && isCondition(mnem[4:]):
		return readsAll
	case in(logicMnemonics, mnem):
		return logic
	case mnem == "move":
		return moveEffects(ops)
	case in(arithDstAn, mnem):
		if len(ops) < 2 {
			return opaque
		}
		if isAReg(&ops[1]) {
			return noFlags
		}
		return arith
	case mnem == "neg":
		return arith
	case in(extendedOps, mnem):
		return extended
	case in(bcdOps, mnem):
		return bcdEffects(opcode)
	case in(logicImm, mnem):
		return logicEffects(mnem, ops)
	case in(bitOps, mnem):
		return FlagEffects{Writes: CCRZ}
	case in(shiftNoX, mnem) || in(rotate, mnem) || in(rotateX, mnem):
		if len(ops) == 0 {
			return opaque
		}
		return shiftEffects(mnem, ops)
	}
	if strings.HasPrefix(mnem, "b") {
		if e, ok := condEffects(mnem[1:]); ok {
			return e
		}
	}
	if strings.HasPrefix(mnem, "db") {
		if e, ok := condEffects(mnem[2:]); ok {
			return e
		}
	}
	if mnem == "st" || mnem == "sf" {
		return noFlags
	}
	if strings.HasPrefix(mnem, "s") {
		if e, ok := condEffects(mnem[1:]); ok {
			return e
		}
	}
	return opaque
}
