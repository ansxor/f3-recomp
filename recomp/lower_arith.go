package main

import (
	"fmt"
)

func lowerAddSub(c *lowerCtx, name string) ([]string, bool) {
	ops := c.ops
	if len(ops) < 2 {
		return nil, false
	}
	sign, upper := "+", "ADD"
	if name == "sub" {
		sign, upper = "-", "SUB"
	}
	toAddr := c.mnem == name+"a" || (ops[1].Type == OpReg && isAddrReg(ops[1].Reg))
	src := decodeEA(c.insn, &ops[0], c.size, "src", false)
	if toAddr {
		if src == nil {
			return nil, false
		}
		dst := addrRegNum(ops[1].Reg)
		stmts := append([]string(nil), src.eaSetup...)
		stmts = append(stmts, src.readStmts...)
		stmts = append(stmts, fmt.Sprintf("cpu->a[%d] = (uint32_t)(cpu->a[%d] %s %s(%s));", dst, dst, sign, addrOperandCast(c.size), src.valExpr))
		return append(stmts, pcCycles(c)...), true
	}
	dst := decodeEA(c.insn, &ops[1], c.size, "dst", false)
	if src == nil || dst == nil {
		return nil, false
	}
	stmts := append([]string(nil), src.eaSetup...)
	stmts = append(stmts, src.readStmts...)
	stmts = append(stmts, dst.eaSetup...)
	stmts = append(stmts, dst.readStmts...)
	mask := maskLiteral(c.size)
	var carry string
	if name == "add" {
		carry = fmt.Sprintf("(add_res & %s) < (add_src & %s)", mask, mask)
	} else {
		carry = fmt.Sprintf("(sub_dst & %s) < (sub_src & %s)", mask, mask)
	}
	stmts = append(stmts,
		fmt.Sprintf("uint32_t %s_src = %s;", name, src.valExpr),
		fmt.Sprintf("uint32_t %s_dst = %s;", name, dst.valExpr),
		fmt.Sprintf("uint32_t %s_res = (%s_dst %s %s_src) & %s;", name, name, sign, name, mask),
	)
	stmts = append(stmts, genWrite(dst, name+"_res", c.size)...)
	stmts = append(stmts, ccArith(c.liveOut, upper, name+"_src", name+"_dst", name+"_res", c.size, carry)...)
	return append(stmts, pcCycles(c)...), true
}

func lowerAdd(c *lowerCtx) ([]string, bool) { return lowerAddSub(c, "add") }

func lowerSub(c *lowerCtx) ([]string, bool) { return lowerAddSub(c, "sub") }

// lowerCmpm decodes the primary word because Capstone describes CMPM's
// postincrement operands as registers. The source is read and incremented before
// the destination so an aliased address register consumes consecutive values.
func lowerCmpm(c *lowerCtx) ([]string, bool) {
	source, destination := c.opcode&7, (c.opcode>>9)&7
	sourceStep, destinationStep := c.size, c.size
	if c.size == 1 {
		if source == 7 {
			sourceStep = 2
		}
		if destination == 7 {
			destinationStep = 2
		}
	}
	bits := c.size * 8
	if c.liveOut&CCRNZVC == 0 {
		return []string{
			fmt.Sprintf("(void)f3_read%d(cpu, cpu->a[%d]);", bits, source),
			fmt.Sprintf("cpu->a[%d] += %du;", source, sourceStep),
			fmt.Sprintf("(void)f3_read%d(cpu, cpu->a[%d]);", bits, destination),
			fmt.Sprintf("cpu->a[%d] += %du;", destination, destinationStep),
			fmt.Sprintf("cpu->pc = 0x%08xu;", c.nextPC),
			fmt.Sprintf("cpu->cycles += %s;", c.cycles),
		}, true
	}
	stmts := []string{
		fmt.Sprintf("uint32_t cmp_src = f3_read%d(cpu, cpu->a[%d]);", bits, source),
		fmt.Sprintf("cpu->a[%d] += %du;", source, sourceStep),
		fmt.Sprintf("uint32_t cmp_dst = f3_read%d(cpu, cpu->a[%d]);", bits, destination),
		fmt.Sprintf("cpu->a[%d] += %du;", destination, destinationStep),
		fmt.Sprintf("uint32_t cmp_res = (cmp_dst - cmp_src) & %s;", maskLiteral(c.size)),
	}
	stmts = append(stmts, ccCmp(c.liveOut, "cmp_src", "cmp_dst", "cmp_res", c.size)...)
	return append(stmts, pcCycles(c)...), true
}

