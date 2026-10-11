---
source_url: relayed by user (runtime investigation results)
ingested: 2026-10-10
sha256: 2a922d3756655948730ef63bd317a417f4da10ac9da3d7934a2dacbe12f0c35d
game: [commandw, landmakrj]
rom_sha256:
  commandw: 09941ebd5dea6b128c3375560dbe3942e55bd0942e50e791c062d88a8cc5dab1
  landmakrj: aa050f39f5810c90bc20d1e3ed1c2510b1df067242535ad80aa86b3d174d49bc
captured_with: "f3rt reference backend in a throwaway git worktree of HEAD (2026-10-10), instrumented es5510.cpp / audio.cpp channel zeroing; landmakrj via f3rt-gameplay-regression --seed 1 --frames 2400; commandw headless --frames 1800"
---
Experiment results (our runtime, reference/LLE backend, routing identical to MAME 0.289 taito_en.cpp + esqpump: OTIS pair0 → Aux bypass to output; OTIS pair1/2/3 → ES5510 SER0/SER1/SER2 registers; output read from SER3; gain 0.18 per channel):
- In both commandw and landmakrj the loaded ESP microprograms read SER0L/R (regs 235/234) at PC 2/4, SER1L/R (237/236) at PC 2/5/14 and SER2L/R (239/238) at PC 2/4/6, and write their stereo output only to SER3L/R (241/240) at PC 14/16. The 11 ESP microprograms in sound ROM 0x1c000..0x1f000 are byte-identical between commandw and landmakrj.
- The sound CPU never writes ES5510 host register 0x18 (serial direction bits); it stays at its reset default.
- Zeroing OTIS pair 1 (→SER0) in commandw (1800 frames): 713,727 of 1,817,654 samples differ, peak 5,364 → 1,101, RMS 300.72 → 109.89. Zeroing pair 2 (→SER1): 1,199,658 samples differ. Zeroing pair 3 (→SER2): 441,966 samples differ.
- Zeroing OTIS pair 0 (Aux bypass): 0 samples differ in commandw (1,817,654 samples) and in landmakrj (2,423,540 samples, 2400 gameplay frames, seed 1).
- The voice-start trace in landmakrj gameplay: 744 voice starts, 2 on pair 0 (boot probes on voice 0 at volume 0). commandw attract: pair1 146, pair2 626, pair3 1, pair0 1. Sound driver at 0xc1959e does add.w #$100,d2 (selector +1 → pair bits), so selector 3 wraps to pair 0; landmakrj ROM has 5,120 selector-3 layers among 54,912 enabled layers in 24,000 program records, but none reached pair 0 in this run.
- Gain experiment (commandw): OTIS→ESP gain 1.0 instead of 0.18 raises RMS 5.55× (300.72 → 1668.4) and peak 5,364 → 29,931 with 0 clipped samples over 1800 attract frames. Our runtime compensates for the MAME mix level after the DSP with Audio::output_boost = 12.0.