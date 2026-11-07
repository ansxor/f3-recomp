package main

import (
	"encoding/binary"
	"slices"
	"strings"
)

func window(rom []byte, a, b int) []byte {
	a = max(0, min(a, len(rom)))
	b = max(a, min(b, len(rom)))
	return rom[a:b]
}

func beUnsigned(b []byte) int {
	n := 0
	for _, x := range b {
		n = n<<8 | int(x)
	}
	return n
}

func beSigned(b []byte) int {
	n := beUnsigned(b)
	if len(b) > 0 && b[0]&0x80 != 0 {
		n -= 1 << (8 * len(b))
	}
	return n
}

func u16(rom []byte, at int) int { return beUnsigned(window(rom, at, at+2)) }

func discoveryBaseMnemonic(insn *Insn) string {
	base, _, _ := strings.Cut(strings.ToLower(insn.Mnemonic), ".")
	return base
}

func isTransfer(base string) bool {
	return inSet(CallMnemonics, base) || inSet(UncondBranchMnemonics, base) || inSet(CondBranchMnemonics, base)
}

func inSet(set map[string]struct{}, key string) bool {
	_, ok := set[key]
	return ok
}

func resolveTarget(insn *Insn, op *Operand) (int, bool) {
	switch {
	case op.Type == OpBrDisp || op.AddressMode == AMBranchDisplacement:
		return int(insn.Address) + 2 + int(op.BrDisp.Disp), true
	case op.AddressMode == AMAbsoluteDataShort || op.AddressMode == AMAbsoluteDataLong:
		return int(op.Imm), true
	case op.AddressMode == AMPCIDisp:
		return int(insn.Address) + 2 + int(op.Mem.Disp), true
	case op.Type == OpImm:
		return int(op.Imm), true
	}
	return 0, false
}

func scanPCIIndexTable(rom []byte, insn *Insn, op *Operand) []int {
	tableAddr := int(insn.Address) + 2 + int(op.Mem.Disp)
	if !(0 <= tableAddr && tableAddr < len(rom)-2) {
		return nil
	}

	firstOff := beSigned(rom[tableAddr : tableAddr+2])
	if firstOff <= 0 || firstOff > 0x7000 {
		return nil
	}

	maxScan := min(firstOff/2, 128)
	if maxScan == 0 {
		return nil
	}

	minOff := 0
	for i := range maxScan {
		ptr := tableAddr + i*2
		if ptr+2 > len(rom) {
			break
		}
		if off := beSigned(rom[ptr : ptr+2]); off > 0 && (minOff == 0 || off < minOff) {
			minOff = off
		}
	}
	if minOff == 0 {
		return nil
	}

	numEntries := min(minOff/2, maxScan)
	if numEntries == 0 {
		return nil
	}

	var entries []int
	for i := range numEntries {
		ptr := tableAddr + i*2
		off := beSigned(window(rom, ptr, ptr+2))
		if off == -1 {
			continue
		}
		target := tableAddr + off
		if 0x400 <= target && target < len(rom) && target%2 == 0 {
			entries = append(entries, target)
		}
	}
	return pySetOrder(entries)
}

func scanAbsoluteTable(d *Machine, rom []byte, tableAddr, destinationOffset int) ([]int, error) {
	var entries []int
	limit := len(rom)
	for index := range 64 {
		ptr := tableAddr + index*4
		if ptr < 0 || ptr+4 > limit {
			break
		}
		target := (int(binary.BigEndian.Uint32(rom[ptr:])) + destinationOffset) & 0xFFFFFFFF
		if !(0x400 <= target && target < len(rom)) || target&1 != 0 {
			break
		}
		insn, ok, err := d.Decode(window(rom, target, target+24), uint64(target))
		if err != nil {
			return nil, err
		}
		if !ok || insn.ID == 0 {
			break
		}
		entries = append(entries, target)
		if target >= tableAddr {
			limit = min(limit, target)
		}
	}
	slices.Sort(entries)
	return slices.Compact(entries), nil
}

