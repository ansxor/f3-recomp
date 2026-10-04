# Land Maker sound driver

Target: supplied Japan 2.01J main program and interleaved sound chips `e61-14.32`
(high byte) / `e61-15.33` (low byte). The supplied chips contain 0x20000 bytes
apiece; padding each with FF to 0x40000 reproduces the validated ROM loader.
Interleaved, padded sound-program CRC32: `5a7e9117`; reset vector: `$c1089e`.
Addresses below are sound-68000 addresses unless marked main CPU.

## Evidence and invariants

ROM behavior and observed output outrank hardware notes and MAME source. The
notes in `~/Workspace/taito-f3/{audiocpu,otis-bank,duart,sound-address}.txt` are
WIP board observations, not a complete driver specification. `audiocpu.txt`
is a pin list, not evidence for an 8 MHz clock. This runtime retains its
15,238,090 Hz interpreted sound CPU, 4 MHz DUART and 16 MHz main timebase.
Neither instruction-atomic bus accesses nor those timings are claimed to be
physical-bus measurements.

ES5505/ES5510 stay emulated. The interpreted driver remains the reference oracle;
SDL3 remains the playback backend. Observation does not affect device scheduling,
change a register write, or perform additional reads of side-effectful devices.

## Capturing and decoding the oracle

```sh
build/f3rt-gameplay-regression --seed 5 --frames 6000 \
  --sound-trace build/seed5.sound --wav build/seed5.wav
python3 tools/decode_sound.py build/seed5.sound \
  --output build/seed5-writes.jsonl.gz
python3 tools/decode_sound.py build/seed5.sound --notes-only \
  --output build/seed5-notes.jsonl
```

The frontend accepts the same `--sound-trace FILE` option, including independent
main-CPU interpretation with `f3rt-run`. Seeded gameplay uses the established
coin/start/64-bit LCG schedule; the sound CPU is interpreted in both cases.
All captures and extracted game data remain ignored, not committed.

The binary header is eight bytes `F3SND2\0\0`. Records are 32 bytes,
little-endian Python `struct` format `<QQIIIBB2x`:

| Field | Meaning |
|---|---|
| tick, u64 | Absolute effective 16 MHz device cursor; main writes use their synchronized main instruction tick |
| sample, u64 | Number of PCM frames already generated at the bus operation |
| pc, u32 | Sound instruction PPC or main writer PC |
| address, u32 | Original bus address |
| value, u32 | Actual read result or write operand |
| kind, u8 | 1 main write, 2 sound read, 3 sound write, 4 board reset, 5 end; 6 completed voice context (address=RAM pointer, value=physical voice), 7 RAM snapshot, 8 allocated note, 9 direct-note operand, 10 note-event pointer |
| width, u8 | Bus bytes 1/2/4; zero for metadata/reset/end |

Sound MMIO reads/writes and main mailbox/reset writes are retained, including
repeated writes. Long sound transfers decode to two ordered word transfers at
the same instruction timestamp. Missing end markers, truncated records or
backward time are errors. Warm board reset clears DSP/volume but preserves OTIS;
it does not reset the trace clock. Initial cold state is implicit.

JSONL preserves effective byte masks and raw data, resolves OTIS pages and
voice numbers, and emits the accumulated register state for each low-page
voice write. `voice_start` means an observed stopped-to-running CR transition,
including silent startup probes; it is not automatically an audible note.
`--notes-only` is an onset projection, **not** a substitute for the full
pitch/filter/envelope stream. A voice continues to change between note starts.

### Command ownership, not nearest-event matching

Main `$2fde` copies length-prefixed packets into the 1024-byte ring at
`$c00000`, wrapping with `& $3ff`. Main `$3270` copies lists of these packets.
The big-endian producer word at main `$c00480` is **twice** the byte index.
Sound sees even byte lanes: producer `$140900`, consumer `$140904`, ring
`$140000..$1407fe`. `$c11116` reads packet length; `$c11128` reads opcode.
`$c11146..$c11150` advances/wraps the consumer at `$800` and publishes it
with MOVEP. Length includes the length and opcode bytes.

Consumption posts a task message carrying the ring offset. Actual dispatch
is later: `$c12ecc` for high commands, `$c0b414` for effects commands.
The decoder follows both queues by ring offset and occurrence; it does not
attribute an onset to the most recently submitted packet.

