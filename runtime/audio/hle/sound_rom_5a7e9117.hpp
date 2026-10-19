#pragma once
#include "audio/hle/observe.hpp"
#include "audio/hle/rom_view.hpp"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace f3rt::sound_5a7e9117 {

constexpr uint32_t SOUND_ROM_CRC = 0x5a7e9117;

// ROM addresses (68000 bus, 0xc00000 .. 0xc80000)
constexpr uint32_t PROGRAM_DATA_BASE = 0xc00134;         // Program data bank 0
constexpr uint32_t BEND_RANGE_PTR = 0xc085f2;            // Pitch bend range startup parameter
constexpr uint32_t RATE_TABLE = 0xc089dc;                // Envelope / fade rate multipliers
constexpr uint32_t PHASE_RATE_TABLE = 0xc08aa4;          // LFO phase rate table
constexpr uint32_t PAN_TABLE = 0xc08b6c;                 // Stereo pan table (128 entries)
constexpr uint32_t ATTENUATION_TABLE = 0xc08bec;         // Attenuation curve (128 entries)
constexpr uint32_t VELOCITY_CURVE_TABLE = 0xc08c6c;      // Velocity response curves (512 entries)
constexpr uint32_t FILTER_TABLE = 0xc08e6c;              // Filter coefficient table
constexpr uint32_t PITCH_TABLE = 0xc09e6c;               // Pitch frequency table
constexpr uint32_t PITCH_MOD_TABLE = 0xc0a46c;           // Pitch modulation multipliers
constexpr uint32_t SPLIT_MODE_TABLE = 0xc0a734;          // Sample split mode control table
constexpr uint32_t LFO_WAVE_TABLE = 0xc0a73c;            // LFO waveform table
constexpr uint32_t CONTROLLER_DEFAULTS_TABLE = 0xc13556; // Channel controller defaults
constexpr uint32_t HEAP_BASE_PTR = 0xc15fb6;             // Sample RAM heap base pointer
constexpr uint32_t HEAP_LIMIT_PTR_A = 0xc15fba;          // Sample RAM heap limit pointer word A
constexpr uint32_t HEAP_LIMIT_PTR_B = 0xc15fc0;          // Sample RAM heap limit pointer word B
constexpr uint32_t HEAP_RESERVE_PTR = 0xc15fcc;          // Sample RAM heap reserve size pointer
constexpr uint32_t HEAP_NEXT_PTR = 0xc15fd8;             // Sample RAM heap next allocation pointer
constexpr uint32_t NOISE_SEED_PTR = 0xc16f04;            // Random noise generator seed
constexpr uint32_t NOISE_HISTORY_PTR = 0xc16f0a;         // Random noise generator history
constexpr uint32_t RESET_START_PTR = 0xc17b60;           // Voice reset start pointer
constexpr uint32_t RESET_END_PTR = 0xc17b64;             // Voice reset end pointer
constexpr uint32_t BUDGET_TABLE = 0xc1a56e;              // Voice processing budget table
constexpr uint32_t SPLIT_TABLE_BASE = 0xc1ae76;          // Sample split key table base
constexpr uint32_t SPECIAL_SPLIT_TABLE = 0xc1ae84;       // Special sample split table
constexpr uint32_t SPLIT_DIRECTORY = 0xc1c1e0;           // Sample split directory
constexpr uint32_t SEQ_STREAM_BASE = 0xc20000;           // Sequence data streams base in ROM

// Sound ROM relative offsets (within 0x80000 sound image)
constexpr uint32_t VOLUME_PAN_TABLE_OFFSET =
    0xa634; // Volume/pan attenuation table offset (0xc0a634)
constexpr uint32_t PRESET_TABLE_OFFSET =
    0x1c2d2; // Effects preset parameter table offset (0xc1c2d2)

// Sequencer / Voice parameter record offsets
constexpr uint32_t VOICE_OWNER_OFFSET = 0x0a;     // Base offset for controller parameters in voice
constexpr uint32_t PAN_CONTROLLER_OFFSET = 0x1c;  // Pan controller offset in channel record
constexpr uint32_t HEAP_METADATA_OFFSET = 0x6e9c; // Metadata offset for heap header

