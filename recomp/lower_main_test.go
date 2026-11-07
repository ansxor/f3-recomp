package main

import (
	"encoding/hex"
	"fmt"
	"slices"
	"testing"
)

type lowerGolden struct {
	asm     string
	hex     string
	addr    int
	liveOut int
	want    []string
}

func TestLowerMainGoldens(t *testing.T) {
	dec, err := OpenMachine(ModeBigEndian | Mode68020)
	if err != nil {
		t.Fatal(err)
	}
	defer dec.Close()
	for _, g := range lowerMainGoldens {
		t.Run(fmt.Sprintf("%s/%s@%x/live%02x", g.asm, g.hex, g.addr, g.liveOut), func(t *testing.T) {
			code, err := hex.DecodeString(g.hex)
			if err != nil {
				t.Fatal(err)
			}
			padded := append(slices.Clone(code), make([]byte, 8)...)
			insn, ok, err := dec.Decode(padded, uint64(g.addr))
			if err != nil || !ok {
				t.Fatalf("disasm: ok=%v err=%v", ok, err)
			}
			if int(insn.Size) != len(code) {
				t.Fatalf("decoded %d bytes, golden has %d", insn.Size, len(code))
			}
			got, ok := insn.Lower(g.liveOut)
			if g.want == nil {
				if ok {
					t.Fatalf("expected unsupported, got %q", got)
				}
				return
			}
			if !ok {
				t.Fatal("unexpected unsupported")
			}
			if !slices.Equal(got, g.want) {
				t.Errorf("mismatch\n got: %q\nwant: %q", got, g.want)
			}
		})
	}
}
