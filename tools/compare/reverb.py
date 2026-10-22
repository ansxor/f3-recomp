"""Measure reverb and wetness of rendered WAVs against a PCB recording.

The PCB recording is the reference. Each render is aligned to it with one global lag
(render sample r sits at PCB sample r + lag), then compared on the shared song body.
Everything is evaluated on the PCB sample rate. Levels are dBFS on the float signal in
[-1, 1]; "mono" is the mean of the channels. Output is numbers, not a verdict.
"""

from __future__ import annotations

import argparse
import json
import math
import shutil
import subprocess
import sys
import tempfile
from fractions import Fraction
from pathlib import Path
from typing import Any, TypedDict

import numpy as np
from numpy.lib.stride_tricks import sliding_window_view
from scipy import signal
from scipy.io import wavfile

FRAME_S = 0.050
HOP_S = 0.010
BODY_FLOOR_DB = -50.0
END_TOLERANCE_DB = 20.0
TAIL_TIMES_S = (0.1, 0.2, 0.3, 0.5, 0.75, 1.0, 1.5)
TAIL_REF_S = 2.0
ONSET_BAND_HZ = (2000.0, 14000.0)
ONSET_MIN_SPACING_S = 0.120
ONSET_KEEP = 40
ONSET_BLOCK_S = 10.0
C50_BANDS_HZ = ((300.0, 2000.0), (2000.0, 6000.0), (6000.0, 14000.0))
C50_BAND_KEYS = ("c50_300_2k", "c50_2k_6k", "c50_6k_14k")
C50_EARLY_S = 0.050
C50_LATE_S = 0.300
MAX_LAG_S = 10.0
FINE_LAG_S = 0.005
MIN_OVERLAP_S = 5.0
DB_FLOOR = -200.0
MUSIC_ONSET_MIN_DB = -40.0
MUSIC_ONSET_AFTER_S = 1.0
MUSIC_ONSET_SUSTAIN_S = 0.5
CONFIDENCE_MIN_ENV_R = 0.6

TAIL_KEYS = tuple(f"tail_{t:.2f}s" for t in TAIL_TIMES_S)

TABLE_ROWS = (
    ("rms_db", "rms_db (dBFS)", 2),
    ("song_end_s", "song_end (s)", 3),
    *((f"tail_{t:.2f}s", f"tail_db@+{t:.2f}s", 2) for t in TAIL_TIMES_S),
    ("end_level_db", "end level (dB)", 2),
    ("tail_mae", "tail_mae (dB)", 2),
    ("tail_bias", "tail_bias (dB)", 2),
    ("c50_300_2k", "C50 300-2k (dB)", 2),
    ("c50_2k_6k", "C50 2k-6k (dB)", 2),
    ("c50_6k_14k", "C50 6k-14k (dB)", 2),
    ("onsets", "onsets used", 0),
    ("floor_db", "floor p10-p90 (dB)", 2),
    ("lr_corr", "L/R correlation", 4),
    ("side_mid_db", "side/mid (dB)", 2),
)

