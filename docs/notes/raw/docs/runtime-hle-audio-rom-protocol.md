---
source_url: docs/developer/HLE-AUDIO.md:8-116
ingested: 2026-10-11
sha256: 3363fd507af07f9787452bdc30f167ed89319732a8634c754830c7e1e5623f06
game: landmakrj
rom_sha256: aa050f39f5810c90bc20d1e3ed1c2510b1df067242535ad80aa86b3d174d49bc
captured_with: "repository documentation text (runtime-derived, not hardware); source file deleted after ingest"
---
<!-- excerpt: docs/developer/HLE-AUDIO.md lines 8-116 -->
## ROM-derived protocol and data

Enhanced is the default audio backend; when unavailable for a loaded set (if its
sound or sample ROM data fails load-time HLE capability validation), the default resolves to Reference. Reference runs the
cycle-accurate chips and uses the recompiled native sound driver when compiled
in, otherwise the interpreted Musashi driver. Enhanced audio uses the ROM-data
sequencer and must not execute the sound CPU, OTIS device or ESP instruction
program. Its inputs are the loaded sound and sample ROMs, not extracted traces.

The mailbox is **1024 bytes**, not 8 KiB: main `$c00000..c003ff`, inside
2048-byte shared RAM. `$c00480/481` publishes twice the byte index;
`$c00482/483` acknowledges it. Sound sees only even lanes at `$140000`.
Main `$2fde` appends inclusive-length packets and masks each byte index with
`$3ff`. See [SOUND-DRIVER.md](../SOUND-DRIVER.md) for the complete command grammar.
`8f` really releases the first matching sequence/track/key, through `$c141ce`.
It has no instance ID.

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

Gunlock, Recalhorn, Twin Cobra II and Puchi Carat ROMs were evaluated. Their
sound images differ in song and instrument tables, but the entire code bank
`$10000..1ffff` (sound address `0xc08000..0xc1ffff`), including fixed tables and DSP
configuration/program bytes, is byte-identical to Land Maker. Only `$c085f2` (bend
range: 0x02 in Land Maker, 0x0c in Twin Cobra II) differs and is read dynamically from
ROM. Per-game data comprises the program table at `$c00134`, song bank at `$c20000+`,
and sample ROM. Load-time capability validation (`Sequencer::validate` and
`Synth::validate`) validates the loaded sound and sample ROMs dynamically without
game-specific config flags or literals.
Physical PCB wiring observed on hardware (12's routing note,
`docs/notes/raw/docs/otis-esp-dac-routing-2026-10-09.txt`) swaps OTIS pairs 1
and 3 relative to MAME and leaves pair 0 unconnected. Enhanced follows it: pair
3 (channels 6/7) → ESP inputs 0/1, dry pass-through; pair 2 (channels 4/5) → ESP
inputs 2/3, damped stereo cross-delay (~274.8 ms) plus secondary reverb send;
pair 1 (channels 2/3) → ESP inputs 4/5, dry plus diffused reverb; pair 0
(channels 0/1) is not connected and is silent. Reference keeps MAME's routing.
The isolated ES5510 oracle experiment replayed 15,609 actual host writes from
the boot capture and used 1024- and 4096-level impulses. At 4096, input2 has
first peak2663 at native frame1, then echoes at 8178L2094,16355R946,24532L427,
32709R191. The cross-delay interval is 8177/29761 seconds (~274.8 ms), feedback
~0.45. Input4 has dry3779 at frame0, early reflections449L520/687R520 and
signed diffusion taps. Reverb remains above quantization noise through two
seconds. The Enhanced routing test and 2400-frame Land Maker measurement are
recorded in the wiki's runtime evidence note.

ROM menu callbacks identify parameter2 as delay-address increments
`value*$7000` (24-bit address, eight fractional bits), parameter6's high nibble
as damping and low nibble as route balance, and parameters8/9 as complementary
ROM-table wet/dry gains. Parameter0 controls decay/diffusion and parameter1 a
signed feedback coefficient. These control semantics must remain responsive;
a fixed pre-rendered impulse is not the runtime implementation.
