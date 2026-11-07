package main

import (
	"bytes"
	"encoding/hex"
	"fmt"
	"reflect"
	"strings"
	"testing"
)

var detailCode = []byte{
	0xd4, 0x40, 0x87, 0x5a, 0x4e, 0x71, 0x02, 0xb4, 0xc0, 0xde, 0xc0, 0xde, 0x5c, 0x00,
	0x1d, 0x80, 0x71, 0x12, 0x01, 0x23, 0xf2, 0x3c, 0x44, 0x22, 0x40, 0x49, 0x0e, 0x56,
	0x54, 0xc5, 0xf2, 0x3c, 0x44, 0x00, 0x44, 0x7a, 0x00, 0x00, 0xf2, 0x00, 0x0a, 0x28,
}

var detailWant = []struct {
	addr uint64
	size uint16
	id   uint32
	mnem string
	op   string
	ops  string
}{
	{0x1000, 2, 2, "add.w", "d0, d2", "1/1:d0 1/1:d2"},
	{0x1002, 2, 296, "or.w", "d3, (a2)+", "1/1:d3 3/4"},
	{0x1004, 2, 294, "nop", "", ""},
	{0x1006, 8, 8, "andi.l", "#$c0dec0de, (a4, d5.l * 4)", "2/18=-1059143458 3/8"},
	{0x100e, 6, 281, "move.b", "d0, ([a6, d7.w], $123)", "1/1:d0 3/10"},
	{0x1014, 8, 89, "fadd.s", "#3.141500, fp0", "4/18=1078529622 1/0:fp0"},
	{0x101c, 2, 330, "scc.b", "d5", "1/1:d5"},
	{0x101e, 8, 176, "fmove.s", "#1000.000000, fp0", "4/18=1148846080 1/0:fp0"},
	{0x1026, 4, 232, "fsub", "fp2, fp4", "1/0:fp2 1/0:fp4"},
}

type searchCase struct {
	code string
	id   uint32
	mnem string
	op   string
	ops  string
}

var search020BE = []searchCase{
	{"0b4eb4d9", 285, "movep.l", "-$4b27(a6), d5", "3/6 1/0:d5"},
	{"c3c4", 289, "muls.w", "d4, d1", "1/1:d4 1/1:d1"},
	{"d6ea8eec", 3, "adda.w", "-$7114(a2), a3", "3/6 1/2:a3"},
	{"2291", 281, "move.l", "(a1), (a1)", "3/3 3/3"},
	{"3b583bad", 281, "move.w", "(a0)+, $3bad(a5)", "3/4 3/6"},
	{"8164", 296, "or.w", "d0, -(a4)", "1/1:d0 3/5"},
	{"3986a607", 281, "move.w", "d6, $7(a4, a2.w)", "1/1:d6 3/7"},
	{"d7f1bffcb0342d4c", 3, "adda.l", "$b0342d4c(invalid.w), a3", "3/8 1/2:a3"},
	{"2d8c952edc17cc8d", 281, "move.l", "a4, ([$dc17, a6], a1.w * 4, $cc8d)", "1/2:a4 3/9"},
	{"1bb583d5eb9a4b20", 281, "move.b", "([]), ([, a6.l * 2], $4b20)", "3/9 3/10"},
	{"803afb03", 296, "or.b", "$b05(pc), d0", "3/11 1/1:d0"},
	{"2c7bca42", 282, "movea.l", "$1044(pc, a4.l), a6", "3/12 1/2:a6"},
	{"46fbf000", 281, "move.w", "$1002(pc,a7.w), sr", "3/13 1/0:sr"},
	{"82fbcba672561511", 78, "divu.w", "([$8258], a4.l * 2, $1511), d1", "3/14 1/1:d1"},
	{"2f3b0b4aa671", 281, "move.l", "([$1002pc], $a671), -(a7)", "3/15 3/5"},
	{"46f8a538", 281, "move.w", "$a538.w, sr", "3/16 1/0:sr"},
	{"053904652509", 32, "btst.l", "d2, $4652509.l", "1/1:d2 3/17"},
	{"e651", 319, "roxr.w", "#$3, d1", "2/18=3 1/1:d1"},
	{"fc966b29", 95, "fbf.w", "$7b2b", "8/19=27433"},
	{"f22b64a0cce0", 162, "fdiv.s", "-$3320(a3), fp1", "4/6=0 1/0:fp1"},
	{"f80777e1", 176, "fmove.d", "d7, fp7", "5/1=8 1/0:fp7"},
	{"4c9f356d", 284, "movem.w", "(a7)+, d0/d2-d3/d5-d6/a0/a2/a4-a5", "3/4 6/0=0x356d"},
	{"4c570403", 78, "divu.l", "(a7), d3:d0", "3/3 7/0:d3,d0"},
}

