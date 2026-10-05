# Write a compatible client

Reuse `Machine`, `Session`, `Rollback` and `Transport` for another frontend. Correct UDP packets alone do not make a different emulator compatible. Snapshot portability and cross-implementation determinism are not promised.

## Configure and ready

Load `landmakrj`, register generated blocks, disable main fallback and sound tracing, and configure compatible audio/video simulation. Presentation geometry and EEPROM history can remain local. Build identity with `machine_identity(machine)`; do not substitute a Git revision for the generated fingerprint.

Create `Session` with server, room, requested slot, delay and host/join role. Require exactly one host, independent of slot. Pump and advance through preparation; do not wait at cold boot for `Transport::ready()`. The host reaches real versus selection using ordinary inputs. The guest may continue local play until the host snapshot arrives.

For a custom lifecycle rather than `Session`, preserve these invariants:

1. Validate identity and complementary roles during pairing; adopt host delay.
2. Save a drained, neutral-input canonical handoff using `save_sync_state` only at `versus_handoff_ready`.
3. Compress/transfer with metadata, bounded chunks, checksums and acknowledgements.
4. Preserve the guest's pre-handoff full local state until acceptance; parse/load the canonical snapshot and validate versus entry and the drained audio queue.
5. Call `accept_snapshot` with the loaded canonical CRC. Both clients must cross the loaded barrier before constructing rollback.

## Rollback loop

Frames are match-relative; `origin_frame()` retains host machine time. Use window 16 by default (16–32 accepted) and delay 0–8. The first delay frames are neutral and known.

Pump transport with current simulated/confirmed frontiers. Drain inputs and checksums, call `synchronize`, sample once when `needs_local_input`, then `advance`. A false advance is a bounded stall, not permission to stop pumping. Drain outgoing checksums and confirmed PCM even while stalled. Present only the newest corrected image.

Input words are active-high bits 0–10 (`0x7ff`). Preserve immutable actual input history; changed duplicates are errors. Input acknowledgements are cumulative and cannot jump gaps. Send oldest unacknowledged inputs/checksums first. Confirmation CRCs cover canonical state, not just RAM/pixels, every 60 frames.

Exclusive emulation-thread ownership is required. Host audio consumers must not drain machine audio behind rollback's back. HLE is a separate host reconciliation path, not an excuse to replay speculative accurate PCM.

## Return and rematch

Detect natural exit at a confirmed boundary, restore it with `restore_confirmed`, and negotiate the final canonical CRC through the relay. For finite runs, finish only at a confirmed target. A peer finish ACK alone is not the server verdict. On disconnect, restore confirmed state before local execution; a failed pre-barrier guest handoff restores its previous local state.

Destroy the old transport/session and ready a new one for a rematch, with a fresh host snapshot. The same room may be reused. There is no hot reconnect into the old rollback history.

## Verify

Exercise real relay handoff from independent solo histories, host-P2 assignment, differing geometry/requested delays, late input, stalls, rejection, natural exit and disconnect. Compare each match against its saved host handoff reference, including state, pixels, confirmed PCM and sample count. Exercise actual frontend remapping, menu capture and output too.

See [wire protocol](/developer/netplay/protocol), [frontend integration](/developer/netplay/frontend-integration), and [implementation/evidence](https://github.com/ansxor/f3-recomp/blob/main/docs/developer/IMGUI-NETPLAY.md). Source APIs: [session](https://github.com/ansxor/f3-recomp/blob/main/include/f3rt/netplay_session.hpp), [rollback](https://github.com/ansxor/f3-recomp/blob/main/include/f3rt/netplay.hpp), [transport](https://github.com/ansxor/f3-recomp/blob/main/include/f3rt/netplay_transport.hpp).
