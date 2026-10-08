// license:BSD-3-Clause
#include "renderer/decode.hpp"
#include <cstring>


namespace f3rt {
namespace {

constexpr int H_START = 46;
constexpr int H_VIS = 320;

inline uint16_t read_be16(const uint8_t *p) {
    return (uint16_t(p[0]) << 8) | uint16_t(p[1]);
}

// Producer axis accumulator shared by X and Y.
struct sprite_axis {
    int32_t block_scale = 1 << 8;
    int32_t pos = 0, block_pos = 0;
    int16_t global = 0, subglobal = 0;

    void update(uint8_t scroll, uint16_t posw, bool multi, uint8_t block_ctrl, uint8_t new_zoom) {
        int16_t new_pos = int16_t((posw & 0x800) ? int16_t(posw | 0xf000) : int16_t(posw & 0xfff));
        if (scroll & 0x01) subglobal = new_pos;
        if (scroll & 0x02) global = new_pos;
        if (!(scroll & 0x08)) {
            new_pos += global;
            if (!(scroll & 0x04))
                new_pos += subglobal;
        }

        switch (block_ctrl) {
        case 0b00:
            if (!multi) {
                block_pos = int32_t(new_pos) << 8;
                block_scale = 0x100 - new_zoom;
            }
            [[fallthrough]];
        case 0b10:
            pos = block_pos;
            break;
        case 0b11:
            pos += block_scale * 16;
            break;
        }
    }
};

} // namespace

size_t decode_sprite_list(const uint8_t *spriteram, int visible_y, int visible_height,
                          std::span<DecodedSpriteEntry> out, SpriteRamState &state,
                          bool apply_flip, const SpritePresentation *presentation) {
    sprite_axis x, y;
    uint8_t color = 0;
    bool multi = false;
    size_t count = 0;

    // One list entry through the hardware state machine. Returns false when the entry is a
    // self-jump (list end). `allow_jump` is false for splice replacements.
    auto step = [&](const uint8_t *spr, int &offs, bool allow_jump, uint64_t identity, uint64_t object,
                    uint8_t flags) -> bool {
        const uint16_t w0 = read_be16(&spr[0]);
        const uint16_t w1 = read_be16(&spr[2]);
        const uint16_t w2 = read_be16(&spr[4]);
        const uint16_t w3 = read_be16(&spr[6]);
        const uint16_t w4 = read_be16(&spr[8]);
        const uint16_t w5 = read_be16(&spr[10]);
        const uint16_t w6 = read_be16(&spr[12]);

        // Special command bit in word 3
        if (w3 & 0x8000) {
            state.flipscreen = (w5 & 0x2000) != 0;
            state.extra_planes = uint8_t((w5 >> 8) & 3);
            state.pen_mask = uint8_t((state.extra_planes << 4) | 0x0f);
            state.trails = (w5 & 0x0002) != 0;
            state.bank = (w5 & 0x0001) != 0;
        }

        // Sprite list jump bit in word 6
        if (allow_jump && (w6 & 0x8000)) {
            const int new_offs = w6 & 0x03ff;
            if (new_offs == offs)
                return false;
            offs = new_offs - 1;
        }

        const uint8_t spritecont = uint8_t(w4 >> 8);
        const bool lock = (spritecont & 0x04) != 0;
        if (!lock)
            color = uint8_t(w4 & 0xff);

        const uint8_t scroll_mode = uint8_t((w2 >> 12) & 0x0f);
        x.update(scroll_mode, uint16_t(w2 & 0x0fff), multi, uint8_t((spritecont >> 6) & 3), uint8_t(w1 & 0xff));
        y.update(scroll_mode, uint16_t(w3 & 0x0fff), multi, uint8_t((spritecont >> 4) & 3), uint8_t(w1 >> 8));
        multi = (spritecont & 0x08) != 0;

        const uint32_t tile = uint32_t(w0) | (uint32_t(w5 & 0x0001) << 16);
        if (!tile)
            return true;

        const int32_t tx = state.flipscreen ? ((512 << 8) - x.block_scale * 16 - x.pos) : x.pos;
        const int32_t ty = state.flipscreen ? ((256 << 8) - y.block_scale * 16 - y.pos) : y.pos;

        // Cull against the configured scanout crop, not the top edge.
        if (tx + x.block_scale * 16 <= (H_START << 8) || tx > ((H_START + H_VIS - 1) << 8) ||
            ty + y.block_scale * 16 <= (visible_y << 8) ||
            ty > ((visible_y + visible_height - 1) << 8))
            return true;

        if (count >= out.size())
            return true;

        const bool flip_x = (spritecont & 0x01) != 0;
        const bool flip_y = (spritecont & 0x02) != 0;

        auto &s = out[count++];
        s.x = apply_flip ? tx : x.pos;
        s.y = apply_flip ? ty : y.pos;
        s.scale_x = x.block_scale;
        s.scale_y = y.block_scale;
        s.tile = tile;
        s.color = color;
        s.flip_x = apply_flip && state.flipscreen ? !flip_x : flip_x;
        s.flip_y = apply_flip && state.flipscreen ? !flip_y : flip_y;
        s.pri = uint8_t((color >> 6) & 3);
        s.identity = identity;
        s.object = object;
        s.flags = flags;
        return true;
    };

    const std::span<const SpriteSplice> splices =
        presentation ? presentation->splices : std::span<const SpriteSplice>{};
    const std::span<const uint64_t> identities =
        presentation ? presentation->identity : std::span<const uint64_t>{};
    const std::span<const uint64_t> objects =
        presentation ? presentation->object : std::span<const uint64_t>{};
    const std::span<const uint8_t> slot_flags =
        presentation ? presentation->flags : std::span<const uint8_t>{};

    int total_sprites = 0;
    for (int offs = 0; offs < 0x400 && total_sprites < 0x400; ++offs) {
        total_sprites++;
        const uint32_t bank_offset = state.bank ? 0x8000 : 0x0000;
        const uint8_t *spr = &spriteram[bank_offset + size_t(offs) * 16];

        for (const SpriteSplice &sp : splices) {
            if (sp.bank != state.bank || sp.first != offs)
                continue;
            const size_t n = size_t(sp.last) - sp.first + 1;
            if (sp.last < sp.first || sp.last >= 0x400 || sp.real.size() != n * 16 ||
                std::memcmp(spr, sp.real.data(), n * 16) != 0)
                break;
            const size_t reps = sp.replacement.size() / 16;
            for (size_t k = 0; k < reps; ++k) {
                int dummy = offs;
                step(&sp.replacement[k * 16], dummy, false, sprite_identity_mix(sp.identity, uint32_t(k)), sp.identity,
                     k < sp.flags.size() ? sp.flags[k] : uint8_t(0));
            }
            total_sprites += int(n) - 1;
            offs = sp.last;
            goto next_entry;
        }

        {
            const size_t slot = size_t(state.bank ? 0x400 : 0) + size_t(offs);
            const uint64_t id = slot < identities.size() ? identities[slot] : 0;
            const uint64_t object = slot < objects.size() ? objects[slot] : 0;
            if (!step(spr, offs, true, id, object, slot < slot_flags.size() ? slot_flags[slot] : uint8_t(0)))
                break;
        }
    next_entry:;
    }
    return count;
}

} // namespace f3rt