METRIC_HELP = """\
Metrics (every render row is shown next to the PCB value, plus render - PCB):

  rms_db       RMS level in dBFS over the aligned song body. The body is the PCB span from its
               first 50 ms frame above -50 dBFS to its song_end; renders are measured on the
               same PCB samples (mapped through the lag). Levels are NOT normalised, so a gain
               difference shows up here.
  song_end     Song end in PCB seconds. Each signal finds its own end: take the frames from its
               first to last 50 ms frame above -50 dBFS, the median frame level of that span,
               and the last frame within 20 dB of that median. Renders report their own end
               plus the lag.
  tail_db@+t   Level of a 50 ms window centred at song_end + t, relative to the mean 50 ms RMS
               over the 2 s before that signal's own song_end. t = 0.1 0.2 0.3 0.5 0.75 1.0 1.5 s.
               Each signal is normalised by its own reference, so this compares decay shape.
               EOF = the point lies past the end of that file, so there are no samples to measure.
  end level    Level of the file's last 0.5 s relative to the same 2 s reference as tail_db. About
               0 dB = still sounding at end of file (no decay); far below 0 = it has faded out.
  tail_mae     Mean absolute difference |render tail - PCB tail| over the t points (points where
               either side is EOF or past the PCB end are skipped).
  tail_bias    Mean signed difference (render tail - PCB tail). Positive = render decays louder
               (more tail).
  C50 band     Clarity, 10 log10(E[0,50ms] / E[50,300ms]) after each in-song onset, in the
               300-2k, 2k-6k and 6k-14k bands; reported as the median over onsets. Lower =
               more energy after the onset = wetter / looser. Positive = dry.
  onsets used  Number of onsets (from the PCB) inside the body and window that have room for
               300 ms after them, and that are valid for this render.
  floor_db     10th percentile minus 90th percentile of 50 ms frame dB in the body (gap-to-peak
               contrast). Reverb that fills the gaps between notes lifts the quiet frames toward
               the loud ones, so higher (closer to 0) = more fill; more negative = deeper gaps
               (drier). Digital silence (-200 dB clamp) inside the body counts as a gap.
  L/R corr     Pearson correlation of L and R over the body (symmetric under L/R swap).
  side/mid     10 log10(side power / mid power) over the body, side = (L-R)/2, mid = (L+R)/2
               (symmetric under L/R swap). Higher = wider.

Method:
  1. Resample each render to the PCB rate with scipy.signal.resample_poly (exact rational
     ratio, e.g. 29761 -> 29762 Hz).
  2. Coarse lag: cross-correlate 2-14 kHz band envelopes (20 ms RMS frames, 10 ms hop) over
     every lag in +/-max-lag (Pearson over the overlap, >= 5 s overlap). Only the PCB section
     from its music onset on is searched. The onset is the start of the first 0.5 s window
     (10 ms hop, from 1 s on) whose mean level is above -40 dBFS. Leading silence cannot match.
     alignment_confidence is "high" when the envelope r >= 0.6, else "low"; in that case
     the per-onset C50 rows are marked * in the table; the JSON carries alignment_confidence.
  3. Fine lag: maximise raw mono Pearson correlation over the full overlap, within +/-5 ms.
     The printed envelope r and raw r are the correlations at the chosen lags; check them.
  4. Onsets (PCB only): spectral flux of the 2-14 kHz STFT (512-sample Hann, 5 ms hop), log
     magnitude, positive differences summed over bins. Peaks at least 120 ms apart, keeping the
     strongest 40 per 10 s block. Onsets before --onset-skip seconds are dropped first.
  5. --window START:END limits metrics C50, floor, L/R and side/mid to that span in render
     time (each render's window is mapped through its own lag).

Notes:
  --swap-pcb swaps the PCB channels. Every reported metric is channel-symmetric (mono
  analysis, L/R correlation, side/mid power), so the flag changes only the per-channel levels
  in the JSON. Our renders already apply the DAC L/R swap; leave it off.
  Mono inputs are accepted; stereo metrics are reported as n/a unless both sides are stereo.
  FLAC input (and any non-WAV) is decoded with ffmpeg to a temporary WAV.
  The script is deterministic: no randomness, fixed parameters, argmax picks the first best lag.

Exit status: 0 = comparison ran; 2 = error (bad arguments or unreadable input)."""


def _db(power):
    return 10.0 * np.log10(np.maximum(power, 10.0 ** (DB_FLOOR / 10.0)))


def _pearson(a: np.ndarray, b: np.ndarray) -> float:
    a = a - a.mean()
    b = b - b.mean()
    denom = math.sqrt(float(np.dot(a, a)) * float(np.dot(b, b)))
    return float(np.dot(a, b)) / denom if denom > 0 else float("nan")


def _decode_with_ffmpeg(path: Path):
    ffmpeg = shutil.which("ffmpeg")
    if ffmpeg is None:
        raise RuntimeError(f"{path}: not a WAV file and ffmpeg is not on PATH to decode it")
    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp) / "decoded.wav"
        proc = subprocess.run(
            [ffmpeg, "-v", "error", "-nostdin", "-y", "-i", str(path), "-c:a", "pcm_f32le", str(out)],
            capture_output=True,
            text=True,
        )
        if proc.returncode != 0:
            raise RuntimeError(f"{path}: ffmpeg failed: {proc.stderr.strip()}")
        return wavfile.read(out)


