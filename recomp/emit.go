package main

import (
	"errors"
	"fmt"
	"hash/crc32"
	"os"
	"path/filepath"
	"slices"
	"strconv"
	"strings"
)

func (p *Program) Write(dir string) (Emission, error) {
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return Emission{}, err
	}
	analysis, image := p.Analysis, p.Image
	exhaustive := analysis.exhaustive()
	unitEnter, unitExit := p.Units.HookPCs()

	abiGuard := fmt.Sprintf("#if F3RT_ABI_VERSION != %d\n#error \"Generated program and f3rt ABI versions differ\"\n#endif\n", RuntimeABIVersion)
	preamble := "/* Generated from user-supplied ROM. Do not commit. */\n" +
		"#include <f3rt/cpu_abi.h>\n" + abiGuard + "#include <f3rt/block_profile.h>\n" +
		"#include \"recomp/cpu_ops.h\"\n"

	var declarations strings.Builder
	for _, block := range p.Blocks {
		fmt.Fprintf(&declarations, "extern void %s(f3_cpu *cpu);\n", block.Name)
	}

	stats := &p.Stats
	indirectSites, indirectCases := stats.IndirectSites, stats.IndirectCases
	var sourceNames []string
	tierSources := map[Tier][]string{TierHot: nil, TierCold: nil}
	shards := map[Tier][]string{}
	shardCalls := map[Tier]map[string]bool{TierHot: {}, TierCold: {}}
	shardHooked := map[Tier]bool{}
	remaining := map[Tier]int{}
	for _, block := range p.Blocks {
		remaining[block.Tier]++
	}
	blocksPerFile := p.Options.BlocksPerFile
	profile := p.Options.ProfileTiers != "" || p.Options.ProfileSlim != ""
	names := p.blockNames()

	for _, block := range p.Blocks {
		tier, name := block.Tier, block.Name
		lines := []string{fmt.Sprintf("void %s(f3_cpu *cpu) {", name), "    switch (cpu->pc) {"}
		for _, pc := range block.PCs {
			lines = append(lines, fmt.Sprintf("    case 0x%08xu: goto L_%06x;", pc, pc))
		}
		lines = append(lines, "    default: return;", "    }")
		for _, pc := range block.PCs {
			insn := analysis.Instructions[pc]
			lines = append(lines, fmt.Sprintf("L_%06x: {", pc))
			for _, id := range unitExit[pc] {
				lines = append(lines, fmt.Sprintf("    F3_UNIT_EXIT(cpu, %d);", id))
				stats.UnitHooks[unitHook{id, "exit"}]++
				shardHooked[tier] = true
			}
			for _, id := range unitEnter[pc] {
				lines = append(lines, fmt.Sprintf("    F3_UNIT_ENTER(cpu, %d);", id))
				stats.UnitHooks[unitHook{id, "enter"}]++
				shardHooked[tier] = true
			}
			lines = append(lines, fmt.Sprintf("    F3_PROFILE_HIT_MAIN(0x%08xu);", pc))
			if loop, ok := p.Idle[pc]; ok {
				for _, statement := range loop.Statements {
					lines = append(lines, "    "+statement)
				}
			}
			var statements []string
			ok := !p.Fallback[pc]
			if ok {
				statements = p.Unflagged[pc]
				if len(statements) == 0 {
					statements, ok = insn.Lower(p.Live.LiveOut[pc])
				}
			}
			if !ok {
				stats.Unsupported[insn.Mnemonic]++
				stats.UnsupportedPCs = append(stats.UnsupportedPCs, pc)
				lines = append(lines, "    if (!f3_fallback(cpu)) cpu->halted = 1;", "    return;")
				lines = append(lines, "}")
				continue
			}
			stats.Supported[insn.Mnemonic]++
			base := baseMnemonic(insn.Mnemonic)
			if (base == "rts" || base == "rtd") && strings.TrimSpace(statements[len(statements)-1]) == "return;" {
				statements = append(slices.Clone(statements[:len(statements)-1]), "F3_DISPATCH_PC(cpu);", "return;")
			}
			lazy, eagerX := countFlagStores(statements)
			stats.LazyEmitted += lazy
			stats.EagerEmitted += eagerX
			for _, statement := range statements {
				lines = append(lines, "    "+inlineMemory(statement))
			}
			if strings.TrimSpace(statements[len(statements)-1]) != "return;" {
				lines = appendChains(lines, analysis, insn, pc, base, name, tier, names, shardCalls[tier], &chainStats{
					sites: indirectSites, cases: indirectCases, capped: &stats.IndirectCapped,
				})
			}
			lines = append(lines, "}")
		}
		lines = append(lines, "}\n")
		shards[tier] = append(shards[tier], strings.Join(lines, "\n"))
		remaining[tier]--
		if len(shards[tier]) == blocksPerFile || remaining[tier] == 0 {
			var filename string
			if !profile {
				filename = fmt.Sprintf("blocks_%04d.c", len(sourceNames))
			} else {
				filename = fmt.Sprintf("blocks_%s_%04d.c", tier, len(tierSources[tier]))
			}
			var externs strings.Builder
			for _, callee := range sortedKeys(shardCalls[tier]) {
				fmt.Fprintf(&externs, "extern void %s(f3_cpu *cpu);\n", callee)
			}
			guard := ""
			if shardHooked[tier] {
				guard = p.Guard
			}
			text := preamble + guard + externs.String() + strings.Join(shards[tier], "\n")
			if _, err := WriteIfChanged(filepath.Join(dir, filename), []byte(text)); err != nil {
				return Emission{}, err
			}
			sourceNames = append(sourceNames, filename)
			tierSources[tier] = append(tierSources[tier], filename)
			shards[tier] = nil
			shardCalls[tier] = map[string]bool{}
			shardHooked[tier] = false
		}
	}

	for _, unit := range p.Units.Entries {
		for _, group := range []struct {
			kind string
			pcs  []int
		}{{"enter", unit.Starts}, {"exit", unit.Ends}} {
			if emitted := stats.UnitHooks[unitHook{unit.ID, group.kind}]; emitted != len(group.pcs) {
				return Emission{}, fmt.Errorf("Emit unit %s: %d of %d %s hooks emitted",
					Format(unit.Name), emitted, len(group.pcs), group.kind)
			}
		}
	}
	unitsReport := NewFields()
	for _, unit := range p.Units.Entries {
		unitsReport.Set(unit.Name, NewFields(
			"id", unit.ID,
			"enter_hooks", stats.UnitHooks[unitHook{unit.ID, "enter"}],
			"exit_hooks", stats.UnitHooks[unitHook{unit.ID, "exit"}],
		))
	}
	unitReport := NewFields(
		"digest", fmt.Sprintf("0x%08x", p.Units.Digest()),
		"units", unitsReport,
		"frame_writer_ranges", len(p.Units.FrameWriters),
	)

	table := slices.Clone(p.Table)
	exceptionEntries := map[int]int{}
	for _, entry := range p.Exceptions {
		table = append(table, TableEntry{PC: entry.PC, Name: fmt.Sprintf("f3_rom_exception_%d", entry.Vector)})
		exceptionEntries[entry.Vector]++
	}
	slices.SortFunc(table, func(a, b TableEntry) int {
		if a.PC != b.PC {
			return a.PC - b.PC
		}
		return strings.Compare(a.Name, b.Name)
	})
	if len(table) == 0 {
		return Emission{}, errors.New("discovery produced no instructions")
	}
	var program strings.Builder
	program.WriteString(preamble + "#include \"program.h\"\n" + declarations.String())
	exceptionSources := map[Tier][]string{}
	for _, vector := range sortedKeys(exceptionEntries) {
		storage := "static "
		if p.Options.ProfileTiers != "" {
			storage = ""
		}
		definition := fmt.Sprintf("%svoid f3_rom_exception_%d(f3_cpu *cpu) {\n"+
			"    F3_PROFILE_HIT_MAIN(cpu->pc);\n"+
			"    f3_exception(cpu, %d, cpu->pc);\n"+
			"}\n", storage, vector, vector)
		if p.Options.ProfileTiers == "" {
			program.WriteString(definition)
			continue
		}
		fmt.Fprintf(&program, "extern void f3_rom_exception_%d(f3_cpu *cpu);\n", vector)
		tier := TierCold
		if vectorTier(p.Exceptions, vector) == TierHot {
			tier = TierHot
		}
		exceptionSources[tier] = append(exceptionSources[tier], definition)
	}
	for _, tier := range []Tier{TierHot, TierCold} {
		if definitions := exceptionSources[tier]; len(definitions) > 0 {
			filename := fmt.Sprintf("exceptions_%s.c", tier)
			if _, err := WriteIfChanged(filepath.Join(dir, filename), []byte(preamble+strings.Join(definitions, ""))); err != nil {
				return Emission{}, err
			}
			sourceNames = append(sourceNames, filename)
			tierSources[tier] = append(tierSources[tier], filename)
		}
	}
	program.WriteString("static const f3_block translated_blocks[] = {\n")
	for _, entry := range table {
		fmt.Fprintf(&program, "    { 0x%08xu, %s },\n", entry.PC, entry.Name)
	}
	program.WriteString("};\n")
	exclusions := analysis.Exclusions
	if len(exclusions) > 0 {
		program.WriteString("static const f3_excluded_range excluded_ranges[] = {\n")
		for _, region := range exclusions {
			fmt.Fprintf(&program, "    { 0x%08xu, 0x%08xu, %s, %s },\n",
				region.Start, region.End, Quote(region.Reason), Quote(region.Evidence))
		}
		program.WriteString("};\n")
	}
	romCRC := crc32.ChecksumIEEE(image)
	fmt.Fprintf(&program, "f3_block_fn f3_entry_by_word[F3_ENTRY_WORDS];\n"+
		"int f3_generated_register(f3_cpu *cpu) {\n"+
		"    if (!f3_validate_main_rom(cpu, %du, 0x%08xu)) return 0;\n"+
		"    if (!f3_register_blocks(cpu, translated_blocks,\n"+
		"        sizeof(translated_blocks) / sizeof(translated_blocks[0]))) return 0;\n"+
		"    for (size_t i = 0; i < sizeof(translated_blocks) / sizeof(translated_blocks[0]); ++i)\n"+
		"        f3_entry_by_word[translated_blocks[i].address >> 1] = translated_blocks[i].execute;\n",
		len(image), romCRC)
	if len(exclusions) > 0 {
		program.WriteString("    return f3_register_exclusions(cpu, excluded_ranges,\n" +
			"        sizeof(excluded_ranges) / sizeof(excluded_ranges[0]));\n}\n")
	} else {
		program.WriteString("    return f3_register_exclusions(cpu, NULL, 0);\n}\n")
	}
	write := func(name string, data []byte) error {
		_, err := WriteIfChanged(filepath.Join(dir, name), data)
		return err
	}
	if err := write("program.c", []byte(program.String())); err != nil {
		return Emission{}, err
	}
	if err := write("program.h", []byte("#ifndef F3_GENERATED_PROGRAM_H\n#define F3_GENERATED_PROGRAM_H\n"+
		"#include <f3rt/cpu_abi.h>\n"+abiGuard+"#ifdef __cplusplus\nextern \"C\" {\n#endif\n"+
		"int f3_generated_register(f3_cpu *cpu);\n"+
		"#ifdef __cplusplus\n}\n#endif\n#endif\n")); err != nil {
		return Emission{}, err
	}
	sourceNames = append(sourceNames, "program.c")
	tierSources[TierHot] = append(tierSources[TierHot], "program.c")
	var sources strings.Builder
	for _, variable := range []struct {
		name  string
		files []string
	}{
		{"F3_GENERATED_SOURCES", sourceNames},
		{"F3_GENERATED_HOT_SOURCES", tierSources[TierHot]},
		{"F3_GENERATED_COLD_SOURCES", tierSources[TierCold]},
	} {
		fmt.Fprintf(&sources, "set(%s\n", variable.name)
		for _, name := range variable.files {
			fmt.Fprintf(&sources, "    \"${CMAKE_CURRENT_LIST_DIR}/%s\"\n", name)
		}
		sources.WriteString(")\n")
	}
	if err := write("sources.cmake", []byte(sources.String())); err != nil {
		return Emission{}, err
	}
	if err := write("program.bin", image); err != nil {
		return Emission{}, err
	}
	generatedBytes := 0
	for _, name := range sourceNames {
		info, err := os.Stat(filepath.Join(dir, name))
		if err != nil {
			return Emission{}, err
		}
		generatedBytes += int(info.Size())
	}
	entries := analysis.entryPCs()
	inventory := NewFields(
		"version", 1,
		"region", "main",
		"rom_crc32", fmt.Sprintf("%08x", romCRC),
		"base", 0,
		"size", len(image),
		"entries", entries,
		"retained_entries", len(table),
		"hot_entries", stats.HotEntries,
		"cold_entries", len(entries)-stats.HotEntries,
		"generated_c_bytes", generatedBytes,
	)
	if err := write("profile_inventory.json", []byte(inventory.Encode(2)+"\n")); err != nil {
		return Emission{}, err
	}

	idleReport := p.Idle.Report()
	exceptionReport := NewFields()
	for _, vector := range sortedKeys(exceptionEntries) {
		exceptionReport.Set(strconv.Itoa(vector), exceptionEntries[vector])
	}
	exclusionReport := []any{}
	excludedEntries := 0
	for _, region := range exclusions {
		exclusionReport = append(exclusionReport, NewFields(
			"start", region.Start, "end", region.End, "reason", region.Reason, "evidence", region.Evidence))
		excludedEntries += (region.End - region.Start) / 2
	}
	coverageMode := "recursive"
	undecoded := 0
	if exhaustive {
		coverageMode = "all_aligned"
		undecoded = len(analysis.InvalidPCs) - exceptionVectorCount(analysis)
	}
	if stats.IndirectCapped == nil {
		stats.IndirectCapped = []any{}
	}
	report := NewFields(
		"decoded_instructions", len(analysis.Instructions),
		"registered_entries", len(table),
		"native_instructions", sumValues(stats.Supported),
		"exception_entries", exceptionReport,
		"undecoded_valid_primary_entries", undecoded,
		"fallback_instructions", sumValues(stats.Unsupported),
		"native_blocks", len(p.Blocks),
		"native_mnemonics", countMap(stats.Supported),
		"fallback_mnemonics", countMap(stats.Unsupported),
		"fallback_pcs", intsOrEmpty(stats.UnsupportedPCs),
		"source_files", sourceNames,
		"runtime_abi_version", RuntimeABIVersion,
		"emit_units", unitReport,
		"idle_loops", idleReport,
		"flag_liveness", NewFields(
			"lazy_producers", stats.LazyProduced,
			"lazy_producers_emitted", stats.LazyEmitted,
			"eager_x_writes", stats.EagerProduced,
			"eager_x_writes_emitted", stats.EagerEmitted,
		),
		"coverage_mode", coverageMode,
		"exclusions", exclusionReport,
		"excluded_entries", excludedEntries,
		"profile_mode", p.profileMode(),
		"original_entries", stats.OriginalEntries,
		"retained_entries", len(table),
		"hot_entries", stats.HotEntries,
		"cold_entries", stats.OriginalEntries-stats.HotEntries,
		"max_block_instructions", p.Options.MaxBlockInstructions,
		"indirect_chain_sites", indirectSites[TierHot]+indirectSites[TierCold],
		"indirect_chain_cases", indirectCases[TierHot]+indirectCases[TierCold],
		"indirect_chain_hot", NewFields("sites", indirectSites[TierHot], "cases", indirectCases[TierHot]),
		"indirect_chain_cold", NewFields("sites", indirectSites[TierCold], "cases", indirectCases[TierCold]),
		"indirect_chain_capped_sites", stats.IndirectCapped,
		"timing", "68EC020 reference instruction costs; runtime deadlines end native blocks at instruction boundaries",
	)
	if err := write("lowering.json", []byte(report.Encode(2)+"\n")); err != nil {
		return Emission{}, err
	}
	if err := write("coverage.json", []byte(analysis.Report.Encode(2)+"\n")); err != nil {
		return Emission{}, err
	}
	if err := p.sweepStale(dir, sourceNames); err != nil {
		return Emission{}, err
	}
	return Emission{Files: sourceNames, Report: report}, nil
}

