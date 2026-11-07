package main

import (
	"bytes"
	"slices"
	"strings"
	"testing"
)

func exclusionConfig(fields map[string]any) Manifest {
	r := map[string]any{"start": 0x800, "end": 0x1000, "reason": "r", "evidence": "e"}
	for k, v := range fields {
		r[k] = v
	}
	return ManifestOf(map[string]any{"exclude": []any{r}})
}

func exclusionMustParse(t *testing.T, cfg Manifest, size int, cpu string, base int, image []byte) Exclusions {
	t.Helper()
	if image == nil {
		image = make([]byte, size)
	}
	regions, err := ParseExclusions(cfg, cpu, base, image)
	if err != nil {
		t.Fatal(err)
	}
	return regions
}

func TestAliased24BitBusTargetIsExcluded(t *testing.T) {
	regions := exclusionMustParse(t, exclusionConfig(nil), 0x2000, "main", 0, nil)
	if regions.At(0x800) == nil {
		t.Error("0x800 not excluded")
	}
	if regions.At(0xFF000800) != regions.At(0x800) {
		t.Error("aliased PC resolves to a different region")
	}
	if regions.At(0xFF001000) != nil {
		t.Error("end is exclusive")
	}
	regions = exclusionMustParse(t, exclusionConfig(map[string]any{"start": 0x020000, "end": 0x088000}), 0x200000, "main", 0, nil)
	if regions.At(0xFF020000) == nil {
		t.Error("0xff020000 should alias 0x020000")
	}
}

func TestFillMatchingImagePasses(t *testing.T) {
	image := bytes.Repeat([]byte{0xFF}, 0x2000)
	regions := exclusionMustParse(t, exclusionConfig(map[string]any{"fill": 0xFF}), len(image), "main", 0, image)
	if regions[0].Start != 0x800 || regions[0].End != 0x1000 {
		t.Errorf("region = %+v", regions[0])
	}
}

func TestFillErrors(t *testing.T) {
	image := bytes.Repeat([]byte{0xFF}, 0x2000)
	image[0xA02] = 0
	base := 0xC00000
	sound := exclusionConfig(map[string]any{"start": base + 0x800, "end": base + 0x1000, "cpu": "sound", "fill": 0})
	zeros := make([]byte, 0x2000)
	nonzero := slices.Clone(zeros)
	nonzero[0x900] = 1
	tests := []struct {
		name  string
		cfg   Manifest
		cpu   string
		base  int
		image []byte
		want  string
	}{
		{"contradicted", exclusionConfig(map[string]any{"fill": 0xFF}), "main", 0, image, "fill 0xff contradicted by byte 0x00 at 0xa02"},
		{"sound base contradicted", sound, "sound", base, nonzero, "contradicted"},
		{"too large", exclusionConfig(map[string]any{"fill": 256}), "main", 0, zeros, "fill must be a byte value, got 256"},
		{"negative", exclusionConfig(map[string]any{"fill": -1}), "main", 0, zeros, "fill must be a byte value, got -1"},
		{"bool", exclusionConfig(map[string]any{"fill": true}), "main", 0, zeros, "fill must be a byte value, got True"},
		{"float", exclusionConfig(map[string]any{"fill": 1.5}), "main", 0, zeros, "fill must be a byte value, got 1.5"},
		{"bad string", exclusionConfig(map[string]any{"fill": "zz"}), "main", 0, zeros, "invalid literal for int()"},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			_, err := ParseExclusions(tt.cfg, tt.cpu, tt.base, tt.image)
			if err == nil || !strings.Contains(err.Error(), tt.want) {
				t.Errorf("error = %v, want containing %q", err, tt.want)
			}
		})
	}
	if _, err := ParseExclusions(sound, "sound", base, zeros); err != nil {
		t.Errorf("zero fill rejected: %v", err)
	}
	if _, err := ParseExclusions(exclusionConfig(map[string]any{"fill": "0xff"}), "main", 0, bytes.Repeat([]byte{0xFF}, 0x2000)); err != nil {
		t.Errorf("string fill rejected: %v", err)
	}
	if err := verifyFill(0, base+0x800, base+0x1000, base, nil, "sound"); err == nil || !strings.Contains(err.Error(), "no program image") {
		t.Errorf("fill without an image: error = %v, want containing %q", err, "no program image")
	}
}

