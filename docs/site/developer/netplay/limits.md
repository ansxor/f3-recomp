# Limits and security

**What you will learn.** Learn the supported modes, storage bounds and trust boundary. Learn what rollback can recover and what requires a new match.

Use [How netplay works](/developer/netplay/) for the design. Use [Oracle and verification](/developer/netplay/oracle) for acceptance evidence.

## Supported scope

Player netplay supports two players of Japanese Land Maker 2.01J (`landmakrj`). Each client runs the complete machine. The Go relay pairs endpoints and forwards controller packets. It is not an authoritative game server.

Main execution must be strict native without fallback or sound tracing. Independent EEPROM/solo histories and presentation geometry are supported. Only versus runs in rollback: local → lobby → host preparation → canonical snapshot → both-loaded barrier → versus → confirmed local return.

Clients must match protocol, ROM region CRCs, build fingerprint and audio/video state format. Host role and player slot are independent. Host delay is authoritative, not an equality requirement.

## Core storage bounds

Values in this table come from [netplay.hpp](https://github.com/ansxor/f3-recomp/blob/main/include/f3rt/netplay.hpp) and [netplay.cpp](https://github.com/ansxor/f3-recomp/blob/main/runtime/netplay.cpp).

| Item | Bound | Result at the bound |
| --- | --- | --- |
| Players | Slots 0 and 1 | Other constructor slots fail |
| Input word | Bits 0 to 10, mask `0x7ff` | Unknown bits fail |
| Input delay | 0 to 8; default 2 | Other values fail |
| Prediction window | 16 to 32; default 16 | `advance()` returns false at `frame - confirmed >= window` |
| Snapshot ring | `window + 1` full local snapshots | Includes configured expanded buffers; tagged frames detect overwrites |
| Input history | 1024 tagged frames | Peer inputs at least 512 frames ahead fail; inputs at least 512 frames behind confirmation are ignored |
| Per-frame PCM | 4096 interleaved `int16_t` values, or 2048 stereo frames | Remaining device audio after this drain fails |
| Confirmed PCM queue | `4096 * (32 + 2)` values, or 69,632 stereo frames | Promotion fails if the caller does not drain it |
| Core outgoing checksums | 64 pairs | Confirmation fails if the caller does not drain them |
| Checksum cadence | Every 60 confirmed frames | Invalid peer checksum frames fail |
| Core hash records | 1024 entries, indexed by `(frame / 60) % 1024` | Frames are separately tagged |
| Frame counter | Below `UINT32_MAX - 1024` | Reaching the guard fails; start a new match |

The canonical native sync snapshot is 4,231,509 bytes. At unexpanded geometry, full local snapshots are the same size and the 17-slot ring is 71,935,653 bytes (~68.6 MiB). Expanded 2x/border-48 local state is 6,547,797 bytes and costs more ring storage. Inputs/audio/checksums, machine/ROM data and host output are additional.

Save/load allocate no memory on the valid path. Full local state includes expanded buffers; canonical sync state omits them. Loading validates unsafe fields as well as byte count. Construction, status strings and capture tooling are outside the zero-allocation claim.

## Transport bounds

Values come from [netplay_transport.cpp](https://github.com/ansxor/f3-recomp/blob/main/runtime/netplay_transport.cpp).

| Item | Value |
| --- | --- |
| Protocol version | 2 |
| Header / identity | 20 / 64 bytes |
| Declared maximum datagram | 1400 bytes |
| Local / remote input rings | 2048 entries each |
| Input backpressure limit | Submitted frame may be at most 512 above the effective peer ACK |
| Checksum history and each queue | 512 entries |
| Outstanding ping table | 64 entries |
| Input words per sent packet | At most 128 |
| Checksum pairs per sent packet | At most 8; parser accepts up to 32 |
| Maximum standard client `GameData` | 378 bytes including header and finish record |
| Regular connected send interval | At least 8 ms; nominal maximum 125 packets/s |
| Idle send interval | 20 ms |
| Join retry interval | 80 ms |
| No server reply / total waiting | 10 s / 120 s limits, checked with whole-second durations and `>` |
| Peer silence | More than 8000 ms without accepted traffic |
| Snapshot cap / chunk / transfer window | 16 MiB / 1024 bytes / 32 chunks |
| Snapshot handoff deadline | 120 s |
| Host preparation | At most 3600 frames |

`finish()` sends immediately outside the regular 8 ms pacing gate. The relay packet limit is enforced on ingress. The C++ receive buffer has 1528 bytes and does not independently reject every datagram above 1400 in its common header handler. Do not treat the declared packet limit as complete validation in every code path.

The checksum receiver records the highest queued received checksum frame. It does not maintain a general gap bitmap. The normal sender repeats the oldest pending checksums first. A custom sender must preserve that rule. Full incoming or outgoing checksum queues do not grow: a new pair can be omitted. Drain the APIs promptly. These queues are not durable storage.

## Relay bounds

Values come from [server.go](https://github.com/ansxor/f3-recomp/blob/main/netplay/server/server.go), [protocol.go](https://github.com/ansxor/f3-recomp/blob/main/netplay/server/protocol.go) and [impairment.go](https://github.com/ansxor/f3-recomp/blob/main/netplay/server/impairment.go).

| Item | Value |
| --- | --- |
| Rooms | 1024 maximum |
| Tracked per-IP limiters | 4096 maximum; arbitrary map entry eviction at capacity |
| Rate refill / burst | 1000 packets/s / 500 tokens per source IP |
| Packet size | 20 to 1400 bytes |
| Inputs / checksums per parsed `GameData` | At most 128 / 32 |
| Active client timeout | More than 8 s without endpoint-valid forwarded traffic or accepted join activity |
| Waiting room lifetime | More than 60 s from creation, even if join retries continue |
| Finished / terminated room retention | More than 30 s since room activity |
| Janitor cadence | 1 s |
| Inactive limiter retention | More than 60 s |
| Impairment delay queue | 2048 packets, including duplicates |

Two clients behind one router share a rate bucket. Other users behind that source IP share it too. The limiter drops excess packets silently. It is not an account quota or comprehensive denial-of-service defense.

Finished verdicts exist in memory only. The relay repeats them in response to valid finish traffic and other eligible traffic. Continuing traffic can refresh room activity. Once the room expires or the relay restarts, the verdict is gone.

## Recovery boundary

A short delay, loss or pause can cause prediction, replay and a bounded stall. Missing remote input never lets the simulation run beyond its window. The client resumes when actual input arrives before the connection timeout.

A disconnect restores the last confirmed boundary and returns local. There is no hot reconnect into the old rollback timeline. A fresh Host/Join creates a new snapshot handoff; the same room may be reused without restarting the process.

The relay can rebind an occupied slot to a new UDP address after a same-nonce, same-identity join. The standard connected C++ transport does not send new join requests after `ready()`. This relay feature is not a promise of automatic NAT-rebinding recovery in the frontend.

A correction outside retained history is an error. It is not replaced by a fabricated neutral input or an approximate state.

## Performance is not window size

The rollback window bounds history, not wall-clock cost. Deep corrections can take multiple display intervals. Accurate confirmed-only audio adds latency. Current measurements and lifecycle evidence live in [IMGUI-NETPLAY.md](https://github.com/ansxor/f3-recomp/blob/main/docs/developer/IMGUI-NETPLAY.md); older timing tables are not guarantees.

## Trust and validation boundary

The host's canonical snapshot travels through the relay compressed and checksummed. Guest parsing validates sizes and unsafe fields before acceptance. CRCs are corruption checks, not cryptographic authentication. Full offline save slots are a separate, build/ROM/geometry-compatible format.

The connected UDP socket accepts datagrams only from the resolved relay endpoint. The relay checks session ID, sender slot and registered source endpoint before forwarding gameplay traffic. This prevents ordinary stale-session and wrong-endpoint interference.

It does not provide authentication. Room names, nonces, session IDs and identities travel in clear text. There is no encryption or integrity MAC. A compromised relay or client is outside the trust model. A forged identity or collision is not ruled out by CRC equality.

The packet parsers also have distinct checks. The relay validates `GameData` payload lengths, flags, counts, frame-range overflow, input bits and trailing bytes. It does not compare header flags with payload flags or enforce every client semantic bound. Heartbeats are endpoint-checked but forwarded without a payload parse. Read [Wire protocol](/developer/netplay/protocol#parser-validation-and-asymmetries) before writing a compatible implementation.

## Features not provided

- More than two players, spectators or room discovery.
- Account authentication, encryption or anti-cheat.
- Authoritative server emulation or video streaming.
- Portable snapshots, cross-build saves or cross-platform determinism guarantees.
- Hot resume of disconnected rollback history or late joining an active match.
- Dynamic input delay, automatic window selection or congestion control.
- Durable results or replay storage on the relay.

Use a trusted relay and reachable UDP port. A room code is not a password. Ready a fresh Host/Join after local return to reuse the room.
