package main

import (
	"encoding/hex"
	"maps"
	"reflect"
	"slices"
	"testing"
)

type idleChunk struct {
	addr int
	code string
}

var idleChunks = []idleChunk{
	{0x400, "60fe"},
	{0x410, "4a2d001056c8fffa4e75"},
	{0x420, "4a6d002066fa4e75"},
	{0x440, "4eb90000046060f8"},
	{0x460, "4eb804704e75"},
	{0x470, "202d00304c3c0800000000075a802b40003048404e75"},
}

func decode(t testing.TB, idleChunks ...idleChunk) map[int]*Insn {
	t.Helper()
	d, err := OpenMachine(ModeBigEndian | Mode68020)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { d.Close() })
	instructions := map[int]*Insn{}
	for _, c := range idleChunks {
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

func always(int) bool { return true }

func findIdleLoops(instructions map[int]*Insn, native func(pc int) bool) IdleLoops {
	return (&Analysis{Instructions: instructions}).FindIdleLoops(native)
}

func TestRecognisesTheThreeShapes(t *testing.T) {
	loops := findIdleLoops(decode(t, idleChunks...), always)
	want := map[int]Loop{
		0x400: {"spin", []string{"F3_IDLE_SPIN(cpu, 10u);"}},
		0x410: {"poll", []string{"F3_IDLE_POLL_DBCC(cpu, (uint32_t)(cpu->a[5] + (int32_t)(16)), 1, 6, 0, 13u);"}},
		0x420: {"poll", []string{"F3_IDLE_POLL_BCC(cpu, (uint32_t)(cpu->a[5] + (int32_t)(32)), 2, 6, 13u);"}},
		0x440: {"affine", []string{"F3_IDLE_AFFINE_CALL(cpu, (uint32_t)(cpu->a[5] + (int32_t)(48)), 8u, 7u, 5u, 103u);"}},
	}
	if !reflect.DeepEqual(map[int]Loop(loops), want) {
		t.Errorf("loops = %v, want %v", loops, want)
	}
}

func TestNonNativeInstructionKeepsLoopUnmodified(t *testing.T) {
	loops := findIdleLoops(decode(t, idleChunks...), func(pc int) bool { return pc != 0x47C })
	if _, ok := loops[0x440]; ok {
		t.Error("affine loop at 0x440 recognised despite non-native instruction")
	}
	if _, ok := loops[0x410]; !ok {
		t.Error("poll loop at 0x410 missing")
	}
}

func TestPollCounterIsTheDbccRegister(t *testing.T) {
	loops := findIdleLoops(decode(t, idleChunk{0x400, "4a2d001056c9fffa"}), always)
	want := []string{"F3_IDLE_POLL_DBCC(cpu, (uint32_t)(cpu->a[5] + (int32_t)(16)), 1, 6, 1, 13u);"}
	if got := loops[0x400].Statements; !slices.Equal(got, want) {
		t.Errorf("statements = %v, want %v (loops %v)", got, want, slices.Collect(maps.Keys(loops)))
	}
}

func TestRejectsPollsWhoseAddressCanChange(t *testing.T) {
	for _, code := range []string{
		"4a1d56c8fffc",
		"4a35000056c8fffa",
		"4a3a000256c8fffa",
	} {
		if loops := findIdleLoops(decode(t, idleChunk{0x400, code}), always); len(loops) != 0 {
			t.Errorf("%s: loops = %v, want none", code, loops)
		}
	}
}
