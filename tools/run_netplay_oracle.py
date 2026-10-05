#!/usr/bin/env python3
"""Automated real-server F3 netplay oracle and snapshot proof test runner.

Executes and verifies:
1. Full rollback snapshots and canonical host snapshots across sound drivers,
   boot/gameplay boundaries, different presentation settings, and replay depths.
2. Divergent solo histories and EEPROMs converging through the real relay's host
   snapshot barrier; both peers' state, native framebuffer, and confirmed PCM
   agree with an independent reference loaded from that exact handoff.
3. Baseline and impaired (80ms RTT, 20ms jitter, 3% loss/reorder) campaigns,
   natural versus exits, local advancement, and fresh snapshots in the same room.
4. Late inputs, bounded-window stalls, presentation/host-slot independence,
   explicit incompatible-build rejection, and local recovery after peer death.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import socket
import subprocess
import sys
import threading
import time
from typing import Callable, Dict, List, Optional, Tuple


def find_free_udp_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def parse_oracle_output(stdout: str) -> Dict[str, str]:
    res = {}
    for line in stdout.splitlines():
        if "SUCCESS" in line:
            parts = line.strip().split()
            for p in parts[1:]:
                if "=" in p:
                    k, v = p.split("=", 1)
                    res[k] = v
        elif "REGRESSION FAILURE" in line or "ORACLE ERROR" in line:
            res["error"] = line.strip()
    return res


def assert_parity_match(c1: Dict[str, str], c2: Dict[str, str], ref: Dict[str, str],
                        expected_frames: int, context: str) -> None:
    """Verifies that all CRC, frame, and audio fields are non-empty and strictly match."""
    c1_f = int(c1.get("frames", 0))
    c2_f = int(c2.get("frames", 0))
    ref_f = int(ref.get("frames", 0))

    if c1_f != expected_frames or c2_f != expected_frames or ref_f != expected_frames:
        raise AssertionError(
            f"[{context}] Frame count mismatch: expected {expected_frames}, "
            f"got C1={c1_f}, C2={c2_f}, REF={ref_f}"
        )

    c1_crc = c1.get("final_crc", "")
    c2_crc = c2.get("final_crc", "")
    ref_crc = ref.get("final_crc", "")
    if not c1_crc or not c2_crc or not ref_crc:
        raise AssertionError(f"[{context}] Empty final_crc: C1='{c1_crc}' C2='{c2_crc}' REF='{ref_crc}'")
    if c1_crc != c2_crc or c1_crc != ref_crc:
        raise AssertionError(f"[{context}] State CRC mismatch: C1={c1_crc} C2={c2_crc} REF={ref_crc}")

    c1_fcrc = c1.get("frame_crc", "")
    c2_fcrc = c2.get("frame_crc", "")
    ref_fcrc = ref.get("frame_crc", "")
    if not c1_fcrc or not c2_fcrc or not ref_fcrc:
        raise AssertionError(f"[{context}] Empty frame_crc: C1='{c1_fcrc}' C2='{c2_fcrc}' REF='{ref_fcrc}'")
    if c1_fcrc != c2_fcrc or c1_fcrc != ref_fcrc:
        raise AssertionError(f"[{context}] Framebuffer CRC mismatch: C1={c1_fcrc} C2={c2_fcrc} REF={ref_fcrc}")

    c1_acrc = c1.get("audio_crc", "")
    c2_acrc = c2.get("audio_crc", "")
    ref_acrc = ref.get("audio_crc", "")
    if not c1_acrc or not c2_acrc or not ref_acrc:
        raise AssertionError(f"[{context}] Empty audio_crc: C1='{c1_acrc}' C2='{c2_acrc}' REF={ref_acrc}")
    if c1_acrc != c2_acrc or c1_acrc != ref_acrc:
        raise AssertionError(f"[{context}] Audio PCM CRC mismatch: C1={c1_acrc} C2={c2_acrc} REF={ref_acrc}")

    c1_asamp = c1.get("audio_samples", "")
    c2_asamp = c2.get("audio_samples", "")
    ref_asamp = ref.get("audio_samples", "")
    if not c1_asamp or not c2_asamp or not ref_asamp:
        raise AssertionError(f"[{context}] Empty audio_samples: C1='{c1_asamp}' C2='{c2_asamp}' REF={ref_asamp}")
    if c1_asamp != c2_asamp or c1_asamp != ref_asamp:
        raise AssertionError(f"[{context}] Audio sample count mismatch: C1={c1_asamp} C2={c2_asamp} REF={ref_asamp}")


class GoServerProcess:
    """Manages the Go UDP relay server process with reliable log-based readiness detection."""

    def __init__(self, server_bin: Path, port: int, rtt: str = "", jitter: str = "",
                 loss: float = 0.0, reorder: float = 0.0, seed: int = 0,
                 log_path: Optional[Path] = None):
        self.server_bin = server_bin
        self.port = port
        self.rtt = rtt
        self.jitter = jitter
        self.loss = loss
        self.reorder = reorder
        self.seed = seed
        self.log_path = log_path
        self.proc: Optional[subprocess.Popen] = None
        self.ready_event = threading.Event()
        self.log_file = None

    def start(self, timeout: float = 10.0) -> None:
        cmd = []
        cwd = None

        if self.server_bin.is_file():
            cmd = [str(self.server_bin)]
        elif self.server_bin.is_dir() or self.server_bin.suffix == ".go":
            cmd = ["go", "run", "."]
            cwd = str(self.server_bin if self.server_bin.is_dir() else self.server_bin.parent)
        else:
            raise FileNotFoundError(
                f"Go relay server binary not found at '{self.server_bin}'. "
                f"Compile it first with: go build -o build/netplay-server ./netplay/server"
            )

        cmd += ["-port", str(self.port), "-addr", f"127.0.0.1:{self.port}"]
        if self.rtt: cmd += ["-rtt", self.rtt]
        if self.jitter: cmd += ["-jitter", self.jitter]
        if self.loss > 0: cmd += ["-loss", str(self.loss)]
        if self.reorder > 0: cmd += ["-reorder", str(self.reorder)]
        if self.seed > 0: cmd += ["-seed", str(self.seed)]

        if self.log_path:
            self.log_path.parent.mkdir(parents=True, exist_ok=True)
            self.log_file = open(self.log_path, "w", encoding="utf-8")

        self.proc = subprocess.Popen(
            cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1
        )

        def reader():
            try:
                for line in self.proc.stdout:
                    if self.log_file:
                        self.log_file.write(line)
                        self.log_file.flush()
                    if "Relay Server listening on" in line:
                        self.ready_event.set()
            except Exception:
                pass

        t = threading.Thread(target=reader, daemon=True)
        t.start()

        if not self.ready_event.wait(timeout=timeout):
            self.stop()
            raise RuntimeError(f"Server failed to log 'Relay Server listening on' within {timeout}s")

    def stop(self) -> None:
        if self.proc:
            try:
                self.proc.terminate()
                self.proc.wait(timeout=2.0)
            except (subprocess.TimeoutExpired, OSError):
                try:
                    self.proc.kill()
                    self.proc.wait(timeout=1.0)
                except OSError:
                    pass
            self.proc = None
        if self.log_file:
            try:
                self.log_file.close()
            except OSError:
                pass
            self.log_file = None


def run_match_pair(oracle_bin: Path, server_addr: str, room: str, seed: int, frames: int,
                   delay: int, window: int, sound_driver: str, rom_dir: Optional[Path],
                   c1_extra_args: Optional[List[str]] = None,
                   c2_extra_args: Optional[List[str]] = None,
                   c1_dump: Optional[Path] = None, c2_dump: Optional[Path] = None,
                   timeout_sec: float = 300.0,
                   on_c1_progress: Optional[Callable[[int, subprocess.Popen, subprocess.Popen], None]] = None
                   ) -> Tuple[int, str, str, int, str, str]:
    """Runs Client 1 and Client 2 concurrently with reliable lifecycle management and deadlock-free I/O."""
    c1_cmd = [
        str(oracle_bin), "--mode", "client", "--player", "1",
        "--server", server_addr, "--room", room,
        "--seed", str(seed), "--frames", str(frames),
        "--delay", str(delay), "--window", str(window),
        "--sound-driver", sound_driver, "--unthrottled",
    ]
    c2_cmd = [
        str(oracle_bin), "--mode", "client", "--player", "2",
        "--server", server_addr, "--room", room,
        "--seed", str(seed), "--frames", str(frames),
        "--delay", str(delay), "--window", str(window),
        "--sound-driver", sound_driver, "--unthrottled",
    ]
    if rom_dir:
        c1_cmd += ["--rom-dir", str(rom_dir)]
        c2_cmd += ["--rom-dir", str(rom_dir)]
    if c1_dump:
        c1_dump.mkdir(parents=True, exist_ok=True)
        c1_cmd += ["--dump-dir", str(c1_dump), "--surface", str(c1_dump / "surface.bmp")]
    if c2_dump:
        c2_dump.mkdir(parents=True, exist_ok=True)
        c2_cmd += ["--dump-dir", str(c2_dump), "--surface", str(c2_dump / "surface.bmp")]
    if c1_extra_args:
        c1_cmd += c1_extra_args
    if c2_extra_args:
        c2_cmd += c2_extra_args

    p1 = subprocess.Popen(c1_cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, bufsize=1)
    p2 = subprocess.Popen(c2_cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, bufsize=1)

    out1_lines, err1_lines = [], []
    out2_lines, err2_lines = [], []

    def stream_reader(pipe, lines_dest, cb=None, proc_self=None, proc_other=None):
        try:
            for line in pipe:
                lines_dest.append(line)
                if cb and "[PROGRESS]" in line:
                    m = re.search(r"confirmed=(\d+)", line)
                    if m:
                        cb(int(m.group(1)), proc_self, proc_other)
        except Exception:
            pass

    t1_out = threading.Thread(target=stream_reader, args=(p1.stdout, out1_lines, on_c1_progress, p1, p2))
    t1_err = threading.Thread(target=stream_reader, args=(p1.stderr, err1_lines))
    t2_out = threading.Thread(target=stream_reader, args=(p2.stdout, out2_lines))
    t2_err = threading.Thread(target=stream_reader, args=(p2.stderr, err2_lines))

    for t in (t1_out, t1_err, t2_out, t2_err):
        t.start()

    try:
        p1.wait(timeout=timeout_sec)
        p2.wait(timeout=timeout_sec)
    except subprocess.TimeoutExpired:
        raise
    finally:
        # Guarantee all processes are terminated and reaped on error/timeout/exception
        for p in (p1, p2):
            if p.poll() is None:
                try:
                    p.kill()
                    p.wait(timeout=2.0)
                except OSError:
                    pass
        for t in (t1_out, t1_err, t2_out, t2_err):
            t.join(timeout=2.0)

    return (
        p1.returncode if p1.returncode is not None else -1,
        "".join(out1_lines), "".join(err1_lines),
        p2.returncode if p2.returncode is not None else -1,
        "".join(out2_lines), "".join(err2_lines)
    )


# -----------------------------------------------------------------------------
# Test Suites
# -----------------------------------------------------------------------------

def test_snapshot_proof(oracle_bin: Path, rom_dir: Optional[Path], seeds: List[int],
                        frames: int, sound_driver: str, capture_dir: Optional[Path],
                        log_dir: Path) -> bool:
    print("\n========================================================")
    print(f"TEST SUITE: Automated Snapshot Save/Load Proof & Benchmark")
    print(f"  Drivers: {sound_driver} | Frames: {frames} | K depths: 1, 7, 16, 31, 97")
    print("========================================================")
    all_ok = True

    # Cover both oracle and native drivers if "all" (default) or individual
    drivers_to_test = ["oracle", "native"] if sound_driver == "all" else [sound_driver]

    for drv in drivers_to_test:
        for seed in seeds:
            print(f"\n--- Snapshot Proof: seed={seed} frames={frames} driver={drv} (K=1,7,16,31,97, N includes 0) ---")
            cmd = [
                str(oracle_bin),
                "--mode", "snapshot",
                "--seed", str(seed),
                "--frames", str(frames),
                "--sound-driver", drv,
                "--snapshot-interval", "1000",
            ]
            if rom_dir:
                cmd += ["--rom-dir", str(rom_dir)]
            if capture_dir:
                surf = capture_dir / f"snapshot_proof_{drv}_s{seed}.bmp"
                cmd += ["--surface", str(surf), "--dump-dir", str(capture_dir / f"proof_{drv}_s{seed}")]

            print("Command:", " ".join(cmd))
            t0 = time.monotonic()
            res = subprocess.run(cmd, capture_output=True, text=True)
            elapsed = time.monotonic() - t0

            (log_dir / f"snapshot_{drv}_s{seed}.log").write_text(res.stdout + "\nSTDERR:\n" + res.stderr)
            print(res.stdout)
            if res.stderr:
                print("STDERR:", res.stderr, file=sys.stderr)

            if res.returncode != 0:
                print(f"FAILED: snapshot proof for seed {seed} ({drv}) returned {res.returncode}")
                all_ok = False
                continue

            parsed = parse_oracle_output(res.stdout)
            if not parsed.get("final_crc"):
                print(f"FAILED: snapshot proof output missing final_crc")
                all_ok = False
                continue

            print(f"PASS: seed={seed} driver={drv} checks={parsed.get('checks', '?')} "
                  f"save_mean={parsed.get('save_us_mean', '?')}us save_p95={parsed.get('save_us_p95', '?')}us "
                  f"load_mean={parsed.get('load_us_mean', '?')}us load_p95={parsed.get('load_us_p95', '?')}us "
                  f"step_fps={parsed.get('step_fps', '?')} "
                  f"depth_60hz(mean={parsed.get('affordable_depth_mean', '?')}, p95={parsed.get('affordable_depth_p95', '?')}) "
                  f"allocs=(save={parsed.get('total_save_allocs')}, load={parsed.get('total_load_allocs')}) "
                  f"perturbations={parsed.get('wall_clock_perturbations')} elapsed={elapsed:.1f}s")

    return all_ok


def records(stdout: str, prefix: str) -> List[Dict[str, str]]:
    return [dict(word.split("=", 1) for word in line.split()[1:] if "=" in word)
            for line in stdout.splitlines() if line.startswith(prefix + " ")]


def verify_campaign(oracle_bin: Path, rom_dir: Optional[Path], seed: int, frames: int,
                    driver: str, out1: str, out2: str, host_dump: Path,
                    log_dir: Path, label: str) -> Tuple[Dict[str, str], Dict[str, str]]:
    """Replay each actual host handoff independently, never a cold-boot substitute."""
    clients = [parse_oracle_output(out1), parse_oracle_output(out2)]
    first, second = clients
    for key in ("frames", "final_crc", "frame_crc", "audio_crc", "audio_samples",
                "matches", "natural_ends", "local_returns"):
        assert key in first and first[key] == second.get(key), f"{label}: peer {key} mismatch"
    assert int(first["frames"]) == frames, f"{label}: wrong aggregate frame count"
    assert first["pre_crc"] != second["pre_crc"], f"{label}: prehistories did not diverge"
    assert first["pre_eeprom_crc"] != second["pre_eeprom_crc"], f"{label}: EEPROMs did not diverge"
    assert all(c["fallback"] == "0" for c in clients), f"{label}: native fallback"
    matches = [records(out1, "MATCH"), records(out2, "MATCH")]
    handoffs = [records(out1, "[HANDOFF]"), records(out2, "[HANDOFF]")]
    returns = [records(out1, "[LOCAL_RETURN]"), records(out2, "[LOCAL_RETURN]")]
    count = int(first["matches"])
    assert count == int(first["local_returns"]), f"{label}: session did not return locally"
    assert all(len(rows) == count for rows in matches + handoffs + returns), f"{label}: missing lifecycle transitions"
    assert sum(int(m["frames"]) for m in matches[0]) == frames
    if frames >= 20000:
        assert int(first["natural_ends"]) > 0, f"{label}: no natural match exit exercised"
    previous_end = 0
    for index, (m1, m2, h1, h2, r1, r2) in enumerate(zip(*matches, *handoffs, *returns)):
        assert m1["match"] == m2["match"] == str(index)
        assert m1["natural_end"] == m2["natural_end"]
        assert h1["initial_crc"] == h2["initial_crc"], f"{label}: adoption CRC mismatch"
        assert h1["origin"] == h2["origin"] == m1["origin"] == m2["origin"]
        assert int(h1["origin"]) > previous_end, f"{label}: stale rematch snapshot"
        assert all(int(h["flags"]) & 3 == 3 and int(h["flags"]) & 0xc0
                   and h["match_kind"] == "1" for h in (h1, h2))
        assert h1["delay"] == h2["delay"] == m1["delay"] == m2["delay"]
        assert r1["crc"] != r2["crc"], f"{label}: local post-match histories stayed locked"
        assert r1["fallback"] == r2["fallback"] == "0"
        previous_end = int(h1["origin"]) + int(m1["frames"])
        cmd = [str(oracle_bin), "--mode", "reference", "--seed", str(seed),
               "--frames", m1["frames"], "--delay", m1["delay"], "--sound-driver", driver,
               "--match-index", str(index), "--initial-state", str(host_dump / f"match_{index}" / "handoff.bin")]
        if rom_dir:
            cmd += ["--rom-dir", str(rom_dir)]
        reference = subprocess.run(cmd, capture_output=True, text=True, timeout=180)
        (log_dir / f"reference_{label}_m{index}.log").write_text(reference.stdout + "\nSTDERR:\n" + reference.stderr)
        assert reference.returncode == 0, f"{label}: reference failed: {reference.stderr}"
        assert_parity_match(m1, m2, parse_oracle_output(reference.stdout), int(m1["frames"]), f"{label}/match{index}")
    print(f"PASS {label}: frames={frames} matches={count} natural_ends={first['natural_ends']} "
          f"state={first['final_crc']} audio={first['audio_crc']} samples={first['audio_samples']} "
          f"rollbacks={first['rollbacks']}/{second['rollbacks']} depth={first['max_depth']}/{second['max_depth']}")
    return first, second


def campaign_pair(oracle_bin: Path, server_bin: Path, rom_dir: Optional[Path],
                  seed: int, frames: int, delay: int, window: int, driver: str,
                  capture_dir: Optional[Path], log_dir: Path, label: str,
                  impaired: bool = False, extras1=None, extras2=None, host_player: int = 1):
    port = find_free_udp_port()
    server = GoServerProcess(server_bin, port, rtt="80ms" if impaired else "",
                             jitter="20ms" if impaired else "", loss=0.03 if impaired else 0,
                             reorder=0.03 if impaired else 0, seed=seed,
                             log_path=log_dir / f"server_{label}.log")
    server.start()
    dumps = [(capture_dir or log_dir) / label / f"c{i}" for i in (1, 2)]
    try:
        rc1, out1, err1, rc2, out2, err2 = run_match_pair(
            oracle_bin, f"127.0.0.1:{port}", label[:30], seed, frames, delay, window,
            driver, rom_dir, c1_dump=dumps[0], c2_dump=dumps[1],
            c1_extra_args=extras1, c2_extra_args=extras2, timeout_sec=max(180, frames / 30 + 120))
        (log_dir / f"c1_{label}.log").write_text(out1 + "\nSTDERR:\n" + err1)
        (log_dir / f"c2_{label}.log").write_text(out2 + "\nSTDERR:\n" + err2)
        assert rc1 == rc2 == 0, f"{label}: clients {rc1}/{rc2}\n{err1}\n{err2}"
        c1, c2 = verify_campaign(oracle_bin, rom_dir, seed, frames, driver, out1, out2,
                                 dumps[host_player - 1], log_dir, label)
        if impaired:
            assert int(c1["rollbacks"]) + int(c2["rollbacks"]) > 0, "Impairment induced no corrections"
        return c1, c2, out1, out2
    finally:
        server.stop()


def test_campaigns(oracle_bin: Path, server_bin: Path, rom_dir: Optional[Path],
                   seeds: List[int], frames: int, delay: int, window: int,
                   sound_driver: str, capture_dir: Optional[Path], log_dir: Path,
                   impaired: bool = False) -> bool:
    label = "impaired" if impaired else "baseline"
    print(f"\nTEST SUITE: {label}: divergent solo/EEPROM -> host handoff -> versus -> local/rematch")
    if impaired:
        print("Relay impairment: 80ms RTT, 20ms jitter, 3% loss, 3% reorder, including snapshot chunks")
    all_ok = True
    for seed in seeds:
        try:
            campaign_pair(oracle_bin, server_bin, rom_dir, seed, frames, delay, window,
                          "native" if sound_driver == "all" else sound_driver,
                          capture_dir, log_dir, f"{label}_s{seed}", impaired)
        except (AssertionError, RuntimeError, subprocess.TimeoutExpired) as error:
            print(f"FAILED {label} seed={seed}: {error}")
            all_ok = False
    return all_ok


def test_edge_cases(oracle_bin: Path, server_bin: Path, rom_dir: Optional[Path],
                    log_dir: Path) -> bool:
    print("\nTEST SUITE: late inputs, long stalls, presentation/host-slot independence, rejection, disconnect")
    all_ok = True
    for name, extras1, extras2, host_player in [
        ("late", ["--withhold-input-at", "1500", "--withhold-input-ms", "120"],
         ["--observe-event-at", "1500"], 1),
        ("stall", ["--stall-at", "1500", "--stall-ms", "1000"],
         ["--observe-event-at", "1500"], 1),
        ("presentation", [], ["--video-scale", "2", "--video-border", "48", "--delay", "8"], 1),
        ("host_p2", ["--host-player", "2", "--delay", "8"], ["--host-player", "2"], 2),
    ]:
        try:
            c1, c2, out1, out2 = campaign_pair(
                oracle_bin, server_bin, rom_dir, 12345, 3000, 2, 16, "native",
                None, log_dir, name, extras1=extras1, extras2=extras2, host_player=host_player)
            if name == "late":
                assert "[WITHHOLD_BEGIN]" in out1 and "[WITHHOLD_END]" in out1
                assert int(c2["event_delta_rollbacks"]) > 0 and int(c2["event_max_depth"]) > 0
                assert float(c2["event_stall_ms"]) >= 20, "No bounded-window stall during withheld input"
            elif name == "stall":
                assert "[STALL_BEGIN]" in out1 and "[STALL_END]" in out1
                assert int(c2["event_delta_stalls"]) > 0 and float(c2["event_stall_ms"]) >= 200
            elif name in ("presentation", "host_p2"):
                assert all(h["delay"] == "2" for h in records(out1, "[HANDOFF]") + records(out2, "[HANDOFF]"))
        except (AssertionError, RuntimeError, subprocess.TimeoutExpired) as error:
            print(f"FAILED {name}: {error}")
            all_ok = False

    for name in ("mismatch", "disconnect"):
        port = find_free_udp_port()
        server = GoServerProcess(server_bin, port, log_path=log_dir / f"server_{name}.log")
        server.start()
        killed = threading.Event()
        def on_progress(confirmed, proc1, proc2):
            if confirmed >= 800 and not killed.is_set():
                killed.set()
                proc2.kill()
        try:
            common = ["--timeout", "12"]
            if name == "mismatch":
                common += ["--prelude-frames", "0"]
            rc1, out1, err1, rc2, out2, err2 = run_match_pair(
                oracle_bin, f"127.0.0.1:{port}", name, 12345, 5000, 2, 16,
                "native", rom_dir, c1_extra_args=common,
                c2_extra_args=common + (["--corrupt-build-hash"] if name == "mismatch" else []),
                on_c1_progress=on_progress if name == "disconnect" else None, timeout_sec=90)
            (log_dir / f"c1_{name}.log").write_text(out1 + "\nSTDERR:\n" + err1)
            (log_dir / f"c2_{name}.log").write_text(out2 + "\nSTDERR:\n" + err2)
            if name == "mismatch":
                assert rc2 != 0 and "build hash mismatch" in (out2 + err2).lower()
            else:
                diagnostic = (out1 + err1).lower()
                assert killed.is_set() and rc1 != 0
                assert any(text in diagnostic for text in ("peer timeout", "connection timeout", "peer disconnected", "opponent"))
                assert "oracle watchdog" not in diagnostic
                recovered = records(out1, "[LOCAL_RETURN]")
                assert recovered[-1]["advanced"] == "60" and recovered[-1]["fallback"] == "0"
            print(f"PASS {name}: explicit network result; strict-native local recovery")
        except (AssertionError, RuntimeError, subprocess.TimeoutExpired) as error:
            print(f"FAILED {name}: {error}")
            all_ok = False
        finally:
            server.stop()
    return all_ok


# -----------------------------------------------------------------------------
# Main Entry Point
# -----------------------------------------------------------------------------

def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--oracle-bin", type=Path, default=Path("build/f3rt-netplay-oracle"))
    parser.add_argument("--server-bin", type=Path, default=Path("build/netplay-server"))
    parser.add_argument("--rom-dir", type=Path)
    parser.add_argument("--seeds", type=int, nargs="+", default=[1, 2, 3, 5, 8, 13, 21, 34])
    parser.add_argument("--frames", type=int, default=20000)
    parser.add_argument("--delay", type=int, default=2)
    parser.add_argument("--window", type=int, default=16)
    parser.add_argument("--sound-driver", choices=["oracle", "native", "all"], default="native",
                        help="Sound driver (default: native). When 'all', snapshots test both and netplay runs native.")
    parser.add_argument("--suite", choices=["all", "snapshot", "baseline", "impaired", "cases"],
                        default="all")
    parser.add_argument("--dump-captures-dir", type=Path)
    parser.add_argument("--log-dir", type=Path, default=Path("build/netplay_logs"))
    args = parser.parse_args()

    oracle_bin = args.oracle_bin.resolve()
    server_bin = args.server_bin.resolve()
    log_dir = args.log_dir.resolve()
    log_dir.mkdir(parents=True, exist_ok=True)

    if not oracle_bin.is_file():
        alt_paths = [
            Path("build/native/f3rt-netplay-oracle"),
            Path("build/f3rt-netplay-oracle"),
        ]
        found = False
        for p in alt_paths:
            if p.is_file():
                oracle_bin = p.resolve()
                found = True
                break
        if not found:
            parser.error(f"Oracle binary not found: {oracle_bin}. Build f3rt-netplay-oracle first.")

    if args.dump_captures_dir:
        args.dump_captures_dir.mkdir(parents=True, exist_ok=True)

    results = []

    # 1. Snapshot save/load proof & performance benchmark
    if args.suite in ["all", "snapshot"]:
        ok = test_snapshot_proof(oracle_bin, args.rom_dir, args.seeds,
                                 frames=min(args.frames, 6000),
                                 sound_driver=args.sound_driver,
                                 capture_dir=args.dump_captures_dir,
                                 log_dir=log_dir)
        for seed in args.seeds:
            for driver in (["native", "oracle"] if args.sound_driver == "all" else [args.sound_driver]):
                cmd = [str(oracle_bin), "--mode", "sync-proof", "--seed", str(seed),
                       "--frames", str(max(2400, min(args.frames, 6000))), "--sound-driver", driver]
                if args.rom_dir:
                    cmd += ["--rom-dir", str(args.rom_dir)]
                result = subprocess.run(cmd, capture_output=True, text=True)
                (log_dir / f"sync_proof_{driver}_s{seed}.log").write_text(
                    result.stdout + "\nSTDERR:\n" + result.stderr)
                print(result.stdout)
                if result.returncode:
                    print(result.stderr, file=sys.stderr)
                    ok = False
        results.append(("snapshot_proof", ok))

    # 2. Baseline real-server match (full frames, all seeds)
    if args.suite in ["all", "baseline"]:
        ok = test_campaigns(oracle_bin, server_bin, args.rom_dir,
                                   args.seeds, args.frames, args.delay,
                                   args.window, args.sound_driver,
                                   args.dump_captures_dir, log_dir)
        results.append(("baseline_matches", ok))

    # 3. Impairment tests (full frames, all seeds)
    if args.suite in ["all", "impaired"]:
        ok = test_campaigns(oracle_bin, server_bin, args.rom_dir,
                            args.seeds, args.frames, args.delay,
                            args.window, args.sound_driver,
                            args.dump_captures_dir, log_dir, impaired=True)
        results.append(("impaired_network", ok))

    # 4. Edge cases
    if args.suite in ["all", "cases"]:
        ok_cases = test_edge_cases(oracle_bin, server_bin, args.rom_dir, log_dir)
        results.append(("edge_cases", ok_cases))

    print("\n========================================================")
    print("FINAL TEST RUNNER SUMMARY:")
    all_passed = True
    for name, ok in results:
        status = "PASSED" if ok else "FAILED"
        print(f"  - {name}: {status}")
        if not ok:
            all_passed = False

    print("OVERALL RESULT:", "ALL PASSED" if all_passed else "FAILURES DETECTED")
    print("========================================================")

    return 0 if all_passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
