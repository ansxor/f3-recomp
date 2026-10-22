#include "audio/hle/voice_service.hpp"
#include "audio/hle/sound_rom_5a7e9117.hpp"
#include <algorithm>
#include <stdexcept>

namespace f3rt::hle {

VoiceService::VoiceService(SoundBank &bank) : m_bank(bank) {
    m_buffered.reserve(32);
    reset(0);
}

void VoiceService::set_observer(std::function<void(const VoiceEvent &)> observer) {
    m_observer = std::move(observer);
}

void VoiceService::reset(uint64_t tick) {
    for (uint8_t i = 0; i < 32; ++i) {
        stop(m_voices[i], tick);
        m_voices[i] = Voice{};
        m_voices[i].slot = i;
    }
    for (Pending &p : m_pending) p.active = false;
    m_queue.clear();
    m_driver.reset(tick);
    for (unsigned i = 0; i < 32; ++i) {
        m_next[i] = uint8_t((i + 1) % 32);
        m_prev[i] = uint8_t((i + 31) % 32);
    }
    m_cursor = 0;
    m_budget_slot = 0;
    m_tick = (tick / TIMER_TICKS + 1) * TIMER_TICKS;
    m_noise.value = m_bank.rom.read_u16(sound_5a7e9117::NOISE_SEED_PTR);
    m_noise.history = m_bank.rom.read_u16(sound_5a7e9117::NOISE_HISTORY_PTR);
    m_serial = 0;
    m_buffered.clear();
    m_buffering = false;
}

void VoiceService::emit(const Voice &v, VoiceEvent::Kind kind, uint64_t tick, const Onset *onset) {
    if (!m_observer) return;
    VoiceEvent e{};
    e.kind = kind;
    e.instance = v.owner.instance;
    e.tick = tick;
    e.sequence = v.owner.sequence;
    e.track = uint8_t(v.owner.track + 1);
    e.key = v.owner.key;
    e.layer = v.patch->index();
    e.output_pair = v.pair;
    e.sample_start = uint32_t(v.start);
    e.sample_end = uint32_t(v.end);
    e.frequency = v.frequency;
    e.left_volume = v.left_volume;
    e.right_volume = v.right_volume;
    e.k1 = v.filter_cutoff_low;
    e.k2 = v.filter_cutoff_high;
    if (onset) {
        e.left_volume = onset->left_volume;
        e.right_volume = onset->right_volume;
        e.k1 = onset->k1;
        e.k2 = onset->k2;
    }
    e.loop = v.loop;
    e.reverse = v.reverse;
    e.slot = v.slot;
    e.sounding_key = v.effective_key;
    e.velocity = v.owner.velocity;
    e.filter_mode = v.filter.mode;
    e.direct = !v.patch->layered();
    e.program = v.owner.program;
    e.pitch = v.pitch;
    e.glide = v.glide_target;
    e.lfo = v.lfo;
    for (unsigned i = 0; i < 3; ++i) e.env[i] = v.envelope[i].level();
    if (m_buffering) m_buffered.push_back({m_current_frame, m_current_voice, e});
    else m_observer(e);
}

void VoiceService::begin_render() {
    if (!m_observer) return;
    m_buffering = true;
    m_buffered.clear();
}

void VoiceService::finish_render() {
    if (!m_buffering) return;
    m_buffering = false;
    std::stable_sort(m_buffered.begin(), m_buffered.end(),
                     [](const BufferedEvent &a, const BufferedEvent &b) {
                         return a.frame != b.frame ? a.frame < b.frame : a.voice < b.voice;
                     });
    for (const BufferedEvent &event : m_buffered) m_observer(event.event);
    m_buffered.clear();
}

void VoiceService::parameters(Voice &v, bool initial, uint64_t tick, bool notify) {
    v.parameters(initial);
    if (!initial && notify) emit(v, VoiceEvent::Kind::Parameters, tick);
}

void VoiceService::move_before_cursor(unsigned i) {
    unsigned a = m_prev[i], b = m_next[i];
    m_next[a] = uint8_t(b);
    m_prev[b] = uint8_t(a);
    if (m_cursor == i) m_cursor = uint8_t(b);
    a = m_prev[m_cursor];
    m_next[a] = uint8_t(i);
    m_prev[i] = uint8_t(a);
    m_next[i] = m_cursor;
    m_prev[m_cursor] = uint8_t(i);
}

unsigned VoiceService::allocate(const NoteOnEvent &n, const Patch &patch, unsigned priority,
                                AllocationMode mode) {
    if (mode == AllocationMode::Legato || mode == AllocationMode::LegatoHold) {
        Voice *found = nullptr;
        for (Voice &v : m_voices)
            if (v.active && !v.released && v.owner.sequence == n.sequence &&
                v.owner.track == n.track && patch.reuses(v))
                if (!found || v.serial < found->serial) found = &v;
        if (found) return unsigned(found - m_voices.data());
    }
    if (mode == AllocationMode::Retrigger) {
        for (Voice &v : m_voices)
            if (v.active && !v.released && v.owner.sequence == n.sequence &&
                v.owner.track == n.track && patch.reuses(v)) {
                release(v, n.tick);
                break;
            }
    }
    for (unsigned i = 32; i--;)
        if (!m_voices[i].active) return i;
    for (unsigned klass : std::array<unsigned, 3>{0, 64, 128}) {
        if (klass == 64 && priority == 0) break;
        if (klass == 128 && priority <= 64) break;
        for (bool released : std::array<bool, 2>{true, false}) {
            Voice *old = nullptr;
            for (Voice &v : m_voices)
                if (v.released == released && (klass == 128 || v.priority == klass)) {
                    if (!old) old = &v;
                    else {
                        const uint64_t v_k = released ? v.release_serial : v.serial;
                        const uint64_t o_k = released ? old->release_serial : old->serial;
                        if (v_k < o_k) old = &v;
                    }
                }
            if (old) {
                unsigned index = unsigned(old - m_voices.data());
                stop(*old, n.tick);
                return index;
            }
        }
    }
    return 32;
}

void VoiceService::begin(const NoteOnEvent &n, const Patch &patch, Track &track, uint64_t at) {
    const int kernel = track.config.parameters.transposed_key(n.kernel_key);
    const int weight = patch.weight(n, kernel);
    if (weight < 0) return;
    const AllocationMode mode = patch.allocation();
    const unsigned priority = patch.priority();
    int prior_pitch = 0;
    bool prior_found = false;
    if (mode == AllocationMode::Retrigger)
        for (const Voice &old : m_voices)
            if (old.active && old.owner.sequence == n.sequence && old.owner.track == n.track &&
                old.patch->index() == patch.index()) {
                prior_pitch = old.glide_target;
                prior_found = true;
                break;
            }
    const unsigned index = allocate(n, patch, priority, mode);
    if (index == 32) return;
    Voice &v = m_voices[index];
    const bool legato =
        v.active && (mode == AllocationMode::Legato || mode == AllocationMode::LegatoHold);
    const double old_board = double((v.selector >> 1) << 20);
    const PriorWindow prior{v.start - old_board, v.end - old_board, v.hardware_valid};
    if (legato) {
        const uint8_t velocity = v.owner.velocity;
        v.owner = n;
        v.owner.velocity = velocity;
        v.released = false;
        v.retarget(kernel, patch.program());
        parameters(v, false, at);
        return;
    }
    stop(v, at);
    v = Voice{};
    v.slot = uint8_t(index);
    v.filter_epoch = ++m_filter_epoch;
    v.owner = n;
    v.patch = &patch;
    v.track = &track;
    v.priority = uint8_t(priority);
    v.allocation = uint8_t(mode);
    v.effective_key = uint8_t(std::clamp(kernel, 21, 108));
    v.weight = uint8_t(weight);
    v.serial = ++m_serial;
    const NoteStart start{
        n,           kernel,         track.config.parameters.signed_transpose(), mode, prior_pitch,
        prior_found, patch.program()};
    v.initialize(start, m_noise, prior);
    v.active = v.hardware_valid = true;
    // Ring admission, then the opening step: a layered voice steps once here, a direct voice waits for the ring.
    move_before_cursor(v.slot);
    parameters(v, true, at, false);
    const Onset onset{v};
    const OnsetStep step = v.onset(m_noise);
    if (step.restarted) emit(v, VoiceEvent::Kind::Start, at);
    if (step.ended) {
        v.active = false;
        return;
    }
    emit(v, VoiceEvent::Kind::Start, at, &onset);
    if (step.parameters) emit(v, VoiceEvent::Kind::Parameters, at);
}

void VoiceService::launch(const NoteOnEvent &n, const Patch &patch, Track &track, uint64_t start) {
    if (start == n.tick && m_queue.empty()) {
        begin(n, patch, track, start);
        return;
    }
    Pending q{n, &patch, &track, start, true, false};
    if (!m_queue.push(q, n.instance, n.tick, start))
        throw std::runtime_error("HLE driver key-on queue exhausted");
}

void VoiceService::release(Voice &v, uint64_t tick) {
    if (v.released) return;
    v.released = true;
    v.release_serial = ++m_serial;
    emit(v, VoiceEvent::Kind::Release, tick);
    if (v.release()) stop(v, tick);
}

void VoiceService::stop(Voice &voice, uint64_t tick) {
    if (voice.active && !voice.sample_finished) emit(voice, VoiceEvent::Kind::Stop, tick);
    voice.active = false;
}

void VoiceService::end_sample(Voice &voice, uint64_t tick) {
    if (voice.allocation) {
        emit(voice, VoiceEvent::Kind::Stop, tick);
        voice.sample_finished = true;
    } else stop(voice, tick);
}

void VoiceService::release_instance(uint64_t instance, uint64_t tick) {
    for (Voice &v : m_voices)
        if (v.active && v.owner.instance == instance) release(v, tick);
}

void VoiceService::note_on(const NoteOnEvent &event, const PatchList &patches, Track &track) {
    NoteOnEvent note = event;
    note.velocity &= 127; // c16faa clears the velocity flag bit.
    unsigned immediate = 0;
    for (const Patch *patch : patches)
        if (!patch->delay_ticks()) ++immediate;
    const uint64_t start = immediate ? m_driver.start(note.tick, immediate) : note.tick;
    for (const Patch *patch : patches) {
        if (!patch->delay_ticks()) {
            launch(note, *patch, track, start);
            continue;
        }
        auto slot = std::find_if(m_pending.begin(), m_pending.end(),
                                 [](const Pending &p) { return !p.active; });
        if (slot == m_pending.end()) throw std::runtime_error("HLE delayed-note pool exhausted");
        *slot = {note,   patch,
                 &track, note.tick + uint64_t(patch->delay_ticks()) * TIMER_TICKS,
                 true,   patch->starts_on_release()};
    }
}

void VoiceService::note_off(uint64_t instance, uint64_t tick) {
    m_queue.note_off(instance, tick);
    for (Pending &p : m_pending)
        if (p.active && p.note.instance == instance) {
            if (p.release_trigger) {
                NoteOnEvent n = p.note;
                n.tick = tick;
                begin(n, *p.patch, *p.track, tick);
            }
            p.active = false;
        }
    release_instance(instance, tick);
}

void VoiceService::service_queue(uint64_t tick) {
    m_queue.service(
        tick,
        [&](const Pending &p, uint64_t start) {
            NoteOnEvent note = p.note;
            note.tick = start;
            begin(note, *p.patch, *p.track, start);
        },
        [&](uint64_t instance, uint64_t at) { release_instance(instance, at); });
}

void VoiceService::refresh_track(uint8_t sequence, uint8_t track, uint64_t tick) {
    for (Voice &v : m_voices)
        if (v.active && v.owner.sequence == sequence && v.owner.track == track)
            parameters(v, false, tick);
}

void VoiceService::refresh_sequence(uint8_t sequence, uint64_t tick) {
    // Track-major, as the old per-track channel sync was: all voices of track 0, then track 1, ...
    for (uint8_t track = 0; track < TRACK_COUNT; ++track)
        for (Voice &v : m_voices)
            if (v.active && v.owner.sequence == sequence && v.owner.track == track)
                parameters(v, false, tick);
}

void VoiceService::service() {
    const uint64_t tick = m_tick;
    m_tick += TIMER_TICKS;
    // Delayed layered patches whose delay elapsed; the start still pays the driver's key-on cost.
    for (Pending &p : m_pending)
        if (p.active && !p.release_trigger && p.tick <= tick) {
            NoteOnEvent n = p.note;
            n.tick = p.tick;
            p.active = false;
            launch(n, *p.patch, *p.track, m_driver.start(n.tick, 1));
        }
    unsigned count = m_bank.budgets[m_budget_slot];
    m_budget_slot = uint8_t((m_budget_slot + 1) % 12);
    while (count--) {
        const unsigned index = m_cursor;
        m_cursor = m_next[m_cursor];
        Voice &v = m_voices[index];
        if (!v.active) continue;
        const EnvelopeTick step = v.step(m_noise);
        if (step.restarted) emit(v, VoiceEvent::Kind::Start, tick);
        if (step.ended) {
            stop(v, tick);
            continue;
        }
        advance_glide(v);
        parameters(v, false, tick);
    }
}

} // namespace f3rt::hle
