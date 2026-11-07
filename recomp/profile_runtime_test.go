package main

import (
	"bytes"
	"hash/crc32"
	"maps"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

const driverSource = `
#include "debug/profile.hpp"
#include "f3rt/block_profile.h"
#include <cassert>
#include <filesystem>
int main(int argc, char **argv) {
    assert(argc == 2);
    const auto root = std::filesystem::absolute(argv[1]);
    std::filesystem::create_directories(root / "moved");
    std::filesystem::current_path(root);
    f3rt::RomSet roms;
    main = {0x4e, 0x71}; roms.sound = {0x4e, 0x71};
    {
        f3rt::BlockProfileSession session(roms, "profiles/run.profile");
        f3_profile_main_counts[0] = 7;
        f3_profile_sound_counts[0] = 3;
        std::filesystem::current_path(root / "moved");
        session.flush();
    }
    {
        f3rt::BlockProfileSession session(roms, "../profiles/run.profile");
        assert(f3_profile_main_counts[0] == 7 && f3_profile_sound_counts[0] == 3);
        f3_profile_main_counts[0] += 2;
        ++f3_profile_sound_counts[0];
        std::filesystem::current_path(root);
        session.flush();
    }
    assert(!std::filesystem::exists(root / "moved/profiles/run.profile"));
    return 0;
}
`

func requireCompiler(t *testing.T) string {
	t.Helper()
	cxx := os.Getenv("CXX")
	if cxx == "" {
		cxx = "c++"
	}
	if _, err := exec.LookPath(cxx); err != nil {
		t.Skipf("C++ compiler %q not found: %v", cxx, err)
	}
	probe := exec.Command(cxx, "-x", "c++", "-E", "-o", os.DevNull, "-")
	probe.Stdin = strings.NewReader("#include <zlib.h>\n")
	if output, err := probe.CombinedOutput(); err != nil {
		t.Skipf("host lacks zlib.h for %s: %v\n%s", cxx, err, output)
	}
	return cxx
}

func run(t *testing.T, name string, args ...string) {
	t.Helper()
	cmd := exec.Command(name, args...)
	if output, err := cmd.CombinedOutput(); err != nil {
		t.Fatalf("%s: %v\n%s", name, err, output)
	}
}

func TestRelativeDestinationStaysFixedAndMergesAfterCwdChange(t *testing.T) {
	cxx := requireCompiler(t)
	repo, err := filepath.Abs("..")
	if err != nil {
		t.Fatal(err)
	}
	configs, err := filepath.Glob(filepath.Join(repo, "games", "*", "config.toml"))
	if err != nil || len(configs) == 0 {
		t.Skipf("no game configs under %s", repo)
	}

	root := t.TempDir()
	driver := filepath.Join(root, "driver.cpp")
	if err := os.WriteFile(driver, []byte(driverSource), 0o644); err != nil {
		t.Fatal(err)
	}
	var stdout, stderr bytes.Buffer
	args := append(append([]string{"--configs"}, configs...), "--output", filepath.Join(root, "rom_manifest.hpp"))
	if exit := runRoms(args, &stdout, &stderr); exit != 0 {
		t.Fatalf("compile roms exit %d: %s", exit, stderr.String())
	}

	binary := filepath.Join(root, "profile-regression")
	run(t, cxx, "-std=c++20", "-O2", "-Wall", "-Wextra", "-Werror", "-Wno-deprecated", "-DF3_PROFILE_INSTRUMENT=1",
		"-I", filepath.Join(repo, "include"), "-I", filepath.Join(repo, "runtime"), "-I", root,
		driver,
		filepath.Join(repo, "runtime/debug/profile.cpp"),
		filepath.Join(repo, "runtime/rom.cpp"),
		filepath.Join(repo, "runtime/third_party/sha1dc/sha1.c"),
		filepath.Join(repo, "runtime/third_party/sha1dc/ubc_check.c"),
		"-lz", "-o", binary)
	run(t, binary, root)

	got, err := ReadProfile(filepath.Join(root, "profiles/run.profile"))
	if err != nil {
		t.Fatal(err)
	}
	crc := crc32.ChecksumIEEE([]byte{0x4e, 0x71})
	want := map[RowKey]uint64{{"main", crc, 0}: 9, {"sound", crc, 0xC00000}: 4}
	if !maps.Equal(got.Hits, want) {
		t.Errorf("hits = %v, want %v", got.Hits, want)
	}
}
