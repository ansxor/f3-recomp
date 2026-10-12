"""End-to-end netplay lockstep regression test."""

import json
import os
import re
import subprocess
import tempfile
import time
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]

P1_INPUT_SCRIPT = """# Player 1 inputs
700+20 coin
800-2400 mash keys=start period=45 seed=7
1200-3500 mash keys=b1+b2 period=10 seed=3
"""

P2_INPUT_SCRIPT = """# Player 2 inputs
740+20 coin
850-2400 mash keys=start period=45 seed=11
1200-3500 mash keys=up+b3 period=15 seed=19
"""

POKE_INPUT_SCRIPT = """# Illegal poke script
700+20 coin
1000 poke 400000.w=1234
"""

PATH_A_SCRIPT = """# Path A (Machine A): host inserts 6 coins (3 P1, 3 P2), fires at frame 2065
680+15 coin
710+15 coin
740+15 coin
770+15 p2 coin
800+15 p2 coin
830+15 p2 coin
900-1400 mash keys=start period=45
1450+5 right
2000-end mash keys=b1 period=45 seed=5
"""

PATH_B_SCRIPT = """# Path B (Machine B): joiner inserts 2 coins (1 P1, 1 P2), fires at frame 2945
1500+20 coin
1540+20 p2 coin
1600-2000 mash keys=start period=45
2200+5 right
2700-end mash keys=b1 period=45 seed=9
"""

P1_MATCH_SCRIPT = """# Player 1 match inputs
1-30000 mash keys=b1 period=45 seed=5
"""

P2_MATCH_SCRIPT = """# Player 2 match inputs
1-30000 mash keys=b2 period=30 seed=13
"""


# Residue paths: the two peers reach the 2P-versus trigger by different attract/charsel histories (lobby trigger at machine
# frames 1600 and 1450, i.e. edge frames 1665 and 1515) that leave different player-structure residue in 0x401f40..0x40212f.
# With the old v3 sync set that residue stayed unsynced and leaked into the hash at the gate flip (DESYNC on 0x402078).
RESIDUE_HOST_SCRIPT = """# residue path 1: 1 coin each, start mash 900-1400, confirm at 1600
800+20 coin
820+20 p2 coin
900-1400 mash keys=start period=45
1500+5 right
1600+1 b1
"""

RESIDUE_JOINER_SCRIPT = """# residue path 2: start mash 800-1300 with a different period, confirm at 1450
700+20 coin
720+20 p2 coin
800-1300 mash keys=start period=29
1400+5 right
1450+1 b1
"""


def find_rom_dir() -> Path | None:
    env_dir = os.environ.get("F3_ROM_DIR") or os.environ.get("ROM_DIR")
    candidates = [
        Path(env_dir) if env_dir else None,
        ROOT / "roms/landmakrj",
        Path("/Users/darien/Workspace/f3-stuff/f3-recomp/roms/landmakrj"),
    ]
    for c in candidates:
        if c and c.is_dir() and (c / "e61-05.39").is_file():
            return c
    return None


def find_tool_binary() -> Path | None:
    candidates = [
        ROOT / "build-vcpkg/f3rt-tool",
        ROOT / "build-landmakrj/f3rt-tool",
        ROOT / "build/f3rt-tool",
    ]
    for c in candidates:
        if c.is_file() and os.access(c, os.X_OK):
            return c
    return None


def find_or_build_server_binary() -> tuple[list[str], Path | None]:
    candidates = [
        ROOT / "build-vcpkg/f3-netplay-server",
        ROOT / "build/netplay-server",
    ]
    for c in candidates:
        if c.is_file() and os.access(c, os.X_OK):
            return [str(c)], c

    res = subprocess.run(["make", "netplay-server"], cwd=ROOT, capture_output=True)
    if res.returncode == 0:
        for c in candidates:
            if c.is_file() and os.access(c, os.X_OK):
                return [str(c)], c

    return [], None


