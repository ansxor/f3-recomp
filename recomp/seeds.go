package main

import (
	"fmt"
)

func extractScriptCallbacks(rom []byte, spec Manifest) (map[int]struct{}, error) {
	callbacks := map[int]struct{}{}
	if len(spec.Table()) == 0 {
		return callbacks, nil
	}
	operandBytes, ok := spec.Value("operand_bytes")
	if !ok {
		return nil, keyError("operand_bytes")
	}
	lengths, err := intList(operandBytes)
	if err != nil {
		return nil, err
	}
	codePointerOpcodes, ok := spec.Value("code_pointer_opcodes")
	if !ok {
		return nil, keyError("code_pointer_opcodes")
	}
	codeOps := map[int]struct{}{}
	for _, item := range listValue(codePointerOpcodes) {
		n, err := intOf(item)
		if err != nil {
			return nil, err
		}
		codeOps[n] = struct{}{}
	}
	rawField, ok := spec.Value("pointer_field")
	if !ok {
		return nil, keyError("pointer_field")
	}
	field, err := intOf(rawField)
	if err != nil {
		return nil, err
	}
	invalidOperation := func(operation int) bool {
		return operation >= len(lengths) || lengths[operation] < 0
	}
	var roots []int

	for pc := 0; pc < len(rom)-7; pc += 2 {
		word := u16(rom, pc)
		if word&0xF1FF == 0x217C && u16(rom, pc+6) == field {
			roots = append(roots, beUnsigned(window(rom, pc+2, pc+6)))
		}
		tableStore := word&0xF1FF == 0x217B && u16(rom, pc+4) == field
		if word&0xF1FF == 0x203B || word&0xF1FF == 0x207B {
			storePC := pc + 4
			for storePC < pc+8 && u16(rom, storePC) == 0x4E71 {
				storePC += 2
			}
			store := u16(rom, storePC)
			storeBase := 0x2140
			if word&0x0040 != 0 {
				storeBase = 0x2148
			}
			tableStore = store&0xF1F8 == storeBase &&
				store&7 == (word>>9)&7 &&
				u16(rom, storePC+2) == field
		}
		if !tableStore {
			continue
		}
		if u16(rom, pc+2)&0x100 != 0 {
			continue
		}
		table := pc + 2 + beSigned(window(rom, pc+3, pc+4))
		strides := []int{4}
		if raw, present := spec.Value("pointer_table_strides"); present {
			if strides, err = intList(raw); err != nil {
				return nil, err
			}
		}
		for _, stride := range strides {
			for offset := range 64 {
				entry := table + offset*stride
				if entry < 0 || entry+4 > len(rom) {
					break
				}
				target := beUnsigned(rom[entry : entry+4])
				if target < 0x400 || target+2 > len(rom) || target&1 != 0 {
					break
				}
				if invalidOperation(u16(rom, target)) {
					break
				}
				roots = append(roots, target)
			}
		}
	}

	if raw, present := spec.Value("entry_points"); present {
		entryPoints, err := intList(raw)
		if err != nil {
			return nil, err
		}
		roots = append(roots, entryPoints...)
	}
	pointerTables, _ := spec.Value("pointer_tables")
	for _, rawSpec := range listValue(pointerTables) {
		tableSpec, _ := manifestValue(rawSpec)
		rawTable, ok := tableSpec.Value("table")
		if !ok {
			return nil, keyError("table")
		}
		table, err := ParseAddress(rawTable)
		if err != nil {
			return nil, err
		}
		rawCount, ok := tableSpec.Value("count")
		if !ok {
			return nil, keyError("count")
		}
		count, err := intOf(rawCount)
		if err != nil {
			return nil, err
		}
		for entry := table; entry < table+count*4; entry += 4 {
			if entry >= 0 && entry+4 <= len(rom) {
				roots = append(roots, beUnsigned(rom[entry:entry+4]))
			}
		}
	}

	visited := map[int]struct{}{}
	for len(roots) > 0 {
		pc := roots[len(roots)-1]
		roots = roots[:len(roots)-1]
		for 0x400 <= pc && pc < len(rom)-1 && pc&1 == 0 {
			if _, seen := visited[pc]; seen {
				break
			}
			visited[pc] = struct{}{}
			operation := u16(rom, pc)
			if invalidOperation(operation) {
				break
			}
			end := pc + 2 + lengths[operation]
			if end > len(rom) {
				break
			}
			target := beUnsigned(window(rom, pc+2, pc+6))
			if _, isCode := codeOps[operation]; isCode && 0x400 <= target && target < len(rom) && target&1 == 0 {
				callbacks[target] = struct{}{}
			}
			returnOpcode, err := specOpcode(spec, "return_opcode")
			if err != nil {
				return nil, err
			}
			if operation == returnOpcode {
				break
			}
			callOpcode, err := specOpcode(spec, "call_opcode")
			if err != nil {
				return nil, err
			}
			if operation == callOpcode {
				roots = append(roots, target)
			}
			jumpOpcode, err := specOpcode(spec, "jump_opcode")
			if err != nil {
				return nil, err
			}
			if operation == jumpOpcode {
				pc = target
			} else {
				pc = end
			}
		}
	}
	return callbacks, nil
}

func specOpcode(spec Manifest, key string) (int, error) {
	raw, ok := spec.Value(key)
	if !ok {
		return 0, keyError(key)
	}
	if n, ok := pyIntValue(raw); ok {
		return n, nil
	}
	return -1, nil
}

func intList(v any) ([]int, error) {
	list := listValue(v)
	out := make([]int, len(list))
	for i, item := range list {
		n, err := intOf(item)
		if err != nil {
			return nil, err
		}
		out[i] = n
	}
	return out, nil
}

func keyError(key string) error {
	return fmt.Errorf("%s", Format(key))
}
