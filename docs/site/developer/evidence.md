# Evidence and technical records

f3-recomp's exercised target is Land Maker Japan 2.01J (`landmakrj`). World
(`landmakr`) has a configuration but no recorded validation. Measurements below
apply to the named builds, ROM sets, inputs, and sampled frames, not every F3 game
or every possible game state.

## What the comparisons establish

- **Native versus interpreter:** agreement between CPU execution paths using the
  runtime's device models. Native sound translates the sound ROM; it is not HLE.
- **Game-data video versus FDP:** agreement with the internal MAME-derived
  TC0630FDP renderer for supported scenes.
- **MAME captures and device replay:** compatibility with recorded emulator output,
  not measurements of a physical chip or board. Audio waveform differences remain.
- **Snapshots and netplay:** repeatability and rollback agreement for the recorded
  schedules, not independent emulation-accuracy verification.

Keep physical-hardware evidence distinct from software-model comparisons. A CRC,
pixel match, correlation metric, and listening observation answer different questions.

## Canonical records

| Record | Scope |
| --- | --- |
| [Validation scope](https://github.com/ansxor/f3-recomp/blob/main/docs/developer/VALIDATION.md) | Historical combined-build coverage and limits; links the detailed canonical measurements without duplicating them. |
| [Decisions](https://github.com/ansxor/f3-recomp/blob/main/docs/developer/DECISIONS.md) | CPU coverage, timing rationale, ROM observations, and compatibility decisions. |
| [ABI changes](https://github.com/ansxor/f3-recomp/blob/main/docs/developer/ABI-CHANGES.md) | ABI history and canonical snapshot inventory. |
| [Game-data video](https://github.com/ansxor/f3-recomp/blob/main/docs/developer/VIDEO-HLE.md) | Land Maker producer addresses, parity measurements, and unsupported scenes. |
| [GPU video](https://github.com/ansxor/f3-recomp/blob/main/docs/developer/GPU-VIDEO.md) | Presentation design, interpolation boundaries, and recorded performance. |
| [Combined binary size](https://github.com/ansxor/f3-recomp/blob/main/docs/developer/BINSIZE-COMBINED.md) | Recorded combined size experiments. |
| [Exclusion experiments](https://github.com/ansxor/f3-recomp/blob/main/docs/developer/BINSIZE-EXCLUDE.md) | Binary-size exclusion experiments and coverage tradeoffs. |
| [Profile experiments](https://github.com/ansxor/f3-recomp/blob/main/docs/developer/BINSIZE-PROFILE.md) | Profiling and tier/slim measurements; experimental builds are not general-play support. |
| [Sound driver](https://github.com/ansxor/f3-recomp/blob/main/docs/SOUND-DRIVER.md) | Driver traces, mailbox format, native execution, and audio compatibility limits. |
| [Netplay](https://github.com/ansxor/f3-recomp/blob/main/docs/NETPLAY.md) | Protocol, supported frontend contract, and recorded network scenarios. |
| [Historical user-document examples](/developer/user-doc-evidence) | Unique retained examples removed from user-facing pages, not current defaults or results. |

## Reproduce and interpret

Start with [Testing strategy](/developer/testing/). Use the focused pages for
[MAME captures](/developer/testing/mame), [instruction comparisons](/developer/testing/differential),
[gameplay regression](/developer/testing/gameplay-regression),
[frame comparison](/developer/testing/frame-compare),
[audio comparison](/developer/testing/audio-compare),
[sound traces](/developer/testing/sound-tools), and
[snapshot/netplay checks](/developer/testing/netplay-oracle).

The records preserve evidence rather than promise that it was rerun on your build.
Report the revision, configuration, command, reference, and comparison boundary
when adding a new result. Keep ROMs, captures, traces, and extracted assets outside
committed documentation.
