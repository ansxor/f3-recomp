package main

import (
	"fmt"
	"hash/crc32"
	"os"
	"path/filepath"
	"regexp"
	"slices"
	"strings"
)

var Registers = []string{"d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7", "a0", "a1", "a2", "a3", "a4", "a5", "a6"}

var Owners = []string{"unit", "writer"}

var unitKeys = []string{"name", "start", "end", "unit", "size", "owner"}

var identifier = regexp.MustCompile(`^[A-Za-z_][A-Za-z0-9_]*\z`)

var reserved = map[string]bool{}

func init() {
	for _, name := range strings.Fields(`all frame_writers digest detail alignas alignof asm auto bool break case
		catch char class concept const constexpr continue default delete do double else enum explicit export extern
		false float for friend goto if inline int long namespace new noexcept nullptr operator private protected
		public register requires return short signed sizeof static switch template this throw true try typedef
		typename union unsigned using virtual void volatile while`) {
		reserved[name] = true
	}
}

const maxPC = 1 << 24

type EmitUnit struct {
	ID     int
	Name   string
	Starts []int
	Ends   []int
	Reg    string
	Size   int
	Owner  string
}

type Units struct {
	Entries      []EmitUnit
	FrameWriters [][2]int
}

func (u *Units) HookPCs() (enter, exit map[int][]int) {
	enter, exit = map[int][]int{}, map[int][]int{}
	for _, unit := range u.Entries {
		for _, pc := range unit.Starts {
			enter[pc] = append(enter[pc], unit.ID)
		}
		for _, pc := range unit.Ends {
			exit[pc] = append(exit[pc], unit.ID)
		}
	}
	return enter, exit
}

func pc(value any, what string) (int, error) {
	n, ok := intValue(value)
	if !ok {
		return 0, fmt.Errorf("%s must be an integer, got %s", what, Format(value))
	}
	if n < 0 || n >= maxPC {
		return 0, fmt.Errorf("%s %#x outside the 24-bit address space", what, n)
	}
	if n&1 != 0 {
		return 0, fmt.Errorf("%s %#x must be even", what, n)
	}
	return n, nil
}

func pcList(value any, what string) ([]int, error) {
	items, isList := asList(value)
	if !isList {
		items = []any{value}
	}
	if len(items) == 0 {
		return nil, fmt.Errorf("%s must not be empty", what)
	}
	pcs := make([]int, len(items))
	for i, item := range items {
		var err error
		if pcs[i], err = pc(item, what); err != nil {
			return nil, err
		}
	}
	return pcs, nil
}

func hasDuplicates(values []int) bool {
	seen := map[int]bool{}
	for _, v := range values {
		if seen[v] {
			return true
		}
		seen[v] = true
	}
	return false
}

func tableList(video Manifest, key string, absent []any) ([]any, bool) {
	raw, present := video.Value(key)
	if !present {
		return absent, true
	}
	return asList(raw)
}

func ParseUnits(m Manifest) (*Units, error) {
	video := m.Section("video")
	rawUnits, ok := tableList(video, "emit_units", nil)
	if !ok {
		return nil, fmt.Errorf("video.emit_units must be an array of tables")
	}
	spec := &Units{}
	names := map[string]bool{}
	for index, rawUnit := range rawUnits {
		unit, err := parseUnit(rawUnit, index, len(spec.Entries), names)
		if err != nil {
			return nil, err
		}
		spec.Entries = append(spec.Entries, unit)
	}

	writers := Manifest{}
	if raw, present := video.Value("frame_writers"); present {
		if writers, ok = manifestValue(raw); !ok {
			return nil, fmt.Errorf("video.frame_writers must be a table with only 'ranges'")
		}
	}
	for _, key := range writers.SortedKeys() {
		if key != "ranges" {
			return nil, fmt.Errorf("video.frame_writers must be a table with only 'ranges'")
		}
	}
	ranges, ok := tableList(writers, "ranges", nil)
	if !ok {
		return nil, fmt.Errorf("video.frame_writers.ranges must be an array")
	}
	for index, rawRange := range ranges {
		pair, isList := asList(rawRange)
		var first, last int
		var firstOK, lastOK bool
		if isList && len(pair) == 2 {
			first, firstOK = intValue(pair[0])
			last, lastOK = intValue(pair[1])
		}
		if !firstOK || !lastOK {
			return nil, fmt.Errorf("video.frame_writers.ranges[%d] must be [first, last]", index)
		}
		if _, err := pc(first, fmt.Sprintf("frame writer range %d first", index)); err != nil {
			return nil, err
		}
		if last < first || last >= maxPC {
			return nil, fmt.Errorf("frame writer range %d: invalid [%#x, %#x]", index, first, last)
		}
		spec.FrameWriters = append(spec.FrameWriters, [2]int{first, last})
	}
	return spec, nil
}