`$8e` operands are `[sequence, logical_track, key, velocity]`. Its handler
at `$c130ea` writes the key at `$c130f0` and branches to the note factory
`$c140d6`. At `$c140e4`, A5 is the allocated note node, A1 the owning
channel, A6 the track. The voice initializer copies that node to voice
`+$0e`; it copies the channel pointer to `+$10`. Music notes use the same
factory but are owned by the sequence-start command, not the last control
command. Channel `+$0e` points to `$5e5c + sequence * $28`.

The optional observer snapshots **work RAM only** at note allocation and
voice programming/key-on. JSON `driver.origin` includes the stable note ID,
originating command ID, sequence, logical track, key and velocity. Reusing a
RAM node or stealing a hardware voice does not retroactively change old
events. Raw voice/channel/sample-descriptor snapshots preserve evidence for
fields whose semantics are not yet established. Command IDs identify note
origin; subsequent volume/program/control commands remain separate timeline
events, not falsely relabeled as new note initiators.
Legato owner changes at `$c1762c` are observed at the following `$c17632`
write, so voice ownership can change without a stopped-to-running CR edge.
F3SND2 emits the completed context after its RAM words; the exploratory
F3SND1 files must be recaptured rather than decoded with stale RAM ordering.
Probes match the instruction's bus address and width as well as PPC. An IRQ
stack push can retain the interrupted instruction's PPC; counting that push
as a voice or note update would invent metadata without a corresponding write.

Seed 5/frame 6000: **852 published, consumed and dispatched commands**;
1,639 note allocations; all **2,202 non-startup voice starts** have an
originating command. 277 are direct `$8e` notes, 1,925 are sequenced voice
starts. The other two starts are silent initialization probes. The augmented
capture still produces the same WAV bytes as the untraced run.


### Voice fields

- Sample addresses are **16-bit ROM word addresses**, not byte offsets or
  instrument IDs. `sample_word` derives from the last observed ACC value;
  it is not an extrapolated live playback position at every later write.
- START/END/ACC exposed by OTIS have nine fractional bits. START/END discard
  low five bits. The board bank supplies `(bank & 7) << 20` words in this
  16 MiB sample region. Effective FC discards bit zero; sample increment is
  `FC / 1024` words per output frame.
- CR bits 0–1 stop, bit 3 loop, bit 4 bidirectional loop, bit 6 direction;
  bits 8–9 choose one of four stereo output pairs. Pair 0 bypasses DSP;
  pairs 1–3 enter the six serial DSP inputs. Loop enable, direction and stop
  are separate fields, not a guessed musical articulation.
- LVOL/RVOL expose the upper eight bits. ES5505 does not generate the driver's
  software envelope; its repeated volume/filter writes remain timestamped.
  K1/K2 discard low four bits.
- DSP host writes include register-select commits and, when all latch bytes
  are known, the 24-bit GPR or 48-bit instruction value. A read-select of live
  DSP state invalidates the offline latch until actual bus reads/writes
  establish it; unknown values are null, not fabricated.
- DUART and MB87078 host writes remain explicit events. No backend-neutral
  note abstraction is used to hide them.

### Measured observation gate

Initial seed 5 through frame 6000: 80,338,232 strict-native main blocks, zero
fallback, 3,029,425 stereo PCM frames. Traced and untraced WAVs are byte-identical.
Raw trace: 9,643 main writes, 10,489,728 sound writes, 1,420,314 sound reads and
one warm board reset. The register decoder finds 2,204 stopped-to-running
transitions, including two silent startup voice-0 probes.

Fresh 3600-frame attract runs with native and interpreted main CPUs have
byte-identical complete WAVs, SHA-256
`62bac3a4d1baef4e7c2343ce987e44eb654a71f90d653b5b88c1d12d15eff05b`.
Against the retained reset-aware native-rate MAME capture, seconds 20–54 retain
L/R correlation **0.9956761849580716 / 0.9952171577492129**, RMS error
18.722660457703864 / 19.499802079856668 LSB and fixed lag -1 sample. This is the
existing compatibility baseline, not waveform equality to physical hardware.
`f3rt-check` passes. Evidence: local ignored `build/seed5*`, `build/oracle-*`.

