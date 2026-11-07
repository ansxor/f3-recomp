package main

import (
	"fmt"
	"math/rand/v2"
	"os"
	"os/exec"
	"path/filepath"
	"slices"
	"strconv"
	"strings"
	"testing"
)

const (
	equivalencePrograms = 120
	equivalenceStates   = 40
)

type randomItem struct {
	words []int
	kind  string
	cond  int
}

type generator struct{ *rand.Rand }

func (g generator) dn() int   { return g.IntN(8) }
func (g generator) size() int { return g.IntN(3) }

func choose[T any](g generator, options ...T) T { return options[g.IntN(len(options))] }

func (g generator) imm(size int) []int {
	if size < 2 {
		return []int{g.IntN(1 << 16)}
	}
	return []int{g.IntN(1 << 16), g.IntN(1 << 16)}
}

func (g generator) ea(size int) (int, []int) {
	switch choose(g, "d", "d", "m", "i") {
	case "d":
		return g.dn(), nil
	case "m":
		return 0x10 | g.dn(), nil
	}
	return 0x3C, g.imm(size)
}

func words(w ...int) randomItem { return randomItem{words: w} }

func (g generator) item() randomItem {
	choice := g.IntN(36)
	switch {
	case choice == 0:
		return words(0x7000 | g.dn()<<9 | g.IntN(256))
	case choice <= 6:
		base := []int{0xD000, 0x9000, 0xC000, 0x8000, 0xB000, 0xD000, 0x9000}[choice]
		size := g.size()
		mode, ext := g.ea(size)
		return words(append([]int{base | g.dn()<<9 | size<<6 | mode}, ext...)...)
	case choice == 7:
		base, size := choose(g, 0xD100, 0x9100, 0xC100, 0x8100, 0xB100), g.size()
		return words(base | g.dn()<<9 | size<<6 | choose(g, 0x10|g.dn(), 0x10|g.dn(), g.dn()))
	case choice == 8:
		base := choose(g, 0xD0C0, 0x90C0, 0xB0C0)
		size := choose(g, 0, 0x100)
		mode, ext := g.ea(1 + size/0x100)
		return words(append([]int{base | size | g.dn()<<9 | mode}, ext...)...)
	case choice <= 11:
		base := choose(g, 0x0600, 0x0400, 0x0C00, 0x0200, 0x0000, 0x0A00)
		size := g.size()
		head := base | size<<6 | choose(g, g.dn(), 0x10|g.dn())
		if size == 0 {
			return words(head, g.IntN(256))
		}
		return words(append([]int{head}, g.imm(size)...)...)
	case choice == 12:
		size := g.size()
		alt := g.dn()
		if size != 0 {
			alt = 0x08 | g.dn()
		}
		mode := choose(g, g.dn(), g.dn(), 0x10|g.dn(), alt)
		return words(0x5000 | g.IntN(8)<<9 | choose(g, 0, 0x100) | size<<6 | mode)
	case choice == 13:
		base := choose(g, 0x4400, 0x4000, 0x4600, 0x4200, 0x4A00)
		return words(base | g.size()<<6 | choose(g, g.dn(), g.dn(), 0x10|g.dn()))
	case choice == 14:
		return words(choose(g, 0x4880, 0x48C0, 0x49C0, 0x4840, 0x4800, 0x4AC0) | g.dn())
	case choice == 15:
		base := choose(g, 0xD100, 0x9100) | g.size()<<6
		if g.Float64() < 0.3 {
			base = choose(g, 0xC100, 0x8100)
		}
		return words(base | choose(g, 0x0, 0x8) | g.dn()<<9 | g.dn())
	case choice <= 18:
		kind := g.IntN(4)
		if g.Float64() < 0.15 {
			return words(0xE0C0 | kind<<9 | choose(g, 0, 0x100) | 0x10 | g.dn())
		}
		return words(0xE000 | g.IntN(8)<<9 | choose(g, 0, 0x100) | g.size()<<6 | choose(g, 0, 0x20) | kind<<3 | g.dn())
	case choice == 19:
		return words(choose(g, 0xC0C0, 0xC1C0, 0x80C0, 0x81C0) | g.dn()<<9 | g.dn())
	case choice == 20:
		return words(0x4C00|g.dn(), g.dn()<<12|choose(g, 0, 0x800))
	case choice <= 22:
		kind := g.IntN(4) << 6
		if g.Float64() < 0.5 {
			return words(0x0100 | g.dn()<<9 | kind | g.dn())
		}
		return words(0x0800|kind|g.dn(), g.IntN(32))
	case choice == 23:
		return words(0x50C0 | g.IntN(16)<<8 | g.dn())
	case choice == 24:
		return words(0xE8C0|g.IntN(8)<<8|g.dn(), g.dn()<<12|g.IntN(32)<<6|g.IntN(32))
	case choice <= 27:
		size := choose(g, 1, 2, 3)
		eaSize := 1
		switch size {
		case 1:
			eaSize = 0
		case 2:
			eaSize = 2
		}
		mode, ext := g.ea(eaSize)
		dest := choose(g, 0, 0, 2, 3, 4)
		return words(append([]int{size<<12 | g.dn()<<9 | dest<<6 | mode}, ext...)...)
	case choice == 28:
		return choose(g,
			words(0x44C0|g.dn()),
			words(0x42C0|g.dn()),
			words(0x023C, g.IntN(256)),
			words(0x003C, g.IntN(256)),
			words(0x0A3C, g.IntN(256)),
		)
	case choice == 29:
		return choose(g,
			words(0xC140|g.dn()<<9|g.dn()),
			words(0xC148|g.dn()<<9|g.dn()),
			words(0x2040|g.dn()<<9|g.dn()),
			words(0x41D0|g.dn()<<9|g.dn()),
		)
	case choice == 30:
		return words(0xB108 | g.dn()<<9 | g.size()<<6 | g.dn())
	case choice == 31:
		return choose(g,
			words(0x50FC|g.IntN(16)<<8),
			words(0x4E76),
			words(0x4180|g.dn()<<9|g.dn()),
			words(0x4100|g.dn()<<9|g.dn()),
		)
	case choice == 32:
		return words(0x4E71)
	case choice <= 34:
		return randomItem{kind: "b", cond: 2 + g.IntN(14)}
	}
	return randomItem{kind: "db", cond: g.IntN(16)}
}

