package main

import (
	"crypto/sha1"
	"encoding/hex"
	"fmt"
	"hash/crc32"
	"os"
	"path/filepath"
	"slices"
	"strings"
	"testing"
)

func lane(name string, data []byte, offset, stride, group int) map[string]any {
	sum := sha1.Sum(data)
	return map[string]any{
		"file": name, "size": len(data), "offset": offset, "stride": stride, "group": group,
		"crc": fmt.Sprintf("%08x", crc32.ChecksumIEEE(data)), "sha1": hex.EncodeToString(sum[:]),
	}
}

func with(l map[string]any, kv ...any) map[string]any {
	for i := 0; i < len(kv); i += 2 {
		l[kv[i].(string)] = kv[i+1]
	}
	return l
}

func region(size int, lanes ...map[string]any) map[string]any {
	list := make([]any, len(lanes))
	for i, l := range lanes {
		list[i] = l
	}
	return map[string]any{"size": size, "lanes": list}
}

func writeChips(t *testing.T, root string, chips map[string][]byte) {
	t.Helper()
	for name, data := range chips {
		if err := os.WriteFile(filepath.Join(root, name), data, 0o644); err != nil {
			t.Fatal(err)
		}
	}
}

func mustLoad(t *testing.T, cfg Manifest, name, root string) []byte {
	t.Helper()
	got, err := LoadRegion(cfg, name, root)
	if err != nil {
		t.Fatal(err)
	}
	return got
}

func wantError(t *testing.T, err error, substring string) {
	t.Helper()
	if err == nil || !strings.Contains(err.Error(), substring) {
		t.Errorf("error = %v, want containing %q", err, substring)
	}
}

func TestSizeZeroRegionIsAbsentPlaneNotAChipFile(t *testing.T) {
	cfg := ManifestOf(map[string]any{"tiles_hi": region(0)})
	spec, err := cfg.RegionSpec("tiles_hi")
	if err != nil || spec.Size != 0 || spec.Fill != 0 || len(spec.Lanes) != 0 {
		t.Errorf("RegionSpec = %+v, %v", spec, err)
	}
	if got := mustLoad(t, cfg, "tiles_hi", t.TempDir()); len(got) != 0 {
		t.Errorf("image = %v", got)
	}
	_, err = ManifestOf(map[string]any{"tiles_hi": region(0, lane("fake", make([]byte, 4), 0, 1, 1))}).RegionSpec("tiles_hi")
	wantError(t, err, "exceeds region")
	spec, err = ManifestOf(map[string]any{}).RegionSpec("eeprom")
	if err != nil || spec.Size != 0 || len(spec.Lanes) != 0 {
		t.Errorf("absent eeprom = %+v, %v", spec, err)
	}
}

func TestGroupedTileLanesAndSparseZeroSamples(t *testing.T) {
	root := t.TempDir()
	writeChips(t, root, map[string][]byte{"low": {1, 2, 3, 4}, "mid": {5, 6, 7, 8}})
	cfg := ManifestOf(map[string]any{"tiles": region(8, lane("low", []byte{1, 2, 3, 4}, 0, 4, 2), lane("mid", []byte{5, 6, 7, 8}, 2, 4, 2))})
	if got := mustLoad(t, cfg, "tiles", root); !slices.Equal(got, []byte{1, 2, 5, 6, 3, 4, 7, 8}) {
		t.Errorf("tiles = %v", got)
	}
	cfg = ManifestOf(map[string]any{"samples": region(12, lane("low", []byte{1, 2, 3, 4}, 4, 2, 1))})
	if got := mustLoad(t, cfg, "samples", root); !slices.Equal(got, []byte{0, 0, 0, 0, 1, 0, 2, 0, 3, 0, 4, 0}) {
		t.Errorf("samples = %v", got)
	}
}

func TestSoundMirrorKeepsBothBanksAndRejectsCorruption(t *testing.T) {
	root := t.TempDir()
	data := []byte{1, 2, 3, 4}
	writeChips(t, root, map[string][]byte{"program": data})
	cfg := ManifestOf(map[string]any{"sound": with(region(4, lane("program", data, 0, 1, 1)), "mirror_size", 8)})
	if got := mustLoad(t, cfg, "sound", root); !slices.Equal(got, []byte{1, 2, 3, 4, 1, 2, 3, 4}) {
		t.Errorf("sound = %v", got)
	}
	writeChips(t, root, map[string][]byte{"program": {1, 2, 3, 5}})
	_, err := LoadRegion(cfg, "sound", root)
	wantError(t, err, "CRC32 mismatch")
}