// Sequence header offsets and sizes
constexpr uint32_t SEQ_HEADER_SIZE = 0xa8;
constexpr uint32_t SEQ_HEADER_BARS_OFFSET = 0x12;
constexpr uint32_t SEQ_HEADER_TIME_SIG_FACTOR_OFFSET = 0x14;
constexpr uint32_t SEQ_HEADER_TIME_SIG_EXP_OFFSET = 0x15;
constexpr uint32_t SEQ_HEADER_LOOP_START_OFFSET = 0x16;
constexpr uint32_t SEQ_HEADER_LOOP_END_OFFSET = 0x1a;
constexpr uint32_t SEQ_HEADER_RATE_OFFSET = 0x1e;
constexpr uint32_t SEQ_HEADER_TRACKS_OFFSET = 0x20;
constexpr size_t SEQ_TRACK_RECORD_SIZE = 14;
constexpr uint32_t SEQ_HEADER_LINKED_OFFSET = 0xa6;

// Channel parameter offsets
constexpr size_t CHANNEL_PARAM_EXPRESSION = 0x14;
constexpr size_t CHANNEL_PARAM_VOLUME = 0x20;
constexpr size_t CHANNEL_PARAM_PITCH = 0x22;
constexpr size_t CHANNEL_PARAM_PAN = 0x26;
constexpr size_t CHANNEL_PARAM_VOLUME_RECORD = 0x2c;
constexpr size_t CHANNEL_PARAM_PAN_RECORD = 0x2d;
constexpr size_t CHANNEL_PARAM_TRANSPOSE = 0x35;

// Program data record layout
constexpr size_t PROGRAM_RECORD_STRIDE = 0x198;
constexpr uint32_t PROGRAM_COLUMN_STRIDE = 0x3fc0;
constexpr size_t PROGRAM_ENABLED_OFFSET = 0x196;
constexpr size_t PROGRAM_BANK_MASK_OFFSET = 0x154;
constexpr size_t PROGRAM_MAGIC_OFFSET_0 = 0x15c;
constexpr size_t PROGRAM_MAGIC_OFFSET_1 = 0x15e;
constexpr size_t PROGRAM_DEFAULT_BANK_OFFSET = 0x160;
constexpr size_t PROGRAM_GLIDE_OFFSET = 0x17e;
constexpr size_t SPLIT_RECORD_STRIDE = 0x14;

// Sample ROM layout and offsets
constexpr size_t SAMPLE_BANK_SIZE = 0x100000;
constexpr size_t SAMPLE_HALF_BANK_SIZE = 0x80000;
constexpr uint32_t SAMPLE_TAG_PROBE_OFFSET = 0x7ffff;
constexpr uint32_t REVERSE_START_OFFSET = 0x800;

// Sound Driver Routine ROM PCs
constexpr uint32_t SND_PC_DIRECT_NOTE = 0xc130f0;

// Direct region record layout (20 bytes per region, 17 regions in a direct program)
constexpr size_t DIRECT_REGION_STRIDE = 20;
constexpr size_t DIRECT_REGION_COUNT = 17;
constexpr size_t DIRECT_PARAM_DECAY = 0x00;
constexpr size_t DIRECT_PARAM_RELEASE = 0x02;
constexpr size_t DIRECT_PARAM_SAMPLE_ID = 0x04;
constexpr size_t DIRECT_PARAM_TRANSPOSE = 0x06;
constexpr size_t DIRECT_PARAM_SENSITIVITY = 0x08;
constexpr size_t DIRECT_PARAM_KEY_LOW = 0x0a;
constexpr size_t DIRECT_PARAM_KEY_HIGH = 0x0c;
constexpr size_t DIRECT_PARAM_FILTER_INDEX = 0x0e;
constexpr size_t DIRECT_PARAM_LEVEL = 0x10;
constexpr size_t DIRECT_PARAM_PAN_CHOICE = 0x12;

