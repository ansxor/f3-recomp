package main

import (
	"bufio"
	"embed"
	"fmt"
	"math/bits"
	"strconv"
	"strings"
)

//go:embed 68000_cycles.csv 68020_cycles.csv
var cycleCSVs embed.FS

var opcodeCycles68020 = loadOpcodeCycles68020()

func loadOpcodeCycles68020() *[65536]uint8 {
	data, err := cycleCSVs.ReadFile("68020_cycles.csv")
	if err != nil {
		panic(err)
	}
	table := new([65536]uint8)
	scanner := bufio.NewScanner(strings.NewReader(string(data)))
	for scanner.Scan() {
		line := scanner.Text()
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}
		fields := strings.Split(line, ",")
		if len(fields) != 3 {
			panic(fmt.Sprintf("cycles: malformed 68020_cycles.csv line %q", line))
		}
		mask, err1 := strconv.ParseUint(fields[0], 16, 16)
		match, err2 := strconv.ParseUint(fields[1], 16, 16)
		cycles, err3 := strconv.ParseUint(fields[2], 10, 8)
		if err1 != nil || err2 != nil || err3 != nil {
			panic(fmt.Sprintf("cycles: malformed 68020_cycles.csv line %q", line))
		}
		free := ^mask & 0xFFFF
		subset := free
		for {
			table[match|subset] = uint8(cycles)
			if subset == 0 {
				break
			}
			subset = (subset - 1) & free
		}
	}
	return table
}

var fullIndexCycles = func() (t [64]int) {
	copy(t[16:], []int{0, 5, 7, 7, 0, 5, 7, 7, 0, 5, 7, 7, 0, 5, 7, 7})
	copy(t[32:], []int{2, 7, 9, 9})
	for _, i := range []int{36, 40, 44} {
		copy(t[i:], []int{0, 7, 9, 9})
	}
	copy(t[48:], []int{6, 11, 13, 13})
	for _, i := range []int{52, 56, 60} {
		copy(t[i:], []int{0, 11, 13, 13})
	}
	return
}()

func beUint(b []byte, start, n int) int {
	start = min(start, len(b))
	v := 0
	for _, c := range b[start:min(start+n, len(b))] {
		v = v<<8 | int(c)
	}
	return v
}

func (i *Insn) CycleCost() string {
	raw := i.Bytes
	opcode := beUint(raw, 0, 2)
	mnemonic, _, _ := strings.Cut(i.Mnemonic, ".")
	cycles := int(opcodeCycles68020[opcode])
	if mnemonic == "movem" {
		perRegister := 3
		if opcode&0x0400 != 0 {
			perRegister = 4
		}
		cycles += bits.OnesCount(uint(beUint(raw, 2, 2))) * perRegister
	}
	if mnemonic == "reset" {
		cycles += 518
	}
	offset := i.PCBase() - int(i.Address)
	size := 4
	if i.ID != 0 {
		size = int(i.OpSize.Size)
	}
	type operand struct{ mode, reg, position int }
	sourceMode, sourceReg := (opcode>>3)&7, opcode&7
	operands := []operand{{sourceMode, sourceReg, offset}}
	if mnemonic == "move" {
		destinationOffset := offset + i.EAExtensionBytes(offset, sourceMode, sourceReg, size)
		operands = append(operands, operand{(opcode >> 6) & 7, (opcode >> 9) & 7, destinationOffset})
	}
	hasMem := false
	for n := range i.Operands {
		if i.Operands[n].Type == OpMem {
			hasMem = true
			break
		}
	}
	if hasMem {
		for _, op := range operands {
			if op.mode == 6 || (op.mode == 7 && op.reg == 3) {
				extension := beUint(raw, op.position, 2)
				if extension&0x100 != 0 {
					cycles += fullIndexCycles[extension&0x3F]
				}
			}
		}
	}
	if opcode&0xF000 == 0x6000 && (opcode>>8)&15 >= 2 && i.Size == 2 {
		return fmt.Sprintf("(cpu->pc == 0x%xu ? %du : %du)", i.Address+uint64(i.Size), cycles-2, cycles)
	}
	return fmt.Sprintf("%du", cycles)
}