func lowerCmp(c *lowerCtx) ([]string, bool) {
	ops := c.ops
	if len(ops) < 2 {
		return nil, false
	}
	toAddr := c.mnem == "cmpa" || (ops[1].Type == OpReg && isAddrReg(ops[1].Reg))
	if toAddr {
		src := decodeEA(c.insn, &ops[0], c.size, "src", false)
		if src == nil {
			return nil, false
		}
		dst := addrRegNum(ops[1].Reg)
		stmts := append([]string(nil), src.eaSetup...)
		stmts = append(stmts, src.readStmts...)
		if c.liveOut&CCRNZVC != 0 {
			stmts = append(stmts,
				fmt.Sprintf("uint32_t cmp_src = %s(%s);", addrOperandCast(c.size), src.valExpr),
				fmt.Sprintf("uint32_t cmp_dst = cpu->a[%d];", dst),
				"uint32_t cmp_res = cmp_dst - cmp_src;",
			)
			stmts = append(stmts, ccCmp(c.liveOut, "cmp_src", "cmp_dst", "cmp_res", 4)...)
		} else {
			stmts = append(stmts, discardValue(src)...)
		}
		return append(stmts, pcCycles(c)...), true
	}
	src := decodeEA(c.insn, &ops[0], c.size, "src", false)
	dst := decodeEA(c.insn, &ops[1], c.size, "cmp_dst", false)
	if src == nil || dst == nil {
		return nil, false
	}
	stmts := append([]string(nil), src.eaSetup...)
	stmts = append(stmts, src.readStmts...)
	stmts = append(stmts, dst.eaSetup...)
	stmts = append(stmts, dst.readStmts...)
	if c.liveOut&CCRNZVC != 0 {
		stmts = append(stmts,
			fmt.Sprintf("uint32_t cmp_src = %s;", src.valExpr),
			fmt.Sprintf("uint32_t cmp_dst = %s;", dst.valExpr),
			fmt.Sprintf("uint32_t cmp_res = (cmp_dst - cmp_src) & %s;", maskLiteral(c.size)),
		)
		stmts = append(stmts, ccCmp(c.liveOut, "cmp_src", "cmp_dst", "cmp_res", c.size)...)
	} else {
		stmts = append(stmts, discardValue(src)...)
		stmts = append(stmts, discardValue(dst)...)
	}
	return append(stmts, pcCycles(c)...), true
}

const condFlagsToSR = "cpu->sr = (cpu->sr & ~0x1fu) | (c ? 0x11u : 0u) | (n ? 0x08u : 0u) | (z ? 0x04u : 0u) | (v ? 0x02u : 0u);"