## Mailbox grammar and main-game selectors

High-command handler words are at `$c1323e`; accepted lengths at `$c13260`.
The dispatcher clears opcode bit 7, bounds it to `$11` entries and requires
the exact length. Invalid packets can be consumed without a driver action.
The table below describes ROM operations; it does not infer an audible note
from every command. Multi-byte operands are big-endian.

| Opcode | Length | Payload | Handler / effect |
|---|---:|---|---|
| 20 | 3 | effect | `$c0b454`: effect <13, selects configuration; queues effects worker |
| 21 | 4 | parameter, value | `$c0b47e`: byte at `$c84a + 2*parameter`, recompute effects |
| 80 | 3 | sequence | `$c12f20`: stop/restart, non-looping start `$c12ce0` |
| 81 | 3 | sequence | `$c12f26`: stop/restart, looping start `$c12ce6` |
| 82 | 3 | sequence | `$c12f1a` → `$c12c30`: stop/remove active sequence |
| 83 | 3 | sequence | `$c12f2c`: zero sequence rate and release track notes |
| 84 | 3 | sequence | `$c12f42`: restore saved sequence rate |
| 85 | 5 | sequence, position-hi, position-lo | `$c1301a`: initialize/seek by advancing event cursors |
| 86 | 4 | sequence, volume | `$c12f62`: sequence `+$20`, clear fade step `+$22` |
| 87 | 4 | sequence, value | `$c12f96`: all active tracks' channel `+$26/+$2d` |
| 88 | 4 | sequence, rate | `$c12f7a`: current/saved rate `+$00/+$1c`, override `+$25` |
| 89 | 7 | sequence, start, end, rate-index, unused | `$c12fcc`: volume fade using signed multiplier table `$c089dc` |
| 8a | 5 | sequence, track, value | `$c130c6`: channel `+$20/+$2c` |
| 8b | 5 | sequence, track, value | `$c130ba`: channel `+$26/+$2d` |
| 8c | 6 | sequence, track, index, value | `$c130d2`: doubles index, dispatches controller `$c14330` |
| 8d | 6 | sequence, track, program-hi, program-lo | `$c130da` → `$c11dce`: select track instrument/program |
| 8e | 6 | sequence, track, key, velocity | `$c130ea`: allocate sustained note, duration `$7fff` |
| 8f | 5 | sequence, track, key | `$c1313c`: unlink matching note and release through `$c141ce` |
| 90 | 6 | sequence, track, key, value | `$c13146`: matching-note continuation stores value at A3+6; not exercised in seed 5 |

`$c13072` finds a live track by its logical ID; an absent track produces no
note. It reads four argument bytes even for five-byte packets; handlers that
do not need the final operand ignore it. The fade handler reads four operands
despite accepting length seven; do not invent a meaning for its final byte.
The musical names of the channel fields for 87/8a/8b and the intended use of
90 are not established by this capture. Their addresses and stores are
documented instead of guessed MIDI-controller names.

Sequence bit 7 is **not** a universal ignored flag: `$c12c30/$c12cb8/$c12cea`
implement extra selection/state behavior. `$c12de6` also follows linked
sequence aliases through header byte `+$a6` via `$c158cc`. The decoded
seeded capture uses ordinary IDs; origin inference outside captured aliases
must not silently substitute the latest command. A missing origin stays null.

Main program `$6bde` is the sound-selector pointer table; maximum selector
`$5f` is stored at `$6d5e`. Main `$30be` dispatches descriptor type:

- Type 0: `[type, song, volume]`; `$30ee` emits 81 then 86.
- Type 2: `[type, unit, velocity, track, program-hi, program-lo, key, duration]`;
  `$31cc` emits optional old-note 8f, then 8d and 8e. Duration is scheduled
  by the main game, not an ES5505 sample-end value.
- Type 3: counted raw command bytes, emitted through `$3270`.

These selectors are **not** mailbox opcodes or physical voice numbers.
Music selector descriptors occupy IDs `$4e..$5d`; the event extraction tool
accepts actual packets so it does not embed this copyrighted table.

## Driver tables and state

Only table locations/formats are tracked here; ROM-derived table contents and
generated C stay in ignored build directories.

