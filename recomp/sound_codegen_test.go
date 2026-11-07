package main

import (
	"encoding/hex"
	"fmt"
	"hash/crc32"
	"os"
	"os/exec"
	"path/filepath"
	"slices"
	"strconv"
	"strings"
	"testing"
)

const driverPrelude = `
#include <f3rt/cpu_abi.h>
int f3_validate_main_rom(f3_cpu *cpu, size_t size, uint32_t crc) {
    (void)cpu; (void)size; (void)crc; return 1;
}
static const f3_excluded_range *registered_exclusions;
static size_t registered_exclusion_count;
int f3_register_exclusions(f3_cpu *cpu, const f3_excluded_range *ranges, size_t count) {
    (void)cpu; registered_exclusions=ranges; registered_exclusion_count=count; return 1;
}
`

func soundExecute(t *testing.T, output string, emission Emission, driver string) {
	t.Helper()
	root := generateRepoRoot(t)
	if _, err := os.Stat(filepath.Join(root, "runtime", "audio", "reference", "native", "sound_native_ops.h")); err != nil {
		t.Skip("sound_native_ops.h missing")
	}
	compiler := os.Getenv("CC")
	if compiler == "" {
		compiler = "cc"
	}
	if _, err := exec.LookPath(compiler); err != nil {
		t.Skipf("C compiler unavailable: %v", err)
	}
	path := filepath.Join(output, "driver.c")
	if err := os.WriteFile(path, []byte(driverPrelude+driver), 0o666); err != nil {
		t.Fatal(err)
	}
	binary := filepath.Join(output, "driver")
	args := []string{
		"-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-DF3_PROFILE_INSTRUMENT=1",
		"-I", root, "-I", filepath.Join(root, "include"), "-I", output, path,
	}
	for _, name := range emission.Files {
		args = append(args, filepath.Join(output, name))
	}
	args = append(args, "-o", binary)
	if out, err := exec.Command(compiler, args...).CombinedOutput(); err != nil {
		t.Fatalf("compile: %v\n%s", err, out)
	}
	if out, err := exec.Command(binary).CombinedOutput(); err != nil {
		t.Fatalf("driver: %v\n%s", err, out)
	}
}

func compileSoundProgram(rom []byte, cfg Manifest, opts SoundOptions) (*SoundProgram, error) {
	exclusions, err := ParseExclusions(cfg, "sound", romBase, rom)
	if err != nil {
		return nil, err
	}
	return (&SoundImage{Image: rom, Manifest: cfg, Exclusions: exclusions}).Compile(opts)
}

func compileSound(t *testing.T, rom []byte, cfg Manifest, output string, opts SoundOptions) Emission {
	t.Helper()
	program, err := compileSoundProgram(rom, cfg, opts)
	if err != nil {
		t.Fatal(err)
	}
	emission, err := program.Write(output)
	if err != nil {
		t.Fatal(err)
	}
	return emission
}

func soundMustHex(t *testing.T, s string) []byte {
	t.Helper()
	data, err := hex.DecodeString(s)
	if err != nil {
		t.Fatal(err)
	}
	return data
}

func soundWriteProfile(t *testing.T, path string, rom []byte, base int, hits ...int) {
	t.Helper()
	profile := NewProfile()
	crc := crc32.ChecksumIEEE(rom)
	profile.Roms[RomKey{Region: "sound", CRC: crc}] = Bounds{Base: base, Size: len(rom)}
	for _, address := range hits {
		profile.Hits[RowKey{Region: "sound", CRC: crc, Address: address}] = 1
	}
	if err := profile.Write(path); err != nil {
		t.Fatal(err)
	}
}

func profileOptions(mode, path string) SoundOptions {
	if mode == "tiers" {
		return SoundOptions{ProfileTiers: path}
	}
	return SoundOptions{ProfileSlim: path}
}

func slimInt(mode string) string {
	return strconv.Itoa(map[bool]int{false: 0, true: 1}[mode == "slim"])
}