var search000 = []searchCase{
	{"c74f", 82, "exg.l", "a3, a7", "1/0:a3 1/0:a7"},
	{"e472", 319, "roxr.w", "d2, d2", "1/1:d2 1/1:d2"},
	{"dff22084", 3, "adda.l", "-$7c(a2, d2.w), a7", "3/7 1/2:a7"},
	{"3aaee0ea", 281, "move.w", "-$1f16(a6), (a5)", "3/6 3/3"},
	{"5a5c", 5, "addq.w", "#$5, (a4)+", "2/18=5 3/4"},
	{"9da0", 345, "sub.l", "d6, -(a0)", "1/1:d6 3/5"},
	{"532c2282", 348, "subq.b", "#$1, $2282(a4)", "2/18=1 3/6"},
	{"b9747ca8", 80, "eor.w", "d4, -$58(a4, d7.l)", "1/1:d4 3/7"},
	{"5bf57524cc5f", 339, "smi.b", "$cc5f(a5, d7.w * 4)", "3/8"},
	{"84f5eb1e9c5d", 78, "divu.w", "([a5], a6.l * 2, $9c5d), d2", "3/9 1/1:d2"},
	{"20f7071a592a", 281, "move.l", "([a7, d0.w * 8], $592a), (a0)+", "3/10 3/4"},
	{"8a3a7195", 296, "or.b", "$8197(pc), d5", "3/11 1/1:d5"},
	{"253baae5", 281, "move.l", "$fe7(pc, a2.l), -(a2)", "3/12 3/5"},
	{"1c3b8a00", 281, "move.b", "$1002(pc,a0.l * 2), d6", "3/13 1/1:d6"},
	{"dc7b8b6de092", 2, "add.w", "([$f094, pc]), d6", "3/14 1/1:d6"},
	{"103b91b21b4ea31f98b2", 281, "move.b", "([$1b4eb321, a1.w], $98b2), d0", "3/15 1/1:d0"},
	{"34f8f7c2", 281, "move.w", "$f7c2.w, (a2)+", "3/16 3/4"},
	{"d239d5249950", 2, "add.b", "$d5249950.l, d1", "3/17 1/1:d1"},
	{"6ea3", 25, "bgt.b", "$fa5", "8/19=-93"},
	{"48a6744c", 284, "movem.w", "d1-d3/d5/a1/a4-a5, -(a6)", "6/0=0x322e 3/5"},
}

func opSummary(d *Machine, ops []Operand) string {
	parts := make([]string, len(ops))
	for i, op := range ops {
		s := fmt.Sprintf("%d/%d", op.Type, op.AddressMode)
		switch op.Type {
		case OpReg:
			s += ":" + d.RegName(op.Reg)
		case OpImm, OpFPSingle, OpFPDouble:
			s += fmt.Sprintf("=%d", op.Imm)
		case OpRegBits:
			s += fmt.Sprintf("=%#x", op.RegisterBits)
		case OpRegPair:
			s += ":" + d.RegName(op.RegPair[0]) + "," + d.RegName(op.RegPair[1])
		case OpBrDisp:
			s += fmt.Sprintf("=%d", op.BrDisp.Disp)
		}
		parts[i] = s
	}
	return strings.Join(parts, " ")
}

func mustOpen(t *testing.T, mode Mode) *Machine {
	t.Helper()
	d, err := OpenMachine(mode)
	if err != nil {
		t.Fatalf("OpenMachine(%#x): %v", uint32(mode), err)
	}
	t.Cleanup(func() { _ = d.Close() })
	return d
}

func decodeOne(t *testing.T, d *Machine, hexCode string) Insn {
	t.Helper()
	code, err := hex.DecodeString(hexCode)
	if err != nil {
		t.Fatalf("%s: %v", hexCode, err)
	}
	insn, ok, err := d.Decode(code, 0x1000)
	if err != nil {
		t.Fatalf("%s: Decode: %v", hexCode, err)
	}
	if !ok || int(insn.Size) != len(code) {
		t.Fatalf("%s: ok=%v size=%d, want one %d-byte instruction", hexCode, ok, insn.Size, len(code))
	}
	return insn
}

