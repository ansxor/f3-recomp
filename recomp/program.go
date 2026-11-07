package main

import (
	"errors"
	"fmt"
	"regexp"
	"slices"
	"strings"
)

const RuntimeABIVersion = 6

const indirectCaseCap = 256

// CodegenOptions configure emission. Zero values are invalid; use DefaultCodegen.
type CodegenOptions struct {
	MaxBlockInstructions int
	BlocksPerFile        int
	ProfileTiers         string
	ProfileSlim          string
}

func DefaultCodegen() CodegenOptions {
	return CodegenOptions{MaxBlockInstructions: 32, BlocksPerFile: 128}
}

var memoryAccess = regexp.MustCompile(`\bf3_(read|write)(8|16|32)\(`)

func inlineMemory(statement string) string {
	return memoryAccess.ReplaceAllStringFunc(statement, func(match string) string {
		sub := memoryAccess.FindStringSubmatch(match)
		op := "st"
		if sub[1] == "read" {
			op = "ld"
		}
		return "f3_" + op + sub[2] + "("
	})
}

func countFlagStores(statements []string) (lazy, eagerX int) {
	for _, s := range statements {
		if strings.Contains(s, "cpu->cc_op = F3_CC_OP_") && !strings.Contains(s, "F3_CC_OP_NONE") {
			lazy++
		}
		if strings.Contains(s, "cpu->sr = (cpu->sr & ~0x10u) |") {
			eagerX++
		}
	}
	return lazy, eagerX
}

type Tier int

const (
	TierHot Tier = iota
	TierCold
)

func (t Tier) String() string {
	switch t {
	case TierHot:
		return "hot"
	case TierCold:
		return "cold"
	}
	return fmt.Sprintf("tier(%d)", int(t))
}

type Block struct {
	Tier Tier
	Name string
	PCs  []int
}

type TableEntry struct {
	PC   int
	Name string
}

type ExceptionEntry struct {
	PC     int
	Vector int
	Tier   Tier
}

type unitHook struct {
	id   int
	kind string
}

type successor struct {
	target     int
	dispatched bool
}

type Stats struct {
	Supported, Unsupported map[string]int
	UnsupportedPCs         []int
	UnitHooks              map[unitHook]int
	LazyProduced           int
	LazyEmitted            int
	EagerProduced          int
	EagerEmitted           int
	IndirectSites          map[Tier]int
	IndirectCases          map[Tier]int
	IndirectCapped         []any
	Functions              int
	OriginalEntries        int
	HotEntries             int
}

type Program struct {
	Analysis   *Analysis
	Image      []byte
	Manifest   Manifest
	Options    CodegenOptions
	Blocks     []Block
	Table      []TableEntry
	Exceptions []ExceptionEntry
	Units      *Units
	Guard      string
	Live       *Liveness
	Idle       IdleLoops
	Fallback   map[int]bool
	Unflagged  map[int][]string
	Stats      Stats
}

func (a *Analysis) exhaustive() bool {
	coverage, _ := a.Game.Manifest.Section("discovery").String("coverage")
	return coverage == "all_aligned"
}

