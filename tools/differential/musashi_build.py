"""Locate, generate, and build the Musashi 68EC020 reference core."""
from __future__ import annotations

import os
from pathlib import Path
import shutil
import subprocess
import sys
from typing import NamedTuple


class MusashiBuildInfo(NamedTuple):
    source_dir: Path
    build_dir: Path
    include_dirs: list[Path]
    library_path: Path


CANDIDATE_SEARCH_PATHS = [
    Path("runtime/third_party/musashi"),
    Path("../runtime/runtime/third_party/musashi"),
]


def find_musashi_source(explicit_path: Path | str | None = None) -> Path:
    if explicit_path:
        path = Path(explicit_path).resolve()
        if (path / "m68k.h").exists():
            return path
        raise FileNotFoundError(f"Specified Musashi path does not contain m68k.h: {path}")

    # Check candidates
    for candidate in CANDIDATE_SEARCH_PATHS:
        resolved = candidate.resolve()
        if (resolved / "m68k.h").exists():
            return resolved

    raise FileNotFoundError(
        "Musashi source not found in candidate paths. Specify --musashi /path/to/musashi"
    )


def build_musashi(source_dir: Path, output_dir: Path, verbose: bool = False) -> MusashiBuildInfo:
    source_dir = Path(source_dir).resolve()
    build_dir = (Path(output_dir) / "musashi").resolve()
    build_dir.mkdir(parents=True, exist_ok=True)

    lib_path = build_dir / "libmusashi.a"
    ops_c = build_dir / "m68kops.c"
    ops_h = build_dir / "m68kops.h"
    m68kmake_bin = build_dir / "m68kmake"

    cc = os.environ.get("CC", "clang")
    ar = os.environ.get("AR", "ar")

    source_files = [
        source_dir / "m68k.h",
        source_dir / "m68kconf.h",
        source_dir / "m68kcpu.h",
        source_dir / "m68kcpu.c",
        source_dir / "m68kmake.c",
        source_dir / "m68k_in.c",
        source_dir / "softfloat" / "softfloat.c",
    ]

    # Check if rebuild is needed
    rebuild = not lib_path.exists()
    if not rebuild:
        lib_mtime = lib_path.stat().st_mtime
        for src in source_files:
            if src.exists() and src.stat().st_mtime > lib_mtime:
                rebuild = True
                break

    if rebuild:
        if verbose:
            print(f"[musashi_build] Building Musashi reference in {build_dir}...")

        # 1. Compile m68kmake
        if not m68kmake_bin.exists() or (source_dir / "m68kmake.c").stat().st_mtime > m68kmake_bin.stat().st_mtime:
            cmd = [cc, "-O2", "-o", str(m68kmake_bin), str(source_dir / "m68kmake.c")]
            subprocess.run(cmd, check=True, cwd=build_dir, capture_output=not verbose)

        # 2. Run m68kmake to generate m68kops.c and m68kops.h
        cmd = [str(m68kmake_bin), ".", str(source_dir / "m68k_in.c")]
        subprocess.run(cmd, check=True, cwd=build_dir, capture_output=not verbose)

        # 3. Compile m68kcpu.c, m68kops.c, softfloat.c
        includes = ["-I.", f"-I{source_dir}"]
        cflags = ["-O2", "-w", "-DM68K_INSTRUCTION_HOOK=1"] + includes

        obj_cpu = build_dir / "m68kcpu.o"
        obj_ops = build_dir / "m68kops.o"
        obj_sf = build_dir / "softfloat.o"

        subprocess.run([cc, "-c"] + cflags + [str(source_dir / "m68kcpu.c"), "-o", str(obj_cpu)],
                       check=True, cwd=build_dir, capture_output=not verbose)
        subprocess.run([cc, "-c"] + cflags + [str(ops_c), "-o", str(obj_ops)],
                       check=True, cwd=build_dir, capture_output=not verbose)
        subprocess.run([cc, "-c"] + cflags + [str(source_dir / "softfloat" / "softfloat.c"), "-o", str(obj_sf)],
                       check=True, cwd=build_dir, capture_output=not verbose)

        # 4. Create archive libmusashi.a
        subprocess.run([ar, "rcs", str(lib_path), str(obj_cpu), str(obj_ops), str(obj_sf)],
                       check=True, cwd=build_dir, capture_output=not verbose)

        if verbose:
            print(f"[musashi_build] Built {lib_path} successfully.")

    return MusashiBuildInfo(
        source_dir=source_dir,
        build_dir=build_dir,
        include_dirs=[source_dir, build_dir],
        library_path=lib_path,
    )