def read_audio(path: Path) -> tuple[int, np.ndarray]:
    """Return (rate, float64 array shaped (frames, channels)) scaled to [-1, 1]."""
    if path.suffix.lower() == ".wav":
        rate, data = wavfile.read(path)
    else:
        rate, data = _decode_with_ffmpeg(path)
    if data.dtype == np.int16:
        x = data.astype(np.float64) / 32768.0
    elif data.dtype == np.int32:
        x = data.astype(np.float64) / 2147483648.0
    elif data.dtype == np.uint8:
        x = (data.astype(np.float64) - 128.0) / 128.0
    elif data.dtype in (np.float32, np.float64):
        x = data.astype(np.float64)
    else:
        raise ValueError(f"{path}: unsupported sample format {data.dtype}")
    if x.ndim == 1:
        x = x[:, None]
    channels = x.shape[-1]
    if channels not in (1, 2):
        raise ValueError(f"{path}: expected mono or stereo, got {channels} channels")
    return int(rate), x


def resample_to(x: np.ndarray, rate: int, target: int) -> np.ndarray:
    if rate == target:
        return x
    frac = Fraction(target, rate)
    return signal.resample_poly(x, frac.numerator, frac.denominator, axis=0)


def bandpass(x: np.ndarray, rate: int, lo: float, hi: float) -> np.ndarray:
    sos = signal.butter(4, [lo, hi], btype="bandpass", fs=rate, output="sos")
    return signal.sosfiltfilt(sos, x)


def frame_power(x: np.ndarray, rate: int, length_s: float, hop_s: float):
    """Mean-square per frame. Returns (power, window_samples, hop_samples)."""
    win = round(length_s * rate)
    hop = round(hop_s * rate)
    if len(x) < win:
        return np.empty(0, dtype=np.float64), win, hop
    frames = sliding_window_view(x, win)[::hop]
    return np.mean(frames * frames, axis=1), win, hop


def frame_db(x: np.ndarray, rate: int):
    """50 ms frames every 10 ms. Returns (start_samples, dB, window_samples); NaN where x is NaN."""
    power, win, hop = frame_power(x, rate, FRAME_S, HOP_S)
    return np.arange(len(power)) * hop, _db(power), win


class SongBounds(TypedDict):
    body_start: int
    song_end: int
    median_db: float


def song_bounds(mono: np.ndarray, rate: int) -> SongBounds:
    starts, db, win = frame_db(mono, rate)
    audible = np.flatnonzero(db > BODY_FLOOR_DB)
    if audible.size == 0:
        raise ValueError("signal never exceeds -50 dBFS; cannot find a song body")
    first, last = int(audible[0]), int(audible[-1])
    span = db[first : last + 1]
    median = float(np.median(span))
    near = np.flatnonzero(np.abs(span - median) <= END_TOLERANCE_DB)
    end_frame = first + (int(near[-1]) if near.size else last - first)
    return {
        "body_start": int(starts[first]),
        "song_end": int(starts[end_frame] + win),
        "median_db": median,
    }


def _tail_reference_db(x: np.ndarray, rate: int, end: int) -> float:
    """Mean 50 ms RMS (dB) over the TAIL_REF_S seconds before end."""
    seg = x[max(0, end - round(TAIL_REF_S * rate)) : end]
    power, _, _ = frame_power(seg, rate, FRAME_S, HOP_S)
    if power.size == 0:
        raise ValueError("song_end is too close to the start of the signal for a tail reference")
    return float(_db(np.mean(np.sqrt(power)) ** 2))


