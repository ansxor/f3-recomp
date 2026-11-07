package main

import (
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

func TestInlineAccessMatchesTheCallbacks(t *testing.T) {
	cc := cCompiler(t)
	root := generateRepoRoot(t)
	dir := t.TempDir()
	driver, err := os.ReadFile("testdata/inline_memory_driver.c")
	if err != nil {
		t.Fatal(err)
	}
	source := filepath.Join(dir, "driver.c")
	if err := os.WriteFile(source, driver, 0o644); err != nil {
		t.Fatal(err)
	}
	binary := filepath.Join(dir, "driver")
	build := exec.Command(cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
		"-I", root, "-I", filepath.Join(root, "include"), source, "-o", binary)
	if out, err := build.CombinedOutput(); err != nil {
		t.Fatalf("compile: %v\n%s", err, out)
	}
	out, err := exec.Command(binary).Output()
	if err != nil {
		t.Fatal(err)
	}
	if got := strings.TrimSpace(string(out)); got != "ok" {
		t.Fatalf("driver output %q", got)
	}
}

func TestInlineMemoryRewritesOnlyGuestAccesses(t *testing.T) {
	got := inlineMemory("f3_write16(cpu, a, f3_read8(cpu, b)); xf3_read32(c); f3_read64(d);")
	want := "f3_st16(cpu, a, f3_ld8(cpu, b)); xf3_read32(c); f3_read64(d);"
	if got != want {
		t.Fatalf("got %q want %q", got, want)
	}
}
