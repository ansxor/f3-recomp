# HLE audio

Implementation evidence from `hle-audio` commit `5e8c775`, measured on macOS
arm64 with Land Maker Japan 2.01J. Historical captures remain in that worktree;
they are not bundled downloads or a whole-F3 compatibility claim.

## ROM-derived protocol and data

The accurate native/interpreted sound path remains the default and oracle.
The HLE path must not execute the sound CPU, OTIS device or ESP instruction
program. Its inputs are the loaded sound and sample ROMs, not extracted traces.

The mailbox is **1024 bytes**, not 8 KiB: main `$c00000..c003ff`, inside
2048-byte shared RAM. `$c00480/481` publishes twice the byte index;
`$c00482/483` acknowledges it. Sound sees only even lanes at `$140000`.
Main `$2fde` appends inclusive-length packets and masks each byte index with
`$3ff`. See [SOUND-DRIVER.md](../SOUND-DRIVER.md) for the complete command grammar.
`8f` really releases the first matching sequence/track/key, through `$c141ce`.
It has no instance ID: rollback cancellation must target host instance IDs,
not send a synthetic equal-key `8f` that could release a replacement sound.

Command `90` correction: `$c1310e` pops the return PC into A3. Matching-note
lookup returns by JMP(A3); `$c13148` writes `6(A3)` at ROM `$c1314e`, ignored
by the bus. It does not update note pressure. Main board-gain requests appear
at shared `$7f8..7fb`; observed requests are `$30`.

`tools/hle_sequence.py` decodes all 27 sequences, 206 tracks and 24,470 events
in Land Maker. Stream/header banks are `$c20000/$c3879e`. Headers are `$a8`
bytes; eight 14-byte channel records begin at +$20, eight auxiliary bytes at
+$90, effect controls at +$98, linked-sequence selector at +$a6. Stream blocks
start with a long inclusive size and eleven long track offsets. Track lengths
are not reliable termination bounds for arrangements; directory bounds and E9
termination are authoritative. Ordinary event tokens are big-endian words;
bit 15 marks an event, high seven bits of the operand delay **after** that
event. Classes 00..57 are notes; duration is low ten bits, with a following
word for zero. Velocity expands the seven-bit packed field; factory key
translation is +21. B0..D8 are controllers, D9 selects program/bank, E6 is a
long delay, E7 is an arrangement entry and E9 ends a track. Selection lists
7/14/18/24 require the sequence-ID high bit: ordinary `81 07` is silent;
`81 87` selects child4, then child5 at the section boundary. Mailbox starts
first stop/reset the selection; internal transitions advance it. Repeat100
marks the loop-selection point. The callback skips that marker and returns
to its following phase at E9. Notes belong to the child sequence.
The sequencer timer is 1 kHz, with rate accumulation crossing 625. Rate 120
therefore produces 192 ticks/s (96 PPQ). Startup events execute immediately;
header bar boundaries, not the last track's E9, drive looping. Fades use an
independent 12 ms signed 8.8 update.

Physical channels allocate highest-free first, 56 down to1. Previous-key
history survives explicit sequence destruction and is reused for portamento.
Natural non-looping completion parks the sequence but retains its channels,
so later direct `8d/8e` SFX still work; explicit `82` frees them.

`tools/hle_instruments.py` decodes all 100 programs $4000..4063: 79 layered,
21 direct/key-region programs. Programs are packed alternating bank columns,
stride $198. A zero enable mask at +$196 selects seventeen 20-byte key-region
records; other masks select up to three layers. Sample headers in the PCM
ROM's high bytes supply the sixteen bank directories copied into sound RAM.
The observed split at RAM $711a is **copied ROM metadata**, not evidence of a
runtime override at $d840. Split records are 14 bytes: signed Q8 tuning, ACC,
START and END, with cutoff and mode embedded in low bytes. Addresses are Q9
sample-word positions with bank bits, not byte offsets. Pitch uses ROM lookup
and octave shifts; FC advances at 29761/(1024*48000) source words per host
sample. Envelopes use wrapping fixed-point segments and ROM rate tables, not
a substituted ADSR. Normal layers advance their envelope once before key-on;
direct records do not. Steady-state physical-voice service is once per 12 ms.

A live oracle snapshot after 900 interpreted-main boot frames establishes the
direct $4002/key39/velocity104 example: kernel key60, sample bank2,
start/end words 2157724/2200571, ACC31012991, FC796, K1=K2=64496,
initial envelope26416, left/right encoded volume $cba0. These are measured
values, not bundled runtime assets.

## Effects inventory and measured approximation target

