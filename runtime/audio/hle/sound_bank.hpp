#pragma once
#include "audio/hle/patch.hpp"
#include "audio/hle/rom_view.hpp"
#include "audio/hle/sound_rom_5a7e9117.hpp"
#include "f3rt/bytes.hpp"
#include "f3rt/memory_map.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

namespace f3rt::hle {

using f3rt::s16;
using f3rt::s8;
using sound_5a7e9117::Bank;
using sound_5a7e9117::program_slot;
using sound_5a7e9117::ProgramData;
using sound_5a7e9117::Split;

class SoundBank {
public:
    SoundBank(const RomView &rom_view, std::span<const uint16_t> sample_words)
        : rom(rom_view), samples(sample_words) {
        if (rom.size() < SOUND_ROM_SIZE || samples.size() < SOUND_ROM_SIZE ||
            !std::has_single_bit(samples.size())) {
            throw std::runtime_error(
                "HLE requires mapped sound and power-of-two sample ROM regions");
        }
        board_bank_mask = (samples.size() >= sound_5a7e9117::SAMPLE_BANK_SIZE)
                              ? unsigned((samples.size() / sound_5a7e9117::SAMPLE_BANK_SIZE) - 1)
                              : 0;
        metadata.reserve(0x10000);
        decode_banks();

        uint32_t heap_base = rom.read_u16(sound_5a7e9117::HEAP_BASE_PTR);
        uint32_t heap_limit = rom.read_u32(sound_5a7e9117::HEAP_LIMIT_PTR_A) +
                              rom.read_u32(sound_5a7e9117::HEAP_LIMIT_PTR_B);
        uint32_t heap_size =
            heap_limit - heap_base - rom.read_u32(sound_5a7e9117::HEAP_RESERVE_PTR);
        uint32_t heap_next = rom.read_u32(sound_5a7e9117::HEAP_NEXT_PTR);
        if (heap_base >= sound_5a7e9117::HEAP_METADATA_OFFSET &&
            heap_base - sound_5a7e9117::HEAP_METADATA_OFFSET + 8 <= metadata.size()) {
            for (unsigned i = 0; i < 4; ++i) {
                metadata[heap_base - sound_5a7e9117::HEAP_METADATA_OFFSET + i] =
                    uint8_t(heap_size >> (24 - i * 8));
                metadata[heap_base - sound_5a7e9117::HEAP_METADATA_OFFSET + 4 + i] =
                    uint8_t(heap_next >> (24 - i * 8));
            }
        }
        cache_programs();
        for (unsigned i = 0; i < rates.size(); ++i)
            rates[i] = rom.read_u16(sound_5a7e9117::RATE_TABLE + 2 * i);
        for (unsigned i = 0; i < phase_rates.size(); ++i)
            phase_rates[i] = rom.read_u16(sound_5a7e9117::PHASE_RATE_TABLE + 2 * i);
        for (unsigned i = 0; i < filters.size(); ++i)
            filters[i] = rom.read_u16(sound_5a7e9117::FILTER_TABLE + 2 * i);
        for (unsigned i = 0; i < pitches.size(); ++i)
            pitches[i] = rom.read_u16(sound_5a7e9117::PITCH_TABLE + 2 * i);
        bend_range = uint8_t(rom.read_u16(sound_5a7e9117::BEND_RANGE_PTR));
        reset_start = ((uint32_t(rom.read_u16(sound_5a7e9117::RESET_START_PTR)) << 16) |
                       rom.read_u16(sound_5a7e9117::RESET_START_PTR + 2)) &
                      0x1fffffe0;
        reset_end = ((uint32_t(rom.read_u16(sound_5a7e9117::RESET_END_PTR)) << 16) |
                     rom.read_u16(sound_5a7e9117::RESET_END_PTR + 2)) &
                    0x1fffffe0;
        for (unsigned i = 0; i < 128; ++i) {
            attenuation[i] = rom.read_byte(sound_5a7e9117::ATTENUATION_TABLE + i);
            pan[i] = rom.read_byte(sound_5a7e9117::PAN_TABLE + i);
        }
        for (unsigned i = 0; i < 512; ++i)
            velocity_curves[i] = rom.read_byte(sound_5a7e9117::VELOCITY_CURVE_TABLE + i);
        for (unsigned i = 0; i < 12; ++i)
            budgets[i] = rom.read_byte(sound_5a7e9117::BUDGET_TABLE + i);
    }

    void set_bank(uint8_t bank) noexcept { rom.set_bank(bank); }

    const ProgramData &program_data(unsigned slot) const noexcept { return programs[slot]; }

    Split split_for(Layer patch, unsigned layer, int pitch, uint8_t &selector,
                    uint16_t program_id) const {
        if (patch.sample().table()) {
            selector = uint8_t(board_bank_mask * 2);
            return Split{};
        }
        return split_lookup(patch.sample_id(), layer, pitch, selector, program_id);
    }

    Split split_for(DirectRegion patch, unsigned layer, int pitch, uint8_t &selector,
                    uint16_t program_id) const {
        return split_lookup(patch.sample_id(), layer, pitch, selector, program_id);
    }