func scanPCMemITable(d *Machine, rom []byte, insn *Insn) ([]int, error) {
	// Capstone 5 exposes a signed word base displacement as an unsigned value.
	// Read full-extension widths/signs from the opcode rather than its text/detail.
	raw := insn.Bytes
	extension := beUnsigned(window(raw, 2, 4))
	var baseSize int
	switch (extension >> 4) & 3 {
	case 1:
		baseSize = 0
	case 2:
		baseSize = 2
	case 3:
		baseSize = 4
	default:
		return nil, nil
	}
	indirect := extension & 7
	var outerSize int
	switch indirect {
	case 1:
		outerSize = 0
	case 2:
		outerSize = 2
	case 3:
		outerSize = 4
	default:
		return nil, nil
	}
	displacement := beSigned(window(raw, 4, 4+baseSize))
	outer := beSigned(window(raw, 4+baseSize, 4+baseSize+outerSize))
	table := displacement
	if extension&0x80 == 0 {
		table += int(insn.Address) + 2
	}
	return scanAbsoluteTable(d, rom, table, outer)
}

func scanRegisterTable(d *Machine, rom []byte, insn *Insn) ([]int, error) {
	jump := beUnsigned(window(insn.Bytes, 0, 2))
	indirect := jump&0xFFF8 == 0x4EB0 || jump&0xFFF8 == 0x4EF0
	if indirect {
		if !slices.Equal(window(insn.Bytes, 2, len(insn.Bytes)), []byte{0x01, 0x51}) {
			return nil, nil
		}
	} else if jump&0xFFF8 != 0x4E90 && jump&0xFFF8 != 0x4ED0 {
		return nil, nil
	}
	address := int(insn.Address)
	end := address
	for end >= address-2 && end >= 2 && rom[end-2] == 0x4E && rom[end-1] == 0x71 {
		end -= 2
	}
	pc := end - 4
	if pc < 0 {
		return nil, nil
	}
	load := u16(rom, pc)
	want := 0x207B
	if indirect {
		want = 0x41FB
	}
	if load&0xF1FF != want {
		return nil, nil
	}
	if (load>>9)&7 != jump&7 || rom[pc+2]&1 != 0 {
		return nil, nil
	}
	table := pc + 2 + beSigned(window(rom, pc+3, pc+4))
	return scanAbsoluteTable(d, rom, table, 0)
}

func computedTransfer(insn *Insn) bool {
	base, _, _ := strings.Cut(insn.Mnemonic, ".")
	if base != "jmp" && base != "jsr" {
		return false
	}
	ops := insn.Operands
	if len(ops) == 0 {
		return false
	}
	switch ops[len(ops)-1].AddressMode {
	case AMAbsoluteDataShort, AMAbsoluteDataLong, AMPCIDisp:
		return false
	}
	return true
}

func isPCIIndexMode(m AddressMode) bool {
	return m == AMPCIIndex8BitDisp || m == AMPCIIndexBaseDisp
}

func isPCMemIMode(m AddressMode) bool {
	return m == AMPCMemIPreIndex || m == AMPCMemIPostIndex
}

func scanComputedTargets(d *Machine, rom []byte, insn *Insn, op *Operand) ([]int, error) {
	scanned, err := scanRegisterTable(d, rom, insn)
	if err != nil || len(scanned) > 0 {
		return scanned, err
	}
	switch {
	case isPCIIndexMode(op.AddressMode):
		return scanPCIIndexTable(rom, insn, op), nil
	case isPCMemIMode(op.AddressMode):
		return scanPCMemITable(d, rom, insn)
	}
	return nil, nil
}

func candidateTargets(rom []byte, instructions map[int]*Insn, exclusions Exclusions, scanned []int) []int {
	var targets []int
	for _, target := range slices.Compact(slices.Sorted(slices.Values(scanned))) {
		if target < 0 || target >= len(rom) || target&1 != 0 {
			continue
		}
		if _, ok := instructions[target]; !ok {
			continue
		}
		if exclusions.At(target) != nil {
			continue
		}
		targets = append(targets, target)
	}
	return targets
}

