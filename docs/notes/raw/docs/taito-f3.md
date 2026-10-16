---
source_url: https://github.com/12Me21/taito-f3
ingested: 2026-10-15
sha256:              # n/a: content is pinned by the submodule commit below
git_commit: d95fddf8660f318578fff0f6fd4ae8d0108feda1
game:
rom_sha256:
captured_with: 12Me21/taito-f3 notes (git d95fddf), git submodule at raw/docs/taito-f3/
---

12Me21's Taito F3 hardware notes: PCB tracing, chip pinouts, PAL dumps, oscilloscope timing captures
and FDP/FDA hardware tests, plus the draft website under `website/`. Mounted as a git submodule so
the files stay byte-identical to upstream; the files carry no per-file frontmatter. This page is
their provenance record.

Drift check: `git -C docs/notes/raw/docs/taito-f3 rev-parse HEAD` must equal `git_commit`. Moving
the submodule to a new commit is a re-ingest: update `git_commit`, diff the changed files, and
re-check every page that cites them.

Evidence kind: `doc` (see [[SCHEMA]]).

## Re-ingest 2026-10-15 (a070cbd → d95fddf)

Changes across commits `eaa5a0a`, `fea9d1d`, `be2f796`, `d95fddf`:
- `5505-5510.txt` (new): 12's oscilloscope signal tracing and timing measurements for the serial audio interconnect between OTIS (ES5505), FCM (TC0660FCM), ESP (ES5510), and the TDA1543 DAC. Documents serial channel routing through FCM (OTIS ser1..3 → FCM → ESP serio0..2, ESP serio3 → FCM → DAC data), register presets (OTIS Host Serial Control = `$63` Sony slave mode, ESP SERMODE = `$07F8`), FCM replacement of 74x174 4x D flip-flop and 74x373 8x transparent latch, and 128-BCLK LRCLK frame timing (16 bits high left sample, 112 bits low right sample, BCLK rising edge data transition).
- `clocks.txt` (new): master clock frequencies (main CPU 15.23809 MHz, audio CPU 15.23809 MHz, OTIS 15.23809 MHz, ESP 8 MHz, DUART 4 MHz); audio bit clock (BCLK = 3.8095225 MHz) and sample rate (~29.76189453 kHz, BCLK/128).
- `chips.txt`: added MC33274 [AP] quad op-amp, TDA1543 pin counting note (zig-zag order), TA8221A 30W dual audio power amp, and TC0660FCM link to Ensoniq 5701 "SUPER GLU" datasheet.
- `list.txt`: audio nets traced: BCLK (otis.12 → esp.16, dr2.8, c8.2, dac.1, fcm.49), WCLK (otis.10 → pal6.5 → pal6.15 → esp.18), LRCLK (otis.11 → esp.17, dr2.9, dac.2); DAC analog output chain (dac.6/dac.8 through dual op-amps ic53, ic54, ic63 to stereo output connector); ESP control/bus lines (`esp halt`, `esp cs/as`, `esp/otis rw`, `esp gate`, `esp hdb0..7`).
- `website/fcm.html`: pinout table updated with audio clock/serial pins (48-49, 51-58), ESP RAM address/data latch pins (62-78), and ESP gate (79).
