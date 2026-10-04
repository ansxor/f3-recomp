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

ES5505/ES5510 stay emulated. The default interpreted driver remains the oracle;
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

The binary header is eight bytes `F3SND1\0\0`. Records are 32 bytes,
little-endian Python `struct` format `<QQIIIBB2x`:

| Field | Meaning |
|---|---|
| tick, u64 | Absolute effective 16 MHz device cursor; main writes use their synchronized main instruction tick |
| sample, u64 | Number of PCM frames already generated at the bus operation |
| pc, u32 | Sound instruction PPC or main writer PC |
| address, u32 | Original bus address |
| value, u32 | Actual read result or write operand |
| kind, u8 | 1 main write, 2 sound read, 3 sound write, 4 board reset, 5 end |
| width, u8 | Bus bytes 1/2/4; zero for reset/end |

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
