#pragma once
#include "audio/hle/patch.hpp"
#include <array>
#include <cstddef>
#include <cstdint>

namespace f3rt::sound_5a7e9117 {

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
constexpr uint32_t CONTROLLER_DEFAULTS_TABLE = 0xc13556; // Track controller defaults
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
        return {static_cast<int16_t>((uint16_t(p[0]) << 8) | p[1]),
                (uint32_t(p[2]) << 24) | (uint32_t(p[3]) << 16) | (uint32_t(p[4]) << 8) | p[5],
                (uint32_t(p[6]) << 24) | (uint32_t(p[7]) << 16) | (uint32_t(p[8]) << 8) | p[9],
                (uint32_t(p[10]) << 24) | (uint32_t(p[11]) << 16) | (uint32_t(p[12]) << 8) | p[13],
                p[5],
                p[9]};
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

namespace {
using hle_patch = f3rt::hle::Layer::At;
using hle_program = f3rt::hle::Program::At;
using hle_direct = f3rt::hle::DirectPatch::At;
static_assert(hle_patch::size == LAYER_RECORD_STRIDE);
static_assert(hle_patch::pitch_envelope == LAYER_ENV_PITCH_OFFSET);
static_assert(hle_patch::filter_envelope == LAYER_ENV_FILTER_OFFSET);
static_assert(hle_patch::amplitude_envelope == LAYER_ENV_AMP_OFFSET);
static_assert(hle_patch::coarse_pitch == LAYER_PARAM_BASE_PITCH);
static_assert(hle_patch::fine_pitch == LAYER_PARAM_BASE_FINE);
static_assert(hle_patch::pitch_envelope_depth == LAYER_PARAM_PITCH_ENV_DEPTH);
static_assert(hle_patch::lfo_pitch_depth == LAYER_PARAM_LFO_PITCH_DEPTH);
static_assert(hle_patch::flags == LAYER_PARAM_FLAGS);
static_assert(hle_patch::pitch_source_depth == LAYER_PARAM_PITCH_SRC_DEPTH);
static_assert(hle_patch::k1_base == LAYER_PARAM_K1_BASE);
static_assert(hle_patch::k1_key == LAYER_PARAM_K1_KEY);
static_assert(hle_patch::k1_envelope == LAYER_PARAM_K1_ENV);
static_assert(hle_patch::filter_source == LAYER_PARAM_FILTER_SRC);
static_assert(hle_patch::filter_source_depth == LAYER_PARAM_FILTER_SRC_DEPTH);
static_assert(hle_patch::k2_base == LAYER_PARAM_K2_BASE);
static_assert(hle_patch::k2_key == LAYER_PARAM_K2_KEY);
static_assert(hle_patch::k2_envelope == LAYER_PARAM_K2_ENV);
static_assert(hle_patch::weight_amount == LAYER_PARAM_KEY_WEIGHT);
static_assert(hle_patch::weight_low == LAYER_PARAM_KEY_LOW);
static_assert(hle_patch::weight_high == LAYER_PARAM_KEY_HIGH);
static_assert(hle_patch::level == LAYER_PARAM_LEVEL);
static_assert(hle_patch::amplitude_source == LAYER_PARAM_AMP_SRC);
static_assert(hle_patch::amplitude_source_depth == LAYER_PARAM_AMP_SRC_DEPTH);
static_assert(hle_patch::lfo_source == LAYER_PARAM_LFO_SRC);
static_assert(hle_patch::lfo_depth == LAYER_PARAM_LFO_DEPTH);
static_assert(hle_patch::lfo_rate == LAYER_PARAM_LFO_RATE);
static_assert(hle_patch::lfo_wave == LAYER_PARAM_LFO_WAVE);
static_assert(hle_patch::sample_id == LAYER_PARAM_SAMPLE_ID);
static_assert(hle_patch::gate == LAYER_PARAM_GATE_SRC);
static_assert(hle_patch::start_amount == LAYER_PARAM_START_AMOUNT);
static_assert(hle_patch::start_offset == LAYER_PARAM_START_OFFSET);
static_assert(hle_patch::delay == LAYER_PARAM_DELAY_TICKS);
static_assert(hle_direct::size == DIRECT_REGION_STRIDE);
static_assert(hle_direct::decay == DIRECT_PARAM_DECAY);
static_assert(hle_direct::release == DIRECT_PARAM_RELEASE);
static_assert(hle_direct::sample_id == DIRECT_PARAM_SAMPLE_ID);
static_assert(hle_direct::transpose == DIRECT_PARAM_TRANSPOSE);
static_assert(hle_direct::sensitivity == DIRECT_PARAM_SENSITIVITY);
static_assert(hle_direct::key_low == DIRECT_PARAM_KEY_LOW);
static_assert(hle_direct::key_high == DIRECT_PARAM_KEY_HIGH);
static_assert(hle_direct::filter_index == DIRECT_PARAM_FILTER_INDEX);
static_assert(hle_direct::level == DIRECT_PARAM_LEVEL);
static_assert(hle_direct::pan == DIRECT_PARAM_PAN_CHOICE);
static_assert(hle_program::size == PROGRAM_RECORD_STRIDE);
static_assert(hle_program::glide == PROGRAM_GLIDE_OFFSET);
static_assert(hle_program::enabled == PROGRAM_ENABLED_OFFSET);
static_assert(hle_program::layer_count == LAYER_RECORD_COUNT);
static_assert(hle_program::region_count == DIRECT_REGION_COUNT);
} // namespace

} // namespace f3rt::sound_5a7e9117