func extractTrap1Tasks(d *Machine, rom []byte) (map[int]struct{}, error) {
	targets := map[int]struct{}{}
	for i := 0; i < len(rom)-2; i += 2 {
		if int(rom[i])<<8|int(rom[i+1]) != 0x4E41 {
			continue
		}
		var dis []Insn
		start := max(0, i-24)
		for code, addr := rom[start:i], start; len(code) > 0; {
			insn, ok, err := d.Decode(code, uint64(addr))
			if err != nil {
				return nil, err
			}
			if !ok || insn.Size == 0 {
				break
			}
			dis = append(dis, insn)
			code, addr = code[insn.Size:], addr+int(insn.Size)
		}
		for j := len(dis) - 1; j >= 0; j-- {
			ins := &dis[j]
			base := ins.Base()
			if base == "pea" && len(ins.Operands) > 0 {
				if t, ok := resolveTarget(ins, &ins.Operands[0]); ok && 0x400 <= t && t < len(rom) && t%2 == 0 {
					targets[t] = struct{}{}
				}
				break
			} else if base == "move" && len(ins.Operands) == 2 {
				if ins.Operands[0].Type == OpImm {
					t := int(ins.Operands[0].Imm)
					if 0x400 <= t && t < len(rom) && t%2 == 0 {
						targets[t] = struct{}{}
					}
				}
				break
			}
		}
	}
	return targets, nil
}

func validateCodeSequence(d *Machine, rom []byte, target int) (bool, error) {
	if target%2 != 0 {
		return false, nil
	}
	cur := target
	for range 32 {
		if cur >= len(rom) {
			return false, nil
		}
		insn, ok, err := d.Decode(window(rom, cur, cur+24), uint64(cur))
		if err != nil {
			return false, err
		}
		if !ok || insn.ID == 0 || insn.Size%2 != 0 || cur+int(insn.Size) > len(rom) {
			return false, nil
		}
		base := insn.Base()
		if base == "illegal" || base == "dc" {
			return false, nil
		}
		if inSet(TerminalMnemonics, base) || inSet(UncondBranchMnemonics, base) || inSet(CondBranchMnemonics, base) {
			return true, nil
		}
		cur += int(insn.Size)
	}
	return false, nil
}

func extractLeaMoveCallbacks(d *Machine, rom []byte) (map[int]struct{}, error) {
	callbacks := map[int]struct{}{}
	for i := 0; i < len(rom)-8; i += 2 {
		w1 := int(rom[i])<<8 | int(rom[i+1])
		if w1&0xF1FF != 0x41FA {
			continue
		}
		reg := (w1 >> 9) & 7
		target := i + 2 + beSigned(rom[i+2:i+4])
		if !(0x400 <= target && target < len(rom) && target%2 == 0) {
			continue
		}
		w2 := int(rom[i+4])<<8 | int(rom[i+5])
		storesCallback := w2&0xF000 == 0x2000 && (w2>>3)&7 == 1 && w2&7 == reg
		registerLink := w2&0xFFC0 == 0x4EC0
		if storesCallback || registerLink {
			valid, err := validateCodeSequence(d, rom, target)
			if err != nil {
				return nil, err
			}
			if valid {
				callbacks[target] = struct{}{}
			}
		}
	}
	return callbacks, nil
}

func extractVectorSeeds(rom []byte) map[int]struct{} {
	seeds := map[int]struct{}{}
	valid := func(v int) bool { return 0x400 <= v && v < len(rom) && v%2 == 0 }
	if reset := int(binary.BigEndian.Uint32(rom[4:])); valid(reset) {
		seeds[reset] = struct{}{}
	}
	for i := 2; i < 256; i++ {
		if v := int(binary.BigEndian.Uint32(rom[i*4:])); valid(v) {
			seeds[v] = struct{}{}
		}
	}
	return seeds
}
