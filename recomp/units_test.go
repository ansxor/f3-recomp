package main

import (
	"fmt"
	"path/filepath"
	"reflect"
	"strings"
	"testing"
)

func cfg(units []any, ranges []any) Manifest {
	video := map[string]any{}
	if units != nil {
		video["emit_units"] = units
	}
	if ranges != nil {
		video["frame_writers"] = map[string]any{"ranges": ranges}
	}
	return ManifestOf(map[string]any{"video": video})
}

func unit(overrides ...any) Manifest {
	u := map[string]any{"name": "objects", "start": 0x400, "end": 0x40A, "unit": "a6", "size": 0x80}
	for i := 0; i < len(overrides); i += 2 {
		u[overrides[i].(string)] = overrides[i+1]
	}
	return ManifestOf(u)
}

func units(us ...Manifest) []any {
	list := make([]any, len(us))
	for i, u := range us {
		list[i] = u.Table()
	}
	return list
}

func unitsMustParse(t *testing.T, c Manifest) *Units {
	t.Helper()
	spec, err := ParseUnits(c)
	if err != nil {
		t.Fatal(err)
	}
	return spec
}

func TestDefaultsAndScalarLists(t *testing.T) {
	spec := unitsMustParse(t, cfg(units(unit(), ManifestOf(map[string]any{"name": "q", "start": []any{2, 4}, "end": []any{6, 8}, "unit": "d3", "owner": "writer"})), nil))
	if len(spec.Entries) != 2 || spec.Entries[0].ID != 0 || spec.Entries[1].ID != 1 {
		t.Fatalf("units = %+v", spec.Entries)
	}
	if !reflect.DeepEqual(spec.Entries[0].Starts, []int{0x400}) {
		t.Errorf("starts = %v", spec.Entries[0].Starts)
	}
	if spec.Entries[1].Size != 0 || spec.Entries[1].Owner != "writer" {
		t.Errorf("unit 1 = %+v", spec.Entries[1])
	}
	enter, exit := spec.HookPCs()
	if want := map[int][]int{0x400: {0}, 2: {1}, 4: {1}}; !reflect.DeepEqual(enter, want) {
		t.Errorf("enter = %v", enter)
	}
	if want := map[int][]int{0x40A: {0}, 6: {1}, 8: {1}}; !reflect.DeepEqual(exit, want) {
		t.Errorf("exit = %v", exit)
	}
}

func TestEmptyConfig(t *testing.T) {
	spec := unitsMustParse(t, ManifestOf(map[string]any{}))
	if len(spec.Entries) != 0 || len(spec.FrameWriters) != 0 {
		t.Errorf("spec = %+v", spec)
	}
}

func TestUnitsErrors(t *testing.T) {
	r := func(ranges ...[]any) []any {
		list := make([]any, len(ranges))
		for i, v := range ranges {
			list[i] = v
		}
		return list
	}
	tests := []struct {
		name string
		cfg  Manifest
		want string
	}{
		{"odd start", cfg(units(unit("start", 0x401)), nil), "must be even"},
		{"odd end", cfg(units(unit("end", 0x40B)), nil), "must be even"},
		{"arity", cfg(units(unit("start", []any{0x400, 0x500}, "end", []any{0x40A})), nil), "start has 2 entries, end has 1"},
		{"duplicate name", cfg(units(unit(), unit()), nil), "duplicate emit unit name"},
		{"register a7", cfg(units(unit("unit", "a7")), nil), "unit register"},
		{"register sp", cfg(units(unit("unit", "sp")), nil), "unit register"},
		{"identifier", cfg(units(unit("name", "1bad")), nil), "invalid unit name"},
		{"reserved", cfg(units(unit("name", "all")), nil), "invalid unit name"},
		{"inverted", cfg(units(unit("start", 0x40A, "end", 0x400)), nil), "must be below"},
		{"negative size", cfg(units(unit("size", -1)), nil), "size"},
		{"owner", cfg(units(unit("owner", "both")), nil), "owner"},
		{"unknown key", cfg(units(unit("extra", 1)), nil), "unknown keys ['extra']"},
		{"missing unit", cfg(units(ManifestOf(map[string]any{"name": "x", "start": 2, "end": 4})), nil), "missing 'unit'"},
		{"duplicate pc", cfg(units(unit("start", []any{2, 2}, "end", []any{4, 6})), nil), "duplicate"},
		{"odd range", cfg(nil, r([]any{0x41, 0x50})), "must be even"},
		{"inverted range", cfg(nil, r([]any{0x50, 0x40})), "invalid"},
		{"short range", cfg(nil, r([]any{0x40})), "[first, last]"},
		{"bool pc", cfg(units(unit("start", true)), nil), "must be an integer, got True"},
		{"huge pc", cfg(units(unit("end", 1<<24)), nil), "outside the 24-bit address space"},
		{"empty list", cfg(units(unit("start", []any{})), nil), "must not be empty"},
		{"units not list", ManifestOf(map[string]any{"video": map[string]any{"emit_units": "x"}}), "must be an array of tables"},
		{"unit not table", cfg([]any{3}, nil), "video.emit_units[0] must be a table"},
		{"writers keys", ManifestOf(map[string]any{"video": map[string]any{"frame_writers": map[string]any{"other": 1}}}), "only 'ranges'"},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			_, err := ParseUnits(tt.cfg)
			if err == nil || !strings.Contains(err.Error(), tt.want) {
				t.Errorf("error = %v, want containing %q", err, tt.want)
			}
		})
	}
}

