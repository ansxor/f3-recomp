package main

import (
	"fmt"
	"strings"
)

func lowerNop(c *lowerCtx) ([]string, bool) {
	return pcCycles(c), true
}

func lowerBra(c *lowerCtx) ([]string, bool) {
	if len(c.ops) == 0 || c.ops[0].Type != OpBrDisp {
		return nil, false
	}
	target := branchTarget(c, &c.ops[0])
	return []string{
		fmt.Sprintf("cpu->pc = 0x%08xu;", target),
		fmt.Sprintf("cpu->cycles += %s;", c.cycles),
	}, true
}

func lowerBsr(c *lowerCtx) ([]string, bool) {
	if len(c.ops) == 0 || c.ops[0].Type != OpBrDisp {
		return nil, false
	}
	target := branchTarget(c, &c.ops[0])
	return []string{
		"cpu->a[7] -= 4u;",
		fmt.Sprintf("f3_write32(cpu, cpu->a[7], 0x%08xu);", c.nextPC),
		fmt.Sprintf("cpu->pc = 0x%08xu;", target),
		fmt.Sprintf("cpu->cycles += %s;", c.cycles),
	}, true
}

func matchBcc(c *lowerCtx) bool {
	if !strings.HasPrefix(c.mnem, "b") {
		return false
	}
	_, ok := CondMap[c.mnem[1:]]
	return ok
}

func lowerBcc(c *lowerCtx) ([]string, bool) {
	cond := CondMap[c.mnem[1:]]
	if len(c.ops) == 0 || c.ops[0].Type != OpBrDisp {
		return nil, false
	}
	target := branchTarget(c, &c.ops[0])
	return []string{
		fmt.Sprintf("if (f3_eval_cond(cpu, %d)) { cpu->pc = 0x%08xu; } else { cpu->pc = 0x%08xu; }", cond, target, c.nextPC),
		fmt.Sprintf("cpu->cycles += %s;", c.cycles),
	}, true
}

func matchDBcc(c *lowerCtx) bool { return strings.HasPrefix(c.mnem, "db") }

func lowerDBcc(c *lowerCtx) ([]string, bool) {
	cond, ok := CondMap[c.mnem[2:]]
	if !ok || len(c.ops) < 2 {
		return nil, false
	}
	if c.ops[0].Type != OpReg || c.ops[1].Type != OpBrDisp {
		return nil, false
	}
	reg := dataRegNum(c.ops[0].Reg)
	target := branchTarget(c, &c.ops[1])
	return []string{
		fmt.Sprintf("if (!f3_eval_cond(cpu, %d)) {", cond),
		fmt.Sprintf("    int16_t cnt = (int16_t)(cpu->d[%d] & 0xffffu) - 1;", reg),
		fmt.Sprintf("    cpu->d[%d] = (cpu->d[%d] & 0xffff0000u) | ((uint16_t)cnt & 0xffffu);", reg, reg),
		fmt.Sprintf("    if (cnt != -1) { cpu->pc = 0x%08xu; } else { cpu->pc = 0x%08xu; cpu->cycles += 4u; }", target, c.nextPC),
		"} else {",
		fmt.Sprintf("    cpu->pc = 0x%08xu;", c.nextPC),
		"}",
		fmt.Sprintf("cpu->cycles += %s;", c.cycles),
	}, true
}

func matchScc(c *lowerCtx) bool {
	if !strings.HasPrefix(c.mnem, "s") {
		return false
	}
	_, ok := CondMap[c.mnem[1:]]
	return ok || c.mnem == "st" || c.mnem == "sf"
}

func lowerScc(c *lowerCtx) ([]string, bool) {
	condName := c.mnem[1:]
	cond := CondMap[condName]
	if len(c.ops) == 0 {
		return nil, false
	}
	dst := decodeEA(c.insn, &c.ops[0], 1, "dst", false)
	if dst == nil {
		return nil, false
	}
	var value string
	switch condName {
	case "t":
		value = "0xffu"
	case "f":
		value = "0x00u"
	default:
		value = fmt.Sprintf("(f3_eval_cond(cpu, %d) ? 0xffu : 0x00u)", cond)
	}
	stmts := append([]string(nil), dst.eaSetup...)
	stmts = append(stmts, genWrite(dst, value, 1)...)
	return append(stmts, pcCycles(c)...), true
}

func lowerJmp(c *lowerCtx) ([]string, bool) {
	if len(c.ops) == 0 {
		return nil, false
	}
	target := decodeEA(c.insn, &c.ops[0], 4, "jmp", false)
	if target == nil || !target.isMem {
		return nil, false
	}
	stmts := append([]string(nil), target.eaSetup...)
	return append(stmts,
		fmt.Sprintf("cpu->pc = %s;", target.eaExpr),
		fmt.Sprintf("cpu->cycles += %s;", c.cycles),
	), true
}

