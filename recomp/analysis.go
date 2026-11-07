package main

import (
	"errors"
	"fmt"
	"maps"
	"os"
	"path/filepath"
	"slices"
	"strconv"
	"strings"
)

var (
	TerminalMnemonics     = newSet("rts", "rte", "rtr", "rtd", "illegal")
	CallMnemonics         = newSet("bsr", "jsr")
	UncondBranchMnemonics = newSet("bra", "jmp")
	CondBranchMnemonics   = newSet(
		"bcc", "bcs", "beq", "bge", "bgt", "bhi", "ble", "bls", "blt", "bmi", "bne", "bpl", "bvc", "bvs",
		"dbcc", "dbcs", "dbeq", "dbf", "dbge", "dbgt", "dbhi", "dble", "dbls", "dblt", "dbmi", "dbne", "dbpl",
		"dbt", "dbvc", "dbvs", "dbra",
		"fbcc", "fbeq", "fbge", "fbgt", "fble", "fblt", "fbne", "fbnge", "fbnle",
	)
)

func newSet(items ...string) map[string]struct{} {
	set := make(map[string]struct{}, len(items))
	for _, item := range items {
		set[item] = struct{}{}
	}
	return set
}

// Decoder is the instruction-decoding test hook. During the rooted recursive
// walk (never during all-aligned decoding or table scans) it is asked first
// for the instruction at each PC; ok=false falls back to Capstone. It receives
// the 24-byte, zero-padded window at the PC.
type Decoder interface {
	Decode(code []byte, addr uint64) (Insn, bool, error)
}

type Analysis struct {
	Instructions map[int]*Insn
	Blocks       map[int][]int
	Functions    map[int]struct{}
	// Report is ordered exactly like the Python report dict.
	Report                *Fields
	AlignedCandidateCount int
	AlignedDecodedCount   int
	AlignedInvalidCount   int
	InvalidPCs            []int
	BranchTargets         map[int][]int
	IndirectTargets       map[int][]int
	Exclusions            Exclusions
	Game                  Game
}

type DiscoverOptions struct{ Decoder Decoder }

type unresolved struct {
	pc                      int
	mnemonic, opStr, reason string
}

func hexPC(pc int) string { return fmt.Sprintf("0x%06x", pc) }

func (u unresolved) report() *Fields {
	return NewFields("pc", hexPC(u.pc), "mnemonic", u.mnemonic, "op_str", u.opStr, "reason", u.reason)
}

func truthyDefault(v any, present bool, def bool) bool {
	if !present {
		return def
	}
	return truthy(v)
}

func hexList(set map[int]struct{}) []string {
	out := make([]string, 0, len(set))
	for _, pc := range slices.Sorted(maps.Keys(set)) {
		out = append(out, hexPC(pc))
	}
	return out
}

func chunk(rom []byte, pc int, scratch *[24]byte) []byte {
	if pc+24 <= len(rom) {
		return rom[pc : pc+24]
	}
	*scratch = [24]byte{}
	copy(scratch[:], rom[pc:])
	return scratch[:]
}

func validInstruction(insn *Insn, ok bool, pc, romLen int) bool {
	return ok && insn.ID != 0 && !strings.HasPrefix(insn.Mnemonic, "dc") && insn.Size%2 == 0 && pc+int(insn.Size) <= romLen
}

type state struct {
	rom       []byte
	md        *Machine
	scratch   [24]byte
	instrs    map[int]*Insn
	functions map[int]struct{}
	worklist  []int
	head      int
}

func (s *state) enqueue(pc int) { s.worklist = append(s.worklist, pc) }

func (s *state) enqueueNew(pc int) {
	if _, ok := s.instrs[pc]; !ok {
		s.enqueue(pc)
	}
}

