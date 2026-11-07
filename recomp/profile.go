package main

import (
	"cmp"
	"fmt"
	"hash/crc32"
	"math"
	"math/bits"
	"os"
	"path/filepath"
	"slices"
	"strconv"
	"strings"
)

const (
	Version  = 1
	MaxCount = math.MaxUint64
)

type RomKey struct {
	Region string
	CRC    uint32
}

type Bounds struct {
	Base, Size int
}

type RowKey struct {
	Region  string
	CRC     uint32
	Address int
}

type Profile struct {
	Roms   map[RomKey]Bounds
	Hits   map[RowKey]uint64
	Misses map[RowKey]uint64
}

func NewProfile() *Profile {
	return &Profile{Roms: map[RomKey]Bounds{}, Hits: map[RowKey]uint64{}, Misses: map[RowKey]uint64{}}
}

func saturatingAdd(a, b uint64) uint64 {
	sum, carry := bits.Add64(a, b, 0)
	if carry != 0 {
		return MaxCount
	}
	return sum
}

// Merge folds other into p, saturating counts; ROM identities must agree on bounds.
func (p *Profile) Merge(other *Profile) error {
	for key, bounds := range other.Roms {
		if existing, ok := p.Roms[key]; ok && existing != bounds {
			return fmt.Errorf("profile ROM bounds disagree for ('%s', %d)", key.Region, key.CRC)
		}
		p.Roms[key] = bounds
	}
	for _, pair := range [...]struct{ destination, source map[RowKey]uint64 }{{p.Hits, other.Hits}, {p.Misses, other.Misses}} {
		for key, count := range pair.source {
			pair.destination[key] = saturatingAdd(pair.destination[key], count)
		}
	}
	return nil
}

func sortedRows(rows map[RowKey]uint64) []RowKey {
	keys := make([]RowKey, 0, len(rows))
	for key := range rows {
		keys = append(keys, key)
	}
	slices.SortFunc(keys, func(a, b RowKey) int {
		return cmp.Or(strings.Compare(a.Region, b.Region), cmp.Compare(a.CRC, b.CRC), cmp.Compare(a.Address, b.Address))
	})
	return keys
}

// Write stores the profile atomically at path, creating parent directories.
func (p *Profile) Write(path string) error {
	lines := []string{fmt.Sprintf("F3-BLOCK-PROFILE %d", Version)}
	romKeys := make([]RomKey, 0, len(p.Roms))
	for key := range p.Roms {
		romKeys = append(romKeys, key)
	}
	slices.SortFunc(romKeys, func(a, b RomKey) int {
		return cmp.Or(strings.Compare(a.Region, b.Region), cmp.Compare(a.CRC, b.CRC))
	})
	for _, key := range romKeys {
		bounds := p.Roms[key]
		lines = append(lines, fmt.Sprintf("rom %s %08x %08x %08x", key.Region, key.CRC, bounds.Base, bounds.Size))
	}
	for _, kind := range [...]struct {
		name string
		rows map[RowKey]uint64
	}{{"hit", p.Hits}, {"miss", p.Misses}} {
		for _, key := range sortedRows(kind.rows) {
			if count := kind.rows[key]; count != 0 {
				lines = append(lines, fmt.Sprintf("%s %s %08x %08x %d", kind.name, key.Region, key.CRC, key.Address, count))
			}
		}
	}
	if err := os.MkdirAll(filepath.Dir(path), 0o777); err != nil {
		return err
	}
	temporary := path + ".tmp"
	if err := os.WriteFile(temporary, []byte(strings.Join(lines, "\n")+"\n"), 0o666); err != nil {
		return err
	}
	return os.Rename(temporary, path)
}

func isHex8(s string) bool {
	return len(s) == 8 && strings.IndexFunc(s, func(r rune) bool { return !strings.ContainsRune("0123456789abcdefABCDEF", r) }) < 0
}

func splitLines(text string) []string {
	text = strings.ReplaceAll(strings.ReplaceAll(text, "\r\n", "\n"), "\r", "\n")
	lines := strings.Split(text, "\n")
	if lines[len(lines)-1] == "" {
		lines = lines[:len(lines)-1]
	}
	return lines
}

type record struct {
	kind, region string
	crc          uint32
	address      int
	count        uint64
}

