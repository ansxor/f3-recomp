package main

import (
	"encoding/json"
	"hash/crc32"
	"os"
	"os/exec"
	"path/filepath"
	"slices"
	"testing"
)

func generateExecute(t *testing.T, output string, report *Fields, driverFile string, slim bool) {
	t.Helper()
	prelude, err := os.ReadFile("testdata/profile_prelude.c")
	if err != nil {
		t.Fatal(err)
	}
	driver, err := os.ReadFile(filepath.Join("testdata", driverFile))
	if err != nil {
		t.Fatal(err)
	}
	define := "#define SLIM 0\n"
	if slim {
		define = "#define SLIM 1\n"
	}
	path := filepath.Join(output, "driver.c")
	if err := os.WriteFile(path, slices.Concat(prelude, []byte(define), driver), 0o644); err != nil {
		t.Fatal(err)
	}
	args := []string{"-DF3_PROFILE_INSTRUMENT=1", path}
	for _, name := range sourceFiles(t, report) {
		args = append(args, filepath.Join(output, name))
	}
	binary := filepath.Join(output, "driver")
	compileC(t, output, append(args, "-o", binary)...)
	if out, err := exec.Command(binary).CombinedOutput(); err != nil {
		t.Fatalf("driver: %v\n%s", err, out)
	}
}

func generateWriteProfile(t *testing.T, path string, rom []byte, hits ...int) *Profile {
	t.Helper()
	crc := crc32.ChecksumIEEE(rom)
	profile := NewProfile()
	profile.Roms[RomKey{Region: "main", CRC: crc}] = Bounds{Size: len(rom)}
	for _, address := range hits {
		profile.Hits[RowKey{Region: "main", CRC: crc, Address: address}] = 1
	}
	if err := profile.Write(path); err != nil {
		t.Fatal(err)
	}
	return profile
}

func optionsFor(mode, profile string) CodegenOptions {
	opts := DefaultCodegen()
	switch mode {
	case "tiers":
		opts.ProfileTiers = profile
	case "slim":
		opts.ProfileSlim = profile
	}
	return opts
}

var allAligned = ManifestOf(map[string]any{"discovery": map[string]any{"coverage": "all_aligned"}})

func intField(t *testing.T, report *Fields, key string) int {
	t.Helper()
	v, ok := report.Get(key)
	n, isInt := v.(int)
	if !ok || !isInt {
		t.Fatalf("report[%q] = %v", key, v)
	}
	return n
}

func TestCrossTierSuccessorsDeadlinesIndirectEntriesAndSlimRemoval(t *testing.T) {
	cCompiler(t)
	code := generateMustHex(t, "700172027403")
	rom := append(make([]byte, 0x400), code...)
	instructions := instructionMap(decodeAll(t, newDecoder(t), code, 0x400))
	d := syntheticDiscovery(instructions, nil, nil)
	root := t.TempDir()
	profile := filepath.Join(root, "entries.profile")
	generateWriteProfile(t, profile, rom, 0x400, 0x404)
	for _, mode := range []string{"baseline", "tiers", "slim"} {
		t.Run(mode, func(t *testing.T) {
			output := filepath.Join(root, mode)
			report, err := generateProgram(t, rom, d, allAligned, output, optionsFor(mode, profile))
			if err != nil {
				t.Fatal(err)
			}
			generateExecute(t, output, report, "profile_cross_tier.c", mode == "slim")
		})
	}
}

func TestMainSharedExceptionEntriesRemainCountedOnceInEachMode(t *testing.T) {
	cCompiler(t)
	rom := generateMustHex(t, "a000f0007100")
	d := syntheticDiscovery(map[int]*Insn{}, nil, []int{0, 2, 4})
	root := t.TempDir()
	profile := filepath.Join(root, "entries.profile")
	generateWriteProfile(t, profile, rom, 0, 4)
	for _, mode := range []string{"baseline", "tiers", "slim"} {
		t.Run(mode, func(t *testing.T) {
			output := filepath.Join(root, mode)
			report, err := generateProgram(t, rom, d, allAligned, output, optionsFor(mode, profile))
			if err != nil {
				t.Fatal(err)
			}
			generateExecute(t, output, report, "profile_main_exceptions.c", mode == "slim")
		})
	}
}

