package main

import (
	"fmt"
	"regexp"
	"strconv"
	"strings"
)

const (
	mask               = 0xFFFFFFFF
	maxPath            = 32
	returnAddressBytes = 4
)

var affineAdd = map[string]int{"addq": 1, "addi": 1, "add": 1, "subq": -1, "subi": -1, "sub": -1}

var (
	conditionalCycles = regexp.MustCompile(`^\(cpu->pc == 0x[0-9a-f]+u \? (\d+)u : (\d+)u\)$`)
	plainCycles       = regexp.MustCompile(`^(\d+)u$`)
)

type Loop struct {
	Shape      string
	Statements []string
}

type IdleLoops map[int]Loop

func (l IdleLoops) Report() *Fields {
	report := NewFields()
	for _, pc := range sortedKeys(l) {
		report.Set(fmt.Sprintf("0x%06x", pc), l[pc].Shape)
	}
	return report
}

func cycles(insn *Insn) int {
	text := insn.CycleCost()
	match := conditionalCycles.FindStringSubmatch(text)
	if match != nil {
		match = match[1:]
		match[0] = match[1]
	} else if match = plainCycles.FindStringSubmatch(text); match != nil {
		match = match[1:]
	} else {
		panic(fmt.Sprintf("idle: unexpected cycle expression %q", text))
	}
	n, err := strconv.Atoi(match[0])
	if err != nil {
		panic(fmt.Sprintf("idle: cycle expression %q: %v", text, err))
	}
	return n
}

func address(op *Operand, allowStack bool) (string, bool) {
	switch op.AddressMode {
	case AMAbsoluteDataShort:
		value := int(op.Imm) & 0xFFFF
		if value&0x8000 != 0 {
			value -= 0x10000
		}
		return fmt.Sprintf("0x%08xu", value&mask), true
	case AMAbsoluteDataLong:
		return fmt.Sprintf("0x%08xu", int(op.Imm)&mask), true
	case AMRegIAddr:
		register := int(op.Reg) - int(RegA0)
		if !allowStack && register == 7 {
			return "", false
		}
		return fmt.Sprintf("cpu->a[%d]", register), true
	case AMRegIAddrDisp:
		register := int(op.Mem.BaseReg) - int(RegA0)
		if !allowStack && register == 7 {
			return "", false
		}
		return fmt.Sprintf("(uint32_t)(cpu->a[%d] + (int32_t)(%d))", register, op.Mem.Disp), true
	}
	return "", false
}

func target(insn *Insn) (int, bool) {
	flow := insn.Flow()
	if len(flow.Targets) == 1 && !flow.Escapes {
		return flow.Targets[0], true
	}
	return 0, false
}

func spin(insn *Insn) ([]string, bool) {
	if dest, ok := target(insn); insn.Base() != "bra" || !ok || dest != int(insn.Address) {
		return nil, false
	}
	return []string{fmt.Sprintf("F3_IDLE_SPIN(cpu, %du);", cycles(insn))}, true
}

func poll(instructions map[int]*Insn, insn *Insn) ([]string, bool) {
	if insn.Base() != "tst" || len(insn.Operands) != 1 {
		return nil, false
	}
	size := insn.OperandSize(4)
	addr, ok := address(&insn.Operands[0], true)
	closing := instructions[int(insn.Address)+int(insn.Size)]
	if !ok || closing == nil {
		return nil, false
	}
	if dest, ok := target(closing); !ok || dest != int(insn.Address) {
		return nil, false
	}
	mnemonic := closing.Base()
	if cond, ok := condition(mnemonic, "db"); ok && len(closing.Operands) > 0 {
		if counter, ok := dataRegister(&closing.Operands[0]); ok {
			total := cycles(insn) + cycles(closing)
			return []string{fmt.Sprintf("F3_IDLE_POLL_DBCC(cpu, %s, %d, %d, %d, %du);", addr, size, cond, counter, total)}, true
		}
	}
	if cond, ok := condition(mnemonic, "b"); ok && mnemonic != "bra" && mnemonic != "bsr" {
		total := cycles(insn) + cycles(closing)
		return []string{fmt.Sprintf("F3_IDLE_POLL_BCC(cpu, %s, %d, %d, %du);", addr, size, cond, total)}, true
	}
	return nil, false
}

func condition(mnemonic, prefix string) (int, bool) {
	rest, ok := strings.CutPrefix(mnemonic, prefix)
	if !ok {
		return 0, false
	}
	cond, ok := CondMap[rest]
	return cond, ok
}