func lowerJsr(c *lowerCtx) ([]string, bool) {
	if len(c.ops) == 0 {
		return nil, false
	}
	target := decodeEA(c.insn, &c.ops[0], 4, "jsr", false)
	if target == nil || !target.isMem {
		return nil, false
	}
	stmts := append([]string(nil), target.eaSetup...)
	return append(stmts,
		fmt.Sprintf("uint32_t jsr_target = %s;", target.eaExpr),
		"cpu->a[7] -= 4u;",
		fmt.Sprintf("f3_write32(cpu, cpu->a[7], 0x%08xu);", c.nextPC),
		"cpu->pc = jsr_target;",
		fmt.Sprintf("cpu->cycles += %s;", c.cycles),
	), true
}

func lowerRts(c *lowerCtx) ([]string, bool) {
	return []string{
		"cpu->pc = f3_read32(cpu, cpu->a[7]);",
		"cpu->a[7] += 4u;",
		fmt.Sprintf("cpu->cycles += %s;", c.cycles),
		"f3_cc_flush(cpu);",
		"return;",
	}, true
}

func lowerRtd(c *lowerCtx) ([]string, bool) {
	var disp int64
	if len(c.ops) > 0 {
		disp = c.ops[0].Imm
	}
	return []string{
		"cpu->pc = f3_read32(cpu, cpu->a[7]);",
		fmt.Sprintf("cpu->a[7] = (uint32_t)(cpu->a[7] + 4u + (int32_t)(int16_t)(%d));", disp),
		fmt.Sprintf("cpu->cycles += %s;", c.cycles),
		"f3_cc_flush(cpu);",
		"return;",
	}, true
}

func lowerRtr(c *lowerCtx) ([]string, bool) {
	return []string{
		"f3_cc_flush(cpu);",
		"uint16_t rtr_ccr = f3_read16(cpu, cpu->a[7]);",
		"cpu->a[7] += 2u;",
		"cpu->sr = (cpu->sr & 0xff00u) | (rtr_ccr & 0x1fu);",
		"cpu->cc_op = F3_CC_OP_NONE;",
		"cpu->pc = f3_read32(cpu, cpu->a[7]);",
		"cpu->a[7] += 4u;",
		fmt.Sprintf("cpu->cycles += %s;", c.cycles),
		"return;",
	}, true
}

func lowerLea(c *lowerCtx) ([]string, bool) {
	if len(c.ops) < 2 {
		return nil, false
	}
	src := decodeEA(c.insn, &c.ops[0], 4, "lea", false)
	if src == nil || !src.isMem || c.ops[1].Type != OpReg {
		return nil, false
	}
	stmts := append([]string(nil), src.eaSetup...)
	stmts = append(stmts, fmt.Sprintf("cpu->a[%d] = %s;", addrRegNum(c.ops[1].Reg), src.eaExpr))
	return append(stmts, pcCycles(c)...), true
}

func lowerPea(c *lowerCtx) ([]string, bool) {
	if len(c.ops) == 0 {
		return nil, false
	}
	src := decodeEA(c.insn, &c.ops[0], 4, "pea", false)
	if src == nil || !src.isMem {
		return nil, false
	}
	stmts := append([]string(nil), src.eaSetup...)
	stmts = append(stmts,
		fmt.Sprintf("uint32_t pea_target = %s;", src.eaExpr),
		"cpu->a[7] -= 4u;",
		"f3_write32(cpu, cpu->a[7], pea_target);",
	)
	return append(stmts, pcCycles(c)...), true
}

func lowerLink(c *lowerCtx) ([]string, bool) {
	if len(c.ops) < 2 || c.ops[0].Type != OpReg || c.ops[1].Type != OpImm {
		return nil, false
	}
	reg := addrRegNum(c.ops[0].Reg)
	castDisp := "(int32_t)"
	if c.size == 2 {
		castDisp = "(int32_t)(int16_t)"
	}
	stmts := []string{
		"cpu->a[7] -= 4u;",
		fmt.Sprintf("f3_write32(cpu, cpu->a[7], cpu->a[%d]);", reg),
		fmt.Sprintf("cpu->a[%d] = cpu->a[7];", reg),
		fmt.Sprintf("cpu->a[7] = (uint32_t)(cpu->a[7] + %s(%d));", castDisp, c.ops[1].Imm),
	}
	return append(stmts, pcCycles(c)...), true
}

func lowerUnlk(c *lowerCtx) ([]string, bool) {
	if len(c.ops) == 0 || c.ops[0].Type != OpReg {
		return nil, false
	}
	reg := addrRegNum(c.ops[0].Reg)
	pop := "cpu->a[7] += 4u;"
	if reg == 7 {
		pop = "(void)cpu;"
	}
	stmts := []string{
		fmt.Sprintf("cpu->a[7] = cpu->a[%d];", reg),
		fmt.Sprintf("cpu->a[%d] = f3_read32(cpu, cpu->a[7]);", reg),
		pop,
	}
	return append(stmts, pcCycles(c)...), true
}

