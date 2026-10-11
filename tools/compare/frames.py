"""Pixel comparator and frame diff tool for f3rt vs MAME frames."""
from __future__ import annotations

import argparse
import json
import math
import os
from pathlib import Path
import struct
import sys


def parse_raw_buffer(data: bytes, width: int, height: int, pixel_format: str = "bgra") -> bytes:
    total_pixels = width * height
    fmt = pixel_format.lower()
    if fmt in ("bgra", "argb", "rgba"):
        expected_size = total_pixels * 4
        if len(data) < expected_size:
            raise ValueError(f"Buffer size {len(data)} too small for {width}x{height} 32-bit ({expected_size} bytes)")
    elif fmt in ("rgb", "bgr"):
        expected_size = total_pixels * 3
        if len(data) < expected_size:
            raise ValueError(f"Buffer size {len(data)} too small for {width}x{height} 24-bit ({expected_size} bytes)")
    else:
        raise ValueError(f"Unknown pixel format: {pixel_format}")

    rgb_out = bytearray(total_pixels * 3)
    if fmt == "bgra":
        for i in range(total_pixels):
            src = i * 4
            dst = i * 3
            rgb_out[dst] = data[src + 2]
            rgb_out[dst + 1] = data[src + 1]
            rgb_out[dst + 2] = data[src]
    elif fmt == "argb":
        for i in range(total_pixels):
            src = i * 4
            dst = i * 3
            rgb_out[dst] = data[src + 1]
            rgb_out[dst + 1] = data[src + 2]
            rgb_out[dst + 2] = data[src + 3]
    elif fmt == "rgba":
        for i in range(total_pixels):
            src = i * 4
            dst = i * 3
            rgb_out[dst] = data[src]
            rgb_out[dst + 1] = data[src + 1]
            rgb_out[dst + 2] = data[src + 2]
    elif fmt == "rgb":
        rgb_out[:] = data[:total_pixels * 3]
    elif fmt == "bgr":
        for i in range(total_pixels):
            src = i * 3
            dst = i * 3
            rgb_out[dst] = data[src + 2]
            rgb_out[dst + 1] = data[src + 1]
            rgb_out[dst + 2] = data[src]
    return bytes(rgb_out)


