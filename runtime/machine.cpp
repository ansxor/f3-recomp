#include "f3rt/machine.hpp"
#include "f3rt/audio.hpp"
#include "f3rt/video.hpp"
#include "f3rt/game_video.hpp"
#include "sound_trace.hpp"
#include "sound_native.hpp"
#include "eeprom.hpp"
#include "interpreter.hpp"
#include "state_io.hpp"
#include <algorithm>
#include <stdexcept>
#include <sstream>

namespace f3rt {
namespace {
// Decode once only when every byte is contiguous and has no observable bus
// side effect. All boundary/mirror-wrap cases retain ordered byte accesses.
template<unsigned Width, bool Write>
uint8_t *direct_bytes(Machine &m, uint32_t a) {
    a &= 0xffffff;
    if constexpr (!Write) {
        if (a <= m.roms.main.size() - Width) return m.roms.main.data() + a;
    }
    if (a - 0x400000 < 0x40000) {
        const uint32_t offset = a & 0x1ffff;
        if (offset <= m.ram.size() - Width) return m.ram.data() + offset;
    }
    if (a - 0x440000 <= m.palette.size() - Width)
        return m.palette.data() + (a - 0x440000);
    if (a - 0x600000 <= m.graphics.size() - Width) {
        if constexpr (!Write) return m.graphics.data() + (a - 0x600000);
        else if (!m.game_video) return m.graphics.data() + (a - 0x600000);
    }
    if constexpr (!Write) {
        if (a - 0xc00000 <= m.shared.size() - Width)
            return m.shared.data() + (a - 0xc00000);
    }
    return nullptr;
}
}
Machine::Machine(RomSet set) : pixels_(320 * set.video.visible_height),
    roms(std::move(set)), video(std::make_unique<Video>()),
    audio(std::make_unique<Audio>()), eeprom(std::make_unique<Eeprom>()) {
    if (roms.main.size() < 0x400 || roms.main.size() > 0x200000 || roms.main.size() % 4)
        throw std::runtime_error("Main ROM must hold vectors and fit the 2 MiB F3 window");
    if (!video->load_roms(roms.sprites, roms.sprites_hi, roms.tiles, roms.tiles_hi, roms.video))
        throw std::runtime_error("Invalid video ROM regions");
    audio->set_shared_ram(shared.data(), shared.size());
    audio->load_sound_rom(roms.sound);
    audio->load_sample_rom(roms.samples);
    interpreter = std::make_unique<Interpreter>(*this);
    audio->set_cpu_runner([this](int cycles) { return interpreter->run_audio(cycles); });
    audio->set_reset_callback([this](bool asserted) { interpreter->audio_reset(asserted); });
    audio->set_irq_callback([this](bool asserted) { interpreter->audio_irq(asserted); });
    if (!roms.factory_eeprom.empty()) {
        if (roms.factory_eeprom.size() != eeprom->words.size() * 2)
            throw std::runtime_error("Factory EEPROM ROM must be exactly 128 bytes");
        // Seed once: resets preserve guest settings, and explicit EEPROM loads override these words.
        for (size_t i = 0; i < eeprom->words.size(); ++i)
            eeprom->words[i] = uint16_t(uint16_t(roms.factory_eeprom[2 * i]) << 8 |
                                        roms.factory_eeprom[2 * i + 1]);
    }
    // Analog ports start at counter zero; resets preserve the serialized counters.
    if (roms.name == "arkretrnj" || roms.name == "puchicarj")
        inputs[2] = inputs[3] = 0xffff0000;
    reset();
}
Machine::~Machine() = default;
const std::vector<uint32_t> &Machine::native_pixels() const {
    if (game_video) game_video->materialize_native();
    return pixels_;
}
void Machine::use_native_sound(const f3_block *program, size_t count,
                               std::span<const f3_excluded_range> excluded, uint32_t expected_crc) {
    if (audio->backend() == Audio::Backend::Hle)
        throw std::runtime_error("HLE audio does not execute a native sound driver");
    if (!audio->is_reset() || audio->clock_ticks())
        throw std::runtime_error("Select the native sound driver before machine execution");
    sound_native = std::make_unique<SoundNative>(*this, program, count, excluded, expected_crc);
    audio->set_cpu_runner([this](int cycles) { return sound_native->run(cycles); });
    audio->set_reset_callback([this](bool asserted) { sound_native->reset(asserted); });
    // Native SR/IRQ recognition reads the DUART's current line directly.
    audio->set_irq_callback({});
    sound_native->reset(true);
}
uint32_t Machine::sound_pc() const {
    if (audio->backend() == Audio::Backend::Hle) return 0;
    return sound_native ? sound_native->pc() : interpreter->sound_pc();
}
uint64_t Machine::raster_cycle(uint64_t pixels) const {
    return (pixels * main_clock + pixel_clock - 1) / pixel_clock;
}
void Machine::reset_devices() {
    cpu.dispatch_deadline = 0;
    audio->set_reset(true);
    eeprom->pins(0, cpu.cycles);
    pending_irqs = 0;
    irq3_at = UINT64_MAX;
    watchdog_at = cpu.cycles + 3ull * main_clock;
}
void Machine::reset() {
    native_pixels();
    cpu = {};
    cpu.runtime = this;
    eeprom->reset();
    frame = hardware_cycles = 0;
    // Reference beam time zero is VBSTART, not scanline zero.
    next_vblank = raster_cycle(frame_pixels);
    reset_devices();
    audio->reset_board();
    if (sound_trace) sound_trace->record(*this, SoundTrace::Reset, cpu.pc, 0, 2, 0);
    video->reset();
    if (game_video) game_video->reset();
    interpreter->reset_main();
}
uint32_t Machine::input_word(unsigned index) const {
    if (index >= inputs.size()) return 0xffffffff;
    uint32_t value = inputs[index];
    if (index == 0) {
        const uint32_t io = (system_inputs & 0xfe) | unsigned(eeprom->output(cpu.cycles));
        value = (value & 0xffff) | (io << 16) | (io << 24);
    } else if (index == 1 || index == 5) {
        value = (value & 0xffff) | (uint32_t(coin_word[index == 5]) << 16);
    }
    return value;
}
uint8_t Machine::read8(uint32_t a) {
    a &= 0xffffff;
    if (a < 0x200000) return a < roms.main.size() ? roms.main[a] : 0xff;
    if (a >= 0x400000 && a < 0x440000) return ram[a & 0x1ffff];
    if (a >= 0x440000 && a < 0x448000) return palette[a - 0x440000];
    if (a >= 0x4a0000 && a < 0x4a0020) return uint8_t(input_word((a - 0x4a0000) / 4) >> (24 - 8 * (a & 3)));
    if (a >= 0x600000 && a < 0x640000) return graphics[a - 0x600000];
    if (a >= 0xc00000 && a < 0xc00800) return shared[a - 0xc00000];
    return 0xff; // MAME unmapped bus value, not physical-board mirror speculation.
}
uint16_t Machine::read16(uint32_t a) {
    if (const auto *p = direct_bytes<2, false>(*this, a))
        return uint16_t(uint16_t(p[0]) << 8 | p[1]);
    return uint16_t(uint16_t(read8(a)) << 8 | read8(a + 1));
}
uint32_t Machine::read32(uint32_t a) {
    if (const auto *p = direct_bytes<4, false>(*this, a))
        return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
    return uint32_t(read16(a)) << 16 | read16(a + 2);
}
void Machine::coin_write(unsigned bank, uint8_t value) {
    const uint8_t old = uint8_t(coin_word[bank] >> 8);
    coin_word[bank] = uint16_t(value << 8) | (coin_word[bank] & 0xff);
    for (unsigned i = 0; i < 2; ++i) {
        coin_locked[bank * 2 + i] = !(value & (1u << i));
        if ((value & (4u << i)) && !(old & (4u << i))) ++coin_count[bank * 2 + i];
    }
}
void Machine::write8(uint32_t a, uint8_t v) {
    a &= 0xffffff;
    if (a >= 0x400000 && a < 0x440000) { ram[a & 0x1ffff] = v; return; }
    if (a >= 0x440000 && a < 0x448000) { palette[a - 0x440000] = v; return; }
    if (a >= 0x600000 && a < 0x640000) {
        if (game_video) game_video->observe_write(cpu.pc, a);
        graphics[a - 0x600000] = v;
        return;
    }
    if (a >= 0x660000 && a < 0x660020) {
        if (game_video) game_video->observe_write(cpu.pc, a);
        control[a - 0x660000] = v; return;
    }
    if (a >= 0xc00000 && a < 0xc00800) {
        if (sound_trace) sound_trace->record(*this, SoundTrace::MainWrite, cpu.pc, a, v, 1);
        shared[a - 0xc00000] = v;
        audio->shared_write(a - 0xc00000, frame);
        return;
    }
    if ((a >= 0xc80000 && a <= 0xc80003) || (a >= 0xc80100 && a <= 0xc80103)) {
        if (sound_trace) sound_trace->record(*this, SoundTrace::MainWrite, cpu.pc, a, v, 1);
        audio->set_reset(a >= 0xc80100); return;
    }
    if (a >= 0x4a0000 && a <= 0x4a0003) { watchdog_at = cpu.cycles + 3ull * main_clock; return; }
    if (a == 0x4a0004 || a == 0x4a0014) { coin_write(a == 0x4a0014, v); return; }
    if (a == 0x4a0005 || a == 0x4a0015) { auto &word = coin_word[a == 0x4a0015]; word = uint16_t((word & 0xff00) | v); return; }
    if (a == 0x4a0013) { eeprom->pins(v, cpu.cycles); return; }
    if (a == 0x4c0000) timer_control = uint16_t((timer_control & 0xff) | (v << 8));
    if (a == 0x4c0001) timer_control = uint16_t((timer_control & 0xff00) | v);
    // MAME records this timer control but does not assert timer IRQ5.
}
void Machine::write16(uint32_t a, uint16_t v) {
    if (auto *p = direct_bytes<2, true>(*this, a)) {
        p[0] = uint8_t(v >> 8);
        p[1] = uint8_t(v);
        return;
    }
    write8(a, uint8_t(v >> 8));
    write8(a + 1, uint8_t(v));
}
void Machine::write32(uint32_t a, uint32_t v) {
    if (auto *p = direct_bytes<4, true>(*this, a)) {
        p[0] = uint8_t(v >> 24);
        p[1] = uint8_t(v >> 16);
        p[2] = uint8_t(v >> 8);
        p[3] = uint8_t(v);
        return;
    }
    write16(a, uint16_t(v >> 16));
    write16(a + 2, uint16_t(v));
}
void Machine::advance_to(uint64_t cycles) {
    if (cycles < hardware_cycles) throw std::runtime_error("CPU cycle clock moved backwards");
    while (hardware_cycles < cycles) {
        const uint64_t event = std::min(next_vblank, irq3_at);
        const uint64_t end = std::min(cycles, event);
        audio->advance(uint32_t(end - hardware_cycles));
        hardware_cycles = end;
        if (hardware_cycles == next_vblank) {
            if (game_video) game_video->render_frame();
            else video->render_frame(palette, graphics, control, pixels_);
            pending_irqs |= 1u << 2;
            irq3_at = next_vblank + 10000;
            ++frame;
            next_vblank = raster_cycle((frame + 1) * frame_pixels);
        }
        if (hardware_cycles == irq3_at) { pending_irqs |= 1u << 3; irq3_at = UINT64_MAX; }
    }
}
int Machine::boundary() {
    cpu.dispatch_deadline = 0;
    advance_to(cpu.cycles);
    if (cpu.halted) return 1;
    for (int level = 7; level > int((cpu.sr >> 8) & 7); --level) {
        if (pending_irqs & (1u << level)) {
            pending_irqs &= uint8_t(~(1u << level));
            const uint16_t old_sr = cpu.sr;
            const uint32_t old_pc = cpu.pc;
            f3_exception(&cpu, 24 + unsigned(level), old_pc);
            if (old_sr & 0x1000) {
                // 68020 interrupt from master mode: additional format-1 frame on ISP.
                f3_set_sr(&cpu, uint16_t(cpu.sr & ~0x1000));
                cpu.a[7] -= 8;
                write16(cpu.a[7], uint16_t(old_sr | 0x2000));
                write32(cpu.a[7] + 2, old_pc);
                write16(cpu.a[7] + 6, uint16_t(0x1000 | ((24 + level) * 4)));
            }
            f3_set_sr(&cpu, uint16_t((cpu.sr & ~0x0700) | (level << 8)));
            if (!cpu.pc) cpu.pc = read32(cpu.vbr + 15 * 4);
            return 1;
        }
    }
    if (cpu.cycles >= watchdog_at) {
        reset_devices();
        audio->reset_board();
        if (sound_trace) sound_trace->record(*this, SoundTrace::Reset, cpu.pc, 0, 2, 0);
        interpreter->reset_main();
        return 1;
    }
    const uint64_t next_event = std::min({next_vblank, irq3_at, watchdog_at});
    if (cpu.stopped) {
        cpu.cycles = next_event;
        advance_to(cpu.cycles);
        return 1;
    }
    cpu.dispatch_deadline = next_event;
    return 0;
}
int Machine::fallback() {
    if (cpu.halted) return 0;
    const uint32_t physical_pc = cpu.pc & 0xffffffu;
    for (const auto &range : excluded_code) {
        if (physical_pc < range.start) break;
        if (physical_pc < range.end) {
            std::ostringstream message;
            message << "Excluded main CPU instruction at PC 0x" << std::hex << cpu.pc
                    << " in [0x" << range.start << ", 0x" << range.end << "): "
                    << range.reason << "; evidence: " << range.evidence;
            cpu.halted = 1;
            throw std::runtime_error(message.str());
        }
    }
    if (!allow_main_fallback) {
        std::ostringstream message;
        message << "Untranslated main CPU instruction at PC 0x" << std::hex << cpu.pc;
        throw std::runtime_error(message.str());
    }
    ++fallback_instructions;
    if (!fallback_hits.empty()) ++fallback_hits[physical_pc >> 1];
    return interpreter->run_main(1) > 0 && !cpu.halted;
}
bool Machine::run_frame(bool translated) {
    const uint64_t target = frame + 1;
    audio->begin_frame(frame);
    while (frame < target && !cpu.halted) {
        if (translated) {
            if (!f3_dispatch(&cpu)) { audio->finish_frame(frame); return false; }
        }
        else if (!boundary()) {
            // Reference execution must hand MMIO/IRQ changes back at every
            // instruction boundary. Coarse slices alter the ROM boot checks.
            interpreter->run_main(1);
        }
    }
    audio->finish_frame(frame);
    return !cpu.halted;
}
void Machine::load_eeprom(const std::filesystem::path &p) { eeprom->load(p); }
void Machine::save_eeprom(const std::filesystem::path &p) const { eeprom->save(p); }
void Machine::set_input(unsigned port, uint32_t mask, bool pressed) {
    if (port >= inputs.size()) throw std::out_of_range("Input port index");
    if (pressed) inputs[port] &= ~mask; else inputs[port] |= mask;
}
size_t Machine::state_size() const {
    size_t sz = sizeof(CanonicalF3Cpu) +
                sizeof(CanonicalMachineClocks) +
                ram.size() +
                palette.size() +
                graphics.size() +
                control.size() +
                shared.size() +
                sizeof(uint32_t) * pixels_.size() +
                eeprom->state_size() +
                audio->state_size() +
                video->state_size();
    if (audio->backend() != Audio::Backend::Hle) {
        if (sound_native) sz += sound_native->state_size();
        else sz += interpreter->sound_state_size();
    }
    if (game_video) sz += game_video->state_size();
    return sz;
}
size_t Machine::sync_state_size() const {
    return state_size() - (game_video ? game_video->state_size() - game_video->sync_state_size() : 0);
}

void Machine::save_state(std::span<uint8_t> dst) const {
    save_state_impl(dst, false);
}
void Machine::save_sync_state(std::span<uint8_t> dst) const {
    save_state_impl(dst, true);
}
void Machine::save_state_impl(std::span<uint8_t> dst, bool sync) const {
    const size_t expected = sync ? sync_state_size() : state_size();
    if (dst.size() != expected) {
        throw std::invalid_argument("Machine snapshot save size mismatch: expected " +
            std::to_string(expected) + ", got " + std::to_string(dst.size()));
    }
    StateWriter writer(dst);
    // 1. Native CPU
    CanonicalF3Cpu cpu_st{};
    for (int i = 0; i < 8; ++i) {
        cpu_st.d[i] = cpu.d[i];
        cpu_st.a[i] = cpu.a[i];
    }
    cpu_st.pc = cpu.pc;
    cpu_st.usp = cpu.usp;
    cpu_st.ssp = cpu.ssp;
    cpu_st.msp = cpu.msp;
    cpu_st.vbr = cpu.vbr;
    cpu_st.sfc = cpu.sfc;
    cpu_st.dfc = cpu.dfc;
    cpu_st.cacr = cpu.cacr;
    cpu_st.caar = cpu.caar;
    cpu_st.sr = cpu.sr;
    cpu_st.stopped = cpu.stopped;
    cpu_st.halted = cpu.halted;
    cpu_st.cc_src = cpu.cc_src;
    cpu_st.cc_dst = cpu.cc_dst;
    cpu_st.cc_result = cpu.cc_result;
    cpu_st.cc_op = cpu.cc_op;
    cpu_st.cc_width = cpu.cc_width;
    cpu_st.cc_mask = cpu.cc_mask;
    cpu_st.cycles = cpu.cycles;
    cpu_st.dispatch_deadline = cpu.dispatch_deadline;
    writer.write(cpu_st);

    // 2. Machine clocks, scheduler, inputs & coins
    CanonicalMachineClocks mcl{};
    mcl.hardware_cycles = hardware_cycles;
    mcl.next_vblank = next_vblank;
    mcl.irq3_at = irq3_at;
    mcl.watchdog_at = watchdog_at;
    mcl.frame = frame;
    mcl.pending_irqs = pending_irqs;
    mcl.timer_control = timer_control;
    for (int i = 0; i < 6; ++i) mcl.inputs[i] = inputs[i];
    mcl.system_inputs = system_inputs;
    for (int i = 0; i < 4; ++i) {
        mcl.coin_count[i] = coin_count[i];
        mcl.coin_locked[i] = coin_locked[i] ? 1 : 0;
    }
    for (int i = 0; i < 2; ++i) mcl.coin_word[i] = coin_word[i];
    writer.write(mcl);

    // 3. RAM regions
    writer.write_span(std::span<const uint8_t, 0x20000>(ram));
    writer.write_span(std::span<const uint8_t, 0x8000>(palette));
    writer.write_span(std::span<const uint8_t, 0x40000>(graphics));
    writer.write_span(std::span<const uint8_t, 0x20>(control));
    writer.write_span(std::span<const uint8_t, 0x800>(shared));
    writer.write_span(std::span<const uint32_t>(native_pixels()));

    // 4. EEPROM
    eeprom->save_state(writer);

    // 5. Audio
    {
        std::span<uint8_t> audio_slice(writer.current(), audio->state_size());
        audio->save_state(audio_slice);
        writer.advance(audio->state_size());
    }

    // 6. Sound CPU
    if (audio->backend() != Audio::Backend::Hle) {
        if (sound_native) sound_native->save_state(writer);
        else interpreter->save_sound_state(writer);
    }

    // 7. Video (always FDP)
    {
        std::span<uint8_t> v_slice(writer.current(), video->state_size());
        video->save_state(v_slice);
        if (sync) {
            // FDP scanout reloads controls from Machine::control and rebuilds
            // row usages from graphics RAM. Game mode can skip that scanout,
            // so its stale caches must not enter the synchronization checksum.
            constexpr size_t begin = offsetof(CanonicalVideo, control_0);
            constexpr size_t end = offsetof(CanonicalVideo, spritelist);
            std::fill(v_slice.begin() + begin, v_slice.begin() + end, uint8_t(0));
        }
        writer.advance(video->state_size());
    }

    // 8. GameVideo (when present)
    if (game_video) {
        const size_t size = sync ? game_video->sync_state_size() : game_video->state_size();
        std::span<uint8_t> gv_slice(writer.current(), size);
        if (sync) game_video->save_sync_state(gv_slice);
        else game_video->save_state(gv_slice);
        writer.advance(size);
    }

    if (writer.remaining() != 0) {
        throw std::logic_error("Machine::save_state remaining unwritten bytes");
    }
}

void Machine::load_state(std::span<const uint8_t> src) {
    load_state_impl(src, false);
}
void Machine::load_sync_state(std::span<const uint8_t> src) {
    load_state_impl(src, true);
}
void Machine::load_state_impl(std::span<const uint8_t> src, bool sync) {
    const size_t expected = sync ? sync_state_size() : state_size();
    if (src.size() != expected) {
        throw std::invalid_argument("Machine snapshot load size mismatch: expected " +
            std::to_string(expected) + ", got " + std::to_string(src.size()));
    }
    native_pixels();
    StateReader reader(src);
    // 1. Native CPU
    CanonicalF3Cpu cpu_st;
    reader.read(cpu_st);
    for (int i = 0; i < 8; ++i) {
        cpu.d[i] = cpu_st.d[i];
        cpu.a[i] = cpu_st.a[i];
    }
    cpu.pc = cpu_st.pc;
    cpu.usp = cpu_st.usp;
    cpu.ssp = cpu_st.ssp;
    cpu.msp = cpu_st.msp;
    cpu.vbr = cpu_st.vbr;
    cpu.sfc = cpu_st.sfc;
    cpu.dfc = cpu_st.dfc;
    cpu.cacr = cpu_st.cacr;
    cpu.caar = cpu_st.caar;
    cpu.sr = cpu_st.sr;
    cpu.stopped = cpu_st.stopped;
    cpu.halted = cpu_st.halted;
    cpu.cc_src = cpu_st.cc_src;
    cpu.cc_dst = cpu_st.cc_dst;
    cpu.cc_result = cpu_st.cc_result;
    cpu.cc_op = cpu_st.cc_op;
    cpu.cc_width = cpu_st.cc_width;
    cpu.cc_mask = cpu_st.cc_mask;
    cpu.cc_pad = 0;
    cpu.cycles = cpu_st.cycles;
    cpu.dispatch_deadline = cpu_st.dispatch_deadline;
    cpu.runtime = this;

    // 2. Machine clocks, scheduler, inputs & coins
    CanonicalMachineClocks mcl;
    reader.read(mcl);
    hardware_cycles = mcl.hardware_cycles;
    next_vblank = mcl.next_vblank;
    irq3_at = mcl.irq3_at;
    watchdog_at = mcl.watchdog_at;
    frame = mcl.frame;
    pending_irqs = mcl.pending_irqs;
    timer_control = mcl.timer_control;
    for (int i = 0; i < 6; ++i) inputs[i] = mcl.inputs[i];
    system_inputs = mcl.system_inputs;
    for (int i = 0; i < 4; ++i) {
        coin_count[i] = mcl.coin_count[i];
        coin_locked[i] = mcl.coin_locked[i] != 0;
    }
    for (int i = 0; i < 2; ++i) coin_word[i] = mcl.coin_word[i];

    // 3. RAM regions
    reader.read_span(std::span<uint8_t, 0x20000>(ram));
    reader.read_span(std::span<uint8_t, 0x8000>(palette));
    reader.read_span(std::span<uint8_t, 0x40000>(graphics));
    reader.read_span(std::span<uint8_t, 0x20>(control));
    reader.read_span(std::span<uint8_t, 0x800>(shared));
    reader.read_span(std::span<uint32_t>(pixels_));

    // 4. EEPROM
    eeprom->load_state(reader);

    // 5. Audio
    {
        std::span<const uint8_t> audio_slice(reader.current(), audio->state_size());
        audio->load_state(audio_slice);
        reader.skip(audio->state_size());
    }

    // 6. Sound CPU
    if (audio->backend() != Audio::Backend::Hle) {
        if (sound_native) sound_native->load_state(reader);
        else interpreter->load_sound_state(reader);
    }

    // Also sync main interpreter context so Musashi is ready for any fallback
    if (interpreter) {
        interpreter->sync_main_from_cpu();
    }

    // 7. Video (always FDP)
    {
        std::span<const uint8_t> v_slice(reader.current(), video->state_size());
        video->load_state(v_slice);
        reader.skip(video->state_size());
    }

    // 8. GameVideo (when present)
    if (game_video) {
        const size_t size = sync ? game_video->sync_state_size() : game_video->state_size();
        std::span<const uint8_t> gv_slice(reader.current(), size);
        if (sync) game_video->load_sync_state(gv_slice);
        else game_video->load_state(gv_slice);
        reader.skip(size);
    }

    if (reader.remaining() != 0) {
        throw std::logic_error("Machine::load_state remaining unread bytes");
    }
}

uint32_t Machine::state_crc() const {
    const size_t sz = state_size();
    if (state_scratch_.size() != sz) {
        state_scratch_.resize(sz);
    }
    save_state(state_scratch_);
    return crc32(state_scratch_.data(), state_scratch_.size());
}
uint32_t Machine::sync_state_crc() const {
    const size_t sz = sync_state_size();
    if (sync_state_scratch_.size() != sz) sync_state_scratch_.resize(sz);
    save_sync_state(sync_state_scratch_);
    return crc32(sync_state_scratch_.data(), sync_state_scratch_.size());
}
}
