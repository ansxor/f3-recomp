package main

import (
	"encoding/hex"
	"fmt"
	"maps"
	"regexp"
	"slices"
	"strconv"
	"strings"
	"testing"
)

type livenessChunk struct {
	addr int
	code string
}

func livenessOpenDecoder(t testing.TB) *Machine {
	t.Helper()
	d, err := OpenMachine(ModeBigEndian | Mode68020)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { d.Close() })
	return d
}

func livenessDecode(t testing.TB, d *Machine, livenessChunks ...livenessChunk) map[int]*Insn {
	t.Helper()
	instructions := map[int]*Insn{}
	for _, c := range livenessChunks {
		code, err := hex.DecodeString(c.code)
		if err != nil {
			t.Fatal(err)
		}
		for off := 0; off < len(code); {
			insn, ok, err := d.Decode(code[off:], uint64(c.addr+off))
			if err != nil {
				t.Fatal(err)
			}
			if !ok {
				break
			}
			instructions[int(insn.Address)] = &insn
			off += int(insn.Size)
		}
	}
	return instructions
}

func analyse(t *testing.T, pinned []int, livenessChunks ...livenessChunk) *Liveness {
	t.Helper()
	liveness := NewLiveness()
	liveness.Analyse(livenessDecode(t, livenessOpenDecoder(t), livenessChunks...), nil, pinned)
	return liveness
}

func TestLiveOut(t *testing.T) {
	const (
		x   = CCRX
		z   = CCRZ
		all = CCRAll
	)
	tests := []struct {
		name           string
		pinned         []int
		livenessChunks []livenessChunk
		want           map[int]int
	}{
		{"overwritten before read", nil, []livenessChunk{{0x400, "700172024e75"}}, map[int]int{0x400: x, 0x402: all}},
		{"compare feeds beq", nil, []livenessChunk{{0x400, "b0816702760078004e75"}}, map[int]int{0x400: z | x}},
		{"flagless instruction between", nil, []livenessChunk{{0x400, "b08120406702760078004e75"}}, map[int]int{0x400: z | x}},
		{"only x live after add", nil, []livenessChunk{{0x400, "d2807400d98342c5"}}, map[int]int{0x400: x}},
		{"partial writer preserves x", nil, []livenessChunk{{0x400, "d240b443e3544e75"}}, map[int]int{0x402: x, 0x400: x}},
		{"logic producer preserves x", nil, []livenessChunk{{0x400, "d2402602e3544e75"}}, map[int]int{0x400: x}},
		{"rts boundary", nil, []livenessChunk{{0x400, "b0814e75"}}, map[int]int{0x400: all}},
		{"computed jmp boundary", nil, []livenessChunk{{0x400, "b0814ed0"}}, map[int]int{0x400: all}},
		{"falling off emitted code", nil, []livenessChunk{{0x400, "b081"}}, map[int]int{0x400: all}},
		{"bra to emitted instruction", nil, []livenessChunk{{0x400, "b08160044e714e7176004e75"}}, map[int]int{0x400: x}},
		{"bsr to unknown code", nil, []livenessChunk{{0x400, "b08161000100"}}, map[int]int{0x400: all}},
		{"callee rewrites flags", nil, []livenessChunk{{0x400, "b081610000fa76004e75"}, {0x4FE, "76004e75"}}, map[int]int{0x400: x}},
		{"callee leaves flags to rts", nil, []livenessChunk{{0x400, "b081610000fa76004e75"}, {0x4FE, "4e714e75"}}, map[int]int{0x400: all}},
		{"loop back edge", nil, []livenessChunk{{0x400, "76006706b44160fa4e717a004e75"}}, map[int]int{0x404: z | x, 0x400: z | x}},
		{"pinned reads all", []int{0x402}, []livenessChunk{{0x400, "700172024e75"}}, map[int]int{0x400: all}},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			l := analyse(t, tt.pinned, tt.livenessChunks...)
			for pc, want := range tt.want {
				if got := l.LiveOut[pc]; got != want {
					t.Errorf("LiveOut[%#x] = %#x, want %#x", pc, got, want)
				}
			}
		})
	}
}

func TestKnownCallFlowsOnlyIntoCallee(t *testing.T) {
	for _, callee := range []string{"76004e75", "4e714e75"} {
		l := analyse(t, nil, livenessChunk{0x400, "b081610000fa76004e75"}, livenessChunk{0x4FE, callee})
		if got := l.Succs[0x402]; !slices.Equal(got, []int{0x4FE}) {
			t.Errorf("callee %s: Succs[0x402] = %v, want [0x4fe]", callee, got)
		}
	}
}

func TestReachingWriters(t *testing.T) {
	l := analyse(t, nil, livenessChunk{0x400, "70004e71670272014e71"})
	tests := []struct {
		pc, flags int
		writers   []int
		open      bool
	}{
		{0x404, CCRZ, []int{0x400}, false},
		{0x408, CCRZ, []int{0x400, 0x406}, false},
		{0x404, CCRX, nil, true},
	}
	for _, tt := range tests {
		writers, open := l.ReachingWriters(tt.pc, tt.flags)
		if !slices.Equal(writers, tt.writers) || open != tt.open {
			t.Errorf("ReachingWriters(%#x, %#x) = %v, %v; want %v, %v", tt.pc, tt.flags, writers, open, tt.writers, tt.open)
		}
	}
}