// Layer record layout (0x76 = 118 bytes per layer, 3 layers in a layered program)
constexpr size_t LAYER_RECORD_STRIDE = 0x76;
constexpr size_t LAYER_RECORD_COUNT = 3;
constexpr size_t LAYER_ENV_PITCH_OFFSET = 0x00;
constexpr size_t LAYER_ENV_FILTER_OFFSET = 0x14; // +20
constexpr size_t LAYER_ENV_AMP_OFFSET = 0x28;    // +40
constexpr size_t LAYER_PARAM_BASE_PITCH = 0x3c;
constexpr size_t LAYER_PARAM_BASE_FINE = 0x3e;
constexpr size_t LAYER_PARAM_PITCH_ENV_DEPTH = 0x40;
constexpr size_t LAYER_PARAM_LFO_PITCH_DEPTH = 0x42;
constexpr size_t LAYER_PARAM_FLAGS = 0x44;
constexpr size_t LAYER_PARAM_PITCH_SRC_DEPTH = 0x46;
constexpr size_t LAYER_PARAM_K1_BASE = 0x48;
constexpr size_t LAYER_PARAM_K1_KEY = 0x4a;
constexpr size_t LAYER_PARAM_K1_ENV = 0x4c;
constexpr size_t LAYER_PARAM_FILTER_SRC = 0x4e;
constexpr size_t LAYER_PARAM_FILTER_SRC_DEPTH = 0x50;
constexpr size_t LAYER_PARAM_K2_BASE = 0x52;
constexpr size_t LAYER_PARAM_K2_KEY = 0x54;
constexpr size_t LAYER_PARAM_K2_ENV = 0x56;
constexpr size_t LAYER_PARAM_KEY_WEIGHT = 0x58;
constexpr size_t LAYER_PARAM_KEY_LOW = 0x5a;
constexpr size_t LAYER_PARAM_KEY_HIGH = 0x5c;
constexpr size_t LAYER_PARAM_LEVEL = 0x5e;
constexpr size_t LAYER_PARAM_AMP_SRC = 0x60;
constexpr size_t LAYER_PARAM_AMP_SRC_DEPTH = 0x62;
constexpr size_t LAYER_PARAM_LFO_SRC = 0x64;
constexpr size_t LAYER_PARAM_LFO_DEPTH = 0x66;
constexpr size_t LAYER_PARAM_LFO_RATE = 0x68;
constexpr size_t LAYER_PARAM_LFO_WAVE = 0x6a;
constexpr size_t LAYER_PARAM_SAMPLE_ID = 0x6c;
constexpr size_t LAYER_PARAM_GATE_SRC = 0x6e;
constexpr size_t LAYER_PARAM_START_AMOUNT = 0x70;
constexpr size_t LAYER_PARAM_START_OFFSET = 0x72;
constexpr size_t LAYER_PARAM_DELAY_TICKS = 0x74;

struct Split {
    int16_t tuning = 0;
    uint32_t accumulator = 0, start = 0, end = 0;
    uint8_t cutoff = 0, mode = 0;
    static constexpr Split read(const uint8_t *p) noexcept {
        return {
            static_cast<int16_t>((uint16_t(p[0]) << 8) | p[1]),
            (uint32_t(p[2]) << 24) | (uint32_t(p[3]) << 16) | (uint32_t(p[4]) << 8) | p[5],
            (uint32_t(p[6]) << 24) | (uint32_t(p[7]) << 16) | (uint32_t(p[8]) << 8) | p[9],
            (uint32_t(p[10]) << 24) | (uint32_t(p[11]) << 16) | (uint32_t(p[12]) << 8) | p[13],
            p[5],
            p[9]
        };
    }
};

struct Bank {
    uint8_t tag = 0, count = 0;
    uint32_t offset = 0, probe = 0;
};

struct ProgramData {
    const uint8_t *base = nullptr;
    std::array<int8_t, 17> sample_banks{};
    uint8_t enabled = 0;
};

constexpr unsigned program_slot(uint16_t id) noexcept {
    return (((id >> 14) & 3) * 256) | (id & 0xff);
}