func TestSoundSharedExceptionsCountedOnceAndSlimTableSparse(t *testing.T) {
	rom := soundMustHex(t, "4e714e71a000f0004afc")
	root := t.TempDir()
	profile := filepath.Join(root, "entries.profile")
	soundWriteProfile(t, profile, rom, 0xC00000, 0xC00000, 0xC00004)
	const driver = `
#include <assert.h>
#include "sound_program.h"
#include <f3rt/block_profile.h>
uint64_t counts[5];
uint64_t *f3_profile_main_counts;
uint64_t *f3_profile_sound_counts=counts;
static unsigned exception;
void f3_sound_exception(f3_cpu *cpu,unsigned vector,uint32_t pc) {
    (void)cpu; (void)pc; exception=vector;
}
void f3_sound_unsupported_pc(f3_cpu *cpu,uint32_t pc) { (void)cpu; (void)pc; assert(0); }
int main(void) {
    assert(f3_sound_block_count==(SLIM ? 2u : 5u));
    for(size_t i=0;i<f3_sound_block_count;++i) {
        f3_cpu cpu={0}; cpu.pc=f3_sound_blocks[i].address;
        unsigned index=(cpu.pc-0xc00000u)/2;
        exception=0; f3_sound_blocks[i].soundExecute(&cpu);
        assert(counts[index]==1);
        if(index<2) assert(cpu.pc==0xc00000u+(index+1)*2 && cpu.cycles==4);
        else assert(exception==(index==2 ? 10u : index==3 ? 11u : 4u));
    }
    if(SLIM) assert(!counts[1] && !counts[3] && !counts[4]);
    return 0;
}
`
	for _, mode := range []string{"baseline", "tiers", "slim"} {
		t.Run(mode, func(t *testing.T) {
			output := filepath.Join(root, mode)
			var opts SoundOptions
			if mode != "baseline" {
				opts = profileOptions(mode, profile)
			}
			emission := compileSound(t, rom, defaultManifest, output, opts)
			soundExecute(t, output, emission, "#define SLIM "+slimInt(mode)+"\n"+driver)
		})
	}
}

func TestSoundExclusionsPreserveTierComplementAndSlimSharedExceptions(t *testing.T) {
	rom := soundMustHex(t, "4e714e71a000a001a002f000")
	const base = 0xC00000
	cfg := ManifestOf(map[string]any{"exclude": []any{map[string]any{
		"cpu": "sound", "start": int64(base + 4), "end": int64(base + 6), "reason": "synthetic data", "evidence": "fixture",
	}}})
	root := t.TempDir()
	path := filepath.Join(root, "entries.profile")
	soundWriteProfile(t, path, rom, base, base, base+6)
	const driver = `
#include <assert.h>
#include "sound_program.h"
#include <f3rt/block_profile.h>
uint64_t counts[6];
uint64_t *f3_profile_main_counts;
uint64_t *f3_profile_sound_counts=counts;
static unsigned exception;
void f3_sound_exception(f3_cpu *cpu,unsigned vector,uint32_t pc) {
    assert(pc==cpu->pc); exception=vector;
}
void f3_sound_unsupported_pc(f3_cpu *cpu,uint32_t pc) { (void)cpu; (void)pc; assert(0); }
int main(void) {
    assert(f3_sound_excluded_count==1);
    assert(f3_sound_excluded_ranges[0].start==0xc00004u);
    assert(f3_sound_excluded_ranges[0].end==0xc00006u);
    const uint32_t expected[]={0, SLIM ? 6u : 2u, 6, 8, 10};
    assert(f3_sound_block_count==(SLIM ? 2u : 5u));
    for(size_t i=0;i<f3_sound_block_count;++i) {
        f3_cpu cpu={0}; cpu.pc=f3_sound_blocks[i].address;
        assert(cpu.pc==0xc00000u+expected[i]); exception=0;
        f3_sound_blocks[i].soundExecute(&cpu);
        if(expected[i]>=6) assert(exception==(expected[i]==10 ? 11u : 10u));
        else assert(cpu.pc==0xc00000u+expected[i]+2 && cpu.cycles==4);
    }
    assert(counts[0]==1 && counts[3]==1 && !counts[2]);
    assert(counts[1]==(SLIM ? 0u : 1u));
    assert(counts[4]==(SLIM ? 0u : 1u) && counts[5]==(SLIM ? 0u : 1u));
    return 0;
}
`
	for _, mode := range []string{"tiers", "slim"} {
		t.Run(mode, func(t *testing.T) {
			output := filepath.Join(root, mode)
			emission := compileSound(t, rom, cfg, output, profileOptions(mode, path))
			retained := 5
			if mode == "slim" {
				retained = 2
			}
			got := []int{
				intField(t, emission.Report, "original_entries"), intField(t, emission.Report, "retained_entries"),
				intField(t, emission.Report, "compiled_blocks"), intField(t, emission.Report, "hot_entries"),
				intField(t, emission.Report, "cold_entries"), intField(t, emission.Report, "excluded_words"),
			}
			if want := []int{5, retained, retained, 2, 3, 1}; !slices.Equal(got, want) {
				t.Errorf("report counts = %v, want %v", got, want)
			}
			data, err := os.ReadFile(filepath.Join(output, "profile_inventory.json"))
			if err != nil {
				t.Fatal(err)
			}
			var entries []string
			for _, offset := range []int{0, 2, 6, 8, 10} {
				entries = append(entries, strconv.Itoa(base+offset))
			}
			wantEntries := "\"entries\": [\n    " + strings.Join(entries, ",\n    ") + "\n  ]"
			if !strings.Contains(string(data), wantEntries) {
				t.Errorf("inventory entries mismatch:\n%s", data)
			}
			for _, want := range []string{
				fmt.Sprintf(`"retained_entries": %d`, retained), `"hot_entries": 2`, `"cold_entries": 3`,
			} {
				if !strings.Contains(string(data), want) {
					t.Errorf("inventory lacks %s:\n%s", want, data)
				}
			}
			soundExecute(t, output, emission, "#define SLIM "+slimInt(mode)+"\n"+driver)
		})
	}
}

