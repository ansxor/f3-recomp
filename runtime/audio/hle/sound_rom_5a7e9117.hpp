#pragma once
#include <cstddef>
#include <cstdint>

namespace f3rt::sound_5a7e9117 {

constexpr uint32_t SOUND_ROM_CRC = 0x5a7e9117;

// ROM addresses (68000 bus, 0xc00000 .. 0xc80000)
constexpr uint32_t PROGRAM_DATA_BASE = 0xc00134;      // Program data bank 0
constexpr uint32_t BEND_RANGE_PTR = 0xc085f2;         // Pitch bend range startup parameter
constexpr uint32_t RATE_TABLE = 0xc089dc;             // Envelope / fade rate multipliers
constexpr uint32_t PHASE_RATE_TABLE = 0xc08aa4;       // LFO phase rate table
constexpr uint32_t PAN_TABLE = 0xc08b6c;              // Stereo pan table (128 entries)
constexpr uint32_t ATTENUATION_TABLE = 0xc08bec;      // Attenuation curve (128 entries)
constexpr uint32_t VELOCITY_CURVE_TABLE = 0xc08c6c;   // Velocity response curves (512 entries)
constexpr uint32_t FILTER_TABLE = 0xc08e6c;           // Filter coefficient table
constexpr uint32_t PITCH_TABLE = 0xc09e6c;            // Pitch frequency table
constexpr uint32_t PITCH_MOD_TABLE = 0xc0a46c;        // Pitch modulation multipliers
constexpr uint32_t SPLIT_MODE_TABLE = 0xc0a734;       // Sample split mode control table
constexpr uint32_t LFO_WAVE_TABLE = 0xc0a73c;         // LFO waveform table
constexpr uint32_t CONTROLLER_DEFAULTS_TABLE = 0xc13556; // Channel controller defaults
constexpr uint32_t HEAP_BASE_PTR = 0xc15fb6;          // Sample RAM heap base pointer
constexpr uint32_t HEAP_LIMIT_PTR_A = 0xc15fba;       // Sample RAM heap limit pointer word A
constexpr uint32_t HEAP_LIMIT_PTR_B = 0xc15fc0;       // Sample RAM heap limit pointer word B
constexpr uint32_t HEAP_RESERVE_PTR = 0xc15fcc;       // Sample RAM heap reserve size pointer
constexpr uint32_t HEAP_NEXT_PTR = 0xc15fd8;          // Sample RAM heap next allocation pointer
constexpr uint32_t NOISE_SEED_PTR = 0xc16f04;         // Random noise generator seed
constexpr uint32_t NOISE_HISTORY_PTR = 0xc16f0a;      // Random noise generator history
constexpr uint32_t RESET_START_PTR = 0xc17b60;        // Voice reset start pointer
constexpr uint32_t RESET_END_PTR = 0xc17b64;          // Voice reset end pointer
constexpr uint32_t BUDGET_TABLE = 0xc1a56e;           // Voice processing budget table
constexpr uint32_t SPLIT_TABLE_BASE = 0xc1ae76;       // Sample split key table base
constexpr uint32_t SPECIAL_SPLIT_TABLE = 0xc1ae84;    // Special sample split table
constexpr uint32_t SPLIT_DIRECTORY = 0xc1c1e0;        // Sample split directory
constexpr uint32_t SEQ_STREAM_BASE = 0xc20000;        // Sequence data streams base in ROM

// Sound ROM relative offsets (within 0x80000 sound image)
constexpr uint32_t VOLUME_PAN_TABLE_OFFSET = 0xa634;  // Volume/pan attenuation table offset (0xc0a634)
constexpr uint32_t PRESET_TABLE_OFFSET = 0x1c2d2;     // Effects preset parameter table offset (0xc1c2d2)

// Sequencer / Voice parameter record offsets
constexpr uint32_t VOICE_OWNER_OFFSET = 0x0a;         // Base offset for controller parameters in voice
constexpr uint32_t PAN_CONTROLLER_OFFSET = 0x1c;      // Pan controller offset in channel record
constexpr uint32_t HEAP_METADATA_OFFSET = 0x6e9c;     // Metadata offset for heap header

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

} // namespace f3rt::sound_5a7e9117