func (p *Program) profileMode() string {
	switch {
	case p.Options.ProfileTiers != "":
		return "tiers"
	case p.Options.ProfileSlim != "":
		return "slim"
	}
	return "baseline"
}

func (p *Program) blockNames() map[int]string {
	names := make(map[int]string, len(p.Table))
	for _, entry := range p.Table {
		names[entry.PC] = entry.Name
	}
	return names
}

func vectorTier(exceptions []ExceptionEntry, vector int) Tier {
	for _, entry := range exceptions {
		if entry.Vector == vector && entry.Tier == TierHot {
			return TierHot
		}
	}
	return TierCold
}

func exceptionVectorCount(a *Analysis) int {
	if !a.exhaustive() {
		return 0
	}
	count := 0
	for _, pc := range a.InvalidPCs {
		if _, ok := romExceptionVector(a.Game.Image, pc); ok {
			count++
		}
	}
	return count
}

func (p *Program) sweepStale(dir string, sourceNames []string) error {
	emitted := map[string]bool{
		"program.h": true, "sources.cmake": true, "program.bin": true,
		"profile_inventory.json": true, "lowering.json": true, "coverage.json": true,
	}
	for _, name := range sourceNames {
		emitted[name] = true
	}
	entries, err := os.ReadDir(dir)
	if err != nil {
		return err
	}
	for _, entry := range entries {
		name := entry.Name()
		if emitted[name] || strings.HasPrefix(name, ".") || strings.HasSuffix(name, ".stamp") {
			continue
		}
		path := filepath.Join(dir, name)
		if info, err := os.Stat(path); err == nil && info.Mode().IsRegular() {
			if err := os.Remove(path); err != nil {
				return err
			}
		}
	}
	return nil
}

