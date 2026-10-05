"""Actual profile persistence survives a frontend changing the process cwd."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest
import zlib

from recomp.block_profile import read_profile

ROOT = Path(__file__).resolve().parents[1]


class RuntimeProfileTests(unittest.TestCase):
    def test_relative_destination_stays_fixed_and_merges_after_cwd_change(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            driver = root / "driver.cpp"
            driver.write_text(r'''
#include "block_profile.hpp"
#include "f3rt/block_profile.h"
#include <cassert>
#include <filesystem>
int main(int argc, char **argv) {
    assert(argc == 2);
    const auto root = std::filesystem::absolute(argv[1]);
    std::filesystem::create_directories(root / "moved");
    std::filesystem::current_path(root);
    f3rt::RomSet roms;
    roms.main = {0x4e, 0x71}; roms.sound = {0x4e, 0x71};
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
''')
            binary = root / "profile-regression"
            subprocess.run(["python3", str(ROOT / "tools/compile_roms.py"), "--configs",
                            *map(str, sorted((ROOT / "games").glob("*/config.toml"))),
                            "--output", str(root / "rom_manifest.hpp")], check=True, cwd=ROOT)
            subprocess.run([os.environ.get("CXX", "c++"), "-std=c++20", "-O2", "-Wall", "-Wextra", "-Werror",
                            "-DF3_PROFILE_INSTRUMENT=1", "-I", str(ROOT / "include"), "-I", str(ROOT / "runtime"),
                            "-I", str(root),
                            str(driver), str(ROOT / "runtime/block_profile.cpp"), str(ROOT / "runtime/rom.cpp"),
                            "-o", str(binary)], check=True)
            subprocess.run([str(binary), str(root)], check=True)
            profile = read_profile(root / "profiles/run.profile")
            crc = zlib.crc32(bytes.fromhex("4e71"))
            self.assertEqual(profile.hits, {("main", crc, 0): 9, ("sound", crc, 0xc00000): 4})


if __name__ == "__main__":
    unittest.main()