func parseUnit(rawUnit any, index, id int, names map[string]bool) (EmitUnit, error) {
	raw, ok := manifestValue(rawUnit)
	if !ok {
		return EmitUnit{}, fmt.Errorf("video.emit_units[%d] must be a table", index)
	}
	var unknown []any
	for _, key := range raw.SortedKeys() {
		if !slices.Contains(unitKeys, key) {
			unknown = append(unknown, key)
		}
	}
	if len(unknown) > 0 {
		slices.SortFunc(unknown, func(a, b any) int { return strings.Compare(a.(string), b.(string)) })
		return EmitUnit{}, fmt.Errorf("video.emit_units[%d]: unknown keys %s", index, Format(unknown))
	}
	for _, key := range unitKeys[:4] {
		if _, present := raw.Value(key); !present {
			return EmitUnit{}, fmt.Errorf("video.emit_units[%d]: missing '%s'", index, key)
		}
	}
	name, isString := raw.String("name")
	if !isString || !identifier.MatchString(name) || reserved[name] {
		return EmitUnit{}, fmt.Errorf("video.emit_units[%d]: invalid unit name %s", index, Format(name))
	}
	if names[name] {
		return EmitUnit{}, fmt.Errorf("duplicate emit unit name %s", Format(name))
	}
	names[name] = true
	where := "emit unit " + Format(name)
	startValue, _ := raw.Value("start")
	starts, err := pcList(startValue, where+" start")
	if err != nil {
		return EmitUnit{}, err
	}
	endValue, _ := raw.Value("end")
	ends, err := pcList(endValue, where+" end")
	if err != nil {
		return EmitUnit{}, err
	}
	if len(starts) != len(ends) {
		return EmitUnit{}, fmt.Errorf("%s: start has %d entries, end has %d", where, len(starts), len(ends))
	}
	if hasDuplicates(starts) || hasDuplicates(ends) {
		return EmitUnit{}, fmt.Errorf("%s: duplicate start or end PC", where)
	}
	for i := range starts {
		if starts[i] >= ends[i] {
			return EmitUnit{}, fmt.Errorf("%s: start %#x must be below end %#x", where, starts[i], ends[i])
		}
	}
	reg, _ := raw.String("unit")
	if !slices.Contains(Registers, reg) {
		return EmitUnit{}, fmt.Errorf("%s: unit register %s must be one of d0-d7/a0-a6", where, Format(reg))
	}
	size := 0
	if rawSize, present := raw.Value("size"); present {
		if size, ok = intValue(rawSize); !ok || size < 0 {
			return EmitUnit{}, fmt.Errorf("%s: size must be a non-negative integer", where)
		}
	}
	owner := "unit"
	if rawOwner, present := raw.Value("owner"); present {
		owner, _ = rawOwner.(string)
		if !slices.Contains(Owners, owner) {
			return EmitUnit{}, fmt.Errorf("%s: owner must be 'unit' or 'writer', got %s", where, Format(rawOwner))
		}
	}
	return EmitUnit{id, name, starts, ends, reg, size, owner}, nil
}

func hexJoin(values []int) string {
	parts := make([]string, len(values))
	for i, v := range values {
		parts[i] = fmt.Sprintf("%x", v)
	}
	return strings.Join(parts, ",")
}

// Canonical is the textual form hashed by Digest; changing it invalidates every generated tree.
func (u *Units) Canonical() string {
	units := make([]string, len(u.Entries))
	for i, unit := range u.Entries {
		units[i] = fmt.Sprintf("%s|%s|%s|%s|%d|%s", unit.Name, hexJoin(unit.Starts), hexJoin(unit.Ends), unit.Reg, unit.Size, unit.Owner)
	}
	writers := make([]string, len(u.FrameWriters))
	for i, w := range u.FrameWriters {
		writers[i] = fmt.Sprintf("%x-%x", w[0], w[1])
	}
	return fmt.Sprintf("units:%s\nframe_writers:%s\n", strings.Join(units, ";"), strings.Join(writers, ","))
}

func (u *Units) Digest() uint32 {
	return crc32.ChecksumIEEE([]byte(u.Canonical()))
}

