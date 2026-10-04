#!/usr/bin/env python3
"""Compare PCM16 stereo WAVs with one fixed latency correction, not time warping.

Requires NumPy and SciPy. Metrics are evidence, not an automatic parity verdict.
Negative lag means the candidate plays earlier than the reference.
"""
import argparse
import json
from pathlib import Path

import numpy as np
from scipy import signal
from scipy.io import wavfile


def load(path):
    rate, samples = wavfile.read(path)
    if samples.dtype != np.int16 or samples.ndim != 2 or samples.shape[1] != 2:
        raise ValueError(f"{path}: expected PCM16 stereo WAV")
    return rate, samples.astype(np.float64)


def metrics(reference, candidate):
    difference = candidate - reference
    energy = np.sum(reference * reference, axis=0)
    actual_energy = np.sum(candidate * candidate, axis=0)
    error = np.sum(difference * difference, axis=0)
    correlation = []
    snr = []
    for channel in range(2):
        denominator = float(np.sqrt(energy[channel] * actual_energy[channel]))
        correlation.append(float(np.dot(reference[:, channel], candidate[:, channel]) / denominator)
                           if denominator else None)
        snr.append(float(10 * np.log10(energy[channel] / error[channel]))
                   if energy[channel] and error[channel] else None)
    return {
        "frames": len(reference),
        "reference_rms": np.sqrt(energy / len(reference)).tolist(),
        "candidate_rms": np.sqrt(actual_energy / len(reference)).tolist(),
        "error_rms": np.sqrt(error / len(reference)).tolist(),
        "maximum_error": np.max(np.abs(difference), axis=0).tolist(),
        "within_one_lsb_percent": (np.mean(np.abs(difference) <= 1, axis=0) * 100).tolist(),
        "correlation": correlation,
        "snr_db": snr,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reference", type=Path)
    parser.add_argument("candidate", type=Path)
    parser.add_argument("--start", type=float, default=18.0)
    parser.add_argument("--end", type=float)
    parser.add_argument("--window", type=float, default=2.0)
    parser.add_argument("--max-lag", type=float, default=0.25)
    parser.add_argument("--lag", type=float, help="Explicit fixed candidate lag in seconds")
    parser.add_argument("--json", type=Path)
    args = parser.parse_args()
    rate, reference = load(args.reference)
    candidate_rate, original = load(args.candidate)
    candidate = signal.resample_poly(original, rate, candidate_rate, axis=0)
    end = min(len(reference), len(candidate))
    if args.end is not None:
        end = min(end, round(args.end * rate))
    start = round(args.start * rate)
    window = round(args.window * rate)
    if start < 0 or window < 1 or args.max_lag < 0 or end - start < window:
        raise ValueError("Need a nonnegative start/lag limit and one complete positive-duration window")
    if args.lag is None:
        ref = reference[start:start + window]
        actual = candidate[start:start + window]
        channel = int(np.argmax(np.sum(ref * ref, axis=0)))
        if not np.any(ref[:, channel]) or not np.any(actual[:, channel]):
            raise ValueError("Calibration window is silent; choose --start or an explicit --lag")
        correlation = signal.correlate(actual[:, channel], ref[:, channel], method="fft")
        lags = signal.correlation_lags(window, window)
        allowed = np.flatnonzero(np.abs(lags) <= round(args.max_lag * rate))
        lag = int(lags[allowed[np.argmax(correlation[allowed])]])
    else:
        lag = round(args.lag * rate)
    start = max(start, -lag)
    end = min(end, len(candidate) - lag)
    if end <= start:
        raise ValueError("No overlapping samples after applying latency")
    report = {
        "reference": str(args.reference), "candidate": str(args.candidate),
        "reference_rate": rate, "candidate_rate": candidate_rate,
        "reference_peak": np.max(np.abs(reference), axis=0).tolist(),
        "candidate_peak": np.max(np.abs(original), axis=0).tolist(),
        "lag_samples": lag, "lag_seconds": lag / rate,
        "start_seconds": start / rate, "end_seconds": end / rate,
        "resampling": "scipy.resample_poly" if rate != candidate_rate else "none",
        "aggregate": metrics(reference[start:end], candidate[start + lag:end + lag]),
        "windows": [],
    }
    for offset in range(start, end, window):
        stop = min(offset + window, end)
        report["windows"].append({"start_seconds": offset / rate,
            **metrics(reference[offset:stop], candidate[offset + lag:stop + lag])})
    print(f"Fixed lag: {lag} samples ({lag / rate * 1000:.6f} ms); negative = candidate earlier")
    print(json.dumps(report["aggregate"], indent=2, allow_nan=False))
    if args.json:
        args.json.write_text(json.dumps(report, indent=2, allow_nan=False) + "\n")


if __name__ == "__main__":
    main()
