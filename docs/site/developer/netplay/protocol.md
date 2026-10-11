# Netplay protocol

This page is the wire reference for the two-player netplay relay (protocol version 1). The relay lives in `netplay/server/`. It pairs two clients in a room, hands out a shared session key and seed, forwards per-frame inputs, and compares checksums. It never transfers or forces machine state. Only session parameters and per-frame inputs cross the wire.

## Conventions

- All integers are big-endian.
- `u8`, `u16`, `u32`, `u64` are unsigned integers of that width.
- `str` is a `u8` byte length followed by that many UTF-8 bytes.
- `NB` means N raw bytes.

## Ports

| Transport | Default | Flag | Purpose |
| --- | --- | --- | --- |
| TCP | `:7845` | `-tcp` | Control connection: lobby, match start, checksums, results |
| UDP | `:7845` | `-udp` | Per-frame input packets |

## TCP framing

Every TCP message is a frame:

| Field | Type | Meaning |
| --- | --- | --- |
| length | `u32` | Number of bytes in `type` plus `payload` |
| type | `u8` | Message type |
| payload | bytes | Message body, as listed below |

A frame length of 0 or more than 4096 is invalid. The receiver closes the connection without sending ERROR.

## Client to server

| Type | Name | Payload |
| --- | --- | --- |
| 0x01 | HELLO | `u16` proto_version (= 1), `str` game_id, `7×u32` rom_crc, `32B` build_hash, `str` player_name (1 to 24 bytes) |
| 0x02 | CREATE_ROOM | `u8` input_delay (1 to 8), `u8` flags (bit 0 = sync blob required; other bits must be 0) |
| 0x03 | JOIN_ROOM | `6B` room_code |
| 0x04 | CRC | `u32` frame, `u32` crc |
| 0x05 | RESULT | `u32` frame, `u8` winner (0 = P1, 1 = P2, 0xFF = draw), `u32` final_crc |
| 0x06 | BYE | empty |
| 0x07 | SYNC_BLOB | `u16` length (1 to 2048), then `length` bytes. Host only, at most once, before GO. Only in rooms created with flag bit 0. |
| 0x08 | CHAT | `u16` length (1 to 256), then `length` bytes of UTF-8 text. No byte may be below 0x20 or equal to 0x7F. Allowed only from MATCH_START until the session ends. Rate limited, see the rules. |

Room codes use the alphabet `ABCDEFGHJKLMNPQRSTUVWXYZ23456789`. Codes are six characters long.

## Server to client

| Type | Name | Payload |
| --- | --- | --- |
| 0x81 | WELCOME | `u16` proto_version |
| 0x82 | ROOM_CREATED | `6B` room_code |
| 0x83 | MATCH_START | `u64` session_id, `32B` session_key, `u64` seed, `u8` player_index, `u8` input_delay, `u16` udp_port, `str` peer_name |
| 0x84 | GO | empty |
| 0x85 | DESYNC | `u32` frame, `u32` crc_p1, `u32` crc_p2 |
| 0x86 | RESULT_ACK | `u8` agreed, `u8` winner |
| 0x87 | PEER_LEFT | empty |
| 0x88 | ERROR | `u16` code, `str` message |
| 0x89 | SYNC_BLOB | Same payload as the host's SYNC_BLOB, forwarded unchanged to the joiner only. |
| 0x8A | CHAT | `u8` from_player (0 = host, 1 = joiner), `u16` length, then `length` bytes of text. Sent to the other player only. |

Field notes:

- MATCH_START: `session_id`, `session_key`, and `seed` come from a cryptographically secure random source. `player_index` is 0 for the host and 1 for the joiner. `input_delay` is the host's value from CREATE_ROOM. `udp_port` is the UDP port the server is listening on. `peer_name` is the other player's `player_name`.
- GO is sent once, to both clients, after both have registered their UDP endpoints and, if the room requires a sync blob, after the blob has been relayed to the joiner.
- RESULT_ACK `winner` is the agreed winner when `agreed` is 1, and 0xFE otherwise.

## Error codes

| Code | Meaning |
| --- | --- |
| 1 | Bad protocol version |
| 2 | Bad message (malformed, unknown, or invalid for the current state) |
| 3 | Room not found |
| 4 | Room full |
| 5 | Identity mismatch (game_id, rom_crc, or build_hash differs from the host) |
| 6 | Not in a lobby state for this message. Also sent for CHAT before MATCH_START. |
| 7 | Server full |

## Session flow

