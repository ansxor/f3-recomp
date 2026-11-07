package main

import (
	"fmt"
	"iter"
	"slices"
	"strings"
)

// PhysicalAddressMask is the 68EC020 24-bit bus: 0xff020000 aliases 0x020000.
const PhysicalAddressMask = 0x00FFFFFF

type Region struct {
	Start, End int
	Reason     string
	Evidence   string
}

type Exclusions []Region

func ParseAddress(v any) (int, error) {
	if n, ok := pyIntValue(v); ok {
		return n, nil
	}
	if s, ok := v.(string); ok {
		s = strings.TrimSpace(s)
		if strings.HasPrefix(s, "0x") || strings.HasPrefix(s, "0X") {
			return ParseNumber(s, 16)
		}
		return ParseNumber(s, 10)
	}
	return 0, fmt.Errorf("Invalid address value: %s", Format(v))
}

func ParseExclusions(manifest Manifest, cpu string, base int, image []byte) (Exclusions, error) {
	var exclusions Exclusions
	var records []any
	if raw, ok := manifest.Value("exclude"); ok {
		var isList bool
		if records, isList = raw.([]any); !isList {
			return nil, fmt.Errorf("Exclusions must be top-level [[exclude]] records")
		}
	}
	for _, rawRecord := range records {
		record, isTable := manifestValue(rawRecord)
		recordCPU := "main"
		if isTable {
			if v, present := record.Value("cpu"); present {
				s, isString := v.(string)
				if !isString {
					return nil, fmt.Errorf("Invalid exclusion CPU: %s", Format(record))
				}
				recordCPU = s
			}
		}
		if !isTable || (recordCPU != "main" && recordCPU != "sound") {
			return nil, fmt.Errorf("Invalid exclusion CPU: %s", Format(rawRecord))
		}
		if recordCPU != cpu {
			continue
		}
		startValue, hasStart := record.Value("start")
		endValue, hasEnd := record.Value("end")
		if !hasStart || !hasEnd {
			return nil, fmt.Errorf("Each exclusion requires start and exclusive end")
		}
		start, err := ParseAddress(startValue)
		if err != nil {
			return nil, err
		}
		end, err := ParseAddress(endValue)
		if err != nil {
			return nil, err
		}
		if start&1 != 0 || end&1 != 0 || !(base <= start && start < end && end <= base+len(image)) {
			return nil, fmt.Errorf("Invalid %s exclusion interval [%#x, %#x)", cpu, start, end)
		}
		var text [2]string
		for i, key := range [...]string{"reason", "evidence"} {
			s, isString := record.String(key)
			if !isString || strings.TrimSpace(s) == "" {
				return nil, fmt.Errorf("Exclusion [%#x, %#x) requires nonempty %s", start, end, key)
			}
			text[i] = s
		}
		exclusions = append(exclusions, Region{start, end, text[0], text[1]})
		if fill, present := record.Value("fill"); present {
			if err := verifyFill(fill, start, end, base, image, cpu); err != nil {
				return nil, err
			}
		}
	}
	slices.SortStableFunc(exclusions, func(a, b Region) int { return a.Start - b.Start })
	for i := 1; i < len(exclusions); i++ {
		if exclusions[i-1].End > exclusions[i].Start {
			return nil, fmt.Errorf("Overlapping %s exclusions at %#x", cpu, exclusions[i].Start)
		}
	}
	return exclusions, nil
}

func verifyFill(fill any, start, end, base int, image []byte, cpu string) error {
	value, parseErr := 0, error(nil)
	_, isBool := fill.(bool)
	_, isInt := pyIntValue(fill)
	_, isString := fill.(string)
	if isInt || isString {
		value, parseErr = ParseAddress(fill)
		if parseErr != nil {
			return parseErr
		}
	}
	if !(isInt || isString) || isBool || value < 0 || value > 0xFF {
		return fmt.Errorf("%s exclusion [%#x, %#x) fill must be a byte value, got %s", cpu, start, end, Format(fill))
	}
	if image == nil {
		return fmt.Errorf("%s exclusion [%#x, %#x) declares fill but no program image was given", cpu, start, end)
	}
	span := image[min(start-base, len(image)):min(end-base, len(image))]
	if offset := slices.IndexFunc(span, func(b byte) bool { return b != byte(value) }); offset >= 0 {
		return fmt.Errorf("%s exclusion [%#x, %#x) fill %#02x contradicted by byte %#02x at %#x",
			cpu, start, end, value, span[offset], start+offset)
	}
	return nil
}

func (e Exclusions) At(pc int) *Region {
	pc &= PhysicalAddressMask
	for i := range e {
		if pc < e[i].Start {
			break
		}
		if pc < e[i].End {
			return &e[i]
		}
	}
	return nil
}

func (e Exclusions) Ranges(start, end int) iter.Seq2[int, int] {
	return func(yield func(int, int) bool) {
		for _, region := range e {
			if start < region.Start && !yield(start, region.Start) {
				return
			}
			start = region.End
		}
		if start < end {
			yield(start, end)
		}
	}
}

func (e Exclusions) Require(pc int, source string) error {
	if region := e.At(pc); region != nil {
		return fmt.Errorf("%s targets excluded PC %#x in [%#x, %#x): %s; %s",
			source, pc, region.Start, region.End, region.Reason, region.Evidence)
	}
	return nil
}