inline f3rt::hle::EnvelopeInfo decode_envelope(const uint8_t *p) noexcept {
    f3rt::hle::EnvelopeInfo env{};
    for (unsigned i = 0; i < 4; ++i) {
        env.levels[i] = p[i * 4] & 127;
        env.rates[i] = p[i * 4 + 2] & 127;
    }
    env.key_rate = int8_t(int8_t((p[16] & 15) << 4) >> 4);
    env.velocity_curve = (p[16] >> 4) & 3;
    env.one_shot = (p[16] & 0x40) != 0;
    env.loop = (p[16] & 0x80) != 0;
    env.vel_level = p[18] >> 4;
    env.vel_attack = p[18] & 15;
    return env;
}

inline std::optional<f3rt::hle::ProgramInfo> describe_program(
    const f3rt::hle::RomView &rom,
    std::span<const uint8_t> metadata,
    std::span<const Bank> banks,
    std::span<const ProgramData> programs,
    unsigned board_bank_mask,
    uint16_t id)
{
    using namespace f3rt::hle;
    if (id == 0xffff) return std::nullopt;
    const unsigned slot = program_slot(id);
    if (slot >= programs.size()) return std::nullopt;
    const auto &entry = programs[slot];
    if (!entry.base) return std::nullopt;
    const uint8_t *prog = entry.base;

    auto get_splits = [&](unsigned sample_id, int bank, bool reverse_sample) {
        std::vector<SplitInfo> splits;
        if (sample_id >= 0x78) return splits;
        uint8_t selector = 0;
        const uint8_t *data = nullptr;
        size_t available = 0;
        if (bank >= 0 && unsigned(bank) < banks.size() && banks[unsigned(bank)].count) {
            const Bank &b = banks[unsigned(bank)];
            selector = uint8_t(bank);
            if (sample_id >= b.count) return splits;
            const size_t off = b.offset + 2 + size_t(sample_id) * 14;
            if (off + 2 > metadata.size()) return splits;
            int16_t word_offset = static_cast<int16_t>((uint16_t(metadata[off]) << 8) | metadata[off + 1]);
            size_t offset = b.offset + size_t(word_offset);
            if (offset + 14 > metadata.size()) return splits;
            data = metadata.data() + offset;
            available = metadata.size() - offset;
        } else {
            selector = uint8_t(board_bank_mask * 2);
            if (sample_id > 120) return splits;
            const uint32_t dir_addr = uint32_t(SPLIT_DIRECTORY + sample_id * 2);
            int16_t word_dir = static_cast<int16_t>(rom.read_u16(dir_addr));
            uint32_t address = uint32_t(SPLIT_TABLE_BASE + word_dir);
            if (address < SOUND_ROM_BASE || address + 14 > SOUND_ROM_END) return splits;
            data = rom.pointer(address, 14);
            available = rom.size() - size_t(data - rom.data());
        }
        uint32_t bit = uint32_t(selector & 1) << 28;
        unsigned board = ((selector >> 1) & board_bank_mask) << 20;
        uint8_t prev_cutoff = 0;
        for (size_t i = 0; i + 14 <= available; i += 14) {
            Split s = Split::read(data + i);
            SplitInfo sp;
            sp.key_low = prev_cutoff;
            sp.key_high = s.cutoff;
            prev_cutoff = uint8_t(std::min<int>(255, int(s.cutoff) + 1));
            sp.tuning = s.tuning;
            sp.loop_mode = s.mode;
            sp.accumulator = s.accumulator;
            sp.start = s.start;
            sp.end = s.end;
            uint8_t control = rom.read_byte(SPLIT_MODE_TABLE + s.mode);
            sp.reverse = reverse_sample;
            if (sp.reverse) control = 0x43;
            sp.loop = (control & 8) != 0;
            sp.bidirectional = (control & 16) != 0;
            uint32_t start_addr = sp.reverse ? s.accumulator : s.start;
            uint32_t phase_addr = sp.reverse ? s.end : s.accumulator;
            uint32_t end_addr = s.end;
            sp.start_word = uint32_t(double(board) + double((start_addr | bit) & 0x1fffffe0) / 512);
            sp.end_word = uint32_t(double(board) + double((end_addr | bit) & 0x1fffffe0) / 512);
            sp.phase_word = uint32_t(double(board) + double((phase_addr | bit) & 0x1fffffff) / 512);
            splits.push_back(sp);
            if (s.cutoff >= 127) break;
        }
        return splits;
    };

    ProgramInfo info{};
    info.id = id;
    info.enabled_mask = prog[PROGRAM_ENABLED_OFFSET] >> 5;
    info.glide_rate = prog[PROGRAM_GLIDE_OFFSET];
    if (!info.enabled_mask) {
        info.kind = ProgramInfo::Kind::Direct;
        info.regions.reserve(DIRECT_REGION_COUNT);
        for (unsigned idx = 0; idx < DIRECT_REGION_COUNT; ++idx) {
            const uint8_t *p = prog + size_t(idx) * DIRECT_REGION_STRIDE;
            DirectRegionInfo r{};
            r.index = uint8_t(idx);
            std::copy_n(p, DIRECT_REGION_STRIDE, r.raw.data());
            r.decay_rate = p[DIRECT_PARAM_DECAY] & 127;
            r.ignore_note_off = (p[DIRECT_PARAM_DECAY] & 128) != 0;
            r.release_rate = p[DIRECT_PARAM_RELEASE] & 127;
            r.root_key_relative = (p[DIRECT_PARAM_RELEASE] & 128) != 0;
            r.sample_id = p[DIRECT_PARAM_SAMPLE_ID] & 127;
            r.reverse = (p[DIRECT_PARAM_SAMPLE_ID] & 128) != 0;
            r.transposition = int8_t(p[DIRECT_PARAM_TRANSPOSE]);
            r.fine_pitch = p[DIRECT_PARAM_SENSITIVITY] & 15;
            r.sensitivity = p[DIRECT_PARAM_SENSITIVITY] >> 4;
            r.key_low = int8_t(p[DIRECT_PARAM_KEY_LOW]);
            r.key_high = int8_t(p[DIRECT_PARAM_KEY_HIGH]);
            r.filter_index = p[DIRECT_PARAM_FILTER_INDEX];
            r.level = p[DIRECT_PARAM_LEVEL] & 127;
            r.level_plus_168 = (p[DIRECT_PARAM_LEVEL] & 128) != 0;
            r.pan = p[DIRECT_PARAM_PAN_CHOICE] & 15;
            r.pair_choice = (p[DIRECT_PARAM_PAN_CHOICE] >> 4) & 3;
            r.velocity_curve = p[DIRECT_PARAM_PAN_CHOICE] >> 6;
            int bank = static_cast<int>(entry.sample_banks[idx]);
            r.splits = get_splits(r.sample_id, bank, r.reverse);
            info.regions.push_back(r);
        }
    } else {
        info.kind = ProgramInfo::Kind::Layered;
        for (unsigned layer_idx = 0; layer_idx < LAYER_RECORD_COUNT; ++layer_idx) {
            LayerInfo &layer = info.layers[layer_idx];
            layer.index = uint8_t(layer_idx);
            layer.enabled = (info.enabled_mask & (1u << layer_idx)) != 0;
            const uint8_t *patch = prog + size_t(layer_idx) * LAYER_RECORD_STRIDE;
            std::copy_n(patch, LAYER_RECORD_STRIDE, layer.raw.data());
            for (unsigned e = 0; e < 3; ++e) {
                layer.envelope[e] = decode_envelope(patch + size_t(e) * 20);
            }
            layer.base_pitch = patch[LAYER_PARAM_BASE_PITCH];
            layer.base_fine = int8_t(patch[LAYER_PARAM_BASE_FINE]);
            layer.pitch_envelope_depth = int8_t(patch[LAYER_PARAM_PITCH_ENV_DEPTH]);
            layer.pitch_envelope_offset = patch[0x0c] & 127;
            layer.lfo_pitch_depth = int8_t(patch[LAYER_PARAM_LFO_PITCH_DEPTH]);
            layer.pitch_source = patch[LAYER_PARAM_FLAGS] & 15;
            layer.allocation_mode = (patch[LAYER_PARAM_FLAGS] >> 4) & 3;
            layer.filter_mode = patch[LAYER_PARAM_FLAGS] >> 6;
            layer.k2_bank = (patch[LAYER_PARAM_FLAGS] & 128) != 0;
            layer.pitch_source_depth = int8_t(patch[LAYER_PARAM_PITCH_SRC_DEPTH]);
            layer.use_played_key = (patch[LAYER_PARAM_K1_BASE] & 128) != 0;
            layer.k1_base = patch[LAYER_PARAM_K1_BASE] & 127;
            layer.k1_key = int8_t(patch[LAYER_PARAM_K1_KEY]);
            layer.k1_envelope = int8_t(patch[LAYER_PARAM_K1_ENV]);
            layer.filter_source = patch[LAYER_PARAM_FILTER_SRC] & 15;
            layer.default_pan = patch[LAYER_PARAM_FILTER_SRC] & 0xf0;
            layer.filter_source_depth = int8_t(patch[LAYER_PARAM_FILTER_SRC_DEPTH]);
            layer.k2_apply_source = (patch[LAYER_PARAM_K2_BASE] & 128) != 0;
            layer.k2_base = patch[LAYER_PARAM_K2_BASE] & 127;
            layer.k2_key = int8_t(patch[LAYER_PARAM_K2_KEY]);
            layer.k2_envelope = int8_t(patch[LAYER_PARAM_K2_ENV]);
            layer.weight_amount = int8_t(patch[LAYER_PARAM_KEY_WEIGHT]);
            layer.weight_low = patch[LAYER_PARAM_KEY_LOW];
            layer.weight_high = patch[LAYER_PARAM_KEY_HIGH];
            layer.level = patch[LAYER_PARAM_LEVEL] & 127;
            layer.level_plus_43 = (patch[LAYER_PARAM_LEVEL] & 128) != 0;
            layer.amplitude_source = patch[LAYER_PARAM_AMP_SRC] & 15;
            layer.pair_choice = (patch[LAYER_PARAM_AMP_SRC] >> 4) & 3;
            layer.priority = patch[LAYER_PARAM_AMP_SRC] & 0xc0;
            layer.amplitude_source_depth = int8_t(patch[LAYER_PARAM_AMP_SRC_DEPTH]);
            layer.lfo_source = patch[LAYER_PARAM_LFO_SRC] & 15;
            layer.random_period = patch[LAYER_PARAM_LFO_SRC] >> 4;
            layer.lfo_depth = patch[LAYER_PARAM_LFO_DEPTH] & 127;
            layer.lfo_phase_reset = (patch[LAYER_PARAM_LFO_DEPTH] & 128) != 0;
            layer.lfo_rate_index = patch[LAYER_PARAM_LFO_RATE];
            layer.lfo_wave = patch[LAYER_PARAM_LFO_WAVE] & 15;
            layer.lfo_fade_rate = patch[LAYER_PARAM_LFO_WAVE] >> 4;
            layer.sample_id = patch[LAYER_PARAM_SAMPLE_ID] & 127;
            layer.reverse = (patch[LAYER_PARAM_SAMPLE_ID] & 128) != 0;
            layer.velocity_gate = int8_t(patch[LAYER_PARAM_GATE_SRC]);
            layer.start_source = patch[LAYER_PARAM_GATE_SRC] & 15;
            layer.start_amount = int8_t(patch[LAYER_PARAM_START_AMOUNT]);
            layer.start_offset = patch[LAYER_PARAM_START_OFFSET];
            layer.delay_ticks = patch[LAYER_PARAM_DELAY_TICKS];
            int bank = static_cast<int>(entry.sample_banks[layer_idx]);
            layer.splits = get_splits(layer.sample_id, bank, layer.reverse);
        }
    }
    return info;
}

} // namespace f3rt::sound_5a7e9117
