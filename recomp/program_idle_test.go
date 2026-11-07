package main

import (
	"bytes"
	"os"
	"os/exec"
	"path/filepath"
	"slices"
	"strconv"
	"strings"
	"testing"
)

var generateIdleChunks = []generateChunk{
	{0x400, "60fe"},
	{0x410, "4a2d001056c8fffa4e75"},
	{0x420, "4a6d002066fa4e75"},
	{0x440, "4eb90000046060f8"},
	{0x460, "4eb804704e75"},
	{0x470, "202d00304c3c0800000000075a802b40003048404e75"},
}

func TestIdleRecognisesTheThreeShapes(t *testing.T) {
	_, d := synthetic(t, 0x500, generateIdleChunks)
	loops := d.FindIdleLoops(func(int) bool { return true })
	shapes := map[int]string{}
	for pc, loop := range loops {
		shapes[pc] = loop.Shape
	}
	wantShapes := map[int]string{0x400: "spin", 0x410: "poll", 0x420: "poll", 0x440: "affine"}
	if len(shapes) != len(wantShapes) {
		t.Fatalf("shapes %v", shapes)
	}
	for pc, shape := range wantShapes {
		if shapes[pc] != shape {
			t.Fatalf("shapes %v", shapes)
		}
	}
	for pc, want := range map[int]string{
		0x400: "F3_IDLE_SPIN(cpu, 10u);",
		0x410: "F3_IDLE_POLL_DBCC(cpu, (uint32_t)(cpu->a[5] + (int32_t)(16)), 1, 6, 0, 13u);",
		0x420: "F3_IDLE_POLL_BCC(cpu, (uint32_t)(cpu->a[5] + (int32_t)(32)), 2, 6, 13u);",
		0x440: "F3_IDLE_AFFINE_CALL(cpu, (uint32_t)(cpu->a[5] + (int32_t)(48)), 8u, 7u, 5u, 103u);",
	} {
		if !slices.Equal(loops[pc].Statements, []string{want}) {
			t.Fatalf("%#x: %q", pc, loops[pc].Statements)
		}
	}
}

func TestIdleKeepsLoopsWithNonNativeInstructionsUnmodified(t *testing.T) {
	_, d := synthetic(t, 0x500, generateIdleChunks)
	loops := d.FindIdleLoops(func(pc int) bool { return pc != 0x47C })
	if _, ok := loops[0x440]; ok {
		t.Fatal("0x440 must not be fast-forwarded")
	}
	if _, ok := loops[0x410]; !ok {
		t.Fatal("0x410 must be fast-forwarded")
	}
}

func TestIdlePollCounterIsTheDbccRegister(t *testing.T) {
	dec := newDecoder(t)
	instructions := instructionMap(decodeAll(t, dec, generateMustHex(t, "4a2d001056c9fffa"), 0x400))
	loops := (&Analysis{Instructions: instructions}).FindIdleLoops(func(int) bool { return true })
	want := []string{"F3_IDLE_POLL_DBCC(cpu, (uint32_t)(cpu->a[5] + (int32_t)(16)), 1, 6, 1, 13u);"}
	if !slices.Equal(loops[0x400].Statements, want) {
		t.Fatalf("%q", loops[0x400].Statements)
	}
}

func TestIdleRejectsPollsWhoseAddressCanChangeInsideTheLoop(t *testing.T) {
	dec := newDecoder(t)
	for _, code := range []string{
		"4a1d56c8fffc",
		"4a35000056c8fffa",
		"4a3a000256c8fffa",
	} {
		instructions := instructionMap(decodeAll(t, dec, generateMustHex(t, code), 0x400))
		if loops := (&Analysis{Instructions: instructions}).FindIdleLoops(func(int) bool { return true }); len(loops) != 0 {
			t.Errorf("%s: %v", code, loops)
		}
	}
}

func TestIdleFastForwardMatchesPlainExecutionAtEveryDeadlinePhase(t *testing.T) {
	cCompiler(t)
	rom, d := synthetic(t, 0x500, generateIdleChunks)
	output := t.TempDir()
	opts := DefaultCodegen()
	report, err := generateProgram(t, rom, d, defaultManifest, output, opts)
	if err != nil {
		t.Fatal(err)
	}
	loops, _ := report.Get("idle_loops")
	want := NewFields("0x000400", "spin", "0x000410", "poll", "0x000420", "poll", "0x000440", "affine")
	if encoded := loops.(*Fields).Encode(0); encoded != want.Encode(0) {
		t.Fatalf("idle_loops %s", encoded)
	}
	driver, err := os.ReadFile("testdata/idle_driver.c")
	if err != nil {
		t.Fatal(err)
	}
	driverPath := filepath.Join(output, "driver.c")
	if err := os.WriteFile(driverPath, driver, 0o644); err != nil {
		t.Fatal(err)
	}
	type result struct {
		stdout string
		reads  int
	}
	var results []result
	for _, variant := range []struct {
		name  string
		extra []string
	}{{"fast", nil}, {"plain", []string{"-DF3_PROFILE_INSTRUMENT=1"}}} {
		executable := filepath.Join(output, variant.name)
		args := append([]string{}, variant.extra...)
		args = append(args, driverPath)
		for _, source := range sourceFiles(t, report) {
			args = append(args, filepath.Join(output, source))
		}
		compileC(t, output, append(args, "-o", executable)...)
		var stdout, stderr bytes.Buffer
		run := exec.Command(executable)
		run.Stdout, run.Stderr = &stdout, &stderr
		if err := run.Run(); err != nil {
			t.Fatal(err)
		}
		reads, err := strconv.Atoi(strings.TrimSpace(stderr.String()))
		if err != nil {
			t.Fatal(err)
		}
		results = append(results, result{stdout.String(), reads})
	}
	fast, plain := results[0], results[1]
	if got := strings.Count(fast.stdout, "\n"); got != 6000 {
		t.Fatalf("%d lines", got)
	}
	if fast.stdout != plain.stdout {
		t.Fatal("fast-forward output differs from plain execution")
	}
	if fast.reads*4 >= plain.reads {
		t.Fatalf("fast reads %d, plain reads %d", fast.reads, plain.reads)
	}
}