func (a *Analysis) Generate(opts CodegenOptions) (*Program, error) {
	if opts.ProfileTiers != "" && opts.ProfileSlim != "" {
		return nil, errors.New("profile tiers and profile slim are mutually exclusive")
	}
	maxBlockInstructions, blocksPerFile := opts.MaxBlockInstructions, opts.BlocksPerFile
	if maxBlockInstructions < 1 || blocksPerFile < 1 {
		return nil, errors.New("block and shard sizes must be positive")
	}
	image, exclusions := a.Game.Image, a.Exclusions
	for _, check := range []struct {
		kind string
		pcs  []int
	}{{"instruction start", sortedKeys(a.Instructions)}, {"invalid PC", a.InvalidPCs}} {
		for _, pc := range check.pcs {
			if region := exclusions.At(pc); region != nil {
				return nil, fmt.Errorf("Discovery contains an excluded %s: %#x in [%#x, %#x): %s",
					check.kind, pc, region.Start, region.End, region.Reason)
			}
		}
	}
	exhaustive := a.exhaustive()
	blocks := packBlocks(a, exhaustive, maxBlockInstructions)

	romExceptions := map[int]int{}
	if exhaustive {
		for _, pc := range a.InvalidPCs {
			if vector, ok := romExceptionVector(image, pc); ok {
				romExceptions[pc] = vector
			}
		}
	}
	entrySet := map[int]bool{}
	for pc := range a.Instructions {
		entrySet[pc] = true
	}
	for pc := range romExceptions {
		entrySet[pc] = true
	}
	originalEntries := sortedKeys(entrySet)

	profilePath := opts.ProfileTiers
	if opts.ProfileSlim != "" {
		profilePath = opts.ProfileSlim
	}
	hotSet := map[int]bool{}
	if profilePath != "" {
		profile, err := ReadProfile(profilePath)
		if err != nil {
			return nil, err
		}
		profiled, err := profile.Hot("main", image, 0, exclusions)
		if err != nil {
			return nil, err
		}
		for pc := range profiled {
			if entrySet[pc] {
				hotSet[pc] = true
			}
		}
		if len(hotSet) == 0 {
			return nil, errors.New("profile contains no matching main executable hits")
		}
	}
	retained := entrySet
	if opts.ProfileSlim != "" {
		retained = hotSet
	}

	units, err := ParseUnits(a.Game.Manifest)
	if err != nil {
		return nil, err
	}
	unitEnter, unitExit := units.HookPCs()
	for _, unit := range units.Entries {
		for _, group := range []struct {
			kind string
			pcs  []int
		}{{"start", unit.Starts}, {"end", unit.Ends}} {
			for _, pc := range group.pcs {
				region := exclusions.At(pc)
				if _, decoded := a.Instructions[pc]; !decoded || region != nil {
					suffix := ""
					if region != nil {
						suffix = fmt.Sprintf(" (excluded: %s)", region.Reason)
					}
					return nil, fmt.Errorf("Emit unit %s %s %#x is not a decoded instruction%s",
						Format(unit.Name), group.kind, pc, suffix)
				}
				if !retained[pc] {
					return nil, fmt.Errorf("Emit unit %s %s %#x is not retained by the slim profile; add it to the profile or use tiers",
						Format(unit.Name), group.kind, pc)
				}
			}
		}
	}
	for _, writer := range units.FrameWriters {
		if writer[1] >= len(image) {
			return nil, fmt.Errorf("Frame writer range [%#x, %#x] lies outside the ROM", writer[0], writer[1])
		}
	}

	var plan []Block
	for _, pcs := range blocks {
		if profilePath == "" {
			plan = append(plan, Block{TierHot, "", pcs})
			continue
		}
		var hotPCs, coldPCs []int
		for _, pc := range pcs {
			if hotSet[pc] {
				hotPCs = append(hotPCs, pc)
			} else {
				coldPCs = append(coldPCs, pc)
			}
		}
		if len(hotPCs) > 0 {
			plan = append(plan, Block{TierHot, "", hotPCs})
		}
		if len(coldPCs) > 0 && opts.ProfileSlim == "" {
			plan = append(plan, Block{TierCold, "", coldPCs})
		}
	}

	var table []TableEntry
	pcToName := map[int]string{}
	for i := range plan {
		block := &plan[i]
		block.Name = fmt.Sprintf("f3_native_%06x", block.PCs[0])
		for _, pc := range block.PCs {
			table = append(table, TableEntry{PC: pc, Name: block.Name})
			pcToName[pc] = block.Name
		}
	}

	var lazyProduced, eagerProduced int
	fallback := map[int]bool{}
	unflagged := map[int][]string{}
	for _, pc := range sortedKeys(pcToName) {
		statements, ok := a.Instructions[pc].Lower(CCRAll)
		if !ok {
			fallback[pc] = true
			continue
		}
		lazy, eagerX := countFlagStores(statements)
		lazyProduced += lazy
		eagerProduced += eagerX
		if lazy == 0 && eagerX == 0 {
			unflagged[pc] = statements
		}
	}
	pinned := map[int]bool{}
	for pc := range fallback {
		pinned[pc] = true
	}
	for pc := range unitEnter {
		pinned[pc] = true
	}
	for pc := range unitExit {
		pinned[pc] = true
	}
	live := a.AnalyseLiveness(sortedKeys(pcToName), sortedKeys(pinned))
	idleLoops := a.FindIdleLoops(func(pc int) bool {
		_, native := pcToName[pc]
		return native && !pinned[pc]
	})

	var exceptions []ExceptionEntry
	for _, pc := range sortedKeys(romExceptions) {
		if retained[pc] {
			exceptions = append(exceptions, ExceptionEntry{PC: pc, Vector: romExceptions[pc], Tier: TierCold})
		}
	}
	for i := range exceptions {
		if opts.ProfileTiers != "" && hotSet[exceptions[i].PC] {
			exceptions[i].Tier = TierHot
		}
	}

	return &Program{
		Analysis:   a,
		Image:      image,
		Manifest:   a.Game.Manifest,
		Options:    opts,
		Blocks:     plan,
		Table:      table,
		Exceptions: exceptions,
		Units:      units,
		Guard:      units.GuardHookLines(),
		Live:       live,
		Idle:       idleLoops,
		Fallback:   fallback,
		Unflagged:  unflagged,
		Stats: Stats{
			Supported:       map[string]int{},
			Unsupported:     map[string]int{},
			UnitHooks:       map[unitHook]int{},
			LazyProduced:    lazyProduced,
			EagerProduced:   eagerProduced,
			IndirectSites:   map[Tier]int{},
			IndirectCases:   map[Tier]int{},
			OriginalEntries: len(originalEntries),
			HotEntries:      len(hotSet),
		},
	}, nil
}