1. Each client opens a TCP connection and sends HELLO first. The server answers with WELCOME.
2. The host sends CREATE_ROOM and receives ROOM_CREATED with a room code.
3. The joiner sends JOIN_ROOM with that code. Both clients receive MATCH_START.
4. If the room requires a sync blob, the host sends SYNC_BLOB after MATCH_START. The server relays it to the joiner. The joiner applies the blob before it sends REGISTER. The server does not check this.
5. Each client binds a UDP socket and sends REGISTER packets (see below). When both endpoints are registered, and the blob has been relayed if one was required, the server sends GO to both.
6. Clients exchange INPUTS packets over UDP. Each client sends CRC values over TCP.
7. At the end of the match, each client sends RESULT. The server replies with RESULT_ACK to both.
8. A client sends BYE, or the TCP connection closes.

Rules:

- HELLO must be the first message on a connection. Anything else gets ERROR 2 and a close.
- A room has one host and one joiner.
- The joiner's identity must equal the host's. Otherwise the joiner gets ERROR 5.
- A room that nobody has joined expires after 10 minutes.
- If either TCP side closes or sends BYE, the other side gets PEER_LEFT and the session ends.
- There is no keepalive during a match.
- Only connections in the lobby state that are idle for more than 10 minutes are closed.

## Checksums

Each client sends CRC for each frame it has simulated. The server collects the values per frame from both clients. On the first frame where the two values differ, the server sends DESYNC to both clients, once.

## UDP packets

Every UDP packet has this layout:

| Field | Type | Meaning |
| --- | --- | --- |
| magic | `4B` | ASCII `N3P1` |
| session_id | `u64` | Session from MATCH_START |
| player_index | `u8` | Sender's player index |
| kind | `u8` | 0 = REGISTER, 1 = INPUTS |
| body | bytes | Kind-specific body |
| tag | `16B` | First 16 bytes of HMAC-SHA256(session_key, all preceding bytes) |

The maximum packet size is 512 bytes.

### kind 0: REGISTER

Empty body. The server records the sender's address for (session, player). The server replies with a REGISTER packet to the sender, tagged the same way. When both players have registered and any required sync blob has been relayed, the server sends GO over TCP to both. A later REGISTER from the same player updates the address and gets a reply, but does not send GO again.

### kind 1: INPUTS

| Field | Type | Meaning |
| --- | --- | --- |
| first_frame | `u32` | Frame number of the first input |
| count | `u8` | Number of inputs, 1 to 32 |
| inputs | `count × u16` | One input per frame, consecutive from first_frame |

The server verifies the tag, appends inputs it has not seen to that player's log (frame to input), and ignores duplicates. A re-sent frame with a different input is a protocol error. The server logs it and drops the packet. A valid packet is forwarded to the other player's registered address with its bytes unchanged.

## Drop rules (UDP)

The server silently drops a packet when:

- its length is outside 32 to 512 bytes,
- its magic is wrong,
- `player_index` is greater than 1,
- `kind` is unknown,
- the session is unknown,
- the tag does not verify.

Each source address is rate limited with a token bucket of 300 packets per second and a burst of 300. A source that exceeds this is dropped.

## Server behaviour where the spec is silent

These choices were made where protocol v1 does not say. They are part of the server's behaviour and clients may rely on them.

