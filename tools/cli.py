"""Unified CLI dispatcher for f3 tools."""
from __future__ import annotations

import argparse
import sys


def main(argv: list[str] | None = None) -> int:
    if argv is None:
        argv = sys.argv[1:]

    parser = argparse.ArgumentParser(
        prog="f3",
        description="Taito F3 recompiler and analysis toolkit",
    )
    subparsers = parser.add_subparsers(dest="subcommand", metavar="<subcommand>")

    # Subcommands
    subparsers.add_parser("analyze", help="Static and headless dynamic game analysis (f3a)", add_help=False)
    subparsers.add_parser("compare", help="Compare frames, audio, or sound traces", add_help=False)
    subparsers.add_parser("decode", help="Decode binary traces", add_help=False)
    subparsers.add_parser("differential", help="Instruction-level differential test harness against Musashi", add_help=False)
    subparsers.add_parser("profile", help="Execution profile management", add_help=False)
    subparsers.add_parser("rom", help="ROM structure and exclusion analysis", add_help=False)
    subparsers.add_parser("gameplay-seeds", help="Seeded headless gameplay regression runner", add_help=False)

    if not argv or argv[0] in ("-h", "--help"):
        parser.print_help()
        return 0

    cmd = argv[0]
    rest = argv[1:]

    if cmd == "analyze":
        from tools.analysis.cli import main as analyze_main
        try:
            analyze_main(rest)
            return 0
        except SystemExit as exc:
            return exc.code if isinstance(exc.code, int) else (0 if exc.code is None else 1)

    elif cmd == "compare":
        compare_parser = argparse.ArgumentParser(
            prog="f3 compare",
            description="Compare frames, audio WAVs, or sound traces",
        )
        compare_sub = compare_parser.add_subparsers(dest="compare_type", metavar="<type>")
        compare_sub.add_parser("frames", help="Compare pixel frames (BMP / raw buffers)", add_help=False)
        compare_sub.add_parser("audio", help="Compare PCM16 audio WAV files", add_help=False)
        compare_sub.add_parser("trace", help="Compare F3SND2 sound bus traces", add_help=False)

        if not rest or rest[0] in ("-h", "--help"):
            compare_parser.print_help()
            return 0

        target = rest[0]
        compare_args = rest[1:]
        if target == "frames":
            from tools.compare.frames import main as frames_main
            return frames_main(compare_args)
        elif target == "audio":
            from tools.compare.audio import main as audio_main
            return audio_main(compare_args)
        elif target == "trace":
            from tools.compare.trace import main as trace_main
            return trace_main(compare_args)
        else:
            compare_parser.error(f"unknown comparison target: {target}")

    elif cmd == "decode":
        decode_parser = argparse.ArgumentParser(
            prog="f3 decode",
            description="Decode binary traces",
        )
        decode_sub = decode_parser.add_subparsers(dest="decode_type", metavar="<type>")
        decode_sub.add_parser("trace", help="Decode F3SND2 sound bus traces to JSONL", add_help=False)

        if not rest or rest[0] in ("-h", "--help"):
            decode_parser.print_help()
            return 0

        target = rest[0]
        decode_args = rest[1:]
        if target == "trace":
            from tools.decode.trace import main as trace_decode_main
            return trace_decode_main(decode_args)
        else:
            decode_parser.error(f"unknown decode target: {target}")

    elif cmd == "differential":
        from tools.differential.runner import run_differential
        diff_parser = argparse.ArgumentParser(
            prog="f3 differential",
            description="Instruction-level differential self-test using Musashi 68EC020 reference core.",
        )
        from pathlib import Path
        diff_parser.add_argument("--musashi", type=Path, default=None,
                                 help="Path to Musashi source directory containing m68k.h")
        diff_parser.add_argument("--output", type=Path, default=Path("build/differential"),
                                 help="Output directory (default: build/differential)")
        diff_parser.add_argument("--cases", type=int, default=500,
                                 help="Number of test cases to generate (default: 500)")
        diff_parser.add_argument("--seed", type=int, default=42,
                                 help="Deterministic PRNG seed (default: 42)")
        diff_parser.add_argument("--instructions", type=Path, default=None,
                                 help="Optional path to external instructions JSON")
        diff_parser.add_argument("--filter", type=str, default=None,
                                 help="Optional filter substring for mnemonic")
        diff_parser.add_argument("--compile-only", action="store_true",
                                 help="Only generate and compile, do not execute")
        diff_parser.add_argument("-v", "--verbose", action="store_true",
                                 help="Enable verbose output")
        args = diff_parser.parse_args(rest)
        try:
            return run_differential(
                musashi_path=args.musashi,
                output_dir=args.output,
                cases_count=args.cases,
                seed=args.seed,
                instructions_json=args.instructions,
                filter_pattern=args.filter,
                compile_only=args.compile_only,
                verbose=args.verbose,
            )
        except (FileNotFoundError, ValueError, ImportError) as err:
            print(f"f3 differential: error: {err}", file=sys.stderr)
            return 2

    elif cmd == "profile":
        profile_parser = argparse.ArgumentParser(
            prog="f3 profile",
            description="Execution profile management",
        )
        profile_sub = profile_parser.add_subparsers(dest="profile_action", metavar="<action>")
        profile_sub.add_parser("merge", help="Merge multiple .profile files into one", add_help=False)

        if not rest or rest[0] in ("-h", "--help"):
            profile_parser.print_help()
            return 0

        action = rest[0]
        profile_args = rest[1:]
        if action == "merge":
            from tools.profile.merge import main as profile_merge_main
            return profile_merge_main(profile_args)
        else:
            profile_parser.error(f"unknown profile action: {action}")

    elif cmd == "rom":
        rom_parser = argparse.ArgumentParser(
            prog="f3 rom",
            description="ROM structure and exclusion analysis",
        )
        rom_sub = rom_parser.add_subparsers(dest="rom_action", metavar="<action>")
        rom_sub.add_parser("exclusions", help="Scan CPU ROM and propose code/data exclusions", add_help=False)

        if not rest or rest[0] in ("-h", "--help"):
            rom_parser.print_help()
            return 0

        action = rest[0]
        rom_args = rest[1:]
        if action == "exclusions":
            from tools.rom.exclusions import main as rom_exclusions_main
            return rom_exclusions_main(rom_args)
        else:
            rom_parser.error(f"unknown rom action: {action}")

    elif cmd == "gameplay-seeds":
        from tools.gameplay_seeds import main as gameplay_seeds_main
        return gameplay_seeds_main(rest)

    else:
        parser.error(f"unknown subcommand: {cmd}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