func TestInvalidMirrorSize(t *testing.T) {
	for _, mirror := range []any{3, 0, 6, "8"} {
		cfg := ManifestOf(map[string]any{"sound": with(region(4, lane("program", []byte{1, 2, 3, 4}, 0, 1, 1)), "mirror_size", mirror)})
		root := t.TempDir()
		writeChips(t, root, map[string][]byte{"program": {1, 2, 3, 4}})
		_, err := LoadRegion(cfg, "sound", root)
		wantError(t, err, "Invalid sound mirrored size")
	}
}

func TestContinueThenPatchPreservesGseekerLaneOrder(t *testing.T) {
	root := t.TempDir()
	mask03 := []byte{1, 2, 3, 4, 5, 6, 7, 8}
	mask04 := []byte{9, 10, 11, 12, 13, 14, 15, 16}
	patch15 := []byte{0x21, 0x22, 0x23, 0x24}
	patch16 := []byte{0x31, 0x32, 0x33, 0x34}
	writeChips(t, root, map[string][]byte{"d40_03.rom": mask03, "d40_04.rom": mask04, "d40_15.rom": patch15, "d40_16.rom": patch16})
	firstHalf := with(lane("d40_04.rom", mask04, 9, 2, 1), "source_offset", 0, "length", 4)
	continuedHalf := with(lane("d40_04.rom", mask04, 0, 2, 1), "source_offset", 4, "length", 4)
	lanes := []map[string]any{lane("d40_03.rom", mask03, 0, 2, 1), firstHalf, continuedHalf}
	cfg := ManifestOf(map[string]any{"sprites": with(region(16, lanes...), "fill", 0)})
	if got := mustLoad(t, cfg, "sprites", root); !slices.Equal(got, []byte{13, 0, 14, 0, 15, 0, 16, 0, 5, 9, 6, 10, 7, 11, 8, 12}) {
		t.Errorf("sprites = %v", got)
	}
	lanes = append(lanes, lane("d40_15.rom", patch15, 0, 2, 1), lane("d40_16.rom", patch16, 1, 2, 1))
	cfg = ManifestOf(map[string]any{"sprites": with(region(16, lanes...), "fill", 0)})
	want := []byte{0x21, 0x31, 0x22, 0x32, 0x23, 0x33, 0x24, 0x34, 5, 9, 6, 10, 7, 11, 8, 12}
	if got := mustLoad(t, cfg, "sprites", root); !slices.Equal(got, want) {
		t.Errorf("patched sprites = %v", got)
	}
}

func TestContinueValidatesBytesOutsideSelectedSourceSlice(t *testing.T) {
	root := t.TempDir()
	chip := []byte{1, 2, 3, 4, 5, 6, 7, 8}
	for _, sourceOffset := range []int{0, 4} {
		t.Run(fmt.Sprint(sourceOffset), func(t *testing.T) {
			spec := with(lane("d40_04.rom", chip, 1, 2, 1), "source_offset", sourceOffset, "length", 4)
			cfg := ManifestOf(map[string]any{"sprites": with(region(8, spec), "fill", 0)})
			writeChips(t, root, map[string][]byte{"d40_04.rom": chip})
			selected := chip[sourceOffset : sourceOffset+4]
			want := []byte{0, selected[0], 0, selected[1], 0, selected[2], 0, selected[3]}
			if got := mustLoad(t, cfg, "sprites", root); !slices.Equal(got, want) {
				t.Errorf("sprites = %v, want %v", got, want)
			}
			corrupt := slices.Clone(chip)
			corrupt[4-sourceOffset] ^= 0xFF
			writeChips(t, root, map[string][]byte{"d40_04.rom": corrupt})
			_, err := LoadRegion(cfg, "sprites", root)
			wantError(t, err, "CRC32 mismatch")
		})
	}
}

func TestOnlyHashValidatedShortDumpsAreFFPadded(t *testing.T) {
	root := t.TempDir()
	short := []byte{1, 2}
	full := []byte{1, 2, 0xFF, 0xFF}
	shortSum := sha1.Sum(short)
	spec := with(lane("chip", full, 0, 1, 1), "short_size", 2,
		"short_crc", fmt.Sprintf("%08x", crc32.ChecksumIEEE(short)), "short_sha1", hex.EncodeToString(shortSum[:]))
	cfg := ManifestOf(map[string]any{"sound": with(region(4, spec), "fill", 255)})
	writeChips(t, root, map[string][]byte{"chip": short})
	if got := mustLoad(t, cfg, "sound", root); !slices.Equal(got, full) {
		t.Errorf("sound = %v", got)
	}
	shape, err := cfg.RegionSpec("sound")
	if err != nil || shape.Lanes[0].ShortSize != 2 || shape.Lanes[0].ShortCRC == "" {
		t.Errorf("lane = %+v, %v", shape.Lanes, err)
	}
	writeChips(t, root, map[string][]byte{"chip": {1, 3}})
	_, err = LoadRegion(cfg, "sound", root)
	wantError(t, err, "CRC32 mismatch")
}