func TestFrameWriterLastMayBeOdd(t *testing.T) {
	spec := unitsMustParse(t, cfg(nil, []any{[]any{0x4400, 0x4421}}))
	if !reflect.DeepEqual(spec.FrameWriters, [][2]int{{0x4400, 0x4421}}) {
		t.Errorf("frame writers = %v", spec.FrameWriters)
	}
}

func TestDigestStableAndSensitive(t *testing.T) {
	ranges := []any{[]any{0x10, 0x20}}
	base := unitsMustParse(t, cfg(units(unit()), ranges))
	if base.Digest() != unitsMustParse(t, cfg(units(unit()), ranges)).Digest() {
		t.Error("digest not stable")
	}
	changed := []Manifest{
		cfg(units(unit("size", 0x40)), ranges),
		cfg(units(unit("end", 0x40C)), ranges),
		cfg(units(unit("unit", "a5")), ranges),
		cfg(units(unit("owner", "writer")), ranges),
		cfg(units(unit("name", "renamed")), ranges),
		cfg(units(unit()), []any{[]any{0x10, 0x22}}),
		cfg(units(unit()), nil),
	}
	for i, c := range changed {
		if base.Digest() == unitsMustParse(t, c).Digest() {
			t.Errorf("change %d did not alter the digest", i)
		}
	}
}

func TestDigestIndependentOfUnrelatedConfig(t *testing.T) {
	a := cfg(units(unit()), nil)
	b := cfg(units(unit()), nil)
	b.Table()["discovery"] = map[string]any{"coverage": "all_aligned"}
	if unitsMustParse(t, a).Digest() != unitsMustParse(t, b).Digest() {
		t.Error("unrelated config changed the digest")
	}
}

func TestCanonicalPinned(t *testing.T) {
	if got := unitsMustParse(t, ManifestOf(map[string]any{})).Canonical(); got != "units:\nframe_writers:\n" {
		t.Errorf("canonical = %q", got)
	}
	spec := unitsMustParse(t, cfg(units(unit()), []any{[]any{0x10, 0x21}}))
	if got, want := spec.Canonical(), "units:objects|400|40a|a6|128|unit\nframe_writers:10-21\n"; got != want {
		t.Errorf("canonical = %q, want %q", got, want)
	}
}

func TestRenderEmpty(t *testing.T) {
	spec := unitsMustParse(t, ManifestOf(map[string]any{}))
	header := spec.CHeader()
	for _, want := range []string{"#define F3_SPRITE_UNIT_COUNT 0\n", fmt.Sprintf("#define F3_SPRITE_UNITS_DIGEST 0x%08xu\n", spec.Digest())} {
		if !strings.Contains(header, want) {
			t.Errorf("C header lacks %q", want)
		}
	}
	text := spec.CPPHeader()
	for _, want := range []string{"std::array<const EmitUnit *, 0> all{}", "std::array<PcRange, 0> frame_writers{}", "namespace f3rt::sprite_units {"} {
		if !strings.Contains(text, want) {
			t.Errorf("C++ header lacks %q", want)
		}
	}
}

func TestRenderUnits(t *testing.T) {
	spec := unitsMustParse(t, cfg(units(unit(),
		ManifestOf(map[string]any{"name": "queue", "start": []any{2, 4}, "end": []any{6, 8}, "unit": "d0", "owner": "writer", "size": 18})),
		[]any{[]any{0x10, 0x21}}))
	if !strings.Contains(spec.CHeader(), "#define F3_SPRITE_UNIT_COUNT 2\n") {
		t.Error("C header unit count")
	}
	text := spec.CPPHeader()
	for _, want := range []string{
		`EmitUnit objects{0u, "objects", detail::objects_starts, detail::objects_ends, UnitRegister::A6, 128u, UnitOwner::Unit}`,
		`EmitUnit queue{1u, "queue", detail::queue_starts, detail::queue_ends, UnitRegister::D0, 18u, UnitOwner::Writer}`,
		"queue_starts{0x2u, 0x4u}",
		"queue_ends{0x6u, 0x8u}",
		"all{&objects, &queue}",
		"PcRange{0x10u, 0x21u}",
		"static_assert(valid_units()",
	} {
		if !strings.Contains(text, want) {
			t.Errorf("C++ header lacks %q", want)
		}
	}
}

func TestGuardLines(t *testing.T) {
	spec := unitsMustParse(t, cfg(units(unit()), nil))
	if !strings.Contains(spec.GuardHookLines(), fmt.Sprintf("0x%08xu", spec.Digest())) {
		t.Error("guard lacks digest")
	}
}

func TestShippedConfigsParse(t *testing.T) {
	paths, err := filepath.Glob("../games/*/config.toml")
	if err != nil || len(paths) == 0 {
		t.Skip("no game configs found")
	}
	names := map[string][]string{}
	for _, path := range paths {
		c, err := LoadManifest(path)
		if err != nil {
			t.Fatal(err)
		}
		spec, err := ParseUnits(c)
		if err != nil {
			t.Fatalf("%s: %v", path, err)
		}
		game := filepath.Base(filepath.Dir(path))
		for _, u := range spec.Entries {
			names[game] = append(names[game], u.Name)
		}
	}
	if want := []string{"objects"}; !reflect.DeepEqual(names["commandw"], want) {
		t.Errorf("commandw = %v", names["commandw"])
	}
	if want := []string{"display_objects", "queue"}; !reflect.DeepEqual(names["landmakrj"], want) {
		t.Errorf("landmakrj = %v", names["landmakrj"])
	}
	if len(names["landmakr"]) != 0 {
		t.Errorf("landmakr = %v", names["landmakr"])
	}
}
