package main

import (
	"fmt"
	"regexp"
	"strings"
)

type rewrite struct {
	re   *regexp.Regexp
	with string
}

var (
	mulDivReads = rewrite{regexp.MustCompile(`\bf3_read(8|16|32)\b`), "f3_sound_read${1}"}
	prefixRemap = []rewrite{
		{regexp.MustCompile(`\bf3_read([0-9]+)\b`), "f3_sound_read${1}"},
		{regexp.MustCompile(`\bf3_write([0-9]+)\b`), "f3_sound_write${1}"},
		{regexp.MustCompile(`\bf3_set_sr\b`), "f3_sound_set_sr"},
		{regexp.MustCompile(`\bf3_exception\b`), "f3_sound_exception"},
		{regexp.MustCompile(`\bf3_reset_devices\b`), "f3_sound_reset_devices"},
		{regexp.MustCompile(`\bf3_div([su])_w\b`), "f3_sound_div${1}_w"},
		{regexp.MustCompile(`\bf3_mul([su])_w\b`), "f3_sound_mul${1}_w"},
	}
	cyclesStatement = regexp.MustCompile(`cpu->cycles \+= [^;]+;`)
)

func trap(pc int, vector int, savedPC int) []string {
	return []string{
		fmt.Sprintf("cpu->pc = 0x%08xu;", pc),
		fmt.Sprintf("f3_sound_exception(cpu, %du, 0x%08xu);", vector, savedPC),
		"return;",
	}
}

func (i *Insn) LowerSound() (stmts []string, ok bool) {
	opcode := opcodeOf(i)
	mnem := i.Base()
	address := int(i.Address)
	nextPC := address + int(i.Size)

	if opcode>>12 == 10 || opcode>>12 == 15 {
		vector := 11
		if opcode>>12 == 10 {
			vector = 10
		}
		return trap(address, vector, address), true
	}
	if opcode == 0x4AFC || opcode&0xFFF8 == 0x4848 {
		return trap(address, 4, address), true
	}

	switch mnem {
	case "rte":
		return []string{
			fmt.Sprintf("cpu->pc = 0x%08xu;", address),
			"f3_sound_rte(cpu);",
			"return;",
		}, true
	case "stop":
		if len(i.Operands) == 0 || i.Operands[0].Type != OpImm {
			return nil, false
		}
		return []string{
			fmt.Sprintf("cpu->pc = 0x%08xu;", address),
			fmt.Sprintf("f3_sound_stop(cpu, 0x%04xu, 0x%08xu);", i.Operands[0].Imm&0xFFFF, nextPC),
			"return;",
		}, true
	case "trap":
		if len(i.Operands) == 0 || i.Operands[0].Type != OpImm {
			return nil, false
		}
		return trap(address, 32+int(i.Operands[0].Imm&0x0F), nextPC), true
	}

	if opcode&0xF0F8 == 0x50C8 {
		cond := (opcode >> 8) & 0x0F
		reg := opcode & 7
		if len(i.Operands) < 2 {
			return nil, false
		}
		target := (address + 2 + int(i.Operands[1].BrDisp.Disp)) & 0xFFFFFFFF
		return []string{
			fmt.Sprintf("cpu->pc = 0x%08xu;", address),
			fmt.Sprintf("if (!f3_sound_eval_cond(cpu, %d)) {", cond),
			fmt.Sprintf("    uint16_t count = (uint16_t)(cpu->d[%d] & 0xffffu);", reg),
			"    count--;",
			fmt.Sprintf("    cpu->d[%d] = (cpu->d[%d] & 0xffff0000u) | count;", reg, reg),
			"    if (count != 0xffffu) {",
			fmt.Sprintf("        cpu->pc = 0x%08xu;", target),
			"        cpu->cycles += 10u;",
			"        return;",
			"    }",
			fmt.Sprintf("    cpu->pc = 0x%08xu;", nextPC),
			"    cpu->cycles += 14u;",
			"    return;",
			"}",
			fmt.Sprintf("cpu->pc = 0x%08xu;", nextPC),
			"cpu->cycles += 12u;",
			"return;",
		}, true
	}

	switch mnem {
	case "mulu", "muls", "divu", "divs":
		return lowerMulDiv(i, mnem, opcode, nextPC)
	}

	stmts, ok = i.Lower(CCRAll)
	if !ok {
		return nil, false
	}
	return remap(i, mnem, stmts, nextPC), true
}

func lowerMulDiv(insn *Insn, mnem string, opcode, nextPC int) ([]string, bool) {
	ea := insn.DecodeEA(&insn.Operands[0], 2, "source", true)
	if ea == nil {
		return nil, false
	}
	register := (opcode >> 9) & 7
	base := int(opcodeCycles68000[opcode])
	statements := append(append(append([]string{}, ea.EASetup...), ea.ReadStmts...),
		fmt.Sprintf("uint16_t source = (uint16_t)(%s);", ea.ValExpr),
		fmt.Sprintf("uint32_t destination = cpu->d[%d];", register),
	)
	if strings.HasPrefix(mnem, "mul") {
		cast := "uint16_t"
		if mnem == "muls" {
			cast = "int16_t"
		}
		statements = append(statements,
			fmt.Sprintf("cpu->cycles += %du + f3_sound_%s_cycles(source);", base, mnem),
			fmt.Sprintf("cpu->d[%d] = f3_%s_w(cpu, (%s)source, (%s)destination);", register, mnem, cast, cast),
		)
	} else {
		cast := "uint16_t"
		if mnem == "divs" {
			cast = "int16_t"
		}
		cost := fmt.Sprintf("%du", base)
		if mnem == "divu" {
			cost = fmt.Sprintf("%du + f3_sound_divu_cycles(destination, source)", base-140)
		}
		statements = append(statements,
			"int exception = 0;",
			fmt.Sprintf("uint32_t result = f3_sound_%s_w(cpu, (%s)source, destination, 0x%xu, &exception);", mnem, cast, nextPC),
			"if (exception) return;",
			fmt.Sprintf("cpu->cycles += %s;", cost),
			fmt.Sprintf("cpu->d[%d] = result;", register),
		)
	}
	statements = append(statements, fmt.Sprintf("cpu->pc = 0x%xu;", nextPC))
	for i, s := range statements {
		statements[i] = mulDivReads.re.ReplaceAllString(s, mulDivReads.with)
	}
	return statements, true
}

func remap(insn *Insn, mnem string, stmts []string, nextPC int) []string {
	remapped := []string{fmt.Sprintf("const uint32_t sound_cycles = %s;", insn.SoundCycleCost())}
	writesSR := false
	for _, s := range stmts {
		if strings.Contains(s, "f3_set_sr(") {
			writesSR = true
			break
		}
	}
	readsSR := mnem == "move" && !writesSR &&
		insn.Operands[0].Type == OpReg && insn.Operands[0].Reg == RegSR
	pcStatement := fmt.Sprintf("cpu->pc = 0x%08xu;", nextPC)
	for _, s := range stmts {
		switch {
		case readsSR && strings.Contains(s, "f3_exception(cpu, 8u,"):
			continue
		case strings.Contains(s, "f3_reset_devices("):
			continue
		case writesSR && s == pcStatement:
			continue
		}
		if strings.Contains(s, "f3_set_sr(") {
			remapped = append(remapped, pcStatement)
		}
		for _, r := range prefixRemap {
			s = r.re.ReplaceAllString(s, r.with)
		}
		s = cyclesStatement.ReplaceAllString(s, "cpu->cycles += sound_cycles;")
		remapped = append(remapped, s)
	}
	return remapped
}
