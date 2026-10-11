# Netplay

**What you will learn:** how to run a relay server, set up two computers for two-player versus over the network, and what the netplay messages mean.

Netplay runs Land Maker Japan two-player versus over the network. Each player runs the game on their own computer. A relay server passes each player's controls to the other player, one frame at a time.

## What netplay does

- **Game:** only Land Maker Japan (`landmakrj`) has a netplay trigger in this tree. Other games do not offer netplay.
- **Trigger:** the netplay lobby opens when the game enters 2-player versus mode.
- **Start state:** when the match starts, the host sends a small part of its game state (the credit count and coin chute subsystem, operator settings including free play, coinage, continue, and difficulty, the match settings, and the whole gameplay block: mode, stage and round state, both players' boards and piece generators) to the joiner. The joiner applies it before the first frame (the joiner's credits and operator settings — free play, coinage, continue, difficulty — are replaced by the host's for the match). This is not a full copy of the game.
- **Lockstep:** both game copies run the same frames with the same controls. Each frame, the game's random number generator is reseeded from the match seed, so both copies draw the same values.
- **Checks:** the two sides compare a fingerprint of the gameplay state every 60 frames. If the fingerprints differ, the match ends with a DESYNC message.
- **Identity:** both players must use the same game, the same ROM revision, and the same build. The server refuses a player whose build or ROM differs from the host's.

## What you need

- A copy of the game built on each computer, with your own Land Maker Japan ROM files. See [Getting started](/guide/getting-started).
- A relay server that both computers can reach. Run one yourself, or use one that someone else gives you.
- Network access on the server port, for both TCP and UDP.

## Run a relay server

Build and start the server from the repository root:

```sh
make netplay-server
./build/netplay-server
```

The server accepts these options:

| Option | Default | Meaning |
| --- | --- | --- |
| `-tcp` | `:7845` | TCP address for the control connection. |
| `-udp` | `:7845` | UDP address for the input packets. |
| `-logdir` | `./netplay-logs` | Directory for one JSON log per session. |
| `-max-rooms` | `64` | Maximum number of rooms. |

The server has no accounts or passwords. Anyone who can reach its port can create a room or join one with the room code.

## Settings

Press `F1` and open the **Netplay** tab.

| Setting | Meaning |
| --- | --- |
| **Server (host:port)** | Relay address. The default is `127.0.0.1:7845`. |
| **Player Name** | Name shown to the other player. 1 to 24 bytes. |
| **Input Delay** | 1 to 8 frames. The host's value is used for the match. A higher value hides more network delay but makes the controls respond later. The default is 2. |

Choose **Save preferences** to keep these values. They are stored under the keys `netplay_server`, `netplay_name` and `netplay_delay`.

The command-line options `--netplay-server`, `--netplay-name` and `--netplay-delay` replace the saved values for one run.

## Start a match

1. Start the game and choose 2-player versus mode on the Game Select screen. The **2P Versus - Netplay Lobby** window opens and shows **2-Player Versus Mode Committed!** The background animation, swirl, sparkles, and music continue playing seamlessly while waiting in the lobby.
2. **Player 1** presses **Host room**. The lobby shows a **Room Code**. Send this code to the other player.
3. **Player 2** types the code in the box next to **Join room** (letters are shown in capitals) and presses **Join room**.
4. The status changes in this order: **Connecting to server...**, **Waiting for peer...**, **Peer matched! Registering UDP...**, **Running**.

While the lobby is open, **Cancel Netplay** (or pressing `Escape` or closing the lobby window) cancels the connection and returns directly to the Game Select menu with your cursor usable and the timer reset to 9, allowing you to pick another mode or re-confirm 2P. Choosing **Play locally** proceeds immediately into local 2-player versus mode.

Room codes use the letters A to Z (except I and O) and the digits 2 to 9. They are six characters long.

During a match, the overlay in the top-right corner shows your player (P1 or P2), the room, the peer's name, ping in milliseconds, the stall count, and the input delay in frames.


## In-session chat

During a netplay session (while waiting in the lobby or during an active match), you can chat with your peer:

- **Opening chat:** Press `Enter` to reveal the chat box in the bottom-left corner with keyboard focus. While in a session, `Enter` is consumed by the chat interface even if bound to a game control. Outside of a netplay session, `Enter` performs no chat action.
- **Sending:** Type your message (up to 256 bytes) and press `Enter`. The message is sent to your peer and echoed locally in the chat scrollback.
- **Canceling:** Press `Enter` with an empty text box or press `Escape` to close the chat box without sending.
- **Controls while typing:** While the chat input is open, keyboard and gamepad inputs to the game are zeroed so that typing does not accidentally trigger game moves, and keys do not trigger solo menu actions (`F1`, `F12`). Emulation continues running uninterrupted in lockstep without stalling.
- **Fading:** When the chat input is closed or when a new message arrives from your peer, the chat frame displays recent messages against a transparent background in the bottom-left corner for 5 seconds, then smoothly fades out over 1.5 seconds.
- **Characters:** Non-ASCII Unicode characters render with the default font as `?`.
## Limits during a match

- **Save and load states are disabled.** The Save and Load buttons are greyed out and the menu says so.
- **Input scripts with memory poke rules are rejected** when netplay is on. The run stops with an error.
- **Fast boot** applies on the next launch only.
- The menu has no fast-forward control.

## Messages

| Message | Meaning |
| --- | --- |
| `Ended: Match completed` | The match finished normally. |
| `Ended: Peer left match` | The other player left or disconnected. |
| `Ended: DESYNC at frame N` | The two game copies disagree at frame N. The match stops. |
| `Ended: Disconnected from server` | The control connection to the server was lost. |
| `Server error <code>: <message>` | The server rejected a request. For example, code 5 means the game, ROM or build differs. |

## If you get a DESYNC

**Send the folder from both players.** When a match ends with a DESYNC, each game writes a diagnostic folder on its own computer and shows its path in the lobby, with **Copy path** and **Open folder** buttons. The folder is `netplay-desync/<UTC time>-<session>-p<player>/` inside the settings folder (on macOS `~/Library/Application Support/f3-recomp/f3rt/`). Nothing is uploaded; the game never sends these files over the network.

| File | Contents |
| --- | --- |
| `info.json` | Mismatch frame, local and remote fingerprints, seed, input delay, which hash phase was active, player index and names, build hash, room code, stall and clock-resync counters, and how to regenerate the state. |
| `hashed.bin` | The exact bytes the fingerprint covered at the mismatch frame, followed by the previous check's bytes. |
| `inputs.txt` | Every input this player applied in the match (both players), plus the frame numbers of the lobby trigger, the sync point and match start, and how long the lobby was held. |
| `trigger.f3s` | The local game state at match frame 0. |

During a match the bundle costs one copy of about 520 bytes (the hashed bytes) every 60 frames, one in-memory copy of the match-start state, and the recorded inputs. No full state is saved during play; nothing is written until a DESYNC.

To compare two bundles offline, run the same build with the same ROMs:

```sh
f3rt-tool netplay-replay --bundle <player 1 folder> --bundle <player 2 folder>
```

It loads each `trigger.f3s`, replays each `inputs.txt`, and prints the first frame and the memory addresses where the fingerprinted bytes differ. With one `--bundle` it checks that the replay reproduces the recorded `hashed.bin`. To get the full game state at any match frame, for example the mismatch frame:

```sh
f3rt-tool netplay-replay --bundle <folder> --dump-state <frame> out.f3s
```

The bundle records which machine configuration it came from (GPU video, audio backend), and `netplay-replay` builds the same one. Folders from older versions do not have this; the tool finds it from the state size, or you can give `--gpu 1 --audio enhanced`. The headless client takes the same bundle with `f3rt-tool netplay --desync-dir DIR`.

A match does not reconnect. After an end message, start again from the lobby with a new room.

## Known limits

- The relay is the only path between players. Both computers must reach the server.
- Only the synced part of the state is copied (the whole gameplay block, credits, operator settings and the rounds setting). Other state, such as sound and animation timing, may differ between the two copies. The check covers only the synced gameplay data, so those differences do not cause a DESYNC. The headless harness observed these differences in the non-gameplay state.
- Sync was checked with a headless two-process harness, not in a live two-computer match. `[INFERENCE]` A real network match may behave differently.
- For the design and open work, see [Netplay for developers](/developer/netplay/).
