"""Per-scenario perf table and frame-budget gate from a bench run's f3rt.log.

Usage: python3 -m tools.bench.n3ds_report f3rt.log [--budget-ms 16.97] [--min-windows 12]

The bench build (F3_N3DS_BENCH) logs `n3ds: scenario NAME frame=N` when the scripted timeline
enters a scenario, a `n3ds: perf ...` line from the presenter and an `n3ds: audio ...` line from
the emulation thread every 5 s. A window belongs to the scenario of the last marker before its
line; the first window after a marker straddles the boundary and is dropped.

Gate (docs: local frame-budget loop): every scenario needs >= 12 whole windows with emu_ms and
comp_ms mean under the budget, emu_ms p95 and comp_max_ms p95 under it, mean emu_fps and comp_fps
>= 58.5, mean present_fps >= 59.0, and no drops, underruns or late frames in any window.
"""

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

BUDGET_MS = 16.97
MIN_FPS = 58.5
MIN_PRESENT_FPS = 59.0
MIN_WINDOWS = 12

SCENARIO = re.compile(r"^n3ds: scenario (\S+) frame=(\d+)")
LINE = re.compile(r"^n3ds: (perf|audio) (.*)$")
FIELD = re.compile(r"(\w+)=(-?\d+(?:\.\d+)?)(?=\s|$)")


@dataclass
class Scenario:
    name: str
    frame: int
    perf: list[dict[str, float]] = field(default_factory=list)
    audio: list[dict[str, float]] = field(default_factory=list)


def parse(text: str) -> list[Scenario]:
    """Scenarios in log order; windows before the first marker and after `end` are ignored."""
    scenarios: list[Scenario] = []
    skip = {"perf": False, "audio": False}
    for line in text.splitlines():
        marker = SCENARIO.match(line)
        if marker:
            scenarios.append(Scenario(marker.group(1), int(marker.group(2))))
            skip = {"perf": True, "audio": True}
            continue
        window = LINE.match(line)
        if not window or not scenarios:
            continue
        kind = window.group(1)
        if skip[kind]:
            skip[kind] = False
            continue
        values = {key: float(value) for key, value in FIELD.findall(window.group(2))}
        getattr(scenarios[-1], kind).append(values)
    return [scenario for scenario in scenarios if scenario.name != "end"]


def percentile(values: list[float], fraction: float) -> float:
    """Linear interpolation between closest ranks (numpy's default)."""
    ordered = sorted(values)
    position = fraction * (len(ordered) - 1)
    low = int(position)
    high = min(low + 1, len(ordered) - 1)
    return ordered[low] + (ordered[high] - ordered[low]) * (position - low)


def mean(values: list[float]) -> float:
    return sum(values) / len(values)


def column(windows: list[dict[str, float]], key: str) -> list[float]:
    return [window[key] for window in windows if key in window]


def verdict(scenario: Scenario, budget_ms: float, min_windows: int) -> list[str]:
    """Reasons the scenario fails the gate (empty: PASS)."""
    perf, audio = scenario.perf, scenario.audio
    if len(perf) < min_windows:
        return [f"only {len(perf)} perf windows (< {min_windows})"]
    problems = []

    def check(ok: bool, text: str) -> None:
        if not ok:
            problems.append(text)

    emu, comp, comp_max = column(perf, "emu_ms"), column(perf, "comp_ms"), column(perf, "comp_max_ms")
    check(mean(emu) < budget_ms, f"emu_ms mean {mean(emu):.2f}")
    check(percentile(emu, 0.95) < budget_ms, f"emu_ms p95 {percentile(emu, 0.95):.2f}")
    check(mean(comp) < budget_ms, f"comp_ms mean {mean(comp):.2f}")
    check(percentile(comp_max, 0.95) < budget_ms, f"comp_max_ms p95 {percentile(comp_max, 0.95):.2f}")
    check(mean(column(perf, "emu_fps")) >= MIN_FPS, f"emu_fps mean {mean(column(perf, 'emu_fps')):.1f}")
    check(mean(column(perf, "comp_fps")) >= MIN_FPS, f"comp_fps mean {mean(column(perf, 'comp_fps')):.1f}")
    present = mean(column(perf, "present_fps"))
    check(present >= MIN_PRESENT_FPS, f"present_fps mean {present:.1f}")
    drops = drops_of(scenario)
    check(drops == 0, f"drops {drops:g}")
    for key in ("underruns", "late"):
        total = sum(column(audio, key))
        check(total == 0, f"{key} {total:g}")
    return problems


def drops_of(scenario: Scenario) -> float:
    """Pacer drops inside the scenario: the audio windows' own counts, or the rise of the
    cumulative perf counter, whichever is larger."""
    cumulative = column(scenario.perf, "drops")
    rise = cumulative[-1] - cumulative[0] if cumulative else 0
    return max(sum(column(scenario.audio, "drops")), rise)


def stat(values: list[float]) -> str:
    if not values:
        return "-"
    return f"{mean(values):.2f}/{percentile(values, 0.95):.2f}/{max(values):.2f}"


def table(scenarios: list[Scenario], budget_ms: float, min_windows: int) -> tuple[str, bool]:
    header = (
        f"{'scenario':<11}{'win':>4}  {'emu_ms m/p95/max':<19}{'comp_ms m/p95/max':<19}"
        f"{'comp_max_ms m/p95/max':<23}{'emu_fps m/min':<15}{'comp_fps m/min':<16}"
        f"{'present_fps':>11}{'drops':>6}{'under':>6}{'late':>5}  gate"
    )
    rows = [header, "-" * len(header)]
    failed = []
    for scenario in scenarios:
        perf = scenario.perf
        problems = verdict(scenario, budget_ms, min_windows)
        emu_fps, comp_fps = column(perf, "emu_fps"), column(perf, "comp_fps")
        rows.append(
            f"{scenario.name:<11}{len(perf):>4}  {stat(column(perf, 'emu_ms')):<19}"
            f"{stat(column(perf, 'comp_ms')):<19}{stat(column(perf, 'comp_max_ms')):<23}"
            + (f"{mean(emu_fps):.1f}/{min(emu_fps):.1f}".ljust(15) if emu_fps else "-".ljust(15))
            + (f"{mean(comp_fps):.1f}/{min(comp_fps):.1f}".ljust(16) if comp_fps else "-".ljust(16))
            + (f"{mean(column(perf, 'present_fps')):>11.1f}" if perf else f"{'-':>11}")
            + f"{drops_of(scenario):>6g}{sum(column(scenario.audio, 'underruns')):>6g}"
            f"{sum(column(scenario.audio, 'late')):>5g}  " + ("PASS" if not problems else "FAIL " + "; ".join(problems))
        )
        if problems:
            failed.append(scenario.name)
    rows.append("")
    rows.append(
        f"budget {budget_ms} ms; 5 s windows, first window after each marker dropped; p95 interpolated; "
        + ("GATE PASS" if not failed and scenarios else "GATE FAIL: " + (", ".join(failed) or "no scenarios in log"))
    )
    return "\n".join(rows), not failed and bool(scenarios)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("log", type=Path)
    parser.add_argument("--budget-ms", type=float, default=BUDGET_MS)
    parser.add_argument("--min-windows", type=int, default=MIN_WINDOWS)
    args = parser.parse_args()
    text, passed = table(parse(args.log.read_text(errors="replace")), args.budget_ms, args.min_windows)
    print(text)
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