func (u *Units) GuardHookLines() string {
	return "#include \"sprite_units.h\"\n" +
		fmt.Sprintf("#if F3_SPRITE_UNITS_DIGEST != 0x%08xu\n", u.Digest()) +
		"#error \"Generated program and sprite_units.h describe different emit units\"\n" +
		"#endif\n"
}

const banner = "/* Generated by recomp/compile/sprite_units.py from config.toml. Do not commit. */\n"

func (u *Units) CHeader() string {
	return banner + "#ifndef F3_SPRITE_UNITS_H\n#define F3_SPRITE_UNITS_H\n" +
		fmt.Sprintf("#define F3_SPRITE_UNITS_DIGEST 0x%08xu\n", u.Digest()) +
		fmt.Sprintf("#define F3_SPRITE_UNIT_COUNT %d\n#endif\n", len(u.Entries))
}

func joinHex(values []int) string {
	parts := make([]string, len(values))
	for i, v := range values {
		parts[i] = fmt.Sprintf("0x%xu", v)
	}
	return strings.Join(parts, ", ")
}

func (u *Units) CPPHeader() string {
	var out strings.Builder
	out.WriteString(banner)
	out.WriteString("#pragma once\n")
	out.WriteString("#include <array>\n#include <cstddef>\n#include <cstdint>\n")
	out.WriteString("#include \"f3rt/emit_unit.hpp\"\n#include \"sprite_units.h\"\n\n")
	out.WriteString("namespace f3rt::sprite_units {\n\n")
	if len(u.Entries) > 0 {
		out.WriteString("namespace detail {\n")
		for _, u := range u.Entries {
			fmt.Fprintf(&out, "inline constexpr std::array<uint32_t, %d> %s_starts{%s};\n", len(u.Starts), u.Name, joinHex(u.Starts))
			fmt.Fprintf(&out, "inline constexpr std::array<uint32_t, %d> %s_ends{%s};\n", len(u.Starts), u.Name, joinHex(u.Ends))
		}
		out.WriteString("} // namespace detail\n\n")
	}
	pointers := make([]string, len(u.Entries))
	for i, u := range u.Entries {
		fmt.Fprintf(&out, "inline constexpr EmitUnit %s{%du, \"%s\", detail::%s_starts, detail::%s_ends, UnitRegister::%s, %du, UnitOwner::%s};\n",
			u.Name, u.ID, u.Name, u.Name, u.Name, strings.ToUpper(u.Reg), u.Size, strings.ToUpper(u.Owner[:1])+u.Owner[1:])
		pointers[i] = "&" + u.Name
	}
	fmt.Fprintf(&out, "\ninline constexpr std::array<const EmitUnit *, %d> all{%s};\n", len(u.Entries), strings.Join(pointers, ", "))
	ranges := make([]string, len(u.FrameWriters))
	for i, r := range u.FrameWriters {
		ranges[i] = fmt.Sprintf("PcRange{0x%xu, 0x%xu}", r[0], r[1])
	}
	fmt.Fprintf(&out, "inline constexpr std::array<PcRange, %d> frame_writers{%s};\n", len(u.FrameWriters), strings.Join(ranges, ", "))
	out.WriteString("inline constexpr uint32_t digest = F3_SPRITE_UNITS_DIGEST;\n\n")
	out.WriteString("constexpr bool valid_units() {\n" +
		"    for (std::size_t i = 0; i < all.size(); ++i) {\n" +
		"        const EmitUnit &unit = *all[i];\n" +
		"        if (unit.id != i || unit.starts.size() == 0 || unit.starts.size() != unit.ends.size())\n" +
		"            return false;\n" +
		"        for (std::size_t j = 0; j < unit.starts.size(); ++j)\n" +
		"            if ((unit.starts[j] & 1u) || (unit.ends[j] & 1u) || unit.starts[j] >= unit.ends[j])\n" +
		"                return false;\n" +
		"    }\n" +
		"    return true;\n" +
		"}\n" +
		"static_assert(valid_units(), \"emit unit ids must equal their index; PCs even, start < end, " +
		"starts/ends of equal arity\");\n" +
		"static_assert(all.size() == F3_SPRITE_UNIT_COUNT, \"unit count mismatch\");\n\n" +
		"} // namespace f3rt::sprite_units\n")
	return out.String()
}

func (u *Units) Write(dir string) error {
	if err := os.MkdirAll(dir, 0o777); err != nil {
		return err
	}
	for name, text := range map[string]string{
		"sprite_units.h":   u.CHeader(),
		"sprite_units.hpp": u.CPPHeader(),
	} {
		if _, err := WriteIfChanged(filepath.Join(dir, name), []byte(text)); err != nil {
			return err
		}
	}
	return nil
}