func TestSoundRejectsContradictoryProfilesAndExcludedVectors(t *testing.T) {
	rom := soundMustHex(t, "4e714e71a000")
	const base = 0xC00000
	cfg := ManifestOf(map[string]any{"exclude": []any{map[string]any{
		"cpu": "sound", "start": int64(base + 4), "end": int64(base + 6), "reason": "synthetic data", "evidence": "fixture",
	}}})
	root := t.TempDir()
	crc := crc32.ChecksumIEEE(rom)
	for _, mode := range []string{"tiers", "slim"} {
		for _, kind := range []string{"hit", "miss"} {
			for _, address := range []int{base + 4, base + 5, 0x01C00004, 0x01C00005} {
				t.Run(fmt.Sprintf("%s/%s/%#x", mode, kind, address), func(t *testing.T) {
					profile := NewProfile()
					profile.Roms[RomKey{Region: "sound", CRC: crc}] = Bounds{Base: base, Size: len(rom)}
					profile.Hits[RowKey{Region: "sound", CRC: crc, Address: base}] = 1
					rows := profile.Hits
					if kind == "miss" {
						rows = profile.Misses
					}
					rows[RowKey{Region: "sound", CRC: crc, Address: address}] = 1
					path := filepath.Join(root, "contradictory.profile")
					if err := profile.Write(path); err != nil {
						t.Fatal(err)
					}
					if _, err := compileSoundProgram(rom, cfg, profileOptions(mode, path)); err == nil {
						t.Error("contradictory profile accepted")
					}
				})
			}
		}
	}
	for _, address := range []int{base + 4, base + 5} {
		t.Run(fmt.Sprintf("vector/%#x", address), func(t *testing.T) {
			vector := append(soundMustHex(t, "4e714e71"), byte(address>>24), byte(address>>16), byte(address>>8), byte(address))
			if _, err := compileSoundProgram(vector, cfg, SoundOptions{}); err == nil {
				t.Error("excluded vector accepted")
			}
		})
	}
}

func TestProfilesAreMutuallyExclusive(t *testing.T) {
	_, err := (&SoundImage{}).Compile(SoundOptions{ProfileTiers: "a", ProfileSlim: "b"})
	if err == nil || err.Error() != "profile_tiers and profile_slim are mutually exclusive" {
		t.Errorf("err = %v", err)
	}
}

func TestLoadSoundROMLandmakrjFFFillsMatchRealProgramImages(t *testing.T) {
	root := generateRepoRoot(t)
	romDir := filepath.Join(root, "roms", "landmakrj")
	if _, err := os.Stat(romDir); err != nil {
		t.Skip("landmakrj ROM directory missing")
	}
	game, err := LoadGame(filepath.Join(root, "games", "landmakrj", "config.toml"), romDir)
	if err != nil {
		t.Fatal(err)
	}
	main, err := ParseExclusions(game.Manifest, "main", 0, game.Image)
	if err != nil {
		t.Fatal(err)
	}
	soundImage, err := LoadSoundImage(game.Manifest, romDir)
	if err != nil {
		t.Fatal(err)
	}
	sound := soundImage.Exclusions
	records := listValue(game.Manifest.Table()["exclude"])
	filled := 0
	for _, record := range records {
		if _, ok := record.(map[string]any)["fill"]; ok {
			filled++
		}
	}
	if filled != 4 {
		t.Errorf("filled records = %d, want 4", filled)
	}
	if len(main)+len(sound) != len(records) {
		t.Errorf("main %d + sound %d != %d exclude records", len(main), len(sound), len(records))
	}
}