func (it randomItem) count() int {
	switch it.kind {
	case "":
		return len(it.words)
	case "b":
		return 1
	}
	return 2
}

func be16(code []byte, v int) []byte { return append(code, byte(v>>8), byte(v)) }

func (g generator) program(t *testing.T, dec *Machine, base int) map[int]*Insn {
	t.Helper()
	for range 100 {
		items := make([]randomItem, 8+g.IntN(7))
		for i := range items {
			items[i] = g.item()
		}
		items = append(items, words(0x4E40))
		starts := []int{base}
		for _, item := range items {
			starts = append(starts, starts[len(starts)-1]+2*item.count())
		}
		var code []byte
		for index, item := range items {
			switch {
			case item.kind == "":
				for _, w := range item.words {
					code = be16(code, w)
				}
			case index > len(items)-3:
				for range item.count() {
					code = be16(code, 0x4E71)
				}
			default:
				target := starts[index+2+g.IntN(len(items)-index-2)]
				if item.kind == "b" {
					code = be16(code, 0x6000|item.cond<<8|((target-starts[index]-2)&0xff))
				} else {
					code = be16(code, 0x50C8|item.cond<<8|g.IntN(8))
					code = be16(code, (target-starts[index]-2)&0xffff)
				}
			}
		}
		insns := decodeAll(t, dec, code, base)
		instructions := instructionMap(insns)
		match := len(insns) == len(starts)-1
		for i, insn := range insns {
			if !match || int(insn.Address) != starts[i] {
				match = false
				break
			}
			if _, ok := insn.Lower(CCRAll); !ok {
				match = false
				break
			}
		}
		if match {
			return instructions
		}
	}
	t.Fatal("could not build a decodable program")
	return nil
}