func lowerNeg(c *lowerCtx) ([]string, bool) {
	if len(c.ops) == 0 {
		return nil, false
	}
	dst := decodeEA(c.insn, &c.ops[0], c.size, "neg", false)
	if dst == nil {
		return nil, false
	}
	stmts := append([]string(nil), dst.eaSetup...)
	stmts = append(stmts, dst.readStmts...)
	mask, sign := maskLiteral(c.size), signLiteral(c.size)
	if c.mnem == "neg" {
		stmts = append(stmts,
			fmt.Sprintf("uint32_t neg_src = %s;", dst.valExpr),
			fmt.Sprintf("uint32_t neg_res = (0u - neg_src) & %s;", mask),
		)
		stmts = append(stmts, genWrite(dst, "neg_res", c.size)...)
		stmts = append(stmts, ccArith(c.liveOut, "SUB", "neg_src", "0u", "neg_res", c.size, fmt.Sprintf("(neg_src & %s) != 0u", mask))...)
	} else {
		stmts = append(stmts,
			"f3_cc_flush(cpu);",
			"uint32_t neg_x = (cpu->sr >> 4) & 1u;",
			"uint32_t prev_z = (cpu->sr >> 2) & 1u;",
			fmt.Sprintf("uint32_t neg_src = %s;", dst.valExpr),
			fmt.Sprintf("uint32_t neg_res = (0u - neg_src - neg_x) & %s;", mask),
		)
		stmts = append(stmts, genWrite(dst, "neg_res", c.size)...)
		stmts = append(stmts,
			fmt.Sprintf("int c = ((uint64_t)(neg_src & %s) + neg_x) > 0;", mask),
			fmt.Sprintf("int v = ((neg_src & neg_res & %s) != 0);", sign),
			fmt.Sprintf("int n = (neg_res & %s) != 0;", sign),
			"int z = (neg_res == 0) ? (int)prev_z : 0;",
			condFlagsToSR,
			"cpu->cc_op = F3_CC_OP_NONE;",
		)
	}
	return append(stmts, pcCycles(c)...), true
}

