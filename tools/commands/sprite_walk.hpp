// Annotated sprite-list walker for the MCP `sprites` tool.
//
// Mirrors the hardware state machine in runtime/renderer/decode.cpp (decode_sprite_list): bank and flipscreen
// state set by command entries (word 3 bit 15), the jump word (word 6 bit 15, a jump to itself ends the list),
// the 24.8 block/scroll axis accumulators with their int16 wrap, flipscreen mirroring, the scanout-crop cull and
// the 1,024-entry traversal budget. Unlike the renderer it records every visited slot so a caller can see why an
// entry was or was not drawn. Splice replacements (SpritePresentation) are render-only and are not applied; the
// walk matches the renderer's canonical list. runtime/tests/sprites.cpp cross-checks the two.
#pragma once

#include "f3rt/bytes.hpp"
#include "f3rt/memory_map.hpp"
#include "renderer/decode.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace f3rt::tool {

// Entries the renderer visits per walk (MAX_HARDWARE_SPRITES, and its `total_sprites < 0x400` loop bound).
inline constexpr unsigned SPRITE_WALK_BUDGET = 0x400;

struct SpriteWalkEntry {
    unsigned slot = 0, bank = 0;
    uint16_t words[8]{};
    uint32_t tile = 0;
    uint8_t color = 0, control = 0;
    int32_t raw_x = 0, raw_y = 0; // 24.8 block position before the flipscreen mirror
    int32_t x = 0, y = 0;         // 24.8 position as the renderer draws it (flipscreen applied)
    int32_t scale_x = 0x100, scale_y = 0x100;
    bool flip_x = false, flip_y = false; // as drawn: control flip bits, inverted under flipscreen
    bool command = false;                // word 3 bit 15: updated bank/flipscreen/planes/trails
    int jump = -1;                       // slot jumped to, or -1
    bool list_end = false;               // jump to itself
    bool culled = false;                 // tile != 0 but outside the scanout crop
    bool drawn = false;                  // the renderer emits this entry
};

struct SpriteWalkResult {
    std::vector<SpriteWalkEntry> entries; // every visited slot, in walk order
    SpriteRamState end_state;             // bank/flipscreen/planes/trails after the walk
    unsigned visited = 0, drawn = 0, culled = 0, jumps = 0, commands = 0;
    int end_slot = -1; // slot of the self-jump that ended the list, or -1
};

namespace sprite_walk_detail {

struct Axis {
    int32_t block_scale = 1 << 8;
    int32_t pos = 0, block_pos = 0;
    int16_t global = 0, subglobal = 0;

    void update(uint8_t scroll, uint16_t posw, bool multi, uint8_t block_ctrl, uint8_t new_zoom) {
        int16_t new_pos = int16_t((posw & 0x800) ? int16_t(posw | 0xf000) : int16_t(posw & 0xfff));
        if (scroll & 0x01) subglobal = new_pos;
        if (scroll & 0x02) global = new_pos;
        if (!(scroll & 0x08)) {
            new_pos = int16_t(new_pos + global);
            if (!(scroll & 0x04)) new_pos = int16_t(new_pos + subglobal);
        }
        switch (block_ctrl) {
        case 0b00:
            if (!multi) {
                block_pos = int32_t(new_pos) << 8;
                block_scale = 0x100 - new_zoom;
            }
            [[fallthrough]];
        case 0b10: pos = block_pos; break;
        case 0b11: pos += block_scale * 16; break;
        default: break;
        }
    }
};

} // namespace sprite_walk_detail

