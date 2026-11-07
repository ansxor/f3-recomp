package main

import "testing"

func TestFloatRepr(t *testing.T) {
	tests := map[float64]string{
		1: "1.0", 1e-05: "1e-05", 0.0001: "0.0001", 1e16: "1e+16", 1e15: "1000000000000000.0",
		1.5e300: "1.5e+300", 123.456: "123.456", -2.5: "-2.5", 0.00012: "0.00012", 1.2345e-7: "1.2345e-07",
	}
	for in, want := range tests {
		if got := formatFloat(in); got != want {
			t.Errorf("formatFloat(%v) = %s, want %s", in, got, want)
		}
	}
}

func TestRepr(t *testing.T) {
	tests := []struct {
		in   any
		want string
	}{
		{nil, "None"}, {true, "True"}, {256, "256"}, {int64(-1), "-1"}, {1.5, "1.5"},
		{"it's", `"it's"`}, {"a'b\"c", `'a\'b"c'`}, {"tab\t\x01é", `'tab\t\x01é'`},
		{[]any{"a", int64(1)}, "['a', 1]"}, {[]string{"x"}, "['x']"},
		{map[string]any{"b": true, "a": nil}, "{'a': None, 'b': True}"},
	}
	for _, tt := range tests {
		if got := Format(tt.in); got != tt.want {
			t.Errorf("Format(%v) = %s, want %s", tt.in, got, tt.want)
		}
	}
}

func TestParseInt(t *testing.T) {
	tests := []struct {
		in   string
		base int
		want int
		bad  bool
	}{
		{"0x10", 16, 16, false}, {"0X_1f", 16, 31, false}, {" 12 ", 10, 12, false}, {"-7", 10, -7, false},
		{"1_000", 10, 1000, false}, {"-0x10", 16, -16, false}, {"abcd1234", 16, 0xabcd1234, false},
		{"", 10, 0, true}, {"0x", 16, 0, true}, {"0x1", 10, 0, true}, {"1__0", 10, 0, true},
		{"_1", 10, 0, true}, {"+-1", 10, 0, true}, {"0xzz", 16, 0, true}, {"1 2", 10, 0, true},
	}
	for _, tt := range tests {
		got, err := ParseNumber(tt.in, tt.base)
		if (err != nil) != tt.bad || got != tt.want {
			t.Errorf("ParseNumber(%q, %d) = %d, %v", tt.in, tt.base, got, err)
		}
	}
	if _, err := ParseNumber("0xzz", 16); err == nil || err.Error() != "invalid literal for int() with base 16: '0xzz'" {
		t.Errorf("message: %v", err)
	}
}
