#!/usr/bin/env python3
"""Build diagnostic native copies and record instruction fetches versus ROM reads.

Requires a completed Release landmakrj build with CMAKE_EXPORT_COMPILE_COMMANDS=ON.
Never modifies the game, runtime, or original generated sources. Profiles cover
only the requested seeded runs; absence of a fetch is not proof of data.
"""
from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
import json
from pathlib import Path
import re
import shlex
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from recomp.discovery import capstone, load_rom
from tools.compile_sound import load_sound_rom


def instrument_generated(text: str, rom: bytes, sound: bool) -> str:
    mode = capstone.CS_MODE_M68K_000 if sound else capstone.CS_MODE_M68K_020
    decoder = capstone.Cs(capstone.CS_ARCH_M68K, capstone.CS_MODE_BIG_ENDIAN | mode)
    base = 0xc00000 if sound else 0

    def size_at(pc: int) -> int:
        offset = pc - base
        code = bytearray(rom[offset:offset + 24])
        opcode = int.from_bytes(code[:2], "big")
        if sound and len(code) >= 4 and (opcode in (0x003c, 0x023c, 0x0a3c)
                                         or opcode & 0xff00 == 0x0800):
            code[2] = 0
        insn = next(decoder.disasm(bytes(code).ljust(24, b"\0"), pc, count=1), None)
        return insn.size if insn is not None and insn.id and pc + insn.size <= base + len(rom) else 2

    if sound:
        pattern = r"(void f3_sound_block_([0-9a-f]+)\(f3_cpu \*cpu\) \{)"
        text = re.sub(pattern, lambda m: m[1] + f"\n    f3_profile_fetch(1, 0x{m[2]}u, {size_at(int(m[2], 16))});", text)
        text = re.sub(r"(static void f3_sound_vector_\d+\(f3_cpu \*cpu\) \{)",
                      r"\1 f3_profile_fetch(1, cpu->pc, 2);", text)
    else:
        text = re.sub(r"(L_([0-9a-f]+): \{)",
                      lambda m: m[1] + f"\n    f3_profile_fetch(0, 0x{m[2]}u, {size_at(int(m[2], 16))});", text)
        text = re.sub(r"(static void f3_rom_exception_\d+\(f3_cpu \*cpu\) \{)",
                      r"\1\n    f3_profile_fetch(0, cpu->pc, 2);", text)
    return '#include "profile_hooks.h"\n' + text


def merge_intervals(values: list[list[int]]) -> list[list[int]]:
    merged: list[list[int]] = []
    for start, end in sorted(values):
        if merged and start <= merged[-1][1]:
            merged[-1][1] = max(merged[-1][1], end)
        else:
            merged.append([start, end])
    return merged


