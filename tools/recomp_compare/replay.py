"""Compare frozen Python and OCaml runtime dumps under the T21 masked relation.

Run this command from the repo root.

    nix develop -c uv run --no-project python tools/recomp_compare/replay.py GAME

CPU CCR is fully masked because live-in sets are unavailable at capture points.
The baseline compiles C at -Og to keep the replay build quick. Tiers uses
the CMake hot and cold source flags.
Work RAM CCR is masked only at still-present exception SR words tagged by both
builds. The tag file is metadata for parity and must match in the strict
determinism check. Binary and register differences are reported Python first.
"""

from __future__ import annotations

import argparse
import dataclasses
import json
import os
import resource
import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
ASSETS = Path("/home/answer/Workspace/f3-recomp")
TRACE = "stacked_sr_low_bytes.txt"
CCR = 0x1F
CPU_FIELDS = ("frame", "pc", "steps", "sr", *(f"{bank}{i}" for bank in "da" for i in range(8)))
REQUIRED = {
    "cpu.json",
    TRACE,
    "mainram.bin",
    "shared.bin",
    "palette.bin",
    "graphics.bin",
    "control.bin",
    "rendered.argb",
    "rendered.bmp",
}


@dataclasses.dataclass
class Comparison:
    status: str
    frame: int | None = None
    differences: dict[str, str] = dataclasses.field(default_factory=dict)


def registers(path: Path) -> dict[str, int]:
    values = json.loads(path.read_text())
    for bank in "da":
        values.update({f"{bank}{i}": value for i, value in enumerate(values.pop(bank))})
    return values


def first_byte(left: bytes, right: bytes) -> str:
    for offset, (a, b) in enumerate(zip(left, right, strict=False)):
        if a != b:
            return f"offset 0x{offset:x}: 0x{a:02x} vs 0x{b:02x}"
    return f"length {len(left)} vs {len(right)}"


def compare(left: Path, right: Path, frames: range, *, strict: bool = False) -> Comparison:
    masked = False
    labels = ("first", "second") if strict else ("Python", "OCaml")
    for frame in frames:
        a, b = (root / f"frame_{frame:04d}" for root in (left, right))
        names_a = {path.name for path in a.glob("*")}
        names_b = {path.name for path in b.glob("*")}
        if not (names_a >= REQUIRED and names_b >= REQUIRED):
            return Comparison(
                "incomplete",
                frame,
                {
                    "dump": f"missing {labels[0]} {sorted(REQUIRED - names_a)}, "
                    f"{labels[1]} {sorted(REQUIRED - names_b)}",
                },
            )
        differences = {}
        if names_a != names_b:
            differences["dump"] = f"file sets differ: {sorted(names_a ^ names_b)}"
        cpu_a, cpu_b = registers(a / "cpu.json"), registers(b / "cpu.json")
        for name in CPU_FIELDS:
            x, y = cpu_a[name], cpu_b[name]
            if x == y:
                continue
            if not strict and name == "sr" and not ((x ^ y) & ~CCR):
                masked = True
            else:
                differences.setdefault("cpu.json", f"{name}: {x:#x} vs {y:#x}")
        for name in sorted((names_a & names_b) - {"cpu.json", TRACE}):
            x, y = (directory.joinpath(name).read_bytes() for directory in (a, b))
            if x == y:
                continue
            if not strict and name == "mainram.bin" and len(x) == len(y):
                # Both traces must identify the byte as the low byte of a surviving SR.
                offsets_a, offsets_b = (set(map(int, (directory / TRACE).read_text().split())) for directory in (a, b))
                x, y = bytearray(x), bytearray(y)
                for offset in offsets_a & offsets_b:
                    x[offset] &= ~CCR
                    y[offset] &= ~CCR
                masked |= x == y
            if x != y:
                differences[name] = first_byte(x, y)
        if strict:
            for name in ("cpu.json", TRACE):
                x, y = (directory.joinpath(name).read_bytes() for directory in (a, b))
                if x != y:
                    differences.setdefault(name, first_byte(x, y))
        if differences:
            return Comparison("diverged", frame, differences)
    return Comparison("masked-only" if masked else "equal")