func callPath(instructions map[int]*Insn, head int) ([]*Insn, int, bool) {
	var path []*Insn
	var stack []int
	depth, pc := 0, head
	for len(path) < maxPath {
		insn := instructions[pc]
		if insn == nil {
			return nil, 0, false
		}
		path = append(path, insn)
		mnemonic := insn.Base()
		flow := insn.Flow()
		switch {
		case mnemonic == "jsr" && len(flow.Targets) == 1:
			stack = append(stack, int(insn.Address)+int(insn.Size))
			depth = max(depth, len(stack))
			pc = flow.Targets[0]
		case mnemonic == "rts" && len(stack) > 0:
			pc = stack[len(stack)-1]
			stack = stack[:len(stack)-1]
		case mnemonic == "bra":
			if dest, ok := target(insn); ok && dest == head && len(stack) == 0 {
				return path, depth, true
			}
			return nil, 0, false
		case len(flow.Targets) > 0 || flow.Escapes:
			return nil, 0, false
		default:
			pc = int(insn.Address) + int(insn.Size)
		}
	}
	return nil, 0, false
}

func dataRegister(op *Operand) (int, bool) {
	if op.Type == OpReg && RegD0 <= op.Reg && op.Reg <= RegD7 {
		return int(op.Reg - RegD0), true
	}
	return 0, false
}

func affine(instructions map[int]*Insn, head int) ([]*Insn, []string, bool) {
	first := instructions[head]
	if first.Base() != "jsr" {
		return nil, nil, false
	}
	path, depth, ok := callPath(instructions, head)
	if !ok {
		return nil, nil, false
	}
	var body []*Insn
	for _, insn := range path {
		switch insn.Base() {
		case "jsr", "rts", "bra":
		default:
			body = append(body, insn)
		}
	}
	if len(body) == 0 {
		return nil, nil, false
	}
	load := body[0]
	if load.Base() != "move" || load.OperandSize(4) != 4 || len(load.Operands) != 2 {
		return nil, nil, false
	}
	register, regOK := dataRegister(&load.Operands[1])
	addr, addrOK := address(&load.Operands[0], false)
	if !regOK || !addrOK {
		return nil, nil, false
	}
	multiplier, addend, index := 1, 0, 1
scan:
	for ; index < len(body); index++ {
		insn := body[index]
		mnemonic, operands := insn.Base(), insn.Operands
		if insn.OperandSize(4) != 4 || len(operands) < 2 {
			break
		}
		if dst, ok := dataRegister(&operands[1]); !ok || dst != register {
			break
		}
		if operands[0].Type != OpImm {
			break
		}
		immediate := int(operands[0].Imm) & mask
		switch sign, isAdd := affineAdd[mnemonic]; {
		case isAdd:
			addend = (addend + sign*immediate) & mask
		case (mnemonic == "muls" || mnemonic == "mulu") && !(len(insn.Bytes) >= 4 && insn.Bytes[2]&0x04 != 0):
			multiplier, addend = multiplier*immediate&mask, addend*immediate&mask
		default:
			break scan
		}
	}
	if index == 1 || index >= len(body) {
		return nil, nil, false
	}
	store := body[index]
	if store.Base() != "move" || store.OperandSize(4) != 4 || len(store.Operands) != 2 {
		return nil, nil, false
	}
	if src, ok := dataRegister(&store.Operands[0]); !ok || src != register {
		return nil, nil, false
	}
	if storeAddr, ok := address(&store.Operands[1], false); !ok || storeAddr != addr {
		return nil, nil, false
	}
	for _, insn := range body[index+1:] {
		if insn.Base() != "swap" {
			return nil, nil, false
		}
		if reg, ok := dataRegister(&insn.Operands[0]); !ok || reg != register {
			return nil, nil, false
		}
	}
	total := 0
	for _, insn := range path {
		total += cycles(insn)
	}
	stack := depth * returnAddressBytes
	return path, []string{fmt.Sprintf("F3_IDLE_AFFINE_CALL(cpu, %s, %du, %du, %du, %du);", addr, stack, multiplier, addend, total)}, true
}

func (a *Analysis) FindIdleLoops(native func(pc int) bool) IdleLoops {
	instructions := a.Instructions
	loops := IdleLoops{}
	for pc, insn := range instructions {
		var shape string
		var path []*Insn
		var statements []string
		if s, ok := spin(insn); ok {
			shape, path, statements = "spin", []*Insn{insn}, s
		} else if s, ok := poll(instructions, insn); ok {
			shape, path, statements = "poll", []*Insn{insn, instructions[pc+int(insn.Size)]}, s
		} else if p, s, ok := affine(instructions, pc); ok {
			shape, path, statements = "affine", p, s
		} else {
			continue
		}
		if allNative(path, native) {
			loops[pc] = Loop{Shape: shape, Statements: statements}
		}
	}
	return loops
}

func allNative(path []*Insn, native func(pc int) bool) bool {
	for _, step := range path {
		if !native(int(step.Address)) {
			return false
		}
	}
	return true
}