func packBlocks(a *Analysis, exhaustive bool, maxBlockInstructions int) [][]int {
	var blocks [][]int
	if exhaustive {
		pageBytes := maxBlockInstructions * 2
		last := -1
		for _, pc := range sortedKeys(a.Instructions) {
			if page := pc / pageBytes; page != last {
				blocks = append(blocks, nil)
				last = page
			}
			blocks[len(blocks)-1] = append(blocks[len(blocks)-1], pc)
		}
		return blocks
	}
	seen := map[int]bool{}
	for _, start := range sortedKeys(a.Blocks) {
		var block []int
		for _, pc := range a.Blocks[start] {
			if seen[pc] {
				if len(block) > 0 {
					blocks = append(blocks, block)
					block = nil
				}
				continue
			}
			if len(block) > 0 && (len(block) >= maxBlockInstructions ||
				block[len(block)-1]+int(a.Instructions[block[len(block)-1]].Size) != pc) {
				blocks = append(blocks, block)
				block = nil
			}
			block = append(block, pc)
			seen[pc] = true
		}
		if len(block) > 0 {
			blocks = append(blocks, block)
		}
	}
	for _, pc := range sortedKeys(a.Instructions) {
		if !seen[pc] {
			blocks = append(blocks, []int{pc})
		}
	}
	slices.SortStableFunc(blocks, func(a, b []int) int { return a[0] - b[0] })
	return blocks
}

func romExceptionVector(image []byte, pc int) (int, bool) {
	opcode := int(image[pc])<<8 | int(image[pc+1])
	switch {
	case opcode>>12 == 10:
		return 10, true
	case opcode>>12 == 15:
		return 11, true
	case opcodeCycles68020[opcode] == 0 && opcode != 0x4E70:
		return 4, true
	}
	return 0, false
}

func baseMnemonic(mnemonic string) string {
	base, _, _ := strings.Cut(mnemonic, ".")
	return strings.ToLower(base)
}

func countMap(m map[string]int) *Fields {
	out := NewFields()
	for _, key := range sortedKeys(m) {
		out.Set(key, m[key])
	}
	return out
}