var opcodeCycles68000 = loadOpcodeCycles68000()

func loadOpcodeCycles68000() *[65536]uint8 {
	data, err := cycleCSVs.ReadFile("68000_cycles.csv")
	if err != nil {
		panic(err)
	}
	table := new([65536]uint8)
	scanner := bufio.NewScanner(strings.NewReader(string(data)))
	for scanner.Scan() {
		line := scanner.Text()
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}
		fields := strings.Split(line, ",")
		if len(fields) != 3 {
			panic(fmt.Sprintf("cycles: malformed 68000_cycles.csv line %q", line))
		}
		mask, err1 := strconv.ParseUint(fields[0], 16, 16)
		match, err2 := strconv.ParseUint(fields[1], 16, 16)
		cycles, err3 := strconv.ParseUint(fields[2], 10, 8)
		if err1 != nil || err2 != nil || err3 != nil {
			panic(fmt.Sprintf("cycles: malformed 68000_cycles.csv line %q", line))
		}
		free := ^mask & 0xFFFF
		for sub := free; ; sub = (sub - 1) & free {
			table[match|sub] = uint8(cycles)
			if sub == 0 {
				break
			}
		}
	}
	return table
}

func beInt(b []byte) int {
	n := 0
	for _, v := range b {
		n = n<<8 | int(v)
	}
	return n
}

func opcodeOf(i *Insn) int {
	return beInt(i.Bytes[:min(2, len(i.Bytes))])
}

func soundBaseMnemonic(i *Insn) string {
	mnemonic, _, _ := strings.Cut(i.Mnemonic, ".")
	return strings.ToLower(mnemonic)
}

func (i *Insn) SoundCycleCost() string {
	opcode := opcodeOf(i)
	mnem, _, _ := strings.Cut(i.Mnemonic, ".")
	cycles := int(opcodeCycles68000[opcode])
	cond := (opcode >> 8) & 15
	reg := (opcode >> 9) & 7
	switch {
	case opcode&0xF000 == 0x6000 && cond >= 2:
		delta := 2
		if i.Size == 2 {
			delta = -2
		}
		return fmt.Sprintf("(f3_eval_cond(cpu, %d) ? %du : %du)", cond, cycles, cycles+delta)
	case mnem == "movem":
		count := bits.OnesCount(uint(beInt(slicePart(i.Bytes, 2, 4))))
		per := 4
		if opcode&0x40 != 0 {
			per = 8
		}
		return fmt.Sprintf("%du", cycles+count*per)
	case opcode&0xF000 == 0xE000 && opcode&0xC0 != 0xC0:
		var count string
		if opcode&0x20 != 0 {
			count = fmt.Sprintf("(cpu->d[%d] & 63u)", reg)
		} else {
			count = fmt.Sprintf("%du", cmpOr(reg, 8))
		}
		return fmt.Sprintf("(%du + 2u * %s)", cycles, count)
	case (mnem == "bchg" || mnem == "bclr" || mnem == "bset") && opcode&0x38 == 0:
		var bit string
		if opcode&0x100 != 0 {
			bit = fmt.Sprintf("cpu->d[%d]", reg)
		} else {
			bit = fmt.Sprintf("%du", i.Operands[0].Imm)
		}
		return fmt.Sprintf("(%du - (((%s) & 31u) < 16u ? 2u : 0u))", cycles, bit)
	case opcode&0xF0F8 == 0x50C0 && cond >= 2:
		return fmt.Sprintf("(%du + (f3_eval_cond(cpu, %d) ? 2u : 0u))", cycles, cond)
	case mnem == "reset":
		return "132u"
	}
	return fmt.Sprintf("%du", cycles)
}

func slicePart(b []byte, from, to int) []byte {
	from = min(from, len(b))
	return b[from:min(to, len(b))]
}

func cmpOr(v, fallback int) int {
	if v == 0 {
		return fallback
	}
	return v
}