def parse_bmp(data: bytes) -> tuple[bytes, int, int]:
    if len(data) < 54 or data[:2] != b"BM":
        raise ValueError("Not a valid BMP file (missing BM magic)")
    file_size, pixel_offset = struct.unpack_from("<II", data, 2 + 8)
    header_size, width, height, planes, bpp, compression = struct.unpack_from("<IiiHHI", data, 14)
    if planes != 1 or compression not in (0, 3) or bpp not in (24, 32):
        raise ValueError(f"Unsupported BMP format: {bpp}bpp, compression {compression}")

    is_top_down = height < 0
    height = abs(height)
    row_bytes = (width * (bpp // 8) + 3) & ~3
    total_pixels = width * height
    rgb_out = bytearray(total_pixels * 3)

    for y in range(height):
        src_y = y if is_top_down else (height - 1 - y)
        row_offset = pixel_offset + src_y * row_bytes
        for x in range(width):
            src_px = row_offset + x * (bpp // 8)
            dst_px = (y * width + x) * 3
            b, g, r = data[src_px], data[src_px + 1], data[src_px + 2]
            rgb_out[dst_px] = r
            rgb_out[dst_px + 1] = g
            rgb_out[dst_px + 2] = b
    return bytes(rgb_out), width, height


def write_bmp(path: str | Path, rgb_data: bytes, width: int, height: int) -> None:
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    row_bytes = width * 3
    pad = (4 - (row_bytes % 4)) % 4
    padded_row_size = row_bytes + pad
    image_size = padded_row_size * height
    file_size = 54 + image_size

    file_header = struct.pack("<2sIHHI", b"BM", file_size, 0, 0, 54)
    info_header = struct.pack("<IIIHHIIIIII", 40, width, height, 1, 24, 0, image_size, 2835, 2835, 0, 0)

    out = bytearray()
    out.extend(file_header)
    out.extend(info_header)
    pad_bytes = b"\x00" * pad

    for y in reversed(range(height)):
        row_offset = y * width * 3
        row = rgb_data[row_offset:row_offset + row_bytes]
        bgr_row = bytearray(row_bytes)
        for x in range(width):
            r = row[x * 3]
            g = row[x * 3 + 1]
            b = row[x * 3 + 2]
            bgr_row[x * 3] = b
            bgr_row[x * 3 + 1] = g
            bgr_row[x * 3 + 2] = r
        out.extend(bgr_row)
        if pad:
            out.extend(pad_bytes)

    path.write_bytes(out)


def load_image(filepath: str | Path, default_width: int = 320, default_height: int = 232,
               pixel_format: str = "bgra") -> tuple[bytes, int, int]:
    path = Path(filepath)
    ext = path.suffix.lower()
    data = path.read_bytes()
    if ext == ".bmp" or data[:2] == b"BM":
        return parse_bmp(data)
    elif ext in (".argb", ".bgra", ".rgba", ".raw", ".bin"):
        fmt = ext[1:] if ext in (".argb", ".bgra", ".rgba") else pixel_format
        return parse_raw_buffer(data, default_width, default_height, fmt), default_width, default_height
    else:
        try:
            return parse_bmp(data)
        except Exception:
            return parse_raw_buffer(data, default_width, default_height, pixel_format), default_width, default_height


def compare_buffers(ref: bytes, actual: bytes, width: int, height: int, tolerance: int = 0,
                    amp: int = 8, make_diff: bool = False) -> tuple[dict, bytes | None]:
    if len(ref) != len(actual):
        raise ValueError(f"Buffer length mismatch: {len(ref)} vs {len(actual)}")

    total_pixels = width * height
    exact_matches = 0
    tolerance_matches = 0
    mismatches = 0
    max_err = 0
    max_r = max_g = max_b = 0
    sum_r = sum_g = sum_b = 0
    sum_sq_err = 0

    diff_rgb = bytearray(total_pixels * 3) if make_diff else None
    error_histogram = {"0": 0, "1-3": 0, "4-15": 0, "16-63": 0, "64+": 0}

    for i in range(total_pixels):
        offset = i * 3
        dr = abs(ref[offset] - actual[offset])
        dg = abs(ref[offset + 1] - actual[offset + 1])
        db = abs(ref[offset + 2] - actual[offset + 2])
        pixel_max = max(dr, dg, db)

        if pixel_max == 0:
            exact_matches += 1
            error_histogram["0"] += 1
        elif pixel_max <= 3:
            error_histogram["1-3"] += 1
        elif pixel_max <= 15:
            error_histogram["4-15"] += 1
        elif pixel_max <= 63:
            error_histogram["16-63"] += 1
        else:
            error_histogram["64+"] += 1

        if pixel_max <= tolerance:
            tolerance_matches += 1
        else:
            mismatches += 1

        if pixel_max > max_err:
            max_err = pixel_max
        if dr > max_r:
            max_r = dr
        if dg > max_g:
            max_g = dg
        if db > max_b:
            max_b = db

        sum_r += dr
        sum_g += dg
        sum_b += db
        sum_sq_err += dr * dr + dg * dg + db * db

        if make_diff and diff_rgb is not None:
            if pixel_max == 0:
                diff_rgb[offset] = 0
                diff_rgb[offset + 1] = 0
                diff_rgb[offset + 2] = 0
            else:
                diff_rgb[offset] = min(255, dr * amp)
                diff_rgb[offset + 1] = min(255, dg * amp)
                diff_rgb[offset + 2] = min(255, db * amp)

    total_channels = total_pixels * 3
    mse = sum_sq_err / total_channels
    rmse = math.sqrt(mse)
    mae = (sum_r + sum_g + sum_b) / total_channels
    psnr = (20 * math.log10(255.0 / rmse)) if rmse > 0 else 999.0

    metrics = {
        "width": width,
        "height": height,
        "total_pixels": total_pixels,
        "exact_matches": exact_matches,
        "exact_pct": round(exact_matches / total_pixels * 100, 2),
        "tolerance": tolerance,
        "tolerance_matches": tolerance_matches,
        "tolerance_pct": round(tolerance_matches / total_pixels * 100, 2),
        "mismatches": mismatches,
        "mismatch_pct": round(mismatches / total_pixels * 100, 2),
        "max_error": max_err,
        "max_error_r": max_r,
        "max_error_g": max_g,
        "max_error_b": max_b,
        "mae": round(mae, 4),
        "mae_r": round(sum_r / total_pixels, 4),
        "mae_g": round(sum_g / total_pixels, 4),
        "mae_b": round(sum_b / total_pixels, 4),
        "rmse": round(rmse, 4),
        "psnr_db": round(psnr, 2),
        "error_distribution": error_histogram,
    }
    return metrics, bytes(diff_rgb) if diff_rgb else None


def format_text_report(metrics: dict) -> str:
    lines = [
        "=" * 60,
        f"  Total Pixels:        {metrics['total_pixels']:,} ({metrics['width']}x{metrics['height']})",
        f"  Exact Matches:       {metrics['exact_matches']:,} ({metrics['exact_pct']:.2f}%)",
        f"  Within Tolerance:    {metrics['tolerance_matches']:,} ({metrics['tolerance_pct']:.2f}%)",
        f"  Mismatches:          {metrics['mismatches']:,} ({metrics['mismatch_pct']:.2f}%)",
        f"  Max Channel Error:   {metrics['max_error']} (R:{metrics['max_error_r']}, G:{metrics['max_error_g']}, B:{metrics['max_error_b']})",
        f"  Mean Abs Error (MAE):{metrics['mae']} (R:{metrics['mae_r']}, G:{metrics['mae_g']}, B:{metrics['mae_b']})",
        f"  Root Mean Sq (RMSE): {metrics['rmse']}",
        f"  PSNR (dB):           {metrics['psnr_db']}",
        "  Error Distribution:  " + ", ".join(f"{k}: {v}" for k, v in metrics["error_distribution"].items()),
    ]
    if "diff_file" in metrics:
        lines.append(f"  Diff Image Saved:    {metrics['diff_file']}")
    lines.append("=" * 60)
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="f3 compare frames",
        description="Pixel comparator and frame diff tool for f3rt vs MAME frames.",
    )
    parser.add_argument("reference", help="Reference frame file or captured directory")
    parser.add_argument("actual", help="Rendered f3rt frame file or replay directory")
    parser.add_argument("--width", type=int, default=320, help="Default width for raw buffers (default: 320)")
    parser.add_argument("--height", type=int, default=232, help="Default height for raw buffers (default: 232)")
    parser.add_argument("--format", default="bgra", choices=["bgra", "argb", "rgba", "rgb", "bgr"],
                        help="Pixel byte order for raw buffers (default: bgra)")
    parser.add_argument("--tolerance", type=int, default=0, help="Allowed channel difference threshold (default: 0)")
    parser.add_argument("--max-error-allowed", type=int, default=0,
                        help="Max error tolerated before exiting with non-zero exit code (default: 0)")
    parser.add_argument("--diff", help="Path to write diff BMP visualization")
    parser.add_argument("--diff-amp", type=int, default=8, help="Amplification factor for diff visualization")
    parser.add_argument("--json", action="store_true", help="Output results in JSON format")
    parser.add_argument("--quiet", action="store_true", help="Suppress text report, output return code only")

    args = parser.parse_args(argv)
    try:
        ref_bytes, rw, rh = load_image(args.reference, args.width, args.height, args.format)
        act_bytes, aw, ah = load_image(args.actual, args.width, args.height, args.format)
        if (rw, rh) != (aw, ah):
            raise ValueError(f"Dimension mismatch: reference {rw}x{rh} vs actual {aw}x{ah}")

        metrics, diff_bytes = compare_buffers(
            ref_bytes, act_bytes, rw, rh,
            tolerance=args.tolerance,
            amp=args.diff_amp,
            make_diff=bool(args.diff),
        )

        if args.diff and diff_bytes:
            write_bmp(args.diff, diff_bytes, rw, rh)
            metrics["diff_file"] = args.diff

        has_mismatch = (metrics["mismatches"] > 0 or metrics["max_error"] > args.max_error_allowed)

        if args.json:
            print(json.dumps(metrics, indent=2))
        elif not args.quiet:
            print(format_text_report(metrics))

        return 1 if has_mismatch else 0
    except Exception as e:
        if args.json:
            print(json.dumps({"error": str(e)}, indent=2))
        else:
            print(f"f3 compare frames: error: {e}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