def build_profile(build: Path, output: Path, config: Path, rom_dir: Path, jobs: int) -> Path:
    database = json.loads((build / "compile_commands.json").read_text())
    main_rom, game_config = load_rom(config, rom_dir)
    if game_config["game"]["id"] != "landmakrj":
        raise ValueError("Native access profiling supports only the verified landmakrj runtime/build")
    sound_rom = load_sound_rom(rom_dir)
    sources = output / "sources"
    objects = output / "objects"
    sources.mkdir(parents=True, exist_ok=True)
    objects.mkdir(parents=True, exist_ok=True)
    (sources / "profile_hooks.h").write_text(
        '#include <stdint.h>\n'
        '#ifdef __cplusplus\nextern "C" {\n#endif\n'
        'void f3_profile_fetch(unsigned cpu, uint32_t pc, unsigned size);\n'
        'void f3_profile_data(unsigned cpu, uint32_t address, unsigned width, uint32_t pc);\n'
        '#ifdef __cplusplus\n}\n#endif\n')
    commands = []
    current_objects = []
    for row in database:
        source = Path(row["file"])
        sound = source.name.startswith("sound_") and source.suffix == ".c"
        main = (source.name.startswith("blocks_") or source.name == "program.c") and source.suffix == ".c"
        runtime = source in (ROOT / "runtime/machine.cpp", ROOT / "runtime/sound_native.cpp")
        harness = source == ROOT / "tools/gameplay_regression.cpp"
        if not (main or sound or runtime or harness):
            continue
        args = shlex.split(row["command"])
        if main or sound:
            private = sources / source.name
            private.write_text(instrument_generated(source.read_text(), sound_rom if sound else main_rom, sound))
        elif runtime:
            private = sources / source.name
            text = source.read_text()
            if source.name == "machine.cpp":
                opener = "uint8_t Machine::read8(uint32_t a) {"
                replacement = opener + "\n    f3_profile_data(0, a & 0xffffffu, 1, cpu.pc);"
            else:
                opener = "void SoundNative::trace_sound(uint32_t address, uint32_t value, uint8_t width, bool write) {"
                replacement = opener + "\n    f3_profile_data(1, address & 0xffffffu, width, m_cpu.pc);"
            if text.count(opener) != 1:
                raise ValueError(f"Cannot identify profiling callback in {source}")
            private.write_text('#include "profile_hooks.h"\n' + text.replace(opener, replacement))
        else:
            private = source
            args.append("-Dmain=f3_gameplay_main")
        obj = objects / (source.name + ".o")
        args[args.index("-o") + 1] = str(obj)
        args[args.index("-c") + 1] = str(private)
        args.extend(["-I", str(sources), "-I", str(ROOT / "runtime")])
        commands.append((args, Path(row["directory"])))
        current_objects.append(obj)
    if not any("gameplay_regression.cpp" in " ".join(command) for command, _ in commands):
        raise ValueError("Build the f3rt-gameplay-regression target first")

    def compile_one(item):
        command, cwd = item
        result = subprocess.run(command, cwd=cwd, capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError(result.stderr + "\n" + shlex.join(command))

    with ThreadPoolExecutor(max_workers=jobs) as pool:
        list(pool.map(compile_one, commands))
    binary = output / "rom-access-profile"
    subprocess.run(["c++", "-std=c++20", "-O3", "-I", str(ROOT / "include"),
                    str(ROOT / "tools/rom_access_profile.cpp"),
                    *(str(path) for path in current_objects),
                    str(build / "libf3rt.a"), str(build / "libf3rt_musashi.a"),
                    "-o", str(binary)], check=True)
    return binary


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=Path("build"))
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--config", type=Path, default=ROOT / "games/landmakrj/config.toml")
    parser.add_argument("--rom-dir", type=Path, required=True)
    parser.add_argument("--seeds", type=int, nargs="+", default=list(range(1, 9)))
    parser.add_argument("--frames", type=int, default=20000)
    parser.add_argument("--jobs", type=int, default=6)
    args = parser.parse_args()
    if args.frames <= 0 or args.jobs <= 0 or any(seed < 0 or seed >= 1 << 64 for seed in args.seeds):
        parser.error("Positive frames/jobs and uint64 seeds are required")
    build, output = args.build_dir.resolve(), args.output_dir.resolve()
    if output == build or output == ROOT or output in (args.config.resolve(), args.rom_dir.resolve()):
        parser.error("Use a separate ignored output directory")
    output.mkdir(parents=True, exist_ok=True)
    binary = build_profile(build, output, args.config.resolve(), args.rom_dir.resolve(), args.jobs)
    profiles = []
    for seed in args.seeds:
        profile_path = output / f"seed{seed}.json"
        command = [str(binary), "--rom-dir", str(args.rom_dir.resolve()), "--seed", str(seed),
                   "--frames", str(args.frames), "--sound-driver", "native",
                   "--wav", str(output / f"seed{seed}.wav"), "--profile-output", str(profile_path)]
        result = subprocess.run(command, capture_output=True, text=True)
        (output / f"seed{seed}.log").write_text(result.stdout + result.stderr)
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)
        print(result.stdout.strip(), flush=True)
        profiles.append(json.loads(profile_path.read_text()))
    merged = {"runs": [run for profile in profiles for run in profile["runs"]]}
    for cpu in ("main", "sound"):
        merged[cpu] = {
            kind: merge_intervals([interval for profile in profiles for interval in profile[cpu][kind]])
            for kind in ("fetched", "data_reads")
        }
        known = merge_intervals([[record["start"], record["end"]]
                                 for profile in profiles for record in profile[cpu]["known_data"]])
        exemplar = next((record for profile in profiles for record in profile[cpu]["known_data"]), None)
        merged[cpu]["known_data"] = [{**exemplar, "start": start, "end": end} for start, end in known] if exemplar else []
    (output / "profile.json").write_text(json.dumps(merged, indent=2) + "\n")
    print(f"Profile: {output / 'profile.json'}")


if __name__ == "__main__":
    main()