| Location | Structure / evidence |
|---|---|
| RAM `$5e5c` | 100 sequence slots, stride `$28`; address calculation `$c12a46` |
| RAM `$6dfc` | Null-terminated active-sequence pointer list; insertion `$c12dca` |
| RAM `$d404` | Sequence-stream bank base; `$c158ae` selects long offset at base+8+4*ID |
| RAM `$d408` | Sequence-header bank base; `$c158cc` selects word offset at base+8+2*ID |
| RAM `$5daa` | Word channel-pointer table; channel lookup `$c11c1a` |
| RAM `$d94a` | Physical voice records, stride `$aa`, initialized `$c17a00..$c17a46` |
| ROM `$c1c1e0` | Word offsets relative to `$c1ae76`, selected `$c17ce6` |
| ROM `$c1ae76` | Sample split records, stride 14 bytes; key cutoff byte +5 |
| ROM `$c0a734` | Sample-loop mode → OTIS control-byte lookup |
| ROM `$c089dc` | Signed scale multipliers used in fades/pitch/envelopes |
| ROM `$c08bec` | Additional voice scaling table |
| ROM `$c1c2d2` | 13 effects configurations, stride 16; selected `$c0f482` |
| ROM `$c1c3a2` | DSP-program pointer table; configuration byte +5 selects entry at `$c19982` |

Voice record fields established by initialization and live snapshots:
`+0/+2` next/previous list links; `+4..+7` effective key/channel/layer tags;
`+$0c` flags (high byte) and OTIS voice (low byte); `+$0e` owning note node;
`+$10` channel pointer; `+$16` patch pointer; `+$1a` selected sample-split pointer
(ROM lookup or the RAM override at `$d840`);
`+$24/+$26` rotating update-list links; `+$8c` channel's embedded descriptor
pointer; `+$a2` sample-bank selector. JSON distinguishes `patch_descriptor`,
`sample_descriptor` and `channel_descriptor`; these are not interchangeable.
The `driver.key/tag5/tag6/tag7` fields preserve raw allocation tags; use
`driver.origin.note` for the command/sequence key. Descriptor pointers are
not necessarily ROM addresses: the captured direct SFX uses a RAM split.

The sample-split record has tuning at +0, packed address/control longwords at
+2/+6/+10, cutoff key at +5, loop-mode byte at +9. `$c17cfa` selects the first
split whose cutoff is at least the effective key. The upper address bits feed
OTIS fixed-point registers, not an unqualified 24-bit word index. `$c1807e`
shifts the bank selector right once for the board bank register and inserts
its old low bit at bit 28 of the OTIS addresses. `$c180b2` writes the bank at
`$300001 + 2*voice`. Use decoded final `start_word/end_word/sample_word`, not
the raw descriptor's flag-bearing low byte, to index sample ROM.

## Allocation, envelopes and sequencing

Allocation is **not simply “steal oldest voice.”** `$c1748e` first searches
matching effective-key/channel/layer voices and the free list at `$d844`.
Exhaustion enters `$c17670/$c17678`: scan released/active lists `$d850/$d848`
for priority class zero, then class `$40` when permitted, then other eligible
heads. Class comes from patch +$60 bits 6–7 and is saved in voice +$97.
The flag checks at `$c176f8` can reject an allocation with carry set.
Retrigger/legato paths `$c1753a..$c17664` may reuse an existing physical voice
and replace its note-node owner without a fresh hardware key-on.

`$c17b94` initializes a selected voice. `$c17e62` selects its OTIS page,
zeroes volume, stops it, initializes filters/accumulator and programs the
sample. Pending starts are linked under `$d84c`. `$c177f6` walks that list;
`$c17814` clears CR stop bits 0–1 unless voice-state high-byte bit 4 suppresses
the trigger. `$c1781c` updates rotating-list links, then splices the pending
list into the active list. `$c1789a` unlinks a freed voice, returns it to
`$d844`, and silences/reinitializes hardware through `$c17a88`.

The `$d85a` ring also schedules recurring per-voice software work
(`$c18360/$c18a2e`), not just allocation order. Pitch/filter/volume writes
continue after key-on; `FC` commits include `$c18296`. Envelopes and release
are driver software state, **not** a replacement ES5505 envelope generator.
Full JSONL retains every write, including repeated values. An onset-only
music export loses these curves and cannot reproduce audio by itself.

