from __future__ import annotations

import numpy as np
import pytest
from scipy import signal
from scipy.io import wavfile

from tools.compare import reverb

RATE = 29762


def _clicks(seconds: float, spacing: float, noise: float, seed: int) -> np.ndarray:
    rng = np.random.default_rng(seed)
    x = rng.normal(0.0, noise, round(seconds * RATE))
    for t in np.arange(0.5, seconds - 1.5, spacing):
        x[round(t * RATE)] += 0.5
    return x


def _onsets(seconds: float, spacing: float) -> np.ndarray:
    return np.array([round(t * RATE) for t in np.arange(0.5, seconds - 1.5, spacing)], dtype=np.int64)


def _wet(dry: np.ndarray, tau: float, seed: int) -> np.ndarray:
    rng = np.random.default_rng(seed)
    n = round(1.5 * RATE)
    t = np.arange(n) / RATE
    tail = rng.normal(0.0, 1.0, n) * np.exp(-t / tau)
    tail[0] = 1.0
    wet = signal.fftconvolve(dry, tail)[: len(dry)]
    return wet / np.max(np.abs(wet)) * 0.5


def test_wet_clicks_have_lower_c50_and_higher_floor():
    seconds, spacing = 8.0, 0.25
    dry = _clicks(seconds, spacing, noise=1e-3, seed=1)
    wet = _wet(dry, tau=0.3, seed=2)
    onsets = _onsets(seconds, spacing)

    c50_dry, n_dry = reverb.c50_db(dry, RATE, onsets)
    c50_wet, n_wet = reverb.c50_db(wet, RATE, onsets)
    assert n_dry == n_wet == len(onsets)
    assert c50_wet < c50_dry

    assert reverb.floor_db(wet, RATE) > reverb.floor_db(dry, RATE)


def test_identical_render_has_zero_deltas(tmp_path):
    seconds = 12.0  # needs >= 5 s left after the music onset for the restricted lag search
    x = _clicks(seconds, 0.25, noise=1e-4, seed=3)
    x = signal.fftconvolve(x, np.exp(-np.arange(round(0.2 * RATE)) / (0.05 * RATE)))[: len(x)]
    stereo = np.stack([x, 0.8 * x], axis=1)
    path = tmp_path / "ref.wav"
    wavfile.write(path, RATE, (stereo * 32767).astype(np.int16))

    report = reverb.compare_reverb(path, [("self", path)])
    entry = report["renders"]["self"]
    assert entry["lag_samples"] == 0
    assert entry["delta"]["rms_db"] == pytest.approx(0.0, abs=1e-9)
    assert entry["render"]["tail_mae"] == pytest.approx(0.0, abs=1e-9)
    assert entry["render"]["tail_bias"] == pytest.approx(0.0, abs=1e-9)
    assert entry["delta"]["c50_300_2k"] == pytest.approx(0.0, abs=1e-9)
