package main

import (
	"fmt"
	"strings"
)

func lowerLogic(c *lowerCtx) ([]string, bool) {
	ops := c.ops
	if c.mnem == "not" {
		if len(ops) == 0 {
			return nil, false
		}
		dst := decodeEA(c.insn, &ops[0], c.size, "not", false)
		if dst == nil {
			return nil, false
		}
		stmts := append([]string(nil), dst.eaSetup...)
		stmts = append(stmts, dst.readStmts...)
		stmts = append(stmts, fmt.Sprintf("uint32_t not_res = (~%s) & %s;", dst.valExpr, maskLiteral(c.size)))
		stmts = append(stmts, genWrite(dst, "not_res", c.size)...)
		stmts = append(stmts, ccLogic(c.liveOut, "not_res", c.size)...)
		return append(stmts, pcCycles(c)...), true
	}
	if len(ops) < 2 {
		return nil, false
	}
	opSym := "^"
	switch {
	case strings.Contains(c.mnem, "and"):
		opSym = "&"
	case strings.Contains(c.mnem, "or") && !strings.Contains(c.mnem, "eor"):
		opSym = "|"
	}
	if ops[1].Type == OpReg && (ops[1].Reg == RegCCR || ops[1].Reg == RegSR) {
		imm := ops[0].Imm
		stmts := []string{"f3_cc_flush(cpu);"}
		if ops[1].Reg == RegSR {
			stmts = append(stmts, fmt.Sprintf("if (!(cpu->sr & 0x2000u)) { f3_exception(cpu, 8u, 0x%08xu); return; }", c.insn.Address))
		}
		if ops[1].Reg == RegCCR {
			stmts = append(stmts, fmt.Sprintf("cpu->sr = (cpu->sr & 0xff00u) | ((cpu->sr %s 0x%xu) & 0x1fu);", opSym, imm))
		} else {
			stmts = append(stmts, fmt.Sprintf("f3_set_sr(cpu, (uint16_t)(cpu->sr %s 0x%xu));", opSym, imm))
		}
		stmts = append(stmts, "cpu->cc_op = F3_CC_OP_NONE;")
		return append(stmts, pcCycles(c)...), true
	}
	src := decodeEA(c.insn, &ops[0], c.size, "src", false)
	dst := decodeEA(c.insn, &ops[1], c.size, "dst", false)
	if src == nil || dst == nil {
		return nil, false
	}
	stmts := append([]string(nil), src.eaSetup...)
	stmts = append(stmts, src.readStmts...)
	stmts = append(stmts, dst.eaSetup...)
	stmts = append(stmts, dst.readStmts...)
	stmts = append(stmts, fmt.Sprintf("uint32_t log_res = (%s %s %s) & %s;", dst.valExpr, opSym, src.valExpr, maskLiteral(c.size)))
	stmts = append(stmts, genWrite(dst, "log_res", c.size)...)
	stmts = append(stmts, ccLogic(c.liveOut, "log_res", c.size)...)
	return append(stmts, pcCycles(c)...), true
}

func lowerBitOp(c *lowerCtx) ([]string, bool) {
	ops := c.ops
	if len(ops) < 2 {
		return nil, false
	}
	bitSize := 1
	if ops[1].Type == OpReg && isDataReg(ops[1].Reg) {
		bitSize = 4
	}
	var bit string
	switch {
	case ops[0].Type == OpImm:
		bit = fmt.Sprintf("0x%xu", ops[0].Imm)
	case ops[0].Type == OpReg && isDataReg(ops[0].Reg):
		bit = fmt.Sprintf("cpu->d[%d]", dataRegNum(ops[0].Reg))
	default:
		return nil, false
	}
	prefix := "bit"
	if c.mnem == "btst" {
		prefix = "btst"
	}
	dst := decodeEA(c.insn, &ops[1], bitSize, prefix, false)
	if dst == nil {
		return nil, false
	}
	stmts := append([]string(nil), dst.eaSetup...)
	stmts = append(stmts, dst.readStmts...)
	fn := "f3_" + c.mnem
	if c.mnem == "btst" {
		stmts = append(stmts, fmt.Sprintf("%s(cpu, %s, %s, %d);", fn, dst.valExpr, bit, bitSize))
	} else {
		stmts = append(stmts, fmt.Sprintf("uint32_t bit_res = %s(cpu, %s, %s, %d);", fn, dst.valExpr, bit, bitSize))
		stmts = append(stmts, genWrite(dst, "bit_res", bitSize)...)
	}
	return append(stmts, pcCycles(c)...), true
}

func lowerShift(c *lowerCtx) ([]string, bool) {
	ops := c.ops
	if len(ops) == 0 {
		return nil, false
	}
	fn := "f3_" + c.mnem
	if len(ops) == 1 {
		dst := decodeEA(c.insn, &ops[0], 2, "shift", false)
		if dst == nil || !dst.isMem {
			return nil, false
		}
		stmts := append([]string(nil), dst.eaSetup...)
		stmts = append(stmts, dst.readStmts...)
		stmts = append(stmts, fmt.Sprintf("uint32_t shift_res = %s(cpu, %s, 1u, 2);", fn, dst.valExpr))
		stmts = append(stmts, genWrite(dst, "shift_res", 2)...)
		return append(stmts, pcCycles(c)...), true
	}
	var count string
	switch {
	case ops[0].Type == OpImm:
		count = fmt.Sprintf("%du", ops[0].Imm)
	case ops[0].Type == OpReg && isDataReg(ops[0].Reg):
		count = fmt.Sprintf("cpu->d[%d]", dataRegNum(ops[0].Reg))
	default:
		return nil, false
	}
	dst := decodeEA(c.insn, &ops[1], c.size, "shift", false)
	if dst == nil || !dst.isReg || dst.regType != "d" {
		return nil, false
	}
	stmts := append([]string(nil), dst.readStmts...)
	stmts = append(stmts, fmt.Sprintf("uint32_t shift_res = %s(cpu, %s, %s, %d);", fn, dst.valExpr, count, c.size))
	stmts = append(stmts, genWrite(dst, "shift_res", c.size)...)
	return append(stmts, pcCycles(c)...), true
}
