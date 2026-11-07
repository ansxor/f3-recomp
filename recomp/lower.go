package main

import (
	"fmt"
	"slices"
	"strings"
)

// emitterLower lowers a single decoded instruction into equivalent C statements.
//
// liveOut is the set of CCR flags (CCR*) that a successor may observe before
// overwriting them. Lazy flag producers store no condition-code state when none
// of the flags they write is live; callers wanting everything pass CCRAll.
//
// ok is false if the instruction is unsupported (signaling fallback to Musashi).
//
// Contract every handler keeps: decode all operands before returning
// statements; return no statements for unsupported operand forms; capture
// aliased source values before the destination changes; guest memory only
// through f3_read*/f3_write* ABI callbacks; keep untouched bits of a partial
// data register; sign-extend where the instruction requires it; keep X current
// while NZVC stays pending; f3_cc_flush before any call that needs canonical
// SR; update pc and cumulative cycles on normal completion; return after an
// exception or terminal control op. Use unsigned arithmetic for guest
// wraparound and guard host division/shift edge cases. A helper that changes
// only Z must materialize pending flags first; a full NZVC producer can
// replace them without a flush. Synthetic test instructions use bytes from a
// documented encoding, not copied game data.
func (i *Insn) Lower(liveOut int) ([]string, bool) {
	opcode := bytesBE(i.Bytes, 0, 2)
	address := i.Address
	if opcode>>12 == 10 || opcode>>12 == 15 {
		vector := 11
		if opcode>>12 == 10 {
			vector = 10
		}
		return []string{fmt.Sprintf("f3_exception(cpu, %d, 0x%xu);", vector, address), "return;"}, true
	}
	if opcode == 0x4AFC || opcode&0xFFF8 == 0x4848 {
		// BKPT without external instruction substitution takes illegal opcode.
		return []string{fmt.Sprintf("f3_exception(cpu, 4, 0x%xu);", address), "return;"}, true
	}
	if i.ID == 0 {
		return nil, false
	}
	c := &lowerCtx{
		insn:    i,
		ops:     i.Operands,
		mnem:    i.Base(),
		size:    i.OperandSize(4),
		opcode:  opcode,
		nextPC:  int(address) + int(i.Size),
		cycles:  i.CycleCost(),
		liveOut: liveOut,
	}
	ops, mnem, size, nextPC, cycles := c.ops, c.mnem, c.size, c.nextPC, c.cycles
	raw := i.Bytes

	if mnem == "reset" {
		return []string{
			"f3_cc_flush(cpu);",
			fmt.Sprintf("if (!(cpu->sr & 0x2000u)) { f3_exception(cpu, 8, 0x%xu); return; }", address),
			"f3_reset_devices(cpu);",
			fmt.Sprintf("cpu->pc = 0x%xu;", nextPC),
			fmt.Sprintf("cpu->cycles += %s;", cycles),
		}, true
	}

	if mnem == "chk" {
		e := decodeEA(i, &ops[0], size, "chk", true)
		if (size != 2 && size != 4) || e == nil || (e.isReg && e.regType != "d") {
			return nil, false
		}
		register := (opcode >> 9) & 7
		bits := size * 8
		return slices.Concat(e.eaSetup, e.readStmts, []string{
			fmt.Sprintf("int%d_t value = (int%d_t)cpu->d[%d];", bits, bits, register),
			fmt.Sprintf("int%d_t bound = (int%d_t)%s;", bits, bits, e.valExpr),
			"f3_cc_flush(cpu);",
			"cpu->sr = (cpu->sr & ~0x1fu) | (cpu->sr & (F3_CCR_X | F3_CCR_N)) | (value == 0 ? F3_CCR_Z : 0u);",
			"if (value < 0 || value > bound) {",
			"    cpu->sr = (cpu->sr & ~F3_CCR_N) | (value < 0 ? F3_CCR_N : 0u);",
			fmt.Sprintf("    f3_exception(cpu, 6, 0x%xu); return;", nextPC),
			"}",
			fmt.Sprintf("cpu->pc = 0x%xu;", nextPC),
			fmt.Sprintf("cpu->cycles += %s;", cycles),
		}), true
	}

	if mnem == "nbcd" {
		e := decodeEA(i, &ops[0], 1, "nbcd", false)
		if e == nil || e.isImm || (e.isReg && e.regType != "d") {
			return nil, false
		}
		stmts := slices.Concat(e.eaSetup, e.readStmts, []string{
			"f3_cc_flush(cpu);",
			fmt.Sprintf("uint32_t result = (0x9au - %s - ((cpu->sr & F3_CCR_X) != 0)) & 0xffu;", e.valExpr),
			"if (result != 0x9au) {",
			"    uint32_t overflow = ~result;",
			"    if ((result & 0xfu) == 0xau) result = (result & 0xf0u) + 0x10u;",
			"    result &= 0xffu;",
			"    cpu->sr = (cpu->sr & ~0x1fu) | F3_CCR_X | F3_CCR_C |",
			"        (result & 0x80u ? F3_CCR_N : 0u) | (overflow & result & 0x80u ? F3_CCR_V : 0u) |",
			"        (result == 0 ? (cpu->sr & F3_CCR_Z) : 0u);",
		})
		if e.isReg {
			stmts = append(stmts, fmt.Sprintf("    cpu->d[%d] = (cpu->d[%d] & 0xffffff00u) | result;", e.regNum, e.regNum))
		} else {
			stmts = append(stmts, fmt.Sprintf("    f3_write8(cpu, %s, (uint8_t)result);", e.eaExpr))
		}
		return slices.Concat(stmts, []string{
			"} else {",
			"    cpu->sr = (cpu->sr & ~0x1fu) | (cpu->sr & F3_CCR_Z) | F3_CCR_N;",
			"}",
		}, e.postStep, []string{
			fmt.Sprintf("cpu->pc = 0x%xu;", nextPC),
			fmt.Sprintf("cpu->cycles += %s;", cycles),
		}), true
	}

	switch mnem {
	case "abcd", "sbcd", "pack", "unpk":
		form := opcode & 0xF1F0
		source, destination := opcode&7, (opcode>>9)&7
		memory := opcode&8 != 0
		stepFor := func(reg int) int {
			if reg == 7 {
				return 2
			}
			return 1
		}
		sourceStep, destinationStep := stepFor(source), stepFor(destination)
		stmts := []string{}
		if memory {
			stmts = append(stmts,
				fmt.Sprintf("cpu->a[%d] -= %du;", source, sourceStep),
				fmt.Sprintf("uint32_t src = f3_read8(cpu, cpu->a[%d]);", source))
		} else {
			stmts = append(stmts, fmt.Sprintf("uint32_t src = cpu->d[%d];", source))
		}
		switch form {
		case 0xC100, 0x8100:
			if memory {
				stmts = append(stmts,
					fmt.Sprintf("cpu->a[%d] -= %du;", destination, destinationStep),
					fmt.Sprintf("uint32_t dst = f3_read8(cpu, cpu->a[%d]);", destination))
			} else {
				stmts = append(stmts, fmt.Sprintf("uint32_t dst = cpu->d[%d];", destination))
			}
			add := form == 0xC100
			resultInit, adjust, sign, carryAdjust := "uint32_t result = (dst & 0xfu) - (src & 0xfu) - extend;", "-=", "-", "+="
			if add {
				resultInit, adjust, sign, carryAdjust = "uint32_t result = (dst & 0xfu) + (src & 0xfu) + extend;", "+=", "+", "-="
			}
			stmts = append(stmts,
				"f3_cc_flush(cpu);",
				"uint32_t extend = (cpu->sr & F3_CCR_X) != 0;",
				resultInit,
				"uint32_t overflow = ~result;",
				fmt.Sprintf("if (result > 9u) result %s 6u;", adjust),
				fmt.Sprintf("result += (dst & 0xf0u) %s (src & 0xf0u);", sign),
				"uint32_t carry = result > 0x99u;",
				fmt.Sprintf("if (carry) result %s 0xa0u;", carryAdjust),
				"result &= 0xffu;",
				"cpu->sr = (cpu->sr & ~0x1fu) | (carry ? F3_CCR_X | F3_CCR_C : 0u) |",
				"    (result & 0x80u ? F3_CCR_N : 0u) | (overflow & result & 0x80u ? F3_CCR_V : 0u) |",
				"    (result == 0 ? (cpu->sr & F3_CCR_Z) : 0u);")
			if memory {
				stmts = append(stmts, fmt.Sprintf("f3_write8(cpu, cpu->a[%d], (uint8_t)result);", destination))
			} else {
				stmts = append(stmts, fmt.Sprintf("cpu->d[%d] = (cpu->d[%d] & 0xffffff00u) | result;", destination, destination))
			}
			nextPC = int(address) + 2
		case 0x8140, 0x8180:
			adjustment := bytesBE(raw, 2, 2)
			var width int
			if form == 0x8140 {
				if memory {
					stmts = append(stmts,
						fmt.Sprintf("cpu->a[%d] -= %du;", source, sourceStep),
						fmt.Sprintf("src = (src << 8) | f3_read8(cpu, cpu->a[%d]);", source))
				}
				stmts = append(stmts,
					fmt.Sprintf("src += 0x%xu;", adjustment),
					"uint32_t result = ((src >> 4) & 0xf0u) | (src & 0xfu);")
				width = 1
			} else {
				stmts = append(stmts, fmt.Sprintf("uint32_t result = ((((src << 4) & 0xf00u) | (src & 0xfu)) + 0x%xu) & 0xffffu;", adjustment))
				width = 2
			}
			if memory {
				for byteIndex := range width {
					stmts = append(stmts,
						fmt.Sprintf("cpu->a[%d] -= %du;", destination, destinationStep),
						fmt.Sprintf("f3_write8(cpu, cpu->a[%d], (uint8_t)(result >> %d));", destination, (width-1-byteIndex)*8))
				}
			} else {
				stmts = append(stmts, fmt.Sprintf("cpu->d[%d] = (cpu->d[%d] & 0x%xu) | result;", destination, destination, ^maskMap[width]&0xFFFFFFFF))
			}
			nextPC = int(address) + 4
		default:
			return nil, false
		}
		return append(stmts, fmt.Sprintf("cpu->pc = 0x%xu;", nextPC), fmt.Sprintf("cpu->cycles += %s;", cycles)), true

	case "movep":
		register := (opcode >> 9) & 7
		areg := opcode & 7
		width := 2
		if opcode&0x40 != 0 {
			width = 4
		}
		displacement := bytesBESigned(raw, 2, 2)
		stmts := []string{fmt.Sprintf("uint32_t ea = cpu->a[%d] + 0x%xu;", areg, displacement&0xFFFFFFFF)}
		if opcode&0x80 != 0 {
			for byteIndex := range width {
				stmts = append(stmts, fmt.Sprintf("f3_write8(cpu, ea + %du, (uint8_t)(cpu->d[%d] >> %d));", byteIndex*2, register, (width-1-byteIndex)*8))
			}
		} else {
			stmts = append(stmts, "uint32_t value = 0;")
			for byteIndex := range width {
				stmts = append(stmts, fmt.Sprintf("value = (value << 8) | f3_read8(cpu, ea + %du);", byteIndex*2))
			}
			high := ""
			if width == 2 {
				high = fmt.Sprintf("(cpu->d[%d] & 0xffff0000u) | ", register)
			}
			stmts = append(stmts, fmt.Sprintf("cpu->d[%d] = %svalue;", register, high))
		}
		return append(stmts, fmt.Sprintf("cpu->pc = 0x%xu;", nextPC), fmt.Sprintf("cpu->cycles += %s;", cycles)), true

	case "moves":
		extension := bytesBE(raw, 2, 2)
		store := extension&0x800 != 0
		register := (extension >> 12) & 7
		bank := "d"
		if extension&0x8000 != 0 {
			bank = "a"
		}
		field := fmt.Sprintf("cpu->%s[%d]", bank, register)
		opIndex := 0
		if store {
			opIndex = 1
		}
		e := decodeEA(i, &ops[opIndex], size, "moves", false)
		if e == nil || !e.isMem {
			return nil, false
		}
		stmts := []string{
			"f3_cc_flush(cpu);",
			fmt.Sprintf("if (!(cpu->sr & 0x2000u)) { f3_exception(cpu, 8, 0x%xu); return; }", address),
		}
		stmts = slices.Concat(stmts, e.eaSetup, []string{fmt.Sprintf("uint32_t ea = %s;", e.eaExpr)}, e.postStep)
		bits := size * 8
		if store {
			stmts = append(stmts, fmt.Sprintf("f3_write%d(cpu, ea, (uint%d_t)%s);", bits, bits, field))
		} else {
			stmts = append(stmts, fmt.Sprintf("uint32_t value = f3_read%d(cpu, ea);", bits))
			if extension&0x8000 != 0 {
				stmts = append(stmts, fmt.Sprintf("%s = (uint32_t)(int32_t)(int%d_t)value;", field, bits))
			} else {
				stmts = append(stmts, fmt.Sprintf("%s = (%s & 0x%xu) | value;", field, field, ^maskMap[size]&0xFFFFFFFF))
			}
		}
		// EC020 memory-to-register and long register-to-memory charge two extra.
		extra := 0
		if !store || size == 4 {
			extra = 2
		}
		return append(stmts, fmt.Sprintf("cpu->pc = 0x%xu;", nextPC), fmt.Sprintf("cpu->cycles += %s + %du;", cycles, extra)), true

	case "cas":
		extension := bytesBE(raw, 2, 2)
		compare, update := extension&7, (extension>>6)&7
		e := decodeEA(i, &ops[2], size, "cas", false)
		if e == nil || !e.isMem {
			return nil, false
		}
		bits := size * 8
		mask := maskMap[size]
		return slices.Concat(e.eaSetup, []string{fmt.Sprintf("uint32_t ea = %s;", e.eaExpr)}, e.postStep, []string{
			fmt.Sprintf("uint32_t value = f3_read%d(cpu, ea);", bits),
			fmt.Sprintf("uint32_t compared = cpu->d[%d] & 0x%xu;", compare, mask),
			fmt.Sprintf("uint32_t result = (value - compared) & 0x%xu;", mask),
			"f3_cc_flush(cpu);",
			fmt.Sprintf("cpu->cc_op = F3_CC_OP_CMP; cpu->cc_src = compared; cpu->cc_dst = value; cpu->cc_result = result; cpu->cc_width = %d;", size),
			"if (result == 0) {",
			"    cpu->cycles += 3u;",
			fmt.Sprintf("    f3_write%d(cpu, ea, (uint%d_t)cpu->d[%d]);", bits, bits, update),
			"} else {",
			fmt.Sprintf("    cpu->d[%d] = (cpu->d[%d] & 0x%xu) | value;", compare, compare, ^mask&4294967295),
			"}",
			fmt.Sprintf("cpu->pc = 0x%xu;", nextPC),
			fmt.Sprintf("cpu->cycles += %s;", cycles),
		}), true

	case "cas2":
		extensions := []int{bytesBE(raw, 2, 2), bytesBE(raw, 4, 2)}
		bits := size * 8
		mask := maskMap[size]
		stmts := []string{}
		for index, extension := range extensions {
			bank := "d"
			if extension&0x8000 != 0 {
				bank = "a"
			}
			stmts = append(stmts,
				fmt.Sprintf("uint32_t ea%d = cpu->%s[%d];", index, bank, (extension>>12)&7),
				fmt.Sprintf("uint32_t value%d = f3_read%d(cpu, ea%d);", index, bits, index),
				fmt.Sprintf("uint32_t compare%d = cpu->d[%d] & 0x%xu;", index, extension&7, mask),
				fmt.Sprintf("uint32_t result%d = (value%d - compare%d) & 0x%xu;", index, index, index, mask))
		}
		stmts = append(stmts,
			"f3_cc_flush(cpu);",
			fmt.Sprintf("cpu->cc_op = F3_CC_OP_CMP; cpu->cc_width = %d;", size),
			"cpu->cc_src = result0 ? compare0 : compare1;",
			"cpu->cc_dst = result0 ? value0 : value1;",
			"cpu->cc_result = result0 ? result0 : result1;",
			"if (result0 == 0 && result1 == 0) {",
			"    cpu->cycles += 3u;")
		for index, extension := range extensions {
			stmts = append(stmts, fmt.Sprintf("    f3_write%d(cpu, ea%d, (uint%d_t)cpu->d[%d]);", bits, index, bits, (extension>>6)&7))
		}
		stmts = append(stmts, "} else {")
		for index, extension := range extensions {
			field := fmt.Sprintf("cpu->d[%d]", extension&7)
			value := fmt.Sprintf("(%s & 0x%xu) | value%d", field, ^mask&0xFFFFFFFF, index)
			// Retain the pinned core's CAS2.W address-bank sign-extension rule.
			if size == 2 && extension&0x8000 != 0 {
				value = fmt.Sprintf("(uint32_t)(int32_t)(int16_t)value%d", index)
			}
			stmts = append(stmts, fmt.Sprintf("    %s = %s;", field, value))
		}
		return append(stmts, "}", fmt.Sprintf("cpu->pc = 0x%xu;", nextPC), fmt.Sprintf("cpu->cycles += %s;", cycles)), true

	case "tas":
		e := decodeEA(i, &ops[0], 1, "tas", false)
		if e == nil || e.isImm || (e.isReg && e.regType != "d") {
			return nil, false
		}
		return slices.Concat(e.eaSetup, e.readStmts, []string{
			fmt.Sprintf("uint32_t original = %s;", e.valExpr),
			"f3_cc_flush(cpu);",
			"cpu->cc_op = F3_CC_OP_LOGIC; cpu->cc_result = original; cpu->cc_width = 1;",
		}, genWrite(e, "(original | 0x80u)", 1), []string{
			fmt.Sprintf("cpu->pc = 0x%xu;", nextPC),
			fmt.Sprintf("cpu->cycles += %s;", cycles),
		}), true
	}

	if rest, isTrap := strings.CutPrefix(mnem, "trap"); mnem == "trapv" || (isTrap && condKnown(rest)) {
		condition := 9
		if mnem != "trapv" {
			condition = CondMap[rest]
		}
		return []string{
			"f3_cc_flush(cpu);",
			fmt.Sprintf("if (f3_eval_cond(cpu, %d)) { f3_exception(cpu, 7, 0x%xu); return; }", condition, nextPC),
			fmt.Sprintf("cpu->pc = 0x%xu;", nextPC),
			fmt.Sprintf("cpu->cycles += %s;", cycles),
		}, true
	}

	switch mnem {
	case "movec":
		ext := bytesBE(raw, 2, 2)
		bank := "d"
		if ext&0x8000 != 0 {
			bank = "a"
		}
		reg := fmt.Sprintf("cpu->%s[%d]", bank, (ext>>12)&7)
		controls := map[int]string{
			0:     "sfc",
			1:     "dfc",
			2:     "cacr",
			0x800: "usp",
			0x801: "vbr",
			0x802: "caar",
			0x803: "msp",
			0x804: "ssp",
		}
		control := ext & 0xFFF
		stmts := []string{
			"f3_cc_flush(cpu);",
			fmt.Sprintf("if (!(cpu->sr & 0x2000u)) { f3_exception(cpu, 8, 0x%xu); return; }", address),
		}
		name, known := controls[control]
		if !known {
			return append(stmts, fmt.Sprintf("f3_exception(cpu, 4, 0x%xu);", address), "return;"), true
		}
		field := "cpu->" + name
		active := ""
		if control == 0x803 || control == 0x804 {
			active = "!(cpu->sr & 0x1000u)"
			if control == 0x803 {
				active = "cpu->sr & 0x1000u"
			}
		}
		if bytesBE(raw, 0, 2) == 0x4E7A {
			if active != "" {
				field = fmt.Sprintf("((%s) ? cpu->a[7] : %s)", active, field)
			}
			stmts = append(stmts, fmt.Sprintf("%s = %s;", reg, field))
		} else {
			value := reg
			if control <= 2 {
				mask := 15
				if control < 2 {
					mask = 7
				}
				value = fmt.Sprintf("(%s & %du)", reg, mask)
			}
			if active != "" {
				stmts = append(stmts, fmt.Sprintf("if (%s) cpu->a[7] = %s; else %s = %s;", active, value, field, value))
			} else {
				stmts = append(stmts, fmt.Sprintf("%s = %s;", field, value))
			}
		}
		return append(stmts, fmt.Sprintf("cpu->pc = 0x%xu;", nextPC), fmt.Sprintf("cpu->cycles += %s;", cycles)), true

	case "rte":
		return []string{
			"f3_cc_flush(cpu);",
			fmt.Sprintf("if (!(cpu->sr & 0x2000u)) { f3_exception(cpu, 8, 0x%xu); return; }", address),
			"for (;;) {",
			"    uint32_t sp = cpu->a[7];",
			"    unsigned format = f3_read16(cpu, sp + 6u) >> 12;",
			fmt.Sprintf("    if (format > 2) { f3_exception(cpu, 14, 0x%xu); return; }", nextPC),
			"    uint16_t sr = f3_read16(cpu, sp);",
			"    uint32_t pc = format == 1 ? 0u : f3_read32(cpu, sp + 2u);",
			"    cpu->a[7] = sp + (format == 2 ? 12u : 8u);",
			"    f3_set_sr(cpu, sr);",
			"    if (format != 1) { cpu->pc = pc; break; }",
			"}",
			fmt.Sprintf("cpu->cycles += %s;", cycles),
			"return;",
		}, true

	case "bftst", "bfextu", "bfchg", "bfexts", "bfclr", "bfffo", "bfset", "bfins":
		ext := bytesBE(raw, 2, 2)
		operation := (opcode >> 8) & 7
		operand := &ops[0]
		if mnem == "bfins" {
			operand = &ops[len(ops)-1]
		}
		e := decodeEA(i, operand, 1, "bitfield", false)
		if e == nil || (e.isReg && e.regType != "d") {
			return nil, false
		}
		offset := fmt.Sprint((ext >> 6) & 31)
		if ext&0x800 != 0 {
			offset = fmt.Sprintf("(int32_t)cpu->d[%d]", (ext>>6)&7)
		}
		width := fmt.Sprint(ext & 31)
		if ext&0x20 != 0 {
			width = fmt.Sprintf("cpu->d[%d]", ext&7)
		}
		location := fmt.Sprint(e.regNum)
		isMem := 0
		if e.isMem {
			location = e.eaExpr
			isMem = 1
		}
		return slices.Concat(e.eaSetup, []string{
			fmt.Sprintf("f3_bitfield(cpu, %d, %d, %s, %s, %s, %d);", operation, isMem, location, offset, width, ext>>12&7),
			fmt.Sprintf("cpu->pc = 0x%xu;", nextPC),
			fmt.Sprintf("cpu->cycles += %s;", cycles),
		}), true

	case "cmp2", "chk2":
		ext := bytesBE(raw, 2, 2)
		e := decodeEA(i, &ops[0], size, "bounds", false)
		if e == nil || !e.isMem {
			return nil, false
		}
		bank := "d"
		if ext&0x8000 != 0 {
			bank = "a"
		}
		reg := fmt.Sprintf("cpu->%s[%d]", bank, (ext>>12)&7)
		bits := size * 8
		cast := fmt.Sprintf("int%d_t", bits)
		if ext&0x8000 != 0 && size < 4 {
			cast = fmt.Sprintf("uint%d_t", bits)
		}
		stmts := slices.Concat(e.eaSetup, []string{
			fmt.Sprintf("int32_t value = (%s)%s;", cast, reg),
			fmt.Sprintf("int32_t lower = (int%d_t)f3_read%d(cpu, %s);", bits, bits, e.eaExpr),
			fmt.Sprintf("int32_t upper = (int%d_t)f3_read%d(cpu, %s + %du);", bits, bits, e.eaExpr, size),
			"f3_cc_flush(cpu);",
			"unsigned outside = value < lower || value > upper;",
			"cpu->sr = (uint16_t)((cpu->sr & ~5u) | outside | ((value == lower || value == upper) ? 4u : 0u));",
		})
		if ext&0x800 != 0 {
			stmts = append(stmts, fmt.Sprintf("if (outside) { f3_exception(cpu, 6, 0x%xu); return; }", nextPC))
		}
		return append(stmts, fmt.Sprintf("cpu->pc = 0x%xu;", nextPC), fmt.Sprintf("cpu->cycles += %s;", cycles)), true
	}

	return lowerMain(c)
}

func condKnown(name string) bool {
	_, ok := CondMap[name]
	return ok
}
