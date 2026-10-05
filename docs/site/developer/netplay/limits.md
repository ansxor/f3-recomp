# Limits and security

**What you will learn.** Learn the supported modes, storage bounds and trust boundary. Learn what rollback can recover and what requires a new match.

Use [How netplay works](/developer/netplay/) for the design. Use [Oracle and verification](/developer/netplay/oracle) for acceptance evidence.

## Supported scope

Player netplay supports two players of Japanese Land Maker 2.01J (`landmakrj`). Each client runs the complete machine. The Go relay pairs endpoints and forwards controller packets. It is not an authoritative game server.

The frontend supports strict-native main CPU, native sound and GameVideo at scale 1, border 0. It cold-boots erased EEPROM and rejects persistence and diagnostic execution modes. The headless oracle tests more snapshot configurations. Those tests do not expand the supported frontend contract.

Both clients must agree on protocol, loaded ROM region CRCs, build fingerprint, settings, EEPROM CRC, initial-state CRC and delay. Compatibility is deliberately conservative. Do not bypass a mismatch to force a match.

## Core storage bounds

Values in this table come from [netplay.hpp](https://github.com/ansxor/f3-recomp/blob/main/include/f3rt/netplay.hpp) and [netplay.cpp](https://github.com/ansxor/f3-recomp/blob/main/runtime/netplay.cpp).

| Item | Bound | Result at the bound |
| --- | --- | --- |
| Players | Slots 0 and 1 | Other constructor slots fail |
| Input word | Bits 0 to 10, mask `0x7ff` | Unknown bits fail |
| Input delay | 0 to 8; default 2 | Other values fail |
| Prediction window | 16 to 32; default 16 | `advance()` returns false at `frame - confirmed >= window` |
| Snapshot ring | `window + 1` full snapshots | Absolute tags detect an overwritten snapshot; no guessed restore |
| Input history | 1024 tagged frames | Peer inputs at least 512 frames ahead fail; inputs at least 512 frames behind confirmation are ignored |
| Per-frame PCM | 4096 interleaved `int16_t` values, or 2048 stereo frames | Remaining device audio after this drain fails |
| Confirmed PCM queue | `4096 * (32 + 2)` values, or 69,632 stereo frames | Promotion fails if the caller does not drain it |
| Core outgoing checksums | 64 pairs | Confirmation fails if the caller does not drain them |
| Checksum cadence | Every 60 confirmed frames | Invalid peer checksum frames fail |
| Core hash records | 1024 entries, indexed by `(frame / 60) % 1024` | Frames are separately tagged |
| Frame counter | Below `UINT32_MAX - 1024` | Reaching the guard fails; start a new match |

The native snapshot has 4,231,509 bytes. The default 17-snapshot ring has 71,935,653 bytes, about 68.6 MiB. This is the snapshot ring alone. Input, audio and checksum arrays, machine RAM, decoded ROM data and host output need more memory.

Save/load allocate no memory on the valid path. The caller owns exact-size buffers. `state_crc()` lazily allocates scratch. Transport construction, status strings, diagnostics and oracle capture work are not part of the zero-allocation save/load claim.

## Transport bounds

Values come from [netplay_transport.cpp](https://github.com/ansxor/f3-recomp/blob/main/runtime/netplay_transport.cpp).

| Item | Value |
| --- | --- |
| Protocol version | 1 |
| Header / identity | 20 / 72 bytes |
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

`finish()` sends immediately outside the regular 8 ms pacing gate. The relay packet limit is enforced on ingress. The C++ receive buffer has 1528 bytes and does not independently reject every datagram above 1400 in its common header handler. Do not treat the declared packet limit as complete validation in every code path.

The checksum receiver records the highest queued received checksum frame. It does not maintain a general gap bitmap. The normal sender repeats the oldest pending checksums first. A custom sender must preserve that rule. Full incoming or outgoing checksum queues do not grow: a new pair can be omitted. Drain the APIs promptly. These queues are not durable storage.

## Relay bounds

Values come from [server.go](https://github.com/ansxor/f3-recomp/blob/main/netplay/server/server.go), [protocol.go](https://github.com/ansxor/f3-recomp/blob/main/netplay/server/protocol.go) and [impairment.go](https://github.com/ansxor/f3-recomp/blob/main/netplay/server/impairment.go).

| Item | Value |
| --- | --- |
| Rooms | 1024 maximum |
| Tracked per-IP limiters | 4096 maximum; arbitrary map entry eviction at capacity |
| Rate refill / burst | 500 packets/s / 250 tokens per source IP |
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

A disconnect is different. There is no state transfer, hot rejoin or migration to a new relay. A new process has a new nonce and cannot join a running or finished room. Both players must cold-start a new match.

The relay can rebind an occupied slot to a new UDP address after a same-nonce, same-identity join. The standard connected C++ transport does not send new join requests after `ready()`. This relay feature is not a promise of automatic NAT-rebinding recovery in the frontend.

A correction outside retained history is an error. It is not replaced by a fabricated neutral input or an approximate state.

## Performance is not window size

[docs/developer/VALIDATION.md](https://github.com/ansxor/f3-recomp/blob/main/docs/developer/VALIDATION.md) records these native timings on Apple M5, Darwin arm64, Release:

| Operation | Mean | p95 | Maximum |
| --- | ---: | ---: | ---: |
| Save | 113.0 µs | 183.5 µs | 290.9 µs |
| Load | 99.8 µs | 176.8 µs | 217.3 µs |
| Normal step | 3620.7 µs | 3989.4 µs | 9848.4 µs |

The measured mean native step throughput is 276.2 FPS. With snapshot copies and a 16.667 ms budget, the recorded affordable correction depth is 3 at mean cost, 2 at p95 cost and 0 at worst observed cost. Oracle sound records 1, 1 and 0.

A 16-frame history permits a 16-frame correction. It does not promise that the correction fits one display interval. Periodic CRC work, network work, SDL and OS scheduling add cost. Catch-up can require several display intervals. Confirmed-only audio adds latency.

## Trust and validation boundary

Snapshots never travel over the network. A peer sends inputs, checksums and finish claims only. `load_state` accepts internally generated same-build snapshots. It checks size, not arbitrary content, schema, pointers or a cryptographic signature. Do not expose it as an untrusted save-file parser.

The connected UDP socket accepts datagrams only from the resolved relay endpoint. The relay checks session ID, sender slot and registered source endpoint before forwarding gameplay traffic. This prevents ordinary stale-session and wrong-endpoint interference.

It does not provide authentication. Room names, nonces, session IDs and identities travel in clear text. There is no encryption or integrity MAC. A compromised relay or client is outside the trust model. A forged identity or collision is not ruled out by CRC equality.

The packet parsers also have distinct checks. The relay validates `GameData` payload lengths, flags, counts, frame-range overflow, input bits and trailing bytes. It does not compare header flags with payload flags or enforce every client semantic bound. Heartbeats are endpoint-checked but forwarded without a payload parse. Read [Wire protocol](/developer/netplay/protocol#parser-validation-and-asymmetries) before writing a compatible implementation.

## Features not provided

- More than two players, spectators or room discovery.
- Account authentication, encryption or anti-cheat.
- Authoritative server emulation or video streaming.
- Portable snapshots, cross-build saves or cross-platform determinism guarantees.
- State transfer, disconnect resume or late joining.
- Dynamic input delay, automatic window selection or congestion control.
- Durable results or replay storage on the relay.

Use a trusted relay and a reachable UDP port. Use a new room after a completed match, or wait for the old finished room to expire. A terminated room can reset on a new join. Never use a room code as a password.
