package main

import (
	"encoding/hex"
	"os"
	"os/exec"
	"path/filepath"
	"testing"
)

func generateMustHex(t testing.TB, s string) []byte {
	t.Helper()
	b, err := hex.DecodeString(s)
	if err != nil {
		t.Fatal(err)
	}
	return b
}

func newDecoder(t testing.TB) *Machine {
	t.Helper()
	dec, err := OpenMachine(ModeBigEndian | Mode68020)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { dec.Close() })
	return dec
}

func decodeAll(t testing.TB, dec *Machine, code []byte, address int) []*Insn {
	t.Helper()
	var out []*Insn
	for offset := 0; offset < len(code); {
		insn, ok, err := dec.Decode(code[offset:], uint64(address+offset))
		if err != nil {
			t.Fatal(err)
		}
		if !ok {
			break
		}
		out = append(out, &insn)
		offset += int(insn.Size)
	}
	return out
}

func instructionMap(insns []*Insn) map[int]*Insn {
	out := map[int]*Insn{}
	for _, insn := range insns {
		out[int(insn.Address)] = insn
	}
	return out
}

var defaultManifest = ManifestOf(map[string]any{})

func syntheticDiscovery(instructions map[int]*Insn, blocks map[int][]int, invalid []int) *Analysis {
	if blocks == nil {
		blocks = map[int][]int{}
	}
	return &Analysis{Instructions: instructions, Blocks: blocks, InvalidPCs: invalid, Report: NewFields()}
}

type generateChunk struct {
	address int
	code    string
}

func synthetic(t testing.TB, size int, generateChunks []generateChunk) ([]byte, *Analysis) {
	t.Helper()
	dec := newDecoder(t)
	instructions := map[int]*Insn{}
	blocks := map[int][]int{}
	rom := make([]byte, size)
	for _, c := range generateChunks {
		code := generateMustHex(t, c.code)
		var pcs []int
		for _, insn := range decodeAll(t, dec, code, c.address) {
			instructions[int(insn.Address)] = insn
			pcs = append(pcs, int(insn.Address))
		}
		blocks[c.address] = pcs
		copy(rom[c.address:], code)
	}
	return rom, syntheticDiscovery(instructions, blocks, nil)
}

func cCompiler(t testing.TB) string {
	t.Helper()
	cc := os.Getenv("CC")
	if cc == "" {
		cc = "cc"
	}
	path, err := exec.LookPath(cc)
	if err != nil {
		t.Skipf("C compiler %q not found", cc)
	}
	return path
}

func generateRepoRoot(t testing.TB) string {
	t.Helper()
	root, err := filepath.Abs("..")
	if err != nil {
		t.Fatal(err)
	}
	for _, header := range []string{"recomp/cpu_ops.h", "include/f3rt/cpu_abi.h", "include/f3rt/block_profile.h"} {
		if _, err := os.Stat(filepath.Join(root, header)); err != nil {
			t.Skipf("runtime header %s not available", header)
		}
	}
	return root
}

func sourceFiles(t testing.TB, report *Fields) []string {
	t.Helper()
	v, _ := report.Get("source_files")
	files, ok := v.([]string)
	if !ok {
		t.Fatalf("source_files has type %T", v)
	}
	return files
}

func compileC(t testing.TB, output string, args ...string) {
	t.Helper()
	root := generateRepoRoot(t)
	full := append([]string{"-std=c11", "-O2", "-Wall", "-Wextra", "-Werror"}, args...)
	full = append(full, "-I", root, "-I", filepath.Join(root, "include"), "-I", output)
	if out, err := exec.Command(cCompiler(t), full...).CombinedOutput(); err != nil {
		t.Fatalf("cc %v: %v\n%s", full, err, out)
	}
}
