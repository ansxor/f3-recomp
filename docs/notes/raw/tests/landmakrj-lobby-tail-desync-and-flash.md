---
source_url: "build/np_hold_sim, build/np_tile (copied: landmakrj-np-hold-sim.cpp.txt, landmakrj-np-tile.cpp.txt)"
ingested: 2026-10-11
sha256: 90f387bc16ab53c0464b5af783f2498ed5c8d6f7b1ea7362723b436888b29cdf
game: landmakrj
rom_sha256: aa050f39f5810c90bc20d1e3ed1c2510b1df067242535ad80aa86b3d174d49bc
captured_with: "harness against build-landmakrj/libf3rt.a, worktree-netplay"
---
# Land Maker lobby flow: post-sync frame-count desync, and the GAME SELECT flash counter

Harness sources (copied here): `landmakrj-np-hold-sim.cpp.txt` (two peers through confirm -> hold N -> restore -> run to edge -> capture/apply_sync -> optional post-sync lobby tail -> lockstep with `on_match_frame`), `landmakrj-np-tile.cpp.txt` (tile screenshots and cancel/play-locally checks). Real session inputs: `landmakrj-ff46-session-inputs.txt` (two columns P1 P2 per match frame). Build: `build/np_build.sh` (cc -O2 against `build-landmakrj/libf3rt*.a`, after `ninja f3rt f3rt-runner`: a stale `libf3rt.a` had the pre-lobby `lobby_trigger` and never fired).

## 1. Real session (relay log /tmp/np-logs/ff46d13f68adb119.json)

- crc: `compared 29, mismatch_count 1`, only `{frame: 1740, crc_p1: 3739394368, crc_p2: 2297936545}` (frames 60..1680 equal, so the full-block hash matched at 1560/1620/1680).
- seed `e3b669ac5fab33c5`, input_delay 2, sync_blob 110 B v3, end_reason disconnect.
- Inputs (P1, P2 words): non-zero only at frames 150-153 P2 right, 175-180 P2 left, 198-201 P2 b1. Nothing from 202 to 1744 (long gap of 1540 frames, both sides). So the 1740 desync is not input related.
- `frames_between_sync_and_start` was not recorded (recorder meta did not exist in that run).

## 2. Mechanism found: frames run after the sync point

`session.cpp` calls `capture_sync` (host, on peer match) and `apply_sync` (joiner, on blob arrival) inside `Session::poll`. The frontend's `in_lobby` stays true while `netplay_session != nullptr && !netplay_active`, so the main loop keeps stepping `apply_local_inputs; lobby_hold_frame; run_frame; lobby_hold_frame` on the already-synced machine until the session state is Running (UDP registering, >=100 ms steps; host and joiner wait different times). Each extra frame advances game logic and applies the Menu pins (`0x401f6e=0`, `0x407902=1`, ...) to a machine already in character select.

Reproduction (`np_hold_sim`, args: scriptA holdA scriptB holdB frames inputs seed varA varB tailA tailB). `tail` = extra pinned lobby frames after sync:

```
tail 0 0   (holds 100/20, same script)  NO DESYNC through 1900 frames
tail 6 6                                NO DESYNC through 1900 frames
tail 2 1                                DESYNC at completed frame 1349  0x401f53: A=03 B=43
tail 5 4                                DESYNC at completed frame 1346  0x401f53: A=03 B=43
tail 12 11                              DESYNC at completed frame 138   0x401f53: A=83 B=c3
tail 10 3                               DESYNC at completed frame 140   0x401f53: A=83 B=c3
tail 1 0 / 3 0                          DESYNC at completed frame 1      0x401f6f: A=00 B=01 (pin writes 0x401f6e=0)
```

Latent from frame 1/2/5: `0x401fb0..b3` and `0x402058..5b` (A=ff ff ff f6, B=00) differ while the pre-gate hash (109 B) does not cover them; the divergence of `0x401f53` (players-active byte, in the synced mode block) shows when the offset charsel steps differ. 1346-1349 is the same kind of failure the user saw at 1740 (late, after the gate).

## 3. Everything else equal: no desync

Tail 0 on both peers, hold durations 0..3000 frames, pre-trigger paths differing in coin timing/credits/start mash/confirm frame (1500, 1533, 1700), real-log inputs 1900 frames and mashed inputs 8000 frames (match ends ~4200, crc constant 0x11801539 thereafter):

```
1 0 1 400 / 0 700 3 0 / 4 90 1 1500 / 2 3000 3 33   tail 0 0   NO DESYNC through 8000 frames; final crc=11801539
audio variants (finish_frame in hold/edge on one peer only)     NO DESYNC through 1900 frames
```

Residue bytes that differ after the edge run but are overwritten before they matter: `0x401ffb 401fff 402003 4020a3 4020a7 4020ab` (and excluded `402033 4020db 4020f7`) for different pre-trigger scripts. They never desynced the hash over 8000 frames.

## 4. GAME SELECT flash counter 0x407903

`f3a xref 0x407903`: readers/writers `sub_09ecf6` (0x09ecfc read, 0x09ed50 `addq.b #1`, 0x09ed54 `cmpi.b #$40` -> `move.b #$ff,-$6fe(a5)` at 0x09ed5c), `sub_09d878` (same counter in another menu), `sub_09ce82` (clears with `clr.w -$6fe(a5)`).

`sub_09ecf6` (called every frame, `tst.b 0x407902 ble` skip): if counter <= $20 and (counter & 4) == 0 it appends a sprite command (tile code $ac, positioned from table indexed by cursor `0x4078f6`) = the white flash overlay; then counter += 1; exit when counter > $40. So the overlay shows for counter in {0-3, 8-11, 16-19, 24-27, 32}, blinking 4 on / 4 off, and is never drawn for counter 0x21..0x40.

Harness `np_tile old|fixed:30|cycle 300` (trigger frame 1500: 407903=01):

```
old       hold 300 frames: 407903 range seen after pin [01..01]   -> tile solid white (assets/landmakrj-tile-old-060.png)
fixed:30  hold 300 frames: 407903 range seen after pin [30..30]   -> gold glow + orange border (assets/landmakrj-tile-fixed30-060.png, -300.png)
cycle     hold 300 frames: range [21..3e]                         -> byte-identical frames to fixed:30
play-locally (all rules): edge after 65 frames, 401f6e=1 401f82=4e
cancel (all rules): 407902=0 407903=0 407908=04b0 4078f6=2 ; after 120 frames with left pressed: 4078f6=1 timer=0438 407902=0 refire=0
```
