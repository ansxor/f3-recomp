package main

import (
	"hash/crc32"
	"os"
	"os/exec"
	"path/filepath"
	"slices"
	"strings"
	"testing"
)

func generateProgram(t *testing.T, rom []byte, d *Analysis, manifest Manifest, output string, opts CodegenOptions) (*Fields, error) {
	t.Helper()
	d.Game = Game{Image: rom, Manifest: manifest}
	exclusions, err := ParseExclusions(manifest, "main", 0, rom)
	if err != nil {
		return nil, err
	}
	d.Exclusions = exclusions
	program, err := d.Generate(opts)
	if err != nil {
		return nil, err
	}
	emission, err := program.Write(output)
	if err != nil {
		return nil, err
	}
	return emission.Report, nil
}

func buildAndRun(t *testing.T, output string, report *Fields, source string, optimisation string) {
	t.Helper()
	args := []string{optimisation, source}
	for _, name := range sourceFiles(t, report) {
		args = append(args, filepath.Join(output, name))
	}
	binary := strings.TrimSuffix(source, ".c")
	compileC(t, output, append(args, "-o", binary)...)
	if out, err := exec.Command(binary).CombinedOutput(); err != nil {
		t.Fatalf("driver: %v\n%s", err, out)
	}
}

func runStandalone(t *testing.T, output string, report *Fields, driverFile string) {
	t.Helper()
	driver, err := os.ReadFile(filepath.Join("testdata", driverFile))
	if err != nil {
		t.Fatal(err)
	}
	source := filepath.Join(output, "driver.c")
	if err := os.WriteFile(source, driver, 0o644); err != nil {
		t.Fatal(err)
	}
	buildAndRun(t, output, report, source, "-O2")
}

func runChaining(t *testing.T, output string, report *Fields, bodyFile string) {
	t.Helper()
	prelude, err := os.ReadFile("testdata/chaining_prelude.c")
	if err != nil {
		t.Fatal(err)
	}
	body, err := os.ReadFile(filepath.Join("testdata", bodyFile))
	if err != nil {
		t.Fatal(err)
	}
	source := filepath.Join(output, "chaining.c")
	if err := os.WriteFile(source, slices.Concat(prelude, body), 0o644); err != nil {
		t.Fatal(err)
	}
	buildAndRun(t, output, report, source, "-O2")
}

func requireMusttail(t *testing.T) {
	t.Helper()
	probe := exec.Command(cCompiler(t), "-std=c11", "-E", "-dM", "-x", "c", "-")
	probe.Stdin = strings.NewReader("#if defined(__has_attribute)\n#if __has_attribute(musttail)\n#define F3_PROBE_MUSTTAIL 1\n#endif\n#endif\n")
	out, err := probe.Output()
	if err != nil || !strings.Contains(string(out), "F3_PROBE_MUSTTAIL") {
		t.Skip("compiler lacks musttail; F3_CHAIN degrades to a plain return")
	}
}

func TestDeadlineYieldsWithMaterializedFlagsAndResumesInsideBlock(t *testing.T) {
	cCompiler(t)
	code := generateMustHex(t, "70ff42c3524042c4720142c5")
	insns := decodeAll(t, newDecoder(t), code, 0x400)
	instructions := instructionMap(insns)
	pcs := make([]int, len(insns))
	for i, insn := range insns {
		pcs[i] = int(insn.Address)
	}
	d := syntheticDiscovery(instructions, map[int][]int{0x400: pcs}, nil)
	output := t.TempDir()
	report, err := generateProgram(t, append(make([]byte, 0x400), code...), d, defaultManifest, output, DefaultCodegen())
	if err != nil {
		t.Fatal(err)
	}
	runStandalone(t, output, report, "deadline.c")
}

func TestOverlappingEntriesSkipExtensionsAndPreserveDeadlineResumption(t *testing.T) {
	cCompiler(t)
	code := generateMustHex(t, "203c700172024e717403")
	dec := newDecoder(t)
	instructions := map[int]*Insn{}
	for pc := 0x400; pc < 0x40A; pc += 2 {
		instructions[pc] = decodeAll(t, dec, code[pc-0x400:], pc)[0]
	}
	d := syntheticDiscovery(instructions, nil, nil)
	output := t.TempDir()
	report, err := generateProgram(t, append(make([]byte, 0x400), code...), d, allAligned, output, DefaultCodegen())
	if err != nil {
		t.Fatal(err)
	}
	runStandalone(t, output, report, "overlap.c")
}

