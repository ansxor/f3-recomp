#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <source_location>
#include <string>
#include <type_traits>

#include "audio/reference/state_oracle.h"
namespace f3rt {
// Validate canonical fields before device loaders can use imported indices.
template <typename T> void validate_state_value(const T &) {}

class StateWriter {
public:
    explicit StateWriter(std::span<uint8_t> dst)
        : m_ptr(dst.data()), m_remaining(dst.size()) {}

    template <typename T>
    void write(const T &val) {
        static_assert(std::is_trivially_copyable_v<T>, "Type must be trivially copyable");
        if (m_remaining < sizeof(T)) {
            throw std::runtime_error("StateWriter buffer overflow");
        }
        std::memcpy(m_ptr, &val, sizeof(T));
        m_ptr += sizeof(T);
        m_remaining -= sizeof(T);
    }

    void write_bytes(const void *src, size_t size) {
        if (!size) return;
        if (m_remaining < size) {
            throw std::runtime_error("StateWriter buffer overflow");
        }
        std::memcpy(m_ptr, src, size);
        m_ptr += size;
        m_remaining -= size;
    }

    template <typename T, size_t N>
    void write_array(const std::array<T, N> &arr) {
        static_assert(std::is_trivially_copyable_v<T>, "Type must be trivially copyable");
        write_bytes(arr.data(), sizeof(T) * N);
    }

    template <typename T, size_t Extent = std::dynamic_extent>
    void write_span(std::span<const T, Extent> sp) {
        static_assert(std::is_trivially_copyable_v<T>, "Type must be trivially copyable");
        write_bytes(sp.data(), sp.size_bytes());
    }

    template <typename T, size_t Extent = std::dynamic_extent>
    void write_span(std::span<T, Extent> sp) {
        static_assert(std::is_trivially_copyable_v<T>, "Type must be trivially copyable");
        write_bytes(sp.data(), sp.size_bytes());
    }

    void advance(size_t size) {
        if (m_remaining < size) {
            throw std::runtime_error("StateWriter buffer overflow");
        }
        m_ptr += size;
        m_remaining -= size;
    }

    size_t remaining() const { return m_remaining; }
    uint8_t *current() { return m_ptr; }

private:
    uint8_t *m_ptr;
    size_t m_remaining;
};

class StateReader {
public:
    explicit StateReader(std::span<const uint8_t> src)
        : m_ptr(src.data()), m_remaining(src.size()) {}

    template <typename T>
    void read(T &val) {
        static_assert(std::is_trivially_copyable_v<T>, "Type must be trivially copyable");
        if (m_remaining < sizeof(T)) {
            throw std::runtime_error("StateReader buffer underflow");
        }
        std::memcpy(&val, m_ptr, sizeof(T));
        validate_state_value(val);
        m_ptr += sizeof(T);
        m_remaining -= sizeof(T);
    }

    void read_bytes(void *dst, size_t size) {
        if (!size) return;
        if (m_remaining < size) {
            throw std::runtime_error("StateReader buffer underflow");
        }
        std::memcpy(dst, m_ptr, size);
        m_ptr += size;
        m_remaining -= size;
    }

    void advance(size_t size) {
        if (m_remaining < size) {
            throw std::runtime_error("StateReader buffer underflow");
        }
        m_ptr += size;
        m_remaining -= size;
    }
    void skip(size_t size) { advance(size); }

    template <typename T, size_t N>
    void read_array(std::array<T, N> &arr) {
        static_assert(std::is_trivially_copyable_v<T>, "Type must be trivially copyable");
        read_bytes(arr.data(), sizeof(T) * N);
    }

    template <typename T, size_t Extent = std::dynamic_extent>
    void read_span(std::span<T, Extent> sp) {
        static_assert(std::is_trivially_copyable_v<T>, "Type must be trivially copyable");
        read_bytes(sp.data(), sp.size_bytes());
    }

