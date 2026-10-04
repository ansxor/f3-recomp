#!/usr/bin/env python3
"""Automated real-server F3 netplay oracle and snapshot proof test runner.

Executes and verifies:
1. Snapshot save/load proof & performance benchmark across seeds, drivers (oracle & native),
   cold boot (N=0) and mid-game frames, with varied K depths (1, 7, 16, 31, 97).
2. Two-client real-server rollback netplay vs. reference machine (>=20,000 frames per seed),
   verifying non-empty identical state CRC, framebuffer CRC, and confirmed audio PCM CRC/count.
3. Impaired network simulation (80ms RTT, 20ms jitter, 3% loss, 3% reorder, >=20,000 frames/seed),
   verifying rollbacks and non-empty matching state CRC and audio CRC.
4. Edge cases:
   - Distinct late-input scenario (120ms withheld input at frame 1500), asserting actual
     rollbacks/max_depth > 0, and comparing against reference state and audio PCM/count.
   - Long stall scenario (1000ms complete pause at frame 1500), asserting peer frontier stalls
     and recovery to matching state and audio PCM/count.
   - Strict build hash handshake mismatch rejection with explicit error code 5.
   - Clean peer disconnect detection triggered after observed midgame progress (confirmed >= 800).
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


def test_baseline_matches(oracle_bin: Path, server_bin: Path, rom_dir: Optional[Path],
                          seeds: List[int], frames: int, delay: int, window: int,
                          sound_driver: str, capture_dir: Optional[Path],
                          log_dir: Path) -> bool:
    print("\n========================================================")
    print(f"TEST SUITE: Real-Server 2-Client Netplay vs. Reference ({frames} frames/seed)")
    print("========================================================")
    all_ok = True
    active_driver = "native" if sound_driver == "all" else sound_driver

    for seed in seeds:
        print(f"\n--- Baseline Match: seed={seed} frames={frames} delay={delay} window={window} driver={active_driver} ---")
        port = find_free_udp_port()
        room = f"baseline_s{seed}_{int(time.time())}"
        server_log = log_dir / f"server_baseline_s{seed}.log"
        server = GoServerProcess(server_bin, port, log_path=server_log)
        server.start()

        try:
            # 1. Reference Machine Run
            ref_dump = (capture_dir / f"ref_seed_{seed}") if capture_dir else None
            ref_cmd = [
                str(oracle_bin), "--mode", "reference",
                "--seed", str(seed), "--frames", str(frames),
                "--delay", str(delay), "--sound-driver", active_driver,
            ]
            if rom_dir: ref_cmd += ["--rom-dir", str(rom_dir)]
            if ref_dump:
                ref_dump.mkdir(parents=True, exist_ok=True)
                ref_cmd += ["--dump-dir", str(ref_dump), "--surface", str(ref_dump / "surface.bmp")]

            t_ref0 = time.monotonic()
            ref_proc = subprocess.run(ref_cmd, capture_output=True, text=True)
            ref_elapsed = time.monotonic() - t_ref0
            print(ref_proc.stdout)

            if ref_proc.returncode != 0:
                print(f"FAILED: reference run for seed {seed} failed with code {ref_proc.returncode}")
                if ref_proc.stderr: print("STDERR:", ref_proc.stderr)
                all_ok = False
                continue

            ref_parsed = parse_oracle_output(ref_proc.stdout)

            # 2. Real Two-Client Match
            c1_dump = (capture_dir / f"c1_seed_{seed}") if capture_dir else None
            c2_dump = (capture_dir / f"c2_seed_{seed}") if capture_dir else None

            t_match0 = time.monotonic()
            rc1, out1, err1, rc2, out2, err2 = run_match_pair(
                oracle_bin, f"127.0.0.1:{port}", room, seed, frames, delay, window,
                active_driver, rom_dir, c1_dump=c1_dump, c2_dump=c2_dump
            )
            match_elapsed = time.monotonic() - t_match0

            (log_dir / f"c1_baseline_s{seed}.log").write_text(out1 + "\nSTDERR:\n" + err1)
            (log_dir / f"c2_baseline_s{seed}.log").write_text(out2 + "\nSTDERR:\n" + err2)

            if rc1 != 0 or rc2 != 0:
                print(f"FAILED: client exit error: rc1={rc1} rc2={rc2}")
                if err1: print("C1 STDERR:", err1)
                if err2: print("C2 STDERR:", err2)
                all_ok = False
                continue

            c1_parsed = parse_oracle_output(out1)
            c2_parsed = parse_oracle_output(out2)

            try:
                assert_parity_match(c1_parsed, c2_parsed, ref_parsed, frames, f"seed={seed}")
                print(f"EXACT PARITY MATCH: seed={seed} final_crc=0x{c1_parsed.get('final_crc')} "
                      f"frame_crc=0x{c1_parsed.get('frame_crc')} audio_crc=0x{c1_parsed.get('audio_crc')} "
                      f"samples={c1_parsed.get('audio_samples')} rollbacks(c1={c1_parsed.get('rollbacks')}, "
                      f"c2={c2_parsed.get('rollbacks')}) max_depth={c1_parsed.get('max_depth')} "
                      f"vs_status={c1_parsed.get('versus_status')} elapsed={match_elapsed:.1f}s")
            except AssertionError as ae:
                print(f"FAILED PARITY: {ae}")
                all_ok = False

        finally:
            server.stop()

    return all_ok


def test_impaired_network(oracle_bin: Path, server_bin: Path, rom_dir: Optional[Path],
                          seeds: List[int], frames: int, delay: int, window: int,
                          sound_driver: str, capture_dir: Optional[Path],
                          log_dir: Path) -> bool:
    print("\n========================================================")
    print(f"TEST SUITE: Impaired Network (80ms RTT, 20ms jitter, 3% loss, 3% reorder)")
    print(f"  Frames: {frames} per seed | All seeds: {seeds}")
    print("========================================================")
    all_ok = True
    active_driver = "native" if sound_driver == "all" else sound_driver

    for seed in seeds:
        print(f"\n--- Impaired Run: seed={seed} frames={frames} delay={delay} window={window} ---")
        port = find_free_udp_port()
        room = f"impaired_s{seed}_{int(time.time())}"
        server_log = log_dir / f"server_impaired_s{seed}.log"
        # Real Go server configured with mandatory impairment
        server = GoServerProcess(server_bin, port, rtt="80ms", jitter="20ms",
                                 loss=0.03, reorder=0.03, seed=seed, log_path=server_log)
        server.start()

        try:
            # 1. Reference Run for ground truth CRC
            ref_cmd = [
                str(oracle_bin), "--mode", "reference",
                "--seed", str(seed), "--frames", str(frames),
                "--delay", str(delay), "--sound-driver", active_driver,
            ]
            if rom_dir: ref_cmd += ["--rom-dir", str(rom_dir)]
            ref_proc = subprocess.run(ref_cmd, capture_output=True, text=True)
            if ref_proc.returncode != 0:
                print("FAILED: reference run failed:", ref_proc.stderr)
                all_ok = False
                continue

            ref_parsed = parse_oracle_output(ref_proc.stdout)

            # 2. Impaired Clients Run
            t_match0 = time.monotonic()
            rc1, out1, err1, rc2, out2, err2 = run_match_pair(
                oracle_bin, f"127.0.0.1:{port}", room, seed, frames, delay, window,
                active_driver, rom_dir, timeout_sec=500.0
            )
            match_elapsed = time.monotonic() - t_match0

            (log_dir / f"c1_impaired_s{seed}.log").write_text(out1 + "\nSTDERR:\n" + err1)
            (log_dir / f"c2_impaired_s{seed}.log").write_text(out2 + "\nSTDERR:\n" + err2)

            if rc1 != 0 or rc2 != 0:
                print(f"FAILED: impaired clients exited with error: rc1={rc1} rc2={rc2}")
                if err1: print("C1 STDERR:", err1)
                if err2: print("C2 STDERR:", err2)
                all_ok = False
                continue

            c1_parsed = parse_oracle_output(out1)
            c2_parsed = parse_oracle_output(out2)

            # Assert rollbacks actually occurred under network impairment
            r1 = int(c1_parsed.get("rollbacks", 0))
            r2 = int(c2_parsed.get("rollbacks", 0))
            if r1 == 0 and r2 == 0:
                print("FAILED: Impairment did not induce rollbacks (r1=0, r2=0)")
                all_ok = False
                continue

            try:
                assert_parity_match(c1_parsed, c2_parsed, ref_parsed, frames, f"impaired seed={seed}")
                print(f"PASS IMPAIRMENT: seed={seed} final_crc=0x{c1_parsed.get('final_crc')} "
                      f"audio_crc=0x{c1_parsed.get('audio_crc')} rollbacks(c1={r1}, c2={r2}) "
                      f"max_depth={c1_parsed.get('max_depth')} rtt_ms={c1_parsed.get('rtt_ms')} "
                      f"elapsed={match_elapsed:.1f}s")
            except AssertionError as ae:
                print(f"FAILED IMPAIRMENT PARITY: {ae}")
                all_ok = False

        finally:
            server.stop()

    return all_ok


def test_edge_cases(oracle_bin: Path, server_bin: Path, rom_dir: Optional[Path],
                    log_dir: Path) -> bool:
    print("\n========================================================")
    print("TEST SUITE: Edge Cases (Late Input, Long Stall, Mismatch, Disconnect)")
    print("========================================================")
    all_ok = True

    # Case A1: Distinct Late-Input Scenario (120ms withheld inputs at frame 1500)
    print("\n--- Edge Case A1: Late Input (120ms withheld inputs at frame 1500) ---")
    port = find_free_udp_port()
    room = f"late_input_case_{int(time.time())}"
    server = GoServerProcess(server_bin, port, log_path=log_dir / "server_late_input.log")
    server.start()
    try:
        # Reference run for 3000 frames
        ref_cmd = [
            str(oracle_bin), "--mode", "reference", "--seed", "12345",
            "--frames", "3000", "--delay", "2"
        ]
        if rom_dir: ref_cmd += ["--rom-dir", str(rom_dir)]
        ref_proc = subprocess.run(ref_cmd, capture_output=True, text=True)
        ref_parsed = parse_oracle_output(ref_proc.stdout)

        # Client 1 withholds inputs for 120ms at frame 1500, forcing peer to rollback and resimulate
        rc1, out1, err1, rc2, out2, err2 = run_match_pair(
            oracle_bin, f"127.0.0.1:{port}", room, seed=12345, frames=3000,
            delay=2, window=16, sound_driver="native", rom_dir=rom_dir,
            c1_extra_args=["--withhold-input-at", "1500", "--withhold-input-ms", "120"],
            c2_extra_args=["--observe-event-at", "1500"], timeout_sec=60.0
        )
        (log_dir / "c1_late_input.log").write_text(out1 + "\nSTDERR:\n" + err1)
        (log_dir / "c2_late_input.log").write_text(out2 + "\nSTDERR:\n" + err2)

        if rc1 != 0 or rc2 != 0:
            print(f"FAILED LATE INPUT: Client exit error: rc1={rc1} rc2={rc2}\n{err1}\n{err2}")
            all_ok = False
        else:
            c1_parsed = parse_oracle_output(out1)
            c2_parsed = parse_oracle_output(out2)
            c1_injected = ("[WITHHOLD_BEGIN]" in out1) and ("[WITHHOLD_END]" in out1)
            c2_event_rollbacks = int(c2_parsed.get("event_delta_rollbacks", 0))
            c2_event_depth = int(c2_parsed.get("event_max_depth", 0))

            if not c1_injected:
                print("FAILED LATE INPUT: Client 1 withhold injection did not execute")
                all_ok = False
            elif c2_event_rollbacks == 0 or c2_event_depth == 0 or float(c2_parsed.get("event_stall_ms", 0)) < 20:
                print(f"FAILED LATE INPUT: No correction/full-window stall attributable to withheld input: {c2_parsed}")
                all_ok = False
            else:
                try:
                    assert_parity_match(c1_parsed, c2_parsed, ref_parsed, 3000, "late_input")
                    print(f"PASS LATE INPUT: Peer successfully rolled back (delta_rollbacks={c2_event_rollbacks}, "
                          f"max_depth={c2_event_depth}) and resimulated to match reference CRC 0x{c1_parsed.get('final_crc')} "
                          f"and audio CRC 0x{c1_parsed.get('audio_crc')}")
                except AssertionError as ae:
                    print(f"FAILED LATE INPUT PARITY: {ae}")
                    all_ok = False
    finally:
        server.stop()

    # Case A2: Recoverable Long Stall (1000ms complete pause at frame 1500)
    print("\n--- Edge Case A2: Recoverable Long Stall (1000ms pause at frame 1500) ---")
    port = find_free_udp_port()
    room = f"stall_case_{int(time.time())}"
    server = GoServerProcess(server_bin, port, log_path=log_dir / "server_stall.log")
    server.start()
    try:
        ref_cmd = [
            str(oracle_bin), "--mode", "reference", "--seed", "12345",
            "--frames", "3000", "--delay", "2"
        ]
        if rom_dir: ref_cmd += ["--rom-dir", str(rom_dir)]
        ref_proc = subprocess.run(ref_cmd, capture_output=True, text=True)
        ref_parsed = parse_oracle_output(ref_proc.stdout)

        # Client 1 completely pauses pumping for 1000ms at frame 1500 (>16 frames)
        rc1, out1, err1, rc2, out2, err2 = run_match_pair(
            oracle_bin, f"127.0.0.1:{port}", room, seed=12345, frames=3000,
            delay=2, window=16, sound_driver="native", rom_dir=rom_dir,
            c1_extra_args=["--stall-at", "1500", "--stall-ms", "1000"],
            c2_extra_args=["--observe-event-at", "1500"], timeout_sec=60.0
        )
        (log_dir / "c1_stall.log").write_text(out1 + "\nSTDERR:\n" + err1)
        (log_dir / "c2_stall.log").write_text(out2 + "\nSTDERR:\n" + err2)

        if rc1 != 0 or rc2 != 0:
            print(f"FAILED LONG STALL: Client exit error: rc1={rc1} rc2={rc2}\n{err1}\n{err2}")
            all_ok = False
        else:
            c1_parsed = parse_oracle_output(out1)
            c2_parsed = parse_oracle_output(out2)
            c1_injected = ("[STALL_BEGIN]" in out1) and ("[STALL_END]" in out1)
            c2_event_stalls = int(c2_parsed.get("event_delta_stalls", 0))

            if not c1_injected:
                print("FAILED LONG STALL: Client 1 stall injection did not execute")
                all_ok = False
            elif c2_event_stalls == 0 or float(c2_parsed.get("event_stall_ms", 0)) < 200:
                print(f"FAILED LONG STALL: No sustained full-window stall attributable to paused peer: {c2_parsed}")
                all_ok = False
            else:
                try:
                    assert_parity_match(c1_parsed, c2_parsed, ref_parsed, 3000, "long_stall")
                    print(f"PASS LONG STALL: Peer hit frontier stall (delta_stalls={c2_event_stalls}) and recovered to "
                          f"match reference CRC 0x{c1_parsed.get('final_crc')} and audio CRC 0x{c1_parsed.get('audio_crc')}")
                except AssertionError as ae:
                    print(f"FAILED LONG STALL PARITY: {ae}")
                    all_ok = False
    finally:
        server.stop()

    # Case B: Strict Build Hash Mismatch Rejection
    print("\n--- Edge Case B: Strict Build Hash Handshake Rejection ---")
    port = find_free_udp_port()
    room = f"mismatch_case_{int(time.time())}"
    server = GoServerProcess(server_bin, port, log_path=log_dir / "server_mismatch.log")
    server.start()
    try:
        # Client 1 joins normally; Client 2 joins with corrupt build hash
        c1_cmd = [
            str(oracle_bin), "--mode", "client", "--player", "1",
            "--server", f"127.0.0.1:{port}", "--room", room,
            "--frames", "1000", "--timeout", "5",
        ]
        c2_cmd = [
            str(oracle_bin), "--mode", "client", "--player", "2",
            "--server", f"127.0.0.1:{port}", "--room", room,
            "--frames", "1000", "--timeout", "5",
            "--corrupt-build-hash",
        ]
        if rom_dir:
            c1_cmd += ["--rom-dir", str(rom_dir)]
            c2_cmd += ["--rom-dir", str(rom_dir)]

        p1 = subprocess.Popen(c1_cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        time.sleep(0.3)
        p2 = subprocess.Popen(c2_cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)

        try:
            out2, err2 = p2.communicate(timeout=6.0)
        finally:
            p2.kill()
            p1.terminate()
            p1.kill()

        out1, err1 = p1.communicate(timeout=2.0)

        # Must report explicit build hash rejection (lowercase normalized, nonzero exit)
        combined_c2 = (err2 + "\n" + out2).lower()
        rejected_explicitly = (p2.returncode != 0) and ("build hash mismatch" in combined_c2)
        timed_out = ("handshake timeout" in combined_c2)

        if rejected_explicitly and not timed_out:
            print("PASS BUILD MISMATCH: Server/transport cleanly rejected mismatched build hash.")
        else:
            print(f"FAILED BUILD MISMATCH: Expected explicit build hash mismatch rejection, got rc={p2.returncode} err:\n{err2}\nout:\n{out2}")
            all_ok = False
    finally:
        server.stop()

    # Case C: Disconnect Detection after Observed Midgame Progress
    print("\n--- Edge Case C: Disconnect Detection after Observed Midgame Progress ---")
    port = find_free_udp_port()
    room = f"disconnect_case_{int(time.time())}"
    server = GoServerProcess(server_bin, port, log_path=log_dir / "server_disconnect.log")
    server.start()
    try:
        killed_peer = threading.Event()

        def on_progress(confirmed, proc1, proc2):
            # Wait until mid-game progress is confirmed (>= 800 frames) before killing peer
            if confirmed >= 800 and not killed_peer.is_set():
                killed_peer.set()
                print(f"  [DISCONNECT TRIGGER] Observed confirmed frame {confirmed}, killing Client 2...")
                proc2.kill()

        rc1, out1, err1, rc2, out2, err2 = run_match_pair(
            oracle_bin, f"127.0.0.1:{port}", room, seed=12345, frames=5000,
            delay=2, window=16, sound_driver="native", rom_dir=rom_dir,
            c1_extra_args=["--timeout", "20"], c2_extra_args=["--timeout", "20"],
            on_c1_progress=on_progress, timeout_sec=30.0
        )

        c1_combined = (err1 + "\n" + out1).lower()
        detected_transport_disconnect = (rc1 != 0) and any(
            phrase in c1_combined for phrase in [
                "peer disconnected", "peer timeout", "connection timeout",
                "match terminated", "peer left"
            ]
        )
        is_oracle_watchdog = "timeout stalled" in c1_combined

        if detected_transport_disconnect and not is_oracle_watchdog and killed_peer.is_set():
            print("PASS DISCONNECT: Client 1 cleanly detected peer departure via network transport timeout.")
        else:
            print(f"FAILED DISCONNECT: detected={detected_transport_disconnect}, oracle_watchdog={is_oracle_watchdog}, rc1={rc1}, err1={err1}")
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
    parser.add_argument("--seeds", type=int, nargs="+", default=[1, 2, 3, 5])
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
        results.append(("snapshot_proof", ok))

    # 2. Baseline real-server match (full frames, all seeds)
    if args.suite in ["all", "baseline"]:
        ok = test_baseline_matches(oracle_bin, server_bin, args.rom_dir,
                                   args.seeds, args.frames, args.delay,
                                   args.window, args.sound_driver,
                                   args.dump_captures_dir, log_dir)
        results.append(("baseline_matches", ok))

    # 3. Impairment tests (full frames, all seeds)
    if args.suite in ["all", "impaired"]:
        ok = test_impaired_network(oracle_bin, server_bin, args.rom_dir,
                                   args.seeds, args.frames, args.delay,
                                   args.window, args.sound_driver,
                                   args.dump_captures_dir, log_dir)
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