func TestCmpmConsumesEepromSignatureAndAliasedOperands(t *testing.T) {
	cCompiler(t)
	code := generateMustHex(t, "b10a56c8fffcb14942c6b18842c6bf0f42c6")
	instructions := instructionMap(decodeAll(t, newDecoder(t), code, 0x400))
	blocks := map[int][]int{}
	for pc := range instructions {
		blocks[pc] = []int{pc}
	}
	d := syntheticDiscovery(instructions, blocks, nil)
	output := t.TempDir()
	report, err := generateProgram(t, append(make([]byte, 0x400), code...), d, defaultManifest, output, DefaultCodegen())
	if err != nil {
		t.Fatal(err)
	}
	runStandalone(t, output, report, "cmpm.c")
}

func generateSynthetic(t *testing.T, size int, chunks []generateChunk, output string, opts CodegenOptions) (*Fields, []byte) {
	t.Helper()
	rom, d := synthetic(t, size, chunks)
	report, err := generateProgram(t, rom, d, defaultManifest, output, opts)
	if err != nil {
		t.Fatal(err)
	}
	return report, rom
}

func TestBackwardLoopChainsInOneCallWithUnchainedCycleSum(t *testing.T) {
	cCompiler(t)
	output := t.TempDir()
	report, _ := generateSynthetic(t, 0x40a, []generateChunk{{0x400, "70044e7151c8fffc7207"}}, output, DefaultCodegen())
	runChaining(t, output, report, "chain_backward_loop.c")
}

func TestChainYieldsAtFirstDeadlineBoundaryAndResumesIdentically(t *testing.T) {
	cCompiler(t)
	output := t.TempDir()
	report, _ := generateSynthetic(t, 0x40a, []generateChunk{{0x400, "70044e7151c8fffc7207"}}, output, DefaultCodegen())
	runChaining(t, output, report, "chain_deadline.c")
}

func TestCrossFunctionTransferChainsButTraceRefuses(t *testing.T) {
	requireMusttail(t)
	output := t.TempDir()
	report, _ := generateSynthetic(t, 0x604, []generateChunk{
		{0x400, "610000fe7409"},
		{0x500, "72074e71"},
		{0x600, "6000fefe"},
	}, output, DefaultCodegen())
	runChaining(t, output, report, "chain_cross_function.c")
}

func TestUnretainedChainTargetReturnsToDispatcher(t *testing.T) {
	cCompiler(t)
	chunks := []generateChunk{{0x400, "610000fe7409"}, {0x500, "72074e71"}}
	rom, d := synthetic(t, 0x504, chunks)
	root := t.TempDir()

	undecoded := filepath.Join(root, "undecoded")
	instructions := map[int]*Insn{}
	for pc, insn := range d.Instructions {
		if pc < 0x500 {
			instructions[pc] = insn
		}
	}
	report, err := generateProgram(t, rom, syntheticDiscovery(instructions, map[int][]int{0x400: {0x400, 0x404}}, nil), defaultManifest, undecoded, DefaultCodegen())
	if err != nil {
		t.Fatal(err)
	}
	runChaining(t, undecoded, report, "chain_undecoded.c")

	profile := filepath.Join(root, "entries.profile")
	generateWriteProfile(t, profile, rom, 0x400)
	slim := filepath.Join(root, "slim")
	report, err = generateProgram(t, rom, d, defaultManifest, slim, optionsFor("slim", profile))
	if err != nil {
		t.Fatal(err)
	}
	if got := intField(t, report, "retained_entries"); got != 1 {
		t.Fatalf("retained_entries = %d", got)
	}
	runChaining(t, slim, report, "chain_slim.c")
}

func indirectFixture(t *testing.T) ([]byte, map[int]*Insn, map[int][]int, map[int][]int) {
	t.Helper()
	code := []generateChunk{
		{0x3FE, "303b00f0" + "4efb00ec" + "72214e714e71"},
		{0x600, "207b00084ed0" + "72214e71"},
		{0x500, "7427"},
		{0x602, "7629"},
	}
	tables := []generateChunk{{0x3F0, "001601100000"}, {0x60A, "0000060600000500"}}
	dec := newDecoder(t)
	instructions := map[int]*Insn{}
	blocks := map[int][]int{}
	rom := make([]byte, 0x700)
	for _, c := range slices.Concat(tables, code) {
		copy(rom[c.address:], generateMustHex(t, c.code))
	}
	for _, c := range code {
		var pcs []int
		for _, insn := range decodeAll(t, dec, generateMustHex(t, c.code), c.address) {
			instructions[int(insn.Address)] = insn
			pcs = append(pcs, int(insn.Address))
		}
		blocks[c.address] = pcs
	}
	return rom, instructions, blocks, map[int][]int{0x402: {0x406, 0x500}, 0x604: {0x606, 0x500}}
}

