package main

import (
	"crypto/sha1"
	"encoding/binary"
	"encoding/hex"
	"fmt"
	"hash/crc32"
	"os"
	"path/filepath"
	"slices"
	"strings"
	"testing"
)

func discoveryMustHex(t testing.TB, s string) []byte {
	t.Helper()
	b, err := hex.DecodeString(s)
	if err != nil {
		t.Fatal(err)
	}
	return b
}

func put32(rom []byte, at int, values ...uint32) {
	for i, v := range values {
		binary.BigEndian.PutUint32(rom[at+i*4:], v)
	}
}

func put16(rom []byte, at int, values ...uint16) {
	for i, v := range values {
		binary.BigEndian.PutUint16(rom[at+i*2:], v)
	}
}

func mustDiscover(t *testing.T, rom []byte, cfg Manifest) *Analysis {
	t.Helper()
	result, err := Game{Image: rom, Manifest: cfg}.Discover(DiscoverOptions{})
	if err != nil {
		t.Fatal(err)
	}
	return result
}

func hasPC(d *Analysis, pc int) bool {
	_, ok := d.Instructions[pc]
	return ok
}

func requirePCs(t *testing.T, d *Analysis, present bool, pcs ...int) {
	t.Helper()
	for _, pc := range pcs {
		if hasPC(d, pc) != present {
			t.Errorf("instruction at %#x present=%v, want %v", pc, !present, present)
		}
	}
}

func table(kv ...any) map[string]any {
	t := map[string]any{}
	for i := 0; i < len(kv); i += 2 {
		t[kv[i].(string)] = kv[i+1]
	}
	return t
}

func ints(values ...int) []any {
	out := make([]any, len(values))
	for i, v := range values {
		out[i] = int64(v)
	}
	return out
}

func actorFixture() ([]byte, Manifest, Manifest) {
	rom := make([]byte, 0x1000)
	put32(rom, 0, 0x410000, 0x400)
	scripts := table(
		"pointer_field", int64(0x1C),
		"operand_bytes", append(ints(4, 4, 4, 4, 4, 4, 4, 4, 4, 4), int64(0)),
		"code_pointer_opcodes", ints(0),
		"jump_opcode", int64(5),
		"call_opcode", int64(9),
		"return_opcode", int64(10),
	)
	disc := table(
		"scan_jump_tables", false,
		"scan_task_traps", false,
		"scan_callbacks", false,
		"actor_scripts", scripts,
	)
	return rom, ManifestOf(table("discovery", disc)), ManifestOf(disc)
}

func TestLongCallbackAndCyclicScriptCalls(t *testing.T) {
	rom, cfg, _ := actorFixture()
	copy(rom[0x400:], discoveryMustHex(t, "297c00000600001c4e75"))
	put16(rom, 0x600, 0)
	put32(rom, 0x602, 0x800)
	put16(rom, 0x606, 9)
	put32(rom, 0x608, 0x620)
	put16(rom, 0x60C, 5)
	put32(rom, 0x60E, 0x600)
	put16(rom, 0x620, 0)
	put32(rom, 0x622, 0x880)
	put16(rom, 0x626, 10, 0)
	put32(rom, 0x62A, 0x900)
	copy(rom[0x800:], slices.Repeat(discoveryMustHex(t, "4e71"), 40))
	copy(rom[0x850:], discoveryMustHex(t, "4e75"))
	copy(rom[0x880:], discoveryMustHex(t, "4e75"))
	copy(rom[0x900:], discoveryMustHex(t, "4e75"))
	result := mustDiscover(t, rom, cfg)
	requirePCs(t, result, true, 0x800, 0x850, 0x880)
	requirePCs(t, result, false, 0x600, 0x620, 0x900)
}

