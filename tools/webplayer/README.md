# Web music player and visualizer (tools/webplayer)

A static web page that plays the music of a Taito F3 game from its **sound test** and shows it like a DAW
(zoomable timeline of the note data per track, instrument per note, effects and mixer, synth behaviour).
The page is generated from a game's ROMs by a native tool. Audio is rendered live in the browser by the
**same HLE audio engine** the game uses, compiled to WebAssembly. Everything the UI draws on the timeline is
**prebaked at export time** by the native tool; the browser never computes or emits timeline data.

This file is the developer documentation of the feature and the contract between its parts. Sections 2-6
define interfaces precisely enough for independent implementation; section 8 lists the acceptance checks.
Statements marked **[measured]** were observed on this repository at `5cb929d` (probe described in 9.1);
**[INFERENCE]** marks anything not observed. Reverse-engineering claims about the sound ROM format are
hypotheses (`docs/notes` is the wiki; `docs/notes/raw` is never edited from here).

Scope: games `kirameki` (Kirameki Star Road) and `landmakrj` (Land Maker), HLE audio engine for sound ROM
`5a7e9117` code. Any other set only works if its sound ROM passes `hle::validate()`.

## 0. Decisions at a glance

| # | Decision |
|---|---|
| D1 | `hle::Core` (new, `runtime/audio/hle/core.{hpp,cpp}`) is the single, thread-free implementation of the mixing path (Synth + Effects + Sequencer, tick->frame loop, gain math, command/bank/reset/gain handling). `AudioEngine::Impl` becomes thread + mailbox + epoch mapping + PCM ring around one `Core`. WASM and the exporter use `Core` directly. |
| D2 | Replay of a sound-test song is a tiny binary **replay script** (`F3RP`), applied by one shared function `hle::replay()`. All items execute at worker tick 0. Verified bit-exact (int16) against `sound-extract` for Kirameki 0x187 **[measured]**. |
| D3 | Trace data is **prebaked natively**: a recorder attached to `Core` through zero-cost null-checked taps, run by the exporter for intro + one loop iteration (default), ending at the sequencer's arrangement wrap, hard cap 360 s. |
| D4 | Trace format: per song `<id>.json` (small structured metadata) + `<id>.bin` (columnar little-endian typed arrays addressed from the JSON). Browser loads both with `fetch`; no parser library. |
| D5 | Time base of every trace field: **output frames at 29762 Hz since the replay origin** (u32), derived from 16 MHz ticks with `frame = floor(tick * 29762 / 16000000)` (12's PCB observation: 15238090 / (16 * 32) = 29761.8945 Hz, rounded to 29762 Hz over MAME's integer truncation). |
| D6 | WASM: `-fwasm-exceptions`, no threads, no SharedArrayBuffer, no emscripten JS glue; `extern "C"` ABI of section 6 with `double` for 64-bit quantities (no BigInt). AudioContext created with `sampleRate: 29762`. |
| D7 | Seek = rewind + fast-forward through the **same** render path with output discarded, so the state is identical by construction. No snapshots. |
| D8 | Generated site and every ROM-derived byte go to `build/webplayer/` (git-ignored through `build/`); never committed. |

## 1. Data flow and ownership

```
 ROMs --> f3rt-tool webplayer-export  (native; Machine boot via tools/runner/sound/capture.hpp)
            |  1. boot to freeze; boot packet stream, gain bytes, per-song request calls
            |  2. write sound.bin / samples.bin / manifest.json / games.json / songs/<id>.replay
            |  3. per song: Core (native, FTZ on) + replay + TraceRecorder  -> songs/<id>.json/.bin
            v
        build/webplayer/ (static site)  --serve-->  browser
            main thread: app.js (UI, timeline from prebaked trace, transport)
            AudioWorklet: worklet.js + f3hle.wasm (Core + C ABI) renders audio live
```

