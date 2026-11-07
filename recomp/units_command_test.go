package main

import (
	"bytes"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

const sampleConfig = `
[[video.emit_units]]
name = "objects"
start = 0x400
end = 0x40A
unit = "a6"
size = 128
`

func write(t *testing.T, dir, text string) string {
	t.Helper()
	path := filepath.Join(dir, "config.toml")
	if err := os.WriteFile(path, []byte(text), 0o644); err != nil {
		t.Fatal(err)
	}
	return path
}

func TestRunWritesHeadersAndKeepsMtimes(t *testing.T) {
	dir := t.TempDir()
	path := write(t, dir, sampleConfig)
	out := filepath.Join(dir, "out", "sub")
	var stdout, stderr bytes.Buffer
	if exit := runSpriteUnits([]string{"--config", path, "--output-dir", out}, &stdout, &stderr); exit != 0 {
		t.Fatalf("exit %d: %s", exit, stderr.String())
	}
	if stdout.Len() != 0 || stderr.Len() != 0 {
		t.Errorf("unexpected output %q %q", stdout.String(), stderr.String())
	}
	header, err := os.ReadFile(filepath.Join(out, "sprite_units.h"))
	if err != nil || !strings.Contains(string(header), "#define F3_SPRITE_UNIT_COUNT 1\n") {
		t.Errorf("sprite_units.h = %q, %v", header, err)
	}
	cpp, err := os.ReadFile(filepath.Join(out, "sprite_units.hpp"))
	if err != nil || !strings.Contains(string(cpp), "all{&objects}") {
		t.Errorf("sprite_units.hpp = %q, %v", cpp, err)
	}
	old := time.Now().Add(-time.Hour).Truncate(time.Second)
	for _, name := range []string{"sprite_units.h", "sprite_units.hpp"} {
		if err := os.Chtimes(filepath.Join(out, name), old, old); err != nil {
			t.Fatal(err)
		}
	}
	if exit := runSpriteUnits([]string{"--config=" + path, "--output-dir=" + out}, &stdout, &stderr); exit != 0 {
		t.Fatalf("second run exit %d: %s", exit, stderr.String())
	}
	for _, name := range []string{"sprite_units.h", "sprite_units.hpp"} {
		if info, _ := os.Stat(filepath.Join(out, name)); !info.ModTime().Equal(old) {
			t.Errorf("%s was rewritten", name)
		}
	}
}

func TestRunReportsInvalidConfigWithExitOne(t *testing.T) {
	dir := t.TempDir()
	path := write(t, dir, strings.Replace(sampleConfig, "0x400", "0x401", 1))
	var stdout, stderr bytes.Buffer
	exit := runSpriteUnits([]string{"--config", path, "--output-dir", filepath.Join(dir, "out")}, &stdout, &stderr)
	if want := path + ": emit unit 'objects' start 0x401 must be even\n"; exit != 1 || stderr.String() != want {
		t.Errorf("exit %d, stderr %q, want %q", exit, stderr.String(), want)
	}
	if _, err := os.Stat(filepath.Join(dir, "out")); !os.IsNotExist(err) {
		t.Error("output directory created despite the error")
	}
}

func TestRunUsageErrors(t *testing.T) {
	var stdout, stderr bytes.Buffer
	if exit := runSpriteUnits([]string{"--bogus"}, &stdout, &stderr); exit != 2 ||
		!strings.HasSuffix(stderr.String(), "sprite_units.py: error: the following arguments are required: --config, --output-dir\n") {
		t.Errorf("exit %d, stderr %q", exit, stderr.String())
	}
	stdout.Reset()
	if exit := runSpriteUnits([]string{"-h"}, &stdout, &stderr); exit != 0 || !strings.HasPrefix(stdout.String(), unitsUsage) {
		t.Errorf("help: exit %d, stdout %q", exit, stdout.String())
	}
}
