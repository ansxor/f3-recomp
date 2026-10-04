#!/usr/bin/env python3
"""
tools/compare_frames.py - Pixel comparator and frame diff tool for f3rt vs MAME.

Features:
- Pure Python standard library: zero external dependencies (no numpy, no PIL).
- Formats: BMP (24-bit, 32-bit, top-down, bottom-up), raw buffers (.argb, .bgra, .rgba, .raw).
- Single frame comparison or full directory comparison.
- Metrics: exact match count/pct, max error, MAE, RMSE, PSNR (dB).
- Diff image generation: BMP highlighting pixel errors (amplified visualization).
- Machine-readable JSON output option for automated CI / regression testing.
"""

import argparse
import json
import math
import os
import struct
import sys


def parse_raw_buffer(data, width, height, pixel_format="bgra"):
    """
    Decodes raw byte buffer into top-down RGB bytes (3 bytes per pixel).
    pixel_format options:
      - bgra: MAME screen memory on little-endian (B, G, R, A)
      - argb: (A, R, G, B)
      - rgba: (R, G, B, A)
      - rgb:  (R, G, B)
      - bgr:  (B, G, R)
    """
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
            rgb_out[dst] = data[src + 2]      # R
            rgb_out[dst + 1] = data[src + 1]  # G
            rgb_out[dst + 2] = data[src]      # B
    elif fmt == "argb":
        for i in range(total_pixels):
            src = i * 4
            dst = i * 3
            rgb_out[dst] = data[src + 1]      # R
            rgb_out[dst + 1] = data[src + 2]  # G
            rgb_out[dst + 2] = data[src + 3]  # B
    elif fmt == "rgba":
        for i in range(total_pixels):
            src = i * 4
            dst = i * 3
            rgb_out[dst] = data[src]          # R
            rgb_out[dst + 1] = data[src + 1]  # G
            rgb_out[dst + 2] = data[src + 2]  # B
    elif fmt == "rgb":
        rgb_out[:] = data[:total_pixels * 3]
    elif fmt == "bgr":
        for i in range(total_pixels):
            src = i * 3
            dst = i * 3
            rgb_out[dst] = data[src + 2]      # R
            rgb_out[dst + 1] = data[src + 1]  # G
            rgb_out[dst + 2] = data[src]      # B

    return bytes(rgb_out)


def read_bmp(path):
    """
    Reads a 24-bit or 32-bit BMP file and returns (width, height, rgb_bytes).
    rgb_bytes is top-down (R, G, B) row by row.
    """
    with open(path, "rb") as f:
        data = f.read()

    if len(data) < 54 or data[:2] != b"BM":
        raise ValueError(f"{path}: Not a valid BMP file")

    offset = struct.unpack_from("<I", data, 10)[0]
    header_size = struct.unpack_from("<I", data, 14)[0]
    width, height = struct.unpack_from("<ii", data, 18)
    planes, bpp = struct.unpack_from("<HH", data, 26)
    compression = struct.unpack_from("<I", data, 30)[0]

    if compression not in (0, 3):
        raise ValueError(f"{path}: Unsupported BMP compression ({compression})")

    top_down = False
    if height < 0:
        top_down = True
        height = -height

    total_pixels = width * height
    rgb_out = bytearray(total_pixels * 3)
    row_bytes = width * (bpp // 8)
    row_stride = (row_bytes + 3) & ~3

    for y in range(height):
        src_y = y if top_down else (height - 1 - y)
        row_start = offset + src_y * row_stride
        row_data = data[row_start:row_start + row_bytes]
        dst_row_start = y * width * 3

        if bpp == 24:
            for x in range(width):
                b = row_data[x * 3]
                g = row_data[x * 3 + 1]
                r = row_data[x * 3 + 2]
                rgb_out[dst_row_start + x * 3] = r
                rgb_out[dst_row_start + x * 3 + 1] = g
                rgb_out[dst_row_start + x * 3 + 2] = b
        elif bpp == 32:
            for x in range(width):
                b = row_data[x * 4]
                g = row_data[x * 4 + 1]
                r = row_data[x * 4 + 2]
                rgb_out[dst_row_start + x * 3] = r
                rgb_out[dst_row_start + x * 3 + 1] = g
                rgb_out[dst_row_start + x * 3 + 2] = b
        else:
            raise ValueError(f"{path}: Unsupported bpp {bpp}")

    return width, height, bytes(rgb_out)


def write_bmp_24(path, width, height, rgb_bytes):
    """
    Writes a standard 24-bit bottom-up BMP file from top-down RGB bytes.
    """
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
        row = rgb_bytes[row_offset:row_offset + row_bytes]
        bgr_row = bytearray(row_bytes)
        for x in range(width):
            r = row[x * 3]
            g = row[x * 3 + 1]
            b = row[x * 3 + 2]
            bgr_row[x * 3] = b
            bgr_row[x * 3 + 1] = g
            bgr_row[x * 3 + 2] = r
        out.extend(bgr_row)
        out.extend(pad_bytes)

    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "wb") as f:
        f.write(out)