| Part | Files | Notes |
|---|---|---|
| Core + replay (**done, M1**) | `runtime/audio/hle/core.{hpp,cpp}`, `replay.{hpp,cpp}` (decoder **and** encoder), refactor of `audio.cpp`, tests `runtime/tests/hle_core.cpp` | runtime lead (this spec's author) |
| Instrumentation (**done, M2**) | `runtime/audio/hle/observe.hpp` (types final), hooks in `sequencer.*`, `synth.*`, extra `VoiceEvent` fields in `events.hpp`, all `Core` query methods implemented in `core.cpp` | runtime lead |
| WASM build + C ABI | `runtime/audio/hle/wasm_api.cpp`, CMake target `f3hle` in `build-wasm` | wasm worker |
| Exporter, recorder, bundle writer | `tools/commands/webplayer_export.cpp`, `tools/webplayer/{trace_recorder,bundle}.{hpp,cpp}` | generator worker; builds on `tools/runner/sound/capture.hpp` (music-export owns it and the song list) |
| Web app | `tools/webplayer/site/{index.html,app.js,worklet.js}` (tracked sources; copied into the bundle) | UI worker |

**Milestone 1 and 2 (landed): notes for the other workers.**

* Sample rate is **29762 Hz** throughout (12's PCB observation: master clock 15,238,090 Hz, 32 voices: `15238090 / (16 * 32) = 29761 + 229/256 = 29761.89453125 Hz`, rounded to 29762 Hz for integer rate APIs).
* WASM compile set: `core.cpp replay.cpp sequencer.cpp synth.cpp` (+ header-only `effects.hpp`, `observe.hpp`),
  `-I include -I runtime -isystem runtime/third_party/faust`. Native: `core.cpp` and `replay.cpp` are in
  `F3RT_CORE_SOURCES` (`libf3rt`).
* `core.hpp` is in its final form and all methods are implemented: `Core::set_trace`, `effects_info`, `describe_program`,
  `describe_sequence`, `sequences_running`, `bend_range`. `observe.hpp` holds the final types (`TraceTap`, `SequenceObserver`,
  `SequenceInfo`, `EffectsInfo`, `ProgramInfo` with decoded `EnvelopeInfo`/`LayerInfo`/`DirectRegionInfo`/`SplitInfo`).
* `VoiceEvent` has all M2 fields populated by `Synth::Impl::emit`: `slot`, `sounding_key`, `velocity`, `filter_mode`,
  `direct`, `program`, `pitch`, `glide`, `lfo`, `env[3]`.
* Deviations from the first draft of 2.2/3.2: `Core` keeps Synth/Sequencer/Effects behind a pimpl (its header does not
  include `effects.hpp`); `AudioEngine::Impl` is now the `PcmSink` and stores `generated` from `core.frames()` after each
  Core call (it used to store it after each `render_until`; it is only polled); `replay.hpp` also exports
  `REPLAY_VERSION`, `REPLAY_MAX_PACKET`, `ReplayScript` and `encode_replay`; `Core::set_gain` masks the channel with 1.
* Tests: `runtime/tests/hle_core.cpp`, linked into `f3rt-test-audio` for every `F3_GAME` (it loads `kirameki` and
  `landmakrj` from the siblings of `F3_ROM_DIR` and skips what is missing). Includes A4 (instrumentation neutrality).

Song discovery (which `(bank, code)` pairs are songs, their labels) belongs to music-export. The exporter
needs, per song: sound-test bank `N` (or none), request code, label.

## 2. Single-threaded core

### 2.1 What exists today (`AudioEngine::Impl`, `runtime/audio/hle/audio.cpp`)

`Impl : VoiceSink` owns `Synth synth; Effects effects; Sequencer sequencer;` (declaration order matters: the
sequencer is constructed with `*this` as sink) plus `worker_tick`, `worker_frames`, `gain[2]`, `gain_model`,
and a worker thread consuming a 4096-entry message queue (`Command | Advance | Reset | Gain | Bank`). Per
message the worker does `advance_worker(max(worker_tick, m.tick))` and then applies the message:

```
advance_worker(tick): while (worker_tick < tick) {
                          next = min(tick, sequencer.next_tick());
                          render_until(next); sequencer.advance_to(next); worker_tick = next; }
render_until(tick):   end = floor(tick*29762/16e6) computed as tick/M*S + tick%M*S/M;
                      per ch: input[ch]  = .18;
                              output[ch] = OUTPUT_BOOST * gain[ch];
                      while (worker_frames < end) { n = min(256, end - worker_frames);
                          synth.render(buses, n); effects.mix(buses, stereo, input, output);
                          append n stereo frames to the PCM ring; worker_frames += n; }
Command: sequencer.command(packet, instance, worker_tick)
Reset:   sequencer.reset(worker_tick) [-> sink.reset: synth.reset(t); effects.reset(); gain = {1,1}];
         synth.set_bank(1)
Bank:    sequencer.set_bank(v, worker_tick); synth.set_bank(v)
Gain:    gain[ch] = powf(10, -.5 * (63 - (raw & 63)) / 20)
```

Everything above except the queue, PCM ring, mailbox parsing and the main/worker tick-epoch mapping
(`Impl::push`: `playback_epoch`/`clock_epoch`, for slot loads) moves into `Core` unchanged. Output channel
order stays OTIS/ESP; the single left/right exchange stays in `Audio::render` (native) and moves into
`f3hle_render` (WASM) - each *final* output applies it exactly once.

### 2.2 `hle::Core` public API

```cpp
// runtime/audio/hle/core.hpp   (namespace f3rt::hle; includes only events.hpp, observe.hpp and <std>; Synth/Sequencer/Effects stay behind a pimpl)
inline constexpr float OUTPUT_BOOST = 12.0f;   // == Audio::OUTPUT_BOOST (static_assert in hle/audio.cpp)

class PcmSink {                                 // called per block of 1..256 frames, in render order
public:
    virtual void write(const float *stereo, size_t frames) = 0;   // interleaved L,R, OTIS/ESP order
protected: ~PcmSink() = default;
};

class Core final {
public:
    static constexpr size_t BLOCK_FRAMES = 256;
    Core(std::span<const uint8_t> sound_rom, std::span<const uint16_t> sample_words); // throws std::runtime_error (same checks as hle::validate)
    ~Core();  Core(const Core&) = delete;  Core &operator=(const Core&) = delete;

    // Output / configuration. Call only while no advance() is running (native: after AudioEngine::flush()).
    void set_pcm_sink(PcmSink *sink);               // nullptr: frames are rendered and dropped (fast-forward)
    void set_voice_observer(std::function<void(const VoiceEvent &)> observer);  // existing --hle-events path
    void set_trace(TraceTap *tap);                  // native exporter only; nullptr = off (section 4.6). M2: declared, not defined yet

    // Time. Worker timebase: 16 MHz ticks since construction, monotonic. A tick argument smaller than tick()
    // is clamped to tick() (as max(worker_tick, m.tick) today).
    uint64_t tick() const;                          // worker_tick: sequencer events up to here have fired
    uint64_t frames() const;                        // worker_frames: frames handed to the sink so far
    uint64_t next_boundary() const;                 // sequencer.next_tick(): next 1 kHz / 12 ms timer tick
    static uint64_t frames_at(uint64_t tick);       // floor(tick * SAMPLE_RATE / MAIN_CLOCK), overflow-free

    // Inputs. Each first runs advance(tick), then applies at tick() (identical to the worker's switch).
    void advance(uint64_t tick);
    void command(std::span<const uint8_t> packet, uint64_t instance, uint64_t tick); // [len, op, args...]; throws on invalid
    void reset(uint64_t tick);
    void set_bank(uint8_t page, uint64_t tick);     // `page` = value Machine passes to Audio::set_sound_bank
    void set_gain(unsigned channel, uint8_t raw, uint64_t tick);  // raw = mailbox byte 0x7fa/0x7fb; uses raw & 63

    // Const queries (no state change, exporter only): see 4.7. M2: declared in core.hpp, types in observe.hpp,
    // NOT DEFINED YET - calling one is a link error, never a silent no-op.
    EffectsInfo effects_info() const;
    std::optional<ProgramInfo> describe_program(uint16_t program) const;
    std::optional<SequenceInfo> describe_sequence(uint8_t id) const;
    bool sequences_running() const;
    uint8_t bend_range() const;

    static void enable_flush_denormals();           // FTZ|DAZ on x86 (SSE) and aarch64 (FPCR.FZ); no-op elsewhere incl. WASM
};
```

`Core` has no thread, mutex, atomic, SDL, `Machine`, mailbox or `Audio` dependency. It does not call
`enable_flush_denormals()`; the thread driving it does (the AudioEngine worker as today; the exporter's thread;
tests). Exceptions thrown by `Synth`/`Sequencer`/`Effects` propagate out of the `Core` call that caused them and
leave the `Core` in an unspecified state (the AudioEngine worker already latches the first exception into
`failure`; the WASM ABI converts it to an error code, section 6).

`AudioEngine::Impl` after the refactor keeps: `shared`, `clock`, `command_count`, `serial`, epochs,
`consumer`, `held`, the queue + mutex/cv, `pcm`/`pcm_mutex`/`generated`, mailbox parsing (`shared_write`),
`push`/`submit`/`flush`, `save`/`load`, and a `Core core`. Its `PcmSink` implementation holds `pcm_mutex`
and does today's ring write and overflow drop; `generated` is stored from `core.frames()` after each Core call
(it is only polled). Worker `run()`: `Command -> core.command(p.data(), p.instance, m.tick)`, `Reset ->
core.reset(m.tick)`, `Bank -> core.set_bank(m.value, m.tick)`, `Gain -> core.set_gain(m.channel, m.value,
m.tick)`, `Advance -> core.advance(m.tick)`. `AudioEngine::set_observer` calls `flush()` then the
matching `Core` setter. **Native output must stay
bit-identical** (acceptance A1).

### 2.3 Headers outside the HLE directory and how they are isolated

Complete include audit of `runtime/audio/hle/*`:

| Header | Pulled in by | Used for | Handling |
|---|---|---|---|
| `f3rt/audio.hpp` (-> `f3rt/memory_map.hpp`, `<functional>`, `<memory>`, `<span>`, `<string_view>`) | `hle/audio.hpp` | `Audio::OUTPUT_BOOST`, `Audio::EnhancedStats` | Only the `AudioEngine` wrapper keeps it. `Core` declares `hle::OUTPUT_BOOST`; `hle/audio.cpp` has `static_assert(Audio::OUTPUT_BOOST == hle::OUTPUT_BOOST)`. `core.hpp` never includes it. |
| `f3rt/memory_map.hpp` | `sequencer.cpp`, `synth.cpp` (`SOUND_ROM_BASE/END/SIZE`), `audio.cpp` (`MAILBOX_*`) | constants | Header has only `<cstddef>/<cstdint>`: usable in the WASM build as is (`-I include`). `MAILBOX_*` use stays in `audio.cpp`, not in `Core`. |
| `<xmmintrin.h>`, `<pmmintrin.h>` | `audio.cpp` (`enable_ftz_daz`) | FTZ/DAZ | Moves to `core.cpp` as `Core::enable_flush_denormals()` under `#if (defined(__x86_64__) \|\| defined(_M_X64) \|\| defined(__SSE__)) && !defined(__EMSCRIPTEN__)` / `#elif defined(__aarch64__)` / else empty. WASM has no FTZ/DAZ (section 7). |
| `<thread>`, `<mutex>`, `<condition_variable>`, `<atomic>`, `<exception>` | `audio.cpp` only | worker | Stay in `audio.cpp`; `sequencer.cpp`, `synth.cpp`, `effects.hpp`, `events.hpp`, `note_timing.hpp`, `core.cpp` use none. |
| `faust/dsp/dsp.h`, `faust/gui/UI.h`, `faust/gui/meta.h` | `effects.hpp`, `faust/support.hpp`, generated DSPs | FAUST interfaces | Vendored, header-only: `runtime/third_party/faust` (`-isystem`). Generated `faust/generated/*.hpp` are committed; the FAUST compiler is never needed (`FAUSTFLOAT` is `float`, effect DSP state is `double`). |

WASM compile set: `core.cpp sequencer.cpp synth.cpp replay.cpp wasm_api.cpp` with
`-I include -I runtime -isystem runtime/third_party/faust`, C++20. `Synth.cpp` uses `<bit>`/`<span>`.
Nothing in the compile set includes a game header or `Machine`.

### 2.4 Determinism properties the design relies on (all **[measured]** with the probe of 9.1)

* **On the timer grid the output is independent of how the caller slices time (bit-exact); below a millisecond it
  is not.** `advance()` splits every call at `sequencer.next_tick()` (the 16000-tick timer grid), so any schedule of
  targets that are multiples of 16000 ticks makes the same render calls as 1 ms stepping. Probe, Kirameki 0x187, 25 s:
  steps of 16000, 160000 (`sound-extract`'s slice), 70001 ticks and 128-frame quanta gave the same FNV-1a hash of all
  744025 float frames. With steps below one frame (1, 100, 537 ticks) the render differs: for 4 s, 20 of 238088 floats
  differ, by at most 1.5e-8 (1 ulp), 0 samples after int16 conversion. Splitting only `Synth::render` per frame stays
  bit-exact, splitting only `Effects::mix` per frame reproduces the deviation: `Effects::mix` keeps per-call state
  (auto-bypass decision, LFO phase carried by `skip_lfos`) [INFERENCE: the first difference lies at a bypass -> active
  transition, i.e. where a call starts the program some frames earlier than another call split would]. Consequences:
  the WASM ABI, the exporter and `sound-extract` step on the grid (`advance(next_boundary())` or whole milliseconds) and
  are bit-exact with each other; the native game steps at machine-frame and message ticks as it always did, which is
  within float noise of the grid result. Acceptance A3 is split accordingly (grid: bit-exact; arbitrary steps:
  `<= 1e-6` absolute).
* FTZ/DAZ on vs off: bit-identical output over the same 25 s (no denormal reached the output).
* Attaching a `Synth` observer: bit-identical output, wall time 0.220 s vs 0.220 s for 25 s of audio.

## 3. Song replay recipe

### 3.1 What `sound-extract --audio-backend enhanced --sound CODE [--bank N]` does to the engine

(`tools/commands/sound_extract.cpp` / `tools/runner/sound/capture.cpp`, read at HEAD; music-export may refactor
it, the recipe below is about the engine-visible effect.)

1. **Boot** (`--boot-frames`, default 900): the interpreted main CPU runs from cold boot; the engine consumes
   the boot mailbox packets in real time. Frozen after the consumer caught up. A post-boot snapshot is taken
   (`AudioEngine::save`: clock, command count, consumer, held; machine RAM including the mailbox ring and the
   gain bytes `shared[0x7fa]`, `shared[0x7fb]`).
2. **Per code**: a *new* machine (hence a new engine, worker tick 0, frame 0) is built and the snapshot loaded:
   `AudioEngine::load` sets `clock_epoch = clock = snapshot clock`, `playback_epoch = worker_tick = 0`, `held =
   false`. Then
   * `set_reset(true)`: `held = true`, `consumer = 0`, queue `Reset` at worker tick 0.
   * `set_reset(false)`: `held = false`, then `shared_write(0x7fa)` and `shared_write(0x7fb)` queue `Gain`
     (channel 0 then 1, raw = the two snapshot bytes) at worker tick 0.
   * if `--bank N`: `write16(0x300000 + 2N, 0)` -> `Machine::write16` -> `Audio::set_sound_bank(page)` ->
     `Bank(page)` at tick 0. `page` is not `N` for N >= 8 (Machine: `idx = N & 0x1f; if (idx >= 8) idx -= 8`).
     The exporter must record the page actually delivered (`m->audio->sound_bank()` after `apply_sound_bank`).
   * the game's main-CPU request routines run in the interpreter, in this order: [category/prelude codes: for
     Kirameki categories <= 1, the two setup codes (effect preset `0x20`, parameters `0x21 x N`)], then the code
     itself. Each routine writes length-prefixed packets into the ring and updates the producer index;
     every producer write calls `shared_write(MAILBOX_PRODUCER_L)`. **The first such write re-parses the ring
     from byte 0**, i.e. the whole boot packet stream followed by the new bytes, in ring order; later writes
     parse only what is new. Packets longer than 8 bytes are consumed but not queued. Each queued packet gets
     `instance = ++serial` (1, 2, ...).
   * **No emulated time passes during setup**: every queued item has worker tick 0 (observed: first event of a
     direct-note request is at tick 0; all music events fall on the 16000-tick grid **[measured]**).
3. **Render**: `advance(10 ms)` + `render()` slices until the cap or until `--tail` of silence follows the first
   audible sample; the WAV is trimmed to `first_audible - 20 ms .. last_audible + 50 ms` and L/R are exchanged
   once. int16 = `trunc(clamp(v * 32768, -32768, 32767))`.

The worker processes the items in queue order, so the engine sees exactly:

```
RESET  ;  GAIN 0 raw0 ; GAIN 1 raw1 ;  [BANK page]  ;  PACKET boot_1 .. boot_n ; PACKET call_1 .. call_m     (all at tick 0)
then  advance(...)  forever
```

### 3.2 `Core` calls and the replay script

```cpp
core.reset(0);                       // sequencer.reset(0); synth/effects reset; gain {1,1}; synth.set_bank(1)
core.set_gain(0, raw0, 0); core.set_gain(1, raw1, 0);
if (has_bank) core.set_bank(page, 0);
uint64_t serial = 0;
for (pkt : boot_packets ++ call_packets) if (pkt.size() <= 8) core.command(pkt, ++serial, 0);   // longer ones are skipped, as natively
// then core.advance(...) / render
```

That sequence is serialized as the **replay script** (`songs/<id>.replay`, produced by the exporter, applied by
`hle::replay(Core&, std::span<const uint8_t>)` in `runtime/audio/hle/replay.cpp`, used by the exporter, by the
tests and by the WASM ABI - one implementation of the recipe). Little-endian, no padding:

```
u8[4] "F3RP"   u8 version = 1   u8 reserved = 0   u16 item_count
items:  u8 op, payload
  0x01 RESET                                   core.reset(0)
  0x02 GAIN    u8 channel(0|1), u8 raw         core.set_gain(channel, raw, 0)
  0x03 BANK    u8 page                         core.set_bank(page, 0)
  0x04 PACKET  u8 n (2..8), n bytes            core.command(bytes, ++serial, 0); bytes[0] must equal n
```

`runtime/audio/hle/replay.{hpp,cpp}` holds the only implementation of the format: the encoder next to the decoder.

```cpp
struct ReplayScript { std::array<uint8_t,2> gain_raw; std::optional<uint8_t> page; std::vector<std::vector<uint8_t>> packets; };
std::vector<uint8_t> encode_replay(const ReplayScript &);   // RESET, GAIN 0, GAIN 1, [BANK], PACKET...
void replay(Core &, std::span<const uint8_t> script);
```

`encode_replay` throws `std::invalid_argument` for a packet that is not `2..8` bytes long with `bytes[0] == size`
(the engine drops longer mailbox packets, so the exporter filters them out of `packets`, as the native path ignores
them) and for more than 65535 items. `replay()` requires a freshly constructed `Core` (`tick() == 0 && frames() ==
0`), validates the **whole** script before applying anything (bad magic, version, non-zero reserved byte, truncated
or unknown item, GAIN channel above 1, PACKET length outside `2..8` or with a wrong length byte, trailing bytes) and
throws `std::invalid_argument` for any of those and for a used `Core`; exceptions thrown by the applied `Core` calls
propagate unchanged. It does not advance time. The human-readable form lives in `manifest.json` (`boot.packets`,
`songs[].calls`); the exporter builds `ReplayScript` from the same data, calls `encode_replay`, and fails if the
manifest form and the script disagree. Tests: `runtime/tests/hle_core.cpp` (`ReplayCodec.*` need no ROM).

Sample-exactness reference **[measured]**: Kirameki `--sound 0x187` (category 0): calls `0x4f9:032005`,
`0x4fa:04210028 04210100 04210214 0421031a 04210425 04210500 04210668 04210740 04210820 04210927`,
`0x187:03812c 04862c7f`; gain raw 48/48 (`10^(-.5*15/20) = 0.4217`). A single-threaded replica of the
worker (probe, 9.1) fed `RESET, GAIN, GAIN` + those 13 packets (boot stream omitted - the prelude overrides
preset/parameters for this song) and rendered 25 s. After L/R exchange and int16 conversion, all
**633510 frames x 2 of the baseline `sound-extract` WAV were identical** (0 differing samples; the WAV starts at
core frame 6162 = first audible frame minus the 20 ms lead). For sets without a prelude (Land Maker: preset
and parameters come with the boot stream) the boot stream is required; acceptance A2 covers it.

## 4. Trace: prebaked DAW data

### 4.1 Time base

* Replay origin = worker tick 0 = output frame 0. The browser starts audio at frame 0 of the same replay, so
  `trace frame == audio playhead frame` with no offset (the baseline WAV's trim is a native-tool artifact only).
* `frame(tick) = floor(tick * 29762 / 16000000)`; computed as `tick / 16000000 * 29762 + tick % 16000000 *
  29762 / 16000000` (= `Core::frames_at`). Sequencer events at tick T first affect frame `frame(T)`: `render_until(T)`
  renders everything before `frame(T)`, then `sequencer.advance_to(T)` fires the events.
  `Synth`'s internal 1 kHz service (envelopes, LFO, glide, parameter updates) fires before the first frame
  whose own grid tick `floor(k*16e6/29762)` is >= the timer tick: that is `ceil` instead of `floor` of the same
  quotient, at most one frame (34 us) apart. Trace fields use `frame(tick)` for all events.
* Seconds = frames / 29762. Musical grid: 96 sequencer pulses per quarter note (`ticks_per_bar = (384 >> exp) *
  numerator`); pulses run at `rate * 1000 / 625` per second, so `rate` is quarter-note BPM (hypothesis from
  the arithmetic; the UI shows `rate` and does not need the hypothesis). Every pulse is on a 16000-tick grid
  point, so all sequencer events are within 1 ms (30 frames) of linear interpolation between bars.
* Frames are stored as `u32` (up to 40 h). Ticks appear only as JSON numbers (< 2^53).

### 4.2 Song structure in the HLE and what "loop" means

* A request starts sequence `S` (`0x80`/`0x81` are identical in the HLE). With bit 7 set (`0x81 0x87`-style
  selection lists, owner `track 10` holds `0xe7` arrangement entries) the owner plays **child sections** one
  after another: `arrangement_next` starts child `c` (a new `SongState`: accumulator reset, tracks re-initialized
  from the child's header), each entry has a repeat count (`arrangement[3] & 0x7f`, the same child restarts via
  `initialize_tracks` each time its bar boundary `bars * ticks_per_bar` pulses is reached), a transpose mask
  and transpose, and **entries with `arrangement[3] == 100` are loop-selection markers**. At the terminator
  (`0xe9` or end of list) the owner's phase resumes at the entry following the **last marker** (the wrap);
  with no marker, `arrangement_next` returns 0 and the song ends.
* A sequence that is not arranged simply stops (`rate = 0`) at its bar boundary.
* **The header fields `loop_start`/`loop_end` (`SEQ_HEADER_LOOP_START/END_OFFSET`) are parsed but never read by
  the HLE**; the loop is the arrangement wrap, not those fields. The trace reports them under
  `sequences[].header_loop` for the UI to display, labelled "unused by HLE".
* Therefore: **intro** = sections before the first section whose `phase` equals the wrap's `resume_phase`;
  **loop body** = from that section to the wrap tick.

### 4.3 How long each song is traced

Defaults (`webplayer-export --loops 1 --max-seconds 360 --tail 1.0`):

1. **Looping song** (an `arrangement_wrap` with `resume_phase != 0` is reported): trace frames
   `[0, frame(wrap_tick of the N-th wrap)]`, N = `--loops` (default 1) = intro + N loop iterations, ending
   exactly at the sequencer wrap. Notes that start at or after the wrap tick (the next iteration's first notes
   fire in the same tick) are **not** recorded. Rendering continues past the wrap, without recording new
   notes, until every voice started before the wrap has stopped (cap: +10 s), so every recorded voice row is
   complete; `trace.frames` stays the wrap frame, `trace.audio_frames` is where rendering ended.
2. **Non-looping song** (`resume_phase == 0` or no arrangement): record until `!sequences_running()`, then until
   the master output peak stays `<= 8/32768` for `--tail` seconds (the `sound-extract` criterion, same default 1 s),
   `end_reason = "song_end"`, `loop = null`.
3. **Hard cap** `--max-seconds` (default 360 s = 10.7 M frames) for songs that neither wrap nor end:
   `end_reason = "cap"`, `loop = null`.

`trace.span.end_reason` is one of `loop_wrap | song_end | cap`.

### 4.4 UI mapping of continued looped playback onto the prebaked timeline

Audio keeps looping live (the HLE sequencer wraps by itself); the timeline repeats its prebaked loop region.
With `L = loop.start_tick`, `E = loop.end_tick` (ticks, multiples of 16000), playhead `p` in frames:

```
if (loop == null || p < frame(E)) q = p                                   // identity
else { t = p * 16000000 / 29762;  t' = L + (t - L) mod (E - L);  q = frame(t') ; iteration = floor((t - L) / (E - L)) }
```

The UI draws the timeline window around `q`, shows "loop iteration k", and wraps the scroll position at the
wrap. Exactness: the loop period is the same every iteration because each section start resets the
sequence accumulator (`start()` -> `SongState{}`) and all section lengths are whole 1 ms timer ticks
[INFERENCE from the code: acceptance A6 checks it by exporting once with `--loops 2` and comparing the two wraps].
Known approximations, all documented in the UI: (a) the data shown for iteration >= 2 is iteration 1's - voices
ringing across the wrap, portamento from the previous key (`previous_key`) and the effect tails differ slightly;
(b) the note-level display is exact for iteration 1; (c) seeking beyond the trace into a later iteration works on
audio (fast-forward) and maps through `q`.

### 4.5 What the trace contains (v1)

Legend: **H** = hook that exists today; **N** = new read-only hook (section 4.6); **D** = derived by the recorder.

| Content | Source |
|---|---|
| Song header per sequence: bars, time signature (`numerator` = byte `+0x14`, `exp` = byte `+0x15`, denominator `2^exp`), `ticks_per_bar`, initial `rate`, header flags/linked alias, parsed-but-unused `loop_start/loop_end` | N: `Sequencer::describe(id)` |
| Sections: owner, child sequence, arrangement `phase`, repeat index, start/end frame, transpose, rate at start | N: `SequenceObserver::section_start/section_end` |
| Bar lines (frame of each `position % ticks_per_bar == 0`, bar index per section) and rate changes (`0x88`, pause `0x83`/resume `0x84`, seek `0x85`) | N: `SequenceObserver::bar`, `rate_change` |
| Loop region `{start_frame, end_frame, start_tick, end_tick, length_ticks, resume_phase, iterations}` | N: `arrangement_wrap` + D |
| Tracks = rows `(sequence, track 1..8)` and their channels | D from `note_on` / `channel_update` (`Note.sequence`, `Note.track`) |
| Notes: on frame (`Note.tick`), release frame (`note_off`), sequence key (`Note.key` 0..87 = sequencer event class), displayed key (`Note.kernel_key` = key + 21 + arrangement transpose, A0 = 21), velocity, program id (u16, `channel.program`: bits 14..15 bank, low byte program), section index, voice rows | H: `VoiceSink::note_on/note_off` |
| Voices (one row per Start..Stop of one ES5505 voice = one layer of one note): note, layer, slot 0..31, output pair 0..3, start/release/stop frame, sounding key (`Voice.effective_key` 21..108, after the channel transpose `parameters[0x35]`), sample region, loop/reverse flags, `direct` flag, onset values | H: `VoiceEvent` Start/Release/Stop; N: `slot`, `sounding_key`, `direct` fields |
| Per-voice curves, 10 lanes (4.8): pitch, glide, lfo, env_pitch, env_filter, env_amp, left, right, k1, k2 | N: extra fields on `VoiceEvent::Parameters` (the Synth already emits one per voice per service) |
| Channel parameter automation per `(sequence, track)` (4.9) | H: `VoiceSink::channel_update` + `note_on`'s `Note.channel` snapshots |
| Effects/mixer: preset selects with resolved program (`Reverb/Hall/Chorus/Echo/Mute`) and p0..p9, `effect_parameter` writes, gain bytes, bus topology per program, `OUTPUT_BOOST`, input gain 0.18, gain model | H: `VoiceSink::effect/effect_parameter`; N: `Core::effects_info()`, `TraceTap::gain` |
| Level timeline (peak per bus and per output channel, 512-frame windows) | N: `TraceTap::mix_block` |
| Instruments for every program id used: layers, envelopes, filter, glide, LFO, pitch routing, split regions with sample start/end/loop (4.10) | N: `Core::describe_program()` |
| Sample regions + 256-column min/max overview of each | D from `VoiceEvent.sample_start/end` and the sample ROM |

Not in v1 (needs new information the HLE does not expose or that is unknown): voice-stop reason (stolen vs envelope
end vs sample end), names of controllers B0..C6 other than the few listed in 4.9, instrument names (the ROM
carries none that the HLE reads), per-voice effect sends beyond `pair` (the HLE has no per-voice send level;
a voice is routed to exactly one bus pair).

### 4.6 HLE instrumentation (native-only users, zero cost when off, no audio change)

All instrumentation is a `nullptr` check on a pointer/`std::function` the hot paths already test or that is
tested once per event; **no hook runs per sample**. `mix_block` is one check per <= 256-frame block.
**[measured]** for the existing observer: attaching it gives identical output and identical wall time.

```cpp
// runtime/audio/hle/observe.hpp
struct SectionInfo { uint8_t owner, sequence, phase; uint16_t repeat_index, bars, ticks_per_bar;
                     uint8_t numerator, exponent, rate; int8_t transpose; uint16_t transpose_mask; };
enum class SectionEnd : uint8_t { Boundary, Repeat, Stopped, Finished };
class SequenceObserver {                       // Sequencer::set_observer(SequenceObserver*)
public: virtual ~SequenceObserver() = default;
  virtual void section_start(const SectionInfo &, uint64_t tick) {}   // start(), repeat, arrangement child, seek
  virtual void section_end(uint8_t sequence, SectionEnd how, uint64_t tick) {}
  virtual void bar(uint8_t sequence, uint32_t bar_index, uint64_t tick) {}   // position % ticks_per_bar == 0
  virtual void rate_change(uint8_t sequence, uint8_t rate, uint64_t tick) {} // 0x88 / 0x83 (rate 0) / 0x84
  virtual void arrangement_wrap(uint8_t owner, uint8_t resume_phase, uint64_t tick) {} // 0 = no marker: song ends
};
class TraceTap : public SequenceObserver {     // Core::set_trace(TraceTap*); all methods default to no-ops
public:
  virtual void note_on(const Note &) {}  virtual void note_off(uint64_t instance, uint64_t tick) {}
  virtual void channel_update(uint8_t sequence, uint8_t track, const Channel &, uint64_t tick) {}
  virtual void effect(uint8_t preset, uint64_t tick) {}
  virtual void effect_parameter(uint8_t index, uint8_t value, uint64_t tick) {}
  virtual void reset(uint64_t tick) {}
  virtual void voice(const VoiceEvent &) {}                           // Synth observer events, already ordered
  virtual void command(std::span<const uint8_t> packet, uint64_t instance, uint64_t tick) {}
  virtual void gain(unsigned channel, uint8_t raw, float linear, uint64_t tick) {}
  virtual void mix_block(uint64_t first_frame, const float *buses8, const float *dac_pair,
                         size_t frames, const std::array<float,2> &input_gain,
                         const std::array<float,2> &output_gain) {}  // pointers valid during the call
};
```

`Core` forwards each `VoiceSink` callback to synth/effects/trace **after** its own handling (so
`effects_info()` called from `effect()` already shows the new preset), composes `set_voice_observer` and
`set_trace` into one `Synth` observer, installs the tap as the `Sequencer` observer, and calls `mix_block`
from `render_until` after `effects.mix`. `Sequencer` hook sites: `initialize_tracks` (`section_start`; called by
start, repeat and `0x85`), `stop`/`finish`/boundary branch of `pulse` (`section_end`), `pulse` (`bar`), `apply` cases
`0x83/0x84/0x88` (`rate_change`), the terminator branch of `arrangement_next` (`arrangement_wrap`, with the marker
index + 1 or 0).

`VoiceEvent` (events.hpp) gains, filled in `Synth::Impl::emit`: `uint8_t slot` (index in `voices[32]`),
`sounding_key`, `velocity`, `filter_mode` (`Voice.filter.mode`), `bool direct`; `uint16_t program`
(`owner.channel.program`); `int16_t pitch` (`Voice.pitch`), `glide` (`Voice.glide_target`), `lfo` (`Voice.lfo`),
`env[3]` (`Voice.envelope[i].level()`). The CSV writer keeps its current columns. `Synth` needs `slot` stored in
`Voice` at `begin()` (set after `v = Voice{}`).

Legato (`allocation >= 2`, `begin()` reusing an active voice) emits only `Parameters` for the new note's
`instance` on an already-active slot, with no `Start`: the recorder closes the previous voice row at that frame
(`flags |= LEGATO_HANDOFF`) and opens a new row for the new note flagged `legato`, so every note owns its voice
rows. A repeated `Start` for an instance whose row was closed by a sample-end `Stop` (mono envelope loop) opens a
new row flagged `retrigger`.

### 4.7 Const query API (exporter only)

* `EffectsInfo Effects::info() const` (new accessor): `{Program program; uint8_t preset; std::array<uint8_t,10>
  p; float decay, damp, feedback, send, dry_send, chorus_to_reverb, chorus_out; double lfo_a, lfo_b, lfo_c
  /*rad per frame*/; float lfo_depth_frames, echo_frames, mod_center, enhance, hall_a, hall_g, hall_g2, hall_g108;
  bool bypassed;}` - the values `Effects::update()` already computes, unchanged.
* `std::optional<ProgramInfo> Synth::describe_program(uint16_t id) const` and `SequenceInfo
  Sequencer::describe(uint8_t id) const`: pure reads of the structures decoded at construction. They return
  `nullopt` for absent entries and never throw for missing data (a program that selects an unpopulated bank
  returns `kind = "absent"`).
* `uint8_t Synth::bend_range() const` (ROM byte `0xc085f2`; Land Maker `0x02`, per `docs/notes`).

### 4.8 Per-voice curve lanes and decimation policy

Source: one `VoiceEvent::Parameters` per voice per Synth service (1 kHz timer, a per-ms voice budget table):
**measured** median spacing 12 ms per voice (min 11, max 12), 40324 Parameters events for 1055 voices in 25 s
of Kirameki 0x187 (a very dense song: 785 notes / 21 s).

| Lane | dtype | Meaning (units) | Source field |
|---|---|---|---|
| `pitch` | i16 | final pitch, 1/256 semitone, 60*256 = key 60; `pitch/256 - sounding_key` = detune/bend/vibrato/envelope in semitones (hypothesis: units from `key*256` use in `begin()`) | `VoiceEvent.pitch` |
| `glide` | i16 | portamento path (`glide_target`), same units; `pitch - glide` = tuning + pitch envelope + LFO + bend | `.glide` |
| `lfo` | i16 | LFO output (depth x wave) | `.lfo` |
| `env_pitch`, `env_filter`, `env_amp` | i16 | envelope generator levels 0..0x7fff (generator 0/1/2 as the HLE uses them: pitch / filter cutoff / amplitude) | `.env[0..2]` |
| `left`, `right` | u16 | encoded volume `0x0ff0..0xfff0` (`0x0ff0` = silent floor); linear gain = `interp(G, e/256)`, `G[i] = (((i & 15) \| 16) << 11 >> (16 - (i >> 4))) / 2048` (16 entries per octave); includes velocity, volume, expression, pan and the amplitude envelope | `.left_volume/.right_volume` |
| `k1`, `k2` | u16 | ES5505 filter coefficients (`& 0xfff0`); filter mode per voice constant in the voices table (`filter_mode` 0..3: stages per `faust/voice_filter.dsp`: 0 = LP,LP,HP,HP; 1 = LP,LP,LP,HP; 2 = LP,LP,LP2,LP2; 3 = LP,LP,LP,LP2, with `a1` from k1, `a2`/high pole from k2) | `.k1/.k2` |

Policy: per voice, per lane, the exporter reconstructs the lane from the `Parameters` stream (plus the
`Start` onset values as the first sample) and applies **greedy piecewise-linear simplification** with a vertical
tolerance (`--curve-tol`, defaults in lane units: pitch/glide 2 (< 1 cent), lfo 256, env_* 256 (0.8 % of full
scale), left/right 170 (about 0.25 dB), k1/k2 320). A lane whose simplified
form is flat is stored as **absent** (count 0); its value is the voice's onset value column. A lane point is
`{u16 dt_frames since the previous point (first point: since voice start_frame); i16/u16 value}`, 4 bytes, an
extra repeated-value point is inserted if `dt > 65535`. Reconstruction between points is linear.

**[measured] size estimate** (Kirameki 0x187, from the `--hle-events` CSV, five lanes: `frequency` register in cents
with a 3-cent tolerance, left/right 170, k1/k2 320; the pitch lane's 2-unit tolerance is tighter and may give
somewhat more points): 41379 raw voice samples x 5 lanes = 207k lane samples reduce to 21.5k lane points (about
10x; 4.4 frequency, 4.7 left, 4.5 right, 4.0 k1, 2.8 k2 points per voice). At 4 bytes/point this is 86 KB for a 21 s very dense song. Order of magnitude for a
3-minute song with ten lanes: **0.5-2 MB of lane data**, voices table 9k rows x ~24 B = 0.2 MB, notes 0.1 MB,
levels 0.1 MB: **about 1-3 MB per song before HTTP compression** (monotone frame deltas compress well).
[INFERENCE: scaled linearly from one song.]

### 4.9 Channel parameter automation

Per `(sequence, track)` channel the recorder diffs consecutive `Channel` snapshots (every `channel_update` and
every `note_on` snapshot) and emits a change event per lane. Only offsets whose use the HLE code shows are named;
the rest are `raw_<hex>`:

| Lane | Source | Notes |
|---|---|---|
| `program` | `Channel.program` (u16) | bits 14..15 bank, low 8 bits program; changes via event `D9` selector / `0x8d` |
| `bend` | word `parameters[0x0a..0x0b]` minus `0x4000` (i16) | controller event `B0`; semitones = `bend / 0x4000 * bend_range` (`bend_range` from the ROM, in the trace header). The HLE uses it as modulation source 10 as well |
| `expression` | `parameters[0x14]` | `CHANNEL_PARAM_EXPRESSION`; added to attenuation when non-zero |
| `volume` | `parameters[0x20]` | `CHANNEL_PARAM_VOLUME`; attenuation table index |
| `pan` | `parameters[0x26]` (s8) | `CHANNEL_PARAM_PAN`; `0x80` = take the patch default |
| `transpose` | `parameters[0x35]` (s8) | `CHANNEL_PARAM_TRANSPOSE`, added to the key for splits/weights |
| `seq_volume` | `Channel.sequence_volume` | sequence master volume/fade (`0x86`/`0x89`), updated every 12 ms while fading |
| `pair_select` | `parameters[0x31]` | output pair selector: `<3` direct pair `(v+1)&3`, `5` = pair 3, `3/4` = take it from the patch |
| `layer_mask` | `parameters[0x1c]` | HLE reads a non-zero value as a layer-enable override; the header constant calls this offset `PAN_CONTROLLER_OFFSET` - **unresolved** |
| `mod_0c`, `mod_10`, `mod_12`, `mod_1a` | words at those offsets | modulation sources 12, 11, 9, 8 of the patch routing; real names unknown |
| `raw_XX` | any other changing byte among `0x0a..0x36` | controller events `B0 + k` write `parameters[0x0a + 2k]` (`k <= 0x16`: offsets below `0x38`; larger `k` throws in the sequencer) |

`parameters[0x22]` (previous-key memory) is internal state, excluded. Table `chan_events` rows:
`{frame u32, channel u16, lane u8, value i32}`; `channels[]` in the JSON maps channel index to `(sequence, track)`.
Interpolation policy per lane in the UI timeline:
* **`seq_volume`**: linear interpolation (the driver applies 12 ms fade steps).
* **`bend`**: step (discrete pitch wheel/controller events).
* All other lanes (`program`, `expression`, `volume`, `pan`, `transpose`, `pair_select`, `layer_mask`, `mod_*`, `raw_*`): **step**.
Lane values are simplified with the same greedy rule when they change every 12 ms (`seq_volume` fades).

### 4.10 Instruments

For every program id used, `describe_program()` returns (all offsets from `programs[slot].base`, program record
stride `0x198`; evidence = the `Synth` code that reads the byte; semantics are **hypotheses**, units as the HLE uses
them):

* `enabled` (`+0x196 >> 5`): `0` = **direct** program: 17 key-region records of 20 bytes at `base + i*20`: key range
  `[+10 .. +12]` (s8), sample id `+4 & 127` (bit 7 = reverse), transposition `+6` (s8), fine pitch `+8 & 15`,
  decay/release rates `+0 & 127`, `+2 & 127`, level `+0x10 & 127`, filter index `+0xe`, pan/pair `+0x12`; raw 20 bytes also
  returned. Otherwise a **layered** program: mask of layers 0..2 (bit per layer; channel `layer_mask` may override).
* program glide rate index `+0x17e` (nonzero enables glide for allocation mode 2 or when a previous voice exists).
* per layer (patch = `base + layer*0x76`, 0x76 bytes returned raw as well):

| Offset | Field | Use in the HLE |
|---|---|---|
| `+0x00 + 20*e`, e = 0,1,2 | envelope e: levels `+0,+4,+8,+12 & 127`, rates `+2,+6,+10,+14 & 127`, flags `+16`, sensitivities `+18` | e0 pitch, e1 filter, e2 amplitude. flags: bits 0..3 signed key-rate scale for rates 1,2; bits 4..5 velocity curve; bit 6 one-shot (ignores note-off, runs release rate after stage 2); bit 7 loops. `+18`: high nibble velocity->level, low nibble velocity->attack rate |
| `+0x3c`, `+0x3e`, `+0x48` bit 7 | base pitch coarse, fine (s8 x 2), "use played key" (else key 60) | initial `glide_target` |
| `+0x40` (s8), `+0x0c & 127` | pitch envelope depth, envelope zero offset | `pitch_mod` |
| `+0x42` (s8) | LFO -> pitch depth | `pitch_mod` |
| `+0x44` | bits 0..3 pitch-mod source, bits 4..5 allocation mode (0 poly, 1 retrigger+glide from previous, 2..3 legato reuse), bits 6..7 filter mode (0..3), bit 7 also selects k2 table bank | |
| `+0x46` (s8) | pitch-mod (source) depth via `PITCH_MOD_TABLE` | |
| `+0x48 & 127`, `+0x4a`, `+0x4c`, `+0x4e & 15`, `+0x50` | k1: base x 8, key tracking, filter-envelope depth, source selector, source depth | `first` in `parameters()` |
| `+0x52 & 127`, bit 7, `+0x54`, `+0x56` | k2: base x 8, apply source, key tracking, envelope depth | `second` |
| `+0x58`, `+0x5a`, `+0x5c` | key-range weighting: amount (s8), low key, high key | `key_weight()` |
| `+0x5e` | level attenuation index (bit 7: +43) | amplitude |
| `+0x60` | bits 0..3 amplitude source, bits 4..5 output pair choice, bits 6..7 priority class | `+0x62` (s8) = amplitude-source depth |
| `+0x64`, `+0x66`, `+0x68`, `+0x6a` | LFO: source/random period (`+0x64`), depth target & phase-reset bit 7 (`+0x66`), rate index into `PHASE_RATE_TABLE` (`+0x68`), wave select (low nibble) and depth fade-in rate (high nibble) (`+0x6a`) | |
| `+0x6c` | sample id `& 127`, bit 7 reverse; ids `>=0x78`, `0x6f..0x77`, `0x54..0x6e` select special split / boundary-loop modes | split lookup |
| `+0x6e` (high nibble s8 gate, low nibble source), `+0x70`, `+0x72` | velocity gate threshold; start-offset modulation | `begin()` |
| `+0x74` | layer start delay in 1 ms ticks (`0xfb` = start on note release) | `note_on()` |

* Modulation source selectors (`source()`): 0 LFO, 1/2/3 envelopes 0/1/2, 4 random, 5 previous random, 6 velocity,
  7 key-60, 8 channel word `0x1a`, 9 `0x12`, 10 `0x0a` (bend), 11 `0x10`, 12 `0x0c`, 13 pressure, 14 constant
  max, 15 zero.
* **Split regions** per layer (the 14-byte records walked by `split_for()`: tuning s16 `+0`, packed accumulator
  (initial phase; low byte = cutoff key) `+2`, start `+6` (low byte = loop-mode byte index), end `+10`): each record
  with its resolved key range (previous cutoff + 1 .. cutoff), sample start/end/phase as word indices into the
  sample array (`board + ((value | bank_bit) & 0x1fffffe0) / 512`, the same arithmetic as `begin()` - factor it into
  a shared helper), `loop`, `bidirectional`, `reverse`.

The sample ROM is 16-bit words whose **low byte is zero** in both supported sets (high-byte lane files only;
`games/*/config.toml` `[samples]`), i.e. effectively 8-bit PCM: the overview uses the high byte as i8.
`regions[]` (distinct `(start,end,loop,bidi,reverse)` used by recorded voices or listed in an instrument's
splits) carries `{start, end, loop, reverse, bidirectional, peak_offset}`; the min/max overview is 256
columns x (i8 min, i8 max) = 512 B per region in the binary. The UI may draw higher zoom directly from
`samples.bin` through the word indices.

### 4.11 Mixer / effects data

Constants in `mixer` (exporter table; `effects.hpp` header comment is the source, a unit test asserts every
`Effects::Program` has an entry): output stage `OUTPUT_BOOST = 12`, ES5510 input gain `0.18` per channel,
gain model, DC blocker 20 Hz. Voice pairs (ES5505 pair = bus pair, `Voice.pair`): pair 0 = unconnected (silent),
pair 1 (buses 2/3) = ESP serio0, dry in every program; pair 2 (buses 4/5) = serio1: reverb input for `Reverb/Hall`, 4-tap
stereo chorus for `Chorus`, ping-pong echo for `Echo`; pair 3 (buses 6/7) = serio2: reverb send. DAC L/R = dry +
`table(127 - p8)` * serio1 path + `table(127 - p9)` * serio2 + reverb taps; `Mute` outputs nothing. Programs and parameter
meanings (decoded; names per `effects.hpp`): Reverb p0 decay, p1 damping, p2 LFO rate, p3 LFO depth, p6 hi nibble
input, p7 all-pass gain; Hall as Reverb with cross-coupled chains; Chorus p0 decay, p1 feedback, p2 LFO rates, p6 mix;
Echo p1 feedback, p2 delay (x112 frames), p6 mix/enhancer. `effects[]` timeline rows: `{frame, kind
"preset"|"param", preset?, index?, value?, program, p[10], info}`; `gain[]`: `{frame, channel, raw, linear}`.

### 4.12 Trace file format

Chosen over all-JSON (notes/curves would be 10-30 MB and slow to parse) and over all-binary (header/instruments are
nested, small and change often, JSON keeps them inspectable): **JSON for structure + one binary of columnar
little-endian arrays**. The browser needs `fetch`, `JSON.parse` and typed-array views only.

`songs/<id>.bin`: `u8[4] "F3TR"`, `u32 version = 1`, `u32 total_bytes`, `u32 0`, then blocks (every block
8-byte aligned, zero padded) addressed only from the JSON.

`songs/<id>.json` (`format: "f3-webplayer-trace"`, `version: 1`):

```ts
interface Column { offset: number; dtype: "u8"|"i8"|"u16"|"i16"|"u32"|"i32"|"f32"; length: number }  // new TypedArray(buf, offset, length)
interface Table  { rows: number; columns: Record<string, Column> }
interface Trace {
  format: "f3-webplayer-trace"; version: 1; set: string;
  song: { id: string; code: number; bank: number|null; page: number|null; label: string };
  time: { sample_rate: 29762; main_clock: 16000000; timer_ticks: 16000; pulses_per_quarter: 96 };
  span: { frames: number; audio_frames: number; ticks: number; end_reason: "loop_wrap"|"song_end"|"cap"; loops: number };
  loop: null | { start_frame: number; end_frame: number; start_tick: number; end_tick: number;
                 length_ticks: number; resume_phase: number; iterations: number };
  synth: { bend_range: number };
  sequences: { id: number; bars: number; numerator: number; exponent: number; ticks_per_bar: number; rate: number;
               flags: number; linked: number; header_loop: { start: number; end: number; used_by_hle: false };
               tracks: { track: number; events: number; initial_program: number }[] }[];
  sections: Table;     // owner u8, sequence u8, phase u8, repeat u16, start_frame u32, end_frame u32, end_how u8, rate u8, bars u16, transpose i8
  bars: Table;         // frame u32, section u16, bar u32          (ruler)
  rate_changes: Table; // frame u32, sequence u8, rate u8
  channels: { id: number; sequence: number; track: number }[];
  notes: Table;        // on_frame u32, off_frame u32 (0xffffffff = none), sequence u8, track u8, key u8, kkey u8, vel u8,
                       // program u16, section u16, voice_first u32, voice_count u8, channel u16
  voices: Table;       // note u32, layer u8, slot u8, pair u8, flags u8 (1 loop,2 reverse,4 bidi,8 direct,16 legato,32 retrigger,64 handoff),
                       // start_frame u32, release_frame u32 (0xffffffff), stop_frame u32 (0xffffffff), skey u8, filter_mode u8, region u16,
                       // pitch0 i16, glide0 i16, lfo0 i16, env_pitch0 i16, env_filter0 i16, env_amp0 i16, left0 u16, right0 u16, k1_0 u16, k2_0 u16,
                       // and per lane L in lanes: L_first u32, L_count u16
  lanes: Record<string, Table>;           // per lane: dt u16, value i16|u16 (rows = points)
  chan_events: Table;  // frame u32, channel u16, lane u8, value i32;  chan_lanes: string[]
  // Lane interpolation semantics:
  // - bend: step
  // - seq_volume: linear
  // - pan: linear
  // - expression: linear
  // - transpose: step
  // - mod_depth: step
  levels: { window_frames: 512; rows: number; buses: Column /*u8 x 8 per row*/; out: Column /*u8 x 2, DAC order*/ ;
            encoding: "u8 = clamp(round(255 + 2.5 * dB), 0, 255), peak of the window" };
  effects: { frame: number; kind: "preset"|"param"; preset?: number; index?: number; value?: number;
             program: "None"|"Reverb"|"Hall"|"Chorus"|"Echo"|"Mute"; p: number[]; info: object }[];
  gain: { frame: number; channel: number; raw: number; linear: number }[];
  mixer: { output_boost: number; input_gain: number; gain_model: string; pairs: object[]; programs: Record<string, object> };
  instruments: {
    program: number;
    kind: "layered" | "direct" | "absent";
    glide_rate: number;
    used_by_notes: number;
    layers: {
      layer: number;
      raw: string; // hex string of 0x76 bytes
      envelopes: {
        levels: number[]; // int16 x 4
        rates: number[];  // uint8 x 4
        flags: number;
        key_rate: number;
        velocity_curve: number;
        one_shot: boolean;
        loop: boolean;
        vel_level: number;
        vel_attack: number;
      }[]; // length 3
      pitch: {
        coarse: number;
        fine: number;
        use_key: boolean;
        env_depth: number;
        env_zero: boolean;
        lfo_depth: number;
        mod_source: number;
        mod_depth: number;
        alloc_mode: number;
      };
      filter: {
        mode: number;
        k1: { base: number; key_track: number; env_depth: number; source: number; source_depth: number };
        k2: { base: number; apply_source: boolean; key_track: number; env_depth: number };
      };
      lfo: {
        source_period: boolean;
        depth_target: boolean;
        phase_reset: boolean;
        rate_index: number;
        wave: number;
        fade: number;
      };
      amp: {
        attenuation: number;
        source: number;
        source_depth: number;
        pair_choice: number;
        priority: number;
      };
      key_weight: { amount: number; lo: number; hi: number };
      velocity_gate: { threshold: number; source: number };
      start_mod: { source: number; depth: number };
      start_delay: number;
      splits: {
        key_lo: number;
        key_hi: number;
        start: number;
        end: number;
        phase: number;
        tuning: number;
        loop: boolean;
        bidirectional: boolean;
        reverse: boolean;
      }[];
    }[];
    keys?: {
      index: number;
      key_lo: number;
      key_hi: number;
      sample: number;
      reverse: boolean;
      transpose: number;
      fine: number;
      decay: number;
      release: number;
      level: number;
      filter: number;
      pan_pair: number;
      raw: string; // hex string of 20 bytes
    }[]; // present only when kind == "direct"
  }[];
  regions: Table;      // start u32, end u32, flags u8 (1 loop, 2 reverse, 4 bidi), peak_offset u32 (into peaks)
  peaks: Column;       // i8 pairs, 256 columns per region
}
```

Voice lane semantics: for lane `L` of voice `v`: if `L_count[v] == 0` the lane is constant at the voice's `L0` column;
otherwise points `j = L_first[v] ..` with `frame_j = start_frame + sum(dt[first..j])`. `notes` and `voices` are sorted by
`on_frame` / `(note, layer)` and `voice_first/voice_count` index contiguous voice rows (a note with no voice,
e.g. rejected by key weight or velocity gate or an unknown program, has `voice_count = 0`: the UI draws it hollow).

Writers must produce deterministic bytes (stable sort keys, no timestamps in the trace) so exports can be diffed.

## 5. Bundle layout

```
<out>/                    default build/webplayer  (git-ignored: build/)
  index.html  app.js  worklet.js  f3hle.wasm  games.json
  <set>/manifest.json
  <set>/sound.bin
  <set>/samples.bin
  <set>/songs/<id>.replay
  <set>/songs/<id>.json
  <set>/songs/<id>.bin
```

`index.html`, `app.js`, `worklet.js` are copied from `tools/webplayer/site/`; `f3hle.wasm` from the wasm build
directory. Everything else is ROM-derived and must never be committed: the exporter refuses an `--out` that
`git check-ignore -q` does not report as ignored unless `--allow-tracked-out` is given, and writes only below it.
`*.bin`, `*.wav` and `build/` are ignored by the repository `.gitignore`; `.json`/`.replay` rely on the directory.

Song id: `b<BB>-<code hex, 3 digits>` (`BB` = sound-test bank, `00` if none), lowercase, `[a-z0-9-]+`.

### 5.1 `games.json`

```json
{ "format": "f3-webplayer-games", "version": 1,
  "games": [ { "set": "kirameki", "title": "Kirameki Star Road (Ver 2.10J 1997/08/29)",
               "manifest": "kirameki/manifest.json", "songs": 123 } ] }
```

### 5.2 `<set>/manifest.json`

```json
{ "format": "f3-webplayer-manifest", "version": 1,
  "set": "kirameki", "title": "…",
  "generator": { "tool": "f3rt-tool webplayer-export", "git": "<short hash>", "abi": 1 },
  "time": { "sample_rate": 29762, "main_clock": 16000000, "timer_ticks": 16000 },
  "rom": {
    "sound":   { "file": "sound.bin", "bytes": 3145728, "banked": true, "crc32": "…",
                 "chips": [ { "file": "e44-20.51", "crc32": "4df7e051" }, … ] },
    "samples": { "file": "samples.bin", "bytes": 16777216, "words": 8388608, "encoding": "u16le",
                 "crc32_region": "…", "chips": [ … ] } },
  "boot": { "gain_raw": [48, 48], "packets": [ "032005", … ] },
  "songs": [ { "id": "b01-187", "code": 391, "bank": 1, "page": 1, "label": "bank 01 / code 0x187",
               "calls": [ { "code": 1273, "bytes": "032005" }, { "code": 1274, "bytes": "…" }, { "code": 391, "bytes": "03812c04862c7f" } ],
               "replay": "songs/b01-187.replay", "trace": "songs/b01-187.json", "trace_bin": "songs/b01-187.bin",
               "frames": 0, "audio_frames": 0, "loop": true, "end_reason": "loop_wrap" } ] }
```

* `rom.sound`: **exactly the byte span `Audio::load_sound_rom` receives = `RomSet::sound`** (Kirameki
  `0x300000` bytes: program image + banked sequences; Land Maker `0x80000`), the `std::span<const uint8_t>`
  the `Core` constructor takes. `Core` requires `>= 0x80000`; sizes above `0x80000` select the banked address
  rules (`0xc20000..0xc3ffff` -> `page * 0x20000`, `>= 0xc40000` -> `0x40000 + …`). `crc32` is `crc32(sound.bin)`
  (= `rom_region_crcs()[5]`).
* `rom.samples`: **the `std::span<const uint16_t>` the `Core` constructor takes**: 16 bits per word, **little-endian
  in the file**, word `i` = `(byte[2i] << 8) | byte[2i+1]` of `RomSet::samples` (big-endian ROM image). The file is
  the in-memory image of that array on x86 and WASM (zero-copy into WASM memory); the word count must be a power
  of two (`Synth` requirement, `16777216 / 2 = 8388608` for both sets). `crc32_region` is `rom_region_crcs()[6]` of
  the big-endian `RomSet::samples` bytes, *not* of `samples.bin`.
* `boot.packets`: the ring bytes `0 .. producer` of the post-boot machine split into length-prefixed packets
  (`capture.hpp: boot_packet_stream`; it throws unless the packet count equals the engine's `boot_commands`);
  `gain_raw` = `shared[0x7fa], shared[0x7fb]` at the freeze.
* `songs[].calls`: the exact ring bytes each request produced (`SoundTestCapture.calls`: prelude codes, then the
  code); `bank` = sound-test BANK `N` or `null`; `page` = `m->audio->sound_bank()` after `apply_sound_bank` or
  `null`. Setup consumed no emulated time: the exporter asserts `audio->clock_ticks()` equals the load-time clock after
  the requests, else the song is skipped with an error.

## 6. WASM C ABI and AudioWorklet protocol

### 6.1 Build constraints

Emscripten, C++20, `-O3`, **`-fwasm-exceptions`** (the HLE throws `std::runtime_error`/`invalid_argument`),
`--no-entry -sSTANDALONE_WASM=1`, no threads, no `SharedArrayBuffer`, no pthreads, no emscripten JS glue.
`-sALLOW_MEMORY_GROWTH=1`, `-sSTACK_SIZE=1048576` (the render path keeps about 22 KB of arrays on the stack
(`render_until` 10 KB, `Effects::mix` 11 KB) plus FAUST locals; the 64 KB default is too tight [INFERENCE]), `-sWASM_BIGINT` **not**
required: the ABI uses only i32 and f64. Only the exported symbols below; the module's imports are whatever the
toolchain emits for libc (typically `wasi_snapshot_preview1.*`): the worker enumerates them with
`WebAssembly.Module.imports()` and `worklet.js` supplies stubs (`proc_exit` throws, `fd_write` returns
`ENOSYS`/discards, `clock_time_get` returns 0, `environ_*` return 0 entries, `random_get` fills zeros - none
may be reachable on the render path). Native/WASM `Core` code is identical.

### 6.2 `extern "C"` functions (`runtime/audio/hle/wasm_api.cpp`, `F3HLE_ABI_VERSION = 1`)

```c
uint32_t     f3hle_abi_version(void);
const char  *f3hle_last_error(void);                 // "" when none; valid until the next failing call
void        *f3hle_alloc(uint32_t bytes);            // malloc, 16-byte aligned; 0 on OOM (sets error)
void         f3hle_free(void *p);

typedef struct f3hle_engine f3hle_engine;
// ROM views are borrowed (copied by JS into wasm memory first); they must outlive the engine.
f3hle_engine *f3hle_create(const uint8_t *sound, uint32_t sound_bytes,
                           const uint16_t *sample_words, uint32_t sample_word_count);   // 0 + error on failure
void         f3hle_destroy(f3hle_engine *);

int          f3hle_start(f3hle_engine *, const uint8_t *replay, uint32_t replay_bytes); // new Core + replay(); 0 ok, -1 error; position = 0
int          f3hle_rewind(f3hle_engine *);                      // restart the current replay from frame 0 (new Core)
uint32_t     f3hle_render(f3hle_engine *, float *stereo, uint32_t frames);   // interleaved, DAC order (L/R exchanged); returns frames or 0xffffffff on error
uint32_t     f3hle_skip(f3hle_engine *, uint32_t frames);       // same state transition as render, output discarded (seek); 0xffffffff on error
double       f3hle_position_frames(const f3hle_engine *);       // frames delivered by render/skip since start/rewind
double       f3hle_position_ticks(const f3hle_engine *);        // Core::tick(): ticks fully simulated
```

Semantics. The engine owns a `Core`, a FIFO (>= 4096 stereo frames) used as the `PcmSink`, the replay copy and
the delivered-frame counter. `render`/`skip` loop `while (fifo.size() < n) core.advance(core.next_boundary())`
and then pop `n` frames, so a render of `n` frames after any sequence of renders/skips is bit-identical to one render
of the total (the FIFO preserves the surplus of a 1 ms block). `skip` pops without copying. Every function body is
`try { … } catch (const std::exception &e) { set_error(e.what()); … } catch (...) { set_error("unknown C++ exception"); … }`;
after an error the engine is *faulted*: `render/skip/rewind` fail until a successful `f3hle_start`. Nothing aborts and
no exception crosses the ABI. `f3hle_start` constructs a **fresh** `Core` (a reused one would keep a different
tick->frame phase and DSP history) [construction cost not measured; INFERENCE: tens of ms]. The default gain model is
`SingleStage` (as the native engine); the ABI does not expose it.

### 6.3 Worklet and main-thread protocol

`AudioContext` is created with `{ sampleRate: 29762 }`, so the worklet renders at the context rate and no resampler runs.
The page checks `ctx.sampleRate === 29762` and shows an error otherwise (open question Q2). `worklet.js` registers
`registerProcessor("f3-player", …)`; the node has 0 inputs and 1 stereo output. `process()` renders each 128-frame
quantum directly (`f3hle_render(engine, ptr, 128)` into a preallocated wasm buffer, copy to the two output
channels) while `state == "playing"`, outputs silence otherwise.

Main -> worklet (`port.postMessage`, `T` = transferred):

| Message | Fields | Effect |
|---|---|---|
| `init` | `module: WebAssembly.Module` (or `wasm: ArrayBuffer(T)` fallback), `sound: ArrayBuffer(T)`, `samples: ArrayBuffer(T)` | instantiate manually (`new WebAssembly.Instance(module, imports)`), copy ROMs into `f3hle_alloc` memory, `f3hle_create`, drop the JS copies -> `ready` / `error` |
| `load` | `song: string`, `replay: ArrayBuffer(T)` | state `paused`, `f3hle_start` -> `loaded` / `error` |
| `play` / `pause` | - | state change |
| `stop` | - | `pause` + `f3hle_rewind` |
| `seek` | `frame: number`, `token: number` | state `seeking`; if `frame < position`, `f3hle_rewind`; then each `process()` call skips up to `SEEK_SLICE` (8192) frames until `position == frame`; newer `seek` replaces the target -> `seeked` |

Worklet -> main: `ready {abi, memory_bytes}`; `loaded {song}`; `position {frame, state, ticks}` about every 1488 frames
(50 ms) while playing/seeking; `seeking {frame, target, token}` about every 20 `process()` calls; `seeked {frame, token}`;
`error {stage: "init"|"load"|"render"|"seek", message}` (from `f3hle_last_error`); the node stays silent after `error`
until the next `load`. Seek speed is bounded by `f3hle_skip` throughput (about the render speed, section 7); seeking
backwards re-simulates from zero. The UI treats `position.frame` as the playhead and applies the mapping of 4.4.

## 7. Sizes, performance, risks

* **Bundle sizes**: `sound.bin` 3 MiB (Kirameki) / 0.5 MiB (Land Maker); `samples.bin` 16 MiB per set (half of it
  zero low bytes; compresses roughly 2x [INFERENCE]); per-song trace 1-3 MB, replay < 200 B; WASM module a few
  hundred KB [INFERENCE]; wasm linear memory about 16 MiB samples + 3 MiB sound + about 1.5 MB `Effects`
  + synth tables (< 25 MiB total). Serve with gzip/brotli; `.wasm` needs `application/wasm`; no COOP/COEP.
* **Native speed [measured]**: single thread, `-O3`, x86-64: 25 s of Kirameki 0x187 (dense) in 0.22 s = **113x real
  time** (synth 0.185 s, effects 0.027 s). WASM is expected within 2-4x of that [INFERENCE] (no SIMD used); a
  worst-case seek of 3 min costs roughly 3-8 s. If seek latency matters, the next step is state snapshots, not
  in this version.
* **Denormals**: WASM has no FTZ/DAZ. Native FTZ on/off was bit-identical over 25 s [measured]; `Effects` bypasses
  silent programs (`K_THRESHOLD`) and the voice filter state dies with its voice, so tails are not expected to go
  subnormal, but the long-run (minutes, paused-then-resumed reverb) case is **unmeasured** (acceptance A5).
  Subnormal arithmetic is slow on some hosts; it does not change audible output.
* **libm differences**: `std::pow`/`std::exp`/`std::fmod`/`sin`/`cos`/`tan` appear in `Filter::targets` (3 `pow` per
  parameter update per voice), `Core::set_gain` (`powf`), `Effects` (`exp`, `fmod`) and the generated DSPs. Emscripten uses musl;
  native uses glibc, whose x86-64 `pow/exp/sin/cos` select FMA variants at run time by CPU [INFERENCE from glibc multiarch,
  not measured here], so last-bit results of coefficients can differ between native hosts as well. Required: native `AudioEngine`
  vs native `Core` **bit-exact** (same binary, same libm). Native vs WASM target (to be measured by the wasm worker and pinned
  here): int16 output after quantization identical for >= 99.9 % of samples, max |diff| <= 2 LSB16 and RMS error <= -90 dBFS over a
  full song. Not asserted before measurement.
* **FMA/contraction**: baseline x86-64 builds do not use FMA; the WASM build must not enable relaxed-SIMD FMA, and
  `-ffast-math` is forbidden for both.
* **Worklet specifics**: no `performance.now()` in `AudioWorkletGlobalScope`, so slicing is by frames, not time;
  `WebAssembly.Module` structured clone to a worklet is supported by current Chrome/Firefox/Safari [INFERENCE;
  `init.wasm` bytes fallback exists]; the browser may not honour `sampleRate: 29762` (Q2).
* **Chunk invariance** (2.4): bit-exact on the 1 ms grid (property-tested on 8 songs), float-noise-level (1 ulp, no int16
  difference seen) for sub-millisecond steps because `Effects::mix` keeps per-call state.

## 8. Acceptance checks

* **A0 baseline**: `/tmp/f3rt-base/{f3rt-tool,f3rt-test-audio}` are copies of the `build-kirameki` binaries
  (built 13:53 from the working tree that became `5cb929d`; the only files newer than them under `runtime/ tools/ cmake/ faust`
  are `cmake/faust.cmake` and `runtime/LICENSES.txt`, neither compiled; `ninja -n` can only report the always-dirty glob
  re-check step, so "no work" was established by timestamps).
  WAV baselines (`/tmp/wp-probe`, Kirameki, produced with `sound-extract --rom-dir roms/kirameki --set kirameki` at 29762 Hz):
  `out/391.wav` (`--sound 0x187 --seconds 200 --tail 3`) md5 `ee2503561049c4a30fa2ce1826e7e504`;
  `c0x100.wav` `bfd0d909…`, `c0x150.wav` `782618e0…`, `c0x188.wav` `985fd2e1…`, `c0x190.wav` `8c23f96d…`, `c0x1a0.wav`
  `13ef44ef…` (`--seconds 30 --tail 2`, codes 0x100,0x150,0x188,0x190,0x1a0). Land Maker baselines were made with the
  same baseline `f3rt-tool` (`--rom-dir roms/landmakrj --set landmakrj --seconds 30 --tail 2`): codes 0x10, 0x20, 0x30,
  0x40, 0x50 (music, 19 s) and 1, 2, 3 (direct-note effects); md5 `62b25a7a…`, `a2592d25…`, `35e7a149…`, `eaea24a4…`,
  `6323510b…`, `99f6c39a…`, `278a0a44…`, `7cd14d75…`. The baseline WAV files are kept in `/tmp/f3rt-base/{k-wavs,lm-wavs}`.
* **A1 native bit-identity** (**passed at M1 and M2**): the same 14 `sound-extract` invocations with the `Core`-based `build-kirameki`
  `f3rt-tool` produce byte-identical WAVs (6 Kirameki + 8 Land Maker; `cmp` against the baseline files), and
  `f3rt-test-audio` passes. Land Maker runs on the kirameki-built tool (every set is in the ROM manifest).
* **A2 replay equivalence** (`runtime/tests/hle_core.cpp`, passed at M1): `HleCore.EngineAndCoreReplayAreBitExact` renders 6 s
  through `AudioEngine` (snapshot restore, reset pulse, gain bytes, BANK, request bytes through the mailbox ring, 10 ms
  advance + render slices) and through `Core` + `hle::replay()` of the `encode_replay` script, and compares the float PCM
  bit-exactly: Kirameki 0x187 with `BANK 1` (its prelude overrides the boot-time effects), Land Maker 0x50 with its real
  boot packet stream and gain bytes from a 900-frame interpreted boot of the test (about 6.5 s, cached per process).
  `HleCore.EncodeReplayRoundTripMatchesManualCalls` checks `encode_replay` -> `replay` against the same calls made by
  hand; `ReplayCodec.*` (no ROM) cover the byte layout, encoder rejection and 18 malformed-script cases.
* **A3 step-size independence** (`runtime/tests/hle_core.cpp`, passed at M1, 24 RapidCheck cases each, 8 songs: Kirameki
  0x187/0x001/0x100/0x188, Land Maker 0x50..0x53, 4 s each, stable over seeds 11/22/33/44): `HleCore.GridAlignedStepsAreBitExact`
  (schedules of whole milliseconds, 1 ms .. 2 s steps: identical float hash as 1 ms stepping) and
  `HleCore.ArbitraryStepsAgreeWithinFloatNoise` (arbitrary tick steps from 1 tick, quantum >= 64 ticks: same frame count
  and `|difference| <= 1e-6`). The original wording of A3 (bit-exact for any step) is false below a millisecond, see 2.4.
  Total runtime of `ReplayCodec.*:HleCore.*` is about 9 s, of which about 6.5 s is the Land Maker boot.
* **A4 instrumentation neutrality** (`runtime/tests/hle_core.cpp`, **passed at M2**): PCM with `set_trace` and
  `set_voice_observer` installed equals PCM without, bit for bit (`HleCore.InstrumentationNeutralityAndQueries`).
* **A5 native-vs-WASM** (node harness running `f3hle.wasm` through the same ABI as `worklet.js`): numbers of section 7; also a
  10-minute run with a paused-then-resumed tail for subnormal slowdown.
* **A6 loop period**: export one looping song with `--loops 2`; `wrap2 - wrap1 == wrap1 - loop.start` ticks.
* **A7 trace sanity**: every recorded note's voices lie inside the song span; all `frame` columns monotone where required; every
  `voice` row's `note` < notes.rows; lane points reconstruct within tolerance of the raw `Parameters` stream.
* **A8 repository hygiene**: `git status` after an export shows nothing under `tools/` or `docs/` changed and the output
  directory ignored.

## 9. Appendix

### 9.1 Probe used for the measurements

A throwaway program (not in the repository, `/tmp/wp-probe/probe.cpp`) that links `synth.cpp`, `sequencer.cpp`, `rom.cpp`
and replicates `AudioEngine::Impl::{render_until, advance_worker}` single-threaded with in-memory PCM, then replays a
packet list at tick 0 (recipe 3.2 without boot stream). Results used above: chunking hashes identical across four
step modes; FTZ on/off identical; observer on/off identical and equally fast; 113x real time; bit-exact against the
`sound-extract` WAV of Kirameki 0x187; event statistics from `sound-extract --hle-events` (43440 rows: 1055 starts,
1006 releases, 1055 stops, 40324 parameter events; 785 distinct note instances). Lane simplification size was
computed from that CSV with a sliding-window linear fit at the tolerances of 4.8.

### 9.2 Hypotheses and unknowns recorded by this spec

* Pitch units/MIDI numbering, quarter-note-BPM meaning of the header `rate`, and all field names of 4.10 are
  descriptive hypotheses from the HLE code; they are not wiki facts.
* Header `loop_start/loop_end` units are unknown; the HLE ignores them.
* Controller offsets other than 4.9's list, `parameters[0x1c]` double use, and the semantics of filter modes against the
  chip's documentation are unresolved.

## 10. Open questions needing a product decision

1. **Looped playback fidelity (Q1)**: default trace is intro + 1 loop iteration, UI repeats that data. Keep, or default to
   `--loops 2` (steady-state data, +50-100 % trace size)?
2. **Browsers refusing 29762 Hz (Q2)**: Chrome accepts arbitrary `sampleRate` in 3000-768000; Firefox/Safari range or
   rounding [INFERENCE]. If `ctx.sampleRate !== 29762`: show an error (current spec), or add an in-worklet resampler (contradicts the
   "no resampler" constraint)?
3. **`samples.bin` size (Q3)**: 16 MiB/set uncompressed. Acceptable (rely on HTTP compression), or pack the 8-bit data
   (halves it; JS or WASM expands at load, `rom.samples.encoding` becomes `u8hi`)?
4. **Distribution (Q4)**: the bundle contains ROM-derived data; it is for local/private serving only and is never committed.
   Confirm there is no plan to host it publicly.
5. **Song set and labels (Q5)**: the sound test shows numbers, the ROM carries no names the HLE reads. Labels default to
   `bank NN / code 0xXXX`; music-export owns the list. Confirm.
6. **Hard cap (Q6)**: 360 s default for non-looping/odd songs.
