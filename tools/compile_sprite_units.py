#!/usr/bin/env python3
"""Write sprite_units.h/.hpp for one game config (no ROM needed)."""
from pathlib import Path
import argparse
import sys
import tomllib

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from recomp.sprite_units import parse_sprite_units, render_c_header, render_cpp_header


def write_if_changed(path: Path, text: str) -> None:
    # Keep mtimes stable so unchanged configs do not rebuild the runtime.
    if not path.exists() or path.read_text("utf-8") != text:
        path.write_text(text, "utf-8")


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    args = parser.parse_args(argv)
    config = tomllib.loads(args.config.read_text("utf-8"))
    try:
        spec = parse_sprite_units(config)
    except ValueError as error:
        parser.exit(1, f"{args.config}: {error}\n")
    args.output_dir.mkdir(parents=True, exist_ok=True)
    write_if_changed(args.output_dir / "sprite_units.h", render_c_header(spec))
    write_if_changed(args.output_dir / "sprite_units.hpp", render_cpp_header(spec))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
