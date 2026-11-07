package main

import (
	"encoding/hex"
	"fmt"
	"slices"
	"testing"
)

type coreGolden struct {
	asm     string
	hex     string
	addr    int
	liveOut int
	want    []string
}

func disasm(t *testing.T, dec *Machine, hexCode string, addr int) Insn {
	t.Helper()
	code, err := hex.DecodeString(hexCode)
	if err != nil {
		t.Fatal(err)
	}
	insn, ok, err := dec.Decode(append(slices.Clone(code), make([]byte, 8)...), uint64(addr))
	if err != nil || !ok {
		t.Fatalf("disasm %s: ok=%v err=%v", hexCode, ok, err)
	}
	if int(insn.Size) != len(code) {
		t.Fatalf("decoded %d bytes, want %d", insn.Size, len(code))
	}
	return insn
}

func emitterOpenDecoder(t *testing.T) *Machine {
	t.Helper()
	dec, err := OpenMachine(ModeBigEndian | Mode68020)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { dec.Close() })
	return dec
}

func TestLowerCoreGoldens(t *testing.T) {
	dec := emitterOpenDecoder(t)
	for _, g := range lowerCoreGoldens {
		t.Run(fmt.Sprintf("%s/%s@%x/live%02x", g.asm, g.hex, g.addr, g.liveOut), func(t *testing.T) {
			insn := disasm(t, dec, g.hex, g.addr)
			got, ok := insn.Lower(g.liveOut)
			if g.want == nil {
				if ok {
					t.Fatalf("expected unsupported, got %q", got)
				}
				return
			}
			if !ok || !slices.Equal(got, g.want) {
				t.Errorf("ok=%v\n got: %q\nwant: %q", ok, got, g.want)
			}
		})
	}
}

func TestInstructionCyclesAndFlow(t *testing.T) {
	dec := emitterOpenDecoder(t)
	tests := []struct {
		hex     string
		cycles  string
		targets []int
		falls   bool
		escapes bool
	}{
		{"4e71", "2u", nil, true, false},
		{"60000010", "10u", []int{0x1012}, false, false},
		{"6600fff0", "6u", []int{0xff2}, true, false},
		{"6704", "(cpu->pc == 0x1002u ? 4u : 6u)", []int{0x1006}, true, false},
		{"60ff00000100", "10u", []int{0x1102}, false, false},
		{"51c8fffe", "6u", []int{0x1000}, true, false},
		{"4e75", "10u", nil, false, true},
		{"4eb900001234", "4u", []int{0x1234}, false, false},
		{"4efa0010", "5u", []int{0x1012}, false, false},
		{"4ef00170" + "00000000", "13u", nil, false, true},
		{"4ed0", "4u", nil, false, true},
		{"4e73", "20u", nil, false, true},
		{"48e7c0c0", "16u", nil, true, false},
		{"4cdf0303", "24u", nil, true, false},
		{"203b0170" + "00000000", "15u", nil, true, false},
		{"203b0901", "9u", nil, true, false},
		{"23fb090100001000", "13u", nil, true, false},
		{"4e7b0002", "12u", nil, true, false},
		{"0cb90000000100001234", "6u", nil, true, false},
		{"203900001234", "6u", nil, true, false},
		{"e8d00120", "17u", nil, true, false},
	}
	for _, tc := range tests {
		t.Run(tc.hex, func(t *testing.T) {
			insn := disasm(t, dec, tc.hex, 0x1000)
			if got := insn.CycleCost(); got != tc.cycles {
				t.Errorf("cycles = %q, want %q", got, tc.cycles)
			}
			flow := insn.Flow()
			if !slices.Equal(flow.Targets, tc.targets) || flow.FallsThrough != tc.falls || flow.Escapes != tc.escapes {
				t.Errorf("flow = %+v, want targets=%v falls=%v escapes=%v", flow, tc.targets, tc.falls, tc.escapes)
			}
		})
	}
}

func TestEAExtensionBytes(t *testing.T) {
	tests := []struct {
		raw                     []byte
		offset, mode, reg, size int
		want                    int
	}{
		{nil, 0, 3, 0, 4, 0},
		{nil, 0, 5, 0, 4, 2},
		{nil, 0, 7, 2, 4, 2},
		{nil, 0, 7, 1, 4, 4},
		{nil, 0, 7, 4, 1, 2},
		{nil, 0, 7, 4, 4, 4},
		{[]byte{0x4e, 0x71, 0x00, 0x70}, 2, 6, 0, 4, 2},
		{[]byte{0x4e, 0x71, 0x01, 0x70}, 2, 6, 0, 4, 6},
		{[]byte{0x4e, 0x71, 0x01, 0x71}, 2, 6, 0, 4, 6},
		{[]byte{0x4e, 0x71, 0x01, 0x30}, 2, 7, 3, 4, 6},
		{[]byte{0x4e, 0x71, 0x01, 0x33}, 2, 7, 3, 4, 10},
		{[]byte{0x4e, 0x71, 0x01, 0x20}, 2, 6, 0, 4, 4},
		{[]byte{0x4e, 0x71}, 2, 6, 0, 4, 2},
		{nil, 0, 0, 0, 4, 0},
	}
	for _, tc := range tests {
		insn := Insn{Bytes: tc.raw}
		if got := insn.EAExtensionBytes(tc.offset, tc.mode, tc.reg, tc.size); got != tc.want {
			t.Errorf("EAExtensionBytes(%x, %d, %d, %d, %d) = %d, want %d", tc.raw, tc.offset, tc.mode, tc.reg, tc.size, got, tc.want)
		}
	}
}
