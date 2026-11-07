package main

import (
	"fmt"
	"strings"
)

func bytesBE(b []byte, start, n int) int {
	start = min(start, len(b))
	v := 0
	for _, c := range b[start:min(start+n, len(b))] {
		v = v<<8 | int(c)
	}
	return v
}

func bytesBESigned(b []byte, start, n int) int {
	v := bytesBE(b, start, n)
	if n == 0 {
		return 0
	}
	shift := 64 - 8*n
	return v << shift >> shift
}

func (i *Insn) Base() string {
	base, _, _ := strings.Cut(i.Mnemonic, ".")
	return strings.ToLower(base)
}

func (i *Insn) OperandSize(def int) int {
	if i.ID != 0 {
		switch i.OpSize.Size {
		case 1, 2, 4:
			return int(i.OpSize.Size)
		}
	}
	mnem := strings.ToLower(i.Mnemonic)
	switch {
	case strings.HasSuffix(mnem, ".b"):
		return 1
	case strings.HasSuffix(mnem, ".w"):
		return 2
	case strings.HasSuffix(mnem, ".l"):
		return 4
	}
	return def
}

func decodeIndexExpr(mem *Mem) string {
	idxReg := mem.IndexReg
	scale := int(mem.Scale)
	if scale <= 0 {
		scale = 1
	}
	if idxReg == 0 {
		return "0"
	}
	var regS string
	switch {
	case RegD0 <= idxReg && idxReg <= RegD7:
		regS = fmt.Sprintf("cpu->d[%d]", idxReg-RegD0)
	case RegA0 <= idxReg && idxReg <= RegA7:
		regS = fmt.Sprintf("cpu->a[%d]", idxReg-RegA0)
	default:
		return "0"
	}
	cast := fmt.Sprintf("(uint32_t)%s", regS)
	if mem.IndexSize == 0 {
		cast = fmt.Sprintf("(uint32_t)(int32_t)(int16_t)(%s & 0xffffu)", regS)
	}
	if scale > 1 {
		return fmt.Sprintf("(%s * %du)", cast, scale)
	}
	return cast
}

func (i *Insn) PCBase() int {
	address := int(i.Address)
	opcode := bytesBE(i.Bytes, 0, 2)
	mnem := i.Base()
	switch mnem {
	case "cmp2", "chk2", "cas", "moves":
		return address + 4
	}
	if strings.HasPrefix(mnem, "bf") {
		return address + 4
	}
	switch mnem {
	case "movem":
		return address + 4
	case "mulu", "muls", "divu", "divs", "divul", "divsl":
		if i.OperandSize(4) == 4 {
			return address + 4
		}
	}
	if opcode&0xFF00 == 0x0800 {
		return address + 4
	}
	switch mnem {
	case "ori", "andi", "subi", "addi", "eori", "cmpi":
		return address + 2 + max(2, i.OperandSize(4))
	}
	return address + 2
}

func baseExpr(insn *Insn, mem *Mem) string {
	// Capstone 5 reports full-format PC-relative operands as AREGI_INDEX with
	// base_reg=PC. Do not turn this into the nonexistent address register A16.
	if mem.BaseReg == RegPC {
		return fmt.Sprintf("0x%08xu", insn.PCBase())
	}
	if RegA0 <= mem.BaseReg && mem.BaseReg <= RegA7 {
		return fmt.Sprintf("cpu->a[%d]", mem.BaseReg-RegA0)
	}
	return "0u"
}

var displacementBytes = [4]int{0, 0, 2, 4}

func (i *Insn) EAExtensionBytes(offset, mode, reg, size int) int {
	raw := i.Bytes
	switch {
	case mode <= 4:
		return 0
	case mode == 5 || (mode == 7 && (reg == 0 || reg == 2)):
		return 2
	case mode == 7 && reg == 1:
		return 4
	case mode == 7 && reg == 4:
		return max(2, size)
	case mode == 6 || (mode == 7 && reg == 3):
		ext := bytesBE(raw, offset, 2)
		if ext&0x100 == 0 {
			return 2
		}
		return 2 + displacementBytes[(ext>>4)&3] + displacementBytes[ext&3]
	}
	return 0
}

