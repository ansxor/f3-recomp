# Historical examples from user documentation

These excerpts were moved out of guide/reference pages during the documentation cleanup. They record old local observations, **not current acceptance criteria or expected output**. The original pages did not identify an exact source revision or a complete reproduction environment. Do not use their hashes, counts or addresses as cross-build correctness guarantees.

For maintained evidence and scope limits, see [Developer evidence](/developer/evidence).

## Frontend summary: 120-frame headless run

Previously in `guide/running.md`, described as a run in the author's build:

```text
set=landmakrj frames=120 pc=0x1136 sound_pc=0xc108ea sound_driver=native frame_crc=0x2493e2ff cycles=32573410 native_blocks=1853837 fallback_instructions=0 audio_frames=60588 audio_peak=0 nonzero_samples=0
```

## Frontend video compare: 700-frame headless run

Previously in `guide/video.md`, described as a `--renderer compare-cpu` run in the author's build:

```text
VIDEO layer=composite domain=320x232-RGB sampled_frames=469 compared_pixels=34818560 pixel_mismatches=0
VIDEO game_frames=469 oracle_fallback_frames=231
VIDEO fallback=lines producer_pc=0x1003a frames=229 first=1 last=417
VIDEO fallback=text producer_pc=0x0 frames=1 first=115 last=115
VIDEO fallback=sprites producer_pc=0x10412 frames=1 first=418 last=418
```

Only supported game-renderer frames were compared. The startup fallback counts are observations from this run, not fixed timing promises. The old getting-started page also reported zero audio peak in a 700-frame run; that is a startup observation, not a sound-quality check.

## Local netplay: 300-frame finite run

Previously in `guide/netplay.md`, described as the end of a local two-client run:

```text
netplay_ready player=1 delay=2
VIDEO game_frames=185 oracle_fallback_frames=115
...
netplay_confirmed=300 state_crc=3254973699 rollbacks=0 max_rollback_depth=0
set=landmakrj frames=300 pc=0x1016e sound_pc=0xc18f82 sound_driver=native frame_crc=0x2493e2ff ...
```

Both clients reportedly printed the same state CRC. This was a finite connectivity/determinism example, not evidence of a played-through versus match or an impaired-network campaign.

## Discovery counts

Previously in `reference/generated-files.md`, an unspecified local Japan ROM build reported **464523 decoded instructions** and **584053 addresses that did not decode**. All-aligned discovery considers every even address, including data and overlapping instructions; these counts do not measure native gameplay coverage and may predate config exclusions.

## Evidence retained elsewhere, not copied here

- Generated-source/executable sizes and profile entry counts: [combined build report](https://github.com/ansxor/f3-recomp/blob/main/docs/developer/BINSIZE-COMBINED.md).
- Seed-5 native/oracle sound trace and WAV comparisons: [sound-driver evidence](https://github.com/ansxor/f3-recomp/blob/main/docs/SOUND-DRIVER.md).
- Native/oracle throughput and correction-depth measurements: [netplay limits](/developer/netplay/limits) and [netplay document](https://github.com/ansxor/f3-recomp/blob/main/docs/NETPLAY.md).
- GPU presentation/interpolation measurements: [GPU report](https://github.com/ansxor/f3-recomp/blob/main/docs/developer/GPU-VIDEO.md).

The old troubleshooting estimate of about 260 MB of main-CPU generated C was removed as stale: it did not describe the later combined build's exclusion/tier configuration. Use the dated build reports rather than that unqualified estimate.
