"""Compare RAM payload bytes and decoded entry lengths against Python."""

import argparse
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from tools.recomp_py.discovery import load_rom  # noqa: E402
from tools.recomp_py.ram_code import decode_ram_routines  # noqa: E402


def read_routines(output):
    routines = {}
    for line in output.splitlines():
        kind, address, value = line.split("\t")
        if kind == "payload":
            entries = {}
            routines[int(address)] = (bytes.fromhex(value), entries)
        elif kind == "entry":
            entries[int(address)] = int(value)
        else:
            raise ValueError(f"Unknown dump record: {line}")
    return routines


def compare(game, rom_root, dumper):
    config_path = ROOT / "games" / game / "config.toml"
    rom_dir = rom_root / game
    rom, config = load_rom(config_path, rom_dir)
    expected = {routine.destination: routine for routine in decode_ram_routines(rom, config)}
    observed = read_routines(subprocess.check_output(
        [dumper, "--dump", config_path, rom_dir], text=True,
    ))
    if not expected or expected.keys() != observed.keys():
        raise ValueError(f"Payload destinations differ: Python {list(expected)}, OCaml {list(observed)}")
    for destination, routine in expected.items():
        payload, entries = observed[destination]
        if payload != routine.payload:
            raise ValueError(f"Payload bytes differ at {destination:#x}")
        for pc, insn in routine.instructions.items():
            if entries.get(pc) != insn.size:
                raise ValueError(f"PC {pc:#x}: Python length {insn.size}, OCaml {entries.get(pc)}")
        reference_count = len(routine.instructions)
        print(f"{game} {destination:#x}: {len(payload)} identical bytes, "
              f"{reference_count} matching Python PCs, {len(entries) - reference_count} extra OCaml PCs")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rom-root", required=True, type=Path)
    args = parser.parse_args()
    dumper = ROOT / "recomp/_build/default/test/discover/ram_code_test.exe"
    if not dumper.is_file():
        parser.error("run dune build --root recomp first")
    failed = False
    for game in ("pbobble3j", "pbobble4j"):
        try:
            compare(game, args.rom_root, dumper)
        except (OSError, ValueError, subprocess.CalledProcessError) as error:
            failed = True
            print(f"{game}: {error}", file=sys.stderr)
    return int(failed)


if __name__ == "__main__":
    sys.exit(main())
