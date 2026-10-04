# Write a compatible client

**What you will learn.** Integrate the existing C++ core into another frontend. Learn the invariants needed for an independent protocol implementation.

Read in this order: [Build identity](/developer/netplay/build-identity), [Rollback engine](/developer/netplay/rollback), [Transport](/developer/netplay/transport), then [Wire protocol](/developer/netplay/protocol). The existing [frontend loop](/developer/netplay/frontend-integration#loop-order) is the implementation model.

## Choose the scope

Reuse `Machine`, `Rollback` and `Transport` when you only need another host frontend. This keeps prediction, snapshots and wire parsing in one implementation.

An independent transport must implement the version-1 bytes and behavior below. An independent emulator also needs a compatible machine state and build identity. Correct UDP packets alone cannot make a different emulator compatible. There is no standardized portable snapshot schema or cross-implementation compatibility guarantee.

## 1. Configure the machine before identity

Load the supported ROM set. Disable main-CPU fallback. Register generated blocks. Enable native sound and GameVideo at scale 1, border 0. Leave EEPROM erased. Do not run a frame or produce audio yet.

Use exclusive emulation-thread ownership. Do not let a host audio consumer drain `Machine::audio` while rollback owns it. Do not reconfigure devices after `state_size()` establishes storage.

Create `Identity` with `machine_identity(machine, delay)`. Do not substitute a commit ID for the generated fingerprint. Validate delay as 0 to 8 and room as 1 to 32 ASCII letters, digits, `_` or `-`.

## 2. Join without advancing the machine

Create `Transport` with server, room, desired player and delay. Pump it with `(0, 0)` until `ready()` is true. Handle failure as terminal for that match.

For an independent transport:

1. Use a non-zero random 64-bit nonce for the client lifetime.
2. Send the 135-byte `JoinReq`. Retry every 80 ms while connecting.
3. Accept only replies for that nonce from the relay endpoint.
4. On `MatchStart`, check minimum length, slot 0 or 1, non-zero session, exact delay and every identity field.
5. Keep the assigned slot and session for later packets.

The standard socket is connected UDP. It does not accept packets from another endpoint. Do not change this filtering into a claim of cryptographic authentication.

## 3. Construct rollback at frame zero

After the handshake, create `Rollback(machine, slot, delay, window)`. The default window is 16. It accepts 16 to 32.

The constructor marks the first `delay` frames neutral and known for both players. It saves frame 0 and allocates full snapshots. Do not submit the neutral prefix as real controller input. The first transport input frame is `delay`.

## 4. Run one coordinated loop

Use this order on each loop pass:

1. Call `pump(frame(), confirmed_frame())`.
2. Drain `Transport::receive` into `Rollback::receive`.
3. Drain `Transport::receive_checksum` into `Rollback::receive_checksum`.
4. Call `synchronize()` even when no new step is due.
5. When pacing permits and the target is not reached, sample the local word only if `needs_local_input()` is true. Submit the returned `Input` to transport.
6. Call `advance()`. A false result is a bounded stall, not an error.
7. Drain `receive_checksum_to_send` into `Transport::checksum`.
8. Drain all `render_audio` output, including during stalls.
9. Present only the newest pixels after the whole synchronization or step.

A stall does not end socket pumping. A correction does not emit an intermediate framebuffer or speculative PCM. Do not write the current controller state directly into machine ports outside `apply_inputs`.

## 5. Preserve input reliability

Inputs form one contiguous sequence per player. Each `GameData` repeats the oldest unacknowledged run, at most 128 words. It stops at the first missing local frame.

The input ACK is cumulative and inclusive. It is the highest remote frame delivered in order to the application. It starts at `delay - 1`, or `0xffffffff` for delay 0. Receiving a later word does not allow an ACK past a gap.

Preserve received words in tagged history after delivery. An equal duplicate is harmless. A changed actual word for the same frame is fatal. Never interpret input mutation as a new prediction correction.

## 6. Preserve checksum reliability

Emit `Checksum{N, crc}` only after confirmation reaches a positive multiple of 60. The CRC covers the complete canonical state after N frames. It includes renderer and device state. It does not hash only RAM or the framebuffer.

Checksum ACK is separate from input ACK. Send the oldest pending checksums first. The standard client sends at most eight per packet; parsers accept at most 32. Keep repeating pending pairs until acknowledged.

The current receiver uses the highest queued received checksum frame, not a general gap bitmap. A sender that skips older pending checksums can hide a gap. Follow oldest-first order and never advance ACK for a pair that was not retained. Drain received checksums into rollback, where the stronger checksum-frame and mutation rules apply.

## 7. Match the parser contract

Encode all multi-byte wire fields big-endian with alignment-safe byte readers/writers. Do not cast an unaligned packet pointer to a native integer pointer.

For `GameData`, validate the whole structural payload before changing liveness or frontiers. Validate both flags fields, counts, exact length, frame-range overflow, valid input bits, checksum fields and optional finish fields. Apply semantic receive bounds too.

Read [Parser validation and asymmetries](/developer/netplay/protocol#parser-validation-and-asymmetries). The relay and client do not enforce identical checks. A compatible sender emits canonical values even where a current parser is permissive. Do not depend on an ignored reserved byte or unchecked ACK to implement a new feature.

Use the fixed vector in [Worked example](/developer/netplay/protocol#worked-example) to compare wire encoding. Use malformed packet cases from the Go tests to check truncation, counts, unknown flags and trailing bytes.

## 8. Finish a finite match correctly

Stop simulation at target N. Wait for confirmation to reach N. Drain confirmed audio. Call `finish(N, machine.state_crc())` once, then keep pumping and draining receive APIs.

Repeat the finish record in later packets. Receiving a peer finish record or finish-ACK flag is not enough. Exit normally only after a relay `MatchComplete` agrees with the local frame and CRC.

Send Leave code 1 only after this completion. Send code 0 for an abort while connected. The retained relay verdict lets the second finisher recover a lost verdict after the first client leaves. It is not durable and expires after room inactivity.

## 9. Verify through the real relay

Run a single-machine delayed reference and a real two-process client pair. Compare exact target frames, state CRC, framebuffer CRC, PCM CRC and stereo-frame count. Repeat with impairment and require actual rollbacks.

Exercise delayed input, a recoverable full-window stall, build mismatch and peer departure. Test the delay endpoints 0 and 8. Use the [Oracle](/developer/netplay/oracle) for these contracts. Then exercise the actual input, audio and presentation surface of the new frontend.

## Source

- [Rollback API](https://github.com/ansxor/f3-recomp/blob/main/include/f3rt/netplay.hpp).
- [Transport API](https://github.com/ansxor/f3-recomp/blob/main/include/f3rt/netplay_transport.hpp).
- [Frontend integration](https://github.com/ansxor/f3-recomp/blob/main/runtime/frontend.cpp).
- [Go protocol tests](https://github.com/ansxor/f3-recomp/blob/main/netplay/server/protocol_test.go).