def start_server(server_cmd: list[str], log_dir: Path) -> tuple[subprocess.Popen, str]:
    proc = subprocess.Popen(
        [*list(server_cmd), "--port", "0", "--logdir", str(log_dir)],
        cwd=ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    line = proc.stdout.readline()  # "netplay-server: port N, public-address ..."
    match = re.search(r"port (\d+)", line)
    if not match or int(match.group(1)) == 0:
        proc.kill()
        raise RuntimeError(f"relay did not report a port: {line!r}\n{proc.stderr.read()}")
    return proc, f"127.0.0.1:{match.group(1)}"


def run_netplay_match(
    tool_bin: Path, rom_dir: Path, tcp_addr: str, frames: int, p1_script: Path | None, p2_script: Path | None
) -> tuple[str, str]:
    # Start Host
    host_cmd = [
        str(tool_bin),
        "netplay",
        "--server",
        tcp_addr,
        "--host",
        "--delay",
        "2",
        "--frames",
        str(frames),
        "--rom-dir",
        str(rom_dir),
    ]
    if p1_script:
        host_cmd.extend(["--inputs", str(p1_script)])

    host_proc = subprocess.Popen(
        host_cmd,
        cwd=ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )

    # Wait for room code
    room_code = None
    host_out_lines = []
    start_time = time.time()
    while time.time() - start_time < 10.0:
        line = host_proc.stdout.readline()
        if line:
            host_out_lines.append(line)
            if line.startswith("ROOM "):
                room_code = line.strip().split()[1]
                break
        time.sleep(0.05)

    assert room_code, f"Failed to get room code from host:\n{''.join(host_out_lines)}"

    # Start Joiner
    joiner_cmd = [
        str(tool_bin),
        "netplay",
        "--server",
        tcp_addr,
        "--join",
        room_code,
        "--frames",
        str(frames),
        "--rom-dir",
        str(rom_dir),
    ]
    if p2_script:
        joiner_cmd.extend(["--inputs", str(p2_script)])

    joiner_proc = subprocess.Popen(
        joiner_cmd,
        cwd=ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )

    host_remaining_out, host_err = host_proc.communicate(timeout=120)
    joiner_out, joiner_err = joiner_proc.communicate(timeout=120)

    host_full_out = "".join(host_out_lines) + host_remaining_out

    assert host_proc.returncode == 0, f"Host failed (code {host_proc.returncode}):\n{host_err}\n{host_full_out}"
    assert joiner_proc.returncode == 0, f"Joiner failed (code {joiner_proc.returncode}):\n{joiner_err}\n{joiner_out}"

    assert "DESYNC" not in host_full_out, f"Host reported desync:\n{host_full_out}"
    assert "DESYNC" not in joiner_out, f"Joiner reported desync:\n{joiner_out}"

    host_crc_match = re.search(r"state_crc=0x([0-9a-fA-F]+)", host_full_out)
    joiner_crc_match = re.search(r"state_crc=0x([0-9a-fA-F]+)", joiner_out)

    assert host_crc_match, f"Host did not report FINAL crc:\n{host_full_out}"
    assert joiner_crc_match, f"Joiner did not report FINAL crc:\n{joiner_out}"

    host_crc = host_crc_match.group(1).lower()
    joiner_crc = joiner_crc_match.group(1).lower()

    assert host_crc == joiner_crc, f"CRC mismatch between host and joiner: {host_crc} != {joiner_crc}"
    return host_crc, joiner_crc


def test_netplay_poke_script_rejected():
    rom_dir = find_rom_dir()
    tool_bin = find_tool_binary()
    if not rom_dir or not tool_bin:
        pytest.skip("Prerequisites missing")

    with tempfile.TemporaryDirectory() as tmpdir:
        poke_file = Path(tmpdir) / "poke.txt"
        poke_file.write_text(POKE_INPUT_SCRIPT)

        res = subprocess.run(
            [
                str(tool_bin),
                "netplay",
                "--server",
                "127.0.0.1:7845",
                "--host",
                "--inputs",
                str(poke_file),
                "--rom-dir",
                str(rom_dir),
            ],
            cwd=ROOT,
            capture_output=True,
            text=True,
        )
        assert res.returncode != 0
        assert "poke" in res.stderr.lower() or "poke" in res.stdout.lower()


def test_netplay_end_to_end_3000_frames():
    rom_dir = find_rom_dir()
    if not rom_dir:
        pytest.skip("ROM directory missing")

    tool_bin = find_tool_binary()
    if not tool_bin:
        pytest.skip("f3rt-tool binary missing (build f3rt-tool first)")

    server_cmd, _ = find_or_build_server_binary()
    if not server_cmd:
        pytest.skip("Go / netplay-server missing")

    with tempfile.TemporaryDirectory() as tmpdir:
        tmp_path = Path(tmpdir)
        p1_file = tmp_path / "p1.txt"
        p2_file = tmp_path / "p2.txt"
        log_dir = tmp_path / "server-logs"
        log_dir.mkdir(parents=True, exist_ok=True)

        p1_file.write_text(P1_INPUT_SCRIPT)
        p2_file.write_text(P2_INPUT_SCRIPT)

        server_proc, tcp_addr = start_server(server_cmd, log_dir)
        try:
            # 1. Session 1: Both P1 and P2 have distinct real button inputs (3000 frames)
            crc_both, _ = run_netplay_match(
                tool_bin,
                rom_dir,
                tcp_addr,
                frames=3000,
                p1_script=p1_file,
                p2_script=p2_file,
            )

            # Assert server log recorded non-zero inputs for both players
            time.sleep(0.5)
            log_files = list(log_dir.glob("*.json"))
            assert len(log_files) >= 1, "Server did not write session log JSON"
            log_data = json.loads(log_files[0].read_text())
            assert "inputs" in log_data, "Server log missing 'inputs' field"
            p1_logged = log_data["inputs"][0]
            p2_logged = log_data["inputs"][1]
            assert any(x is not None and x != 0 for x in p1_logged), "P1 logged inputs were all zero"
            assert any(x is not None and x != 0 for x in p2_logged), "P2 logged inputs were all zero"

            # 2. Session 2: Same server, but Joiner has empty script (P2 inputs are all zero)
            crc_p1_only, _ = run_netplay_match(
                tool_bin,
                rom_dir,
                tcp_addr,
                frames=3000,
                p1_script=p1_file,
                p2_script=None,
            )

            # Assert P2's inputs affected the simulation state (proves remote inputs reached both machines)
            assert crc_both != crc_p1_only, (
                f"P2's inputs had no effect on simulation state: crc_both={crc_both} == crc_p1_only={crc_p1_only}"
            )

        finally:
            server_proc.terminate()
            try:
                server_proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                server_proc.kill()


def run_differential_sync_match(
    tool_bin: Path,
    rom_dir: Path,
    tcp_addr: str,
    frames: int,
    host_script: Path,
    joiner_script: Path,
    host_match_script: Path,
    joiner_match_script: Path,
    disable_apply_sync: bool = False,
    extra_args: list[str] | None = None,
) -> tuple[str, str, int, str, str, int]:
    # Start Host
    host_cmd = [
        str(tool_bin),
        "netplay",
        "--server",
        tcp_addr,
        "--host",
        "--delay",
        "2",
        "--inputs",
        str(host_script),
        "--match-inputs",
        str(host_match_script),
        "--until-trigger",
        "--frames",
        str(frames),
        "--rom-dir",
        str(rom_dir),
    ] + (extra_args or [])

    host_proc = subprocess.Popen(
        host_cmd,
        cwd=ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )

    # Wait for room code
    room_code = None
    host_out_lines = []
    start_time = time.time()
    while time.time() - start_time < 30.0:
        line = host_proc.stdout.readline()
        if line:
            host_out_lines.append(line)
            if line.startswith("ROOM "):
                room_code = line.strip().split()[1]
                break
        time.sleep(0.05)

    assert room_code, f"Failed to get room code from host:\n{''.join(host_out_lines)}"

    # Start Joiner
    joiner_cmd = [
        str(tool_bin),
        "netplay",
        "--server",
        tcp_addr,
        "--join",
        room_code,
        "--inputs",
        str(joiner_script),
        "--match-inputs",
        str(joiner_match_script),
        "--until-trigger",
        "--frames",
        str(frames),
        "--rom-dir",
        str(rom_dir),
    ]
    if disable_apply_sync:
        joiner_cmd.append("--disable-apply-sync")
    joiner_cmd += extra_args or []

    joiner_proc = subprocess.Popen(
        joiner_cmd,
        cwd=ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )

    host_remaining_out, host_err = host_proc.communicate(timeout=180)
    joiner_out, joiner_err = joiner_proc.communicate(timeout=180)

    host_full_out = "".join(host_out_lines) + host_remaining_out

    return (
        host_full_out,
        host_err,
        host_proc.returncode,
        joiner_out,
        joiner_err,
        joiner_proc.returncode,
    )


def test_netplay_differential_sync_5000_frames():
    rom_dir = find_rom_dir()
    if not rom_dir:
        pytest.skip("ROM directory missing")

    tool_bin = find_tool_binary()
    if not tool_bin:
        pytest.skip("f3rt-tool binary missing (build f3rt-tool first)")

    server_cmd, _ = find_or_build_server_binary()
    if not server_cmd:
        pytest.skip("Go / netplay-server missing")

    with tempfile.TemporaryDirectory() as tmpdir:
        tmp_path = Path(tmpdir)
        path_a_file = tmp_path / "pathA.txt"
        path_b_file = tmp_path / "pathB.txt"
        p1_match_file = tmp_path / "p1_match.txt"
        p2_match_file = tmp_path / "p2_match.txt"
        log_dir = tmp_path / "server-logs"
        log_dir.mkdir(parents=True, exist_ok=True)

        path_a_file.write_text(PATH_A_SCRIPT)
        path_b_file.write_text(PATH_B_SCRIPT)
        p1_match_file.write_text(P1_MATCH_SCRIPT)
        p2_match_file.write_text(P2_MATCH_SCRIPT)

        server_proc, tcp_addr = start_server(server_cmd, log_dir)
        try:
            host_out, host_err, host_rc, joiner_out, joiner_err, joiner_rc = run_differential_sync_match(
                tool_bin,
                rom_dir,
                tcp_addr,
                frames=5000,
                host_script=path_a_file,
                joiner_script=path_b_file,
                host_match_script=p1_match_file,
                joiner_match_script=p2_match_file,
            )

            assert host_rc == 0, f"Host failed (code {host_rc}):\n{host_err}\n{host_out}"
            assert joiner_rc == 0, f"Joiner failed (code {joiner_rc}):\n{joiner_err}\n{joiner_out}"

            assert "TRIGGER frame=2065" in host_out, f"Host did not trigger at frame 2065:\n{host_out}"
            assert "TRIGGER frame=2945" in joiner_out, f"Joiner did not trigger at frame 2945:\n{joiner_out}"

            assert "DESYNC" not in host_out, f"Host reported desync:\n{host_out}"
            assert "DESYNC" not in joiner_out, f"Joiner reported desync:\n{joiner_out}"

            host_sync_match = re.search(r"FINAL frame=\d+ sync_hash=0x([0-9a-fA-F]+)", host_out)
            joiner_sync_match = re.search(r"FINAL frame=\d+ sync_hash=0x([0-9a-fA-F]+)", joiner_out)
            assert host_sync_match, f"Host did not report FINAL sync_hash:\n{host_out}"
            assert joiner_sync_match, f"Joiner did not report FINAL sync_hash:\n{joiner_out}"

            host_sync_hash = host_sync_match.group(1).lower()
            joiner_sync_hash = joiner_sync_match.group(1).lower()
            assert host_sync_hash == joiner_sync_hash, (
                f"sync_hash mismatch: host={host_sync_hash} != joiner={joiner_sync_hash}"
            )

            host_crc_match = re.search(r"state_crc=0x([0-9a-fA-F]+)", host_out)
            joiner_crc_match = re.search(r"state_crc=0x([0-9a-fA-F]+)", joiner_out)
            assert host_crc_match, f"Host did not report state_crc:\n{host_out}"
            assert joiner_crc_match, f"Joiner did not report state_crc:\n{joiner_out}"

            host_state_crc = host_crc_match.group(1).lower()
            joiner_state_crc = joiner_crc_match.group(1).lower()
            assert host_state_crc != joiner_state_crc, (
                f"Expected different state_crc between different attract paths, but both were {host_state_crc}"
            )

        finally:
            server_proc.terminate()
            try:
                server_proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                server_proc.kill()


def test_netplay_differential_sync_negative_control():
    rom_dir = find_rom_dir()
    if not rom_dir:
        pytest.skip("ROM directory missing")

    tool_bin = find_tool_binary()
    if not tool_bin:
        pytest.skip("f3rt-tool binary missing (build f3rt-tool first)")

    server_cmd, _ = find_or_build_server_binary()
    if not server_cmd:
        pytest.skip("Go / netplay-server missing")

    with tempfile.TemporaryDirectory() as tmpdir:
        tmp_path = Path(tmpdir)
        path_a_file = tmp_path / "pathA.txt"
        path_b_file = tmp_path / "pathB.txt"
        p1_match_file = tmp_path / "p1_match.txt"
        p2_match_file = tmp_path / "p2_match.txt"
        log_dir = tmp_path / "server-logs"
        log_dir.mkdir(parents=True, exist_ok=True)

        path_a_file.write_text(PATH_A_SCRIPT)
        path_b_file.write_text(PATH_B_SCRIPT)
        p1_match_file.write_text(P1_MATCH_SCRIPT)
        p2_match_file.write_text(P2_MATCH_SCRIPT)

        server_proc, tcp_addr = start_server(server_cmd, log_dir)
        try:
            host_out, _host_err, host_rc, joiner_out, _joiner_err, joiner_rc = run_differential_sync_match(
                tool_bin,
                rom_dir,
                tcp_addr,
                frames=500,
                host_script=path_a_file,
                joiner_script=path_b_file,
                host_match_script=p1_match_file,
                joiner_match_script=p2_match_file,
                disable_apply_sync=True,
            )

            # Negative control: when apply_sync is disabled, a DESYNC must be detected and reported
            desync_reported = ("DESYNC" in host_out) or ("DESYNC" in joiner_out) or (host_rc != 0) or (joiner_rc != 0)
            assert desync_reported, (
                f"Expected DESYNC when apply_sync was disabled, but both completed cleanly:\n"
                f"Host:\n{host_out}\nJoiner:\n{joiner_out}"
            )

        finally:
            server_proc.terminate()
            try:
                server_proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                server_proc.kill()


@pytest.mark.parametrize("swap_roles", [False, True], ids=["host-early-path", "host-late-path"])
def test_netplay_residue_paths_stay_in_sync(swap_roles):
    """Peers with different pre-trigger residue must not DESYNC once the whole gameplay block is synced (sync v4).

    The hash is ungated: it covers the 0x401f40..0x40212f block from the first CRC report, so any unsynced residue
    shows up at the first report (it used to show up at the gate flip, match frame ~345-540). Running the same scripts
    against a build with the v3 sub-range sync set reports `DESYNC frame=30` (checked manually with --crc-every 30).
    """
    rom_dir = find_rom_dir()
    if not rom_dir:
        pytest.skip("ROM directory missing")
    tool_bin = find_tool_binary()
    if not tool_bin:
        pytest.skip("f3rt-tool binary missing (build f3rt-tool first)")
    server_cmd, _ = find_or_build_server_binary()
    if not server_cmd:
        pytest.skip("Go / netplay-server missing")

    host_text, joiner_text = (
        (RESIDUE_JOINER_SCRIPT, RESIDUE_HOST_SCRIPT) if swap_roles else (RESIDUE_HOST_SCRIPT, RESIDUE_JOINER_SCRIPT)
    )
    with tempfile.TemporaryDirectory() as tmpdir:
        tmp_path = Path(tmpdir)
        host_file, joiner_file = tmp_path / "host.txt", tmp_path / "joiner.txt"
        p1_match_file, p2_match_file = tmp_path / "p1_match.txt", tmp_path / "p2_match.txt"
        log_dir = tmp_path / "server-logs"
        log_dir.mkdir()
        host_file.write_text(host_text)
        joiner_file.write_text(joiner_text)
        p1_match_file.write_text(P1_MATCH_SCRIPT)
        p2_match_file.write_text(P2_MATCH_SCRIPT)

        server_proc, tcp_addr = start_server(server_cmd, log_dir)
        try:
            host_out, host_err, host_rc, joiner_out, joiner_err, joiner_rc = run_differential_sync_match(
                tool_bin,
                rom_dir,
                tcp_addr,
                frames=2200,
                host_script=host_file,
                joiner_script=joiner_file,
                host_match_script=p1_match_file,
                joiner_match_script=p2_match_file,
                extra_args=["--crc-every", "30"],
            )
            assert "DESYNC" not in host_out and "DESYNC" not in joiner_out, f"DESYNC:\n{host_out}\n{joiner_out}"
            assert host_rc == 0, f"Host failed ({host_rc}):\n{host_err}\n{host_out}"
            assert joiner_rc == 0, f"Joiner failed ({joiner_rc}):\n{joiner_err}\n{joiner_out}"
            triggers = sorted(int(m) for m in re.findall(r"TRIGGER frame=(\d+)", host_out + joiner_out))
            assert triggers == [1515, 1665], f"unexpected trigger frames {triggers}"
            assert "frames_between_sync_and_start=0" in host_out and "frames_between_sync_and_start=0" in joiner_out
            sync_hashes = re.findall(r"FINAL frame=\d+ sync_hash=0x([0-9a-f]+)", host_out + joiner_out)
            assert len(sync_hashes) == 2 and sync_hashes[0] == sync_hashes[1], f"sync_hash differs: {sync_hashes}"
        finally:
            server_proc.terminate()
            try:
                server_proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                server_proc.kill()