def tail_profile(x: np.ndarray, rate: int, end: int) -> list[float | None]:
    """Tail level (dB) at each TAIL_TIMES_S point, relative to the 2 s before end.

    None where the point lies past the end of the file: those samples do not exist, so there
    is nothing to measure there. end_level_db reports the level at the file end instead.
    """
    win = round(FRAME_S * rate)
    ref_db = _tail_reference_db(x, rate, end)
    out: list[float | None] = []
    for t in TAIL_TIMES_S:
        start = end + round(t * rate) - win // 2
        if start < 0 or start + win > len(x):
            out.append(None)
            continue
        piece = x[start : start + win]
        out.append(float(_db(np.mean(piece * piece))) - ref_db)
    return out


def end_level_db(x: np.ndarray, rate: int, end: int) -> float:
    """Level of the last 0.5 s of the file (dB) relative to the 2 s before end (as tail_profile)."""
    ref_db = _tail_reference_db(x, rate, end)
    piece = x[-round(0.5 * rate) :]
    return float(_db(np.mean(piece * piece))) - ref_db


def band_envelope(mono: np.ndarray, rate: int) -> np.ndarray:
    power, _, _ = frame_power(bandpass(mono, rate, *ONSET_BAND_HZ), rate, 0.020, HOP_S)
    return np.sqrt(power)


def coarse_lag(env_p: np.ndarray, env_r: np.ndarray, hop: int, max_frames: int, min_frames: int):
    """Lag in samples (PCB index = render index + lag) from envelope Pearson over the overlap."""
    best_lag, best = 0, -math.inf
    for lag in range(-max_frames, max_frames + 1):
        i0 = max(0, -lag)
        i1 = min(len(env_r), len(env_p) - lag)
        if i1 - i0 < min_frames:
            continue
        corr = _pearson(env_r[i0:i1], env_p[i0 + lag : i1 + lag])
        if corr > best:
            best, best_lag = corr, lag
    if best == -math.inf:
        raise ValueError("no lag gives enough envelope overlap; check the inputs")
    return best_lag * hop, best


def fine_lag(mono_p: np.ndarray, mono_r: np.ndarray, coarse: int, radius: int, min_samples: int):
    best_lag, best = coarse, -math.inf
    for lag in range(coarse - radius, coarse + radius + 1):
        j0 = max(0, lag)
        j1 = min(len(mono_p), lag + len(mono_r))
        if j1 - j0 < min_samples:
            continue
        corr = _pearson(mono_r[j0 - lag : j1 - lag], mono_p[j0:j1])
        if corr > best:
            best, best_lag = corr, lag
    if best == -math.inf:
        raise ValueError("no fine lag gives enough raw overlap; check the inputs")
    return best_lag, best


def align(x: np.ndarray, lag: int, n: int) -> np.ndarray:
    """Place x on the PCB timeline (x[r] -> index r + lag); NaN where x has no sample."""
    out = np.full((n, *x.shape[1:]), np.nan)
    j0, j1 = max(0, lag), min(n, lag + len(x))
    if j1 > j0:
        out[j0:j1] = x[j0 - lag : j1 - lag]
    return out