func TestGroupCannotWritePastRegion(t *testing.T) {
	cfg := ManifestOf(map[string]any{"tiles": region(7, lane("chip", []byte{1, 2, 3, 4}, 2, 4, 2))})
	_, err := LoadRegion(cfg, "tiles", "unused")
	wantError(t, err, "exceeds region")
}

func TestWordSwap(t *testing.T) {
	root := t.TempDir()
	writeChips(t, root, map[string][]byte{"chip": {1, 2, 3, 4}})
	cfg := ManifestOf(map[string]any{"rom": region(4, with(lane("chip", []byte{1, 2, 3, 4}, 0, 1, 1), "word_swap", true))})
	if got := mustLoad(t, cfg, "rom", root); !slices.Equal(got, []byte{2, 1, 4, 3}) {
		t.Errorf("rom = %v", got)
	}
	cfg = ManifestOf(map[string]any{"rom": region(8, with(lane("chip", []byte{1, 2, 3, 4}, 0, 2, 1), "word_swap", true))})
	_, err := LoadRegion(cfg, "rom", root)
	wantError(t, err, "word_swap requires group=1, stride=1 and even length")
	cfg = ManifestOf(map[string]any{"rom": region(4, with(lane("chip", []byte{1, 2, 3, 4}, 0, 1, 1), "word_swap", 1))})
	_, err = cfg.RegionSpec("rom")
	wantError(t, err, "Invalid rom word_swap: chip")
}

func TestRegionSpecRejects(t *testing.T) {
	good := lane("chip", []byte{1, 2, 3, 4}, 0, 1, 1)
	tests := []struct {
		name string
		cfg  Manifest
		want string
	}{
		{"fill", ManifestOf(map[string]any{"rom": with(region(4, good), "fill", 1)}), "Invalid rom region size/fill"},
		{"negative size", ManifestOf(map[string]any{"rom": region(-1, good)}), "Invalid rom region size/fill"},
		{"eeprom size", ManifestOf(map[string]any{"eeprom": region(64, good)}), "EEPROM region must be absent or exactly 128 bytes"},
		{"missing lanes", ManifestOf(map[string]any{"rom": region(4)}), "Missing rom ROM lanes"},
		{"geometry", ManifestOf(map[string]any{"rom": region(4, with(lane("chip", []byte{1, 2, 3, 4}, 0, 1, 1), "length", 5))}), "Invalid rom lane geometry: chip"},
		{"non-integer geometry", ManifestOf(map[string]any{"rom": region(4, with(lane("chip", []byte{1, 2, 3, 4}, 0, 1, 1), "offset", 1.5))}), "Invalid rom lane geometry"},
		{"crc", ManifestOf(map[string]any{"rom": region(4, with(lane("chip", []byte{1, 2, 3, 4}, 0, 1, 1), "crc", "abc"))}), "Invalid rom crc: chip"},
		{"sha1 digits", ManifestOf(map[string]any{"rom": region(4, with(lane("chip", []byte{1, 2, 3, 4}, 0, 1, 1), "sha1", strings.Repeat("z", 40)))}), "invalid literal for int()"},
		{"short size", ManifestOf(map[string]any{"rom": region(4, with(lane("chip", []byte{1, 2, 3, 4}, 0, 1, 1), "short_size", 4))}), "Invalid short dump size: chip"},
		{"inconsistent identity", ManifestOf(map[string]any{"rom": region(8, good, with(lane("chip", []byte{1, 2, 3, 4}, 4, 1, 1), "crc", "00000000"))}), "Inconsistent physical chip identity: chip"},
		{"missing region", ManifestOf(map[string]any{}), "missing rom region"},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			name := "rom"
			if _, ok := tt.cfg.Table()["eeprom"]; ok {
				name = "eeprom"
			}
			_, err := tt.cfg.RegionSpec(name)
			wantError(t, err, tt.want)
		})
	}
}

func TestLaneDefaults(t *testing.T) {
	data := []byte{1, 2, 3, 4}
	spec := lane("chip", data, 0, 1, 1)
	delete(spec, "stride")
	delete(spec, "group")
	delete(spec, "offset")
	cfg := ManifestOf(map[string]any{"rom": with(region(8, spec), "interleave", 2)})
	shape, err := cfg.RegionSpec("rom")
	if err != nil {
		t.Fatal(err)
	}
	lanes := shape.Lanes
	want := Lane{File: "chip", Size: 4, CRC: lanes[0].CRC, SHA1: lanes[0].SHA1, Stride: 2, Group: 1, Length: 4}
	if lanes[0] != want {
		t.Errorf("lane = %+v, want %+v", lanes[0], want)
	}
}
