# Online play (netplay)

Netplay connects two players for versus play in Land Maker (`landmakrj`). Solo play stays local. Both clients run the game; a UDP relay forwards the canonical handoff snapshot, inputs and checksums, not video.

## Start the relay

Build and run it on a machine both players can reach:

```sh
(cd netplay/server && go build -o ../../build/netplay-server .)
build/netplay-server -addr 0.0.0.0:9000
```

Without `-addr`, it listens on `127.0.0.1:9000` for local testing only. Open the UDP port in the firewall. See the [CLI reference](/reference/cli) for relay impairment options; those are test aids, not required settings.

## Ready, prepare, play

1. Start each game normally. Independent EEPROM settings and previous solo play are allowed.
2. Open **F1 → Netplay**. Enter the same server and room, choose a requested player slot (or automatic), and select **Host** on one client and **Join** on the other. These actions mark the clients ready. There must be exactly one host; hosting does not imply player 1.
3. The host automatically prepares a versus game through the game's ordinary coin, start and confirmation inputs. Both players must be active and at least one must still be choosing a character. Follow the menu status; no RAM patches or interpreter fallback are used. If preparation cannot finish, exit service or finish the current game locally, then Host again.
4. The host sends a fresh canonical snapshot. The guest loads it into its own presentation geometry. Rollback begins only after both clients have loaded it.
5. A confirmed natural exit from versus, or a disconnect, returns to local play. To rematch, ready both clients again and prepare a new handoff. You can reuse the same room; no process restart is required.

CLI alternatives:

```sh
# Host (may request either player slot)
build/landmakr --netplay-host --netplay-server SERVER:9000 --netplay-room example --netplay-player 1 --netplay-delay 2
# Guest; its requested delay does not override the host
build/landmakr --netplay-join --netplay-server SERVER:9000 --netplay-room example --netplay-player 2
```

Server, room, slot and delay are saved by **Save preferences**. Explicit CLI flags override the loaded preferences. `--netplay-host` or `--netplay-join` is required for CLI entry; specifying connection settings is not a substitute for choosing a role.

| Setting | Default | Limits |
| --- | --- | --- |
| Server | `127.0.0.1:9000` | Reachable UDP relay |
| Room | empty | 1–32 letters, digits, `_` or `-` |
| Requested player | automatic | CLI `1` or `2`; a taken slot is rejected |
| Host input delay | 2 frames | 0–8; host authoritative |

## Controls and presentation

Network play uses your **local P1 input profile** for whichever player slot you receive. Defaults are arrows, `Z`/`X`/`C`, `1` start, `5` coin, `F3` service and `F2` test. Remap keyboard or gamepad controls in F1; there are no default gamepad bindings. See [Controls and options](/guide/running).

F1 opens the menu without pausing network simulation. While it is open, your game input is neutral. Focus loss also releases input. Coin, service and test travel through the same delayed input path; test is shared. F12 saves a local PNG screenshot.

Presentation is local: scale, border, filtering, GPU shaders and host volume need not match. Native pixels and synchronization checksums are not postprocessed. The default accurate audio path plays only confirmed PCM; the optional HLE backend has its own non-rewound reconciliation, not accurate-PCM equivalence. See [Sound](/guide/sound).

## Compatibility and limits

- Use matching ROM region CRCs, build fingerprints and simulation/state-format settings. There is no EEPROM, initial-state, presentation or requested-delay equality requirement.
- Main execution must be strict native, with no interpreter fallback or sound tracing. A mismatched identity is rejected; do not bypass it.
- Input delay trades responsiveness for fewer rollbacks. One native frame is about 17 ms. Deep corrections can stall presentation; a 16-frame history does not guarantee a correction fits into one display frame.
- Snapshot handoff must complete within 120 seconds; preparation is bounded to 3600 frames. A silent peer times out after 8 seconds. A failed session returns local rather than reconnecting into that old rollback session.
- Two players only; no spectators, authentication, encryption or anti-cheat. Room names are routing labels, not passwords. Trust the relay and network.
- The client uses POSIX sockets. Do not infer cross-platform build compatibility from the protocol.

## If the match does not start

| Message or state | Action |
| --- | --- |
| ROM CRC mismatch | Use the same supported ROM set. |
| Build hash mismatch | Match source, generated code, compiler, platform and build options. |
| Snapshot format mismatch | Match audio/video simulation settings and state format. |
| Host/join role conflict | Choose exactly one Host and one Join. |
| Requested slot already taken | Choose a different slot or automatic assignment. |
| Waiting for handoff | The host prepares real versus selection with ordinary game inputs. Tutorial/demo screens are not handoff points; follow status, and leave service or finish an existing local game if preparation fails. |
| Transfer/peer timeout | Check relay reachability; ready a fresh session after local return. |

For implementation, exact versus predicates, shader ABI and observed verification, see [ImGui and versus-netplay evidence](https://github.com/ansxor/f3-recomp/blob/main/docs/developer/IMGUI-NETPLAY.md) and the [developer netplay overview](/developer/netplay/).