    size_t remaining() const { return m_remaining; }
    const uint8_t *current() const { return m_ptr; }

private:
    const uint8_t *m_ptr;
    size_t m_remaining;
};

#pragma pack(push, 1)

// Native f3_cpu canonical representation (no host pointer runtime, no cc_pad)
struct CanonicalF3Cpu {
    uint32_t d[8];
    uint32_t a[8];
    uint32_t pc;
    uint32_t usp;
    uint32_t ssp;
    uint32_t msp;
    uint32_t vbr;
    uint32_t sfc;
    uint32_t dfc;
    uint32_t cacr;
    uint32_t caar;
    uint16_t sr;
    uint8_t stopped;
    uint8_t halted;
    uint32_t cc_src;
    uint32_t cc_dst;
    uint32_t cc_result;
    uint8_t cc_op;
    uint8_t cc_width;
    uint8_t cc_mask;
    uint64_t cycles;
    uint64_t dispatch_deadline;
};

struct CanonicalMachineClocks {
    uint64_t hardware_cycles;
    uint64_t next_vblank;
    uint64_t irq3_at;
    uint64_t watchdog_at;
    uint64_t frame;
    uint8_t pending_irqs;
    uint16_t timer_control;
    uint32_t inputs[6];
    uint8_t system_inputs;
    uint64_t coin_count[4];
    uint8_t coin_locked[4];
    uint16_t coin_word[2];
};

struct CanonicalEeprom {
    uint16_t words[64];
    uint8_t mode;
    uint8_t selected;
    uint8_t old_clock;
    uint8_t data_out;
    uint8_t writable;
    uint32_t shift;
    uint32_t count;
    uint32_t address;
    uint32_t read_bit;
    uint64_t ready_at;
};

struct CanonicalMC68681Tx {
    uint64_t phase;
    uint32_t baud;
    uint8_t remaining;
    uint8_t sent;
    uint8_t counter_prescaler;
    uint8_t clock;
    uint8_t running;
    uint8_t enabled;
    uint8_t buffered;
};

struct CanonicalMC68681 {
    uint8_t acr;
    uint8_t imr;
    uint8_t isr;
    uint8_t ivr;
    uint8_t opcr;
    uint8_t opr;
    uint8_t ipcr;
    uint8_t ip_last_state;
    uint16_t ctr_preset;
    uint32_t ct_remaining;
    uint8_t half_period;
    uint8_t ct_running;
    uint8_t mr1a, mr2a, mr_ptra, sra, csra, cra;
    uint8_t mr1b, mr2b, mr_ptrb, srb, csrb, crb;
    CanonicalMC68681Tx tx[2];
};

struct CanonicalMB87078 {
    uint8_t gain_index[4];
    uint16_t channel_latch[4];
    uint8_t control;
    uint8_t data;
};

struct CanonicalES5505Voice {
    uint32_t control;
    uint64_t freqcount;
    uint64_t start;
    uint32_t lvol;
    uint64_t end;
    uint32_t lvramp;
    uint64_t accum;
    uint32_t rvol;
    uint32_t rvramp;
    uint32_t ecount;
    uint32_t k2;
    uint32_t k2ramp;
    uint32_t k1;
    uint32_t k1ramp;
    int32_t o4n1;
    int32_t o3n1;
    int32_t o3n2;
    int32_t o2n1;
    int32_t o2n2;
    int32_t o1n1;
    uint8_t index;
    uint8_t filtcount;
};

struct CanonicalES5505 {
    uint32_t master_clock;
    uint32_t sample_rate;
    uint8_t active_voices;
    uint8_t current_page;
    uint8_t irqv;
    uint16_t mode;
    int32_t voice_index;
    uint32_t voice_bank[32];
    CanonicalES5505Voice voices[32];
};

struct CanonicalES5510Alu {
    uint8_t aReg;
    uint8_t bReg;
    uint8_t src;
    uint8_t dst;
    uint8_t op;
    int32_t aValue;
    int32_t bValue;
    int32_t result;
    uint8_t update_ccr;
    uint8_t write_result;
};

struct CanonicalES5510MulAcc {
    uint8_t cReg;
    uint8_t dReg;
    uint8_t src;
    uint8_t dst;
    uint8_t accumulate;
    int32_t cValue;
    int32_t dValue;
    int64_t product;
    int64_t result;
    uint8_t write_result;
};

struct CanonicalES5510Ram {
    int32_t address;
    uint8_t io;
    uint8_t cycle;
};

struct CanonicalES5510Registers {
    uint8_t halt_asserted;
    uint8_t pc;
    uint8_t state;
    int16_t ser_regs[8];
    int64_t machl;
    uint8_t mac_overflow;
    int16_t dil;
    int32_t memsiz;
    int32_t memmask;
    int32_t memincrement;
    int8_t memshift;
    int32_t dlength;
    int32_t abase;
    int32_t bbase;
    int32_t dbase;
    int32_t sigreg;
    int32_t mulshift;
    int8_t ccr;
    int8_t cmr;
    int16_t dol[2];
    int32_t dol_count;
    int32_t dol_latch;
    int32_t dil_latch;
    uint32_t dadr_latch;
    int32_t gpr_latch;
    uint64_t instr_latch;
    uint8_t ram_sel;
    uint8_t host_control;
    uint8_t host_serial;
    CanonicalES5510Alu alu;
    CanonicalES5510MulAcc mulacc;
    CanonicalES5510Ram ram;
    CanonicalES5510Ram ram_p;
    CanonicalES5510Ram ram_pp;
};

struct CanonicalAudioCore {
    uint32_t bank_mask;
    uint16_t bank_table[32];
    uint8_t reset_asserted;
    uint8_t esp_halted;
    uint8_t gain_model;
    float volume_gain[2];
    float otis_gain[2];
    float output_gain[2];
    int64_t cpu_accum;
    uint32_t duart_accum;
    uint64_t sample_accum;
    uint64_t clock_ticks;
    uint64_t generated_frames;
    uint32_t rb_count;
};

struct CanonicalSoundNative {
    CanonicalF3Cpu cpu;
    uint8_t needs_reset;
    int32_t reset_cycles;
};

using CanonicalSoundOracle = f3rt_sound_oracle_state;

struct CanonicalTempsprite {
    int32_t code;
    uint8_t color;
    uint8_t flip_x;
    uint8_t flip_y;
    uint8_t pri;
    int32_t x;
    int32_t y;
    int32_t scale_x;
    int32_t scale_y;
};

struct CanonicalVideo {
    uint8_t has_buffered_spriteram;
    uint8_t flipscreen;
    uint8_t sprite_bank;
    uint8_t sprite_trails;
    uint8_t sprite_extra_planes;
    uint8_t sprite_pen_mask;
    uint32_t sprite_count;
    uint16_t control_0[8];
    uint16_t control_1[8];
    uint8_t tilemap_row_usage[32][8];
    uint8_t textram_row_usage[64];
    CanonicalTempsprite spritelist[1024];
};

struct CanonicalSceneSprite {
    int32_t x, y;
    uint16_t scale_x, scale_y;
    uint32_t tile;
    uint8_t palette;
    uint8_t flip_x, flip_y;
};

struct CanonicalSceneLayer {
    uint8_t priority;
    uint8_t blend_mode;
    uint8_t clip_enabled;
    uint8_t clip_inverted;
    uint8_t clip_inverse;
    uint8_t enabled;
    uint8_t blend_select;
    uint8_t mosaic;
};

struct CanonicalScenePlayfield {
    CanonicalSceneLayer layer;
    int32_t source_x;
    int32_t source_y;
    int32_t x_step;
    int32_t y_step;
    uint8_t y_fraction;
    uint16_t palette_add;
};

struct CanonicalSceneClip {
    int16_t left, right;
};

struct CanonicalSceneRow {
    CanonicalScenePlayfield playfields[4];
    CanonicalSceneLayer sprites[4];
    CanonicalSceneLayer text;
    CanonicalSceneClip clips[4];
    uint8_t blend[4];
    uint16_t background;
    int16_t text_x, text_y;
    uint8_t mosaic_period;
    uint8_t bitmap;
};

struct CanonicalLinePivot {
    uint16_t mix_value;
    uint8_t prio;
    uint8_t blend_mode;
    uint8_t clip_enable;
    uint8_t clip_inv;
    uint8_t clip_inv_mode;
    uint8_t blend_select_v;
    uint8_t x_sample_enable;
    uint8_t pivot_control;
    uint16_t pivot_enable;
};

struct CanonicalLineSprite {
    uint16_t mix_value;
    uint8_t prio;
    uint8_t blend_mode;
    uint8_t clip_enable;
    uint8_t clip_inv;
    uint8_t clip_inv_mode;
    uint8_t blend_select_v;
    uint8_t x_sample_enable;
};

struct CanonicalLinePlayfield {
    uint16_t mix_value;
    uint8_t prio;
    uint8_t blend_mode;
    uint8_t clip_enable;
    uint8_t clip_inv;
    uint8_t clip_inv_mode;
    uint8_t x_sample_enable;
    uint16_t colscroll;
    int32_t x_scale;
    int32_t y_scale;
    uint16_t pal_add;
    int32_t rowscroll;
};

struct CanonicalLineParams {
    CanonicalSceneClip clip[4];
    uint8_t blend[4];
    uint8_t x_sample;
    uint16_t bg_palette;
    CanonicalLinePivot pivot;
    CanonicalLineSprite sp[4];
    CanonicalLinePlayfield pf[4];
};

// GameVideo snapshot is now a fixed header plus the per-component payloads
// written by the component save_state functions. Playfield/text/sprite/line
// producers decode from video RAM at VBSTART, so the retired hook bookkeeping
// (tile validity, text references, per-component unsupported PCs, line control
// mirrors), the playfield tile payload and the text raw snapshot are gone: both
// are rebuilt from the serialized video RAM at the next VBSTART. GameLines
// still serializes its decoded line params + rows.
struct CanonicalGameVideoHeader {
    uint8_t rendered;
    uint32_t sprite_staging_count;
    uint32_t sprite_submitted_count;
    uint32_t sprite_current_count;
    uint8_t sprite_current_flipped;
    uint8_t sprite_current_pen_mask;
    uint8_t sprite_current_trails;
    uint8_t sprite_reg_flipped;
    uint8_t sprite_reg_pen_mask;
    uint8_t sprite_reg_trails;
    uint8_t sprite_reg_bank;
};

#pragma pack(pop)
inline void require_state(bool valid, std::source_location where = std::source_location::current()) {
    if (!valid) throw std::invalid_argument(std::string("Snapshot field outside canonical representation: ") +
        where.function_name() + ":" + std::to_string(where.line()));
}
template <> inline void validate_state_value(const CanonicalF3Cpu &s) {
    require_state(s.stopped <= 1 && s.halted <= 1 && s.cc_op <= 4 &&
                  (!s.cc_op || s.cc_width == 1 || s.cc_width == 2 || s.cc_width == 4));
}
template <> inline void validate_state_value(const CanonicalSoundOracle &s) {
    // An unstarted oracle context is zero-filled. Once initialized it is the
    // board's 68000, never a foreign Musashi CPU model or timing table.
    if (s.cpu_type == 0) {
        CanonicalSoundOracle cold{};
        cold.sound_needs_reset = 1;
        require_state(std::memcmp(&s, &cold, sizeof(s)) == 0);
        return;
    }
    require_state(s.cpu_type == 1 && s.address_mask == 0x00ffffff && s.sr_mask == 0xa71f &&
                  s.sound_needs_reset <= 1 && !(s.s_flag & ~4u) && !(s.m_flag & ~2u) &&
                  !(s.t1_flag & ~0x8000u) && !(s.t0_flag & ~0x4000u) &&
                  !(s.int_mask & ~0x700u) && !(s.int_level & ~0x700u) &&
                  !(s.stopped & ~3u) && s.ir <= 0xffff && s.virq_state <= 0xff &&
                  s.nmi_pending <= 1 && (s.instr_mode == 0 || s.instr_mode == 8) &&
                  s.run_mode <= 2 && !s.has_pmmu && !s.pmmu_enabled &&
                  (s.fpu_just_reset == 0 || s.fpu_just_reset == 1) && s.reset_cycles <= 132);
    // m68k_set_cpu_type(M68K_CPU_TYPE_68000), including unsigned -2 encodings.
    require_state(s.cyc_bcc_notake_b == uint32_t(-2) && s.cyc_bcc_notake_w == 2 &&
                  s.cyc_dbcc_f_noexp == uint32_t(-2) && s.cyc_dbcc_f_exp == 2 &&
                  s.cyc_scc_r_true == 2 && s.cyc_movem_w == 2 && s.cyc_movem_l == 3 &&
                  s.cyc_movem_store_w == 4 && s.cyc_movem_store_l == 8 &&
                  s.cyc_shift == 2 && s.cyc_reset == 132);
}
template <> inline void validate_state_value(const CanonicalSoundNative &s) {
    validate_state_value(s.cpu);
    require_state(s.needs_reset <= 1);
}
template <> inline void validate_state_value(const CanonicalEeprom &s) {
    require_state(s.mode <= 4 && s.selected <= 1 && s.old_clock <= 1 &&
                  s.data_out <= 1 && s.writable <= 1 && s.address < 64 &&
                  s.read_bit < 16 && s.count <= 16);
}
template <> inline void validate_state_value(const CanonicalAudioCore &s) {
    require_state(s.rb_count <= 32768 && s.gain_model <= 1 &&
                  s.reset_asserted <= 1 && s.esp_halted <= 1 &&
                  s.sample_accum < 16000000 && s.duart_accum < 4 &&
                  s.cpu_accum < 16000000 && s.cpu_accum >= -16000000LL * 1024);
}
template <> inline void validate_state_value(const CanonicalES5505 &s) {
    require_state(s.active_voices < 32 && s.current_page < 128 &&
                  s.voice_index >= 0 && s.voice_index < 32 &&
                  s.master_clock != 0 && s.sample_rate != 0 &&
                  s.sample_rate == s.master_clock / (16 * (unsigned(s.active_voices) + 1)));
    for (const auto &v : s.voices) require_state(v.index < 32);
}
template <> inline void validate_state_value(const CanonicalES5510Registers &s) {
    require_state(s.state <= 1 && s.halt_asserted <= 1 && s.memshift >= 0 &&
                  s.memshift <= 24 && s.dol_count >= 0 && s.dol_count <= 2 &&
                  s.alu.op < 16 && s.alu.src <= 3 && s.alu.dst <= 3 &&
                  s.mulacc.src <= 3 && s.mulacc.dst <= 3);
    require_state(s.ram.io <= 1 && s.ram.cycle <= 2 &&
                  s.ram_p.io <= 1 && s.ram_p.cycle <= 2 &&
                  s.ram_pp.io <= 1 && s.ram_pp.cycle <= 2);
}
template <> inline void validate_state_value(const CanonicalMB87078 &s) {
    for (auto index : s.gain_index) require_state(index < 67);
}
template <> inline void validate_state_value(const CanonicalMC68681 &s) {
    require_state(s.ct_running <= 1 && (!s.ct_running || s.ct_remaining != 0));
}
template <> inline void validate_state_value(const CanonicalVideo &s) {
    require_state(s.sprite_count <= 1024 && s.has_buffered_spriteram <= 1 &&
                  s.flipscreen <= 1 && s.sprite_bank <= 1 && s.sprite_trails <= 1 &&
                  s.sprite_extra_planes <= 3 && s.sprite_pen_mask <= 0x3f);
}
template <> inline void validate_state_value(const CanonicalSceneSprite &s) {
    require_state(s.flip_x <= 1 && s.flip_y <= 1);
}
inline void validate_scene_layer(const CanonicalSceneLayer &s) {
    require_state(s.priority <= 15 && s.blend_mode <= 3 &&
                  s.clip_enabled <= 15 && s.clip_inverted <= 15 &&
                  s.clip_inverse <= 1 && s.enabled <= 1 &&
                  s.blend_select <= 1 && s.mosaic <= 1);
}
template <> inline void validate_state_value(const CanonicalSceneRow &s) {
    for (const auto &p : s.playfields) validate_scene_layer(p.layer);
    for (const auto &p : s.sprites) validate_scene_layer(p);
    validate_scene_layer(s.text);
    require_state(s.bitmap <= 1);
}

} // namespace f3rt
