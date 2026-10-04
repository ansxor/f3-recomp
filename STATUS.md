DONE — Phase 6 GPU automatic internal resolution; production cap 4, checkpoint tag `gpuvideo-5-auto-scale`.

## Behavior

- GPU-only `--video-scale auto-integer`: largest fitting integer including border, physical SDL window pixels, clamp 1–4; nearest 1:1 centered on black. Below 1x centrally crops. Linear window filter cannot introduce fractional scaling.
- GPU-only `--video-scale auto`: ceil limiting fit ratio, clamp 1–4; existing aspect-preserving nearest/linear final blit. Above the cap the final image upscales rather than supersamples.
- Numeric 1–4 behavior and CPU default preserved. Headless auto stays at startup 1 without SDL; netplay explicitly requires fixed 1/border 0.
- Resize/fullscreen/pixel-density polling; 100ms quiet/250ms maximum debounce. Same-scale changes update viewport only. F11/Alt+Enter fullscreen/back.
- Scale setter replaces only resolution-dependent textures and already-used diagnostic storage. No device/asset/pipeline rebuild or GPU-idle wait. Opt-in interpolation works across scale 1.
- Constructor canonical buffers/snapshot layout remain fixed. Host reference scale is separate; native trail history is retained. No machine, native renderer, sound, snapshot schema or netplay changes.
- Integrated audiosync pre-enqueue 50ms queue cap, rolling deadline and late-clock resync retained; scale work follows audio enqueue.

## Exercised verification

- Final 27-case corpus: 108,000 native frames, 7188 full composites, 2160 comparisons of each of nine isolated layers; **zero differing pixels, zero fallback instructions**.
- Fixed scales 1–4, borders 0/48; changing scales across seeds 5/6/7/41 and linear/fit at both borders. One changing run checks every frame. Repeated `1→3→2→4→1` transitions force exact first-image checks.
- Canonical bytes identical before/after each setter. Same-seed/border fixed-1 and changing runs retain identical machine/audio/state CRCs, cycles, native blocks, sample counts/peaks; independent CPU replay and retained trail history are exact.
- Actual Mac frontend in both auto modes: 1800 frames each, Retina 2x pixels, real 640×480/1280×720/1920×1080/2560×1440 resize, 3024×1898 fullscreen, restored 1920×1080. Water windows/fullscreen/back captured and visually inspected. Six-call resize burst creates no extra targets.
- Both window modes: native frame CRC `fb9bec22`, 26,271,485 native blocks, zero fallback, byte-identical 908,827-frame WAVs. Real setter costs 0.031–0.080ms. Capture pauses and an induced 250ms stall produce 9 pacing resyncs each; queue drops 0/1, mean 32.212/30.305ms, max 61.792/61.826ms after enqueue. The 50ms cap is before enqueue, not a post-enqueue bound.
- Frozen first-frame transitions: 25 samples per target after warmup; setter worst 0.068ms, setter plus first fenced/readback image worst 8.800ms. All 104 resized images exact; canonical bytes unchanged.
- Sequential frozen scale 1–8 measurements select cap 4. GPU fenced/readback mean/p95/worst at 4: 5.998/7.631/9.985ms; at 5: 8.318/13.044/14.552ms before native CPU (~3.4ms), interpolation/presentation. Diagnostic API/harness still measures 1–8; player flags do not expose 5–8.
- Fresh actual headless auto-integer/fit run: 250,114,560 native RGB checks, zero mismatches, frame CRC `3359f200`, 51,507,335 native blocks, zero fallback; 1,817,655-frame WAV byte-identical to prior native integration baseline.
- Fresh `F3RT_GPU=OFF` build and actual CPU-only frontend/device check pass. Actual CLI rejects CPU auto, numeric 5 and automatic netplay with clear errors. Geometry smoke covers border 0/48, exact thresholds, one-pixel crossings, cap and undersized crop.

## Evidence and limits

- Full design, timings, geometry, replay commands and qualified results: `docs/GPU-VIDEO.md`, section “Automatic internal resolution (phase 6)”. README/help/site and ABI docs updated.
- External logs/captures/WAVs retained under `/tmp/f3-gpuvideo/auto-scale`; throwaway drivers and their binaries removed after proof. No ROMs, generated machine code or binaries committed.
- Runtime exercised on M5/Metal only. Retina density and real fullscreen changes observed; no physical second-monitor density transition or other GPU host exercised.
- Ending coverage is an induced producer boundary, not a played campaign ending. No auditory listening claim; queue behavior and native WAV identity are measured.
- Scale 4 is an intentional measured production cap, not a blocked implementation. CPU/off interpolation defaults unchanged. No Phase 7 generalization included before this checkpoint.
