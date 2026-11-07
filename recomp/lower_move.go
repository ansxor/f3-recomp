package main

import (
	"fmt"
	"slices"
)

func lowerMove(c *lowerCtx) ([]string, bool) {
	ops := c.ops
	if len(ops) < 2 {
		return nil, false
	}
	isMovea := c.mnem == "movea" || (ops[1].Type == OpReg && isAddrReg(ops[1].Reg))
	src := decodeEA(c.insn, &ops[0], c.size, "src", false)
	dst := decodeEA(c.insn, &ops[1], c.size, "dst", false)
	if src == nil || dst == nil {
		return nil, false
	}
	special := slices.Contains([]string{"sr", "ccr", "usp"}, src.regType) || slices.Contains([]string{"sr", "ccr", "usp"}, dst.regType)
	privileged := slices.Contains([]string{"sr", "usp"}, src.regType) || slices.Contains([]string{"sr", "usp"}, dst.regType)
	var stmts []string
	if special {
		stmts = append(stmts, "f3_cc_flush(cpu);")
	}
	if privileged {
		stmts = append(stmts, fmt.Sprintf("if (!(cpu->sr & 0x2000u)) { f3_exception(cpu, 8u, 0x%08xu); return; }", c.insn.Address))
	}
	stmts = append(stmts, src.eaSetup...)
	stmts = append(stmts, src.readStmts...)
	stmts = append(stmts, fmt.Sprintf("uint32_t move_value = %s;", src.valExpr))
	val := "move_value"
	if isMovea {
		if c.size == 2 {
			val = fmt.Sprintf("(uint32_t)(int32_t)(int16_t)(%s)", val)
		}
		stmts = append(stmts, dst.eaSetup...)
		stmts = append(stmts, genWrite(dst, val, 4)...)
	} else {
		stmts = append(stmts, dst.eaSetup...)
		stmts = append(stmts, genWrite(dst, val, c.size)...)
		if !special && (!dst.isReg || dst.regType == "d" || dst.regType == "") {
			stmts = append(stmts, ccLogic(c.liveOut, val, c.size)...)
		}
	}
	return append(stmts, pcCycles(c)...), true
}

func movemReg(bit int) string {
	if bit < 8 {
		return fmt.Sprintf("cpu->d[%d]", bit)
	}
	return fmt.Sprintf("cpu->a[%d]", bit-8)
}

func movemAddrDecl(mem *ea, mask int) string {
	if mask != 0 {
		return fmt.Sprintf("uint32_t movem_addr = %s;", mem.eaExpr)
	}
	return fmt.Sprintf("(void)(%s);", mem.eaExpr)
}

func lowerMovem(c *lowerCtx) ([]string, bool) {
	ops := c.ops
	if len(ops) < 2 {
		return nil, false
	}
	step := 4
	if c.size == 2 {
		step = 2
	}
	switch {
	case ops[0].Type == OpRegBits:
		return lowerMovemToMem(c, int(ops[0].RegisterBits), &ops[1], step)
	case ops[1].Type == OpRegBits:
		return lowerMovemFromMem(c, int(ops[1].RegisterBits), &ops[0], step)
	}
	return nil, false
}