def inputs_for(game: str, frames: int, override: Path | None) -> str:
    if override is not None:
        return override.read_text()
    if game in {"landmakr", "landmakrj"}:
        # Scenario B in docs/notes/raw/tests/landmakrj-2p-versus-trigger-runs.md.
        return "700+20 coin\n800-1100 mash keys=start period=45\n1350+10 b1\n1600-end mash keys=b1 period=45 seed=3\n"
    if game == "kirameki":
        # Gameplay reproduction in docs/notes/raw/disasm/kirameki-sound-selectors.md.
        # The first start pulse can precede readiness from a clean boot.
        return (
            "700+20 coin\n750+20 start\n800-1200 mash seed=1\n"
            "1050+12 coin\n1100+12 start\n1250+12 start\n"
            "1350+12 coin\n1400+12 start\n1550+12 start\n"
            "1201-end mash keys=b1+b2+b3+b4 period=31 seed=3\n"
            "2650+12 b1\n2800+12 b2\n2930+12 b3\n"
        )
    pulses = []
    for start in range(650, frames + 1, 300):
        pulses.extend((f"{start}+12 coin", f"{start + 45}+10 start"))
    pulses.append("800-end mash keys=right+left+up+down+b1+b2 period=17 seed=3")
    return "\n".join(pulses) + "\n"


def asset(path: Path) -> Path:
    local = ROOT / path
    return local if local.exists() else ASSETS / path


def run(command: list[str], log: Path, *, cwd: Path = ROOT) -> int:
    with log.open("w") as output:
        return subprocess.run(command, cwd=cwd, stdout=output, stderr=subprocess.STDOUT).returncode


def build(args: argparse.Namespace, variant: str, roms: Path, profile: str) -> Path:
    directory = args.build_dir / variant
    directory.mkdir(parents=True, exist_ok=True)
    options = {
        "CMAKE_BUILD_TYPE": "Release",
        # T21 compares guest state. Keep the uniform C build inexpensive.
        "CMAKE_C_FLAGS_RELEASE": "-Og -DNDEBUG" if args.mode == "baseline" else "-O3 -DNDEBUG",
        "BUILD_TESTING": "OFF",
        "F3_GAME": args.game,
        "F3_ROM_DIR": str(roms),
        "F3RT_TRACE_STACKED_SR": "ON",
        "F3_PROFILE_DEFAULT_TIERS": "OFF",
        "F3_PROFILE_TIERS": profile,
        "F3_PROFILE_SLIM": "",
        "F3_RECOMP_EXECUTABLE": str(args.ocaml) if variant == "ocaml" else "",
    }
    commands = {
        "configure": [
            "cmake",
            "-S",
            str(ROOT),
            "-B",
            str(directory),
            "-G",
            "Ninja",
            *(f"-D{key}={value}" for key, value in options.items()),
        ],
        "build": ["cmake", "--build", str(directory), "--target", args.game, "-j", str(args.jobs)],
    }
    for phase, command in commands.items():
        log = directory / f"{phase}.log"
        if run(command, log):
            raise RuntimeError(f"{variant} {phase} failed. See {log}")
    return directory / args.game