func TestDetailM68K(t *testing.T) {
	d := mustOpen(t, ModeBigEndian|Mode68040)

	var got []Insn
	addr := uint64(0x1000)
	code := detailCode
	for {
		insn, ok, err := d.Decode(code, addr)
		if err != nil {
			t.Fatalf("Decode(%#x): %v", addr, err)
		}
		if !ok {
			break
		}
		got = append(got, insn)
		code = code[insn.Size:]
		addr += uint64(insn.Size)
	}
	if addr != 0x102a {
		t.Fatalf("end address %#x, want 0x102a", addr)
	}
	if len(got) != len(detailWant) {
		t.Fatalf("decoded %d instructions, want %d", len(got), len(detailWant))
	}
	off := 0
	for i, w := range detailWant {
		g := got[i]
		if g.Address != w.addr || g.Size != w.size || g.ID != w.id || g.Mnemonic != w.mnem || g.OpStr != w.op {
			t.Errorf("insn %d: addr=%#x size=%d id=%d %q %q; want addr=%#x size=%d id=%d %q %q",
				i, g.Address, g.Size, g.ID, g.Mnemonic, g.OpStr, w.addr, w.size, w.id, w.mnem, w.op)
		}
		if want := detailCode[off : off+int(g.Size)]; !bytes.Equal(g.Bytes, want) {
			t.Errorf("insn %d: bytes % x, want % x", i, g.Bytes, want)
		}
		off += int(g.Size)
		if s := opSummary(d, g.Operands); s != w.ops {
			t.Errorf("insn %d: operands %q, want %q", i, s, w.ops)
		}
	}

	if got[0].OpSize != (OpSize{Type: SizeCPU, Size: 2}) {
		t.Errorf("add.w OpSize = %+v, want CPU 2", got[0].OpSize)
	}
	if got[5].OpSize != (OpSize{Type: SizeFPU, Size: 4}) {
		t.Errorf("fadd.s OpSize = %+v, want FPU 4", got[5].OpSize)
	}
	if v := got[3].Operands[0].Imm; v != -1059143458 {
		t.Errorf("andi.l Imm = %d, want -1059143458", v)
	}
	if m := got[3].Operands[1].Mem; m != (Mem{BaseReg: RegA4, IndexReg: RegD5, Scale: 4, IndexSize: 1}) {
		t.Errorf("andi.l Mem = %+v", m)
	}
	if m := got[4].Operands[1].Mem; m != (Mem{BaseReg: RegA6, IndexReg: RegD7, OuterDisp: 0x123}) {
		t.Errorf("move.b Mem = %+v", m)
	}
}

func TestSearchM68K(t *testing.T) {
	for _, tc := range []struct {
		name  string
		mode  Mode
		cases []searchCase
	}{
		{"020BE", ModeBigEndian | Mode68020, search020BE},
		{"000", Mode68000, search000},
	} {
		t.Run(tc.name, func(t *testing.T) {
			d := mustOpen(t, tc.mode)
			for _, c := range tc.cases {
				insn := decodeOne(t, d, c.code)
				if insn.Address != 0x1000 || insn.ID != c.id || insn.Mnemonic != c.mnem || insn.OpStr != c.op {
					t.Errorf("%s: addr=%#x id=%d %q %q; want id=%d %q %q",
						c.code, insn.Address, insn.ID, insn.Mnemonic, insn.OpStr, c.id, c.mnem, c.op)
				}
				if s := opSummary(d, insn.Operands); s != c.ops {
					t.Errorf("%s: operands %q, want %q", c.code, s, c.ops)
				}
			}
		})
	}
}

func TestOperandFieldsM68K(t *testing.T) {
	d020 := mustOpen(t, ModeBigEndian|Mode68020)
	d000 := mustOpen(t, Mode68000)

	if m := decodeOne(t, d020, "0b4eb4d9").Operands[0].Mem; m != (Mem{BaseReg: RegA6, Disp: -19239}) {
		t.Errorf("movep.l Mem = %+v", m)
	}
	if m := decodeOne(t, d020, "2c7bca42").Operands[0].Mem; m != (Mem{BaseReg: RegPC, IndexReg: RegA4, Disp: 66, Scale: 2, IndexSize: 1}) {
		t.Errorf("movea.l Mem = %+v", m)
	}
	if m := decodeOne(t, d020, "82fbcba672561511").Operands[0].Mem; m != (Mem{IndexReg: RegA4, InnerDisp: 29270, OuterDisp: 5393, Scale: 2, IndexSize: 1}) {
		t.Errorf("divu.w Mem = %+v", m)
	}
	if m := decodeOne(t, d020, "1bb583d5eb9a4b20").Operands[1].Mem; m != (Mem{IndexReg: RegA6, OuterDisp: 19232, Scale: 2, IndexSize: 1}) {
		t.Errorf("move.b Mem = %+v", m)
	}
	if b := decodeOne(t, d020, "fc966b29").Operands[0].BrDisp; b != (BrDisp{Disp: 27433, DispSize: 2}) {
		t.Errorf("fbf.w BrDisp = %+v", b)
	}
	if b := decodeOne(t, d000, "6ea3").Operands[0].BrDisp; b != (BrDisp{Disp: -93, DispSize: 1}) {
		t.Errorf("bgt.b BrDisp = %+v", b)
	}
	if v := decodeOne(t, d020, "4c9f356d").Operands[1].RegisterBits; v != 0x356d {
		t.Errorf("movem.w (020BE) RegisterBits = %#x, want 0x356d", v)
	}
	if v := decodeOne(t, d000, "48a6744c").Operands[0].RegisterBits; v != 0x322e {
		t.Errorf("movem.w (000) RegisterBits = %#x, want 0x322e", v)
	}
	if p := decodeOne(t, d020, "4c570403").Operands[1].RegPair; p != [2]Reg{RegD3, RegD0} {
		t.Errorf("divu.l RegPair = %v, want [d3 d0]", p)
	}
}