func fullEA(insn *Insn, op *Operand, size int, prefix string) ([]string, string, bool) {
	// Capstone exposes full-format word displacements as unsigned values.
	// Decode the extension layout to retain their widths and suppression bits.
	raw := insn.Bytes
	offset := insn.PCBase() - int(insn.Address)
	if insn.Base() == "move" && prefix == "dst" {
		opcode := bytesBE(raw, 0, 2)
		offset += insn.EAExtensionBytes(offset, (opcode>>3)&7, opcode&7, size)
	}
	if offset+2 > len(raw) {
		return nil, "", false
	}
	ext := bytesBE(raw, offset, 2)
	bank := "d"
	if ext&0x8000 != 0 {
		bank = "a"
	}
	reg := fmt.Sprintf("cpu->%s[%d]", bank, (ext>>12)&7)
	idx := fmt.Sprintf("(uint32_t)(int32_t)(int16_t)%s", reg)
	if ext&0x0800 != 0 {
		idx = reg
	}
	idx = fmt.Sprintf("(%s * %du)", idx, 1<<((ext>>9)&3))
	address := "ea_" + prefix
	if ext&0x100 == 0 {
		// Capstone labels brief extensions with zero displacement BASE_DISP.
		disp := bytesBESigned(raw, offset+1, 1)
		expression := fmt.Sprintf("%s + 0x%xu + %s", baseExpr(insn, &op.Mem), disp&0xFFFFFFFF, idx)
		return []string{fmt.Sprintf("uint32_t %s = %s;", address, expression)}, address, true
	}
	var bdSize int
	switch (ext >> 4) & 3 {
	case 1:
		bdSize = 0
	case 2:
		bdSize = 2
	case 3:
		bdSize = 4
	default:
		return nil, "", false
	}
	indirect := ext & 7
	if ext&8 != 0 || indirect == 4 {
		return nil, "", false
	}
	odSize := displacementBytes[indirect&3]
	if offset+2+bdSize+odSize > len(raw) {
		return nil, "", false
	}
	bd := bytesBESigned(raw, offset+2, bdSize)
	od := bytesBESigned(raw, offset+2+bdSize, odSize)
	base := "0u"
	if ext&0x80 == 0 {
		base = baseExpr(insn, &op.Mem)
	}
	if ext&0x40 != 0 {
		idx = "0u"
	}
	var expression string
	switch {
	case indirect == 0:
		expression = fmt.Sprintf("%s + 0x%xu + %s", base, bd&0xFFFFFFFF, idx)
	case indirect < 4:
		expression = fmt.Sprintf("f3_read32(cpu, %s + 0x%xu + %s) + 0x%xu", base, bd&0xFFFFFFFF, idx, od&0xFFFFFFFF)
	default:
		expression = fmt.Sprintf("f3_read32(cpu, %s + 0x%xu) + %s + 0x%xu", base, bd&0xFFFFFFFF, idx, od&0xFFFFFFFF)
	}
	return []string{fmt.Sprintf("uint32_t %s = %s;", address, expression)}, address, true
}

