"""Compare PCM16 stereo WAVs with one fixed latency correction, not time warping."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys


def compare_audio(reference_path: Path, candidate_path: Path, start: float = 18.0,
                  end: float | None = None, window: float = 2.0, max_lag: float = 0.25,
                  lag_arg: float | None = None) -> dict:
    try:
        import numpy as np
        from scipy import signal
        from scipy.io import wavfile
    except ImportError as exc:
        raise ImportError(
            "f3 compare audio requires numpy and scipy (analysis dependency group).\n"
            "Run with: uv run --group analysis f3 compare audio ..."
        ) from exc

    def load(path: Path):
        rate, samples = wavfile.read(path)
        if samples.dtype != np.int16 or samples.ndim != 2 or samples.shape[1] != 2:
            raise ValueError(f"{path}: expected PCM16 stereo WAV")
        return rate, samples.astype(np.float64)

    def compute_metrics(ref, cand):
        diff = cand - ref
        energy = np.sum(ref * ref, axis=0)
        cand_energy = np.sum(cand * cand, axis=0)
        error = np.sum(diff * diff, axis=0)
        correlation = []
        snr = []
        for ch in range(2):
            denom = float(np.sqrt(energy[ch] * cand_energy[ch]))
            correlation.append(float(np.dot(ref[:, ch], cand[:, ch]) / denom) if denom else None)
            snr.append(float(10 * np.log10(energy[ch] / error[ch])) if energy[ch] and error[ch] else None)
        return {
            "frames": len(ref),
            "reference_rms": np.sqrt(energy / len(ref)).tolist(),
            "candidate_rms": np.sqrt(cand_energy / len(ref)).tolist(),
            "error_rms": np.sqrt(error / len(ref)).tolist(),
            "maximum_error": np.max(np.abs(diff), axis=0).tolist(),
            "within_one_lsb_percent": (np.mean(np.abs(diff) <= 1, axis=0) * 100).tolist(),
            "correlation": correlation,
            "snr_db": snr,
        }

    rate, ref_samples = load(reference_path)
    cand_rate, orig_samples = load(candidate_path)
    cand_samples = signal.resample_poly(orig_samples, rate, cand_rate, axis=0)
    total_end = min(len(ref_samples), len(cand_samples))
    if end is not None:
        total_end = min(total_end, round(end * rate))
    start_samp = round(start * rate)
    window_samp = round(window * rate)
    if start_samp < 0 or window_samp < 1 or max_lag < 0 or total_end - start_samp < window_samp:
        raise ValueError("Need a nonnegative start/lag limit and one complete positive-duration window")

    if lag_arg is None:
        ref_win = ref_samples[start_samp:start_samp + window_samp]
        actual_win = cand_samples[start_samp:start_samp + window_samp]
        channel = int(np.argmax(np.sum(ref_win * ref_win, axis=0)))
        if not np.any(ref_win[:, channel]) or not np.any(actual_win[:, channel]):
            raise ValueError("Calibration window is silent; choose --start or an explicit --lag")
        corr = signal.correlate(actual_win[:, channel], ref_win[:, channel], method="fft")
        lags = signal.correlation_lags(window_samp, window_samp)
        allowed = np.flatnonzero(np.abs(lags) <= round(max_lag * rate))
        lag = int(lags[allowed[np.argmax(corr[allowed])]])
    else:
        lag = round(lag_arg * rate)

    start_samp = max(start_samp, -lag)
    total_end = min(total_end, len(cand_samples) - lag)
    if total_end <= start_samp:
        raise ValueError("No overlapping samples after applying latency")

    report = {
        "reference": str(reference_path),
        "candidate": str(candidate_path),
        "reference_rate": rate,
        "candidate_rate": cand_rate,
        "reference_peak": np.max(np.abs(ref_samples), axis=0).tolist(),
        "candidate_peak": np.max(np.abs(orig_samples), axis=0).tolist(),
        "lag_samples": lag,
        "lag_seconds": lag / rate,
        "start_seconds": start_samp / rate,
        "end_seconds": total_end / rate,
        "resampling": "scipy.resample_poly" if rate != cand_rate else "none",
        "aggregate": compute_metrics(ref_samples[start_samp:total_end],
                                     cand_samples[start_samp + lag:total_end + lag]),
        "windows": [],
    }
    for offset in range(start_samp, total_end, window_samp):
        stop = min(offset + window_samp, total_end)
        report["windows"].append({
            "start_seconds": offset / rate,
            **compute_metrics(ref_samples[offset:stop], cand_samples[offset + lag:stop + lag])
        })
    return report


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="f3 compare audio",
        description="Compare PCM16 stereo WAVs with one fixed latency correction."
    )
    parser.add_argument("reference", type=Path, help="Reference WAV file")
    parser.add_argument("candidate", type=Path, help="Candidate WAV file")
    parser.add_argument("--start", type=float, default=18.0)
    parser.add_argument("--end", type=float)
    parser.add_argument("--window", type=float, default=2.0)
    parser.add_argument("--max-lag", type=float, default=0.25)
    parser.add_argument("--lag", type=float, help="Explicit fixed candidate lag in seconds")
    parser.add_argument("--json", nargs="?", const=True, default=False,
                        help="Output results in JSON format (optional path to save)")
    args = parser.parse_args(argv)

    try:
        report = compare_audio(
            args.reference, args.candidate,
            start=args.start, end=args.end,
            window=args.window, max_lag=args.max_lag,
            lag_arg=args.lag,
        )
    except ImportError as err:
        print(f"f3 compare audio: error: {err}", file=sys.stderr)
        return 2
    except Exception as err:
        print(f"f3 compare audio: error: {err}", file=sys.stderr)
        return 2

    text = json.dumps(report, indent=2, allow_nan=False)
    if isinstance(args.json, str):
        Path(args.json).write_text(text + "\n")
    if args.json:
        print(text)
    else:
        lag = report["lag_samples"]
        rate = report["reference_rate"]
        print(f"Fixed lag: {lag} samples ({lag / rate * 1000:.6f} ms); negative = candidate earlier")
        print(json.dumps(report["aggregate"], indent=2, allow_nan=False))
    return 0


if __name__ == "__main__":
    sys.exit(main())
