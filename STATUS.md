IN PROGRESS

# Phase 3 — Land Maker sound-driver decoding

Worktree `wt/audio`, branch `audio`, based on `checkpoint-2-video` (`4200b04`). Local commits only; no pushes or edits to integration/other worktrees.

## Landed observation milestone

- Lossless interpreted-sound-CPU bus capture plus main mailbox/reset writes, effective 16 MHz device timestamps and PCM ordinals. Default sound execution, ES5505/ES5510 and SDL3 are unchanged.
- `--sound-trace FILE` on frontends and seeded gameplay; gameplay also supports `--wav FILE`.
- `tools/decode_sound.py` resolves voice pages, sample word addresses, pitch, loops, volume/filter, bank, output pair and DSP programming latches. Full register stream or onset projection; generated data stays ignored.
- Initial evidence and capture format in `docs/SOUND-DRIVER.md`.

## Exercised evidence

- Seed 5 ×6000 frames: 80,338,232 native main blocks, zero fallback; 3,029,425 stereo PCM frames. Traced/untraced WAVs byte-identical. Raw trace contains 9,643 main writes, 10,489,728 sound writes, 1,420,314 sound reads and one warm reset.
- Decoder extracts 2,204 stopped-to-running voice transitions, including two silent startup probes. Three decoder boundary regressions pass; `f3rt-check` passes.
- Fresh attract native/interpreter runs ×3600 frames: complete WAVs byte-identical, 1,817,655 stereo PCM frames; native 51,507,335 blocks, zero fallback. SHA-256 `62bac3a4d1baef4e7c2343ce987e44eb654a71f90d653b5b88c1d12d15eff05b`.
- MAME baseline seconds 20–54 remains exactly 0.9956761849580716 / 0.9952171577492129 correlation, RMS 18.722660457703864 / 19.499802079856668 LSB, fixed lag -1 sample.

## Remaining acceptance

ROM-backed command ownership/format, sequencing and allocation analysis is active. The bus/voice log alone does not yet establish per-command causal ownership. No host driver reimplementation or host-model parity is claimed at this milestone. Next: complete decoded command log first, then implement and compare the opt-in host driver model and finish sequence extraction/documentation.

Evidence is local ignored `build/seed5*` and `build/oracle-*`. Reproduce capture and waveform comparisons with README and docs/SOUND-DRIVER.md.
