"""Compare both CMake headers and main ROM images against the Python recompiler."""

import argparse
import difflib
import subprocess
import sys
from pathlib import Path
from tempfile import TemporaryDirectory

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from tools.recomp_py.discovery import load_rom  # noqa: E402


def compare_header(reference, actual):
    expected = reference.read_bytes()
    observed = actual.read_bytes()
    if expected != observed:
        diff = difflib.unified_diff(
            expected.decode().splitlines(keepends=True),
            observed.decode().splitlines(keepends=True),
            fromfile=str(reference),
            tofile=str(actual),
        )
        raise ValueError(f"{actual.name}: byte mismatch\n{''.join(diff)}")


def run(*args):
    result = subprocess.run(args, capture_output=True)
    if result.returncode:
        raise ValueError(f"{args[0]} exited {result.returncode}: {result.stderr.decode().strip()}")
    return result.stdout


def compare_headers(configs, game, directory, cli):
    filenames = ("rom_manifest.hpp", "generated_config/game_video_config.hpp")
    outputs = []
    for name, command in (
        ("python", [sys.executable, "tools/recomp_py/compile/roms.py"]),
        ("ocaml", [cli, "compile-roms"]),
    ):
        manifest, video = (directory / name / filename for filename in filenames)
        run(*command, "--configs", *configs, "--output", manifest,
            "--game", game, "--game-output", video)
        outputs.append((manifest, video))
    errors = []
    for reference, actual in zip(*outputs, strict=True):
        try:
            compare_header(reference, actual)
        except ValueError as error:
            errors.append(str(error))
    if errors:
        raise ValueError("\n".join(errors))


def compare_image(config, rom_dir, dumper):
    expected, _ = load_rom(config, rom_dir)
    observed = run(dumper, config, rom_dir)
    if expected != observed:
        offset = next((i for i, (a, b) in enumerate(zip(expected, observed)) if a != b),
                      min(len(expected), len(observed)))
        raise ValueError(
            f"first difference at {offset:#x}, lengths {len(expected):#x}/{len(observed):#x}; "
            f"Python {expected[offset:offset + 16].hex(' ')}, "
            f"OCaml {observed[offset:offset + 16].hex(' ')}"
        )


def compare_id(config, game, cli):
    script = (
        "import pathlib, sys, tomllib; p = pathlib.Path(sys.argv[1]); "
        "identity = tomllib.loads(p.read_text())['game']['id']; "
        "sys.exit(0 if identity == sys.argv[2] else "
        "'Configured game id does not match directory: ' + str(p))"
    )
    for identity in (game, f"{game}-mismatch"):
        reference = subprocess.run([sys.executable, "-c", script, config, identity],
                                   capture_output=True)
        actual = subprocess.run([cli, "check-game", config, identity], capture_output=True)
        if (actual.returncode, actual.stdout, actual.stderr) != (
            reference.returncode, reference.stdout, reference.stderr
        ):
            raise ValueError(
                f"{identity}: Python exit {reference.returncode}, stderr {reference.stderr!r}; "
                f"OCaml exit {actual.returncode}, stderr {actual.stderr!r}"
            )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rom-root", required=True, type=Path)
    args = parser.parse_args()
    if Path.cwd() != ROOT:
        parser.error("run from the repository root")
    build = Path("recomp/_build/default")
    cli = build / "bin/main.exe"
    dumper = build / "test/dump_main_image.exe"
    if not cli.is_file() or not dumper.is_file():
        parser.error("run dune build --root recomp first")
    configs = sorted(Path("games").glob("*/config.toml"), key=lambda path: path.as_posix())
    failed = False
    with TemporaryDirectory(dir=build) as temporary:
        for config in configs:
            game = config.parent.name
            results = []
            for label, compare, inputs in (
                ("headers", compare_headers, (configs, game, Path(temporary), cli)),
                ("image", compare_image, (config, args.rom_root / game, dumper)),
                ("check-game", compare_id, (config, game, cli)),
            ):
                try:
                    compare(*inputs)
                    results.append(f"{label} OK")
                except (OSError, ValueError) as error:
                    failed = True
                    results.append(f"{label} FAIL\n{error}")
            print(f"{game}: {', '.join(results)}", flush=True)
    return int(failed)


if __name__ == "__main__":
    sys.exit(main())