func TestRegisterStagedScriptCounterRecords(t *testing.T) {
	for _, tc := range []struct{ load, store string }{{"203b", "2940"}, {"207b", "2948"}} {
		t.Run(tc.load, func(t *testing.T) {
			rom, cfg, disc := actorFixture()
			disc.Section("actor_scripts").Table()["pointer_table_strides"] = ints(4, 8)
			copy(rom[0x400:], discoveryMustHex(t, tc.load+"007e4e714e71"+tc.store+"001c4e75"))
			put32(rom, 0x480, 0x600, 0, 0x620, 1, 0xFFFFFFFF)
			for _, s := range [][2]int{{0x600, 0x800}, {0x620, 0x820}} {
				put16(rom, s[0], 0)
				put32(rom, s[0]+2, uint32(s[1]))
				put16(rom, s[0]+6, 10)
				copy(rom[s[1]:], discoveryMustHex(t, "4e75"))
			}
			result := mustDiscover(t, rom, cfg)
			requirePCs(t, result, true, 0x800, 0x820)
			requirePCs(t, result, false, 0x480, 0x488, 0x600, 0x620)
		})
	}
}

func TestExplicitScriptTableCountExcludesAdjacentData(t *testing.T) {
	rom, cfg, disc := actorFixture()
	disc.Section("actor_scripts").Table()["pointer_tables"] = []any{table("table", int64(0x480), "count", int64(2))}
	copy(rom[0x400:], discoveryMustHex(t, "4e75"))
	put32(rom, 0x480, 0x600, 0x620, 0x640)
	for _, s := range [][2]int{{0x600, 0x800}, {0x620, 0x820}, {0x640, 0x840}} {
		put16(rom, s[0], 0)
		put32(rom, s[0]+2, uint32(s[1]))
		put16(rom, s[0]+6, 10)
		copy(rom[s[1]:], discoveryMustHex(t, "4e75"))
	}
	result := mustDiscover(t, rom, cfg)
	requirePCs(t, result, true, 0x800, 0x820)
	requirePCs(t, result, false, 0x480, 0x600, 0x620, 0x840)
}

func TestStagedJumpTablesWithBackwardDestinations(t *testing.T) {
	for _, dispatch := range []string{"207b003e4e714e714ed0", "41fb003e4e714e714ef00151"} {
		t.Run(dispatch, func(t *testing.T) {
			rom, cfg, disc := actorFixture()
			disc.Table()["scan_jump_tables"] = true
			put32(rom, 4, 0x500)
			copy(rom[0x500:], discoveryMustHex(t, dispatch))
			put32(rom, 0x540, 0x400, 0x420, 0xFFFFFFFF)
			copy(rom[0x400:], discoveryMustHex(t, "4e75"))
			copy(rom[0x420:], discoveryMustHex(t, "70014e75"))
			result := mustDiscover(t, rom, cfg)
			requirePCs(t, result, true, 0x400, 0x420, 0x422)
			requirePCs(t, result, false, 0x540)
			want := []int{0x400, 0x420}
			if got := result.IndirectTargets[0x508]; !slices.Equal(got, want) {
				t.Errorf("indirect targets = %v, want %v", got, want)
			}
			if got := result.BranchTargets[0x508]; !slices.Equal(got, want) {
				t.Errorf("branch targets = %v, want %v", got, want)
			}
		})
	}
}

func TestSignedFullExtensionTableDisplacement(t *testing.T) {
	rom, cfg, disc := actorFixture()
	disc.Table()["scan_jump_tables"] = true
	put32(rom, 4, 0x500)
	copy(rom[0x500:], discoveryMustHex(t, "4ebb0121ffe04e75"))
	put32(rom, 0x4E2, 0x400, 0x420, 0xFFFFFFFF)
	copy(rom[0x400:], discoveryMustHex(t, "4e75"))
	copy(rom[0x420:], discoveryMustHex(t, "4e75"))
	result := mustDiscover(t, rom, cfg)
	requirePCs(t, result, true, 0x400, 0x420, 0x506)
	requirePCs(t, result, false, 0x4E2)
}

func alignedFixture(coverage string) ([]byte, Manifest, Manifest) {
	rom := make([]byte, 0x1000)
	put32(rom, 0, 0x410000, 0x400)
	disc := table("coverage", coverage)
	return rom, ManifestOf(table("discovery", disc)), ManifestOf(disc)
}

