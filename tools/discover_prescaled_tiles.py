#!/usr/bin/env python3
"""Decode explicit ROM descriptors and correlate prescaled F3 playfield assets.

Inputs live in [[prescaled_tiles.pairs]] and [[prescaled_tiles.maps]] in the
manifest. A map has either descriptor, table+records, or pointer_table+selections
(and records per selected table), plus columns. Tables contain {u32 pointer,
u16 attribute} records; descriptor headers are unsigned byte DBRA maxima.
Origins/destinations/repeat and evidence_pcs are source context, not pixel guesses.
Each pair explicitly supplies scale_x/scale_y; no default universal transform.

Example: tools/discover_prescaled_tiles.py --config games/commandw/config.toml
--rom-root roms --output command-assets.json. This is asset research, not a
renderer: aliases preserve the half map and name a full-resolution virtual
source. Transform fractional local source coordinates before flooring.
"""
from __future__ import annotations

import argparse
from collections import defaultdict
import hashlib
import json
from pathlib import Path
import sys
import tomllib

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from recomp.roms import load_region


def word(rom, address, size):
    if address < 0 or address + size > len(rom):
        raise ValueError(f"Program read outside image: {address:#x}+{size}")
    return int.from_bytes(rom[address:address + size], "big")


def descriptor(rom, address):
    width, height = word(rom, address, 1) + 1, word(rom, address + 1, 1) + 1
    end = address + 2 + width * height * 2
    if end > len(rom):
        raise ValueError(f"Descriptor {address:#x} exceeds program image")
    return {"address": address, "width": width, "height": height,
            "raw_hex": rom[address:end].hex(),
            "tiles": [word(rom, address + 2 + i * 2, 2) for i in range(width * height)]}