- Frame length counts `type` plus `payload`. Lengths from 1 to 4096 are accepted.
- Any ERROR is fatal. The server sends it, then closes the connection.
- A malformed message gets ERROR 2. So does a message that is invalid for the current state. Examples are CRC or RESULT outside a match, and BYE with a payload. CREATE_ROOM or JOIN_ROOM while already in a room gets ERROR 6.
- A duplicate HELLO gets ERROR 2.
- CREATE_ROOM must be exactly 2 bytes. The old 1-byte form gets ERROR 2. Setting any flag bit other than bit 0 also gets ERROR 2.
- SYNC_BLOB is accepted only from the host, only in a room created with flag bit 0, only once, and only before GO. Any other SYNC_BLOB gets ERROR 2, and the usual close rules apply. Its `u16` length must be 1 to 2048, and the payload must have no trailing bytes.
- The server relays the SYNC_BLOB payload to the joiner unchanged. It does not inspect the blob contents.
- Strings must be valid UTF-8. Trailing bytes after the last field are malformed.
- CHAT is accepted only from MATCH_START until the session ends. That covers the wait before GO. A CHAT before MATCH_START gets ERROR 6, the existing "not in a lobby state" code. Every ERROR is fatal, so a lobby host that sends CHAT is disconnected and its room closes with it.
- A malformed CHAT gets ERROR 2 and ends the session. Malformed means a length of 0 or above 256, a length that does not match the bytes sent, invalid UTF-8, or a byte below 0x20 or equal to 0x7F. The other player gets PEER_LEFT.
- A valid CHAT is relayed to the other player only, as a 0x8A message with `from_player`. The sender gets no echo.
- CHAT is rate limited per connection by a token bucket. The bucket holds 5 tokens and refills at 1 token per second, so a burst of 5 is allowed and then about 5 messages per 5 seconds. A CHAT with no token available is dropped silently. Neither player gets an error or a relay, and the drop is counted in `chat_dropped`.
- Before HELLO, a connection may stay idle for 30 seconds. In the lobby, idle time is 10 minutes and is refreshed by every frame. Matches have no idle timeout.
- When a room expires, the server removes it and closes the host's connection without sending ERROR.
- On session end, the server sends PEER_LEFT to the other client and then closes both connections.
- `-max-rooms` caps the number of rooms, including rooms with an active session. A CREATE_ROOM over the cap gets ERROR 7.
- Joining a room that already has a joiner gets ERROR 4.
- A room code that does not match the alphabet gets ERROR 2. A well-formed code that matches no room gets ERROR 3.
- The identity check compares `game_id`, `rom_crc`, and `build_hash`. It does not compare `player_name`.
- Only the first RESULT from each player counts. RESULT_ACK is sent once, after both players have a result. The result is agreed when frame, winner, and final_crc are all equal. Otherwise `winner` is 0xFE. The session stays open until BYE or disconnect.
- For CRC, the first report from each player for a given frame wins. The pending table holds 256 frames. When it is full, the lowest frame is evicted. Frames at or above 1,048,576 are ignored.
- INPUTS from a player who has not registered are dropped silently. Out-of-range inputs, a bad count, or a conflicting re-send are logged as protocol errors. The whole packet is dropped. Duplicates with no conflict are accepted and forwarded. Forwarding requires the other player to be registered. Frames with no input are recorded as null in the log.
- REGISTER with a non-empty body is logged as a protocol error.
- The UDP rate limiter is keyed by source address, with IPv4-mapped IPv6 addresses unmapped. Idle buckets are pruned after one second. At most 4096 buckets exist. When that is full, new sources are denied.
- The advertised `udp_port` is the port the UDP socket is actually bound to. This matters when `-udp` uses port 0.
- `-tcp` and `-udp` are listen addresses.

## Session logs

When a session ends, the server writes `<logdir>/<session_id hex>.json`, where `session_id hex` is 16 hex digits. It writes to a `.tmp` file and then renames it. The log contains:

| Field | Content |
| --- | --- |
| `session_id` | Session ID (hex) |
| `started_at`, `ended_at` | RFC 3339 timestamps with nanoseconds |
| `game_id`, `build_hash`, `rom_crcs` | Identity from the host's HELLO |
| `seed` | Seed (hex, 16 digits) |
| `input_delay` | Host's input delay |
| `player_names` | Names from both HELLOs |
| `inputs` | Two arrays, one per player, dense by frame, with null for missing frames |
| `crc` | `compared`, `mismatch_count`, `mismatches` (at most 256), `unmatched`, `evicted` |
| `results` | Result from each player, or null |
| `result_ack` | `agreed` and `winner`, or null |
| `sync_blob` | Relayed blob (hex), or null if none was relayed |
| `protocol_errors` | Logged errors (at most 256), plus `protocol_errors_dropped` |
| `chat` | Accepted CHAT lines in arrival order. Each entry has `player` (0 host, 1 joiner), `t_ms` (milliseconds since MATCH_START), and `text`. Dropped lines are not included. The log does not cap this array. The rate limit bounds it to a burst of 5 plus 1 line per second per player. |
| `chat_dropped` | Rate-limited CHAT lines per player, as `[host, joiner]`. |
| `end_reason` | `bye`, `disconnect`, or `protocol_error` |
| `ended_by` | Player index that ended the session, or null |

The server does not replay logs. They are for inspection only.

## Running the server

From the repository root:

```sh
make netplay-server
./build/netplay-server -logdir ./netplay-logs
```

`make netplay-server` builds `build/netplay-server`. `go run .` in `netplay/server/` also works.

| Flag | Default | Meaning |
| --- | --- | --- |
| `-tcp` | `:7845` | TCP listen address for the control connection |
| `-udp` | `:7845` | UDP listen address for input packets |
| `-logdir` | `./netplay-logs` | Directory for per-session JSON logs |
| `-max-rooms` | 64 | Maximum number of rooms, including active sessions |

Security note: the session key is sent in cleartext over TCP. Anyone who can read or alter that TCP traffic can learn the key and forge UDP packets. Run the relay on a network where that is acceptable, or add transport security in front of it.