func TestComputedTargetWithNoPointerLiteral(t *testing.T) {
	rom, cfg, _ := alignedFixture("all_aligned")
	copy(rom[0x400:], discoveryMustHex(t, "303c008041fa017ad0c04ed0"))
	copy(rom[0x600:], discoveryMustHex(t, "702a4e75"))
	if strings.Contains(string(rom), "\x00\x00\x06\x00") {
		t.Fatal("fixture must not contain a pointer literal")
	}
	result := mustDiscover(t, rom, cfg)
	requirePCs(t, result, true, 0x600, 0x602)
}

func TestOddOffset24BitPointerTarget(t *testing.T) {
	rom, cfg, _ := alignedFixture("all_aligned")
	copy(rom[0x481:], []byte{0x00, 0x07, 0x00})
	copy(rom[0x700:], discoveryMustHex(t, "4e714e75"))
	result := mustDiscover(t, rom, cfg)
	requirePCs(t, result, true, 0x700, 0x702)
}

func TestMoreThan32StraightLineInstructionsWithoutFilter(t *testing.T) {
	rom, cfg, _ := alignedFixture("all_aligned")
	copy(rom[0x800:], slices.Repeat(discoveryMustHex(t, "4e71"), 40))
	copy(rom[0x850:], discoveryMustHex(t, "4e75"))
	result := mustDiscover(t, rom, cfg)
	for i := range 41 {
		requirePCs(t, result, true, 0x800+i*2)
	}
}

func TestOverlappingStarts(t *testing.T) {
	rom, cfg, _ := alignedFixture("all_aligned")
	copy(rom[0x500:], discoveryMustHex(t, "203c4e714e75"))
	result := mustDiscover(t, rom, cfg)
	requirePCs(t, result, true, 0x500, 0x502, 0x504)
}

func TestOddPCsAndTruncatedInstructionBoundary(t *testing.T) {
	rom, cfg, _ := alignedFixture("all_aligned")
	copy(rom[0xFFE:], discoveryMustHex(t, "203c"))
	result := mustDiscover(t, rom, cfg)
	for pc, insn := range result.Instructions {
		if pc%2 != 0 {
			t.Errorf("odd PC found: %#x", pc)
		}
		if insn.Size%2 != 0 {
			t.Errorf("odd size at %#x: %d", pc, insn.Size)
		}
		if pc+int(insn.Size) > len(rom) {
			t.Errorf("instruction at %#x overruns the ROM", pc)
		}
	}
	requirePCs(t, result, false, 0x401, 0xFFE)
	if !slices.Contains(result.InvalidPCs, 0xFFE) {
		t.Error("truncated 0xffe missing from invalid PCs")
	}
	if _, err := (Game{Image: rom, Manifest: ManifestOf(table("discovery", table("entry_points", ints(0x401))))}).Discover(DiscoverOptions{}); err == nil {
		t.Error("odd entry point accepted")
	}
}

func TestComputedJumpTableTargetsAreCodegenOnly(t *testing.T) {
	rom, cfg, _ := alignedFixture("all_aligned")
	copy(rom[0x500:], discoveryMustHex(t, "303b0008"))
	copy(rom[0x504:], discoveryMustHex(t, "4efb0004"))
	copy(rom[0x508:], discoveryMustHex(t, "4e71"))
	put16(rom, 0x50A, 6, 8, 0)
	copy(rom[0x510:], discoveryMustHex(t, "4e714e75"))
	copy(rom[0x600:], discoveryMustHex(t, "4ef900000800"))
	copy(rom[0x800:], discoveryMustHex(t, "4e75"))
	result := mustDiscover(t, rom, cfg)
	if got, want := result.IndirectTargets[0x504], []int{0x50A, 0x510, 0x512}; !slices.Equal(got, want) {
		t.Errorf("indirect targets = %v, want %v", got, want)
	}
	if _, ok := result.BranchTargets[0x504]; ok {
		t.Error("computed jump became a proven branch")
	}
	if _, ok := result.Functions[0x504]; ok {
		t.Error("computed jump became a function")
	}
	found := false
	branches, _ := result.Report.Get("unresolved_branches")
	for _, b := range branches.([]any) {
		m := b.(interface {
			Get(string) (any, bool)
		})
		pc, _ := m.Get("pc")
		reason, _ := m.Get("reason")
		found = found || (pc == "0x000504" && reason == "indirect_transfer")
	}
	if !found {
		t.Error("indirect_transfer for 0x504 missing from the report")
	}
	if got := result.BranchTargets[0x600]; !slices.Equal(got, []int{0x800}) {
		t.Errorf("direct jmp targets = %v", got)
	}
	if _, ok := result.IndirectTargets[0x600]; ok {
		t.Error("direct jmp became a candidate")
	}
	if n := summaryInt(t, result, "indirect_targets_resolved_count"); n != 1 {
		t.Errorf("indirect_targets_resolved_count = %d, want 1", n)
	}
}