def decode_tile(low, high, tile_id):
    if tile_id < 0 or (tile_id + 1) * 128 > len(low):
        raise ValueError(f"Tile {tile_id:#x} outside low-plane image")
    if high and (tile_id + 1) * 64 > len(high):
        raise ValueError(f"Tile {tile_id:#x} outside high-plane image")
    # Same layout as runtime/renderer/fdp/video.cpp::decode_roms (tilemaps, not sprites).
    pixels = bytearray(256)
    for y in range(16):
        for x in range(16):
            pen = (low[tile_id * 128 + y * 8 + x // 2] >> ((x & 1) * 4)) & 15
            if high:
                row = tile_id * 64 + y * 4 + (0 if x < 8 else 2)
                pen |= ((high[row] >> (x & 7)) & 1) << 4
                pen |= ((high[row + 1] >> (x & 7)) & 1) << 5
            pixels[y * 16 + x] = pen
    return bytes(pixels)


def maps(rom, spec, name):
    columns = spec["columns"]
    if not isinstance(columns, int) or columns <= 0:
        raise ValueError(f"{name}: columns must be positive")
    sources = sum(key in spec for key in ("descriptor", "table", "pointer_table"))
    if sources != 1:
        raise ValueError(f"{name}: exactly one descriptor/table/pointer_table is required")
    tables = [(None, spec.get("table"))]
    if "pointer_table" in spec:
        tables = [(index, word(rom, spec["pointer_table"] + index * 4, 4))
                  for index in range(spec["selections"])]
    result = []
    for selection, table_address in tables:
        if "descriptor" in spec:
            records = [{"descriptor": spec["descriptor"], "attribute": spec.get("attribute")}]
        else:
            records = [{"record_address": table_address + index * 6,
                        "descriptor": word(rom, table_address + index * 6, 4),
                        "attribute": word(rom, table_address + index * 6 + 4, 2)}
                       for index in range(spec["records"])]
        if len(records) % columns:
            raise ValueError(f"{name}: descriptor count is not a multiple of columns")
        decoded = [descriptor(rom, record["descriptor"]) for record in records]
        width, height = decoded[0]["width"], decoded[0]["height"]
        if any((item["width"], item["height"]) != (width, height) for item in decoded):
            raise ValueError(f"{name}: rectangular assembly requires equal descriptor dimensions")
        tiles = [tile for row in range(len(records) // columns) for y in range(height)
                 for column in range(columns)
                 for tile in decoded[row * columns + column]["tiles"][y * width:(y + 1) * width]]
        result.append({"name": name, "selection": selection, "context": dict(spec),
                       "selected_table": table_address, "records": records, "descriptors": decoded,
                       "columns": columns * width, "rows": len(records) // columns * height,
                       "width_pixels": columns * width * 16,
                       "height_pixels": len(records) // columns * height * 16, "tiles": tiles})
    return result


def pixel(grid, pixels, x, y):
    x %= grid["width_pixels"]
    y %= grid["height_pixels"]
    tile_id = grid["tiles"][(y // 16) * grid["columns"] + x // 16]
    return pixels[tile_id][(y & 15) * 16 + (x & 15)]


def source(grid, pixels, x, y, sx, sy, mask):
    return bytes(pixel(grid, pixels, x + dx, y + dy) & mask
                 for dy in range(16 * sy) for dx in range(16 * sx))


def sampled(full, sx, sy):
    return bytes(full[y * sy * 16 * sx + x * sx] for y in range(16) for x in range(16))


def period(grid, pixels, axis, mask):
    extent = grid["width_pixels" if axis == "x" else "height_pixels"]
    # Report the minimum actual decoded-pixel period, not a tile-ID heuristic.
    for candidate in range(1, extent + 1):
        if extent % candidate:
            continue
        if all((pixel(grid, pixels, x, y) & mask) ==
               (pixel(grid, pixels, x + (candidate if axis == "x" else 0),
                      y + (candidate if axis == "y" else 0)) & mask)
               for y in range(grid["height_pixels"]) for x in range(grid["width_pixels"])):
            return candidate
    return extent


def analyze(config_path, rom_root):
    config_path, rom_root = Path(config_path), Path(rom_root)
    config = tomllib.loads(config_path.read_text("utf-8"))
    candidates = [rom_root / config["game"]["id"], rom_root]
    required = [lane["file"] for region in ("rom", "tiles", "tiles_hi")
                for lane in config.get(region, {}).get("lanes", [])]
    rejected = []
    for directory in candidates:
        if not all((directory / file).is_file() for file in required):
            continue
        try:
            rom = load_region(config, "rom", directory)
            low = load_region(config, "tiles", directory)
            high = load_region(config, "tiles_hi", directory) if "tiles_hi" in config else b""
        except ValueError as error:
            rejected.append({"directory": str(directory), "reason": str(error)})
            continue
        break
    else:
        raise ValueError(f"No matching physical ROM chips under {rom_root}; rejected {rejected}")
    settings = config.get("prescaled_tiles", {})
    if settings.get("descriptor_format") != "byte_dbra_u16be":
        raise ValueError("Unsupported prescaled_tiles.descriptor_format; expected byte_dbra_u16be")
    if not settings.get("pairs"):
        raise ValueError("Manifest must declare evidence-backed prescaled_tiles.pairs")
    pairs, standalone = [], []
    for spec in settings["pairs"]:
        sx, sy, mask = spec["scale_x"], spec["scale_y"], spec["pen_mask"]
        if not all(isinstance(value, int) and value > 0 for value in (sx, sy)) or not 0 < mask <= 63:
            raise ValueError(f"{spec['name']}: invalid scale or pen mask")
        normal = maps(rom, spec["normal"], spec["name"] + "/normal")
        half = maps(rom, spec["half"], spec["name"] + "/half")
        if len(normal) != len(half):
            raise ValueError(f"{spec['name']}: normal and half selection counts differ")
        pairs.extend({"name": spec["name"], "selection": n["selection"],
                      "context": spec, "normal": n, "half": h,
                      "scale_x": sx, "scale_y": sy, "pen_mask": mask}
                     for n, h in zip(normal, half))
    for spec in settings.get("maps", []):
        standalone.extend(maps(rom, spec, spec["name"]))
    all_grids = standalone + [pair[side] for pair in pairs for side in ("normal", "half")]
    pixels = {tile_id: decode_tile(low, high, tile_id)
              for tile_id in {t for grid in all_grids for t in grid["tiles"]}}
    indexes = {}
    virtual_sources = {}
    for transform in {(p["scale_x"], p["scale_y"], p["pen_mask"]) for p in pairs}:
        sx, sy, mask = transform
        index = defaultdict(set)
        # All explicitly rooted normal contexts participate, not just the first pair.
        normal_grids = standalone + [p["normal"] for p in pairs]
        for grid in normal_grids:
            for row in range(grid["rows"]):
                for column in range(grid["columns"]):
                    full = source(grid, pixels, column * 16, row * 16, sx, sy, mask)
                    group = [grid["tiles"][((row + dy) % grid["rows"]) * grid["columns"] +
                                            (column + dx) % grid["columns"]]
                             for dy in range(sy) for dx in range(sx)]
                    digest = hashlib.sha256(full).hexdigest()
                    key = f"{sx}:{sy}:{mask}:{digest}"
                    virtual = virtual_sources.setdefault(key, {"id": key, "scale_x": sx,
                        "scale_y": sy, "pen_mask": mask, "full_source_sha256": digest,
                        "width": 16 * sx, "height": 16 * sy, "contexts": []})
                    virtual["contexts"].append({"map": grid["name"], "selection": grid["selection"],
                                               "origin_tiles": [column, row], "tiles": group})
                    index[sampled(full, sx, sy)].add(key)
        indexes[transform] = index
    aliases = defaultdict(list)
    for pair in pairs:
        normal, half = pair["normal"], pair["half"]
        sx, sy, mask = pair["scale_x"], pair["scale_y"], pair["pen_mask"]
        mismatches, examples = 0, []
        for y in range(half["height_pixels"]):
            for x in range(half["width_pixels"]):
                expected = pixel(normal, pixels, x * sx, y * sy) & mask
                actual = pixel(half, pixels, x, y) & mask
                if expected != actual:
                    mismatches += 1
                    if len(examples) < 32:
                        examples.append({"half_xy": [x, y], "full_xy":
                                         [x * sx % normal["width_pixels"], y * sy % normal["height_pixels"]],
                                         "expected": expected, "actual": actual})
        pair["correlation"] = {"compared_pixels": half["width_pixels"] * half["height_pixels"],
                               "mismatched_pixels": mismatches, "examples": examples,
                               "exact": mismatches == 0, "wrap": "explicit_normal_grid_extent"}
        pair["periods"] = {side: {axis: period(pair[side], pixels, axis, mask) for axis in ("x", "y")}
                           for side in ("normal", "half")}
        cells = []
        index = indexes[sx, sy, mask]
        for row in range(half["rows"]):
            for column in range(half["columns"]):
                tile_id = half["tiles"][row * half["columns"] + column]
                pen_bytes = bytes(p & mask for p in pixels[tile_id])
                candidates = sorted(index.get(pen_bytes, ()))
                variants = [virtual_sources[key]["full_source_sha256"] for key in candidates]
                # Spatially expected source is separate from all pixel aliases.
                x, y = column * 16 * sx, row * 16 * sy
                full = source(normal, pixels, x, y, sx, sy, mask)
                spatial = {"origin_pixels": [x % normal["width_pixels"], y % normal["height_pixels"]],
                           "tiles": [normal["tiles"][((y // 16 + dy) % normal["rows"]) * normal["columns"] +
                                                      (x // 16 + dx) % normal["columns"]]
                                     for dy in range(sy) for dx in range(sx)],
                           "exact": sampled(full, sx, sy) == pen_bytes,
                           "full_source_sha256": hashlib.sha256(full).hexdigest()}
                cell = {"half_tile": tile_id, "half_cell": [column, row], "spatial_source": spatial,
                        "candidate_sources": candidates, "full_resolution_variants": variants,
                        "resolved_source": (f"{sx}:{sy}:{mask}:{spatial['full_source_sha256']}"
                                            if spatial["exact"] else candidates[0] if len(candidates) == 1 else None),
                        "requires_context": len(variants) > 1}
                cells.append(cell)
                aliases[sx, sy, mask, tile_id].append({"pair": pair["name"], "selection": pair["selection"],
                    "half_cell": [column, row], "source": spatial, "candidate_sources": candidates,
                    "resolved_source": cell["resolved_source"], "requires_context": len(variants) > 1})
        pair["cells"] = cells
    return {"schema_version": 1, "tool": "f3_prescaled_tile_asset_discovery", "game": config["game"],
            "config": str(config_path), "rom_directory": str(directory), "address_encoding": "integer_bytes",
            "images": {name: {"size": len(data), "sha256": hashlib.sha256(data).hexdigest()}
                       for name, data in (("rom", rom), ("tiles", low), ("tiles_hi", high))},
            "pairs": pairs, "maps": standalone,
            "virtual_sources": [virtual_sources[key] for key in sorted(virtual_sources)],
            "aliases": [{"scale_x": sx, "scale_y": sy, "pen_mask": mask, "half_tile": tile_id,
                         "contexts": contexts,
                         "spatial_full_resolution_variants": sorted({c["source"]["full_source_sha256"] for c in contexts
                                                                     if c["source"]["exact"]}),
                         "tile_id_alone_is_sufficient": all(c["candidate_sources"] for c in contexts) and
                            len({candidate for c in contexts for candidate in c["candidate_sources"]}) == 1}
                        for (sx, sy, mask, tile_id), contexts in sorted(aliases.items())],
            "sampling_contract": {"retain_half_map_geometry": True,
                "virtual_source_extent": "16*scale_x by 16*scale_y",
                "coordinate_order": "full_local=fractional_half_local*scale; floor only after multiplication",
                "source_selection": "pixel-exact spatial source or unique full-resolution alias variant; unresolved ambiguities remain explicit, never choose the first candidate",
                "hardware_alt_selection": "not inferred from asset correlation; honor source control, line upload gates and destination latches"}}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", required=True, type=Path)
    parser.add_argument("--rom-root", required=True, type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    text = json.dumps(analyze(args.config, args.rom_root), indent=2) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(text, encoding="utf-8")
    else:
        sys.stdout.write(text)


if __name__ == "__main__":
    main()