func parseRecord(profile *Profile, line string) (*record, error) {
	fields := strings.Fields(line)
	if len(fields) < 5 {
		return nil, fmt.Errorf("not enough values to unpack (expected 5, got %d)", len(fields))
	}
	if len(fields) > 5 {
		return nil, fmt.Errorf("too many values to unpack (expected 5)")
	}
	kind, region, crcText, addressText, valueText := fields[0], fields[1], fields[2], fields[3], fields[4]
	if (region != "main" && region != "sound") || (kind != "rom" && kind != "hit" && kind != "miss") {
		return nil, fmt.Errorf("unknown record")
	}
	if !isHex8(crcText) || !isHex8(addressText) {
		return nil, fmt.Errorf("expected eight hex digits")
	}
	crc, _ := strconv.ParseUint(crcText, 16, 32)
	address, _ := strconv.ParseUint(addressText, 16, 32)
	if kind == "rom" {
		if !isHex8(valueText) {
			return nil, fmt.Errorf("expected eight hex size digits")
		}
		size, _ := strconv.ParseUint(valueText, 16, 32)
		if address&1 != 0 || size == 0 || size&1 != 0 || address+size > 1<<32 {
			return nil, fmt.Errorf("invalid ROM bounds")
		}
		key := RomKey{region, uint32(crc)}
		if _, duplicate := profile.Roms[key]; duplicate {
			return nil, fmt.Errorf("duplicate ROM identity")
		}
		profile.Roms[key] = Bounds{int(address), int(size)}
		return nil, nil
	}
	if valueText == "" || strings.IndexFunc(valueText, func(r rune) bool { return r < '0' || r > '9' }) >= 0 {
		return nil, fmt.Errorf("expected decimal count")
	}
	count, err := strconv.ParseUint(valueText, 10, 64)
	if err != nil || count == 0 {
		return nil, fmt.Errorf("count outside uint64")
	}
	return &record{kind, region, uint32(crc), int(address), count}, nil
}

// ReadProfile parses a profile file; malformed records, unknown versions and
// counts for undeclared or out-of-bounds addresses are errors.
func ReadProfile(path string) (*Profile, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	for i, b := range data {
		if b >= 0x80 {
			return nil, fmt.Errorf("'ascii' codec can't decode byte 0x%02x in position %d: ordinal not in range(128)", b, i)
		}
	}
	lines := splitLines(string(data))
	if len(lines) == 0 || lines[0] != fmt.Sprintf("F3-BLOCK-PROFILE %d", Version) {
		return nil, fmt.Errorf("unsupported block profile version: %s", path)
	}
	profile := NewProfile()
	var rows []*record
	for i, line := range lines[1:] {
		if len(strings.Fields(line)) == 0 {
			continue
		}
		row, err := parseRecord(profile, line)
		if err != nil {
			return nil, fmt.Errorf("invalid profile record %s:%d: %w", path, i+2, err)
		}
		if row != nil {
			rows = append(rows, row)
		}
	}
	for _, row := range rows {
		bounds, ok := profile.Roms[RomKey{row.region, row.crc}]
		if !ok {
			return nil, fmt.Errorf("profile record without ROM identity: %s %08x", row.region, row.crc)
		}
		if row.address&1 != 0 || row.address < bounds.Base || row.address >= bounds.Base+bounds.Size {
			return nil, fmt.Errorf("profile address outside aligned ROM: %08x", row.address)
		}
		destination := profile.Hits
		if row.kind == "miss" {
			destination = profile.Misses
		}
		key := RowKey{row.region, row.crc, row.address}
		destination[key] = saturatingAdd(destination[key], row.count)
	}
	return profile, nil
}

// Hot returns the addresses of region's profiled hits. The profile must carry
// the identity (CRC, base, size) of rom, and no hit or miss of that image may
// fall inside an exclusion.
func (p *Profile) Hot(region string, rom []byte, base int, exclusions Exclusions) (map[int]struct{}, error) {
	profile := p
	crc := crc32.ChecksumIEEE(rom)
	if bounds, ok := profile.Roms[RomKey{region, crc}]; !ok || bounds != (Bounds{base, len(rom)}) {
		return nil, fmt.Errorf("profile lacks matching %s ROM CRC %08x, base %08x, size %08x", region, crc, base, len(rom))
	}
	for _, kind := range [...]struct {
		name string
		rows map[RowKey]uint64
	}{{"hit", profile.Hits}, {"miss", profile.Misses}} {
		for _, key := range sortedRows(kind.rows) {
			if key.Region != region || key.CRC != crc {
				continue
			}
			for _, excluded := range exclusions {
				if excluded.Start <= key.Address && key.Address < excluded.End {
					return nil, fmt.Errorf("profile %s targets excluded %s PC %#x in [%#x, %#x): %s",
						kind.name, key.Region, key.Address, excluded.Start, excluded.End, excluded.Reason)
				}
			}
		}
	}
	hot := map[int]struct{}{}
	for key, count := range profile.Hits {
		if key.Region == region && key.CRC == crc && count != 0 {
			hot[key.Address] = struct{}{}
		}
	}
	return hot, nil
}
