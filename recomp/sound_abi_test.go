package main

import (
	"os"
	"path/filepath"
	"regexp"
	"runtime"
	"strconv"
	"strings"
	"testing"
)

func soundRepoRoot(t testing.TB) string {
	t.Helper()
	_, file, _, ok := runtime.Caller(0)
	if !ok {
		t.Fatal("runtime.Caller failed")
	}
	return filepath.Join(filepath.Dir(file), "..")
}

func TestHeaderABIVersionMatchesGenerator(t *testing.T) {
	header, err := os.ReadFile(filepath.Join(soundRepoRoot(t), "include", "f3rt", "cpu_abi.h"))
	if err != nil {
		t.Fatal(err)
	}
	match := regexp.MustCompile(`(?m)^#define\s+F3RT_ABI_VERSION\s+(\d+)u?\b`).FindSubmatch(header)
	if match == nil {
		t.Fatal("F3RT_ABI_VERSION not found in include/f3rt/cpu_abi.h")
	}
	version, err := strconv.Atoi(string(match[1]))
	if err != nil {
		t.Fatal(err)
	}
	if version != RuntimeABIVersion {
		t.Errorf("header ABI version %d, generator %d", version, RuntimeABIVersion)
	}
}

func TestSoundCompilerUsesGeneratorABIVersion(t *testing.T) {
	sources, err := filepath.Glob("*.go")
	if err != nil {
		t.Fatal(err)
	}
	literal := regexp.MustCompile(`F3RT_ABI_VERSION\s*(!=\s*)?\d`)
	var found bool
	for _, name := range sources {
		if strings.HasSuffix(name, "_test.go") {
			continue
		}
		source, err := os.ReadFile(name)
		if err != nil {
			t.Fatal(err)
		}
		found = found || strings.Contains(string(source), "RuntimeABIVersion")
		if literal.Match(source) {
			t.Errorf("%s hard-codes F3RT_ABI_VERSION", name)
		}
	}
	if !found {
		t.Error("no sound compiler source references RuntimeABIVersion")
	}
}