DUART initialization `$c10e22` programs counter `$07d0` in the observed
32-voice mode, ACR `$60`, IMR `$2b`. The IRQ handler `$c10da0` reads
`$28001f`, updates task delays, calls sequencer tick `$c12e3e`, then mailbox
poll `$c110c2`. The capture's modal IRQ spacing is **16,000 main ticks**
(1 ms), with instruction-boundary jitter. The alternate ROM initialization
uses `$09c4`; it is not silently substituted for the observed mode.

Per active sequence, `$c12e3e` adds rate +0 to accumulator +2, subtracts
`$271` (625) at threshold, advances the subdivision and posts a worker
message. Thus rate 120 at the observed 1 kHz IRQ cadence gives 192 sequence
ticks/second (96 ticks/quarter at 120 BPM). This calculation is from ROM
arithmetic and measured cadence, not a guessed audio-sample-driven timer.
Pausing zeros rate; resuming restores +$1c. Task/kernel traps and queued
dispatch mean submission, consumption, note allocation and OTIS start have
different timestamps.

Sequence decoding `$c14730/$c1475a` scans big-endian words for bit 15 set.
The low byte selects event class; high seven bits usually encode delta time.
Classes below `$58` decode note/packed velocity/duration at `$c148b2`;
`$58..$af` and `$b0..$d8` use separate callback classes with adjusted
key/index. `$d9/$da/$db` are further control classes; `$dc` follows a
relative note reference; `$e6..$e9` dispatch extended control records.
Callbacks come from the caller's word table, so the same stream can be
played, scanned or positioned without treating every token as a note.
Note duration is a 10-bit field, with an extra word when zero. The note
factory adds `$15` to the decoded key for the kernel note representation;
mailbox 8f performs the same adjustment before matching.

## DSP and master volume

20/21 update effects state at `$c84a`, then `$c0f430` posts to worker queue
`$fb28`. `$c0f4bc` selects the configuration and `$c19982/$c19998` loads its
DSP program. The loader consumes typed blocks and host-register commits,
not a flat array of interchangeable sound notes. DSP instructions are
48-bit; host GPR values are 24-bit. Both remain emulated by ES5510.

DUART OP6 controls DSP HALT. `$28001d/$28001f` are set/reset-output writes
(reads of those addresses have different DUART meanings). MB87078
`$340000` is the control/channel latch and `$340002` the gain-data latch;
they are **not** independent left/right volume registers. The existing
board mixer and quantized gains are unchanged. Trace reset markers retain
the distinction between resetting DSP/DUART/volume and preserving OTIS.

## Native driver and strict comparison

`--sound-driver oracle|native` is independent of the main-CPU execution mode.
`landmakr` and the gameplay harness default to `native` when sound code is generated;
select `--sound-driver oracle` explicitly for interpreted reference captures.
`native` is a **literal statically recompiled ROM driver**,
not a high-level musical rewrite: its own `f3_cpu` state executes generated C
for the 68000 program, with native memory/control callbacks. It preserves the
ROM's tasks, mailbox parser, tables, allocation, sequencer and DSP worker.
It neither calls Musashi for sound execution nor replays captured events.
ES5505, ES5510, DUART, banks, gain hardware and SDL3 remain the existing devices.

`tools/compile_sound.py` validates sound CRC32 `5a7e9117` and generates under
`build/generated/sound-landmakrj`. CMake does this with `F3_ROM_DIR`; pre-generated
builds can set `F3_SOUND_GENERATED_DIR`. The compiler reuses arithmetic lowering,
but provides 68000 instruction costs, data-dependent multiplication/division
timing, six-byte exception frames, reset debt and immediate IRQ recognition.
Capstone rejects a nonzero ignored upper byte on CCR/bit immediates; only that
decoder input byte is normalized, preserving the CPU's effective operand.
The generated table covers every even ROM address outside explicit sound
`[[exclude]]` intervals, not just PCs from a trace. Dispatch indexes the compact
immutable complement by subtracting preceding excluded words, with no second
map. Excluded targets fail before fetching or interpreting an instruction.