func cFunction(name string, instructions map[int]*Insn, liveOut func(pc int) int) string {
	pcs := sortedKeys(instructions)
	lines := []string{fmt.Sprintf("static void %s(f3_cpu *cpu) {", name), "    for (;;) {", "    switch (cpu->pc) {"}
	for _, pc := range pcs {
		lines = append(lines, fmt.Sprintf("    case 0x%xu: goto L_%x;", pc, pc))
	}
	lines = append(lines, "    default: return;", "    }")
	for _, pc := range pcs {
		lines = append(lines, fmt.Sprintf("L_%x: {", pc))
		statements, _ := instructions[pc].Lower(liveOut(pc))
		for _, statement := range statements {
			lines = append(lines, "    "+statement)
		}
		lines = append(lines, "    continue;", "}")
	}
	return strings.Join(append(lines, "    }", "}"), "\n")
}

func TestElidedAndExactFlagsAgreeFromAnyEntryAndPendingState(t *testing.T) {
	cCompiler(t)
	root := generateRepoRoot(t)
	g := generator{rand.New(rand.NewPCG(0xF3, 0))}
	dec := newDecoder(t)
	programs := make([]map[int]*Insn, equivalencePrograms)
	merged := map[int]*Insn{}
	for i := range programs {
		programs[i] = g.program(t, dec, 0x1000*(i+1))
		for pc, insn := range programs[i] {
			merged[pc] = insn
		}
	}
	live := NewLiveness()
	live.Analyse(merged, nil, nil)
	prelude, err := os.ReadFile("testdata/flag_equivalence.c")
	if err != nil {
		t.Fatal(err)
	}
	tail, err := os.ReadFile("testdata/flag_equivalence_tail.c")
	if err != nil {
		t.Fatal(err)
	}
	var entries, lengths []string
	maxLen := 0
	for _, program := range programs {
		pcs := sortedKeys(program)
		hexes := make([]string, len(pcs))
		for i, pc := range pcs {
			hexes[i] = fmt.Sprintf("0x%xu", pc)
		}
		entries = append(entries, "    {"+strings.Join(hexes, ", ")+"}")
		lengths = append(lengths, strconv.Itoa(len(pcs)))
		maxLen = max(maxLen, len(pcs))
	}
	tailText := strings.NewReplacer(
		"@PROGRAMS@", strconv.Itoa(len(programs)),
		"@STATES@", strconv.Itoa(equivalenceStates),
		"@MAXLEN@", strconv.Itoa(maxLen),
		"@STARTS@", strings.Join(entries, ",\n"),
		"@LENGTHS@", strings.Join(lengths, ", "),
	).Replace(string(tail))
	source := strings.Join([]string{
		string(prelude),
		cFunction("run_exact", merged, func(int) int { return CCRAll }),
		cFunction("run_elided", merged, func(pc int) int { return live.LiveOut[pc] }),
		tailText,
	}, "\n")
	dir := t.TempDir()
	path := filepath.Join(dir, "equivalence.c")
	if err := os.WriteFile(path, []byte(source), 0o644); err != nil {
		t.Fatal(err)
	}
	executable := filepath.Join(dir, "equivalence")
	if out, err := exec.Command(cCompiler(t), "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror",
		"-I", root, "-I", filepath.Join(root, "include"), path, "-o", executable).CombinedOutput(); err != nil {
		t.Fatalf("compile: %v\n%s", err, out)
	}
	out, err := exec.Command(executable).Output()
	if err == nil {
		return
	}
	index, _ := strconv.Atoi(strings.Fields(string(out))[1])
	var listing []string
	for _, pc := range sortedKeys(programs[index]) {
		insn := programs[index][pc]
		listing = append(listing, fmt.Sprintf("%x: %s %s  live_out=%#x", pc, insn.Mnemonic, insn.OpStr, live.LiveOut[pc]))
	}
	t.Fatalf("%s\n%s", out, strings.Join(slices.Clone(listing), "\n"))
}