    int key_weight(Layer patch, int key) const noexcept {
        int amount = patch.weight_amount(), lo = patch.weight_low(), hi = patch.weight_high();
        if (amount == -128) return key < lo || key > hi ? -1 : 127;
        if (amount < 0) {
            if (key <= lo) return 127;
            if (key >= hi) return 127 + amount;
            return hi <= lo ? 127 : 127 + amount + (-amount) * (hi - key) / (hi - lo);
        }
        if (key >= hi) return 127;
        if (key <= lo) return 127 - amount;
        return hi <= lo ? 127 : 127 - amount + amount * (key - lo) / (hi - lo);
    }

    uint16_t frequency(int pitch) const noexcept {
        pitch = std::clamp(int(s16(pitch)), -0x4800, 0x5400);
        unsigned shifts = 0;
        uint32_t result;
        if (pitch < 0) {
            while (pitch < 0) {
                pitch += 0xc00;
                ++shifts;
            }
            result = pitches[unsigned(pitch >> 2)] >> shifts;
        } else {
            while (pitch >= 0xc00) {
                pitch -= 0xc00;
                ++shifts;
            }
            result = uint32_t(pitches[unsigned(pitch >> 2)]) << shifts;
        }
        return uint16_t(result) & 0xfffe;
    }

    uint16_t rate(int r) const noexcept { return rates[unsigned(std::clamp(r, 0, 99))]; }

    uint16_t coefficient(int value, unsigned b = 0) const noexcept {
        unsigned index = unsigned(std::clamp(value, 0, 1023)) + b;
        return uint16_t(filters[index] << 4) & 0xfff0;
    }

    RomView rom;
    std::span<const uint16_t> samples;
    std::vector<uint8_t> metadata;
    std::array<Bank, 16> banks{};
    std::array<ProgramData, 1024> programs{};
    std::array<uint16_t, 128> rates{};
    std::array<uint16_t, 256> phase_rates{};
    std::array<uint16_t, 2048> filters{};
    std::array<uint16_t, 768> pitches{};
    std::array<uint8_t, 128> attenuation{}, pan{};
    std::array<uint8_t, 512> velocity_curves{};
    std::array<uint8_t, 12> budgets{};
    uint8_t bend_range = 0;
    unsigned board_bank_mask = 0;
    uint32_t reset_start = 0, reset_end = 0;

private:
    Split split_lookup(uint8_t id, unsigned layer, int pitch, uint8_t &selector,
                       uint16_t program_id) const {
        int bank_idx = static_cast<int>(programs[program_slot(program_id)].sample_banks[layer]);
        if (bank_idx == -2) throw std::runtime_error("HLE instrument names missing sample bank");
        const uint8_t *data = nullptr;
        size_t available = 0;
        if (bank_idx >= 0) {
            const Bank &b = banks[unsigned(bank_idx)];
            selector = uint8_t(bank_idx);
            if (id >= b.count) throw std::runtime_error("HLE sample ID exceeds external directory");
            size_t offset =
                b.offset +
                size_t(int16_t(read_be16(metadata.data() + b.offset + 2 + size_t(id) * 14)));
            if (offset + 14 > metadata.size())
                throw std::runtime_error("HLE sample offset exceeds metadata");
            data = metadata.data() + offset;
            available = metadata.size() - offset;
        } else {
            selector = uint8_t(board_bank_mask * 2);
            if (id > 120) throw std::runtime_error("HLE ROM split ID exceeds table");
            uint32_t address =
                uint32_t(sound_5a7e9117::SPLIT_TABLE_BASE +
                         int16_t(rom.read_u16(sound_5a7e9117::SPLIT_DIRECTORY + id * 2)));
            data = rom.pointer(address, 14);
            available = rom.size() - size_t(data - rom.data());
        }
        unsigned key = uint8_t(pitch >> 8);
        for (size_t i = 0; i + 14 <= available; i += 14) {
            Split result = Split::read(data + i);
            if (key <= result.cutoff) return result;
        }
        throw std::runtime_error("HLE sample splits do not cover key");
    }

    void append_word(uint16_t w) {
        metadata.push_back(uint8_t(w >> 8));
        metadata.push_back(uint8_t(w));
    }

