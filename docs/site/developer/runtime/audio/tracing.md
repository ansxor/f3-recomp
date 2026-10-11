# Sound traces and command lifecycle

A sound trace records bus operations and driver context. It does not replace the sound CPU or the audio devices.

The runtime writes binary records during execution. `tools/decode_sound.py` decodes them after execution. This separation avoids extra reads of device registers.

## Source map

| Source | Responsibility |
|---|---|
| [sound_trace.hpp](https://github.com/ansxor/f3-recomp/blob/main/runtime/audio/sound_trace.hpp) | Record kinds and writer API. |
| [sound_trace.cpp](https://github.com/ansxor/f3-recomp/blob/main/runtime/audio/sound_trace.cpp) | Binary encoding and end marker. |
| [interpreter.cpp](https://github.com/ansxor/f3-recomp/blob/main/runtime/interpreter.cpp) | Oracle bus observation. |
| [sound_native.cpp](https://github.com/ansxor/f3-recomp/blob/main/runtime/audio/reference/native/sound_native.cpp) | Equivalent native bus observation. |
| [machine.cpp](https://github.com/ansxor/f3-recomp/blob/main/runtime/machine.cpp) | Main mailbox writes, reset-line writes and board-reset markers. |
| [decode/trace.py](https://github.com/ansxor/f3-recomp/blob/main/tools/decode/trace.py) | Format validation, register reconstruction and command ring tracking. |
| [compare_sound.py](https://github.com/ansxor/f3-recomp/blob/main/tools/compare_sound.py) | Strict record comparison. |

## Capture a trace

Select the oracle explicitly when you need an interpreted reference. Sound-driver selection is independent of main-CPU selection.

```sh
build/f3rt-tool gameplay --seed 5 --frames 6000 \
  --sound-driver oracle --sound-trace build/seed5-oracle.sound \
  --wav build/seed5-oracle.wav
uv run f3 decode trace build/seed5-oracle.sound \
  --output build/seed5-writes.jsonl.gz
```

The [extraction tool](/developer/runtime/audio/extraction) and `gameplay` command accept `--sound-trace FILE`.

These commands are examples. The historical results below come from the repository evidence document, not a new capture for this page.

## Binary format

The header contains eight bytes: `F3SND2\0\0`. Each record contains 32 bytes. Numeric fields use little-endian order.

The Python format is `struct.Struct("<QQIIIBB2x")`.

| Byte offset | Field | Size | Meaning |
|---:|---|---:|---|
| 0 | `tick` | 8 | Absolute time in 16 MHz main-clock ticks. |
| 8 | `sample` | 8 | Number of PCM frames already generated. |
| 16 | `pc` | 4 | Sound instruction PPC, native instruction PC, or main writer PC. |
| 20 | `address` | 4 | Bus address or context pointer. |
| 24 | `value` | 4 | Read result, write operand or context data. |
| 28 | `kind` | 1 | Record kind from the table below. |
| 29 | `width` | 1 | Transfer size in bytes, or a metadata-specific size. |
| 30 | Padding | 2 | The writer leaves these bytes zero. |

The padding bytes are not returned by Python unpacking. The comparator compares the seven decoded fields, not the padding or raw file bytes.

### Record kinds

| Kind | Name | Address and value |
|---:|---|---|
| 1 | `MainWrite` | Main mailbox or CPU-reset-line byte write. Width is 1. |
| 2 | `SoundRead` | Original sound bus address and actual returned value. Width is 1, 2 or 4. |
| 3 | `SoundWrite` | Original sound bus address and write operand. Width is 1, 2 or 4. |
| 4 | `Reset` | Warm board reset. Current callers use address 0, value 2 and width 0. |
| 5 | `End` | Completed capture. PC, address, value and width are zero. |

### Two timestamp fields

For kind 1, `SoundTrace::record` uses `Machine::cpu.cycles`. The main bus synchronizes devices before a mailbox or reset-line access.

For all other kinds, it uses `Audio::clock_ticks()`. This is the effective device cursor, not the endpoint of a main block.

The `sample` field always uses `Audio::generated_frames()`. It identifies the PCM boundary already reached at the operation.

Several operations can have the same tick and sample. Their record order remains significant.

[Device scheduling](/developer/runtime/audio/timing) advances the DUART and PCM before a sound instruction due at that tick. Bus transfers remain instruction-atomic.

The trace does not claim physical bus-cycle timestamps. It preserves the runtime's instruction-boundary model.

## Observation order and scope

A sound read reaches `Audio` first. The observer then records the returned value.

A sound write reaches the observer first. The runtime then performs the write.

The observer retains accesses within `SOUND_TRACE_BUS_BEGIN..SOUND_TRACE_BUS_END` (`0x140000..0x340003`), including gaps in that range. It excludes ordinary ROM and RAM accesses.

A raw long transfer remains one width-4 record. Offline decoding expands it into two ordered word transfers at the same timestamp.

Repeated writes remain separate records. An unchanged value can still describe driver timing.

Older `F3SND1` captures use different ordering. The decoder rejects them. Recapture them instead of changing their header.

## Offline register reconstruction

`records()` rejects these conditions:

- An incorrect header.
- A partial 32-byte record.
- A backward tick.
- A missing end marker.
- Data after the end marker.

`Decoder.decode()` rejects unknown record kinds. `words()` rejects invalid bus widths.

### OTIS state

The decoder tracks PAGE, ACT, banks and low-page voice registers. It applies the implemented masks for frequency, addresses, filters and volume.

Each low-page voice write includes the reconstructed voice state. Bank writes also include a voice snapshot.

A `voice_start` is an observed CR transition from stopped to running. It is not proof of audible output.

The decoder emits separate fields for loop, bidirectional loop, direction, stop state and output pair.

Sample addresses use ROM words, not byte offsets. `sample_step` is effective `FC / 1024` words per PCM frame.

`sample_word` uses the last observed ACC value. It is not a simulated live playback position between writes.

### DSP and other devices

The decoder tracks the nine GPR and instruction latch bytes. A select write can include a 24-bit `gpr_value` or 48-bit `instruction`.

A DSP read-select can load values that change during execution. The decoder therefore invalidates its known latch bytes at register `0x80`.

Actual subsequent reads or writes restore known bytes. Unknown combined values remain `null`.

DUART, MB87078 and mailbox transfers remain explicit events. A board-reset event clears decoder DSP latch state and preserves OTIS state.

## Command lifecycle

```mermaid
flowchart TD
    A["Main packet commit at 0xc00481"] --> B["pending queue: ring offset and occurrence"]
    B --> C["Length read at 0xc11116"]
    C --> D["ready queue: same ring offset"]
    D --> E["Dispatch at 0xc12ecc or 0xc0b414"]
```

`CommandRing` reconstructs the [1024-byte packet ring](/developer/runtime/audio/mailbox). Each publication receives an increasing `command_id`.

The producer commit contains twice the byte index. The decoder checks its alignment, range and complete packet lengths.

Consumption removes the oldest pending occurrence at the packet's ring offset. Dispatch removes the oldest ready occurrence at that offset.

These checks reject unpublished consumption, unconsumed dispatch and length disagreement. Ring reuse does not merge command occurrences.
## Output projections

```sh
uv run f3 decode trace build/seed5-oracle.sound \
  --output build/seed5-all.jsonl
uv run f3 decode trace build/seed5-oracle.sound --notes-only \
  --output build/seed5-onsets.jsonl
uv run f3 decode trace build/seed5-oracle.sound --commands-only \
  --output build/seed5-commands.jsonl
```

| Mode | Included events |
|---|---|
| Default | All decoded events. |
| `--notes-only` | `command`, `command_consumed`, `command_dispatch`, `voice_start`, `board_reset`, `end`. |
| `--commands-only` | `command`, `command_consumed`. |

If both flags are supplied, `--commands-only` takes precedence.

Voice starts reflect CR register state transitions from stopped to running. An onset projection omits later envelope, pitch, and filter changes.

An output name ending in `.gz` selects gzip. The decoder prints counts for all decoded event types, including events excluded from the output.

## Strict oracle/native comparison

```sh
uv run f3 compare trace build/seed5-oracle.sound \
  build/seed5-native.sound --json build/seed5-parity.json
cmp build/seed5-oracle.wav build/seed5-native.wav
```

The comparator compares all seven fields in order. It does not sort, deduplicate, fit lag or permit timing tolerance.

A mismatch reports the first differing record and up to eight recent submitted commands. Exit status is 0 for equality and 1 for failure.

WAV equality is a separate check. Matching PCM alone does not prove matching driver operations.

The comparator uses the validating record reader. Equal complete captures therefore require valid headers, record boundaries and end markers.

It returns immediately at a first mismatch. It need not validate unread tails after that mismatch.

[The evidence document](https://github.com/ansxor/f3-recomp/blob/main/docs/SOUND-DRIVER.md) reports a seed-5 gate with 12,258,121 identical records.

That gate includes 852 commands. Its WAVs contain 3,029,425 identical PCM frames.

Those are recorded compatibility results. They do not prove all ROM paths or physical hardware equality.