func decodeEA(insn *Insn, op *Operand, size int, prefix string, postIncOnRead bool) *ea {
	e := &ea{}
	am := op.AddressMode

	if op.Type == OpReg {
		reg := op.Reg
		switch {
		case RegD0 <= reg && reg <= RegD7:
			e.isReg = true
			e.regType = "d"
			e.regNum = int(reg - RegD0)
			switch size {
			case 1:
				e.valExpr = fmt.Sprintf("(uint8_t)(cpu->d[%d] & 0xffu)", e.regNum)
			case 2:
				e.valExpr = fmt.Sprintf("(uint16_t)(cpu->d[%d] & 0xffffu)", e.regNum)
			default:
				e.valExpr = fmt.Sprintf("cpu->d[%d]", e.regNum)
			}
			return e
		case RegA0 <= reg && reg <= RegA7:
			e.isReg = true
			e.regType = "a"
			e.regNum = int(reg - RegA0)
			if size == 2 {
				e.valExpr = fmt.Sprintf("(uint16_t)(cpu->a[%d] & 0xffffu)", e.regNum)
			} else {
				e.valExpr = fmt.Sprintf("cpu->a[%d]", e.regNum)
			}
			return e
		case reg == RegCCR:
			e.isReg = true
			e.regType = "ccr"
			e.valExpr = "(cpu->sr & 0x1fu)"
			return e
		case reg == RegSR:
			e.isReg = true
			e.regType = "sr"
			e.valExpr = "cpu->sr"
			return e
		case reg == RegUSP:
			e.isReg = true
			e.regType = "usp"
			e.valExpr = "cpu->usp"
			return e
		}
		return nil
	}

	if op.Type == OpImm || am == AMImmediate {
		e.isImm = true
		mask, ok := maskMap[size]
		if !ok {
			mask = 0xFFFFFFFF
		}
		e.immVal = int(op.Imm) & mask
		e.valExpr = fmt.Sprintf("0x%xu", e.immVal)
		return e
	}

	e.isMem = true
	addrVar := "ea_" + prefix
	stepFor := func(regNum int) int {
		if regNum == 7 && size == 1 {
			return 2
		}
		return size
	}

	switch am {
	case AMRegIAddr:
		e.eaExpr = fmt.Sprintf("cpu->a[%d]", int(op.Reg)-int(RegA0))

	case AMRegIAddrPostInc:
		regNum := int(op.Reg) - int(RegA0)
		e.eaSetup = append(e.eaSetup, fmt.Sprintf("uint32_t %s = cpu->a[%d];", addrVar, regNum))
		e.eaExpr = addrVar
		e.postStep = append(e.postStep, fmt.Sprintf("cpu->a[%d] += %du;", regNum, stepFor(regNum)))

	case AMRegIAddrPreDec:
		regNum := int(op.Reg) - int(RegA0)
		e.writeLowFirst = size == 4 && insn.Base() == "move"
		e.eaSetup = append(e.eaSetup, fmt.Sprintf("cpu->a[%d] -= %du;", regNum, stepFor(regNum)))
		e.eaExpr = fmt.Sprintf("cpu->a[%d]", regNum)

	case AMRegIAddrDisp:
		regNum := int(op.Mem.BaseReg) - int(RegA0)
		e.eaSetup = append(e.eaSetup, fmt.Sprintf("uint32_t %s = (uint32_t)(cpu->a[%d] + (int32_t)(%d));", addrVar, regNum, op.Mem.Disp))
		e.eaExpr = addrVar

	case AMARegIIndex8BitDisp:
		e.eaSetup = append(e.eaSetup, fmt.Sprintf("uint32_t %s = (uint32_t)(%s + %s + (int32_t)(%d));",
			addrVar, baseExpr(insn, &op.Mem), decodeIndexExpr(&op.Mem), op.Mem.Disp))
		e.eaExpr = addrVar

	case AMARegIIndexBaseDisp, AMMemIPostIndex, AMMemIPreIndex,
		AMPCIIndexBaseDisp, AMPCMemIPostIndex, AMPCMemIPreIndex:
		setup, expr, ok := fullEA(insn, op, size, prefix)
		if !ok {
			return nil
		}
		e.eaSetup, e.eaExpr = setup, expr

	case AMPCIDisp:
		e.eaSetup = append(e.eaSetup, fmt.Sprintf("uint32_t %s = (uint32_t)(0x%08xu + (int32_t)(%d));", addrVar, insn.PCBase(), op.Mem.Disp))
		e.eaExpr = addrVar

	case AMPCIIndex8BitDisp:
		e.eaSetup = append(e.eaSetup, fmt.Sprintf("uint32_t %s = (uint32_t)(0x%08xu + %s + (int32_t)(%d));",
			addrVar, insn.PCBase(), decodeIndexExpr(&op.Mem), op.Mem.Disp))
		e.eaExpr = addrVar

	case AMAbsoluteDataShort:
		e.eaExpr = fmt.Sprintf("0x%08xu", int(int16(op.Imm))&0xFFFFFFFF)

	case AMAbsoluteDataLong:
		e.eaExpr = fmt.Sprintf("0x%08xu", int(op.Imm)&0xFFFFFFFF)

	default:
		return nil
	}

	valVar := "val_" + prefix
	switch size {
	case 1:
		e.readStmts = append(e.readStmts, fmt.Sprintf("uint32_t %s = f3_read8(cpu, %s);", valVar, e.eaExpr))
	case 2:
		e.readStmts = append(e.readStmts, fmt.Sprintf("uint32_t %s = f3_read16(cpu, %s);", valVar, e.eaExpr))
	default:
		e.readStmts = append(e.readStmts, fmt.Sprintf("uint32_t %s = f3_read32(cpu, %s);", valVar, e.eaExpr))
	}
	e.valExpr = valVar

	switch {
	case postIncOnRead, prefix == "src", prefix == "tst", prefix == "cmp_dst", prefix == "btst":
		e.readStmts = append(e.readStmts, e.postStep...)
		e.postStep = nil
	}
	return e
}