def load_image(path, default_width=320, default_height=232, raw_format="bgra"):
    """
    Loads an image from BMP or raw buffer.
    Returns (width, height, rgb_bytes).
    """
    if not os.path.exists(path):
        raise FileNotFoundError(f"File not found: {path}")

    ext = os.path.splitext(path)[1].lower()
    if ext == ".bmp":
        return read_bmp(path)

    # Raw buffer
    with open(path, "rb") as f:
        data = f.read()

    # Determine dimensions: check if matches default
    expected_32 = default_width * default_height * 4
    expected_24 = default_width * default_height * 3

    if len(data) == expected_32:
        return default_width, default_height, parse_raw_buffer(data, default_width, default_height, raw_format)
    elif len(data) == expected_24:
        fmt = "rgb" if raw_format not in ("bgr",) else "bgr"
        return default_width, default_height, parse_raw_buffer(data, default_width, default_height, fmt)
    else:
        # Fallback: check if matches 320x224
        if len(data) == 320 * 224 * 4:
            return 320, 224, parse_raw_buffer(data, 320, 224, raw_format)
        elif len(data) == 320 * 224 * 3:
            return 320, 224, parse_raw_buffer(data, 320, 224, "rgb")
        raise ValueError(
            f"{path}: Buffer length {len(data)} does not match {default_width}x{default_height} (32bpp: {expected_32}, 24bpp: {expected_24})"
        )