func summaryInt(t *testing.T, d *Analysis, key string) int {
	t.Helper()
	summary, _ := d.Report.Get("summary")
	v, _ := summary.(interface{ Get(string) (any, bool) }).Get(key)
	n, ok := v.(int)
	if !ok {
		t.Fatalf("summary[%q] = %v, not an int", key, v)
	}
	return n
}

func TestExhaustiveExplicitJumpTableOverridesScanner(t *testing.T) {
	rom, cfg, disc := alignedFixture("all_aligned")
	copy(rom[0x700:], discoveryMustHex(t, "4ed0"))
	copy(rom[0x720:], discoveryMustHex(t, "4e75"))
	copy(rom[0x740:], discoveryMustHex(t, "4e75"))
	disc.Table()["jump_tables"] = []any{table("address", int64(0x700), "targets", ints(0x740, 0x720))}
	result := mustDiscover(t, rom, cfg)
	if got, want := result.IndirectTargets[0x700], []int{0x720, 0x740}; !slices.Equal(got, want) {
		t.Errorf("indirect targets = %v, want %v", got, want)
	}
	if _, ok := result.BranchTargets[0x700]; ok {
		t.Error("explicit table became a proven branch")
	}
}

func TestRecursiveBehaviorRetained(t *testing.T) {
	rom, cfg, _ := alignedFixture("recursive")
	copy(rom[0x400:], discoveryMustHex(t, "4e75"))
	copy(rom[0x900:], discoveryMustHex(t, "4e714e75"))
	result := mustDiscover(t, rom, cfg)
	requirePCs(t, result, true, 0x400)
	requirePCs(t, result, false, 0x900, 0x902)
}

func exclusionFixture(coverage string) ([]byte, Manifest, Manifest) {
	rom, cfg, disc := alignedFixture(coverage)
	cfg.Table()["exclude"] = []any{exclusionRecord(nil)}
	return rom, cfg, disc
}

func exclusionRecord(changes map[string]any) map[string]any {
	record := table("start", int64(0x800), "end", int64(0x900), "reason", "synthetic data", "evidence", "test-owned descriptor interval")
	for k, v := range changes {
		record[k] = v
	}
	return record
}

func TestHalfOpenIntervalAndCandidateAccounting(t *testing.T) {
	rom, cfg, _ := exclusionFixture("all_aligned")
	copy(rom[0x7FE:], discoveryMustHex(t, "4e714e71"))
	copy(rom[0x8FE:], discoveryMustHex(t, "4e714e71"))
	result := mustDiscover(t, rom, cfg)
	requirePCs(t, result, true, 0x7FE, 0x900)
	for pc := range result.Instructions {
		if 0x800 <= pc && pc < 0x900 {
			t.Errorf("excluded PC %#x decoded", pc)
		}
	}
	for _, pc := range result.InvalidPCs {
		if 0x800 <= pc && pc < 0x900 {
			t.Errorf("excluded PC %#x reported invalid", pc)
		}
	}
	candidates := summaryInt(t, result, "aligned_candidate_count")
	sum := summaryInt(t, result, "aligned_decoded_count") + summaryInt(t, result, "aligned_invalid_count") + summaryInt(t, result, "excluded_candidate_count")
	if candidates != sum {
		t.Errorf("aligned_candidate_count %d != decoded+invalid+excluded %d", candidates, sum)
	}
}

