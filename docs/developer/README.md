# Developer evidence

Normal build, controls and online-play instructions live in the
[README](../../README.md) and [user guide](../site/guide/index.md). These files
retain technical contracts, historical measurements and investigation notes;
they are not a support promise or a requirement to run the game.

## Current contracts and workflows

- [Generation and verification workflows](WORKFLOWS.md).
- [CPU ABI changes](ABI-CHANGES.md): current ABI 3 and earlier interface history.
- [Game scope and porting audit](../site/developer/porting.md): generic F3 pieces, Land Maker assumptions and deferred refactors.
- [Sound-driver investigation](../SOUND-DRIVER.md): ROM-specific driver semantics and native/oracle comparison.
- [Opt-in HLE audio](HLE-AUDIO.md): ROM-data synthesis, non-rewound rollback policy, tolerance-based comparisons and known limits.
- [Netplay usage and contracts](../NETPLAY.md): versus-only lifecycle and player workflow.
- [ImGui/netplay implementation and evidence](IMGUI-NETPLAY.md): overlay, shaders, canonical host handoff, lifecycle campaigns and observed limits.
- [Experimental motion interpolation](MOTION-INTERP.md): render-only GPU geometry history, presentation timing, snap rules and ROM verification.

## Measurement archive

- [Validation scope](VALIDATION.md): the historical combined-build checkpoint, host limits and references to detailed evidence.
- [Exact runtime optimization measurements](WORKFLOWS.md#exact-runtime-optimization-measurements): retained changes, reversal scope, paired timings and profiling limits.
- [Combined binary-size measurements](BINSIZE-COMBINED.md): full-coverage optimization tiers, profile provenance and observed gates.
- [Exclusion experiment](BINSIZE-EXCLUDE.md): reviewed intervals and the failed broader candidate.
- [Profile experiment](BINSIZE-PROFILE.md): retained cold-code tiers versus the rejected slim general-play build.
- [Game-data video investigation](VIDEO-HLE.md): Japanese ROM producer addresses, scene contracts, sampled output and fallback limits.
- [GPU video measurements](GPU-VIDEO.md): compositor contracts, historical checkpoints and presentation surveys.
- [Historical development notes](DECISIONS.md): the implementation/diagnosis record, including superseded observations.

Measurements belong to their named revision, ROM set and host. Video compares
against the MAME-derived renderer or captured MAME output, not verified physical
TC0630FDP behavior. Audio has separate native/oracle, device-replay and integrated
MAME-output comparisons; none is a claim of physical-board waveform equality.

Commands run from the repository root unless a worktree/cwd is explicitly
recorded. Local `build/` and `/tmp/` paths identify ignored artifacts from the
original investigation; they are not checked-in downloads. ROM-derived code,
traces, audio and images must remain untracked.