func TestEffects(t *testing.T) {
	const (
		all  = CCRAll
		nzvc = CCRNZVC
		x    = CCRX
		z    = CCRZ
		n    = CCRN
		v    = CCRV
	)
	tests := []struct {
		code          string
		reads, writes int
	}{
		{"7001", 0, nzvc},
		{"d240", 0, all},
		{"5240", 0, all},
		{"5288", 0, 0},
		{"b041", 0, nzvc},
		{"d983", x | z, all},
		{"c101", x | z, all},
		{"81410000", 0, 0},
		{"e348", 0, all},
		{"e3a8", x, nzvc},
		{"e358", 0, nzvc},
		{"e354", x, all},
		{"6702", z, 0},
		{"6e02", n | v | z, 0},
		{"56c8fffe", z, 0},
		{"51c8fffe", 0, 0},
		{"57c0", z, 0},
		{"5dc0", n | v, 0},
		{"42c2", all, 0},
		{"44c0", 0, all},
		{"46c0", all, 0},
		{"2602", 0, nzvc},
		{"2040", 0, 0},
		{"023c00ef", 0x0F, x},
		{"003c0010", 0x0F, x},
		{"0a3c0010", x, 0},
		{"80c1", all, 0},
		{"4afc", all, 0},
		{"a000", all, 0},
		{"4e71", 0, 0},
	}
	d := livenessOpenDecoder(t)
	for _, tt := range tests {
		t.Run(tt.code, func(t *testing.T) {
			code, _ := hex.DecodeString(tt.code + "000000000000")
			insn, ok, err := d.Decode(code, 0x400)
			if err != nil || !ok {
				t.Fatalf("livenessDecode: ok=%v err=%v", ok, err)
			}
			if e := insn.FlagEffects(); e.Reads != tt.reads || e.Writes != tt.writes {
				t.Errorf("Effects = reads %#x writes %#x, want reads %#x writes %#x", e.Reads, e.Writes, tt.reads, tt.writes)
			}
		})
	}
}

func TestConditionFlagsCoverCondMap(t *testing.T) {
	for _, name := range slices.Sorted(maps.Keys(CondMap)) {
		cond := CondMap[name]
		if ConditionFlags(cond) != condFlags[cond] || ConditionFlags(cond+16) != condFlags[cond] {
			t.Errorf("ConditionFlags(%s=%d) inconsistent", name, cond)
		}
	}
}

var (
	ccOpRE    = regexp.MustCompile(`F3_CC_OP_(LOGIC|ADD|SUB|CMP)`)
	evalCond  = regexp.MustCompile(`f3_eval_cond\(cpu, (\d+)\)`)
	notX      = "~0x10u) |"
	extension = [][]byte{make([]byte, 10), {0x0c, 0x3a, 0, 0, 0, 0, 0x80, 0, 0x12, 0x34, 0x56, 0x78}}
)

func TestEffectsCoverWhatLowerEmits(t *testing.T) {
	d := livenessOpenDecoder(t)
	for _, ext := range extension {
		for opcode := 0; opcode < 0x10000; opcode++ {
			code := append([]byte{byte(opcode >> 8), byte(opcode)}, ext...)
			insn, ok, err := d.Decode(code, 0x1000)
			if err != nil {
				t.Fatal(err)
			}
			if !ok {
				continue
			}
			statements, ok := insn.Lower(CCRAll)
			if !ok || len(statements) == 0 {
				continue
			}
			text := strings.Join(statements, "\n")
			effects := insn.FlagEffects()
			label := fmt.Sprintf("%04x %s %s", opcode, insn.Mnemonic, insn.OpStr)
			ops := map[string]bool{}
			for _, m := range ccOpRE.FindAllStringSubmatch(text, -1) {
				ops[m[1]] = true
			}
			if len(ops) > 0 && effects.Writes&CCRNZVC != CCRNZVC {
				t.Errorf("%s: lazy producer without NZVC writes", label)
			}
			if (ops["ADD"] || ops["SUB"]) && effects.Writes&CCRX == 0 {
				t.Errorf("%s: ADD/SUB producer without X write", label)
			}
			if strings.Contains(text, notX) && effects.Writes&CCRX == 0 {
				t.Errorf("%s: eager X store without X write", label)
			}
			if strings.Contains(text, "f3_exception") && effects.Reads != CCRAll {
				t.Errorf("%s: exception without reading every flag", label)
			}
			for _, m := range evalCond.FindAllStringSubmatch(text, -1) {
				cond, _ := strconv.Atoi(m[1])
				if flags := ConditionFlags(cond); effects.Reads&flags != flags {
					t.Errorf("%s: condition %d flags not read", label, cond)
				}
			}
			if len(ops) == 0 {
				continue
			}
			for _, live := range []int{0, CCRX, CCRNZVC} {
				elided, _ := insn.Lower(live)
				text := strings.Join(elided, "\n")
				if live&CCRNZVC == 0 && !strings.HasPrefix(insn.Mnemonic, "cas") && !strings.HasPrefix(insn.Mnemonic, "tas") && ccOpRE.MatchString(text) {
					t.Errorf("%s: dead producer kept at live=%#x", label, live)
				}
				if live&CCRX == 0 && strings.Contains(text, notX) {
					t.Errorf("%s: dead X store kept at live=%#x", label, live)
				}
			}
		}
	}
}