def detect_onsets(mono: np.ndarray, rate: int, skip_s: float) -> np.ndarray:
    band = bandpass(mono, rate, *ONSET_BAND_HZ)
    nperseg = 512
    hop = max(1, round(0.005 * rate))
    freqs, _, z = signal.stft(
        band,
        fs=rate,
        window="hann",
        nperseg=nperseg,
        noverlap=nperseg - hop,
        boundary=None,  # pyright: ignore[reportArgumentType]  # scipy stub says str; None = no padding (frame k centred at k*hop + nperseg/2)
        padded=False,
    )
    sel = (freqs >= ONSET_BAND_HZ[0]) & (freqs <= ONSET_BAND_HZ[1])
    logmag = np.log10(np.abs(z[sel]) + 1e-6)
    flux = np.zeros(logmag.shape[1])
    flux[1:] = np.maximum(np.diff(logmag, axis=1), 0.0).sum(axis=0)
    peaks, _ = signal.find_peaks(flux, distance=max(1, round(ONSET_MIN_SPACING_S * rate / hop)))
    samples = peaks * hop + nperseg // 2
    keep = samples >= round(skip_s * rate)
    peaks, samples = peaks[keep], samples[keep]
    block = round(ONSET_BLOCK_S * rate)
    chosen = []
    for b in np.unique(samples // block):
        idx = np.flatnonzero(samples // block == b)
        order = idx[np.argsort(-flux[peaks[idx]], kind="stable")][:ONSET_KEEP]
        chosen.extend(order.tolist())
    return np.sort(samples[np.array(chosen, dtype=np.int64)]).astype(np.int64)


def c50_db(band_x: np.ndarray, rate: int, onsets: np.ndarray) -> tuple[float, int]:
    """Median C50 (dB) over onsets; onsets must satisfy 0 <= onset and onset + late <= len."""
    early = round(C50_EARLY_S * rate)
    late = round(C50_LATE_S * rate)
    cs = np.concatenate(([0.0], np.cumsum(band_x * band_x)))
    e0 = cs[onsets + early] - cs[onsets]
    e1 = cs[onsets + late] - cs[onsets + early]
    ok = (e0 > 0) & (e1 > 0)
    if not ok.any():
        return float("nan"), 0
    return float(np.median(10.0 * np.log10(e0[ok] / e1[ok]))), int(ok.sum())


def floor_db(x: np.ndarray, rate: int) -> float:
    """p10 minus p90 of frame dB (gap-to-peak contrast); NaN samples are excluded."""
    _, db, _ = frame_db(x, rate)
    db = db[np.isfinite(db)]
    if db.size == 0:
        return float("nan")
    return float(np.percentile(db, 10) - np.percentile(db, 90))


def signal_metrics(mono: np.ndarray, lr: np.ndarray | None, mask: np.ndarray, rate: int) -> dict[str, float]:
    """Level, floor and stereo metrics over the samples where mask is True."""
    if not mask.any():
        raise ValueError("no overlap between the PCB song body and the render")
    masked = np.where(mask, mono, np.nan)
    x = masked[mask]
    out = {"rms_db": float(_db(np.mean(x * x))), "floor_db": floor_db(masked, rate)}
    if lr is None:
        out.update(lr_corr=float("nan"), side_mid_db=float("nan"), rms_db_left=float("nan"), rms_db_right=float("nan"))
    else:
        left, right = lr[mask, 0], lr[mask, 1]
        mid = (left + right) / 2.0
        side = (left - right) / 2.0
        mid_energy = float(np.sum(mid * mid))
        side_mid = float(_db(np.sum(side * side) / mid_energy)) if mid_energy > 0 else float("nan")
        out.update(
            lr_corr=_pearson(left, right),
            side_mid_db=side_mid,
            rms_db_left=float(_db(np.mean(left * left))),
            rms_db_right=float(_db(np.mean(right * right))),
        )
    return out


def _jsonable(value):
    if isinstance(value, dict):
        return {k: _jsonable(v) for k, v in value.items()}
    if isinstance(value, (list, tuple)):
        return [_jsonable(v) for v in value]
    if isinstance(value, (np.floating, float)):
        return None if not math.isfinite(float(value)) else float(value)
    if isinstance(value, np.integer):
        return int(value)
    return value


def music_onset(mono: np.ndarray, rate: int) -> int:
    """First sample of the first MUSIC_ONSET_SUSTAIN_S window whose mean level exceeds MUSIC_ONSET_MIN_DB.

    Searched from MUSIC_ONSET_AFTER_S on, so a short blip at the start is skipped. The window
    uses mean power, not every frame, so sparse notes or clicks with gaps still count as music.
    """
    power, _, hop = frame_power(mono, rate, MUSIC_ONSET_SUSTAIN_S, HOP_S)
    db = _db(power)
    after = round(MUSIC_ONSET_AFTER_S / HOP_S)
    hits = np.flatnonzero(db[after:] > MUSIC_ONSET_MIN_DB)
    if hits.size == 0:
        raise ValueError(
            f"no {MUSIC_ONSET_SUSTAIN_S} s window above {MUSIC_ONSET_MIN_DB} dBFS after {MUSIC_ONSET_AFTER_S} s"
        )
    return int((after + int(hits[0])) * hop)


def compare_reverb(
    pcb_path: Path,
    renders: list[tuple[str, Path]],
    *,
    swap_pcb: bool = False,
    onset_skip: float = 0.5,
    window: tuple[float, float] | None = None,
    max_lag: float = MAX_LAG_S,
) -> dict[str, Any]:
    rate, pcb = read_audio(pcb_path)
    pcb_stereo = pcb.shape[1] == 2
    if swap_pcb and pcb_stereo:
        pcb = pcb[:, ::-1]
    mono_p = pcb.mean(axis=1)
    n_p = len(mono_p)
    bounds_p = song_bounds(mono_p, rate)
    body_start, body_end = bounds_p["body_start"], bounds_p["song_end"]
    onsets = detect_onsets(mono_p, rate, onset_skip)
    env_p = band_envelope(mono_p, rate)
    tail_p = tail_profile(mono_p, rate, body_end)
    band_p = [bandpass(mono_p, rate, lo, hi) for lo, hi in C50_BANDS_HZ]
    late = round(C50_LATE_S * rate)
    j = np.arange(n_p)
    body = (j >= body_start) & (j < body_end)
    pcb_onset = music_onset(mono_p, rate)
    end_p = end_level_db(mono_p, rate, body_end)

    report: dict[str, Any] = {
        "pcb": {
            "path": str(pcb_path),
            "rate": rate,
            "channels": int(pcb.shape[1]),
            "body_start_s": body_start / rate,
            "music_onset_s": pcb_onset / rate,
            "song_end_s": body_end / rate,
            "body_median_db": bounds_p["median_db"],
            "onsets_detected": len(onsets),
        },
        "params": {
            "swap_pcb": swap_pcb,
            "onset_skip_s": onset_skip,
            "window_render_s": list(window) if window else None,
            "max_lag_s": max_lag,
            "frame_s": FRAME_S,
            "hop_s": HOP_S,
            "body_floor_db": BODY_FLOOR_DB,
            "end_tolerance_db": END_TOLERANCE_DB,
            "c50_bands_hz": [list(b) for b in C50_BANDS_HZ],
            "c50_windows_s": [C50_EARLY_S, C50_LATE_S],
            "onset_band_hz": list(ONSET_BAND_HZ),
            "onset_min_spacing_s": ONSET_MIN_SPACING_S,
            "onset_keep_per_10s": ONSET_KEEP,
            "tail_times_s": list(TAIL_TIMES_S),
        },
        "renders": {},
    }

    for name, path in renders:
        rate_r, data = read_audio(path)
        data = resample_to(data, rate_r, rate)
        mono_r = data.mean(axis=1)
        render_stereo = data.shape[1] == 2
        env_r = band_envelope(mono_r, rate)
        hop = round(HOP_S * rate)
        # Search only the PCB section from its music onset on; the silence before it cannot match.
        first_frame = pcb_onset // hop
        coarse_sub, env_corr = coarse_lag(
            env_p[first_frame:], env_r, hop, round(max_lag / HOP_S), round(MIN_OVERLAP_S / HOP_S)
        )
        coarse = coarse_sub + first_frame * hop
        confidence = "high" if env_corr >= CONFIDENCE_MIN_ENV_R else "low"
        lag, raw_corr = fine_lag(mono_p, mono_r, coarse, round(FINE_LAG_S * rate), round(MIN_OVERLAP_S * rate))

        aligned_mono = align(mono_r, lag, n_p)
        both_stereo = pcb_stereo and render_stereo
        aligned_lr = align(data, lag, n_p) if both_stereo else None
        pcb_lr = pcb if both_stereo else None

        r_idx = j - lag
        if window is None:
            win = np.ones(n_p, dtype=bool)
        else:
            win = (r_idx >= round(window[0] * rate)) & (r_idx < round(window[1] * rate))
        mask = body & win & np.isfinite(aligned_mono)
        coverage = float(mask.sum() / max(1, (body & win).sum()))

        pcb_level = signal_metrics(mono_p, pcb_lr, mask, rate)
        ren_level = signal_metrics(aligned_mono, aligned_lr, mask, rate)

        on = onsets[(onsets >= body_start) & (onsets < body_end)]
        on = on[win[on] & (on + late < n_p)]
        rr = on - lag
        ok = (rr >= 0) & (rr + late < len(mono_r))
        on_p, on_r = on[ok], rr[ok]
        band_r = [bandpass(mono_r, rate, lo, hi) for lo, hi in C50_BANDS_HZ]

        bounds_r = song_bounds(mono_r, rate)
        tail_r = tail_profile(mono_r, rate, bounds_r["song_end"])
        end_r = end_level_db(mono_r, rate, bounds_r["song_end"])
        pairs: list[tuple[float, float]] = []
        for r, p in zip(tail_r, tail_p, strict=True):
            if r is not None and p is not None and math.isfinite(r) and math.isfinite(p):
                pairs.append((r, p))
        tail_mae = float(np.mean([abs(r - p) for r, p in pairs])) if pairs else float("nan")
        tail_bias = float(np.mean([r - p for r, p in pairs])) if pairs else float("nan")

        pcb_metrics = {
            **pcb_level,
            "song_end_s": body_end / rate,
            "onsets": len(on_p),
            **{f"tail_{t:.2f}s": v for t, v in zip(TAIL_TIMES_S, tail_p, strict=True)},
            "end_level_db": end_p,
            "tail_mae": None,
            "tail_bias": None,
        }
        render_metrics = {
            **ren_level,
            "song_end_s": (bounds_r["song_end"] + lag) / rate,
            "onsets": len(on_r),
            **{f"tail_{t:.2f}s": v for t, v in zip(TAIL_TIMES_S, tail_r, strict=True)},
            "end_level_db": end_r,
            "tail_mae": tail_mae,
            "tail_bias": tail_bias,
        }
        for key, bx_p, bx_r in zip(C50_BAND_KEYS, band_p, band_r, strict=True):
            pcb_metrics[key] = c50_db(bx_p, rate, on_p)[0]
            render_metrics[key] = c50_db(bx_r, rate, on_r)[0]

        delta = {}
        for key, value in render_metrics.items():
            base = pcb_metrics.get(key)
            if value is None or base is None:
                delta[key] = None
            else:
                delta[key] = float(value) - float(base)

        report["renders"][name] = {
            "path": str(path),
            "render_rate": rate_r,
            "lag_samples": int(lag),
            "lag_seconds": lag / rate,
            "envelope_corr": env_corr,
            "alignment_confidence": confidence,
            "raw_corr": raw_corr,
            "coverage": coverage,
            "pcb": pcb_metrics,
            "render": render_metrics,
            "delta": delta,
        }
    return report


def format_table(report: dict[str, Any]) -> str:
    pcb = report["pcb"]
    lines = [
        f"PCB {pcb['path']}: {pcb['rate']} Hz, {pcb['channels']} ch, body "
        f"{pcb['body_start_s']:.3f}-{pcb['song_end_s']:.3f} s, onsets detected {pcb['onsets_detected']}",
    ]
    names = list(report["renders"])
    for name in names:
        r = report["renders"][name]
        lines.append(
            f"{name}: lag {r['lag_seconds']:+.6f} s ({r['lag_samples']} samples; PCB index = render index + lag), "
            f"envelope r={r['envelope_corr']:.4f}, raw r={r['raw_corr']:.4f}, "
            f"body coverage {100 * r['coverage']:.1f}%"
        )
        if r["alignment_confidence"] == "low":
            lines.append(
                f"  {name}: alignment LOW confidence (envelope r < {CONFIDENCE_MIN_ENV_R}); "
                "per-onset C50 rows are marked *"
            )
    if report["params"]["window_render_s"] is not None:
        lines.append("window (render time): " + ":".join(str(v) for v in report["params"]["window_render_s"]))
    lines.append("")

    def cell(value, decimals, signed=False, past_eof=False):
        if value is None:
            return "EOF" if past_eof else "-"
        if isinstance(value, float) and not math.isfinite(value):
            return "n/a"
        spec = f"{'+' if signed else ''}.{decimals}f"
        return format(value, spec)

    low = [name for name in names if report["renders"][name]["alignment_confidence"] == "low"]
    width = 12
    header = f"{'metric':<20}{'PCB':>{width}}"
    for name in names:
        header += f"{name:>{width}}{'diff':>{width}}"
    lines.append(header)
    lines.append("-" * len(header))
    first = report["renders"][names[0]]["pcb"]
    for key, label, decimals in TABLE_ROWS:
        past_eof = key in TAIL_KEYS
        if key in C50_BAND_KEYS and low:
            label = f"{label}*"
        row = f"{label:<20}{cell(first.get(key), decimals, past_eof=past_eof):>{width}}"
        for name in names:
            r = report["renders"][name]
            row += (
                f"{cell(r['render'].get(key), decimals, past_eof=past_eof):>{width}}"
                f"{cell(r['delta'].get(key), decimals, True):>{width}}"
            )
        lines.append(row)
    lines.append("")
    lines.append("diff = render - PCB. PCB column uses the first render's sample set (the body, plus window if")
    lines.append("given, and onsets valid for that render); per-render PCB values are in --json.")
    lines.append("EOF = tail point lies past the end of that file (no samples to measure).")
    if low:
        lines.append(
            f"* per-onset C50 rows are low-confidence for: {', '.join(low)} (envelope r < {CONFIDENCE_MIN_ENV_R})."
        )
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="f3 compare reverb",
        description="Compare reverb and wetness of rendered WAVs against a PCB recording.",
        epilog=METRIC_HELP,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--pcb", required=True, type=Path, help="PCB reference recording (WAV or FLAC)")
    parser.add_argument(
        "--render",
        action="append",
        required=True,
        metavar="NAME=PATH",
        help="Rendered WAV to compare, named for the table; repeat for several",
    )
    parser.add_argument(
        "--swap-pcb",
        action="store_true",
        help="Swap PCB L/R channels before analysis (off by default; reported metrics are channel-symmetric)",
    )
    parser.add_argument(
        "--onset-skip",
        type=float,
        default=0.5,
        metavar="SECONDS",
        help="Ignore PCB onsets before this time from the file start (default 0.5; skips the start blip)",
    )
    parser.add_argument(
        "--window",
        metavar="START:END",
        help="Restrict C50, floor, L/R and side/mid to this span in render seconds",
    )
    parser.add_argument(
        "--max-lag",
        type=float,
        default=MAX_LAG_S,
        metavar="SECONDS",
        help="Largest coarse lag searched in either direction (default 10)",
    )
    parser.add_argument("--json", type=Path, metavar="PATH", help="Also write all numbers as JSON to PATH")
    args = parser.parse_args(argv)

    renders = []
    for spec in args.render:
        name, sep, path = spec.partition("=")
        if not sep or not name or not path:
            parser.error(f"--render expects NAME=PATH, got {spec!r}")
        if name in (n for n, _ in renders):
            parser.error(f"duplicate render name {name!r}")
        renders.append((name, Path(path)))

    window = None
    if args.window is not None:
        try:
            start_s, end_s = (float(v) for v in args.window.split(":"))
        except ValueError:
            parser.error(f"--window expects START:END in seconds, got {args.window!r}")
        if not (0 <= start_s < end_s):
            parser.error("--window needs 0 <= START < END")
        window = (start_s, end_s)

    try:
        report = compare_reverb(
            args.pcb,
            renders,
            swap_pcb=args.swap_pcb,
            onset_skip=args.onset_skip,
            window=window,
            max_lag=args.max_lag,
        )
    except (OSError, ValueError, RuntimeError) as err:
        print(f"f3 compare reverb: error: {err}", file=sys.stderr)
        return 2

    print(format_table(report))
    if args.json is not None:
        args.json.write_text(json.dumps(_jsonable(report), indent=2, allow_nan=False) + "\n")
        print(f"\nWrote {args.json}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
