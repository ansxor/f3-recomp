DONE — opt-in implementation and automated verification; listening WAVs ready

# HLE audio — independent threaded sequencer and sample mixer

Task: `/Users/darien/Workspace/f3-stuff/prompts/hle-audio.md`.
Worktree `wt/hle-audio`, branch `hle-audio`, created from integration `faf81d9`.
Only this worktree is modified. Local commits use `ansxor <git@ansxor.ca>`; no pushes or ROM/assets in commits.

## Required invariants

- Existing emulated/native accurate sound remains the default and oracle. HLE is opt-in.
- HLE does not execute the sound 68000 or ES5505/ES5510 programs; it decodes music/SFX data and mixes sample ROM directly at a modern rate.
- Audio engine runs on its own thread and is never restored by game rollback. Main-side command accounting remains deterministic and rollback-able.
- Corrected input reconciles commands with small frame leeway; missing speculative SFX receive fast-fade cancellation. Music is not rewound.
- Strict native main execution and peer state synchronization must remain intact.

## Delivery

All three implementation phases completed. Accurate remains the default;
`--audio-backend hle` selects the independent 48 kHz ROM-data engine.
Build environment: `PYTHONPATH=/private/tmp/sb-context-oracle/lib/python3.13/site-packages`.

- Protocol: 1024-byte command ring inside 2048-byte shared RAM. `8f` releases
  a matching key, not an instance; rollback uses a host-instance 5 ms fade.
  ROM decoders cover all27 Land Maker sequences and100 instrument selectors.
- Effects: measured preset11 delay/reverb response; cheap parameterized
  equivalent, not ESP execution. Four other available F3 sound ROMs share
  this firmware bank; their song/instrument banks are not supported.
- Worker: immutable ROM inputs, bounded queues/pools, no shared-RAM reads,
  no sound CPU/chip execution, no rollback. Canonical main-side protocol
  state stays deterministic; two-frame matching suppresses duplicate SFX.
- Build: landmakr, f3rt-run, extraction, gameplay, netplay and check targets
  built. CTest runtime-devices and hle-audio pass.
- Oracle: all1,362 logical sequence/track/key identities match in the27-song
  gallery. Onset lead8.7–15.1 ms; paired pitch p95 error2.3 cents. Selected
  intro/loop transitions also match note identities.
- Performance, seed5/frame6000: HLE908.1 FPS, accurate-native440.1,
  accurate-interpreted180.8. All frame CRC`04ea93ae`, zero CPU fallback.
  Accurate native/interpreted WAVs have identical SHA256.
- Real impaired netplay: both6000-frame peers and reference state CRC
  `97a71e83`, frame CRC`0a1902d4`.302/304 rollbacks, maximum depth16;
  all31 observed active cancellations stopped within5 ms. Isolated PCM
  regression proves a240-sample linear fade within two int16 LSB.

Evidence, reproduction commands and residuals: [docs/HLE-AUDIO.md](docs/HLE-AUDIO.md).
Listening copies: `build/listen-{music,sfx,gameplay}-{hle,oracle}.wav` and
`build/listen-netplay-hle-p{1,2}.wav`. Common gain per pair; no clipping.
Raw captures and traces remain under ignored `build/`; no assets committed.

Known limits: modulation/physical-voice allocation and some onset levels
differ; sequence25 track3 has about one-semitone onset pitch differences;
one sequence10 legato/start pairing is an outlier. Filters, smoothing and
reverb approximate the oracle. No human ear approval or impossibility of HLE
byte parity is claimed. Default remains accurate pending user approval.

