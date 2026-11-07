package main

import (
	"fmt"
	"hash/crc32"
	"maps"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func profile(roms map[RomKey]Bounds, hits, misses map[RowKey]uint64) *Profile {
	p := NewProfile()
	maps.Copy(p.Roms, roms)
	maps.Copy(p.Hits, hits)
	maps.Copy(p.Misses, misses)
	return p
}

func hotFrom(t *testing.T, path, region string, rom []byte, base int, exclusions Exclusions) (map[int]struct{}, error) {
	t.Helper()
	loaded, err := ReadProfile(path)
	if err != nil {
		return nil, err
	}
	return loaded.Hot(region, rom, base, exclusions)
}

func mustRead(t *testing.T, path string) *Profile {
	t.Helper()
	p, err := ReadProfile(path)
	if err != nil {
		t.Fatal(err)
	}
	return p
}

func equalRows(a, b map[RowKey]uint64) bool {
	return maps.Equal(a, b)
}

func TestUnionKeepsRomIdentitiesAndSaturatesCounts(t *testing.T) {
	left := profile(map[RomKey]Bounds{{"main", 1}: {0, 8}}, map[RowKey]uint64{{"main", 1, 2}: MaxCount}, nil)
	right := profile(
		map[RomKey]Bounds{{"main", 1}: {0, 8}, {"main", 2}: {0, 8}},
		map[RowKey]uint64{{"main", 1, 2}: 3, {"main", 2, 2}: 7},
		map[RowKey]uint64{{"main", 1, 4}: 1},
	)
	if err := left.Merge(right); err != nil {
		t.Fatal(err)
	}
	path := filepath.Join(t.TempDir(), "merged.profile")
	if err := left.Write(path); err != nil {
		t.Fatal(err)
	}
	loaded := mustRead(t, path)
	if want := map[RowKey]uint64{{"main", 1, 2}: MaxCount, {"main", 2, 2}: 7}; !equalRows(loaded.Hits, want) {
		t.Errorf("hits = %v", loaded.Hits)
	}
	if want := map[RowKey]uint64{{"main", 1, 4}: 1}; !equalRows(loaded.Misses, want) {
		t.Errorf("misses = %v", loaded.Misses)
	}
}

func TestWriteFormat(t *testing.T) {
	p := profile(
		map[RomKey]Bounds{{"sound", 0xAB}: {0xC00000, 8}, {"main", 0x10}: {0, 8}},
		map[RowKey]uint64{{"main", 0x10, 4}: 5, {"main", 0x10, 2}: 1, {"main", 0x10, 6}: 0},
		map[RowKey]uint64{{"sound", 0xAB, 0xC00002}: MaxCount},
	)
	path := filepath.Join(t.TempDir(), "nested", "out.profile")
	if err := p.Write(path); err != nil {
		t.Fatal(err)
	}
	got, _ := os.ReadFile(path)
	want := "F3-BLOCK-PROFILE 1\n" +
		"rom main 00000010 00000000 00000008\n" +
		"rom sound 000000ab 00c00000 00000008\n" +
		"hit main 00000010 00000002 1\n" +
		"hit main 00000010 00000004 5\n" +
		"miss sound 000000ab 00c00002 18446744073709551615\n"
	if string(got) != want {
		t.Errorf("profile =\n%s\nwant\n%s", got, want)
	}
	if _, err := os.Stat(path + ".tmp"); !os.IsNotExist(err) {
		t.Error("temporary file left behind")
	}
}

func TestMissIsNotHotAndRomIdentityMustMatch(t *testing.T) {
	rom := []byte{0x4e, 0x71, 0x4e, 0x71, 0x4e, 0x71}
	crc := crc32.ChecksumIEEE(rom)
	p := profile(
		map[RomKey]Bounds{{"sound", crc}: {0xC00000, len(rom)}},
		map[RowKey]uint64{{"sound", crc, 0xC00000}: 2},
		map[RowKey]uint64{{"sound", crc, 0xC00002}: 1},
	)
	path := filepath.Join(t.TempDir(), "profile")
	if err := p.Write(path); err != nil {
		t.Fatal(err)
	}
	hot, err := hotFrom(t, path, "sound", rom, 0xC00000, nil)
	if err != nil {
		t.Fatal(err)
	}
	if _, ok := hot[0xC00000]; !ok || len(hot) != 1 {
		t.Errorf("hot = %v", hot)
	}
	if _, err := hotFrom(t, path, "sound", make([]byte, len(rom)), 0xC00000, nil); err == nil {
		t.Error("accepted a different ROM image")
	}
	if _, err := hotFrom(t, path, "sound", rom, 0, nil); err == nil {
		t.Error("accepted a different base")
	}
}

func TestExclusionsRejectMatchingHitsAndMissesButNotOtherRoms(t *testing.T) {
	rom := []byte{0x4e, 0x71, 0x4e, 0x71, 0x4e, 0x71, 0x4e, 0x71}
	crc := crc32.ChecksumIEEE(rom)
	for _, tt := range []struct {
		cpu, otherCPU string
		base          int
	}{{"main", "sound", 0}, {"sound", "main", 0xC00000}} {
		base, cpu := tt.base, tt.cpu
		excluded := Region{Start: base + 2, End: base + 4, Reason: "synthetic data", Evidence: "fixture"}
		path := filepath.Join(t.TempDir(), "entries.profile")
		otherCRC := crc ^ 1
		identities := map[RomKey]Bounds{
			{cpu, crc}:         {base, len(rom)},
			{cpu, otherCRC}:    {base, len(rom)},
			{tt.otherCPU, crc}: {base, len(rom)},
		}
		hits := map[RowKey]uint64{
			{cpu, crc, base}:             1,
			{cpu, crc, base + 4}:         1,
			{cpu, otherCRC, base + 2}:    1,
			{tt.otherCPU, crc, base + 2}: 1,
		}
		if err := profile(identities, hits, nil).Write(path); err != nil {
			t.Fatal(err)
		}
		hot, err := hotFrom(t, path, cpu, rom, base, Exclusions{excluded})
		if err != nil {
			t.Fatal(err)
		}
		if _, a := hot[base]; len(hot) != 2 || !a {
			t.Errorf("%s hot = %v", cpu, hot)
		}
		if _, b := hot[base+4]; !b {
			t.Errorf("%s hot = %v", cpu, hot)
		}
		for _, kind := range []string{"hit", "miss"} {
			t.Run(cpu+"/"+kind, func(t *testing.T) {
				contradictory := profile(identities, maps.Clone(hits), nil)
				rows := contradictory.Hits
				if kind == "miss" {
					rows = contradictory.Misses
				}
				rows[RowKey{cpu, crc, base + 2}] = 1
				if err := contradictory.Write(path); err != nil {
					t.Fatal(err)
				}
				_, err := hotFrom(t, path, cpu, rom, base, Exclusions{excluded})
				if err == nil {
					t.Fatal("no error")
				}
				for _, detail := range []string{cpu, kind, fmt.Sprintf("%#x", base+2), fmt.Sprintf("[%#x, %#x)", excluded.Start, excluded.End), excluded.Reason} {
					if !strings.Contains(err.Error(), detail) {
						t.Errorf("error %q lacks %q", err, detail)
					}
				}
			})
		}
	}
}

func TestRejectsInvalidAddressesCountsVersionsAndMissingIdentity(t *testing.T) {
	badRows := []string{
		"hit main 00000001 00000001 1",
		"hit main 00000001 00000008 1",
		"hit main 00000002 00000002 1",
		"hit main 00000001 00000002 0",
		"hit main 00000001 00000002 -1",
		"hit main 00000001 00000002 18446744073709551616",
		"hit main 00000001 00000002",
		"hit main 00000001 00000002 1 1",
		"hit other 00000001 00000002 1",
		"hit main 0000001 00000002 1",
		"rom main 00000001 00000000 00000008",
		"rom main 00000003 00000001 00000008",
		"rom main 00000003 00000000 00000000",
	}
	path := filepath.Join(t.TempDir(), "bad.profile")
	for _, row := range badRows {
		t.Run(row, func(t *testing.T) {
			text := "F3-BLOCK-PROFILE 1\nrom main 00000001 00000000 00000008\n" + row + "\n"
			if err := os.WriteFile(path, []byte(text), 0o644); err != nil {
				t.Fatal(err)
			}
			if _, err := ReadProfile(path); err == nil {
				t.Error("accepted")
			}
		})
	}
	for _, text := range []string{"F3-BLOCK-PROFILE 2\n", "", "F3-BLOCK-PROFILE 1\nhit main 00000001 00000002 1\n", "F3-BLOCK-PROFILE 1\n\xc3\xa9\n"} {
		if err := os.WriteFile(path, []byte(text), 0o644); err != nil {
			t.Fatal(err)
		}
		if _, err := ReadProfile(path); err == nil {
			t.Errorf("accepted %q", text)
		}
	}
	if _, err := ReadProfile(filepath.Join(t.TempDir(), "absent")); err == nil {
		t.Error("accepted a missing file")
	}
}

func TestReadAccumulatesDuplicatesAndSkipsBlankLines(t *testing.T) {
	path := filepath.Join(t.TempDir(), "p.profile")
	text := "F3-BLOCK-PROFILE 1\r\nrom main 00000001 00000000 00000008\r\n\r\nhit main 00000001 00000002 4\r\nhit main 00000001 00000002 5\r\n"
	if err := os.WriteFile(path, []byte(text), 0o644); err != nil {
		t.Fatal(err)
	}
	if got := mustRead(t, path).Hits[RowKey{"main", 1, 2}]; got != 9 {
		t.Errorf("count = %d", got)
	}
}

func TestRejectsMergeOfConflictingBounds(t *testing.T) {
	left := profile(map[RomKey]Bounds{{"main", 1}: {0, 8}}, nil, nil)
	if err := left.Merge(profile(map[RomKey]Bounds{{"main", 1}: {0, 10}}, nil, nil)); err == nil {
		t.Error("merged conflicting bounds")
	}
}
