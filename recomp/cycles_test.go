package main

import "testing"

func TestBaseCycles(t *testing.T) {
	tests := []struct {
		opcode int
		want   uint8
	}{
		{0x4E71, 2},
		{0x4E75, 10},
		{0x6000, 10},
		{0x6600, 6},
		{0x4E73, 20},
	}
	for _, tc := range tests {
		if got := opcodeCycles68020[tc.opcode]; got != tc.want {
			t.Errorf("opcodeCycles68020[%#04x] = %d, want %d", tc.opcode, got, tc.want)
		}
	}
}

func TestFullIndexCycles(t *testing.T) {
	tests := []struct{ ext, want int }{{0, 0}, {16, 0}, {17, 5}, {19, 7}, {32, 2}, {33, 7}, {36, 0}, {37, 7}, {48, 6}, {49, 11}, {52, 0}, {63, 13}}
	for _, tc := range tests {
		if got := fullIndexCycles[tc.ext]; got != tc.want {
			t.Errorf("fullIndexCycles[%d] = %d, want %d", tc.ext, got, tc.want)
		}
	}
}