def compare_rgb(rgb_ref, rgb_actual, width, height, tolerance=0, diff_amplification=8):
    """
    Compares two RGB byte buffers.
    Returns (metrics_dict, diff_rgb_bytes).
    """
    total_pixels = width * height
    exact_matches = 0
    tolerance_matches = 0
    mismatches = 0

    max_err_r = 0
    max_err_g = 0
    max_err_b = 0
    max_err_overall = 0

    sum_abs_err_r = 0
    sum_abs_err_g = 0
    sum_abs_err_b = 0
    sum_sq_err = 0

    # Histogram of max channel errors: 0, 1-2, 3-7, 8-15, 16-31, 32+
    hist = {"0": 0, "1-2": 0, "3-7": 0, "8-15": 0, "16-31": 0, "32+": 0}

    diff_rgb = bytearray(total_pixels * 3)

    for i in range(total_pixels):
        idx = i * 3
        r1, g1, b1 = rgb_ref[idx], rgb_ref[idx + 1], rgb_ref[idx + 2]
        r2, g2, b2 = rgb_actual[idx], rgb_actual[idx + 1], rgb_actual[idx + 2]

        dr = abs(r1 - r2)
        dg = abs(g1 - g2)
        db = abs(b1 - b2)
        max_d = max(dr, dg, db)

        if max_d > max_err_overall:
            max_err_overall = max_d
        if dr > max_err_r:
            max_err_r = dr
        if dg > max_err_g:
            max_err_g = dg
        if db > max_err_b:
            max_err_b = db

        sum_abs_err_r += dr
        sum_abs_err_g += dg
        sum_abs_err_b += db
        sum_sq_err += (dr * dr + dg * dg + db * db)

        if max_d == 0:
            exact_matches += 1
            tolerance_matches += 1
            hist["0"] += 1
            # Diff visual: dimmed grayscale of original
            gray = (r1 * 30 + g1 * 59 + b1 * 11) // 400  # quarter brightness
            diff_rgb[idx] = gray
            diff_rgb[idx + 1] = gray
            diff_rgb[idx + 2] = gray
        else:
            if max_d <= tolerance:
                tolerance_matches += 1
            else:
                mismatches += 1

            if max_d <= 2:
                hist["1-2"] += 1
            elif max_d <= 7:
                hist["3-7"] += 1
            elif max_d <= 15:
                hist["8-15"] += 1
            elif max_d <= 31:
                hist["16-31"] += 1
            else:
                hist["32+"] += 1

            # Diff visual: amplified color difference highlighted in red/magenta
            amp = min(255, max_d * diff_amplification)
            diff_rgb[idx] = amp                          # Bright Red channel
            diff_rgb[idx + 1] = min(255, dg * diff_amplification // 2)
            diff_rgb[idx + 2] = min(255, db * diff_amplification)

    mse = sum_sq_err / (total_pixels * 3)
    rmse = math.sqrt(mse)
    mae_r = sum_abs_err_r / total_pixels
    mae_g = sum_abs_err_g / total_pixels
    mae_b = sum_abs_err_b / total_pixels
    mae_overall = (sum_abs_err_r + sum_abs_err_g + sum_abs_err_b) / (total_pixels * 3)

    if mse == 0:
        psnr = float("inf")
    else:
        psnr = 20.0 * math.log10(255.0 / math.sqrt(mse))

    metrics = {
        "width": width,
        "height": height,
        "total_pixels": total_pixels,
        "exact_matches": exact_matches,
        "exact_pct": (exact_matches / total_pixels) * 100.0,
        "tolerance_matches": tolerance_matches,
        "tolerance_pct": (tolerance_matches / total_pixels) * 100.0,
        "mismatches": mismatches,
        "mismatch_pct": (mismatches / total_pixels) * 100.0,
        "max_error": max_err_overall,
        "max_error_r": max_err_r,
        "max_error_g": max_err_g,
        "max_error_b": max_err_b,
        "mae": round(mae_overall, 4),
        "mae_r": round(mae_r, 4),
        "mae_g": round(mae_g, 4),
        "mae_b": round(mae_b, 4),
        "rmse": round(rmse, 4),
        "psnr_db": round(psnr, 2) if psnr != float("inf") else "inf",
        "error_distribution": hist,
    }

    return metrics, bytes(diff_rgb)


def compare_single_file(ref_path, actual_path, args):
    """Compares two single image or raw buffer files."""
    w1, h1, rgb1 = load_image(ref_path, args.width, args.height, args.format)
    w2, h2, rgb2 = load_image(actual_path, args.width, args.height, args.format)

    if (w1, h1) != (w2, h2):
        raise ValueError(f"Dimension mismatch: ref is {w1}x{h1}, actual is {w2}x{h2}")

    metrics, diff_rgb = compare_rgb(rgb1, rgb2, w1, h1, args.tolerance, args.diff_amp)

    if args.diff:
        write_bmp_24(args.diff, w1, h1, diff_rgb)
        metrics["diff_file"] = args.diff

    return metrics


def find_frame_files(directory):
    """
    Finds frames in a directory structure.
    Supports either:
    1. Subdirectories: frame_0001/reference.bmp (or reference.argb)
    2. Direct images: frame_0001.bmp or 0001.bmp
    Returns dict: { frame_number: path }
    """
    frames = {}
    if not os.path.exists(directory):
        return frames

    for entry in sorted(os.listdir(directory)):
        full = os.path.join(directory, entry)
        if os.path.isdir(full):
            # Check for reference.bmp or reference.argb or f3rt.bmp
            for candidate in ("reference.bmp", "f3rt.bmp", "rendered.bmp", "reference.argb", "frame.bmp"):
                cand_path = os.path.join(full, candidate)
                if os.path.isfile(cand_path):
                    # extract frame number
                    num = "".join(c for c in entry if c.isdigit())
                    if num:
                        frames[int(num)] = cand_path
                    break
        elif os.path.isfile(full):
            base, ext = os.path.splitext(entry)
            if ext.lower() in (".bmp", ".argb", ".raw", ".rgba"):
                num = "".join(c for c in base if c.isdigit())
                if num:
                    frames[int(num)] = full

    return frames


def compare_directories(ref_dir, actual_dir, args):
    """Compares matched frames across directories."""
    ref_frames = find_frame_files(ref_dir)
    actual_frames = find_frame_files(actual_dir)

    common_keys = sorted(set(ref_frames.keys()) & set(actual_frames.keys()))
    if not common_keys:
        raise ValueError(
            f"No matching frames found between {ref_dir} ({len(ref_frames)} frames) and {actual_dir} ({len(actual_frames)} frames)"
        )

    all_metrics = []
    overall_mismatches = 0
    max_error_all = 0

    if args.diff_dir:
        os.makedirs(args.diff_dir, exist_ok=True)

    for k in common_keys:
        ref_p = ref_frames[k]
        act_p = actual_frames[k]

        w1, h1, rgb1 = load_image(ref_p, args.width, args.height, args.format)
        w2, h2, rgb2 = load_image(act_p, args.width, args.height, args.format)

        if (w1, h1) != (w2, h2):
            print(f"Warning: Frame {k} dimension mismatch ({w1}x{h1} vs {w2}x{h2}), skipping.", file=sys.stderr)
            continue

        metrics, diff_rgb = compare_rgb(rgb1, rgb2, w1, h1, args.tolerance, args.diff_amp)
        metrics["frame"] = k
        metrics["ref_path"] = ref_p
        metrics["actual_path"] = act_p

        if metrics["mismatches"] > 0:
            overall_mismatches += metrics["mismatches"]
        if metrics["max_error"] > max_error_all:
            max_error_all = metrics["max_error"]

        if args.diff_dir and (metrics["mismatches"] > 0 or args.diff_all):
            diff_p = os.path.join(args.diff_dir, f"diff_{k:04d}.bmp")
            write_bmp_24(diff_p, w1, h1, diff_rgb)
            metrics["diff_file"] = diff_p

        all_metrics.append(metrics)

    summary = {
        "frames_compared": len(all_metrics),
        "total_mismatched_pixels": overall_mismatches,
        "max_error_across_frames": max_error_all,
        "frames": all_metrics,
    }
    return summary


def format_text_report(metrics):
    lines = []
    if "frames" in metrics:
        # Directory summary
        lines.append("=" * 64)
        lines.append(f"  Frame Comparison Summary ({metrics['frames_compared']} frames compared)")
        lines.append("=" * 64)
        lines.append(f"  Total mismatched pixels: {metrics['total_mismatched_pixels']}")
        lines.append(f"  Max error across all frames: {metrics['max_error_across_frames']}")
        lines.append("-" * 64)
        lines.append(f"{'Frame':<8} {'Matches':<16} {'Mismatch':<16} {'MaxErr':<8} {'MAE':<8} {'PSNR (dB)':<10}")
        lines.append("-" * 64)
        for m in metrics["frames"]:
            match_str = f"{m['exact_matches']} ({m['exact_pct']:.1f}%)"
            mism_str = f"{m['mismatches']} ({m['mismatch_pct']:.1f}%)"
            lines.append(
                f"{m['frame']:<8} {match_str:<16} {mism_str:<16} {m['max_error']:<8} {m['mae']:<8.2f} {m['psnr_db']:<10}"
            )
        lines.append("=" * 64)
    else:
        # Single frame
        lines.append("=" * 60)
        lines.append("  Frame Comparison Result")
        lines.append("=" * 60)
        lines.append(f"  Resolution:          {metrics['width']} x {metrics['height']} ({metrics['total_pixels']:,} pixels)")
        lines.append(f"  Exact Matches:       {metrics['exact_matches']:,} / {metrics['total_pixels']:,} ({metrics['exact_pct']:.2f}%)")
        lines.append(f"  Within Tolerance:    {metrics['tolerance_matches']:,} ({metrics['tolerance_pct']:.2f}%)")
        lines.append(f"  Mismatches:          {metrics['mismatches']:,} ({metrics['mismatch_pct']:.2f}%)")
        lines.append(f"  Max Channel Error:   {metrics['max_error']} (R:{metrics['max_error_r']}, G:{metrics['max_error_g']}, B:{metrics['max_error_b']})")
        lines.append(f"  Mean Abs Error (MAE):{metrics['mae']} (R:{metrics['mae_r']}, G:{metrics['mae_g']}, B:{metrics['mae_b']})")
        lines.append(f"  Root Mean Sq (RMSE): {metrics['rmse']}")
        lines.append(f"  PSNR (dB):           {metrics['psnr_db']}")
        lines.append("  Error Distribution:  " + ", ".join(f"{k}: {v}" for k, v in metrics["error_distribution"].items()))
        if "diff_file" in metrics:
            lines.append(f"  Diff Image Saved:    {metrics['diff_file']}")
        lines.append("=" * 60)

    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(
        description="Pixel comparator and frame diff tool for f3rt vs MAME Land Maker frames."
    )
    parser.add_argument("reference", help="Reference frame file or captured directory")
    parser.add_argument("actual", help="Rendered f3rt frame file or replay directory")
    parser.add_argument("--width", type=int, default=320, help="Default width for raw buffers (default: 320)")
    parser.add_argument("--height", type=int, default=232, help="Default height for raw buffers (default: 232)")
    parser.add_argument(
        "--format",
        default="bgra",
        choices=["bgra", "argb", "rgba", "rgb", "bgr"],
        help="Pixel byte order for raw buffers (default: bgra, MAME LE output)",
    )
    parser.add_argument("--tolerance", type=int, default=0, help="Allowed channel difference threshold (default: 0)")
    parser.add_argument(
        "--max-error-allowed",
        type=int,
        default=0,
        help="Max error tolerated before exiting with non-zero exit code (default: 0)",
    )
    parser.add_argument("--diff", help="Path to write diff BMP visualization (single frame mode)")
    parser.add_argument("--diff-dir", help="Directory to write diff BMPs (directory mode)")
    parser.add_argument("--diff-all", action="store_true", help="Write diff BMP for all frames, not just mismatches")
    parser.add_argument("--diff-amp", type=int, default=8, help="Amplification factor for diff visualization (default: 8)")
    parser.add_argument("--json", action="store_true", help="Output results in JSON format")
    parser.add_argument("--quiet", action="store_true", help="Suppress text report, output return code only")

    args = parser.parse_args()

    try:
        if os.path.isdir(args.reference) and os.path.isdir(args.actual):
            metrics = compare_directories(args.reference, args.actual, args)
            has_mismatch = (
                metrics["total_mismatched_pixels"] > 0
                or metrics["max_error_across_frames"] > args.max_error_allowed
            )
        else:
            metrics = compare_single_file(args.reference, args.actual, args)
            has_mismatch = (
                metrics["mismatches"] > 0
                or metrics["max_error"] > args.max_error_allowed
            )

        if args.json:
            print(json.dumps(metrics, indent=2))
        elif not args.quiet:
            print(format_text_report(metrics))

        sys.exit(1 if has_mismatch else 0)

    except Exception as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(2)


if __name__ == "__main__":
    main()