func lowerExt(c *lowerCtx) ([]string, bool) {
	if len(c.ops) == 0 || c.ops[0].Type != OpReg {
		return nil, false
	}
	reg := dataRegNum(c.ops[0].Reg)
	var stmts []string
	switch {
	case c.mnem == "ext" && c.size == 2:
		stmts = append(stmts,
			fmt.Sprintf("int16_t ext_val = (int8_t)(cpu->d[%d] & 0xffu);", reg),
			fmt.Sprintf("cpu->d[%d] = (cpu->d[%d] & 0xffff0000u) | ((uint16_t)ext_val & 0xffffu);", reg, reg),
		)
		stmts = append(stmts, ccLogic(c.liveOut, "(uint16_t)ext_val", 2)...)
	case c.mnem == "ext" && c.size == 4:
		stmts = append(stmts,
			fmt.Sprintf("int32_t ext_val = (int16_t)(cpu->d[%d] & 0xffffu);", reg),
			fmt.Sprintf("cpu->d[%d] = (uint32_t)ext_val;", reg),
		)
		stmts = append(stmts, ccLogic(c.liveOut, "(uint32_t)ext_val", 4)...)
	case c.mnem == "extb":
		stmts = append(stmts,
			fmt.Sprintf("int32_t ext_val = (int8_t)(cpu->d[%d] & 0xffu);", reg),
			fmt.Sprintf("cpu->d[%d] = (uint32_t)ext_val;", reg),
		)
		stmts = append(stmts, ccLogic(c.liveOut, "(uint32_t)ext_val", 4)...)
	case c.mnem == "swap":
		stmts = append(stmts, fmt.Sprintf("cpu->d[%d] = (cpu->d[%d] >> 16) | (cpu->d[%d] << 16);", reg, reg, reg))
		stmts = append(stmts, ccLogic(c.liveOut, fmt.Sprintf("cpu->d[%d]", reg), 4)...)
	}
	return append(stmts, pcCycles(c)...), true
}

func lowerMoveq(c *lowerCtx) ([]string, bool) {
	if len(c.ops) < 2 || c.ops[0].Type != OpImm || c.ops[1].Type != OpReg {
		return nil, false
	}
	imm8 := int(c.ops[0].Imm & 0xFF)
	s32 := imm8
	if imm8 >= 0x80 {
		s32 = imm8 - 0x100
	}
	dst := dataRegNum(c.ops[1].Reg)
	stmts := []string{fmt.Sprintf("cpu->d[%d] = 0x%08xu;", dst, s32&0xFFFFFFFF)}
	stmts = append(stmts, ccLogic(c.liveOut, fmt.Sprintf("cpu->d[%d]", dst), 4)...)
	return append(stmts, pcCycles(c)...), true
}

func lowerExg(c *lowerCtx) ([]string, bool) {
	left := decodeEA(c.insn, &c.ops[0], 4, "left", false)
	right := decodeEA(c.insn, &c.ops[1], 4, "right", false)
	if left == nil || right == nil || !isAD(left.regType) || !isAD(right.regType) {
		return nil, false
	}
	stmts := []string{"uint32_t exchanged = " + left.valExpr + ";"}
	stmts = append(stmts, genWrite(left, right.valExpr, 4)...)
	stmts = append(stmts, genWrite(right, "exchanged", 4)...)
	return append(stmts, pcCycles(c)...), true
}

func isAD(regType string) bool { return regType == "a" || regType == "d" }

func lowerTrap(c *lowerCtx) ([]string, bool) {
	if len(c.ops) == 0 || c.ops[0].Type != OpImm {
		return nil, false
	}
	vec := 32 + int(c.ops[0].Imm&0x0F)
	return []string{
		fmt.Sprintf("f3_exception(cpu, %du, 0x%08xu);", vec, c.nextPC),
		"return;",
	}, true
}

func lowerStop(c *lowerCtx) ([]string, bool) {
	if len(c.ops) == 0 || c.ops[0].Type != OpImm {
		return nil, false
	}
	imm16 := int(c.ops[0].Imm & 0xFFFF)
	return []string{
		fmt.Sprintf("if (!(cpu->sr & 0x2000u)) { f3_exception(cpu, 8u, 0x%08xu); return; }", c.insn.Address),
		"f3_cc_flush(cpu);",
		fmt.Sprintf("f3_set_sr(cpu, 0x%04xu);", imm16),
		"cpu->stopped = 1;",
		fmt.Sprintf("cpu->pc = 0x%08xu;", c.nextPC),
		fmt.Sprintf("cpu->cycles += %s;", c.cycles),
		"return;",
	}, true
}