func genWrite(e *ea, res string, size int) []string {
	stmts := []string{}
	switch {
	case e.isReg:
		switch e.regType {
		case "d":
			switch size {
			case 1:
				stmts = append(stmts, fmt.Sprintf("cpu->d[%d] = (cpu->d[%d] & 0xffffff00u) | ((%s) & 0xffu);", e.regNum, e.regNum, res))
			case 2:
				stmts = append(stmts, fmt.Sprintf("cpu->d[%d] = (cpu->d[%d] & 0xffff0000u) | ((%s) & 0xffffu);", e.regNum, e.regNum, res))
			default:
				stmts = append(stmts, fmt.Sprintf("cpu->d[%d] = (%s);", e.regNum, res))
			}
		case "a":
			if size == 2 {
				stmts = append(stmts, fmt.Sprintf("cpu->a[%d] = (uint32_t)(int32_t)(int16_t)(%s);", e.regNum, res))
			} else {
				stmts = append(stmts, fmt.Sprintf("cpu->a[%d] = (%s);", e.regNum, res))
			}
		case "ccr":
			stmts = append(stmts, "f3_cc_flush(cpu);",
				fmt.Sprintf("cpu->sr = (cpu->sr & 0xff00u) | ((%s) & 0x1fu);", res),
				"cpu->cc_op = F3_CC_OP_NONE;")
		case "sr":
			stmts = append(stmts, "f3_cc_flush(cpu);",
				fmt.Sprintf("f3_set_sr(cpu, (uint16_t)(%s));", res),
				"cpu->cc_op = F3_CC_OP_NONE;")
		case "usp":
			stmts = append(stmts, fmt.Sprintf("cpu->usp = (%s);", res))
		}
	case e.isMem:
		switch {
		case size == 1:
			stmts = append(stmts, fmt.Sprintf("f3_write8(cpu, %s, (uint8_t)(%s));", e.eaExpr, res))
		case size == 2:
			stmts = append(stmts, fmt.Sprintf("f3_write16(cpu, %s, (uint16_t)(%s));", e.eaExpr, res))
		case e.writeLowFirst:
			stmts = append(stmts,
				fmt.Sprintf("f3_write16(cpu, %s + 2u, (uint16_t)(%s));", e.eaExpr, res),
				fmt.Sprintf("f3_write16(cpu, %s, (uint16_t)((%s) >> 16));", e.eaExpr, res))
		default:
			stmts = append(stmts, fmt.Sprintf("f3_write32(cpu, %s, (uint32_t)(%s));", e.eaExpr, res))
		}
		stmts = append(stmts, e.postStep...)
	}
	return stmts
}
