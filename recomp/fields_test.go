package main

import (
	"strings"
	"testing"
)

func fixture() *Fields {
	return NewFields("a", 1, "b", []any{1, 2, NewFields("x", []any{})}, "c", NewFields("k", "v"), "d", []int{}, "e", nil, "f", false)
}

func TestOrderedEncoding(t *testing.T) {
	compact := NewFields("a", 1, "b", []int{1, 2}, "c", NewFields(), "d", []string{}, "e", nil, "f", true)
	tests := []struct {
		name        string
		value       any
		indent      int
		ensureASCII bool
		want        string
	}{
		{"compact", compact, 0, true, `{"a": 1, "b": [1, 2], "c": {}, "d": [], "e": null, "f": true}`},
		{"pretty", compact, 2, true, "{\n  \"a\": 1,\n  \"b\": [\n    1,\n    2\n  ],\n  \"c\": {},\n  \"d\": [],\n  \"e\": null,\n  \"f\": true\n}"},
		{"nested compact", fixture(), 0, true, `{"a": 1, "b": [1, 2, {"x": []}], "c": {"k": "v"}, "d": [], "e": null, "f": false}`},
		{"nested pretty", fixture(), 2, true, "{\n  \"a\": 1,\n  \"b\": [\n    1,\n    2,\n    {\n      \"x\": []\n    }\n  ],\n  \"c\": {\n    \"k\": \"v\"\n  },\n  \"d\": [],\n  \"e\": null,\n  \"f\": false\n}"},
		{"floats", []float64{1.0, 1e-05, 0.1, 1e16, 123456789012345678.0, 1.5e300, 0.0, 2.5, 1e22, 0.0001, 100.0, 3.14159}, 0, true,
			`[1.0, 1e-05, 0.1, 1e+16, 1.2345678901234568e+17, 1.5e+300, 0.0, 2.5, 1e+22, 0.0001, 100.0, 3.14159]`},
		{"string ascii", "é\u00e9\u4e2d😀\n\"\\\t\x01\x7f/", 0, true, `"\u00e9\u00e9\u4e2d\ud83d\ude00\n\"\\\t\u0001\u007f/"`},
		{"string raw", "é\u00e9\u4e2d😀\n\"\\\t\x01\x7f/", 0, false, "\"éé中😀\\n\\\"\\\\\\t\\u0001\x7f/\""},
		{"pretty raw string", []any{"é"}, 2, false, "[\n  \"é\"\n]"},
		{"integers", []any{int8(-3), uint16(7), int64(-1 << 62), uint64(1<<64 - 1)}, 0, true, `[-3, 7, -4611686018427387904, 18446744073709551615]`},
		{"string map sorted", map[string]any{"b": 1, "a": 2}, 0, true, `{"a": 2, "b": 1}`},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			var b strings.Builder
			encodeValue(&b, tt.value, tt.indent, 0, tt.ensureASCII)
			if got := b.String(); got != tt.want {
				t.Errorf("got  %q\nwant %q", got, tt.want)
			}
		})
	}
}

func TestMapOrder(t *testing.T) {
	m := NewFields()
	m.Set("z", 1)
	m.Set("a", 2)
	m.Set("z", 3)
	if got := m.Encode(0); got != `{"z": 3, "a": 2}` {
		t.Errorf("got %s", got)
	}
	if v, ok := m.Get("a"); !ok || v != 2 || m.Len() != 2 {
		t.Errorf("Get/Len wrong: %v %v %d", v, ok, m.Len())
	}
	filtered := fixture().Without("b", "e")
	if got := filtered.Encode(0); got != `{"a": 1, "c": {"k": "v"}, "d": [], "f": false}` {
		t.Errorf("Without got %s", got)
	}
	if got := fixture().Encode(0); !strings.Contains(got, `"b": [1, 2, {"x": []}]`) {
		t.Errorf("Without mutated the receiver: %s", got)
	}
}