func discover(rom []byte, cfg Manifest, decoder Decoder) (*Analysis, error) {
	md, err := OpenMachine(ModeBigEndian | Mode68020)
	if err != nil {
		return nil, err
	}
	defer md.Close()
	if len(rom) < 1024 {
		return nil, errors.New("ROM must contain the complete 68020 vector table.")
	}
	if len(rom)%2 != 0 {
		return nil, fmt.Errorf("ROM size must be word-aligned (even): %d", len(rom))
	}

	discoveryCfg := cfg.Section("discovery")
	coverage := any("recursive")
	if v, ok := discoveryCfg.Value("coverage"); ok {
		coverage = v
	}
	coverageMode, _ := coverage.(string)
	if coverageMode != "recursive" && coverageMode != "all_aligned" {
		return nil, fmt.Errorf("Unknown discovery coverage mode: %s", Format(coverage))
	}
	exhaustive := coverageMode == "all_aligned"
	v, present := discoveryCfg.Value("scan_jump_tables")
	scanJumpTables := truthyDefault(v, present, true)
	v, present = discoveryCfg.Value("scan_task_traps")
	scanTaskTraps := truthyDefault(v, present, true)
	v, present = discoveryCfg.Value("scan_callbacks")
	scanCallbacks := truthyDefault(v, present, true)
	inlineStringHelpers := map[int]struct{}{}
	for _, raw := range discoveryCfg.List("inline_string_helpers") {
		pc, err := ParseAddress(raw)
		if err != nil {
			return nil, err
		}
		inlineStringHelpers[pc] = struct{}{}
	}
	exclusions, err := ParseExclusions(cfg, "main", 0, rom)
	if err != nil {
		return nil, err
	}

	provenSeeds := map[int]struct{}{}
	speculativeSeeds := map[int]struct{}{}

	maps.Copy(provenSeeds, extractVectorSeeds(rom))

	for _, raw := range discoveryCfg.List("entry_points") {
		addr, err := ParseAddress(raw)
		if err != nil {
			return nil, err
		}
		if addr&1 != 0 {
			return nil, fmt.Errorf("Entry point must be word-aligned (even): %#x", addr)
		}
		if 0 <= addr && addr < len(rom) {
			provenSeeds[addr] = struct{}{}
		}
	}

	explicitJumpTables := map[int][]int{}
	for _, rawJT := range discoveryCfg.List("jump_tables") {
		jt, isTable := manifestValue(rawJT)
		if _, hasAddress := jt.Value("address"); !isTable || !hasAddress {
			continue
		}
		address, _ := jt.Value("address")
		addr, err := ParseAddress(address)
		if err != nil {
			return nil, err
		}
		if addr&1 != 0 {
			return nil, fmt.Errorf("Jump table address must be word-aligned (even): %#x", addr)
		}
		var rawTargets []int
		for _, raw := range jt.List("targets") {
			t, err := ParseAddress(raw)
			if err != nil {
				return nil, err
			}
			rawTargets = append(rawTargets, t)
		}
		for _, t := range rawTargets {
			if t&1 != 0 {
				return nil, fmt.Errorf("Jump table target must be word-aligned (even): %#x", t)
			}
		}
		targets := []int{}
		for _, t := range rawTargets {
			if 0 <= t && t < len(rom) {
				targets = append(targets, t)
			}
		}
		if rawTable, ok := jt.Value("table"); ok {
			table, err := ParseAddress(rawTable)
			if err != nil {
				return nil, err
			}
			rawCount, ok := jt.Value("count")
			if !ok {
				return nil, keyError("count")
			}
			count, err := intOf(rawCount)
			if err != nil {
				return nil, err
			}
			if count < 0 || table < 0 || table+count*4 > len(rom) {
				return nil, fmt.Errorf("Jump table outside ROM: %#x, count %d", table, count)
			}
			for p := table; p < table+count*4; p += 4 {
				tgt := int(beUnsigned(rom[p : p+4]))
				if tgt%2 == 0 && 0 <= tgt && tgt < len(rom) {
					targets = append(targets, tgt)
				}
			}
		}
		explicitJumpTables[addr] = targets
		if err := exclusions.Require(addr, "Configured jump-table instruction"); err != nil {
			return nil, err
		}
		for _, target := range targets {
			if err := exclusions.Require(target, fmt.Sprintf("Configured jump table at %#x", addr)); err != nil {
				return nil, err
			}
		}
	}

	if !exhaustive && scanTaskTraps {
		trap1Tasks, err := extractTrap1Tasks(md, rom)
		if err != nil {
			return nil, err
		}
		maps.Copy(speculativeSeeds, trap1Tasks)
	}

	if !exhaustive && scanCallbacks {
		cbSeeds, err := extractLeaMoveCallbacks(md, rom)
		if err != nil {
			return nil, err
		}
		maps.Copy(speculativeSeeds, cbSeeds)
	}
	if !exhaustive {
		scriptSeeds, err := extractScriptCallbacks(rom, discoveryCfg.Section("actor_scripts"))
		if err != nil {
			return nil, err
		}
		maps.Copy(speculativeSeeds, scriptSeeds)
	}

	allSeeds := slices.Sorted(maps.Keys(mergeSets(provenSeeds, speculativeSeeds)))
	for _, seed := range allSeeds {
		if err := exclusions.Require(seed, "Vector/config/callback entry"); err != nil {
			return nil, err
		}
	}

	s := &state{
		rom:       rom,
		md:        md,
		instrs:    map[int]*Insn{},
		functions: map[int]struct{}{},
		worklist:  slices.Clone(allSeeds),
	}
	for _, seed := range allSeeds {
		s.functions[seed] = struct{}{}
	}
	instructions, functions := s.instrs, s.functions
	branchTargets := map[int][]int{}
	indirectTargets := map[int][]int{}
	exhaustiveCandidates := map[int][]int{}
	var unresolvedBranches []unresolved
	var excludedTransfers []*Fields

	activeJumpTables := maps.Clone(explicitJumpTables)

	var invalidPCs []int
	if exhaustive {
		for start, end := range exclusions.Ranges(0, len(rom)) {
			for pc := start; pc < end; pc += 2 {
				insn, ok, err := md.Decode(chunk(rom, pc, &s.scratch), uint64(pc))
				if err != nil {
					return nil, err
				}
				if !validInstruction(&insn, ok, pc, len(rom)) {
					invalidPCs = append(invalidPCs, pc)
					continue
				}
				ins := &insn
				instructions[pc] = ins
				base, _, _ := strings.Cut(ins.Mnemonic, ".")
				if !isTransfer(base) {
					continue
				}
				var target int
				var hasTarget bool
				if len(ins.Operands) > 0 {
					target, hasTarget = resolveTarget(ins, &ins.Operands[len(ins.Operands)-1])
				}
				if hasTarget && 0 <= target && target < len(rom) && target&1 == 0 {
					branchTargets[pc] = []int{target}
					if exclusions.At(target) != nil {
						excludedTransfers = append(excludedTransfers, NewFields(
							"pc", hexPC(pc), "target", hexPC(target), "enforcement", "fatal_at_runtime"))
					} else if inSet(CallMnemonics, base) {
						functions[target] = struct{}{}
					}
				} else {
					if len(ins.Operands) > 0 && computedTransfer(ins) {
						scanned, err := scanComputedTargets(md, rom, ins, &ins.Operands[len(ins.Operands)-1])
						if err != nil {
							return nil, err
						}
						if len(scanned) > 0 {
							exhaustiveCandidates[pc] = scanned
						}
					}
					unresolvedBranches = append(unresolvedBranches, unresolved{pc, ins.Mnemonic, ins.OpStr, "indirect_transfer"})
				}
			}
		}
	}

	for s.head < len(s.worklist) {
		entry := s.worklist[s.head]
		s.head++
		if _, done := instructions[entry]; done || entry < 0 || entry >= len(rom) || entry%2 != 0 {
			continue
		}
		if err := exclusions.Require(entry, "Discovered code entry"); err != nil {
			return nil, err
		}

		curPC := entry
	walk:
		for {
			if _, done := instructions[curPC]; done || curPC < 0 || curPC >= len(rom) || curPC%2 != 0 {
				break
			}
			if err := exclusions.Require(curPC, "Recursive code path"); err != nil {
				return nil, err
			}

			buf := chunk(rom, curPC, &s.scratch)
			var insn Insn
			var ok bool
			if decoder != nil {
				if insn, ok, err = decoder.Decode(buf, uint64(curPC)); err != nil {
					return nil, err
				}
			}
			if !ok {
				if insn, ok, err = md.Decode(buf, uint64(curPC)); err != nil {
					return nil, err
				}
			}
			if !validInstruction(&insn, ok, curPC, len(rom)) {
				unresolvedBranches = append(unresolvedBranches, unresolved{curPC, "invalid", "decode failure", "decode_failure"})
				break
			}

			ins := &insn
			instructions[curPC] = ins
			size := int(ins.Size)
			baseMnem := ins.Base()
			isCall := inSet(CallMnemonics, baseMnem)
			if scanJumpTables && (isCall || inSet(UncondBranchMnemonics, baseMnem)) {
				scanned, err := scanRegisterTable(md, rom, ins)
				if err != nil {
					return nil, err
				}
				if len(scanned) > 0 {
					if _, exists := activeJumpTables[curPC]; !exists {
						activeJumpTables[curPC] = scanned
					}
				}
			}

			if targets, ok := activeJumpTables[curPC]; ok {
				branchTargets[curPC] = targets
				for _, t := range targets {
					if 0 <= t && t < len(rom) && t%2 == 0 {
						if isCall {
							functions[t] = struct{}{}
						}
						s.enqueueNew(t)
					}
				}
				if isCall {
					curPC += size
					continue
				}
				break
			}

			var lastOp *Operand
			if len(ins.Operands) > 0 {
				lastOp = &ins.Operands[len(ins.Operands)-1]
			}
			validTarget := func(t int, has bool) bool { return has && 0 <= t && t < len(rom) && t%2 == 0 }

			switch {
			case isCall:
				fallthroughPC := curPC + size
				var target int
				var hasTarget bool
				if lastOp != nil {
					target, hasTarget = resolveTarget(ins, lastOp)
					if !hasTarget && scanJumpTables {
						var scanned []int
						switch {
						case isPCIIndexMode(lastOp.AddressMode):
							scanned = scanPCIIndexTable(rom, ins, lastOp)
						case isPCMemIMode(lastOp.AddressMode):
							if scanned, err = scanPCMemITable(md, rom, ins); err != nil {
								return nil, err
							}
						}
						if len(scanned) > 0 {
							activeJumpTables[curPC] = scanned
							branchTargets[curPC] = scanned
							for _, t := range scanned {
								if t%2 == 0 {
									functions[t] = struct{}{}
									s.enqueueNew(t)
								}
							}
							curPC = fallthroughPC
							continue
						}
					}
				}

				if validTarget(target, hasTarget) {
					functions[target] = struct{}{}
					branchTargets[curPC] = []int{target}
					s.enqueueNew(target)
				} else {
					unresolvedBranches = append(unresolvedBranches, unresolved{curPC, ins.Mnemonic, ins.OpStr, "indirect_call"})
				}

				if _, helper := inlineStringHelpers[target]; hasTarget && helper {
					ptr := fallthroughPC
					for ptr < len(rom) && rom[ptr] != 0 {
						ptr++
					}
					ptr++
					if ptr%2 != 0 {
						ptr++
					}
					fallthroughPC = ptr
				}
				curPC = fallthroughPC
				continue

			case inSet(CondBranchMnemonics, baseMnem):
				fallthroughPC := curPC + size
				var target int
				var hasTarget bool
				if lastOp != nil {
					target, hasTarget = resolveTarget(ins, lastOp)
				}
				if validTarget(target, hasTarget) {
					branchTargets[curPC] = []int{target}
					s.enqueueNew(target)
				} else {
					unresolvedBranches = append(unresolvedBranches, unresolved{curPC, ins.Mnemonic, ins.OpStr, "indirect_branch"})
				}
				if _, done := instructions[fallthroughPC]; !done && fallthroughPC%2 == 0 {
					s.enqueue(fallthroughPC)
				}
				break walk

			case inSet(UncondBranchMnemonics, baseMnem):
				var target int
				var hasTarget bool
				if lastOp != nil {
					target, hasTarget = resolveTarget(ins, lastOp)
					if !hasTarget && scanJumpTables {
						var scanned []int
						switch {
						case isPCIIndexMode(lastOp.AddressMode):
							scanned = scanPCIIndexTable(rom, ins, lastOp)
						case isPCMemIMode(lastOp.AddressMode):
							if scanned, err = scanPCMemITable(md, rom, ins); err != nil {
								return nil, err
							}
						}
						if len(scanned) > 0 {
							activeJumpTables[curPC] = scanned
							branchTargets[curPC] = scanned
							for _, t := range scanned {
								if t%2 == 0 {
									s.enqueueNew(t)
								}
							}
							break walk
						}
					}
				}
				if validTarget(target, hasTarget) {
					branchTargets[curPC] = []int{target}
					s.enqueueNew(target)
				} else {
					unresolvedBranches = append(unresolvedBranches, unresolved{curPC, ins.Mnemonic, ins.OpStr, "indirect_jump"})
				}
				break walk

			case inSet(TerminalMnemonics, baseMnem):
				break walk

			default:
				curPC += size
			}
		}
	}

	if exhaustive {
		pcs := slices.Sorted(maps.Keys(mergeLists(exhaustiveCandidates, explicitJumpTables)))
		for _, pc := range pcs {
			insn := instructions[pc]
			if insn == nil || !computedTransfer(insn) {
				continue
			}
			scanned := explicitJumpTables[pc]
			if len(scanned) == 0 {
				scanned = exhaustiveCandidates[pc]
			}
			if targets := candidateTargets(rom, instructions, exclusions, scanned); len(targets) > 0 {
				indirectTargets[pc] = targets
			}
		}
	} else {
		for pc, scanned := range branchTargets {
			insn := instructions[pc]
			if insn != nil && computedTransfer(insn) {
				if targets := candidateTargets(rom, instructions, exclusions, scanned); len(targets) > 0 {
					indirectTargets[pc] = targets
				}
			}
		}
	}

	blocks := formBlocks(instructions, branchTargets, allSeeds)

	covered := make([]bool, len(rom))
	totalCodeBytes := 0
	bankInstructions := make([]int, (len(rom)+0xFFFF)/0x10000)
	for pc, insn := range instructions {
		for i := pc; i < min(pc+int(insn.Size), len(rom)); i++ {
			if !covered[i] {
				covered[i] = true
				totalCodeBytes++
			}
		}
		if pc/0x10000 < len(bankInstructions) {
			bankInstructions[pc/0x10000]++
		}
	}
	coverageRatio := float64(totalCodeBytes) / float64(len(rom))

	bankSummary := make([]any, len(bankInstructions))
	for b := range bankInstructions {
		start := b * 0x10000
		end := min(start+0x10000, len(rom))
		codeBytes := 0
		for _, c := range covered[start:end] {
			if c {
				codeBytes++
			}
		}
		classification := "unreached_or_data"
		if bankInstructions[b] > 0 {
			classification = "contains_decoded_code"
		}
		bankSummary[b] = NewFields(
			"bank", b,
			"range", fmt.Sprintf("0x%06x-0x%06x", start, end-1),
			"instruction_count", bankInstructions[b],
			"code_bytes", codeBytes,
			"classification", classification,
		)
	}

	if !exhaustive {
		for _, u := range unresolvedBranches {
			if u.reason == "decode_failure" {
				invalidPCs = append(invalidPCs, u.pc)
			}
		}
	}
	slices.Sort(invalidPCs)
	alignedCandidateCount := 0
	if exhaustive {
		alignedCandidateCount = len(rom) / 2
	}
	alignedDecodedCount := len(instructions)
	alignedInvalidCount := len(invalidPCs)
	excludedCandidateCount := 0
	for _, region := range exclusions {
		excludedCandidateCount += (region.End - region.Start) / 2
	}

	coverageBasis := "Unique bytes reachable from vector/config and heuristic seeds, not a proof that undiscovered bytes are data."
	if exhaustive {
		coverageBasis = "Exhaustive independent decode at every nonexcluded word-aligned ROM offset; " +
			"includes overlapping starts and data, not a reachability classification."
	}

	unresolvedReport := make([]any, len(unresolvedBranches))
	for i, u := range unresolvedBranches {
		unresolvedReport[i] = u.report()
	}
	exclusionReport := make([]any, len(exclusions))
	for i, region := range exclusions {
		exclusionReport[i] = NewFields("start", region.Start, "end", region.End, "reason", region.Reason, "evidence", region.Evidence)
	}
	excludedTransferReport := make([]any, len(excludedTransfers))
	for i, m := range excludedTransfers {
		excludedTransferReport[i] = m
	}
	invalidReport := make([]string, len(invalidPCs))
	for i, pc := range invalidPCs {
		invalidReport[i] = hexPC(pc)
	}

	report := NewFields(
		"coverage_mode", coverageMode,
		"coverage_basis", coverageBasis,
		"summary", NewFields(
			"rom_size_bytes", len(rom),
			"total_instructions", len(instructions),
			"total_blocks", len(blocks),
			"total_functions", len(functions),
			"potential_entries", len(instructions),
			"code_bytes", totalCodeBytes,
			"coverage_pct", roundTo3(coverageRatio*100.0),
			"aligned_candidate_count", alignedCandidateCount,
			"aligned_decoded_count", alignedDecodedCount,
			"aligned_invalid_count", alignedInvalidCount,
			"excluded_candidate_count", excludedCandidateCount,
			"excluded_transfers_count", len(excludedTransfers),
			"invalid_pcs_count", len(invalidPCs),
			"proven_seeds_count", len(provenSeeds),
			"speculative_seeds_count", len(speculativeSeeds),
			"unresolved_branches_count", len(unresolvedBranches),
			"indirect_targets_resolved_count", len(indirectTargets),
		),
		"aligned_candidate_count", alignedCandidateCount,
		"aligned_decoded_count", alignedDecodedCount,
		"aligned_invalid_count", alignedInvalidCount,
		"excluded_candidate_count", excludedCandidateCount,
		"exclusions", exclusionReport,
		"excluded_transfers", excludedTransferReport,
		"invalid_pcs", invalidReport,
		"proven_seeds", hexList(provenSeeds),
		"speculative_seeds", hexList(speculativeSeeds),
		"unresolved_branches", unresolvedReport,
		"bank_summary", bankSummary,
	)

	return &Analysis{
		Instructions:          instructions,
		Blocks:                blocks,
		Functions:             functions,
		Report:                report,
		AlignedCandidateCount: alignedCandidateCount,
		AlignedDecodedCount:   alignedDecodedCount,
		AlignedInvalidCount:   alignedInvalidCount,
		InvalidPCs:            invalidPCs,
		BranchTargets:         branchTargets,
		IndirectTargets:       indirectTargets,
		Exclusions:            exclusions,
		Game:                  Game{Image: rom, Manifest: cfg},
	}, nil
}