This is oracle compatibility at **instruction boundaries**, not cycle-accurate
physical bus emulation. In particular, the existing `$c17814` intra-instruction
read/modify/write versus sample-edge residual is preserved. This is not a
general-purpose 68000 emulator: executing work-RAM code or an unlowered ROM
candidate fails with its PC/opcode, with no interpreter fallback. The current
manifest contains 944 unlowered aligned candidates, including overlapping/data
decodes. None is reached by the exercised runs. All packet variants, sequence
aliases and error paths have **not** been exhaustively exercised.

```sh
build/f3rt-gameplay-regression --seed 5 --frames 6000 \
  --sound-trace build/seed5-oracle.sound --wav build/seed5-oracle.wav
build/f3rt-gameplay-regression --seed 5 --frames 6000 --sound-driver native \
  --sound-trace build/seed5-native.sound --wav build/seed5-native.wav
python3 tools/compare_sound.py build/seed5-oracle.sound build/seed5-native.sound \
  --json build/seed5-parity.json
cmp build/seed5-oracle.wav build/seed5-native.wav
```

`compare_sound.py` compares every field in order, including reads, repeated
writes, timestamps, PCM ordinals, PCs and RAM ownership metadata. No lag fitting,
sorting, tolerance or write deduplication. A mismatch reports the first record
and recent submitted packets; truncated/incomplete captures fail.

Final seed-5 gate: **12,258,121 identical records**, 3,768 voice contexts,
1,639 note allocations, 852 commands and byte-identical 3,029,425-frame WAVs.
The native main CPU executes 80,338,232 blocks with zero fallback in both modes.
Fresh 3600-frame attract WAVs are also identical for native-main/oracle-sound,
interpreted-main/oracle-sound and native-main/native-sound. MAME correlation
retains exactly the observation-gate values above; it does not improve or
regress the separate hardware-model residual.

## Extracting music and SFX events

`f3rt-sound-extract` runs the interpreted main game for 900 frames by default,
then freezes it and advances only the sound subsystem. The game publishes its
startup packets itself. Main `$3400/$3404/$340a/$3410` writes the four gain
requests at main `$c007f8..$c007fb` around 13.2338 seconds; the old 660-frame
freeze point preceded these writes and produced highly attenuated output.
Default 900-frame event origin is tick 244,300,326 / PCM frame 454,413
(15.268770 seconds) with the supplied ROM and cold default state.

Pending main packets are drained before the event origin. `--packet` injects
at time zero; repeated `--at SECONDS:HEX` schedules relative to that origin,
rounded once to the 16 MHz clock. Equal-time packets retain argument order.
Injection uses the actual ring and producer index, with writer PC `ffffffff`.
Full-ring, malformed packets, unreleased reset and events at/after `--seconds`
are errors, not silently delayed/dropped commands. No hidden initialization
packets or master-volume overrides are inserted.

Music sequence 8 with sequence volume `$74`:

```sh
build/f3rt-sound-extract --rom-dir /path/to/roms/landmakr \
  --sound-driver native --packet 038108 --packet 04860874 --seconds 5 \
  --wav-window event --sound-trace build/music.sound --wav build/music.wav
python3 tools/decode_sound.py build/music.sound --notes-only \
  --output build/music.jsonl
```

An observed direct SFX program/key/velocity, followed by note release:

```sh
build/f3rt-sound-extract --rom-dir /path/to/roms/landmakr \
  --sound-driver native --packet 068d01074002 --packet 068e01072768 \
  --at 1.5:058f010727 --seconds 3 --wav-window event \
  --sound-trace build/sfx.sound --wav build/sfx.wav
python3 tools/decode_sound.py build/sfx.sound --notes-only \
  --output build/sfx.jsonl
```

Use `--sound-driver oracle` to reproduce each independent reference and compare
with `compare_sound.py` plus `cmp`. Exercised music: 1,697,152 identical records,
148,805 event-window PCM frames, peak 414; SFX: 1,446,519 identical records,
89,283 event-window frames, peak 530. Both WAVs are byte-identical across drivers.
The complete bus trace always starts at cold boot. `--wav-window event` trims
only WAV output; default `full` retains boot. CLI audio counters include boot
regardless of the chosen WAV window. JSONL uses absolute trace time and retains
command provenance; `--notes-only` omits subsequent envelope/control writes.
For complete reproduction, decode without that projection. This is packet-level
extraction, not a promise that every program/sequence ID yields an audible note.