func lowerMovemToMem(c *lowerCtx, mask int, eaOp *Operand, step int) ([]string, bool) {
	var stmts []string
	if eaOp.AddressMode == AMRegIAddrPreDec {
		base := addrRegNum(eaOp.Reg)
		stmts = append(stmts, fmt.Sprintf("uint32_t movem_addr = cpu->a[%d];", base))
		for bit := 15; bit >= 0; bit-- {
			if (mask>>bit)&1 == 0 {
				continue
			}
			stmts = append(stmts, fmt.Sprintf("movem_addr -= %du;", step))
			val := movemReg(bit)
			if step == 2 {
				stmts = append(stmts, fmt.Sprintf("f3_write16(cpu, movem_addr, (uint16_t)%s);", val))
			} else {
				stmts = append(stmts,
					fmt.Sprintf("f3_write16(cpu, movem_addr + 2u, (uint16_t)%s);", val),
					fmt.Sprintf("f3_write16(cpu, movem_addr, (uint16_t)(%s >> 16));", val),
				)
			}
		}
		stmts = append(stmts, fmt.Sprintf("cpu->a[%d] = movem_addr;", base))
	} else {
		mem := decodeEA(c.insn, eaOp, step, "movem", false)
		if mem == nil || !mem.isMem {
			return nil, false
		}
		stmts = append(stmts, mem.eaSetup...)
		stmts = append(stmts, movemAddrDecl(mem, mask))
		for bit := 0; bit < 16; bit++ {
			if (mask>>bit)&1 == 0 {
				continue
			}
			val := movemReg(bit)
			if step == 2 {
				stmts = append(stmts, fmt.Sprintf("f3_write16(cpu, movem_addr, (uint16_t)%s);", val))
			} else {
				stmts = append(stmts, fmt.Sprintf("f3_write32(cpu, movem_addr, %s);", val))
			}
			stmts = append(stmts, fmt.Sprintf("movem_addr += %du;", step))
		}
	}
	return append(stmts, pcCycles(c)...), true
}

func movemLoads(mask, step int) []string {
	var stmts []string
	for bit := 0; bit < 16; bit++ {
		if (mask>>bit)&1 == 0 {
			continue
		}
		read := "f3_read32(cpu, movem_addr)"
		if step == 2 {
			read = "(uint32_t)(int32_t)(int16_t)f3_read16(cpu, movem_addr)"
		}
		stmts = append(stmts,
			fmt.Sprintf("%s = %s;", movemReg(bit), read),
			fmt.Sprintf("movem_addr += %du;", step),
		)
	}
	return stmts
}

func lowerMovemFromMem(c *lowerCtx, mask int, eaOp *Operand, step int) ([]string, bool) {
	var stmts []string
	if eaOp.AddressMode == AMRegIAddrPostInc {
		base := addrRegNum(eaOp.Reg)
		stmts = append(stmts, fmt.Sprintf("uint32_t movem_addr = cpu->a[%d];", base))
		stmts = append(stmts, movemLoads(mask, step)...)
		stmts = append(stmts, fmt.Sprintf("cpu->a[%d] = movem_addr;", base))
	} else {
		mem := decodeEA(c.insn, eaOp, step, "movem", false)
		if mem == nil || !mem.isMem {
			return nil, false
		}
		stmts = append(stmts, mem.eaSetup...)
		stmts = append(stmts, movemAddrDecl(mem, mask))
		stmts = append(stmts, movemLoads(mask, step)...)
	}
	return append(stmts, pcCycles(c)...), true
}

func lowerClr(c *lowerCtx) ([]string, bool) {
	if len(c.ops) == 0 {
		return nil, false
	}
	dst := decodeEA(c.insn, &c.ops[0], c.size, "clr", false)
	if dst == nil {
		return nil, false
	}
	stmts := append([]string(nil), dst.eaSetup...)
	stmts = append(stmts, genWrite(dst, "0u", c.size)...)
	stmts = append(stmts, ccLogic(c.liveOut, "0u", c.size)...)
	return append(stmts, pcCycles(c)...), true
}

func lowerTst(c *lowerCtx) ([]string, bool) {
	if len(c.ops) == 0 {
		return nil, false
	}
	src := decodeEA(c.insn, &c.ops[0], c.size, "tst", false)
	if src == nil {
		return nil, false
	}
	stmts := append([]string(nil), src.eaSetup...)
	stmts = append(stmts, src.readStmts...)
	cc := ccLogic(c.liveOut, src.valExpr, c.size)
	if len(cc) == 0 {
		cc = discardValue(src)
	}
	stmts = append(stmts, cc...)
	return append(stmts, pcCycles(c)...), true
}
