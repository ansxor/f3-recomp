package main

import (
	"os"
	"path/filepath"
	"reflect"
	"testing"
)

const sample = `
name = "x"
count = 3
flag = true
ratio = 1.5

[video]
rotation = 90

[[exclude]]
start = 0x800
reason = "r"
`

func TestParseAndAccessors(t *testing.T) {
	path := filepath.Join(t.TempDir(), "c.toml")
	if err := os.WriteFile(path, []byte(sample), 0o644); err != nil {
		t.Fatal(err)
	}
	cfg, err := LoadManifest(path)
	if err != nil {
		t.Fatal(err)
	}
	if s, ok := cfg.String("name"); !ok || s != "x" {
		t.Errorf("Str = %q, %v", s, ok)
	}
	if _, ok := cfg.String("count"); ok {
		t.Error("Str accepted an integer")
	}
	if n, ok := cfg.Int("count"); !ok || n != 3 {
		t.Errorf("Int = %d, %v", n, ok)
	}
	if _, ok := cfg.Int("flag"); ok {
		t.Error("Int accepted a bool")
	}
	if _, ok := cfg.Int("ratio"); ok {
		t.Error("Int accepted a float")
	}
	if b, ok := cfg.Bool("flag"); !ok || !b {
		t.Errorf("Bool = %v, %v", b, ok)
	}
	if n, _ := cfg.Section("video").Int("rotation"); n != 90 {
		t.Errorf("rotation = %d", n)
	}
	if got := cfg.Section("missing"); len(got.Table()) != 0 {
		t.Errorf("Sub(missing) = %v", got)
	}
	list := listValue(cfg.Table()["exclude"])
	if len(list) != 1 {
		t.Fatalf("exclude = %v", list)
	}
	record, ok := manifestValue(list[0])
	if !ok || record.Table()["start"] != int64(0x800) {
		t.Errorf("record = %v", record)
	}
	if _, err := LoadManifest(filepath.Join(t.TempDir(), "absent.toml")); err == nil {
		t.Error("Load of a missing file succeeded")
	}
	if _, err := ParseManifest([]byte("a = ")); err == nil {
		t.Error("Parse accepted invalid TOML")
	}
}

func TestIntKinds(t *testing.T) {
	tests := []struct {
		in      any
		asInt   int
		asIntOK bool
		pyInt   int
		pyIntOK bool
	}{
		{int64(5), 5, true, 5, true},
		{7, 7, true, 7, true},
		{true, 0, false, 1, true},
		{false, 0, false, 0, true},
		{1.0, 0, false, 0, false},
		{"1", 0, false, 0, false},
		{nil, 0, false, 0, false},
	}
	for _, tt := range tests {
		if n, ok := intValue(tt.in); n != tt.asInt || ok != tt.asIntOK {
			t.Errorf("intValue(%v) = %d, %v", tt.in, n, ok)
		}
		if n, ok := pyIntValue(tt.in); n != tt.pyInt || ok != tt.pyIntOK {
			t.Errorf("pyIntValue(%v) = %d, %v", tt.in, n, ok)
		}
	}
}

func TestAsList(t *testing.T) {
	if got := listValue([]int{1, 2}); !reflect.DeepEqual(got, []any{1, 2}) {
		t.Errorf("listValue([]int) = %v", got)
	}
	if got := listValue("ab"); got != nil {
		t.Errorf("listValue(string) = %v", got)
	}
	if _, ok := asList(nil); ok {
		t.Error("asList(nil) ok")
	}
}
