#pragma once
#include "audio/hle/block.hpp"
#include "audio/hle/events.hpp"
#include "audio/hle/note_timing.hpp"
#include "audio/hle/patch.hpp"
#include "audio/hle/program_service.hpp"
#include "audio/hle/sound_bank.hpp"
#include "audio/hle/track_service.hpp"
#include "audio/hle/voice.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#if F3RT_HLE_EVENTS
#include <functional>
#include <vector>
#endif

namespace f3rt::hle {

// The 32-voice pool, the driver clock and the delayed patches. Programs are already resolved to patches; this stage
// schedules them, keeps the voices and their envelopes, and walks the slots on the 12 ms sound CPU budget ring.
//
// The ring is serviced from the render at the first rendered frame whose tick reached the service tick, not at the
// clock tick itself: the 1 kHz grid can name a tick whose frame is still pending in the frame -> tick map, and the
// pre-refactor ring was always stepped on the frame that reached the tick.
class VoiceService {
public:
    explicit VoiceService(SoundBank &bank);
    VoiceService(const VoiceService &) = delete;
    VoiceService &operator=(const VoiceService &) = delete;

#if F3RT_HLE_EVENTS
    void set_observer(std::function<void(const VoiceEvent &)> observer);
#endif
    void reset(uint64_t tick);

    // Starts every patch of the note at its driver start tick (or schedules its delay/release trigger).
    void note_on(const NoteOnEvent &event, const PatchList &patches, Track &track);
    void note_off(uint64_t instance, uint64_t tick);

    // Re-runs the voice parameters of one track's / one sequence's live voices after a track event.
    void refresh_track(uint8_t sequence, uint8_t track, uint64_t tick);
    void refresh_sequence(uint8_t sequence, uint64_t tick);

    // The ring's own 1 kHz service tick (grid-aligned from reset) and the service itself: due delayed patches,
    // then the budget ring. The render calls it when it reaches the tick.
    uint64_t next_tick() const { return m_tick; }
    void service();

    // Driver starts that fall between two service ticks, at the exact tick the render reaches them.
    void service_queue(uint64_t tick);
    uint64_t next_queue_tick() const { return m_queue.next_event(); }

    std::array<Voice, 32> &pool() noexcept { return m_voices; }

    // Deactivates a voice and emits Stop. `end_sample` keeps a retriggered voice's slot for its envelope restart.
    void stop(Voice &voice, uint64_t tick);
    void end_sample(Voice &voice, uint64_t tick);

#if F3RT_HLE_EVENTS
    // Render-time events are buffered and delivered in (frame, voice) order once the render call is over.
    void begin_render();
    void finish_render();
    void set_render_voice(unsigned voice) { m_current_voice = voice; }
    void set_render_frame(size_t frame) { m_current_frame = frame; }
#else
    void begin_render() {}
    void finish_render() {}
    void set_render_voice(unsigned) {}
    void set_render_frame(size_t) {}
#endif

private:
    struct Pending {
        NoteOnEvent note{};
        const Patch *patch = nullptr;
        Track *track = nullptr;
        uint64_t tick = 0;
        bool active = false, release_trigger = false;
    };
#if F3RT_HLE_EVENTS
    struct BufferedEvent {
        size_t frame = 0;
        unsigned voice = 0;
        VoiceEvent event;
    };
    // Key-on LVOL/RVOL/K1/K2. A layered voice steps its envelopes after this snapshot, and that step overwrites
    // these four registers on the voice.
    struct Onset {
        uint16_t left_volume = 0, right_volume = 0, k1 = 0, k2 = 0;
        explicit Onset(const Voice &voice)
            : left_volume(voice.left_volume), right_volume(voice.right_volume),
              k1(voice.filter_cutoff_low), k2(voice.filter_cutoff_high) {}
    };
#endif

    void begin(const NoteOnEvent &note, const Patch &patch, Track &track, uint64_t at);
    void launch(const NoteOnEvent &note, const Patch &patch, Track &track, uint64_t start);
    void release(Voice &voice, uint64_t tick);
    void release_instance(uint64_t instance, uint64_t tick);
#if F3RT_HLE_EVENTS
    void emit(const Voice &voice, VoiceEvent::Kind kind, uint64_t tick,
              const Onset *onset = nullptr);
#endif
    void parameters(Voice &voice, bool initial, [[maybe_unused]] uint64_t tick,
                    [[maybe_unused]] bool notify = true);
    void move_before_cursor(unsigned index);
    unsigned allocate(const NoteOnEvent &note, const Patch &patch, unsigned priority,
                      AllocationMode mode);

    SoundBank &m_bank;
    std::array<Voice, 32> m_voices{};
    std::array<Pending, 96> m_pending{};
    DriverClock m_driver;
    NoteQueue<Pending, 96> m_queue;
    std::array<uint8_t, 32> m_next{}, m_prev{};
    uint8_t m_cursor = 0, m_budget_slot = 0;
    NoiseState m_noise{};
    uint64_t m_serial = 0;
    uint64_t m_tick = TIMER_TICKS;
    uint32_t m_filter_epoch = 0;
#if F3RT_HLE_EVENTS
    std::function<void(const VoiceEvent &)> m_observer;
    std::vector<BufferedEvent> m_buffered;
    bool m_buffering = false;
    size_t m_current_frame = 0;
    unsigned m_current_voice = 0;
#endif
};

} // namespace f3rt::hle