Land Maker has thirteen effect configurations at `$c1c2d2`, selecting eleven
ESP programs through `$c1c3a2`. A complete seed5/frame6000 oracle capture
contains one `20 0b` selection and parameter writes 0..9 with values
30,58,73,32,37,0,40,43,18,32. Preset11 selects program6 at `$c1d4d4`;
its loaded instruction CRC is `cdcfa577`. Boot also loads preset0. This is
an observed campaign inventory, not a claim every possible game mode was heard.

Gunlock, Recalhorn, Twin Cobra II and Puchi Carat ROMs were available. Their
sound images differ, but the entire code bank `$10000..1ffff`, including DSP
configuration/program bytes, is byte-identical to Land Maker. This establishes
shared firmware/effects, **not** interchangeable song/instrument banks or full
other-game support.

An isolated ES5510 oracle replayed 15,609 actual host writes from the boot
capture, warmed up with zero input, then measured each serial input with
1024- and 4096-level impulses. Inputs0/1 pass dry; inputs2/3 give damped stereo
cross-delay and reverb; inputs4/5 give a dry plus diffused reverb route.
At 4096, input2 has first peak2663 at native frame1, then echoes at
8178L2094,16355R946,24532L427,32709R191. The cross-delay interval is
8177/29761 seconds (~274.8 ms), feedback ~0.45. Input4 has dry3779 at frame0,
early reflections449L520/687R520 and signed diffusion taps. Reverb remains
above quantization noise through two seconds. The cheap equivalent uses
fractional-rate-independent delay lines, damping and stereo diffusion rather
than interpreting the ESP program. This implementation does not reproduce
ESP output bit-for-bit; that observation does not establish that byte parity
is impossible for HLE in general.

ROM menu callbacks identify parameter2 as delay-address increments
`value*$7000` (24-bit address, eight fractional bits), parameter6's high nibble
as damping and low nibble as route balance, and parameters8/9 as complementary
ROM-table wet/dry gains. Parameter0 controls decay/diffusion and parameter1 a
signed feedback coefficient. These control semantics must remain responsive;
a fixed pre-rendered impulse is not the runtime implementation.

## Runtime and rollback contract

Select `--audio-backend hle` before execution. `--sound-driver` and CPU-bus
traces belong to `--audio-backend accurate` and cannot be combined with HLE.
`landmakr`, `f3rt-run`, `f3rt-gameplay-regression`, `f3rt-sound-extract` and
the netplay oracle's reference/client modes accept the backend switch.
The extraction tool's `--hle-events FILE` records semantic voice events as
CSV; `decode_sound.py` also decodes optional accurate note-release records.

The HLE worker owns sequencer state, sample voices, filters, effects delay
lines and 48 kHz PCM. It never reads mutable shared RAM or writes machine
state. The main thread consumes complete published packets and acknowledges
the ring deterministically, retaining packet count/hash, clock, reset,
consumer offset and direct-note program-selection context in snapshots.
No sound-CPU, chip state, worker queue, voice, host instance ID or PCM enters
an HLE snapshot or peer checksum. Worker failures propagate to the caller.
All queues and voice/note pools are bounded; exhaustion fails explicitly.

The output-side command ledger retains 64 frames, up to 256 packets/frame.
Rollback copies the affected ledger, restores only main-side state and
records resimulated commands without stepping/restarting the worker. At the
old frontier, exact packet plus direct-program matches within two frames
reuse their original instance. Setters re-establish channel context before
new commands; matched starts/releases are not repeated. Missing direct notes
are cancelled by instance with a 240-sample (5 ms) linear fade. Commands at
the last two frames retain a two-frame grace period; a delayed matching
command consumes that pending cancellation. Cancelling also removes note
bookkeeping, without firing release-trigger layers. Music is not rewound:
matched sequence starts are suppressed and genuinely new commands take effect
at the worker's current monotonic time. Completed sounds cannot be unplayed.

Accurate netplay still replaces speculative PCM and publishes only confirmed
audio. HLE netplay instead drains the independent speculative stream; peers
may hear different corrected histories while their canonical game state
must agree. The handshake includes backend selection. A snapshot restore
alone is not an audio rollback transaction: callers must bracket correction
with `begin_rollback(begin,end)` and `end_rollback()`; the netplay core does so.
Headless extraction synchronizes with the worker when draining PCM, making
offline output independent of scheduling without involving audio in gameplay.

## Verification and tolerances

Acceptance is correct note/SFX identity and timing, close pitch/level/envelopes,
and useful listening captures, not waveform parity. The observations below
are automated measurements; no human listening approval is claimed.

