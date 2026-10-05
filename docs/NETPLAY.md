# Land Maker versus netplay

Netplay is opt-in two-player rollback for Japanese Land Maker 2.01J (`landmakrj`). Use the same build and ROMs, and a trusted UDP relay/network. Ordinary local play, EEPROM history and independent presentation settings are supported; only the versus match is synchronized. Strict native main execution and accurate/native audio remain the defaults. `--audio-backend hle` opts both peers into approximate, independently reconciled audio; see [HLE evidence](developer/HLE-AUDIO.md).

## Start a match

Build the normal `landmakr` target and the standard-library Go relay in `netplay/server` (Go 1.22 or newer). Run the relay on an address reachable by both clients:

```sh
build/netplay-server -addr 0.0.0.0:9000

# Exactly one host and one guest; roles do not dictate player slots.
build/landmakr --netplay-host --netplay-server SERVER:9000 \
  --netplay-room example --netplay-player 1 --netplay-delay 2
build/landmakr --netplay-join --netplay-server SERVER:9000 \
  --netplay-room example --netplay-player 2
```

Alternatively open **F1**, select the server, room, player slot and delay in the netplay tab, then choose **Host** or **Join**. Host/Join marks that client ready. Omit `--netplay-player` for automatic assignment. CLI players are 1/2; internal slots are 0/1. Room codes are 1–32 ASCII letters, digits, `_` or `-`. The host's delay (0–8 frames, default 2) is authoritative even if the guest requested another value.

Both clients can play locally while waiting. Once paired, the host automatically prepares two-player character selection through ordinary coin/start/confirmation inputs and sends its canonical state. The guest's prior solo history is replaced at this handoff. Both wait for snapshot acceptance before rollback starts; no game RAM is patched. A natural confirmed match end, disconnect or error returns to local play. Choose Host/Join again for a fresh handoff, including in the same room.

## Controls and menu

Network play uses the local **P1 binding profile** for whichever player slot you were assigned. Defaults are arrows, Z/X/C, 1 (start), 5 (coin), F3 (service), and F2 (test). Offline P1 and P2 profiles can be remapped independently; offline P2 defaults only to 2 (start) and 6 (coin). Gamepads require explicit bindings.

F1 opens the menu; Escape closes an open menu, or quits when it is closed. Solo play pauses with the menu open. Network simulation continues, with local gameplay input neutral while the menu is open. F12 writes a PNG. Host volume and GPU postprocessing are presentation/output controls, not synchronized game inputs. Save preferences explicitly; CLI options override saved preferences. Local state slots 0–9 are offline-only and require compatible build, ROMs and snapshot geometry.

The title/menu reports lifecycle, assigned player, transfer progress and rollback/network status. Strict-native execution is required; interpreter fallback and sound tracing are not supported in a session. Native 320×232 output and accurate/native audio remain the defaults. Expanded/GPU presentation is allowed and need not match the peer.

## Network limits

The client transport uses POSIX sockets and has been exercised on macOS; this is not a Windows client port. A missing peer times out after 8 seconds. Waiting for a room is bounded by 120 seconds (10 seconds without relay replies); snapshot/start-barrier preparation must also finish within the transport's 120-second deadline. There is no hot reconnect, encryption, account authentication or anti-cheat. Room names are routing identifiers, not secrets.

See [ImGui and versus-netplay developer contract](developer/IMGUI-NETPLAY.md) for the exact latch, snapshot validation, wire-v2 bounds, audio invariants, observed cutover evidence and clearly separated historical measurements.