func TestEdgeM68K(t *testing.T) {
	d := mustOpen(t, ModeBigEndian|Mode68020)

	if _, ok, err := d.Decode(nil, 0x1000); ok || err != nil {
		t.Errorf("empty input: ok=%v err=%v, want ok=false err=nil", ok, err)
	}
	if _, ok, err := d.Decode([]byte{0x4e}, 0x1000); ok || err != nil {
		t.Errorf("truncated input: ok=%v err=%v, want ok=false err=nil", ok, err)
	}
	insn, ok, err := d.Decode([]byte{0xff, 0xff}, 0x1000)
	if err != nil || !ok {
		t.Fatalf("ffff: ok=%v err=%v, want a dc.w", ok, err)
	}
	if insn.ID != 0 || insn.Size != 2 || insn.Mnemonic != "dc.w" || insn.OpStr != "$ffff" {
		t.Errorf("ffff: id=%d size=%d %q %q", insn.ID, insn.Size, insn.Mnemonic, insn.OpStr)
	}
	if len(insn.Operands) != 0 || insn.OpSize != (OpSize{}) {
		t.Errorf("ffff: %d operands, OpSize %+v; want none", len(insn.Operands), insn.OpSize)
	}
}

func TestReuseM68K(t *testing.T) {
	d, err := OpenMachine(ModeBigEndian | Mode68040)
	if err != nil {
		t.Fatal(err)
	}
	first, ok, err := d.Decode(detailCode, 0x1000)
	if err != nil || !ok {
		t.Fatalf("first: ok=%v err=%v", ok, err)
	}
	second, ok, err := d.Decode(detailCode[first.Size:], 0x1000+uint64(first.Size))
	if err != nil || !ok {
		t.Fatalf("second: ok=%v err=%v", ok, err)
	}
	if second.Mnemonic != "or.w" {
		t.Errorf("second mnemonic %q, want or.w", second.Mnemonic)
	}
	if first.Mnemonic != "add.w" || !bytes.Equal(first.Bytes, detailCode[:2]) || first.Operands[1].Reg != RegD2 {
		t.Errorf("first insn changed after second decode: %+v", first)
	}
	again, ok, err := d.Decode(detailCode, 0x1000)
	if err != nil || !ok {
		t.Fatalf("again: ok=%v err=%v", ok, err)
	}
	if !reflect.DeepEqual(first, again) {
		t.Errorf("re-decode differs:\n%+v\n%+v", first, again)
	}
	if err := d.Close(); err != nil {
		t.Fatalf("Close: %v", err)
	}
	if err := d.Close(); err != nil {
		t.Errorf("second Close: %v", err)
	}
	if _, _, err := d.Decode(detailCode, 0x1000); err == nil {
		t.Errorf("Decode after Close: err=nil, want error")
	}
}

func TestRegNameM68K(t *testing.T) {
	d := mustOpen(t, ModeBigEndian|Mode68020)
	for r, want := range map[Reg]string{
		RegD0: "d0", RegD7: "d7", RegA0: "a0", RegA7: "a7",
		RegPC: "pc", RegSR: "sr", RegCCR: "ccr", RegUSP: "usp",
	} {
		if got := d.RegName(r); got != want {
			t.Errorf("RegName(%d) = %q, want %q", r, got, want)
		}
	}
	if got := d.RegName(0xffff); got != "" {
		t.Errorf("RegName(0xffff) = %q, want empty", got)
	}
}