func TestVectorAndExplicitTableTargetsRejectExclusions(t *testing.T) {
	for _, source := range []string{"vector", "targets", "table"} {
		t.Run(source, func(t *testing.T) {
			rom, cfg, disc := exclusionFixture("all_aligned")
			if source == "vector" {
				put32(rom, 4, 0x800)
			} else {
				jt := table("address", int64(0x500))
				if source == "targets" {
					jt["targets"] = ints(0x800)
				} else {
					put32(rom, 0x600, 0x800)
					jt["table"], jt["count"] = int64(0x600), int64(1)
				}
				disc.Table()["jump_tables"] = []any{jt}
			}
			_, err := Game{Image: rom, Manifest: cfg}.Discover(DiscoverOptions{})
			if err == nil || !strings.Contains(err.Error(), "excluded PC 0x800") {
				t.Fatalf("error = %v, want excluded PC 0x800", err)
			}
		})
	}
}

func TestUnprovenDirectTransferIsReportedForRuntimeEnforcement(t *testing.T) {
	rom, cfg, _ := exclusionFixture("all_aligned")
	copy(rom[0x500:], discoveryMustHex(t, "4ef900000800"))
	result := mustDiscover(t, rom, cfg)
	transfers, _ := result.Report.Get("excluded_transfers")
	found := false
	for _, tr := range transfers.([]any) {
		m := tr.(interface{ Get(string) (any, bool) })
		pc, _ := m.Get("pc")
		target, _ := m.Get("target")
		enforcement, _ := m.Get("enforcement")
		found = found || (pc == "0x000500" && target == "0x000800" && enforcement == "fatal_at_runtime")
	}
	if !found {
		t.Error("excluded transfer missing from the report")
	}
	requirePCs(t, result, false, 0x800)
}

func TestRecursiveReachableExclusionIsNotSilentlyDropped(t *testing.T) {
	rom, cfg, _ := exclusionFixture("recursive")
	copy(rom[0x400:], discoveryMustHex(t, "4ef900000800"))
	if _, err := (Game{Image: rom, Manifest: cfg}).Discover(DiscoverOptions{}); err == nil || !strings.Contains(err.Error(), "excluded PC 0x800") {
		t.Fatalf("error = %v, want excluded PC 0x800", err)
	}
}

func TestOverlapOddBoundsAndMissingEvidenceReject(t *testing.T) {
	rom, cfg, _ := exclusionFixture("all_aligned")
	for name, change := range map[string]map[string]any{
		"odd start":      table("start", int64(0x801)),
		"end past rom":   table("end", int64(0x1002)),
		"empty evidence": table("evidence", ""),
	} {
		t.Run(name, func(t *testing.T) {
			bad := ManifestOf(table("discovery", cfg.Table()["discovery"], "exclude", []any{exclusionRecord(change)}))
			if _, err := (Game{Image: rom, Manifest: bad}).Discover(DiscoverOptions{}); err == nil {
				t.Error("invalid exclusion accepted")
			}
		})
	}
	overlap := ManifestOf(table("discovery", cfg.Table()["discovery"], "exclude", []any{exclusionRecord(nil), exclusionRecord(nil)}))
	if _, err := (Game{Image: rom, Manifest: overlap}).Discover(DiscoverOptions{}); err == nil || !strings.Contains(err.Error(), "Overlapping") {
		t.Errorf("error = %v, want Overlapping", err)
	}
}

var chip = func() []byte {
	b := make([]byte, 16)
	for i := range b {
		b[i] = byte(i)
	}
	return b
}()

func romConfigText(romExtra, laneExtra string) string {
	sum := sha1.Sum(chip)
	var lanes strings.Builder
	for i := range 4 {
		fmt.Fprintf(&lanes, "[[rom.lanes]]\nfile = \"lane%d\"\noffset = %d\nsize = 16\ncrc = \"%08x\"\nsha1 = \"%s\"\n%s\n",
			i, i, crc32.ChecksumIEEE(chip), hex.EncodeToString(sum[:]), laneExtra)
	}
	return fmt.Sprintf("[game]\nid = \"synthetic\"\n[rom]\nsize = 64\ninterleave = 4\n%s\n%s", romExtra, lanes.String())
}