def capture(binary: Path, directory: Path, inputs: Path, frames: int, every: int, start: int) -> int:
    directory.mkdir()
    home = directory / "config"
    home.mkdir()
    command = [
        str(binary),
        "--headless",
        "--no-audio",
        "--frames",
        str(frames),
        "--inputs",
        str(inputs),
        "--dump-dir",
        str(directory / "dumps"),
        "--dump-start",
        str(start),
        "--dump-every",
        str(every),
    ]
    with (directory / "run.log").open("w") as output:
        return subprocess.run(
            command,
            cwd=directory,
            env={**os.environ, "XDG_CONFIG_HOME": str(home), "XDG_DATA_HOME": str(home)},
            stdout=output,
            stderr=subprocess.STDOUT,
        ).returncode


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("game")
    parser.add_argument("--mode", choices=("baseline", "tiers"), default="baseline")
    parser.add_argument("--frames", type=int, default=3000)
    parser.add_argument("--every", type=int, default=100)
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--inputs", type=Path)
    parser.add_argument("--rom-dir", type=Path)
    parser.add_argument("--profile", type=Path)
    parser.add_argument("--work-dir", type=Path, default=ROOT / "build/t21")
    parser.add_argument("--ocaml", type=Path, default=ROOT / "recomp/_build/default/bin/main.exe")
    parser.add_argument("--check-determinism", action="store_true")
    parser.add_argument(
        "--refine", action="store_true", help="Replay the first divergent sample interval with per-frame dumps"
    )
    args = parser.parse_args()
    if not 1 <= args.every <= args.frames or args.jobs < 1:
        parser.error("require 1 <= every <= frames and positive jobs")
    if args.game not in {path.parent.name for path in ROOT.glob("games/*/config.toml")}:
        parser.error("unknown game")
    args.ocaml = args.ocaml.resolve()
    roms = (args.rom_dir or asset(Path("roms") / args.game)).resolve()
    profile = (args.profile or asset(Path("profiles") / f"{args.game}.profile")).resolve()
    if not roms.is_dir() or not args.ocaml.is_file():
        parser.error("ROM directory and built OCaml CLI are required")
    if args.mode == "tiers" and not profile.is_file():
        parser.error("tiers mode requires a profile")
    for executable in ("cmake", "ninja", "uv"):
        if not shutil.which(executable):
            parser.error(f"{executable} is required inside nix develop")

    soft, hard = resource.getrlimit(resource.RLIMIT_STACK)
    stack = 64 * 1024**2
    if soft != resource.RLIM_INFINITY and soft < stack:
        resource.setrlimit(resource.RLIMIT_STACK, (stack, hard))
    args.build_dir = args.work_dir.resolve() / args.game / args.mode
    args.build_dir.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix="replay-", dir=args.build_dir))
    inputs = work / "inputs.txt"
    inputs.write_text(inputs_for(args.game, args.frames, args.inputs))
    print(f"{args.game} {args.mode} {work}", flush=True)

    binaries, errors = {}, {}
    for variant in ("python", "ocaml"):
        try:
            binaries[variant] = build(args, variant, roms, str(profile) if args.mode == "tiers" else "")
        except RuntimeError as error:
            errors[variant] = str(error)
    codes = {
        variant: capture(binary, work / variant, inputs, args.frames, args.every, args.every)
        for variant, binary in binaries.items()
    }
    dumps = {variant: work / variant / "dumps" for variant in ("python", "ocaml")}
    samples = range(args.every, args.frames + 1, args.every)
    result = compare(dumps["python"], dumps["ocaml"], samples)
    report = {"sampled": dataclasses.asdict(result), "build_errors": errors, "exit_codes": codes}
    if args.check_determinism and codes.get("python") == 0:
        repeat = work / "python-repeat"
        code = capture(binaries["python"], repeat, inputs, args.frames, args.every, args.every)
        report["determinism"] = dataclasses.asdict(compare(dumps["python"], repeat / "dumps", samples, strict=True))
        report["repeat_exit_code"] = code
    if args.refine and result.status == "diverged" and all(code == 0 for code in codes.values()):
        start = result.frame - args.every + 1
        report["refine_exit_codes"] = {
            variant: capture(binary, work / f"{variant}-refine", inputs, result.frame, 1, start)
            for variant, binary in binaries.items()
        }
        report["refined"] = dataclasses.asdict(
            compare(
                work / "python-refine/dumps",
                work / "ocaml-refine/dumps",
                range(start, result.frame + 1),
            )
        )
    serialized = json.dumps(report, indent=2)
    (work / "result.json").write_text(serialized + "\n")
    print(serialized)
    return int(
        result.status not in {"equal", "masked-only"}
        or any(codes.values())
        or report.get("repeat_exit_code", 0) != 0
        or report.get("determinism", {}).get("status", "equal") != "equal"
    )


if __name__ == "__main__":
    raise SystemExit(main())