func generateIndirect(t *testing.T, output string) *Fields {
	t.Helper()
	rom, instructions, blocks, targets := indirectFixture(t)
	d := syntheticDiscovery(instructions, blocks, nil)
	d.IndirectTargets = targets
	report, err := generateProgram(t, rom, d, defaultManifest, output, DefaultCodegen())
	if err != nil {
		t.Fatal(err)
	}
	return report
}

func TestTableIndexesChainInOneCallWithDispatcherCycleSum(t *testing.T) {
	requireMusttail(t)
	output := t.TempDir()
	report := generateIndirect(t, output)
	if intField(t, report, "indirect_chain_sites") != 2 || intField(t, report, "indirect_chain_cases") != 4 {
		t.Fatalf("indirect chain counts: %s", report.Encode(0))
	}
	runChaining(t, output, report, "indirect_table.c")
}

func TestUnknownIndexAndTraceRefuseTheChain(t *testing.T) {
	cCompiler(t)
	output := t.TempDir()
	runChaining(t, output, generateIndirect(t, output), "indirect_unknown.c")
}

func TestXOnlyAddIsNotClobberedByStalePendingState(t *testing.T) {
	cCompiler(t)
	output := t.TempDir()
	report, _ := generateSynthetic(t, 0x408, []generateChunk{{0x400, "d2807400d98342c5"}}, output, DefaultCodegen())
	flag, _ := report.Get("flag_liveness")
	if v, _ := flag.(*Fields).Get("eager_x_writes_emitted"); v != 1 {
		t.Fatalf("eager_x_writes_emitted = %v", v)
	}
	runChaining(t, output, report, "x_only_add.c")
}

func TestPartialWritersThatPreserveXKeepAnEarlierXProducer(t *testing.T) {
	cCompiler(t)
	output := t.TempDir()
	report, _ := generateSynthetic(t, 0x408, []generateChunk{{0x400, "d240b443e3544e75"}}, output, DefaultCodegen())
	runChaining(t, output, report, "partial_writers.c")
}

func TestGenerateRejectsInvalidOptions(t *testing.T) {
	_, d := synthetic(t, 0x402, []generateChunk{{0x400, "7001"}})
	output := t.TempDir()
	for _, opts := range []CodegenOptions{
		{MaxBlockInstructions: 0, BlocksPerFile: 128},
		{MaxBlockInstructions: 32, BlocksPerFile: 0},
		{MaxBlockInstructions: 32, BlocksPerFile: 128, ProfileTiers: "a", ProfileSlim: "b"},
	} {
		if _, err := generateProgram(t, make([]byte, 0x402), d, defaultManifest, output, opts); err == nil {
			t.Errorf("%+v: expected an error", opts)
		}
	}
}

func TestGenerateRemovesStaleFilesButKeepsHiddenAndStamps(t *testing.T) {
	output := t.TempDir()
	for _, name := range []string{"blocks_0099.c", "old.txt", ".hidden", "build.stamp"} {
		if err := os.WriteFile(filepath.Join(output, name), nil, 0o644); err != nil {
			t.Fatal(err)
		}
	}
	rom, d := synthetic(t, 0x402, []generateChunk{{0x400, "7001"}})
	if _, err := generateProgram(t, rom, d, defaultManifest, output, DefaultCodegen()); err != nil {
		t.Fatal(err)
	}
	for name, want := range map[string]bool{"blocks_0099.c": false, "old.txt": false, ".hidden": true, "build.stamp": true, "blocks_0000.c": true, "program.c": true, "program.bin": true} {
		_, err := os.Stat(filepath.Join(output, name))
		if (err == nil) != want {
			t.Errorf("%s: exists=%v want %v", name, err == nil, want)
		}
	}
	data, err := os.ReadFile(filepath.Join(output, "program.bin"))
	if err != nil || crc32.ChecksumIEEE(data) != crc32.ChecksumIEEE(rom) {
		t.Errorf("program.bin does not hold the ROM")
	}
}
