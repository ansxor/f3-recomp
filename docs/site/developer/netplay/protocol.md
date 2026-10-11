# Netplay protocol

This page is the wire reference for the two-player netplay relay (protocol version 3). The relay lives in `netplay/server/`. It pairs two clients in a room, manages connect token minting, forwards per-frame inputs, and compares simulation checksums. It never transfers or forces machine state. Only session parameters and per-frame inputs cross the wire.

The relay is built on [Yojimbo](https://github.com/mas-bandwidth/yojimbo), using [Netcode](https://github.com/mas-bandwidth/netcode) authenticated and encrypted packets over UDP, with client tokens minted over a lightweight HTTP endpoint (`cpp-httplib`).

---

## Ports & Transport

| Transport | Default Port | Option | Purpose |
| :--- | :--- | :--- | :--- |
| **TCP** | `:7845` | `--port` | HTTP token endpoint: client token minting (`GET /token?v=3`) |
| **UDP** | `:7845` | `--port` | Game socket: encrypted Yojimbo traffic (control, chat, sync blob, inputs) |

A single port number (default `7845`) is used for both TCP and UDP. On POSIX and modern operating systems, TCP and UDP sockets bind to the same numeric port concurrently without conflict because they are separate transport protocols. Firewalls and port-forwarding rules need only forward port 7845 for both TCP and UDP.

---

## Connect Token Exchange

Before connecting via UDP, a client acquires an ephemeral 2048-byte Netcode connect token from the relay over HTTP.

### Endpoint
```http
GET /token?v=3
```

- **Query Parameters**:
  - `v`: Protocol version (`3`). Must match `f3rt::netplay::PROTOCOL_VERSION`.
- **Response Headers**:
  - `Content-Type: application/octet-stream`
- **Response Body**:
  - Exactly **2056 bytes**:
    - `0..7`: 64-bit unsigned integer client ID in little-endian byte order (`uint64_t client_id`). Always generated randomly by the server to prevent client spoofing.
    - `8..2055`: 2048-byte raw Netcode connect token encrypted with the server's ephemeral private key.
- **HTTP Status Codes**:
  - `200 OK`: Token minted successfully.
  - `409 Conflict`: Missing or mismatched protocol version `v`. Body is plain text `version mismatch\n`.
  - `429 Too Many Requests`: Per-source-IP rate limit exceeded (token bucket: burst of 10, refilling at 1 token/sec). Body is plain text `rate limited\n`.
  - `500 Internal Server Error`: Server failed to generate token.

### Connection Flow
1. Client requests `GET /token?v=3` from the relay's TCP port.
2. Relay mints token using `netcode_generate_connect_token` with:
   - Server's random 32-byte private key (generated at relay startup).
   - Advertised public address (`--public-address`, default `127.0.0.1:<port>`).
   - 30-second token expiration, 10-second connection timeout.
   - Protocol ID `0xF303000000000001ULL` (`f3rt::netplay::PROTOCOL_ID`).
3. Client decodes `client_id` and token buffer, and calls `yojimbo::Client::Connect(client_id, token)`.
4. Netcode performs a cryptographic challenge/response handshake over UDP to authenticate and bind the client slot. All subsequent UDP datagrams are authenticated and encrypted using ChaCha20-Poly1305 AEAD.

---

## Channels

Every Yojimbo connection operates two virtual channels:

| Channel | ID | Type | Content |
| :--- | :--- | :--- | :--- |
| `CHANNEL_CONTROL` | `0` | Reliable Ordered | Lobby, room setup, match start, sync blob, CRC, results, chat, errors |
| `CHANNEL_INPUTS` | `1` | Unreliable Unordered | High-frequency gameplay inputs (60 Hz lockstep) |

- **Channel 0**: Guarantees in-order, loss-free delivery with automatic retransmissions and fragmentation. Block messages support attached binary payloads up to 2048 bytes (`MAX_SYNC_BLOB_SIZE`).
- **Channel 1**: Datagrams are unreliable and unordered. Packet loss is tolerated via redundant input framing: each message carries up to 32 recent frames of inputs until acknowledged by peer simulation progress.

---

## Message Definitions

All messages are declared in `netplay/shared/contract.hpp` and registered in `NetplayMessageFactory`. Serialization uses Yojimbo's unified bit-level stream serializer.

### Channel 0: Control Messages

| ID | Name | Direction | Fields | Bounds & Notes |
| :--- | :--- | :--- | :--- | :--- |
| `0` | `MSG_HELLO` | Client $\to$ Server | `game_id: char[33]`<br>`rom_crcs: uint32[7]`<br>`build_hash: uint8[32]`<br>`player_name: char[25]` | `game_id` length 1..32<br>7 ROM checksums<br>32-byte SHA-256 build hash<br>`player_name` length 1..24 UTF-8 |
| `1` | `MSG_CREATE_ROOM` | Client $\to$ Server | `input_delay: uint8`<br>`flags: uint8` | `input_delay` in range 1..8<br>`flags` bit 0 = sync blob required |
| `2` | `MSG_JOIN_ROOM` | Client $\to$ Server | `room_code: char[7]` | 6 uppercase chars from alphabet |
| `3` | `MSG_WELCOME` | Server $\to$ Client | *(empty)* | Acknowledges valid `Hello` |
| `4` | `MSG_ROOM_CREATED` | Server $\to$ Client | `room_code: char[7]` | 6-character room code |
| `5` | `MSG_MATCH_START` | Server $\to$ Client | `session_id: uint64`<br>`seed: uint64`<br>`player_index: uint8`<br>`input_delay: uint8`<br>`peer_name: char[25]` | `session_id` cryptographic random<br>`seed` cryptographic random<br>`player_index` 0 for host, 1 for joiner<br>`input_delay` host's delay (1..8)<br>`peer_name` peer player's name |
| `6` | `MSG_SYNC_BLOB` | Host $\to$ Server | Attached block | 1 to 2048 bytes binary state blob. Host only, before `Go`, flag bit 0 rooms only. |
| `7` | `MSG_SYNC_BLOB_FWD` | Server $\to$ Joiner | Attached block | Forwarded block data byte-for-byte to joiner. |
| `8` | `MSG_GO` | Server $\to$ Both | *(empty)* | Authorization to begin emulation. |
| `9` | `MSG_CRC` | Client $\to$ Server | `frame: uint32`<br>`crc: uint32` | State fingerprint for specified frame |
| `10` | `MSG_DESYNC` | Server $\to$ Both | `frame: uint32`<br>`crc_p1: uint32`<br>`crc_p2: uint32` | Sent once on first checksum mismatch |
| `11` | `MSG_RESULT` | Client $\to$ Server | `frame: uint32`<br>`winner: uint8`<br>`final_crc: uint32` | `winner` 0=P1, 1=P2, 0xFF=Draw |
| `12` | `MSG_RESULT_ACK` | Server $\to$ Both | `agreed: bool`<br>`winner: uint8` | `winner` is agreed winner or `0xFE` if disagreed |
| `13` | `MSG_CHAT` | Client $\to$ Server | `text: char[257]` | 1 to 256 bytes UTF-8, no bytes < 0x20 or == 0x7F |
| `14` | `MSG_CHAT_FWD` | Server $\to$ Peer | `from_player: uint8`<br>`text: char[257]` | `from_player` 0 for host, 1 for joiner |
| `15` | `MSG_PEER_LEFT` | Server $\to$ Peer | *(empty)* | Notifies peer that session ended |
| `16` | `MSG_ERROR` | Server $\to$ Client | `code: uint16`<br>`message: char[257]` | Fatal protocol error (see Error Codes) |
| `17` | `MSG_BYE` | Client $\to$ Server | *(empty)* | Clean session termination request |

### Channel 1: Gameplay Inputs

| ID | Name | Direction | Fields | Bounds & Notes |
| :--- | :--- | :--- | :--- | :--- |
| `18` | `MSG_INPUTS` | Client $\to$ Server<br>Server $\to$ Peer | `first_frame: uint32`<br>`count: uint8`<br>`inputs: uint16[32]` | `count` in range 1..32<br>Consecutive inputs starting at `first_frame` |

---

## Error Codes

```cpp
enum class NetplayErrorCode : uint16_t {
    None             = 0,
    BadMessage       = 2, // Malformed, out-of-range, or invalid for state
    RoomNotFound     = 3, // Room code does not exist or expired
    RoomFull         = 4, // Room already has 2 players
    IdentityMismatch = 5, // Game ID, ROM CRCs, or build hash differs from host
    NotInLobby       = 6, // Create/Join while in room, or CHAT before MATCH_START
    ServerFull       = 7  // Server at room capacity (<= 32 rooms)
};
```

Any `ErrorMessage` is fatal. The server queues the message on `CHANNEL_CONTROL`, flushes packets, and disconnects the client.

---

## Session Flow & State Machine

1. **Token Acquisition**: Each client fetches a connect token via `GET /token?v=3` over TCP port 7845.
2. **UDP Handshake**: Client calls `client.Connect(client_id, token)` to establish Netcode/Yojimbo encryption.
3. **Hello**: Client sends `HelloMessage` on Channel 0. Server validates name, game ID, and limits, and replies with `WelcomeMessage`.
4. **Room Creation / Joining**:
   - Host sends `CreateRoomMessage` with `input_delay` (1..8) and `flags`. Server returns `RoomCreatedMessage` with a 6-character room code (`ABCDEFGHJKLMNPQRSTUVWXYZ23456789`).
   - Joiner sends `JoinRoomMessage` with that code.
   - Server compares joiner's `game_id`, `rom_crcs` (7 values), and `build_hash` (32 bytes) against the host. If any differ, joiner gets `Error(IdentityMismatch)`.
5. **Match Start**: Both clients receive `MatchStartMessage` on Channel 0 with assigned `player_index` (0 host, 1 joiner), match seed, host's input delay, and peer name.
6. **Sync Blob Gating**:
   - If room was created without flag bit 0 (`flags == 0`): `GoMessage` is sent to both players immediately after `MatchStart`.
   - If room was created with flag bit 0 (`flags & 1`): `GoMessage` is held. Host sends `SyncBlobMessage` (1..2048 B as block data). Server relays it to joiner as `SyncBlobFwdMessage`. Only after the blob is queued to the joiner does the server send `GoMessage` to both clients.
7. **Gameplay**: Clients exchange `InputsMessage` on Channel 1 and send periodic `CrcMessage` on Channel 0.
8. **Results & Teardown**: Each client sends `ResultMessage`. Server sends `ResultAckMessage` once both have reported. A client sends `ByeMessage` (or disconnects), the other player receives `PeerLeftMessage`, and the server writes `<logdir>/<session_id>.json`.

### Rules & Edge Cases
- `HelloMessage` must be the first message. Anything else gets `Error(NotInLobby)` or `Error(BadMessage)` and a disconnect.
- Before `Hello`, a connection may stay idle for at most 5 seconds (`HELLO_TIMEOUT_SECONDS`).
- In the lobby (unpaired), connection idle timeout is 10 minutes, refreshed by traffic.
- An unjoined room expires after 10 minutes (`ROOM_TTL_SECONDS`). The host is disconnected cleanly without error.
- Matches have no idle timeout during gameplay.
- A single relay process supports at most 32 concurrent rooms (`yojimbo::MaxClients = 64`).
- Conflicting re-sends of inputs (same frame with different value) trigger a protocol error log and the packet is dropped without partial mutation.
- CRC pending table holds up to 256 frames; lowest frame is evicted when full. First mismatch sends `DesyncMessage` once; later mismatches are logged only.
- In-session chat allows a token bucket burst of 5 messages at 1 message/sec. Messages exceeding the limit are dropped silently without echo or error.

---

## Session Logs

When a session terminates, the relay writes `<logdir>/<session_id>.json` (16 lowercase hex digits) via an atomic `.tmp` file write and rename, structured using `cpp-yyjson`:

```json
{
  "session_id": "0123456789abcdef",
  "started_at": "2026-10-11T14:30:00.123456789Z",
  "ended_at": "2026-10-11T14:35:12.987654321Z",
  "game_id": "landmakrj",
  "build_hash": "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
  "rom_crcs": [12345, 67890, 11111, 22222, 33333, 44444, 55555],
  "seed": "fedcba9876543210",
  "input_delay": 2,
  "player_names": ["alice", "bob"],
  "inputs": [
    [0, 0, 1, 1, null, 2],
    [0, 1, 1, null, null, 3]
  ],
  "crc": {
    "compared": 180,
    "mismatch_count": 0,
    "mismatches": [],
    "unmatched": 0,
    "evicted": 0
  },
  "results": [
    { "frame": 3600, "winner": 0, "final_crc": 305419896 },
    { "frame": 3600, "winner": 0, "final_crc": 305419896 }
  ],
  "result_ack": {
    "agreed": true,
    "winner": 0
  },
  "sync_blob": "deadbeef01020304",
  "protocol_errors": [],
  "protocol_errors_dropped": 0,
  "end_reason": "bye",
  "ended_by": 0,
  "chat": [
    { "player": 0, "t_ms": 1250, "text": "gg" }
  ],
  "chat_dropped": [0, 0]
}
```

---

## Running the Relay Server

Build and run from the repository root:

```sh
make netplay-server
./build/netplay-server --port 7845 --logdir ./netplay-logs
```

### Options

| Option | Default | Meaning |
| :--- | :--- | :--- |
| `--port <port>` | `7845` | Port for both HTTP token endpoint (TCP) and game socket (UDP) |
| `--public-address <addr>` | `127.0.0.1:<port>` | Advertised public host:port embedded in client connect tokens |
| `--logdir <dir>` | `./netplay-logs` | Directory for per-session JSON logs |
| `--max-rooms <n>` | `32` | Maximum number of concurrent rooms (1 to 32) |
