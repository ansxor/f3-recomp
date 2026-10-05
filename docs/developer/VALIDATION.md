# Validation scope

This is the scope of the historical `binsize-combined-3-human` checkpoint,
not an assertion that every game state or F3 title works. Detailed measurements,
hashes, command logs and artifact paths are retained once in the
[combined build report](BINSIZE-COMBINED.md), rather than copied into a root
project-status file.

## What was exercised

| Area | Evidence and limits |
| --- | --- |
| Game and host | Japanese Land Maker 2.01J (`landmakrj`), macOS arm64; Metal for GPU presentation. No World, other-game or other-GPU-host validation. |
| Native execution | Strict-native main/sound build with ABI 3, reviewed exclusions and full-coverage compile tiers. Attract and held-out seeded gameplay completed without CPU fallback. |
| Video | Captured MAME attract frames and RAM compared exactly. Game-data/CPU/GPU comparisons cover sampled layers, composites and induced transitions; they do not verify physical TC0630FDP behavior. |
| Audio | Attract and held-out WAVs matched the preserved native reference. Native/oracle equivalence is distinct from MAME waveform agreement or physical-board fidelity; the known instruction-atomic audio residual remains documented in [sound investigation](../SOUND-DRIVER.md). |
| Snapshots and relay | Save/load replay, allocation and Go checks passed at the checkpoint. This was not a new impaired-network campaign; see [netplay investigation](../NETPLAY.md) for that separate evidence. |
| Human run | The foreground GPU/automatic-integer/border-48 window exited normally. Key-versus-window-close exit, completed-match count and a human-played campaign ending were not independently established. |

Full coverage means all existing **nonexcluded** dispatch entries are retained,
not that every unsupported lowering is implemented or every exclusion is proven
safe in all states. Slim's historical held-out failures make it unsuitable as a
general-play cutover. The original human profile is a partial campaign snapshot;
its lost tail was not recovered.

## Provenance

The original sequence was `binsize-combined-1-merged` (semantic merge),
`binsize-combined-2-gated` (automated checks), then
`binsize-combined-3-human` (foreground run). All numerical results, exact commands,
per-seed outputs and the recorded human-run duration/counters remain in
[BINSIZE-COMBINED.md](BINSIZE-COMBINED.md). Its ignored
`build/combined-evidence/` paths refer to the original experiment worktree,
not files distributed with this repository.

For present implementation boundaries and another game's requirements, use the
[portability audit](../site/developer/porting.md). Earlier diagnoses and superseded
observations are in [historical development notes](DECISIONS.md).