    void decode_banks() {
        const size_t num_banks =
            std::min(size_t(16), samples.size() / sound_5a7e9117::SAMPLE_HALF_BANK_SIZE);
        for (unsigned b = 0; b < num_banks; ++b) {
            unsigned at = 0, base = b * sound_5a7e9117::SAMPLE_HALF_BANK_SIZE;
            auto sample_word = [&]() {
                if (at >= sound_5a7e9117::SAMPLE_HALF_BANK_SIZE || base + at >= samples.size())
                    throw std::runtime_error("HLE sample metadata exceeds half-bank");
                return samples[base + at++];
            };
            uint16_t flag = sample_word();
            bool packed = (flag & 0x8000) != 0;
            auto data_word = [&]() {
                if (!packed) return sample_word();
                uint16_t hi = sample_word();
                return uint16_t((hi & 0xff00) | (sample_word() >> 8));
            };
            Bank &bank = banks[b];
            bank.offset = uint32_t(metadata.size());
            append_word(flag);
            unsigned last = 0;
            bool valid = true;
            for (;;) {
                uint16_t offset = data_word();
                append_word(offset);
                if (!offset) break;
                unsigned limit = packed ? offset * 2 + 4 : offset + 8;
                if (limit <= at * 2 || limit % 14) {
                    valid = false;
                    break;
                }
                last = packed ? offset * 2 : offset;
                if (packed) {
                    metadata[metadata.size() - 2] = uint8_t((offset + 1) >> 8);
                    metadata.back() = uint8_t(offset + 1);
                }
                ++bank.count;
                for (unsigned j = 0; j < 6; ++j) append_word(data_word());
            }
            bank.probe = (at * 2 << 8) | ((b & 1) << 28);
            if (!valid || !last) {
                bank.count = 0;
                continue;
            }
            while (at * 2 < last) append_word(data_word());
            if (at * 2 != last) throw std::runtime_error("HLE split metadata is not aligned");
            for (;;) {
                size_t begin = metadata.size();
                for (unsigned j = 0; j < 7; ++j) append_word(data_word());
                if (metadata[begin + 5] == 127) break;
            }
            uint16_t tag = (base + sound_5a7e9117::SAMPLE_TAG_PROBE_OFFSET < samples.size())
                               ? samples[base + sound_5a7e9117::SAMPLE_TAG_PROBE_OFFSET]
                               : 0;
            bank.tag = uint8_t(packed ? tag >> 8 : tag);
        }
    }

    void cache_programs() {
        constexpr uint32_t BASES[4] = {
            sound_5a7e9117::PROGRAM_DATA_BASE,
            sound_5a7e9117::PROGRAM_DATA_BASE,
            0xc7b050,
            0xc760a0,
        };
        auto find_bank = [&](uint8_t tag) {
            int8_t result = -2;
            for (unsigned b = 0; b < banks.size(); ++b)
                if (banks[b].count && banks[b].tag == tag) result = int8_t(b);
            return result;
        };
        for (unsigned bank_idx = 0; bank_idx < 4; ++bank_idx) {
            const uint32_t table_base = BASES[bank_idx];
            const bool populated =
                (table_base >= SOUND_ROM_BASE) &&
                (table_base - SOUND_ROM_BASE + sound_5a7e9117::PROGRAM_RECORD_STRIDE <=
                 rom.size()) &&
                (rom[table_base - SOUND_ROM_BASE + 2] != 0xff ||
                 rom[table_base - SOUND_ROM_BASE + 3] != 0xff);
            for (unsigned id = 0; id < 256; ++id) {
                const unsigned slot = bank_idx * 256 + id;
                ProgramData &data = programs[slot];
                data.sample_banks.fill(-1);
                if (!populated) {
                    data.base = nullptr;
                    data.enabled = 0;
                    continue;
                }
                unsigned index = id, column = 0;
                uint32_t base = table_base;
                if (index >= 80) {
                    base += sound_5a7e9117::PROGRAM_COLUMN_STRIDE;
                    index -= 80;
                    if (index >= 10) {
                        index -= 10;
                        ++column;
                    }
                }
                if (index >= 40) {
                    index -= 40;
                    ++column;
                }
                const uint32_t addr = base + index * sound_5a7e9117::PROGRAM_RECORD_STRIDE + column;
                if (addr - SOUND_ROM_BASE + sound_5a7e9117::PROGRAM_RECORD_STRIDE > rom.size()) {
                    data.base = nullptr;
                    data.enabled = 0;
                    continue;
                }
                data.base = rom.data() + (addr - SOUND_ROM_BASE);
                data.enabled = Program{data.base}.enabled();
                if (!data.enabled) {
                    const uint8_t *p = data.base;
                    uint32_t mask = 0;
                    for (unsigned i = 0; i < 4; ++i)
                        mask = (mask << 8) |
                               p[sound_5a7e9117::PROGRAM_BANK_MASK_OFFSET + size_t(2) * i];
                    if (p[sound_5a7e9117::PROGRAM_MAGIC_OFFSET_0] == 'X' &&
                        p[sound_5a7e9117::PROGRAM_MAGIC_OFFSET_1] == 'E')
                        for (unsigned i = 0; i < 17; ++i)
                            if (mask & (1u << i))
                                data.sample_banks[i] =
                                    find_bank(p[sound_5a7e9117::PROGRAM_DEFAULT_BANK_OFFSET]);
                } else {
                    for (unsigned layer = 0; layer < 3; ++layer) {
                        const uint8_t *p = data.base + size_t(layer) * 0x76;
                        unsigned marker = 0, tag = 0;
                        for (unsigned i = 0; i < 16; i += 2) {
                            if (p[i] & 128) marker |= 1u << i;
                            if (p[20 + i] & 128) tag |= 1u << (i / 2);
                        }
                        if (marker == 0x5515) data.sample_banks[layer] = find_bank(uint8_t(tag));
                    }
                }
            }
        }
    }
};

} // namespace f3rt::hle