type chainStats struct {
	sites, cases map[Tier]int
	capped       *[]any
}

func appendChains(lines []string, a *Analysis, insn *Insn, pc int, base, name string,
	tier Tier, pcToName map[int]string, calls map[string]bool, stats *chainStats) []string {
	fallthrough_ := pc + int(insn.Size)
	flow := insn.Flow()
	var successors []successor
	setSuccessor := func(target int, dispatched bool) {
		for i := range successors {
			if successors[i].target == target {
				successors[i].dispatched = dispatched
				return
			}
		}
		successors = append(successors, successor{target, dispatched})
	}
	if flow.FallsThrough {
		setSuccessor(fallthrough_, pcToName[fallthrough_] != name)
	}
	for _, target := range flow.Targets {
		setSuccessor(target, target != fallthrough_ || pcToName[fallthrough_] != name)
	}
	for _, s := range successors {
		owner, ok := pcToName[s.target]
		if !ok || s.target >= 0x200000 {
			continue
		}
		guard := fmt.Sprintf("cpu->pc == 0x%08xu && ", s.target)
		if s.dispatched {
			guard += "!(cpu->sr & 0xc000u) && "
		}
		guard += "cpu->cycles < cpu->dispatch_deadline"
		if owner == name {
			lines = append(lines, fmt.Sprintf("    if (%s) goto L_%06x;", guard, s.target))
		} else {
			calls[owner] = true
			lines = append(lines, fmt.Sprintf("    if (%s) { F3_CHAIN(%s, cpu); }", guard, owner))
		}
	}
	computed := (base == "jmp" || base == "jsr") && len(flow.Targets) == 0
	if computed {
		lines = append(lines, fmt.Sprintf("    F3_PROFILE_INDIRECT_MAIN(0x%08xu, cpu->pc);", pc))
	}
	if candidates := a.IndirectTargets[pc]; len(candidates) > 0 {
		type chainCase struct {
			target int
			owner  string
		}
		var cases []chainCase
		for _, target := range slices.Compact(slices.Sorted(slices.Values(candidates))) {
			owner, ok := pcToName[target]
			if !ok || target >= 0x200000 {
				continue
			}
			cases = append(cases, chainCase{target, owner})
		}
		if len(cases) > indirectCaseCap {
			*stats.capped = append(*stats.capped, NewFields("tier", tier.String(), "pc", fmt.Sprintf("0x%06x", pc), "cases", len(cases)))
			cases = cases[:indirectCaseCap]
		}
		if len(cases) > 0 {
			stats.sites[tier]++
			stats.cases[tier] += len(cases)
			lines = append(lines, "    if (!(cpu->sr & 0xc000u) && cpu->cycles < cpu->dispatch_deadline) {", "        switch (cpu->pc) {")
			for _, c := range cases {
				if c.owner == name {
					lines = append(lines, fmt.Sprintf("        case 0x%08xu: goto L_%06x;", c.target, c.target))
				} else {
					calls[c.owner] = true
					lines = append(lines, fmt.Sprintf("        case 0x%08xu: F3_CHAIN(%s, cpu);", c.target, c.owner))
				}
			}
			lines = append(lines, "        default: break;", "        }", "    }")
		}
	}
	if computed {
		lines = append(lines, "    F3_DISPATCH_PC(cpu);")
	}
	return append(lines, "    return;")
}