func lowerAddxSubx(c *lowerCtx) ([]string, bool) {
	ops := c.ops
	if len(ops) < 2 {
		return nil, false
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
	mask, sign := maskLiteral(c.size), signLiteral(c.size)
	stmts = append(stmts,
		"f3_cc_flush(cpu);",
		"uint32_t ext_x = (cpu->sr >> 4) & 1u;",
		"uint32_t prev_z = (cpu->sr >> 2) & 1u;",
		fmt.Sprintf("uint32_t op_src = %s;", src.valExpr),
		fmt.Sprintf("uint32_t op_dst = %s;", dst.valExpr),
	)
	if c.mnem == "addx" {
		stmts = append(stmts,
			fmt.Sprintf("uint64_t full = (uint64_t)(op_dst & %s) + (uint64_t)(op_src & %s) + ext_x;", mask, mask),
			fmt.Sprintf("uint32_t op_res = (uint32_t)full & %s;", mask),
		)
		stmts = append(stmts, genWrite(dst, "op_res", c.size)...)
		stmts = append(stmts,
			fmt.Sprintf("int c = full > %s;", mask),
			fmt.Sprintf("int v = ((op_src ^ op_res) & (op_dst ^ op_res) & %s) != 0;", sign),
		)
	} else {
		stmts = append(stmts,
			fmt.Sprintf("int c = (op_dst & %s) < ((uint64_t)(op_src & %s) + ext_x);", mask, mask),
			fmt.Sprintf("uint32_t op_res = (op_dst - op_src - ext_x) & %s;", mask),
		)
		stmts = append(stmts, genWrite(dst, "op_res", c.size)...)
		stmts = append(stmts, fmt.Sprintf("int v = ((op_dst ^ op_src) & (op_dst ^ op_res) & %s) != 0;", sign))
	}
	stmts = append(stmts,
		fmt.Sprintf("int n = (op_res & %s) != 0;", sign),
		"int z = (op_res == 0) ? (int)prev_z : 0;",
		condFlagsToSR,
		"cpu->cc_op = F3_CC_OP_NONE;",
	)
	return append(stmts, pcCycles(c)...), true
}

func lowerMul(c *lowerCtx) ([]string, bool) {
	ops := c.ops
	if len(ops) < 2 {
		return nil, false
	}
	signed := c.mnem == "muls"
	suffix := "l"
	if c.size == 2 {
		suffix = "w"
	}
	fn := fmt.Sprintf("f3_%s_%s", c.mnem, suffix)
	src := decodeEA(c.insn, &ops[0], c.size, "src", false)
	if src == nil {
		return nil, false
	}
	stmts := append([]string(nil), src.eaSetup...)
	stmts = append(stmts, src.readStmts...)
	if c.size == 2 {
		if ops[1].Type != OpReg || !isDataReg(ops[1].Reg) {
			return nil, false
		}
		dst := dataRegNum(ops[1].Reg)
		cast := "uint16_t"
		if signed {
			cast = "int16_t"
		}
		stmts = append(stmts, fmt.Sprintf("cpu->d[%d] = %s(cpu, (%s)(%s), (%s)(cpu->d[%d] & 0xffffu));", dst, fn, cast, src.valExpr, cast, dst))
	} else {
		cast := "uint32_t"
		if signed {
			cast = "int32_t"
		}
		switch {
		case ops[1].Type == OpRegPair:
			hi, lo := dataRegNum(ops[1].RegPair[0]), dataRegNum(ops[1].RegPair[1])
			stmts = append(stmts,
				"uint32_t prod_hi = 0;",
				fmt.Sprintf("uint32_t prod_lo = %s(cpu, (%s)(%s), (%s)cpu->d[%d], &prod_hi);", fn, cast, src.valExpr, cast, lo),
				fmt.Sprintf("cpu->d[%d] = prod_hi;", hi),
				fmt.Sprintf("cpu->d[%d] = prod_lo;", lo),
			)
		case ops[1].Type == OpReg && isDataReg(ops[1].Reg):
			dst := dataRegNum(ops[1].Reg)
			stmts = append(stmts, fmt.Sprintf("cpu->d[%d] = %s(cpu, (%s)(%s), (%s)cpu->d[%d], NULL);", dst, fn, cast, src.valExpr, cast, dst))
		default:
			return nil, false
		}
	}
	return append(stmts, pcCycles(c)...), true
}

func beBytes(b []byte, lo, hi int) int {
	lo = min(lo, len(b))
	hi = min(hi, len(b))
	v := 0
	for _, x := range b[lo:hi] {
		v = v<<8 | int(x)
	}
	return v
}

func lowerDiv(c *lowerCtx) ([]string, bool) {
	ops := c.ops
	if len(ops) < 2 {
		return nil, false
	}
	signed := c.mnem == "divs"
	suffix := "l"
	if c.size == 2 {
		suffix = "w"
	}
	fn := fmt.Sprintf("f3_%s_%s", c.mnem, suffix)
	src := decodeEA(c.insn, &ops[0], c.size, "src", false)
	if src == nil {
		return nil, false
	}
	stmts := append([]string(nil), src.eaSetup...)
	stmts = append(stmts, src.readStmts...)
	stmts = append(stmts, "int div_ex = 0;")
	if c.size == 2 {
		if ops[1].Type != OpReg || !isDataReg(ops[1].Reg) {
			return nil, false
		}
		dst := dataRegNum(ops[1].Reg)
		cast := "uint16_t"
		if signed {
			cast = "int16_t"
		}
		stmts = append(stmts,
			fmt.Sprintf("cpu->d[%d] = %s(cpu, (%s)(%s), cpu->d[%d], 0x%08xu, &div_ex);", dst, fn, cast, src.valExpr, dst, c.nextPC),
			"if (div_ex) return;",
		)
	} else {
		ext := beBytes(c.insn.Bytes, 2, 4)
		quot, rem := (ext>>12)&7, ext&7
		is64 := 0
		if ext&0x0400 != 0 {
			is64 = 1
		}
		cast := "uint32_t"
		if signed {
			cast = "int32_t"
		}
		stmts = append(stmts,
			fmt.Sprintf("uint32_t div_rem = cpu->d[%d];", rem),
			fmt.Sprintf("uint32_t div_quot = %s(cpu, (%s)(%s), cpu->d[%d], cpu->d[%d], %d, 0x%08xu, &div_rem, &div_ex);", fn, cast, src.valExpr, quot, rem, is64, c.nextPC),
			"if (div_ex) return;",
			fmt.Sprintf("cpu->d[%d] = div_rem;", rem),
			fmt.Sprintf("cpu->d[%d] = div_quot;", quot),
		)
	}
	return append(stmts, pcCycles(c)...), true
}
