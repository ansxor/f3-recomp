#include "game_text.hpp"
#include "state_io.hpp"
#include <algorithm>

namespace f3rt {
namespace {
unsigned tested_word_count(uint16_t value) { return value <= 0x8000 ? value : 0; }
}

void GameText::reset() {
    cells_.fill({});
    glyphs_.fill(0);
    glyph_rows_.fill(0);
    references_.fill(0);
    references_[0] = uint16_t(cells_.size());
    map_valid_ = false;
    unsupported_pc_ = 0;
}

void GameText::put(uint32_t destination, uint16_t value) {
    destination &= 0xffffff;
    if (destination < 0x61c000 || destination >= 0x61e000) return;
    if (destination & 1) { map_valid_ = false; return; }
    auto &cell = cells_[(destination - 0x61c000) / 2];
    --references_[cell.tile];
    cell.tile = uint8_t(value);
    cell.palette = uint8_t((value >> 9) & 63);
    cell.flip_x = (value & 0x0100) != 0;
    cell.flip_y = (value & 0x8000) != 0;
    ++references_[cell.tile];
}

void GameText::clear() {
    cells_.fill({0x90, 1, false, false});
    references_.fill(0);
    references_[0x90] = uint16_t(cells_.size());
    map_valid_ = true;
    unsupported_pc_ = 0;
}

void GameText::glyph_row(uint32_t destination, uint32_t value) {
    if (destination < 0x61e000 || destination >= 0x620000 || (destination & 3)) return;
    const unsigned offset = (destination - 0x61e000) / 4;
    for (unsigned x = 0; x < 8; ++x) glyphs_[offset * 8 + x] = uint8_t((value >> (x * 4)) & 15);
    glyph_rows_[offset / 8] |= uint8_t(1u << (offset % 8));
}

void GameText::solid_glyph(unsigned tile, uint8_t pen) {
    std::fill_n(glyphs_.begin() + tile * 64, 64, pen);
    glyph_rows_[tile] = 0xff;
}

void GameText::glyph_mask(uint32_t destination, uint16_t mask, bool set) {
    if (destination < 0x61e000 || destination >= 0x620000 || (destination & 1)) return;
    const unsigned offset = destination - 0x61e000;
    const unsigned first = (offset / 4) * 8 + ((offset & 2) ? 0 : 4);
    for (unsigned x = 0; x < 4; ++x) {
        const uint8_t bits = uint8_t((mask >> (x * 4)) & 15);
        if (set) glyphs_[first + x] |= uint8_t(bits ^ 15);
        else glyphs_[first + x] &= bits;
    }
}

void GameText::observe(GameMemory &memory, const f3_cpu &cpu) {
    const uint32_t sp = cpu.a[7];
    switch (cpu.pc) {
    case 0x59bc: clear(); return;
    case 0x59ee:
        for (unsigned y = 0; y < 30; ++y)
            for (unsigned x = 0; x < 40; ++x) put(0x61c000 + y * 128 + x * 2, 0x0290);
        return;
    case 0x56e6: case 0x5726: case 0x5768: {
        uint32_t source = memory.u32(sp + 4);
        uint32_t destination = memory.u32(sp + 8);
        const unsigned height = memory.u16(source);
        const unsigned width = memory.u16(source + 2);
        source += 4;
        uint8_t attribute = cpu.pc == 0x5768 ? 0 : uint8_t(memory.u16(sp + 12));
        const bool reversed = cpu.pc == 0x5726;
        if (reversed) attribute ^= 0x40;
        const unsigned rows = reversed ? tested_word_count(uint16_t(height)) : height;
        const unsigned columns = reversed ? tested_word_count(uint16_t(width)) : width;
        for (unsigned y = 0; y < rows; ++y) {
            for (unsigned x = 0; x < columns; ++x) {
                uint8_t tile = 0;
                if (cpu.pc == 0x56e6) { tile = uint8_t(memory.u16(source)); source += 2; }
                else if (reversed) tile = memory.u8(source++);
                const unsigned column = reversed ? width - 1 - x : x;
                put(destination + column * 2, uint16_t((unsigned(attribute) << 8) | tile));
            }
            destination += 128;
        }
        break;
    }
    case 0x57a2: {
        uint32_t source = memory.u32(sp + 4);
        uint32_t destination = memory.u32(sp + 8);
        const uint8_t attribute = uint8_t(memory.u16(sp + 12) * 2);
        while (const uint8_t tile = memory.u8(source++)) {
            put(destination, uint16_t((unsigned(attribute) << 8) | tile));
            destination += 2;
        }
        break;
    }
    case 0x57cc: case 0x581c: case 0x5856: {
        uint32_t value = memory.u32(sp + 4);
        const uint16_t count = memory.u16(sp + 8);
        uint32_t destination = memory.u32(sp + 10);
        uint8_t attribute = uint8_t(memory.u16(sp + 14) * 2);
        if (cpu.pc == 0x57cc) attribute &= 0x3e;
        else if (cpu.pc == 0x5856) attribute &= 0x7e;
        if (cpu.pc == 0x581c) {
            for (unsigned n = tested_word_count(count); n; --n) {
                const uint8_t tile = (value & (1u << ((n - 1) & 31))) ? 0x48 : 0x4c;
                put(destination, uint16_t((unsigned(attribute) << 8) | tile));
                destination += 2;
            }
        } else {
            destination += int16_t(uint16_t(count * 2));
            const unsigned digits = cpu.pc == 0x5856 ? (count ? count : 65536u) : tested_word_count(count);
            for (unsigned n = 0; n < digits; ++n) {
                const unsigned digit = cpu.pc == 0x5856 ? value % 10 : value & 15;
                const uint8_t tile = uint8_t(digit + (digit < 10 ? 0x30 : 0x37));
                destination -= 2;
                put(destination, uint16_t((unsigned(attribute) << 8) | tile));
                if (cpu.pc == 0x5856) value /= 10;
                else value >>= 4;
            }
        }
        break;
    }
    case 0x5b80: case 0x5bac: case 0x5bce: {
        uint32_t source = cpu.a[0];
        uint32_t destination = cpu.pc == 0x5bce ? 0x61f400 : 0x61e000;
        const unsigned count = uint16_t(cpu.d[0]) + 1u;
        for (unsigned n = 0; n < count; ++n) {
            const uint32_t value = memory.u32(source);
            glyph_row(destination, (value << 16) | (value >> 16));
            source += 4;
            destination += 4;
        }
        if (cpu.pc != 0x5bce) solid_glyph(0x90, 0);
        break;
    }
    case 0x5be0: case 0x5c08:
        for (unsigned y = 0; y < 29; ++y)
            put(0x61c000 + y * 128 + (cpu.pc == 0x5c08 ? 2 : 0), cpu.pc == 0x5c08 ? 0x0401 : 0x0201);
        return;
    case 0x8de56: solid_glyph(0x90, 0); return;
    case 0x8e9c6: case 0xa1170: solid_glyph(0x90, 15); return;
    case 0x9b530: solid_glyph(0x60, 15); return;
    case 0x8e0a6: case 0x8e0dc:
        // The game animates its blank/text-cover glyph using two ROM mask pairs
        // per task wake. Apply those pairs to the semantic glyph, not char RAM.
        for (unsigned n = 0; n < 2; ++n) {
            const int16_t offset = int16_t(memory.u16(cpu.a[1] + n * 4));
            const uint16_t mask = memory.u16(cpu.a[1] + n * 4 + 2);
            glyph_mask(cpu.a[0] + offset, mask, cpu.pc == 0x8e0a6);
        }
        break;
    case 0x9b544:
        for (unsigned y = 0; y < 6; ++y)
            for (unsigned x = 0; x < 42; ++x) put(0x61c000 + y * 128 + x * 2, 0x0260);
        for (unsigned y = 26; y < 29; ++y)
            for (unsigned x = 0; x < 42; ++x) put(0x61c000 + y * 128 + x * 2, 0x0260);
        return;
    default: return;
    }
    if (!memory.supported) {
        map_valid_ = false;
        if (!unsupported_pc_) unsupported_pc_ = cpu.pc;
    }
}

void GameText::observe_write(uint32_t pc, uint32_t address) {
    if (address < 0x61c000 || address >= 0x620000) return;
    switch (pc) {
    case 0x570e: case 0x5712: case 0x5756: case 0x5758: case 0x578e:
    case 0x57bc: case 0x57be: case 0x580c: case 0x580e:
    case 0x583a: case 0x5840: case 0x5846: case 0x58c2:
    case 0x59d0: case 0x5a08: case 0x5bf8: case 0x5c20:
    case 0x5b8a: case 0x5b9a: case 0x5bb6: case 0x5bc6: case 0x5bd8:
    case 0x8de60: case 0x8e0b2: case 0x8e0c2: case 0x8e0e6: case 0x8e0f4:
    case 0x8e9d2: case 0x9b53a: case 0x9b562: case 0x9b564: case 0x9b586:
    case 0xa1176: case 0xa117c: case 0xa1184: case 0xa118c:
    case 0xa1194: case 0xa119c: case 0xa11a4: case 0xa11ac:
        return;
    default:
        if (address < 0x61e000) map_valid_ = false;
        else glyph_rows_[(address - 0x61e000) / 32] = 0;
        if (!unsupported_pc_) unsupported_pc_ = pc;
    }
}

bool GameText::supported() const {
    if (!map_valid_) return false;
    for (unsigned tile = 0; tile < 256; ++tile)
        if (references_[tile] && glyph_rows_[tile] != 0xff) return false;
    return true;
}

size_t GameText::state_size() const {
    return sizeof(CanonicalGameTextCell) * 4096 +
           256 * 64 +
           256 +
           sizeof(uint16_t) * 256 +
           1 +
           sizeof(uint32_t);
}

void GameText::save_state(StateWriter &writer) const {
    for (size_t i = 0; i < 4096; ++i) {
        const auto &c = cells_[i];
        CanonicalGameTextCell sc{};
        sc.tile = c.tile;
        sc.palette = c.palette;
        sc.flip_x = c.flip_x ? 1 : 0;
        sc.flip_y = c.flip_y ? 1 : 0;
        writer.write(sc);
    }
    writer.write_bytes(glyphs_.data(), glyphs_.size());
    writer.write_bytes(glyph_rows_.data(), glyph_rows_.size());
    writer.write_bytes(references_.data(), sizeof(uint16_t) * references_.size());
    uint8_t mv = map_valid_ ? 1 : 0;
    writer.write(mv);
    writer.write(unsupported_pc_);
}

void GameText::load_state(StateReader &reader) {
    for (size_t i = 0; i < 4096; ++i) {
        CanonicalGameTextCell sc;
        reader.read(sc);
        auto &c = cells_[i];
        c.tile = sc.tile;
        c.palette = sc.palette;
        c.flip_x = sc.flip_x != 0;
        c.flip_y = sc.flip_y != 0;
    }
    reader.read_bytes(glyphs_.data(), glyphs_.size());
    reader.read_bytes(glyph_rows_.data(), glyph_rows_.size());
    reader.read_bytes(references_.data(), sizeof(uint16_t) * references_.size());
    uint8_t mv;
    reader.read(mv);
    map_valid_ = mv != 0;
    reader.read(unsupported_pc_);
}

} // namespace f3rt