func loadSynthetic(t *testing.T, romExtra, laneExtra string) ([]byte, error) {
	t.Helper()
	dir := t.TempDir()
	for i := range 4 {
		if err := os.WriteFile(filepath.Join(dir, fmt.Sprintf("lane%d", i)), chip, 0o644); err != nil {
			t.Fatal(err)
		}
	}
	configPath := filepath.Join(dir, "config.toml")
	if err := os.WriteFile(configPath, []byte(romConfigText(romExtra, laneExtra)), 0o644); err != nil {
		t.Fatal(err)
	}
	game, err := LoadGame(configPath, dir)
	return game.Image, err
}

func TestLoadROMKnownKeysLoad(t *testing.T) {
	rom, err := loadSynthetic(t, "", "")
	if err != nil {
		t.Fatal(err)
	}
	if len(rom) != 64 {
		t.Errorf("len(rom) = %d, want 64", len(rom))
	}
	for i, b := range rom {
		if b != chip[i/4] {
			t.Fatalf("rom[%d] = %#x, want %#x", i, b, chip[i/4])
		}
	}
}

func TestLoadROMUnknownKeysNameFileAndKey(t *testing.T) {
	for _, tc := range []struct {
		name, romExtra, laneExtra, want string
	}{
		{"rom", "bogus = 1", "", `config.toml': unknown key(s) ['bogus'] in [rom]`},
		{"lane", "", "typo = 1", `config.toml': unknown key(s) ['typo'] in [[rom.lanes]]`},
	} {
		t.Run(tc.name, func(t *testing.T) {
			_, err := loadSynthetic(t, tc.romExtra, tc.laneExtra)
			if err == nil || !strings.Contains(err.Error(), tc.want) {
				t.Fatalf("error = %v, want it to contain %q", err, tc.want)
			}
		})
	}
}

func TestEveryGameConfigUsesOnlyKnownROMKeys(t *testing.T) {
	configs, err := filepath.Glob("../games/*/config.toml")
	if err != nil || len(configs) == 0 {
		t.Fatalf("no game configs found: %v", err)
	}
	for _, path := range configs {
		t.Run(filepath.Base(filepath.Dir(path)), func(t *testing.T) {
			cfg, err := LoadManifest(path)
			if err != nil {
				t.Fatal(err)
			}
			romCfg := cfg.Section("rom")
			if err := romCfg.RejectUnknown("[rom]", path, romKeys); err != nil {
				t.Error(err)
			}
			for _, lane := range romCfg.List("lanes") {
				laneTable, _ := manifestValue(lane)
				if err := laneTable.RejectUnknown("[[rom.lanes]]", path, romLaneKeys); err != nil {
					t.Error(err)
				}
			}
		})
	}
}

func TestLandmakrjFFFillsMatchRealProgramImages(t *testing.T) {
	romDir := "../roms/landmakrj"
	if info, err := os.Stat(romDir); err != nil || !info.IsDir() {
		t.Skip("landmakrj ROM directory missing")
	}
	game, err := LoadGame("../games/landmakrj/config.toml", romDir)
	if err != nil {
		t.Fatal(err)
	}
	rom, cfg := game.Image, game.Manifest
	main, err := ParseExclusions(cfg, "main", 0, rom)
	if err != nil {
		t.Fatal(err)
	}
	soundROM, err := LoadRegion(cfg, "sound", romDir)
	if err != nil {
		t.Fatal(err)
	}
	if len(soundROM) < 0x80000 {
		t.Fatal("sound manifest must cover the 512 KiB CPU window")
	}
	sound, err := ParseExclusions(cfg, "sound", 0xC00000, soundROM[:0x80000])
	if err != nil {
		t.Fatal(err)
	}
	records := cfg.List("exclude")
	filled := 0
	for _, r := range records {
		record, _ := manifestValue(r)
		if _, ok := record.Value("fill"); ok {
			filled++
		}
	}
	if filled != 4 {
		t.Errorf("filled records = %d, want 4", filled)
	}
	if len(main)+len(sound) != len(records) {
		t.Errorf("main %d + sound %d != %d records", len(main), len(sound), len(records))
	}
}
