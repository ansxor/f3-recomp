#include "f3rt/machine.hpp"
#include "f3rt/memory_map.hpp"
#include "f3rt/abi/audio.h"
#include "f3rt/abi/eeprom.h"
#include "f3rt/game_video.hpp"
#include "sprites/sprite_system.hpp"
#include "state_io.hpp"
#include <algorithm>
#include <stdexcept>
#include <sstream>
#include <utility>

namespace f3rt {
Machine::AudioLease::~AudioLease() {
    if (held) f3rt_audio_shutdown();
}
Machine::EepromLease::~EepromLease() {
    if (held) f3rt_eeprom_shutdown();
}
namespace {

// Decode once only when every byte is contiguous and has no observable bus
// side effect. All boundary/mirror-wrap cases retain ordered byte accesses.
template <unsigned Width, bool Write> uint8_t *direct_bytes(Machine &m, uint32_t a) {
    a &= 0xffffff;
    if constexpr (!Write) {
        if (a <= m.roms.main.size() - Width) return m.roms.main.data() + a;
    }
    const auto video = [&]() -> uint8_t * {
        if (a - GRAPHICS_BASE > m.graphics.size() - Width) return nullptr;
        if constexpr (Write)
            if (m.sprite_writers && a - GRAPHICS_BASE < SPRITE_RAM_SIZE) return nullptr;
        return m.graphics.data() + (a - GRAPHICS_BASE);
    };
    if constexpr (Write) {
        if (a - GRAPHICS_BASE < GRAPHICS_END - GRAPHICS_BASE) return video();
    }
    if (a - RAM_BASE < RAM_END - RAM_BASE) {
        const uint32_t offset = a & RAM_MIRROR_MASK;
        if (offset <= m.ram.size() - Width) return m.ram.data() + offset;
    }
    if (a - PALETTE_BASE <= m.palette.size() - Width) return m.palette.data() + (a - PALETTE_BASE);
    if constexpr (!Write) {
        if (uint8_t *p = video()) return p;
        if (a - SHARED_BASE <= m.shared.size() - Width) return m.shared.data() + (a - SHARED_BASE);
    }
    return nullptr;
}
} // namespace
Machine::Machine(RomSet set)
    : m_pixels(size_t(320) * set.video.visible_height), roms(std::move(set)),
      sound_bankswitch(roms.sound_bankswitch) {
    if (roms.main.size() < CPU_VECTOR_TABLE_SIZE || roms.main.size() > ROM_END ||
        roms.main.size() % 4)
        throw std::runtime_error("Main ROM must hold vectors and fit the 2 MiB F3 window");
    m_rom_crcs = f3rt::rom_region_crcs(roms);
    const auto &video = roms.video;
    if (video.sprite_lag > 2 || !video.visible_height || video.visible_y >= 256 ||
        video.visible_height > 256 - video.visible_y ||
        !graphics_assets.load_roms(roms.sprites, roms.sprites_hi, roms.tiles, roms.tiles_hi))
        throw std::runtime_error("Invalid video ROM regions");
    std::vector<uint8_t>().swap(roms.sprites);
    std::vector<uint8_t>().swap(roms.sprites_hi);
    std::vector<uint8_t>().swap(roms.tiles);
    std::vector<uint8_t>().swap(roms.tiles_hi);
    switch (f3rt_audio_init(roms.sound.data(), roms.sound.size(), roms.samples.data(),
                            roms.samples.size(), shared.data())) {
    case F3RT_AUDIO_OK:
        m_audio_lease.held = true;
        break;
    case F3RT_AUDIO_UNSUPPORTED_ROM:
        throw std::runtime_error("Audio is not supported for this game");
    default:
        throw std::runtime_error("Audio is already initialized");
    }
    std::vector<uint8_t>().swap(roms.samples);
    if (f3rt_eeprom_init() != F3RT_EEPROM_OK) throw std::runtime_error("EEPROM is already initialized");
    m_eeprom_lease.held = true;
    if (!roms.factory_eeprom.empty()) {
        if (roms.factory_eeprom.size() != F3RT_EEPROM_FILE_BYTES)
            throw std::runtime_error("Factory EEPROM ROM must be exactly 128 bytes");
        // Seed once: resets preserve guest settings, and explicit EEPROM loads override these words.
        f3rt_eeprom_seed_be_bytes(roms.factory_eeprom.data());
    }
    // Analog ports start at counter zero; resets preserve the serialized counters.
    if (roms.input.dial) inputs[2] = inputs[3] = 0xffff0000;
    reset();
}
Machine::~Machine() = default;
const std::vector<uint32_t> &Machine::native_pixels() const {
    if (game_video) game_video->materialize_native();
    return m_pixels;
}
uint64_t Machine::raster_time(uint64_t pixels) const {
    return (pixels * MAIN_CLOCK + PIXEL_CLOCK - 1) / PIXEL_CLOCK;
}
void Machine::bind_cpu_windows() {
#ifdef F3RT_TRACE_STACKED_SR
    // Route generated RAM writes through Machine so overwrites retire SR marks.
    cpu.ram = nullptr;
#else
    cpu.ram = ram.data();
#endif
    cpu.rom = roms.main.data();
    cpu.rom_size = uint32_t(roms.main.size());
    cpu.ram_writer = nullptr;
}
void Machine::reset_devices() {
    cpu.dispatch_deadline = 0;
    f3rt_audio_set_reset(true);
    f3rt_eeprom_pins(0);
    pending_irqs = 0;
    m_irq3_at = UINT64_MAX;
    m_watchdog_at = m_time + 3ull * MAIN_CLOCK;
}
void Machine::reset() {
    if (sprites) sprites->reset();
    native_pixels();
    cpu = {};
    cpu.runtime = this;
    bind_cpu_windows();
    f3rt_eeprom_reset();
    frame = m_time = 0;
    m_idle = false;
    // Reference beam time zero is VBSTART, not scanline zero.
    m_next_vblank = raster_time(FRAME_PIXELS);
    reset_devices();
    f3rt_audio_set_reset(true);
    if (game_video) game_video->reset();
    reset_main_cpu();
}
void Machine::reset_main_cpu() {
    // 68EC020 RESET, mirroring Musashi m68k_pulse_reset: supervisor mode with
    // mask 7, T bits clear, VBR 0, SSP and PC from vectors 0 and 4. CCR, data,
    // address registers, CACR, CAAR, SFC and DFC are preserved.
    f3_cc_flush(&cpu);
    cpu.stopped = 0;
    cpu.halted = 0;
    f3_set_sr(&cpu, uint16_t(0x2700u | (cpu.sr & 0x1fu)));
    cpu.vbr = 0;
    cpu.a[7] = read32(0);
    cpu.ssp = cpu.a[7];
    cpu.pc = read32(4);
    m_slice_end = cpu.steps + SLICE_STEPS;
}
uint32_t Machine::input_word(unsigned index) const {
    if (index >= inputs.size()) return 0xffffffff;
    uint32_t value = inputs[index];
    if (index == 0) {
        const uint32_t io = (system_inputs & 0xfe) | unsigned(f3rt_eeprom_output());
        value = (value & 0xffff) | (io << 16) | (io << 24);
    } else if (index == 1 || index == 5) {
        value = (value & 0xffff) | (uint32_t(coin_word[index == 5]) << 16);
    }
    return value;
}
uint8_t Machine::read8(uint32_t a) {
    a &= 0xffffff;
    if (a < ROM_END) return a < roms.main.size() ? roms.main[a] : 0xff;
    if (a >= RAM_BASE && a < RAM_END) return ram[a & RAM_MIRROR_MASK];
    if (a >= PALETTE_BASE && a < PALETTE_END) return palette[a - PALETTE_BASE];
    if (a >= IO_BASE && a < IO_END)
        return uint8_t(input_word((a - IO_BASE) / 4) >> (24 - 8 * (a & 3)));
    if (a >= GRAPHICS_BASE && a < GRAPHICS_END) return graphics[a - GRAPHICS_BASE];
    if (a >= SHARED_BASE && a < SHARED_END) return shared[a - SHARED_BASE];
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
// Unlisted addresses, including ROM, ignore writes.
void Machine::write8(uint32_t a, uint8_t v) {
#ifdef F3RT_TRACE_STACKED_SR
    invalidate_stacked_sr(a, 1);
#endif
    a &= 0xffffff;
    if (a >= RAM_BASE && a < RAM_END) {
        ram[a & RAM_MIRROR_MASK] = v;
        if (sprites) sprites->note_write(a, 1);
        return;
    }
    if (a >= PALETTE_BASE && a < PALETTE_END) {
        palette[a - PALETTE_BASE] = v;
        return;
    }
    if (a >= GRAPHICS_BASE && a < GRAPHICS_END) {
        const uint32_t offset = a - GRAPHICS_BASE;
        if (sprite_writers && offset < SPRITE_RAM_SIZE) (*sprite_writers)[offset >> 4] = cpu.pc;
        graphics[offset] = v;
        if (sprites) sprites->note_write(a, 1);
        return;
    }
    if (a >= CONTROL_BASE && a < CONTROL_END) {
        control[a - CONTROL_BASE] = v;
        return;
    }
    if (a >= SHARED_BASE && a < SHARED_END) {
        shared[a - SHARED_BASE] = v;
        f3rt_audio_shared_write(a - SHARED_BASE);
        return;
    }
    if ((a >= SOUND_RESET_RELEASE && a < SOUND_RESET_RELEASE + 4) ||
        (a >= SOUND_RESET_ASSERT && a < SOUND_RESET_ASSERT + 4)) {
        f3rt_audio_set_reset(a >= SOUND_RESET_ASSERT);
        return;
    }
    // Watchdog deadline: 3 seconds of device time.
    if (a >= WATCHDOG && a < WATCHDOG + 4) {
        m_watchdog_at = m_time + 3ull * MAIN_CLOCK;
        return;
    }
    if (a == COIN_BANK0 || a == COIN_BANK1) {
        coin_write(a == COIN_BANK1, v);
        return;
    }
    if (a == COIN_BANK0 + 1 || a == COIN_BANK1 + 1) {
        auto &word = coin_word[a == COIN_BANK1 + 1];
        word = uint16_t((word & 0xff00) | v);
        return;
    }
    if (a == EEPROM_PINS) {
        f3rt_eeprom_pins(v);
        return;
    }
    if (sound_bankswitch && a >= MAIN_SOUND_BANKSWITCH_BASE && a < MAIN_SOUND_BANKSWITCH_END) {
        uint32_t offset = (a - MAIN_SOUND_BANKSWITCH_BASE) >> 2;
        bool low16 = (a & 2) != 0;
        uint32_t idx = (offset << 1) & 0x1e;
        if (low16) idx += 1;
        if (idx >= 8) idx -= 8;
        f3rt_audio_set_sound_bank(idx);
        return;
    }
    if (a == TIMER_CONTROL) timer_control = uint16_t((timer_control & 0xff) | (v << 8));
    if (a == TIMER_CONTROL + 1) timer_control = uint16_t((timer_control & 0xff00) | v);
    // MAME records this timer control but does not assert timer IRQ5.
}
void Machine::write16(uint32_t a, uint16_t v) {
    if (auto *p = direct_bytes<2, true>(*this, a)) {
#ifdef F3RT_TRACE_STACKED_SR
        invalidate_stacked_sr(a, 2);
#endif
        p[0] = uint8_t(v >> 8);
        p[1] = uint8_t(v);
        if (sprites) sprites->note_write(a, 2);
        return;
    }
    if (sound_bankswitch && a >= MAIN_SOUND_BANKSWITCH_BASE && a < MAIN_SOUND_BANKSWITCH_END) {
        uint32_t offset = (a - MAIN_SOUND_BANKSWITCH_BASE) >> 2;
        bool low16 = (a & 2) != 0;
        uint32_t idx = (offset << 1) & 0x1e;
        if (low16) idx += 1;
        if (idx >= 8) idx -= 8;
        f3rt_audio_set_sound_bank(idx);
        return;
    }
    write8(a, uint8_t(v >> 8));
    write8(a + 1, uint8_t(v));
}
void Machine::write32(uint32_t a, uint32_t v) {
    if (auto *p = direct_bytes<4, true>(*this, a)) {
#ifdef F3RT_TRACE_STACKED_SR
        invalidate_stacked_sr(a, 4);
#endif
        p[0] = uint8_t(v >> 24);
        p[1] = uint8_t(v >> 16);
        p[2] = uint8_t(v >> 8);
        p[3] = uint8_t(v);
        if (sprites) sprites->note_write(a, 4);
        return;
    }
    if (sound_bankswitch && a >= MAIN_SOUND_BANKSWITCH_BASE && a < MAIN_SOUND_BANKSWITCH_END) {
        uint32_t offset = (a - MAIN_SOUND_BANKSWITCH_BASE) >> 2;
        uint32_t idx = ((offset << 1) & 0x1e) + 1;
        if (idx >= 8) idx -= 8;
        f3rt_audio_set_sound_bank(idx);
        return;
    }
    write16(a, uint16_t(v >> 16));
    write16(a + 2, uint16_t(v));
}
#ifdef F3RT_TRACE_STACKED_SR
void Machine::invalidate_stacked_sr(uint32_t address, unsigned width) {
    address &= 0xffffff;
    if (address - RAM_BASE >= RAM_END - RAM_BASE) return;
    for (unsigned i = 0; i <= width; ++i)
        stacked_sr_low_bytes.reset((address + i) & RAM_MIRROR_MASK);
}
#endif
void Machine::write_stacked_sr(uint32_t address, uint16_t value) {
    write16(address, value);
#ifdef F3RT_TRACE_STACKED_SR
    address &= 0xffffff;
    if (address - RAM_BASE < RAM_END - RAM_BASE - 1)
        stacked_sr_low_bytes.set((address + 1) & RAM_MIRROR_MASK);
#endif
}
void Machine::idle() {
    m_idle = true;
    cpu.dispatch_deadline = 0;
}
// Advance device time to the next event and raise it. The watchdog is checked
// whenever time moves, so it fires at the first event past its deadline.
void Machine::next_event() {
    const uint64_t at = std::min(m_next_vblank, m_irq3_at);
    f3rt_audio_advance(uint32_t(at - m_time));
    m_time = at;
    if (m_time == m_next_vblank) {
        if (game_video) game_video->render_frame();
        else if (inspect_scanout) inspect_scanout(m_pixels);
        pending_irqs |= 1u << 2;
        m_irq3_at = m_next_vblank + IRQ3_DELAY;
        ++frame;
        m_next_vblank = raster_time((frame + 1) * FRAME_PIXELS);
        if (sprites) sprites->frame_end();
    } else {
        pending_irqs |= 1u << 3;
        m_irq3_at = UINT64_MAX;
    }
    if (m_time >= m_watchdog_at) {
        reset_devices();
        f3rt_audio_set_reset(true);
        reset_main_cpu();
    }
    m_slice_end = cpu.steps + SLICE_STEPS;
}
int Machine::boundary() {
    cpu.dispatch_deadline = 0;
    if (cpu.halted) return 1;
    const bool idle = std::exchange(m_idle, false);
    for (int level = 7; level > int((cpu.sr >> 8) & 7); --level) {
        if (pending_irqs & (1u << level)) {
            pending_irqs &= uint8_t(~(1u << level));
            f3_cc_flush(&cpu);
            const uint16_t old_sr = cpu.sr;
            const uint32_t old_pc = cpu.pc;
            f3_exception(&cpu, 24 + unsigned(level), old_pc);
            if (old_sr & 0x1000) {
                // 68020 interrupt from master mode: additional format-1 frame on ISP.
                f3_set_sr(&cpu, uint16_t(cpu.sr & ~0x1000));
                cpu.a[7] -= 8;
                write_stacked_sr(cpu.a[7], uint16_t(old_sr | 0x2000));
                write32(cpu.a[7] + 2, old_pc);
                write16(cpu.a[7] + 6, uint16_t(0x1000 | ((24 + level) * 4)));
            }
            f3_set_sr(&cpu, uint16_t((cpu.sr & ~0x0700) | (level << 8)));
            if (!cpu.pc) cpu.pc = read32(cpu.vbr + 15 * 4);
            return 1;
        }
    }
    // A waiting, stopped or long-running CPU moves time to the next device event.
    if (idle || cpu.stopped || cpu.steps >= m_slice_end) {
        next_event();
        return 1;
    }
    cpu.dispatch_deadline = m_slice_end;
    return 0;
}
int Machine::fallback() {
    if (cpu.halted) return 0;
    const uint32_t physical_pc = cpu.pc & 0xffffffu;
    for (const auto &range : excluded_code) {
        if (physical_pc < range.start) break;
        if (physical_pc < range.end) {
            std::ostringstream message;
            message << "Excluded main CPU instruction at PC 0x" << std::hex << cpu.pc << " in [0x"
                    << range.start << ", 0x" << range.end << "): " << range.reason
                    << "; evidence: " << range.evidence;
            cpu.halted = 1;
            throw std::runtime_error(message.str());
        }
    }
    std::ostringstream message;
    message << "Untranslated main CPU instruction at PC 0x" << std::hex << cpu.pc;
    throw std::runtime_error(message.str());
}
bool Machine::run_frame() {
    const uint64_t target = frame + 1;
    while (frame < target && !cpu.halted) {
        if (!f3_dispatch(&cpu)) {
            f3rt_audio_finish_frame();
            return false;
        }
    }
    f3rt_audio_finish_frame();
    return !cpu.halted;
}
void Machine::load_eeprom(const std::filesystem::path &p) {
    const std::string path = p.string();
    switch (f3rt_eeprom_load_file(path.c_str())) {
    case F3RT_EEPROM_OK:
        return;
    case F3RT_EEPROM_INVALID_FILE:
        throw std::runtime_error("Invalid EEPROM file: " + path);
    default:
        throw std::runtime_error("EEPROM read failed");
    }
}
void Machine::save_eeprom(const std::filesystem::path &p) const {
    if (f3rt_eeprom_save_file(p.string().c_str()) != F3RT_EEPROM_OK)
        throw std::runtime_error("EEPROM write failed");
}
void Machine::set_input(unsigned port, uint32_t mask, bool pressed) {
    if (port >= inputs.size()) throw std::out_of_range("Input port index");
    if (pressed) inputs[port] &= ~mask;
    else inputs[port] |= mask;
}
size_t Machine::state_size() const {
    size_t sz = sizeof(CanonicalF3Cpu) + sizeof(CanonicalMachineClocks) + ram.size() +
                palette.size() + graphics.size() + control.size() + shared.size() +
                sizeof(uint32_t) * m_pixels.size() + F3RT_EEPROM_STATE_SIZE + F3RT_AUDIO_STATE_SIZE;
    if (game_video) sz += game_video->state_size();
    return sz;
}

void Machine::save_state(std::span<uint8_t> dst) const {
    const size_t expected = state_size();
    if (dst.size() != expected) {
        throw std::invalid_argument("Machine snapshot save size mismatch: expected " +
                                    std::to_string(expected) + ", got " +
                                    std::to_string(dst.size()));
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
    cpu_st.steps = cpu.steps;
    cpu_st.dispatch_deadline = cpu.dispatch_deadline;
    writer.write(cpu_st);

    // 2. Machine clocks, scheduler, inputs & coins
    CanonicalMachineClocks mcl{};
    mcl.time = m_time;
    mcl.next_vblank = m_next_vblank;
    mcl.irq3_at = m_irq3_at;
    mcl.watchdog_at = m_watchdog_at;
    mcl.slice_end = m_slice_end;
    mcl.idle = m_idle ? 1 : 0;
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
    f3rt_eeprom_save_state(writer.current());
    writer.advance(F3RT_EEPROM_STATE_SIZE);

    // 5. Audio
    f3rt_audio_save_state(writer.current());
    writer.advance(F3RT_AUDIO_STATE_SIZE);

    if (game_video) {
        const size_t size = game_video->state_size();
        std::span<uint8_t> gv_slice(writer.current(), size);
        game_video->save_state(gv_slice);
        writer.advance(size);
    }

    if (writer.remaining() != 0) {
        throw std::logic_error("Machine::save_state remaining unwritten bytes");
    }
}

void Machine::load_state(std::span<const uint8_t> src) {
    const size_t expected = state_size();
    if (src.size() != expected) {
        throw std::invalid_argument("Machine snapshot load size mismatch: expected " +
                                    std::to_string(expected) + ", got " +
                                    std::to_string(src.size()));
    }
    if (sprites) sprites->reset();
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
    cpu.steps = cpu_st.steps;
    cpu.dispatch_deadline = cpu_st.dispatch_deadline;
    cpu.runtime = this;

    // 2. Machine clocks, scheduler, inputs & coins
    CanonicalMachineClocks mcl;
    reader.read(mcl);
    m_time = mcl.time;
    m_next_vblank = mcl.next_vblank;
    m_irq3_at = mcl.irq3_at;
    m_watchdog_at = mcl.watchdog_at;
    m_slice_end = mcl.slice_end;
    m_idle = mcl.idle != 0;
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
#ifdef F3RT_TRACE_STACKED_SR
    stacked_sr_low_bytes.reset();
#endif
    reader.read_span(std::span<uint8_t, 0x8000>(palette));
    reader.read_span(std::span<uint8_t, 0x40000>(graphics));
    reader.read_span(std::span<uint8_t, 0x20>(control));
    reader.read_span(std::span<uint8_t, 0x800>(shared));
    reader.read_span(std::span<uint32_t>(m_pixels));

    // 4. EEPROM
    if (f3rt_eeprom_load_state(reader.current()) != F3RT_EEPROM_OK)
        throw std::invalid_argument("Invalid EEPROM snapshot");
    reader.skip(F3RT_EEPROM_STATE_SIZE);

    // 5. Audio
    if (f3rt_audio_load_state(reader.current()) != F3RT_AUDIO_OK)
        throw std::invalid_argument("Invalid audio snapshot");
    reader.skip(F3RT_AUDIO_STATE_SIZE);

    if (game_video) {
        const size_t size = reader.remaining();
        std::span<const uint8_t> gv_slice(reader.current(), size);
        game_video->load_state(gv_slice);
        reader.skip(size);
    }

    if (reader.remaining() != 0) {
        throw std::logic_error("Machine::load_state remaining unread bytes");
    }
}

uint32_t Machine::state_crc() const {
    const size_t sz = state_size();
    if (m_state_scratch.size() != sz) {
        m_state_scratch.resize(sz);
    }
    save_state(m_state_scratch);
    return crc32(m_state_scratch.data(), m_state_scratch.size());
}

static_assert(SnapshotRoot<Machine>);
} // namespace f3rt
