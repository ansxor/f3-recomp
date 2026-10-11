"""Run strict-native gameplay regression across multiple seeds; any failed seed fails the run."""
from __future__ import annotations

import argparse
from pathlib import Path
import subprocess
import sys
import time


def find_default_binary() -> Path | None:
    candidates = [
        Path("build/f3rt-tool"),
        Path("build-landmakrj/f3rt-tool"),
        Path("build-commandw/f3rt-tool"),
        Path("build/f3rt-gameplay-regression"),
        Path("build-landmakrj/f3rt-gameplay-regression"),
    ]
    for candidate in candidates:
        if candidate.is_file():
            return candidate.resolve()
    return None


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="f3 gameplay-seeds",
        description="Run strict-native gameplay regression across multiple seeds."
    )
    parser.add_argument("--binary", type=Path, default=None,
                        help="Path to f3rt-tool binary (or f3rt-gameplay-regression)")
    parser.add_argument("--rom-dir", type=Path)
    parser.add_argument("--seeds", type=int, nargs="+", default=list(range(1, 9)))
    parser.add_argument("--frames", type=int, default=40000)
    parser.add_argument("--dump-captures-dir", type=Path)
    parser.add_argument("--video-diff", action="store_true")
    parser.add_argument("--video-layer-mask", type=lambda value: int(value, 0), default=511)
    parser.add_argument("--video-diff-every", type=int, default=120)
    args = parser.parse_args(argv)

    if args.frames <= 0 or any(seed < 0 or seed >= 1 << 64 for seed in args.seeds):
        parser.error("frames must be positive and seeds must fit uint64")
    if args.video_diff_every <= 0 or not 1 <= args.video_layer_mask <= 511:
        parser.error("video interval must be positive and layer mask must select bits 0..8")

    binary = (args.binary.resolve() if args.binary else find_default_binary())
    if not binary or not binary.is_file():
        parser.error("f3rt-tool executable not found; build target f3rt-tool first or specify --binary")

    is_f3rt_tool = ("f3rt-tool" in binary.name)

    if args.dump_captures_dir:
        args.dump_captures_dir.mkdir(parents=True, exist_ok=True)
    failures = []
    started = time.monotonic()
    for seed in args.seeds:
        command = [str(binary)]
        if is_f3rt_tool:
            command.append("gameplay")
        command += ["--seed", str(seed), "--frames", str(args.frames)]
        if args.rom_dir:
            command += ["--rom-dir", str(args.rom_dir)]
        if args.video_diff:
            command += ["--video-diff", "--video-layer-mask", str(args.video_layer_mask),
                        "--video-diff-every", str(args.video_diff_every)]
        if args.dump_captures_dir:
            command += ["--surface", str(args.dump_captures_dir / f"seed_{seed}.bmp")]
        print(f"seed={seed} requested_frames={args.frames}", flush=True)
        result = subprocess.run(command)
        if result.returncode:
            failures.append(seed)
    print(f"seeds={len(args.seeds)} failures={failures} elapsed_seconds={time.monotonic() - started:.3f}")
    return int(bool(failures))


if __name__ == "__main__":
    sys.exit(main())
