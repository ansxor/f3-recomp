---
source_url: relayed by user (runtime test and render results)
ingested: 2026-10-10
sha256: 577559433a9d20fdd8058870674eb14e4025c1b24a7495318d2da8018ce51eda
game: [landmakrj]
rom_sha256:
  landmakrj: aa050f39f5810c90bc20d1e3ed1c2510b1df067242535ad80aa86b3d174d49bc
captured_with: "user-reported runtime/tests/audio.cpp Audio.EnhancedEffectsRoutingMatchesHardwareWiring and landmakrj 2400-frame Enhanced render"
---
Enhanced routing test: `runtime/tests/audio.cpp` test `Audio.EnhancedEffectsRoutingMatchesHardwareWiring` checks the hardware OTIS→ESP wiring implemented by Enhanced: pair 3 (channels 6/7) to dry inputs 0/1, pair 2 (channels 4/5) to delay inputs 2/3, pair 1 (channels 2/3) to reverb inputs 4/5, and pair 0 silent. Reference remains unchanged and follows MAME routing.

A landmakrj 2400-frame Enhanced render measured RMS −25.02 → −25.30 dBFS and peak −6.52 → −6.61 dBFS following the routing change.
