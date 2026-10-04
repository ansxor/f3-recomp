#!/usr/bin/env python3
"""Build embedded SDL GPU SPIR-V and MSL; outputs belong in the build tree."""
import argparse
import json
from pathlib import Path
import subprocess

# SDL slots: sampled textures, storage buffers; uniforms occupy a separate set.
SHADERS = (
    ("fullscreen", "vert", 0, 0, 0),
    ("sprite", "vert", 0, 1, 1),
    ("sprite", "frag", 0, 1, 0),
    ("scene", "frag", 1, 3, 1),
)


def run(*args):
    result = subprocess.run(args, capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError(f"{args[0]} failed ({result.returncode}):\n{result.stdout}{result.stderr}")
    return result.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-dir", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--glslang", default="glslangValidator")
    parser.add_argument("--spirv-cross", default="spirv-cross")
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    work = args.output.parent / "video-shaders"
    work.mkdir(exist_ok=True)
    header = ["#pragma once", "#include <cstddef>", "#include <cstdint>", "namespace f3rt::video_shaders {",
              "struct Shader { const uint8_t *spirv; size_t spirv_size; const char *msl; size_t msl_size; const char *msl_entry; unsigned samplers, buffers, uniforms; };"]
    for name, stage, samplers, buffers, uniforms in SHADERS:
        ident = name + "_" + stage
        source = args.source_dir / (name + "." + stage)
        spv = work / (ident + ".spv")
        msl_spv = work / (ident + "-msl.spv")
        run(args.glslang, "-V", "--target-env", "vulkan1.0", "-I" + str(args.source_dir), "-o", str(spv), str(source))
        reflection = json.loads(run(args.spirv_cross, str(spv), "--reflect"))
        counts = (len(reflection.get("textures", [])), len(reflection.get("ssbos", [])), len(reflection.get("ubos", [])))
        if counts != (samplers, buffers, uniforms):
            raise RuntimeError(f"{ident}: resource counts {counts}, expected {(samplers, buffers, uniforms)}")
        # The vertex stage's storage buffer moves from Vulkan set0/binding0 to Metal buffer1.
        # Other shaders already have SDL's Metal binding numbers; decoration binding
        # avoids translator auto-assignment changing the ABI when resources reorder.
        run(args.glslang, "-V", "--target-env", "vulkan1.0", "-DVIDEO_MSL=1", "-I" + str(args.source_dir), "-o", str(msl_spv), str(source))
        entry = ident + "_main"
        msl = run(args.spirv_cross, str(msl_spv), "--msl", "--msl-version", "20100", "--msl-decoration-binding", "--rename-entry-point", "main", entry, stage)
        (work / (ident + ".metal")).write_text(msl)
        data = spv.read_bytes()
        header.append(f"alignas(4) inline constexpr uint8_t {ident}_spv[] = {{" + ",".join(str(x) for x in data) + "};")
        header.append(f'inline constexpr char {ident}_msl[] = R"F3VIDEO({msl})F3VIDEO";')
        header.append(f'inline constexpr Shader {ident}{{{ident}_spv, sizeof({ident}_spv), {ident}_msl, sizeof({ident}_msl)-1, "{entry}", {samplers}, {buffers}, {uniforms}}};')
    header.append("} // namespace f3rt::video_shaders")
    content = "\n".join(header) + "\n"
    if not args.output.exists() or args.output.read_text() != content:
        args.output.write_text(content)


if __name__ == "__main__":
    main()