func (a *Analysis) PCs() []int { return sortedKeys(a.Instructions) }

func (a *Analysis) entryPCs() []int {
	entries := make(map[int]bool, len(a.Instructions))
	for pc := range a.Instructions {
		entries[pc] = true
	}
	if a.exhaustive() {
		for _, pc := range a.InvalidPCs {
			if _, ok := romExceptionVector(a.Game.Image, pc); ok {
				entries[pc] = true
			}
		}
	}
	return sortedKeys(entries)
}

// WriteCoverage writes the coverage report into dir, keeping the file's mtime
// when its content is unchanged, and returns the report and where it landed.
func (a *Analysis) WriteCoverage(dir string) (Coverage, error) {
	path := filepath.Join(dir, "coverage.json")
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return Coverage{}, err
	}
	if _, err := WriteIfChanged(path, []byte(a.Report.Encode(2)+"\n")); err != nil {
		return Coverage{}, err
	}
	return Coverage{Path: path, Report: a.Report}, nil
}

func formBlocks(instructions map[int]*Insn, branchTargets map[int][]int, seeds []int) map[int][]int {
	allPCs := slices.Sorted(maps.Keys(instructions))
	leaders := map[int]struct{}{}
	for _, pc := range seeds {
		if _, ok := instructions[pc]; ok {
			leaders[pc] = struct{}{}
		}
	}
	for _, pc := range allPCs {
		insn := instructions[pc]
		base := insn.Base()
		nextPC := pc + int(insn.Size)
		_, nextDecoded := instructions[nextPC]

		for _, t := range branchTargets[pc] {
			if _, ok := instructions[t]; ok {
				leaders[t] = struct{}{}
			}
		}
		if (inSet(CondBranchMnemonics, base) || inSet(CallMnemonics, base)) && nextDecoded {
			leaders[nextPC] = struct{}{}
		}
		if (inSet(TerminalMnemonics, base) || inSet(UncondBranchMnemonics, base)) && nextDecoded {
			leaders[nextPC] = struct{}{}
		}
	}

	blocks := map[int][]int{}
	var current []int
	entry, open := 0, false
	for _, pc := range allPCs {
		if _, leader := leaders[pc]; leader || !open {
			if len(current) > 0 && open {
				blocks[entry] = current
				current = nil
			}
			entry, open = pc, true
		}

		current = append(current, pc)
		base := instructions[pc].Base()
		if inSet(TerminalMnemonics, base) || inSet(UncondBranchMnemonics, base) || inSet(CondBranchMnemonics, base) {
			blocks[entry] = current
			current, open = nil, false
		}
	}
	if len(current) > 0 && open {
		blocks[entry] = current
	}
	return blocks
}

func roundTo3(x float64) float64 {
	rounded, _ := strconv.ParseFloat(strconv.FormatFloat(x, 'f', 3, 64), 64)
	return rounded
}

func mergeSets(a, b map[int]struct{}) map[int]struct{} {
	out := maps.Clone(a)
	maps.Copy(out, b)
	return out
}

func mergeLists(a, b map[int][]int) map[int]struct{} {
	out := make(map[int]struct{}, len(a)+len(b))
	for k := range a {
		out[k] = struct{}{}
	}
	for k := range b {
		out[k] = struct{}{}
	}
	return out
}