func TestMainExclusionsPreserveTierComplementAndSlimExceptionSubset(t *testing.T) {
	cCompiler(t)
	rom := generateMustHex(t, "70017202a000a000a001f000")
	instructions := instructionMap(decodeAll(t, newDecoder(t), rom[:4], 0))
	d := syntheticDiscovery(instructions, nil, []int{6, 8, 10})
	cfg := ManifestOf(map[string]any{
		"discovery": map[string]any{"coverage": "all_aligned"},
		"exclude": []any{map[string]any{
			"start": int64(4), "end": int64(6), "reason": "synthetic data", "evidence": "fixture",
		}},
	})
	root := t.TempDir()
	profile := filepath.Join(root, "entries.profile")
	generateWriteProfile(t, profile, rom, 0, 6)
	for _, mode := range []string{"tiers", "slim"} {
		t.Run(mode, func(t *testing.T) {
			output := filepath.Join(root, mode)
			report, err := generateProgram(t, rom, d, cfg, output, optionsFor(mode, profile))
			if err != nil {
				t.Fatal(err)
			}
			retained := 5
			wantExceptions := NewFields("10", 2, "11", 1)
			if mode == "slim" {
				retained = 2
				wantExceptions = NewFields("10", 1)
			}
			got := []int{
				intField(t, report, "original_entries"), intField(t, report, "retained_entries"),
				intField(t, report, "registered_entries"), intField(t, report, "hot_entries"),
				intField(t, report, "cold_entries"), intField(t, report, "excluded_entries"),
			}
			if want := []int{5, retained, retained, 2, 3, 1}; !slices.Equal(got, want) {
				t.Fatalf("counts %v, want %v", got, want)
			}
			exceptions, _ := report.Get("exception_entries")
			if encoded := exceptions.(*Fields).Encode(0); encoded != wantExceptions.Encode(0) {
				t.Fatalf("exception_entries %s", encoded)
			}
			data, err := os.ReadFile(filepath.Join(output, "profile_inventory.json"))
			if err != nil {
				t.Fatal(err)
			}
			var inventory struct {
				Entries         []int `json:"entries"`
				RetainedEntries int   `json:"retained_entries"`
				HotEntries      int   `json:"hot_entries"`
				ColdEntries     int   `json:"cold_entries"`
			}
			if err := json.Unmarshal(data, &inventory); err != nil {
				t.Fatal(err)
			}
			if !slices.Equal(inventory.Entries, []int{0, 2, 6, 8, 10}) ||
				inventory.RetainedEntries != retained || inventory.HotEntries != 2 || inventory.ColdEntries != 3 {
				t.Fatalf("inventory %+v", inventory)
			}
			generateExecute(t, output, report, "profile_main_exclusions.c", mode == "slim")
		})
	}
}

func TestMainRejectsExcludedDiscoveryAndProfileRecordsBeforeEmission(t *testing.T) {
	rom := generateMustHex(t, "7001a000")
	instruction := decodeAll(t, newDecoder(t), rom[:2], 0)[0]
	cfg := ManifestOf(map[string]any{
		"discovery": map[string]any{"coverage": "all_aligned"},
		"exclude": []any{map[string]any{
			"start": int64(2), "end": int64(4), "reason": "synthetic data", "evidence": "fixture",
		}},
	})
	crc := crc32.ChecksumIEEE(rom)
	root := t.TempDir()
	for _, kind := range []string{"decoded", "invalid"} {
		t.Run(kind, func(t *testing.T) {
			instructions := map[int]*Insn{0: instruction}
			var invalid []int
			if kind == "decoded" {
				instructions[2] = instruction
			} else {
				invalid = []int{2}
			}
			output := filepath.Join(root, kind)
			if _, err := generateProgram(t, rom, syntheticDiscovery(instructions, nil, invalid), cfg, output, DefaultCodegen()); err == nil {
				t.Fatal("expected an error")
			}
			if _, err := os.Stat(filepath.Join(output, "program.c")); err == nil {
				t.Fatal("program.c emitted")
			}
		})
	}
	d := syntheticDiscovery(map[int]*Insn{0: instruction}, nil, nil)
	for _, mode := range []string{"tiers", "slim"} {
		for _, kind := range []string{"hit", "miss"} {
			t.Run(mode+kind, func(t *testing.T) {
				profile := generateWriteProfile(t, filepath.Join(root, "unused.profile"), rom, 0)
				rows := profile.Hits
				if kind == "miss" {
					rows = profile.Misses
				}
				rows[RowKey{Region: "main", CRC: crc, Address: 2}] = 1
				path := filepath.Join(root, "contradictory.profile")
				if err := profile.Write(path); err != nil {
					t.Fatal(err)
				}
				if _, err := generateProgram(t, rom, d, cfg, filepath.Join(root, mode+kind), optionsFor(mode, path)); err == nil {
					t.Fatal("expected an error")
				}
			})
		}
	}
}