`runtime-devices` and `hle-audio` pass under CTest. The latter exercises real
ROM synthesis, a sound-CPU callback that fails if invoked, worker isolation,
snapshot independence, parked-sequence direct SFX, arrangement selection/stop,
identical replay, changed instruments, missing SFX and one/two-frame leeway.
The actual native frontend also completed 900 HLE frames with sound PC0 and
zero main-CPU fallback.

### Oracle comparison

Both extraction paths boot the real interpreted main for 900 frames, freeze
at main tick244300326, and inject the same packets into the real mailbox.
Accurate extraction uses the interpreted sound oracle.

| Capture | Observed agreement |
| --- | --- |
| Sequence8, five seconds, volume116 | 43/43 voice starts; sample intervals, FC, initial encoded volumes and K1/K2 agree. 41/41 note releases. HLE source onsets lead by 7.7–9.8 ms; release offsets stay about 7.7–7.9 ms. |
| Direct `$4002`, key39, velocity104; release at150 ms | Same sample interval, FC796 and encoded volume `$cba0`. HLE physical start is 2.68 ms earlier, release1.14 ms earlier. Peaks527/520; whole-clip RMS differs by +0.19 dB. |
| All27 ordinary sequence IDs, four seconds each | All1,362 logical sequence/track/key identities match. Onsets lead by8.73–15.12 ms (median11.12). Among1,317 paired observable release durations, p95 error7.49 ms, maximum11.38 ms. |
| Selected arrangement7, repeated mailbox starts at0/2/4 s | Both produce90 voice starts owned by child4, rather than by arrangement7. |
| Selected arrangement7, uninterrupted17 s | Both produce399 intro notes from child4 and145 from child5. Child5 begins at13.6215 s HLE versus13.6443 s oracle. |

The ordinary gallery has1,719 HLE physical starts versus1,718 oracle CR
start edges. Logical note identities still all match: physical legato
retargeting is not a one-to-one CR edge. Pairing by logical identity and
sample interval yields1,714 voice comparisons:

- 1,565 initial frequencies exactly match; p95 absolute pitch error2.28 cents.
  1,666 are within5 cents,1,690 within25 cents,1,713 within110 cents.
- 3,318/3,428 encoded left/right volume bytes exactly match;3,378 differ by
  at most one encoded step. These bytes are nonlinear, not linear PCM gains.
- Residuals are not hidden by these percentiles: sequence25 track3 has
  approximately one-semitone onset differences; sequence10 has an unmatched
  physical-start/legato case and an octave-scale outlier in the grouped
  comparison. Some modulation/allocation-dependent volume differences are
  larger than one step. Full physical allocator/service-order parity is not
  claimed.

Immediate HLE dispatch omits sound-CPU queue and service latency. The measured
onset offsets and section-transition error are below32 ms in these captures;
they are not a promise of cycle-identical timing. Filters are continuous-time
approximations at48 kHz; volume/filter smoothing and reverb diffusion differ
from the native-rate oracle. The music8 whole-clip RMS difference is−1.24 dB.
Only the observed preset11 has a measured effect target; other configurations
use the same cheap parameterized topology, not individually validated models.
Other-game banks are not supported by this Land Maker implementation.

### Performance

Release build on this Apple-arm64 workstation; three sequential runs of
`f3rt-gameplay-regression --seed 5 --frames 6000 --wav ...`, strict native main,
same game-video path. These are end-to-end throughput numbers, not isolated
audio-thread timings.

| Audio backend | FPS | Relative to HLE |
| --- | ---: | ---: |
| HLE | 908.1 | 1.00 |
| Accurate native sound | 440.1 | HLE2.06× faster |
| Accurate interpreted sound | 180.8 | HLE5.02× faster |

All three: PC`1136`, frame CRC`04ea93ae`,1,628,668,881 main cycles,
80,338,233 native blocks, **zero fallback instructions**. HLE emits4,886,006
48-kHz frames; accurate emits3,029,425 native-rate frames. Accurate native
and interpreted WAVs are byte-identical, SHA256
`9cc8b2028974c345b56bf1b78d4a4134c924b05ae3e6276830491b66a9ed7f0e`.
This is a finding about those accurate paths, not evidence that HLE byte
parity is impossible.

### Real rollback and cancellation

A real local UDP relay used RTT80 ms, jitter20 ms, loss3%, reorder3%,
duplicate1%, seed5. Two HLE clients ran6,000 versus frames, delay2/window16.
P1 withheld input at frame1500 for120 ms. The separate no-network reference
and both peers finished with state CRC`97a71e83`, frame CRC`0a1902d4`,
1,950 canonical commands and4,886,006 audio frames. Strict native execution
remained enforced.