// `spriteram` is the 64 KiB sprite RAM (two 0x8000 banks of 0x400 16-byte entries). `state` is the sprite
// register state the walk starts from (the renderer carries it across frames). `limit` caps visited entries
// and is clamped to SPRITE_WALK_BUDGET. The crop is the renderer's scanout window (geometry FIRST_LINE/HEIGHT).
inline SpriteWalkResult walk_sprite_list(std::span<const uint8_t> spriteram, SpriteRamState state,
                                         unsigned limit, int visible_y, int visible_height) {
    constexpr int H_START = 46, H_VIS = 320; // decode.cpp's horizontal crop
    SpriteWalkResult r;
    if (limit > SPRITE_WALK_BUDGET) limit = SPRITE_WALK_BUDGET;
    sprite_walk_detail::Axis x, y;
    uint8_t color = 0;
    bool multi = false;
    unsigned visited = 0;
    for (int offs = 0; offs < 0x400 && visited < limit; ++offs) {
        ++visited;
        const unsigned bank = state.bank ? 1 : 0;
        const size_t base = size_t(bank) * SPRITE_BANK_STRIDE + size_t(offs) * 16;
        if (base + 16 > spriteram.size()) break;
        const uint8_t *spr = &spriteram[base];
        SpriteWalkEntry e;
        e.slot = unsigned(offs);
        e.bank = bank;
        for (int i = 0; i < 8; ++i) e.words[i] = read_be16(&spr[2 * i]);
        const uint16_t w0 = e.words[0], w1 = e.words[1], w2 = e.words[2], w3 = e.words[3], w4 = e.words[4],
                       w5 = e.words[5], w6 = e.words[6];

        if (w3 & 0x8000) {
            e.command = true;
            ++r.commands;
            state.flipscreen = (w5 & 0x2000) != 0;
            state.extra_planes = uint8_t((w5 >> 8) & 3);
            state.pen_mask = uint8_t((state.extra_planes << 4) | 0x0f);
            state.trails = (w5 & 0x0002) != 0;
            state.bank = (w5 & 0x0001) != 0;
        }
        if (w6 & 0x8000) {
            const int target = w6 & 0x03ff;
            if (target == offs) {
                e.list_end = true;
                r.end_slot = offs;
                r.entries.push_back(e);
                break;
            }
            e.jump = target;
            ++r.jumps;
            offs = target - 1;
        }

        const uint8_t spritecont = uint8_t(w4 >> 8);
        e.control = spritecont;
        if (!(spritecont & 0x04)) color = uint8_t(w4 & 0xff);
        e.color = color;
        const uint8_t scroll_mode = uint8_t((w2 >> 12) & 0x0f);
        x.update(scroll_mode, uint16_t(w2 & 0x0fff), multi, uint8_t((spritecont >> 6) & 3), uint8_t(w1 & 0xff));
        y.update(scroll_mode, uint16_t(w3 & 0x0fff), multi, uint8_t((spritecont >> 4) & 3), uint8_t(w1 >> 8));
        multi = (spritecont & 0x08) != 0;

        e.tile = uint32_t(w0) | (uint32_t(w5 & 0x0001) << 16);
        e.raw_x = x.pos;
        e.raw_y = y.pos;
        e.scale_x = x.block_scale;
        e.scale_y = y.block_scale;
        e.x = state.flipscreen ? ((512 << 8) - x.block_scale * 16 - x.pos) : x.pos;
        e.y = state.flipscreen ? ((256 << 8) - y.block_scale * 16 - y.pos) : y.pos;
        const bool flip_x = (spritecont & 0x01) != 0, flip_y = (spritecont & 0x02) != 0;
        e.flip_x = state.flipscreen ? !flip_x : flip_x;
        e.flip_y = state.flipscreen ? !flip_y : flip_y;
        if (e.tile) {
            e.culled = e.x + e.scale_x * 16 <= (H_START << 8) || e.x > ((H_START + H_VIS - 1) << 8) ||
                       e.y + e.scale_y * 16 <= (visible_y << 8) || e.y > ((visible_y + visible_height - 1) << 8);
            e.drawn = !e.culled;
        }
        r.culled += e.culled;
        r.drawn += e.drawn;
        r.entries.push_back(e);
    }
    r.visited = visited;
    r.end_state = state;
    return r;
}

} // namespace f3rt::tool