func TestInvalidRecords(t *testing.T) {
	tests := []struct {
		name string
		cfg  Manifest
		want string
	}{
		{"odd start", exclusionConfig(map[string]any{"start": 0x801}), "Invalid main exclusion interval [0x801, 0x1000)"},
		{"odd end", exclusionConfig(map[string]any{"end": 0x1001}), "Invalid main exclusion interval"},
		{"past rom", exclusionConfig(map[string]any{"end": 0x4000}), "Invalid main exclusion interval"},
		{"missing evidence", exclusionConfig(map[string]any{"evidence": ""}), "requires nonempty evidence"},
		{"blank reason", exclusionConfig(map[string]any{"reason": "  "}), "requires nonempty reason"},
		{"cpu", exclusionConfig(map[string]any{"cpu": "gpu"}), "Invalid exclusion CPU"},
		{"missing end", ManifestOf(map[string]any{"exclude": []any{map[string]any{"start": 0}}}), "requires start and exclusive end"},
		{"not records", ManifestOf(map[string]any{"exclude": 3}), "top-level [[exclude]] records"},
		{"overlap", ManifestOf(map[string]any{"exclude": []any{
			map[string]any{"start": 0x800, "end": 0x1000, "reason": "r", "evidence": "e"},
			map[string]any{"start": 0x800, "end": 0x1000, "reason": "r", "evidence": "e"},
		}}), "Overlapping main exclusions at 0x800"},
		{"bad address", exclusionConfig(map[string]any{"start": 1.5}), "Invalid address value: 1.5"},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			_, err := ParseExclusions(tt.cfg, "main", 0, make([]byte, 0x2000))
			if err == nil || !strings.Contains(err.Error(), tt.want) {
				t.Errorf("error = %v, want containing %q", err, tt.want)
			}
		})
	}
}

func TestParseSortsAndFiltersByCPU(t *testing.T) {
	cfg := ManifestOf(map[string]any{"exclude": []any{
		map[string]any{"start": "0x1000", "end": "0x1800", "reason": "b", "evidence": "e"},
		map[string]any{"start": 0xC00000, "end": 0xC00100, "cpu": "sound", "reason": "s", "evidence": "e"},
		map[string]any{"start": 0x800, "end": 0x1000, "reason": "a", "evidence": "e"},
	}})
	main := exclusionMustParse(t, cfg, 0x2000, "main", 0, nil)
	if len(main) != 2 || main[0].Reason != "a" || main[1].Reason != "b" {
		t.Errorf("main = %+v", main)
	}
	if sound := exclusionMustParse(t, cfg, 0x100, "sound", 0xC00000, nil); len(sound) != 1 || sound[0].Reason != "s" {
		t.Errorf("sound = %+v", sound)
	}
	if none := exclusionMustParse(t, ManifestOf(map[string]any{}), 0x100, "main", 0, nil); len(none) != 0 {
		t.Errorf("none = %+v", none)
	}
}

func TestParseAddress(t *testing.T) {
	tests := []struct {
		in   any
		want int
		bad  bool
	}{
		{int64(5), 5, false}, {" 0x1F ", 31, false}, {"0X10", 16, false}, {"42", 42, false},
		{true, 1, false}, {"abc", 0, true}, {2.0, 0, true}, {nil, 0, true},
	}
	for _, tt := range tests {
		got, err := ParseAddress(tt.in)
		if (err != nil) != tt.bad || got != tt.want {
			t.Errorf("ParseAddress(%v) = %d, %v", tt.in, got, err)
		}
	}
}

func TestIncludedRanges(t *testing.T) {
	regions := Exclusions{{Start: 0x10, End: 0x20}, {Start: 0x30, End: 0x40}}
	tests := []struct {
		start, end int
		want       [][2]int
	}{
		{0, 0x50, [][2]int{{0, 0x10}, {0x20, 0x30}, {0x40, 0x50}}},
		{0x10, 0x40, [][2]int{{0x20, 0x30}}},
		{0x14, 0x38, [][2]int{{0x20, 0x30}}},
	}
	for _, tt := range tests {
		var got [][2]int
		for a, b := range regions.Ranges(tt.start, tt.end) {
			got = append(got, [2]int{a, b})
		}
		if !slices.Equal(got, tt.want) {
			t.Errorf("IncludedRanges(%#x, %#x) = %v, want %v", tt.start, tt.end, got, tt.want)
		}
	}
	for range regions.Ranges(0, 0x50) {
		break
	}
}

func TestRequireIncluded(t *testing.T) {
	regions := Exclusions{{0x800, 0x1000, "synthetic data", "fixture"}}
	if err := regions.Require(0x1000, "vector"); err != nil {
		t.Error(err)
	}
	err := regions.Require(0x800, "vector")
	want := "vector targets excluded PC 0x800 in [0x800, 0x1000): synthetic data; fixture"
	if err == nil || err.Error() != want {
		t.Errorf("error = %v, want %q", err, want)
	}
}