| Observation | P1 | P2 |
| --- | ---: | ---: |
| Rollbacks / maximum depth | 302 / 16 | 304 / 16 |
| Reused commands | 973 | 860 |
| Missing-SFX cancel commands | 20 | 26 |
| Actually active cancelled voices / observed stops | 15 / 15 | 16 / 16 |
| Maximum cancel-to-stop main ticks | 79,772 | 79,778 |

Some cancel commands target already-finished voices. Every observed active
cancel stopped within5 ms; P2's withholding-event correction reached depth16.
Its full-window stall lasted83.23 ms. PCM CRCs differ (`d17b3dde`/`d15518fa`),
as expected for non-rewound speculative histories, without a game-state desync.

The isolated PCM cancellation regression compares against an uncancelled
audible reference: all240 samples follow the linear5-ms fade within two
int16 LSB, then the dry voice is silent. That proves the cancellation path
does not hard-cut a waveform. The netplay run proves actual cancellations
complete; neither check substitutes for listening to the final mix for pops.
Reverb tails are deliberately allowed to decay rather than being hard-cut.

### Listening artifacts and reproduction

Artifacts remain local under `wt/hle-audio/build/`; none is committed:

| Files | Content / common listening gain |
| --- | --- |
| `listen-music-{hle,oracle}.wav` | Five-second sequence8 comparison,32× |
| `listen-sfx-{hle,oracle}.wav` | Two-second direct-note/release comparison,32× |
| `listen-gameplay-{hle,oracle}.wav` | About102 s of seed5 gameplay,16× |
| `listen-netplay-hle-p{1,2}.wav` | Actual impaired peer output,8× |
| `gallery-{hle,oracle}.wav` | Raw108-second sequence gallery |
| `arrangement-transition-{hle,oracle}.wav` | Raw17-second intro/loop transition |

Listening copies apply one identical fixed gain to both members of a pair,
with no clipping, time shifting, independent normalization or resampling.
Raw captures (`music-*`, `sfx-*`, `gameplay-*`, `netplay-*`) retain runtime
levels. HLE is48 kHz and accurate is29,761 Hz.

```sh
ROM=/Users/darien/Workspace/f3-stuff/roms/landmakr
build/f3rt-sound-extract --rom-dir "$ROM" --audio-backend hle \
  --packet 038108 --packet 04860874 --seconds 5 --wav-window event \
  --wav build/music-hle.wav --hle-events build/music-hle.csv
build/f3rt-sound-extract --rom-dir "$ROM" \
  --packet 038108 --packet 04860874 --seconds 5 --wav-window event \
  --wav build/music-oracle.wav --sound-trace build/music-oracle.sound
build/f3rt-sound-extract --rom-dir "$ROM" --audio-backend hle \
  --packet 068d02014002 --packet 068e02012768 --at .15:058f020127 \
  --seconds 2 --wav-window event --wav build/sfx-hle.wav
ctest --test-dir build --output-on-failure
```

## Integration merge verification

The merge of `hle-audio` (`5e8c775`) into `integration` (`4c74107`) was checked
from the clean main integration worktree, not `wt/imgui-netplay`. The new
documentation layout is retained: no root `STATUS.md`, concise opt-in user
guidance, and HLE evidence here under `docs/developer/`.

A fresh Release build of all targets completed in `build/hle-merge`, using
`PYTHONPATH=/private/tmp/sb-context-oracle/lib/python3.13/site-packages` and
`F3_ROM_DIR=/Users/darien/Workspace/f3-stuff/roms/landmakr`.
`ctest --test-dir build/hle-merge --output-on-failure` passed both
`runtime-devices` and `hle-audio` (2/2).

Both actual frontend runs completed 1,800 frames:

```sh
build/hle-merge/landmakr --headless --frames 1800 --unthrottled \
  --wav build/hle-merge/merged-default.wav
build/hle-merge/landmakr --headless --frames 1800 --unthrottled \
  --audio-backend hle --wav build/hle-merge/merged-hle.wav
```

Both reported PC`1136`, frame CRC`f08f089c`, 488,600,676 main cycles,
27,238,881 native blocks and **zero CPU fallback instructions**. The default
selected `audio_backend=accurate sound_driver=native`; HLE selected
`audio_backend=hle sound_driver=none` with sound PC0. The 231 retained video
renderer fallback frames in each run are separate from CPU fallback.
WAVs remain ignored local artifacts.

The VitePress production build also passed. A Chromium preview verified the
rendered opt-in HLE guide, explicit emulated default, and relocated developer
evidence link. This merge check does not replace the broader historical
sequencer, performance and impaired-netplay measurements above.

