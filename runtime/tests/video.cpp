#include "f3rt/machine.hpp"
#include "f3rt/video.hpp"
#include "f3rt/game_video.hpp"
#include "f3rt/input.hpp"
#include "renderer/game/tiles.hpp"
#include "renderer/game/text.hpp"
#include "renderer/game/sprites.hpp"
#include "renderer/game/lines.hpp"
#include "renderer/game/captured_frame.hpp"
#include "renderer/game/clip.hpp"
#include "renderer/game/indexed_compositor.hpp"
#include "renderer/game/row_workers.hpp"
#include "sprites/units.hpp"
#include "runner.hpp"
#include "support.hpp"
#include <gtest/gtest.h>
#include <rapidcheck/gtest.h>
#include <algorithm>
#include <array>
#include <filesystem>
#include <cstdio>
#include <memory>
#include <random>
#include <vector>

namespace {
// Graphics-RAM sub-tables, as offsets from f3rt::GRAPHICS_BASE.
constexpr uint32_t LINE_LATCH_TABLE = 0x20400;
constexpr uint32_t LINE_MODE_TABLE = 0x26400;
constexpr uint32_t LINE_BACKGROUND_TABLE = 0x26600;
constexpr uint32_t LINE_MODE_ALT_TABLE = 0x26c00;
constexpr uint32_t LINE_SELECT_TABLE = 0x26200;
constexpr uint32_t LINE_TABLE_20800 = 0x20800;
constexpr uint32_t LINE_TABLE_28600 = 0x28600;
constexpr uint32_t PF_ROW_TABLE_24000 = 0x24000;
constexpr uint32_t PF_ROW_TABLE_24400 = 0x24400;
constexpr uint32_t PF_ROW_TABLE_24600 = 0x24600;
constexpr uint32_t PF_ROW_STRIDE = 0x200;
constexpr uint32_t PF_SELECT_TABLE = 0x20e00;
constexpr uint32_t PF_ALT_ENABLE_0 = 0x2b400;
constexpr uint32_t PF_ALT_ENABLE_1 = 0x2b600;
constexpr uint32_t PF_MAP_STRIDE = 0x1000;
constexpr uint32_t PF_EXT_MAP_STRIDE = 0x2000;
constexpr uint32_t PF_MAP2_BASE = f3rt::OFFS_PLAYFIELDS + 2 * PF_MAP_STRIDE;
constexpr uint32_t PF_EXT_ALT_BASE = 0x1a000;
// Sprite and high-plane ROM offsets of the second sprite bank (Command War).
constexpr size_t SPRITE_ROM_BANK1 = 0x400000;
constexpr size_t HIGH_PLANE_ROM_BANK1 = 0x200000;
#ifdef F3RT_GAME_VIDEO
// Game tile cell table offset within graphics RAM (test fixture: layer 1).
constexpr uint32_t GAME_TILE_CELLS = f3rt::OFFS_PLAYFIELDS + 1 * f3rt::GameTiles::MAP_STRIDE;
#else
constexpr uint32_t GAME_TILE_CELLS = 0x12000;
#endif
void set_word(std::vector<uint8_t> &bytes, size_t at, uint16_t value) {
    bytes[at] = uint8_t(value >> 8);
    bytes[at + 1] = uint8_t(value);
}

void set_word(std::array<uint8_t, 0x40000> &bytes, size_t at, uint16_t value) {
    bytes[at] = uint8_t(value >> 8);
    bytes[at + 1] = uint8_t(value);
}

void set_word(std::array<uint8_t, 0x20> &bytes, size_t at, uint16_t value) {
    bytes[at] = uint8_t(value >> 8);
    bytes[at + 1] = uint8_t(value);
}

void set_color(std::array<uint8_t, 0x8000> &bytes, size_t at, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes[at + i] = uint8_t(value >> (24 - 8 * i));
}
} // namespace

TEST(Video, RomPlaneLoading4BppAndCustomVisibleScanline) {
    std::vector<uint8_t> sprites(256, 0x21), tiles(128, 0x43);
    f3rt::Video video;
    f3rt::VideoConfig config{90, 2, 31, 224};
    EXPECT_TRUE(video.load_roms(sprites, {}, tiles, {}, config))
        << "4-bpp ROMs need no fabricated high planes";
    EXPECT_TRUE(video.sprite_tiles()[256] == 1 && video.sprite_tiles()[257] == 2 &&
                video.playfield_tiles()[0] == 3 && video.playfield_tiles()[1] == 4)
        << "Absent high planes preserve packed low pens across independent ROM geometries";

    std::array<uint8_t, 0x8000> palette{};
    std::array<uint8_t, 0x40000> graphics{};
    std::array<uint8_t, 0x20> control{};
    std::vector<uint32_t> pixels(size_t(320) * 224);
    set_color(palette, 4, 0x00112233);
    set_color(palette, 8, 0x00445566);
    set_word(graphics, LINE_LATCH_TABLE + 31 * 2, 12);
    set_word(graphics, LINE_BACKGROUND_TABLE + 31 * 2, 1);
    set_word(graphics, LINE_MODE_TABLE + 31 * 2, 0x6000);
    set_word(graphics, LINE_LATCH_TABLE + 32 * 2, 8);
    set_word(graphics, LINE_BACKGROUND_TABLE + 32 * 2, 2);
    set_word(graphics, 0, 1);
    set_word(graphics, 4, 46);
    set_word(graphics, 6, 31);
    set_word(graphics, 8, 1);
    set_word(graphics, 16 + 12, 0x8001);
    video.render_frame(palette, graphics, control, pixels);
    EXPECT_TRUE(pixels[0] == 0xff112233 && pixels[320] == 0xff445566)
        << "Custom first visible scanline is 31, not default 24";
}

TEST(Video, SpriteLagAndDelayedPositionSnapshot) {
    std::vector<uint8_t> sprites(256, 0x21), tiles(128, 0x43);
    f3rt::Video video;
    f3rt::VideoConfig config{90, 2, 31, 224};
    video.load_roms(sprites, {}, tiles, {}, config);

    std::array<uint8_t, 0x8000> palette{};
    std::array<uint8_t, 0x40000> graphics{};
    std::array<uint8_t, 0x20> control{};
    std::vector<uint32_t> pixels(size_t(320) * 224);
    set_word(graphics, 0, 1);
    set_word(graphics, 4, 46);
    set_word(graphics, 6, 31);
    set_word(graphics, 8, 1);
    set_word(graphics, 16 + 12, 0x8001);
    video.render_frame(palette, graphics, control, pixels);
    EXPECT_EQ(video.sprite_plane()[31 * 432 + 46], 0) << "Lag 2 delays a newly parsed sprite list";

    std::vector<uint8_t> snapshot(video.state_size());
    video.save_state(snapshot);
    set_word(graphics, 4, 50);
    video.render_frame(palette, graphics, control, pixels);
    EXPECT_TRUE(video.sprite_plane()[31 * 432 + 46] == 0x1011 &&
                video.sprite_plane()[31 * 432 + 50] == 0x1011)
        << "Lag 2 draws the previous parsed position, not current sprite RAM";
    video.render_frame(palette, graphics, control, pixels);
    EXPECT_TRUE(video.sprite_plane()[31 * 432 + 46] == 0 &&
                video.sprite_plane()[31 * 432 + 50] == 0x1011)
        << "Lag 2 advances the captured list on the next frame";
    video.load_state(snapshot);
    video.render_frame(palette, graphics, control, pixels);
    EXPECT_EQ(video.sprite_plane()[31 * 432 + 46], 0x1011)
        << "Snapshots preserve the delayed parsed list, not only the framebuffer";
}

// Video::vblank parses the sprite list at once but draws it only when something reads the plane. A
// reference Video fed the same sprite RAM through set_active_spriteram (parse and draw at once) must
// agree with it whenever the deferred one is observed: plane, rendering and saved state, with trail
// frames accumulating over lists that were never drawn.
TEST(Video, VblankDeferredSpriteDrawMatchesImmediate) {
    std::mt19937_64 rng(5);
    std::vector<uint8_t> sprites(16 * 128), tiles(128, 0x43);
    for (auto &byte : sprites) byte = rng() % 3 ? uint8_t(rng()) : uint8_t(0);
    const f3rt::VideoConfig config{0, 1, 31, 224};
    f3rt::Video deferred, immediate;
    ASSERT_TRUE(deferred.load_roms(sprites, {}, tiles, {}, config));
    ASSERT_TRUE(immediate.load_roms(sprites, {}, tiles, {}, config));
    std::array<uint8_t, 0x8000> palette{};
    for (size_t i = 0; i < palette.size(); ++i) palette[i] = uint8_t(rng());
    std::array<uint8_t, 0x40000> graphics{};
    std::array<uint8_t, 0x20> control{};
    std::vector<uint32_t> deferred_pixels(size_t(320) * 224),
        immediate_pixels(deferred_pixels.size());
    const auto plane_of = [](const f3rt::Video &video) {
        const auto plane = video.sprite_plane();
        return std::vector<uint16_t>(plane.begin(), plane.end());
    };
    const auto state_of = [](const f3rt::Video &video) {
        std::vector<uint8_t> state(video.state_size());
        video.save_state(state);
        return state;
    };
    for (unsigned frame = 0; frame < 240; ++frame) {
        std::fill_n(graphics.begin() + f3rt::OFFS_SPRITERAM, f3rt::SPRITE_RAM_SIZE, uint8_t(0));
        const unsigned count = 12 + unsigned(rng() % 30);
        const bool trails = rng() % 10 < 3;
        for (unsigned i = 0; i < count; ++i) {
            const size_t entry = f3rt::OFFS_SPRITERAM + size_t(i) * 16;
            set_word(graphics, entry + 0, uint16_t(1 + rng() % 15));
            set_word(graphics, entry + 4, uint16_t(46 + rng() % 300));
            // Entry 0 carries the command word: trails on or off for this and later frames.
            set_word(graphics, entry + 6, uint16_t((31 + rng() % 200) | (i == 0 ? 0x8000 : 0)));
            set_word(graphics, entry + 8, uint16_t(rng() & 0xff));
            if (i == 0) set_word(graphics, entry + 10, trails ? 0x0002 : 0x0000);
        }
        deferred.vblank(graphics);
        immediate.set_active_spriteram(std::span<const uint8_t>(graphics).subspan(
            f3rt::OFFS_SPRITERAM, f3rt::SPRITE_RAM_SIZE));
        if (frame % 5 == 1) EXPECT_EQ(plane_of(deferred), plane_of(immediate)) << "frame " << frame;
        if (frame % 7 == 3) EXPECT_EQ(state_of(deferred), state_of(immediate)) << "frame " << frame;
        if (frame % 11 == 5) {
            deferred.render_frame(palette, graphics, control, deferred_pixels);
            immediate.render_frame(palette, graphics, control, immediate_pixels);
            EXPECT_EQ(deferred_pixels, immediate_pixels) << "frame " << frame;
            EXPECT_EQ(plane_of(deferred), plane_of(immediate)) << "frame " << frame;
        }
    }
    EXPECT_EQ(state_of(deferred), state_of(immediate));
}

TEST(Video, CustomGeometryAndFdaPaletteBlur) {
    std::vector<uint8_t> sprites(256, 0x21), tiles(128, 0x43);
    f3rt::Video video;
    f3rt::VideoConfig config{0, 1, 32, 224};
    EXPECT_TRUE(video.load_roms(sprites, {}, tiles, {}, config)) << "Custom geometry loads";
    video.reset();
    std::array<uint8_t, 0x40000> graphics{};
    std::array<uint8_t, 0x8000> palette{};
    std::array<uint8_t, 0x20> control{};
    std::vector<uint32_t> pixels(size_t(320) * 224);
    set_color(palette, 0, 0x00abcdee);
    set_color(palette, 4, 0x0000abc8);
    set_color(palette, 8, 0x0000abc4);
    set_color(palette, 12, 0x0000abc2);
    set_color(palette, 16, 0x0000fffe);
    set_color(palette, 20, 0x00abcdef);

    const auto line = [&](unsigned y, uint16_t latch, uint16_t mode, uint16_t background) {
        set_word(graphics, LINE_LATCH_TABLE + y * 2, latch);
        set_word(graphics, LINE_MODE_TABLE + y * 2, mode);
        set_word(graphics, LINE_BACKGROUND_TABLE + y * 2, background);
    };
    line(32, 12, 0x2000, 1);
    line(33, 8, 0x6000, 2);
    line(34, 12, 0x6000, 3);
    line(35, 0x48, 0x6000, 4);
    set_word(graphics, LINE_MODE_ALT_TABLE + 35 * 2, 0x2000);
    line(36, 8, 0, 0);
    line(37, 8, 0, 5);
    line(38, 12, 0, 1);
    video.render_frame(palette, graphics, control, pixels);
    EXPECT_EQ(pixels[0], 0xffa8b0c0)
        << "15-bit palette includes red bit 3, without 5-to-8-bit replication";
    EXPECT_EQ(pixels[320], 0xffa0b8c0)
        << "Unlatched mode changes are ignored while green bit 2 is preserved";
    EXPECT_EQ(pixels[640], 0xff00abc2)
        << "FDA bit 14 switches to full 24-bit color on the next latched line";
    EXPECT_EQ(pixels[960], 0xfff8f8f8)
        << "Alternate-bank latches restore 15-bit mode with a maximum channel of 248";
    EXPECT_TRUE(pixels[1280] == 0xffc8d8e8 && pixels[1600] == 0xffc8d8e8)
        << "15-bit color includes blue bit 1 and ignores upper bits and unused bit 0";
    EXPECT_TRUE(pixels[1920] == 0xff545860 && pixels[1921] == 0xffa8b0c0)
        << "FDA forward blur averages with the unfiltered preceding pixel, starting from black";
}

TEST(Video, SecondSpriteBankHighPlanes) {
    f3rt::Video video;
    f3rt::VideoConfig config{0, 1, 32, 224};
    std::vector<uint8_t> sprites(0x800000, 0), tiles(0x200000, 0), high(0x400000, 0);
    sprites[SPRITE_ROM_BANK1] = 0x65;
    high[HIGH_PLANE_ROM_BANK1] = 3;
    EXPECT_TRUE(video.load_roms(sprites, high, tiles, {}, config))
        << "Command War's second sprite bank loads";
    EXPECT_TRUE(video.sprite_tiles()[size_t(32768) * 256] == 0x35 &&
                video.sprite_tiles()[32768 * 256 + 1] == 6)
        << "Command War's bank-1 pens include its own high planes, without wrapping to bank 0";
}

TEST(Video, FdpGeometryAndNonPowerOfTwoAssets) {
    std::vector<uint8_t> sprites(size_t(3) * 128), tiles(size_t(5) * 128);
    for (unsigned tile = 0; tile < 3; ++tile)
        std::fill_n(sprites.begin() + ptrdiff_t(tile) * 128, 128, uint8_t((tile + 1) * 0x11));
    for (unsigned tile = 0; tile < 5; ++tile)
        std::fill_n(tiles.begin() + ptrdiff_t(tile) * 128, 128, uint8_t((tile + 1) * 0x11));
    f3rt::Video video;
    f3rt::VideoConfig config{0, 0, 0, 256, false};
    EXPECT_TRUE(video.load_roms(sprites, {}, tiles, {}, config))
        << "Independent non-power-of-two FDP assets load";
    std::array<uint8_t, 0x40000> graphics{};
    std::array<uint8_t, 0x8000> palette{};
    std::array<uint8_t, 0x20> control{};
    std::vector<uint32_t> pixels(320 * 256 + 1, 0xdeadbeef);
    for (unsigned map = 0; map < 8; ++map)
        for (unsigned cell = 0; cell < 1024; ++cell)
            set_word(graphics, f3rt::OFFS_PLAYFIELDS + map * PF_MAP_STRIDE + cell * 4 + 2,
                     uint16_t(map + 5));
    auto line = video.inspect_playfield_line(4, 511, graphics);
    EXPECT_TRUE(line.palette.size() == 512 && line.palette[0] == 5 && line.palette[511] == 5)
        << "Nonextended alternate map 4 uses its own 32-column base and count-correct tile "
           "wrapping";
    line = video.inspect_playfield_line(7, 0, graphics);
    EXPECT_EQ(line.palette[0], 3)
        << "Last nonextended physical map remains independently addressable";

    for (unsigned cell = 0; cell < 1024; ++cell) set_word(graphics, PF_MAP2_BASE + cell * 4 + 2, 0);
    for (unsigned y = 0; y < 3; ++y) {
        set_word(graphics, f3rt::OFFS_LINERAM + y * 2, 4);
        set_word(graphics, PF_ROW_TABLE_24400 + y * 2, y == 1 ? 0x200 : 0);
    }
    set_word(graphics, LINE_LATCH_TABLE, 6);
    set_word(graphics, LINE_SELECT_TABLE, 0xff00);
    set_word(graphics, LINE_MODE_TABLE, 0x6000);
    set_word(graphics, PF_SELECT_TABLE, 4);
    set_word(graphics, PF_ALT_ENABLE_0, 0x2001);
    palette[5 * 4 + 1] = 0x12;
    palette[5 * 4 + 2] = 0x34;
    palette[5 * 4 + 3] = 0x56;
    video.render_frame(palette, graphics, control, pixels);
    EXPECT_TRUE(pixels[0] == 0xff000000 && pixels[320] == 0xff123456 && pixels[640] == 0xff000000)
        << "Latched alternate-map selection changes visible pixels and row-usage gating per "
           "scanline";
    EXPECT_EQ(pixels[size_t(320) * 256], 0xdeadbeef) << "Full-height crop writes exactly 256 rows";

    set_word(graphics, PF_SELECT_TABLE, 12);
    set_word(graphics, PF_ALT_ENABLE_0, 0);
    set_word(graphics, PF_ALT_ENABLE_1, 0x2001);
    set_word(graphics, f3rt::OFFS_LINERAM, 8);
    set_word(graphics, PF_ROW_TABLE_24600, 0x200);
    palette[4 + 1] = 0x65;
    palette[4 + 2] = 0x43;
    palette[4 + 3] = 0x21;
    video.render_frame(palette, graphics, control, pixels);
    EXPECT_EQ(pixels[0], 0xff654321)
        << "PF3 alternate selection reads map 5 rather than map 4 or 7";

    graphics.fill(0);
    set_word(graphics, 0, 5);
    set_word(graphics, 4, 46);
    set_word(graphics, 6, 0);
    set_word(graphics, 16 + 12, 0x8001);
    video.render_frame(palette, graphics, control, pixels);
    EXPECT_EQ(video.sprite_plane()[46], 0x1003)
        << "Non-power-of-two sprite code wraps by count at scanout row zero";

    config = {0, 0, 255, 1, false};
    EXPECT_TRUE(video.load_roms(sprites, {}, tiles, {}, config)) << "Bottom-only crop loads";
    video.reset();
    graphics.fill(0);
    set_word(graphics, 0, 4);
    set_word(graphics, 4, 365);
    set_word(graphics, 6, 255);
    set_word(graphics, 16 + 12, 0x8001);
    pixels.assign(321, 0xdeadbeef);
    video.render_frame(palette, graphics, control, pixels);
    EXPECT_TRUE(video.sprite_plane()[255 * 432 + 365] == 0x1002 &&
                video.sprite_plane()[254 * 432 + 365] == 0)
        << "Bottom/right crop includes its final pixel without painting outside vertical bounds";
    EXPECT_EQ(pixels[320], 0xdeadbeef) << "One-row crop preserves the output boundary sentinel";

    config = {0, 0, 24, 232, true};
    EXPECT_TRUE(video.load_roms(sprites, {}, tiles, {}, config))
        << "Extended geometry accepts independent counts";
    graphics.fill(0);
    set_word(graphics, f3rt::OFFS_PLAYFIELDS + 63 * 4 + 2, 9);
    set_word(graphics, f3rt::OFFS_PLAYFIELDS + 64 * 4 + 2, 7);
    line = video.inspect_playfield_line(0, 0, graphics);
    EXPECT_TRUE(line.palette.size() == 1024 && line.palette[1008] == 5)
        << "Extended map spans all 64 columns";
    line = video.inspect_playfield_line(0, 16, graphics);
    EXPECT_EQ(line.palette[0], 3) << "Extended row stride is 64 descriptors, not 32";
    EXPECT_TRUE(!config.extended_alt_maps &&
                video.inspect_playfield_line(4, 0, graphics).palette.empty())
        << "Ordinary extended profiles retain four physical maps by default";

    config = {0, 0, 0, 256, true, true};
    EXPECT_TRUE(video.load_roms(sprites, {}, tiles, {}, config))
        << "Extended alternate-map layout loads";
    pixels.assign(320 * 256 + 1, 0xdeadbeef);
    graphics.fill(0);
    control.fill(0);
    palette.fill(0);
    for (unsigned map = 4; map < 6; ++map)
        for (unsigned cell = 0; cell < 2048; ++cell)
            set_word(graphics, f3rt::OFFS_PLAYFIELDS + map * PF_EXT_MAP_STRIDE + cell * 4 + 2,
                     uint16_t(map + 5));
    set_word(graphics, PF_EXT_ALT_BASE + 63 * 4 + 2, 7);
    set_word(graphics, PF_EXT_ALT_BASE + 64 * 4 + 2, 8);
    set_word(graphics, PF_EXT_ALT_BASE + (31 * 64 + 63) * 4 + 2, 9);
    line = video.inspect_playfield_line(5, 0, graphics);
    EXPECT_TRUE(line.palette.size() == 1024 && line.palette[0] == 1 && line.palette[1008] == 3)
        << "Extended map 5 has its own native 64-column base and count wrapping";
    line = video.inspect_playfield_line(5, 16, graphics);
    EXPECT_EQ(line.palette[0], 4) << "Extended alternate row stride stays 64 columns";
    line = video.inspect_playfield_line(5, 511, graphics);
    EXPECT_EQ(line.palette[1023], 5) << "Extended alternate final row/column stays within PF RAM";
    EXPECT_TRUE(video.inspect_playfield_line(6, 0, graphics).palette.empty())
        << "Extended alternates stop at map 5 before text/character RAM, never alias pivot RAM";

    for (unsigned pen = 1; pen <= 5; ++pen) palette[pen * 4 + 3] = uint8_t(pen * 0x11);
    for (unsigned pf = 2; pf <= 3; ++pf) {
        set_word(graphics, PF_SELECT_TABLE, uint16_t(1u << pf));
        set_word(graphics, PF_ALT_ENABLE_0, pf == 2 ? 0x2001 : 0);
        set_word(graphics, PF_ALT_ENABLE_1, pf == 3 ? 0x2001 : 0);
        for (unsigned y = 0; y < 4; ++y) {
            set_word(graphics, f3rt::OFFS_LINERAM + y * 2, y == 2 ? 0 : uint16_t(1u << pf));
            set_word(graphics, PF_ROW_TABLE_24000 + pf * PF_ROW_STRIDE + y * 2, y == 1 ? 0x200 : 0);
        }
        set_word(graphics, LINE_LATCH_TABLE, 6);
        set_word(graphics, LINE_SELECT_TABLE, 0xff00);
        set_word(graphics, LINE_MODE_TABLE, 0x6000);
        video.render_frame(palette, graphics, control, pixels);
        const uint32_t alternate = pf == 2 ? 0xff000055 : 0xff000011;
        EXPECT_TRUE(pixels[0] == 0xff000000 && pixels[320] == alternate &&
                    pixels[640] == alternate && pixels[960] == 0xff000000)
            << "Extended PF2/PF3 selection and row usage follow per-line latches including "
               "retention and return";
        config.extended_alt_maps = false;
        EXPECT_TRUE(video.load_roms(sprites, {}, tiles, {}, config))
            << "Default extended layout reloads";
        video.render_frame(palette, graphics, control, pixels);
        EXPECT_EQ(pixels[320], 0xff000000)
            << "JP/default extended mode ignores alternate selector bit";
        config.extended_alt_maps = true;
        EXPECT_TRUE(video.load_roms(sprites, {}, tiles, {}, config))
            << "Extended alternate layout reloads";
    }

    set_word(control, 6, uint16_t((1016 - 28) * 64));
    set_word(control, 14, uint16_t(-128));
    set_word(graphics, LINE_TABLE_20800, 8);
    set_word(graphics, LINE_TABLE_28600, 0);
    set_word(graphics, f3rt::OFFS_LINERAM, 8);
    set_word(graphics, PF_ROW_TABLE_24600, 0x3ff);
    set_word(graphics, f3rt::OFFS_LINERAM + 2, 8);
    set_word(graphics, PF_ROW_TABLE_24600 + 2, 0x200);
    video.render_frame(palette, graphics, control, pixels);
    EXPECT_TRUE(pixels[0] == 0xff000055 && pixels[8] == 0xff000011 && pixels[320] == 0xff000033)
        << "Extended alternate scanout wraps x at 1024 and y at 512 without a 32-column mask";
}

#ifdef F3RT_GAME_VIDEO
TEST(Video, GameTileObservation) {
    std::vector<uint8_t> graphics(0x40000, 0);
    std::array<uint8_t, 0x20> control{};
    const auto store = [&](unsigned cell, uint16_t attributes, uint16_t code) {
        const unsigned at = GAME_TILE_CELLS + cell * 4;
        graphics[at] = uint8_t(attributes >> 8);
        graphics[at + 1] = uint8_t(attributes);
        graphics[at + 2] = uint8_t(code >> 8);
        graphics[at + 3] = uint8_t(code);
    };
    store(0, uint16_t(3 | 0x200), 5);              // palette 3, blend
    store(1, uint16_t(4 | (1 << 10) | 0x4000), 6); // palette 4, extra plane, flip X
    store(2, uint16_t(5 | (1 << 10)), 7);          // palette low bit set: extra plane masked
    store(3, uint16_t(6 | 0x8000), 8);             // palette 6, flip Y
    std::array<uint8_t, size_t(16) * 256> assets{};
    for (unsigned i = 0; i < assets.size(); ++i) assets[i] = uint8_t(i * 5 + 1);

    f3rt::GameTiles scene;
    f3rt::VideoRam vram{graphics, control, 0};
    scene.decode(vram);

    const auto plain = scene.playfield_pixel(1, 0, 0, false, assets);
    EXPECT_TRUE(plain.palette == 48 + (assets[size_t(5) * 256] & 15) && plain.flags == 0x11)
        << "Raw cell decodes palette base, pen mask and blend selector";
    const auto flip0 = scene.playfield_pixel(1, 16, 0, false, assets);
    const auto flip15 = scene.playfield_pixel(1, 31, 0, false, assets);
    EXPECT_TRUE(flip0.palette == 64 + (assets[6 * 256 + 15] & 31) &&
                flip15.palette == 64 + (assets[size_t(6) * 256] & 31))
        << "Raw cell flip X mirrors the texel column and applies the extra-plane mask";
    const auto masked = scene.playfield_pixel(1, 32, 0, false, assets);
    EXPECT_EQ(masked.palette, 80 + (assets[size_t(7) * 256] & 15))
        << "Extra pen plane is masked off when the palette's low bit is set";
    const auto flip_y = scene.playfield_pixel(1, 48, 0, false, assets);
    EXPECT_TRUE(flip_y.palette == 96 + (assets[8 * 256 + 15 * 16] & 15) && flip_y.flags == 0x10)
        << "Raw cell flip Y mirrors the texel row";
}

TEST(Video, GameTileRowSampling) {
    std::vector<uint8_t> graphics(0x40000, 0);
    std::array<uint8_t, 0x20> control{};
    std::array<uint8_t, size_t(64) * 256> assets{};
    for (unsigned i = 0; i < assets.size(); ++i) assets[i] = uint8_t(i * 37 + i / 16);
    struct RawCell {
        uint16_t attributes, code;
    };
    std::array<std::array<RawCell, f3rt::GameTiles::CELLS_PER_MAP>, 4> cells{};
    for (unsigned layer = 0; layer < cells.size(); ++layer)
        for (unsigned i = 0; i < cells[layer].size(); ++i) {
            auto &cell = cells[layer][i];
            cell.attributes = uint16_t((i & 0x1ff) | (uint16_t((i >> 9) & 1) << 9) |
                                       (uint16_t((i >> 2) & 1) << 10) | (uint16_t(i & 1) << 14) |
                                       (uint16_t((i >> 1) & 1) << 15));
            cell.code = uint16_t(0x8000 | (i & 63));
            const unsigned at =
                f3rt::OFFS_PLAYFIELDS + layer * f3rt::GameTiles::MAP_STRIDE + size_t(i) * 4;
            graphics[at] = uint8_t(cell.attributes >> 8);
            graphics[at + 1] = uint8_t(cell.attributes);
            graphics[at + 2] = uint8_t(cell.code >> 8);
            graphics[at + 3] = uint8_t(cell.code);
        }
    f3rt::GameTiles scene;
    f3rt::VideoRam vram{graphics, control, 0};
    scene.decode(vram);
    for (unsigned layer = 0; layer < cells.size(); ++layer)
        for (bool flipped : {false, true})
            for (int y : {-513, -512, -1, 0, 15, 16, 511, 512}) {
                auto sampler = scene.row_sampler(layer, y, flipped, assets);
                for (int x : {0, 15, 16, 17, 17, 31, 32, int(f3rt::GameTiles::MAP_WIDTH_PX - 1),
                              int(f3rt::GameTiles::MAP_WIDTH_PX), -1, -16, -17,
                              -int(f3rt::GameTiles::MAP_WIDTH_PX),
                              -int(f3rt::GameTiles::MAP_WIDTH_PX + 1), 511, 256, 0,
                              int(f3rt::GameTiles::MAP_WIDTH_PX - 1)}) {
                    const unsigned sx = (unsigned(x) & (f3rt::GameTiles::MAP_WIDTH_PX - 1)) ^
                                        (flipped ? (f3rt::GameTiles::MAP_WIDTH_PX - 1) : 0);
                    const unsigned sy = (unsigned(y) & 511) ^ (flipped ? 511 : 0);
                    const auto &cell =
                        cells[layer][(sy / 16) * f3rt::GameTiles::COLUMNS + sx / 16];
                    const unsigned palette_code = cell.attributes & 0x1ff;
                    const unsigned pen_mask =
                        (((cell.attributes >> 10) & 3 & ~cell.attributes) << 4) | 15;
                    const unsigned tx = (sx & 15) ^ ((cell.attributes & 0x4000) ? 15 : 0);
                    const unsigned ty = (sy & 15) ^ ((cell.attributes & 0x8000) ? 15 : 0);
                    const uint8_t pen =
                        assets[(cell.code & 0x7fff) * 256 + ty * 16 + tx] & pen_mask;
                    const auto pixel = sampler.pixel(x);
                    EXPECT_TRUE(pixel.palette == uint16_t(palette_code * 16 + pen) &&
                                pixel.flags ==
                                    uint8_t((pen ? 0x10 : 0) | ((cell.attributes >> 9) & 1)))
                        << "Raw row sampling preserves palette base, mask, flips and wrapped X "
                           "jumps";
                }
            }
}

TEST(Video, GameSpriteVramBlockChaining) {
    std::vector<uint8_t> graphics(0x40000, 0);
    std::array<uint8_t, 0x20> control{};
    const auto word = [&](unsigned offset, uint16_t value) {
        graphics[offset] = uint8_t(value >> 8);
        graphics[offset + 1] = uint8_t(value);
    };
    word(0 * 16 + 0, 1);      // tile
    word(0 * 16 + 2, 0);      // zoom 0 -> scale 256
    word(0 * 16 + 4, 50);     // x position
    word(0 * 16 + 6, 30);     // y position
    word(0 * 16 + 8, 0x0005); // spritecont 0, colour 5
    word(1 * 16 + 0, 2);
    word(1 * 16 + 2, 0);
    word(1 * 16 + 8, 0xf005); // block control 3/3, colour 5
    f3rt::GameSprites scene;
    f3rt::VideoRam vram{graphics, control, 0};
    scene.decode(vram);
    EXPECT_TRUE(scene.sprites().empty())
        << "Decoded sprites are submitted, not visible before the latch";
    scene.latch();
    const auto sprites = scene.sprites();
    ASSERT_EQ(sprites.size(), 2) << "Two sprite-list entries decode to two sprites";
    EXPECT_TRUE(sprites[0].x == 50 * 256 && sprites[0].y == 30 * 256)
        << "Sprite-list coordinates are scanout coordinates, with no extra origin";
    EXPECT_TRUE(sprites[1].x == (50 + 16) * 256 && sprites[1].y == (30 + 16) * 256)
        << "Block-chained sprite advances one 16-pixel block on each axis";
    EXPECT_TRUE(sprites[0].tile == 1 && sprites[1].tile == 2 && sprites[0].palette == 5 &&
                sprites[0].scale_x == 256 && sprites[0].scale_y == 256)
        << "Tile code, colour and zoom survive VRAM decode";
}

TEST(Video, GameLineVramCarryForward) {
    std::vector<uint8_t> graphics(0x40000, 0);
    std::array<uint8_t, 0x20> control{};
    const auto word = [&](unsigned offset, uint16_t value) {
        graphics[offset] = uint8_t(value >> 8);
        graphics[offset + 1] = uint8_t(value);
    };
    word(LINE_LATCH_TABLE + 0, 1 << 3); // line 0 latches subsection 3
    word(LINE_BACKGROUND_TABLE, 0x1234);
    word(LINE_LATCH_TABLE + 4, 1 << 3); // line 2 latches it again with a new value
    word(LINE_BACKGROUND_TABLE + 4, 0x00ff);
    f3rt::GameLines scene;
    f3rt::VideoRam vram{graphics, control, 0};
    scene.decode(vram);
    scene.prepare(false);
    EXPECT_EQ(scene.row(0).background, 0x1234) << "Line 0 decodes its latched background palette";
    EXPECT_EQ(scene.row(1).background, 0x1234)
        << "Line 1 carries the previous line's unlatched background palette forward";
    EXPECT_EQ(scene.row(2).background, 0x00ff)
        << "Line 2 adopts its own latched background palette";
}

TEST(Video, GameTextVramDecode) {
    std::vector<uint8_t> graphics(0x40000, 0);
    std::array<uint8_t, 0x20> control{};
    const auto word = [&](unsigned offset, uint16_t value) {
        graphics[offset] = uint8_t(value >> 8);
        graphics[offset + 1] = uint8_t(value);
    };
    word(f3rt::OFFS_TEXTRAM_MAP, uint16_t((3 << 9) | 0x12));
    graphics[f3rt::OFFS_TEXTRAM_GLYPHS + 0x12 * 32 + 3] = 0x05;
    f3rt::GameText scene;
    f3rt::VideoRam vram{graphics, control, 0};
    scene.decode(vram);
    const auto px = scene.pixel(0, 0, false);
    EXPECT_TRUE(px.palette == 3 * 16 + 5 && px.flags == 0x10)
        << "Text map and glyph RAM decode palette base and pen";
    word(f3rt::OFFS_TEXTRAM_MAP + 2, uint16_t((2 << 9) | 0x0100 | 0x8000 | 0x21));
    graphics[f3rt::OFFS_TEXTRAM_GLYPHS + 0x21 * 32 + 24] = 0x9a;
    scene.decode(vram);
    const auto flipped = scene.pixel(8, 1, false);
    EXPECT_TRUE(flipped.palette == 2 * 16 + 9 && flipped.flags == 0x10)
        << "Text flip bits and glyph nibble order decode from raw words";
}
#endif

#ifdef F3RT_GAME_VIDEO
// RapidCheck property: GameText::pixel decodes arbitrary glyph pixels and correctly mirrors texel
// coordinates when flip X or flip Y bits are set in the text map VRAM words.
RC_GTEST_PROP(Video, GameTextFlipMirrorsTexels, ()) {
    const auto tx = *rc::gen::inRange(0u, 8u);
    const auto ty = *rc::gen::inRange(0u, 8u);
    const auto flip_x = *rc::gen::arbitrary<bool>();
    const auto flip_y = *rc::gen::arbitrary<bool>();
    const auto palette_code = *rc::gen::inRange(0u, 64u);

    std::vector<uint8_t> graphics(0x40000, 0);
    std::array<uint8_t, 0x20> control{};

    // In text-map cell (0, 0): glyph 0, palette_code, flip_x (bit 8), flip_y (bit 15).
    const uint16_t map_word =
        uint16_t((palette_code << 9) | (flip_x ? 0x0100 : 0) | (flip_y ? 0x8000 : 0));
    graphics[f3rt::OFFS_TEXTRAM_MAP] = uint8_t(map_word >> 8);
    graphics[f3rt::OFFS_TEXTRAM_MAP + 1] = uint8_t(map_word);

    // Populate glyph 0 in glyph RAM (0x1e000) with a unique non-zero pen for each (gx, gy) in 8x8:
    // Raw glyph RAM layout: pixel (gx, gy) is nibble (gx & 1) of byte gy*4 + (3 - gx/2).
    for (unsigned gy = 0; gy < 8; ++gy) {
        for (unsigned gx = 0; gx < 8; ++gx) {
            const uint8_t pen = uint8_t(((gy * 8 + gx) % 15) + 1);
            const size_t byte_idx = f3rt::OFFS_TEXTRAM_GLYPHS + gy * 4 + (3 - gx / 2);
            const unsigned shift = (gx & 1) * 4;
            graphics[byte_idx] = uint8_t((graphics[byte_idx] & ~(0x0f << shift)) | (pen << shift));
        }
    }

    f3rt::GameText scene;
    f3rt::VideoRam vram{graphics, control, 0};
    scene.decode(vram);

    const auto px = scene.pixel(int(tx), int(ty), false);

    const unsigned expected_gx = flip_x ? (7 - tx) : tx;
    const unsigned expected_gy = flip_y ? (7 - ty) : ty;
    const uint8_t expected_pen = uint8_t(((expected_gy * 8 + expected_gx) % 15) + 1);
    const uint16_t expected_palette = uint16_t(palette_code * 16 + expected_pen);

    RC_ASSERT(px.palette == expected_palette);
    RC_ASSERT(px.flags == 0x10);
}
#endif

// RapidCheck property: Video::render_frame converts arbitrary 15-bit palette words
// (RRRRGGGGBBBBRGBx) to 32-bit ARGB pixels matching independent 5-to-8-bit channel expansion.
RC_GTEST_PROP(Video, Color15BitUnpackingInvariant, ()) {
    const auto color_word = *rc::gen::arbitrary<uint16_t>();
    static auto video = [] {
        f3rt::Video v;
        std::vector<uint8_t> sprites(256, 0x21), tiles(128, 0x43);
        f3rt::VideoConfig config{0, 1, 32, 224};
        v.load_roms(sprites, {}, tiles, {}, config);
        return v;
    }();

    std::array<uint8_t, 0x40000> graphics{};
    std::array<uint8_t, 0x8000> palette{};
    std::array<uint8_t, 0x20> control{};
    std::vector<uint32_t> pixels(size_t(320) * 224);

    // Scanline 32 programmed to 15-bit mode (latch 12, mode 0x2000, background palette 1).
    set_word(graphics, LINE_LATCH_TABLE + 32 * 2, 12);
    set_word(graphics, LINE_MODE_TABLE + 32 * 2, 0x2000);
    set_word(graphics, LINE_BACKGROUND_TABLE + 32 * 2, 0);

    // Background color 0 stored in lower 16 bits of 32-bit palette entry 0
    palette[0] = 0;
    palette[1] = 0;
    palette[2] = uint8_t(color_word >> 8);
    palette[3] = uint8_t(color_word);

    video.render_frame(palette, graphics, control, pixels);

    // Independent reference: FDA 15-bit channel mapping
    // Red: high 4 bits in 15..12, low bit in 3 -> 5 bits scaled to 8-bit (<< 3)
    const uint32_t expected_r =
        (((color_word >> 12) & 0x0fu) << 4) | (((color_word >> 3) & 1u) << 3);
    // Green: high 4 bits in 11..8, low bit in 2 -> 5 bits scaled to 8-bit (<< 3)
    const uint32_t expected_g =
        (((color_word >> 8) & 0x0fu) << 4) | (((color_word >> 2) & 1u) << 3);
    // Blue: high 4 bits in 7..4, low bit in 1 -> 5 bits scaled to 8-bit (<< 3)
    const uint32_t expected_b =
        (((color_word >> 4) & 0x0fu) << 4) | (((color_word >> 1) & 1u) << 3);
    const uint32_t expected_pixel =
        0xff000000u | (expected_r << 16) | (expected_g << 8) | expected_b;

    RC_ASSERT(pixels[0] == expected_pixel);
}

#if defined(F3RT_GAME_VIDEO)
// Land Maker attract mode after boot, composed at both geometries by the row workers and by the
// caller: chunking and per-job kernel selection must not change a single pixel. ROM-gated.
TEST(GameCompositor, ParallelMatchesSerial) {
    const char *rom_env = std::getenv("F3_ROM_DIR");
#ifdef F3RT_DEFAULT_ROM_DIR
    if (!rom_env) rom_env = F3RT_DEFAULT_ROM_DIR;
#endif
    if (!rom_env || !*rom_env)
        GTEST_SKIP() << "F3_ROM_DIR not set; skipping ROM-backed compositor check";
    std::unique_ptr<f3rt::Machine> machine;
    try {
        machine = f3rt::runner::create_strict_machine(std::filesystem::path(rom_env),
                                                      F3RT_DEFAULT_SET, true, 1);
    } catch (const std::exception &error) {
        GTEST_SKIP() << "Strict-native machine unavailable: " << error.what();
    }
    for (unsigned frame = 0; frame < 900; ++frame) {
        f3rt::apply_local_inputs(*machine, {});
        ASSERT_TRUE(machine->run_frame(true));
    }
    ASSERT_NE(machine->game_video, nullptr);
    const auto &captured = machine->game_video->captured_frame();
    ASSERT_FALSE(captured.fallback) << "Frame 900 must be a supported native scanout";
    for (const unsigned border : {0u, 40u}) {
        f3rt::GameVideoOptions options;
        options.scale = 1;
        options.border = border;
        std::vector<uint16_t> plane(f3rt::sprite_plane_size(options));
        f3rt::raster_captured_sprites(captured, machine->video->sprite_tiles(), plane, options);
        std::vector<uint32_t> expected(size_t(options.width()) * options.height());
        f3rt::compose_captured_scene(captured, machine->video->playfield_tiles(), plane, expected,
                                     options, f3rt::ComposeMode::Serial);
        std::vector<uint32_t> parallel(expected.size());
        f3rt::compose_captured_scene(captured, machine->video->playfield_tiles(), plane, parallel,
                                     options, f3rt::ComposeMode::Parallel);
        EXPECT_EQ(parallel, expected) << "border=" << border;
    }
}

// Indexed-kernel equivalence helpers. Both composers receive the same captured frame; the canonical
// serial composition of the frame (with the whole-plane sprite raster) is the reference.
namespace {
// The chunk raster leaves plane texels no sprite wrote stale: this is the plane with those zeroed,
// the form comparable to the whole-plane raster.
std::vector<uint16_t> covered_plane(std::span<const uint16_t> plane, f3rt::GameVideoOptions options,
                                    const f3rt::SpriteCoverage &coverage) {
    const bool expanded = options.expanded();
    const unsigned width = expanded ? options.width() : 432;
    const unsigned left = expanded ? 0 : 46;
    std::vector<uint16_t> result(plane.size(), 0);
    for (unsigned row = 0; row < plane.size() / width; ++row)
        for (unsigned layer = 0; layer < f3rt::SpriteCoverage::LAYERS; ++layer) {
            const auto bits = coverage.row(row, layer);
            for (unsigned x = left; x < left + options.width(); ++x)
                if (bits[(x - left) / 32] >> ((x - left) % 32) & 1)
                    result[size_t(row) * width + x] = plane[size_t(row) * width + x];
        }
    return result;
}

struct IndexedComparison {
    size_t pixels = 0, mismatches = 0, plane_mismatches = 0;
    size_t rows = 0, fallback_rows = 0; // composed rows, and those the canonical row kernel decided
    size_t first_pixel = SIZE_MAX, first_plane = SIZE_MAX;
};

IndexedComparison compare_indexed_frame(const f3rt::CapturedFrame &frame,
                                        std::span<const uint8_t> tile_pixels,
                                        std::span<const uint8_t> sprite_tiles,
                                        unsigned border, f3rt::IndexedScratch &scratch) {
    f3rt::GameVideoOptions options;
    options.scale = 1;
    options.border = border;
    const size_t pixel_count = size_t(options.width()) * options.height();
    std::vector<uint16_t> plane(f3rt::sprite_plane_size(options));
    f3rt::raster_captured_sprites(frame, sprite_tiles, plane, options);
    std::vector<uint32_t> expected(pixel_count);
    f3rt::compose_captured_scene(frame, tile_pixels, plane, expected, options,
                                 f3rt::ComposeMode::Serial);
    std::vector<uint32_t> indexed(pixel_count);
    f3rt::compose_captured_indexed(frame, tile_pixels, sprite_tiles, scratch, indexed,
                                   options.width(), f3rt::PixelFormat::Argb8888, options);
    IndexedComparison result;
    result.pixels = pixel_count;
    result.rows = options.height();
    result.fallback_rows = scratch.stats.fallback_rows.load();
    for (size_t i = 0; i < pixel_count; ++i)
        if (indexed[i] != expected[i]) {
            if (result.first_pixel == SIZE_MAX) result.first_pixel = i;
            ++result.mismatches;
        }
    const auto written = covered_plane(scratch.plane, options, scratch.coverage(options));
    for (size_t i = 0; i < plane.size(); ++i)
        if (written[i] != plane[i]) {
            if (result.first_plane == SIZE_MAX) result.first_plane = i;
            ++result.plane_mismatches;
        }
    // The 3DS staging order is the canonical word rotated for the display transfer.
    std::vector<uint32_t> staging(pixel_count);
    f3rt::compose_captured_indexed(frame, tile_pixels, sprite_tiles, scratch, staging,
                                   options.width(), f3rt::PixelFormat::Rgba8888, options);
    for (size_t i = 0; i < pixel_count; ++i)
        if (staging[i] != f3rt::to_pixel_format(expected[i], f3rt::PixelFormat::Rgba8888)) {
            if (result.first_pixel == SIZE_MAX) result.first_pixel = i;
            ++result.mismatches;
        }
    return result;
}

// Deterministic synthetic scene generator for the indexed-comparison property test. Land Maker
// frames never select clip windows, mosaic, blur, 15-bit words, flipscreen or alternate maps, so the
// property test has to build them.
f3rt::CapturedFrame synthetic_frame(std::mt19937_64 &rng) {
    const auto range = [&rng](unsigned lo, unsigned hi) {
        return unsigned(rng() % (hi - lo + 1)) + lo;
    };
    f3rt::CapturedFrame frame;
    std::array<uint8_t, 0x40000> graphics{};
    std::array<uint8_t, 0x20> control{};
    const auto word = [&](size_t offset, uint16_t value) {
        graphics[offset] = uint8_t(value >> 8);
        graphics[offset + 1] = uint8_t(value);
    };
    // Playfield cells: attributes exercise palette codes, flips, the blend-select bit and the extra
    // pen planes; tile codes cover the small synthetic ROM.
    for (unsigned map = 0; map < f3rt::GameTiles::MAP_COUNT; ++map)
        for (unsigned i = 0; i < f3rt::GameTiles::CELLS_PER_MAP; ++i) {
            const uint16_t flags = uint16_t(range(0, 1) * 0x8000) |       // flip Y
                                   uint16_t(range(0, 1) * 0x4000) |       // flip X
                                   uint16_t(range(0, 1) * 0x0200) |       // blend select
                                   uint16_t(range(0, 3) << 10) |          // extra pen planes
                                   uint16_t(range(0, 0x1ff));             // palette code
            const uint16_t code = uint16_t(range(0, 3));
            word(f3rt::OFFS_PLAYFIELDS + map * f3rt::GameTiles::MAP_STRIDE + i * 4 + 0, flags);
            word(f3rt::OFFS_PLAYFIELDS + map * f3rt::GameTiles::MAP_STRIDE + i * 4 + 2, code);
        }
    // Text map and glyph RAM.
    for (unsigned i = 0; i < 4096; ++i)
        word(f3rt::OFFS_TEXTRAM_MAP + i * 2, uint16_t(rng()));
    // Blank glyph rows and texels exist in real text, and the kernels skip them.
    for (unsigned i = 0; i < 0x2000; ++i)
        graphics[f3rt::OFFS_TEXTRAM_GLYPHS + i] = range(0, 3) ? uint8_t(rng()) : uint8_t(0);
    for (unsigned row = 0; row < 0x2000 / 4; ++row)
        if (range(0, 7) == 0)
            for (unsigned i = 0; i < 4; ++i) graphics[f3rt::OFFS_TEXTRAM_GLYPHS + row * 4 + i] = 0;
    const f3rt::VideoRam vram{graphics, control, 0};
    frame.tiles.decode(vram);
    frame.text.decode(vram);
    frame.pen_mask = uint8_t(range(0x0f, 0x3f));
    frame.flipped = range(0, 1) != 0;
    for (unsigned i = 0; i < frame.colors.size(); ++i)
        frame.colors[i] = uint32_t(rng()) & 0xffffffu;
    for (auto &row : frame.rows) {
        row.background = uint16_t(rng() & 0x1fff);
        row.text_x = int16_t(rng() % 1024);
        row.text_y = int16_t(rng() % 1024);
        row.mosaic_period = uint8_t(range(0, 17));
        row.palette_15bit = range(0, 3) == 0;
        row.blur = range(0, 3) == 0;
        row.bitmap = false;
        for (unsigned i = 0; i < 4; ++i) row.blend[i] = uint8_t(range(0, 8));
        // Half the rows give both slots weights summing to eight, the only case where a mode 0/3
        // sample resolves its pixel on sight.
        if (range(0, 1)) {
            row.blend[2] = uint8_t(8 - row.blend[0]);
            row.blend[3] = uint8_t(8 - row.blend[1]);
        }
        // Half the rows give every layer its own priority, so no tie sends the row to the
        // canonical kernel and the first-visible-layer kernels are what the property checks.
        std::array<uint8_t, f3rt::LAYER_COUNT> distinct{1, 2, 3, 4, 5, 6, 7, 8, 9};
        const bool distinct_priorities = range(0, 1) != 0;
        std::shuffle(distinct.begin(), distinct.end(), rng);
        for (unsigned i = 0; i < 4; ++i) {
            row.clips[i].left = int16_t(int(range(0, 400)) - 40);
            row.clips[i].right = int16_t(int(row.clips[i].left) + int(range(0, 400)));
        }
        for (unsigned i = 0; i < f3rt::LAYER_COUNT; ++i) {
            auto &layer = row.layer(f3rt::LayerId{uint8_t(i)});
            layer.enabled = range(0, 3) != 0;
            layer.priority = distinct_priorities ? distinct[i] : uint8_t(range(0, 15));
            layer.blend_mode = uint8_t(range(0, 3));
            layer.blend_select = range(0, 1) != 0;
            layer.mosaic = range(0, 3) == 0;
            layer.clip_enabled = uint8_t(range(0, 15));
            layer.clip_inverted = uint8_t(range(0, 15));
            layer.clip_inverse = range(0, 1) != 0;
        }
        for (auto &pf : row.playfields) {
            pf.source_x = int32_t(int(rng() % (f3rt::PF_MAP_WIDTH_PX * 512)) -
                                  f3rt::PF_MAP_WIDTH_PX * 256);
            pf.source_y = int32_t(int(rng() % 1024) - 256);
            pf.x_step = range(0, 8) == 0 ? int32_t(int(range(0, 512)) - 64) : 256;
            pf.y_step = range(0, 8) == 0 ? int32_t(int(range(0, 512)) - 64) : 256;
            pf.y_fraction = uint8_t(rng());
            pf.palette_add = uint16_t(rng() & 0x7f);
            pf.empty_row = range(0, 15) == 0;
            pf.full_res_offset = -1;
            pf.alt_map = false;
        }
        if constexpr (f3rt::game_config::video.extended_alt_maps)
            for (unsigned i = 2; i < 4; ++i)
                row.playfields[i].alt_map = range(0, 1) != 0;
    }
    const unsigned sprite_count = range(0, 200);
    for (unsigned i = 0; i < sprite_count; ++i) {
        auto &sprite = frame.sprites[i];
        sprite.x = int32_t(int(rng() % (512 * 256)) - 40 * 256);
        sprite.y = int32_t(int(rng() % (256 * 256)) - 32 * 256);
        sprite.scale_x = uint16_t(range(16, 512));
        sprite.scale_y = uint16_t(range(16, 512));
        sprite.tile = uint32_t(range(0, 3));
        sprite.palette = uint8_t((range(0, 3) << 6) | range(0, 0x3f));
        sprite.flip_x = range(0, 1) != 0;
        sprite.flip_y = range(0, 1) != 0;
    }
    frame.sprite_count = sprite_count;
    frame.fallback = false;
    return frame;
}
} // namespace

// ROM-gated: the indexed kernel must reproduce the canonical serial composition of every sampled
// Land Maker frame (attract and a played game), at both 3DS geometries, including the chunked sprite
// plane.
TEST(GameCompositor, IndexedMatchesReference) {
    const char *rom_env = std::getenv("F3_ROM_DIR");
#ifdef F3RT_DEFAULT_ROM_DIR
    if (!rom_env) rom_env = F3RT_DEFAULT_ROM_DIR;
#endif
    if (!rom_env || !*rom_env)
        GTEST_SKIP() << "F3_ROM_DIR not set; skipping ROM-backed indexed compositor check";
    size_t frames = 0, mismatches = 0, plane_mismatches = 0;
    for (const bool play : {false, true}) {
        size_t rows = 0, fallback_rows = 0;
        std::unique_ptr<f3rt::Machine> machine;
        try {
            machine = f3rt::runner::create_strict_machine(std::filesystem::path(rom_env),
                                                          F3RT_DEFAULT_SET, true, 1);
        } catch (const std::exception &error) {
            GTEST_SKIP() << "Strict-native machine unavailable: " << error.what();
        }
        machine->sprite_units = f3rt::make_game_sprite_units(*machine, false);
        f3rt::runner::SeededScheduleInputSource schedule(
            1, f3rt::runner::ScheduleConfig{.versus = false});
        f3rt::IndexedScratch scratch;
        for (unsigned index = 0; index < 3600; ++index) {
            if (play) schedule.apply(*machine, index);
            else f3rt::apply_local_inputs(*machine, {});
            ASSERT_TRUE(machine->run_frame(true));
            if (index < 600 || index % 30) continue;
            const auto &captured = machine->game_video->captured_frame();
            ASSERT_FALSE(captured.fallback) << "frame " << index << " must be a native scanout";
            for (const unsigned border : {0u, 40u}) {
                const auto result = compare_indexed_frame(captured, machine->video->playfield_tiles(),
                                                          machine->video->sprite_tiles(), border,
                                                          scratch);
                ++frames;
                rows += result.rows;
                fallback_rows += result.fallback_rows;
                mismatches += result.mismatches;
                plane_mismatches += result.plane_mismatches;
                EXPECT_EQ(result.mismatches, 0u)
                    << "frame " << index << " border " << border << " first pixel "
                    << result.first_pixel;
                EXPECT_EQ(result.plane_mismatches, 0u)
                    << "frame " << index << " border " << border << " first plane texel "
                    << result.first_plane;
            }
        }
        // The first-visible-layer shortcut has to cover the real game: rows it cannot prove exact
        // go to the slow canonical row kernel and may not become the common case.
        std::printf("indexed compositor %s: rows=%zu fallback_rows=%zu\n",
                    play ? "play" : "attract", rows, fallback_rows);
        EXPECT_LT(fallback_rows * 100, rows) << (play ? "play" : "attract");
    }
    std::printf("indexed compositor: frames=%zu mismatches=%zu plane_mismatches=%zu\n", frames,
                mismatches, plane_mismatches);
    EXPECT_EQ(mismatches, 0u);
    EXPECT_EQ(plane_mismatches, 0u);
}

// ROM-gated: the native sprite plane is drawn on demand when captured presentation is on. Observed at
// irregular intervals (state CRC, native picture), a lazy machine must match the eager one that
// draws the plane at every latch.
TEST(GameVideo, LazyNativePlaneMatchesEagerPlane) {
    const char *rom_env = std::getenv("F3_ROM_DIR");
#ifdef F3RT_DEFAULT_ROM_DIR
    if (!rom_env) rom_env = F3RT_DEFAULT_ROM_DIR;
#endif
    if (!rom_env || !*rom_env)
        GTEST_SKIP() << "F3_ROM_DIR not set; skipping ROM-backed lazy sprite plane check";
    std::unique_ptr<f3rt::Machine> lazy, eager;
    try {
        lazy = f3rt::runner::create_strict_machine(std::filesystem::path(rom_env), F3RT_DEFAULT_SET,
                                                   true, 1);
        eager = f3rt::runner::create_strict_machine(std::filesystem::path(rom_env),
                                                    F3RT_DEFAULT_SET, false, 1);
    } catch (const std::exception &error) {
        GTEST_SKIP() << "Strict-native machine unavailable: " << error.what();
    }
    // The eager control has no captured presentation, so latch_sprites draws its plane every frame.
    eager->game_video = std::make_unique<f3rt::GameVideo>(*eager, f3rt::GameVideoMode::Game);
    lazy->sprite_units = f3rt::make_game_sprite_units(*lazy, false);
    f3rt::runner::SeededScheduleInputSource lazy_input(
        1, f3rt::runner::ScheduleConfig{.versus = false}),
        eager_input(1, f3rt::runner::ScheduleConfig{.versus = false});
    for (unsigned index = 0; index < 1800; ++index) {
        lazy_input.apply(*lazy, index);
        eager_input.apply(*eager, index);
        ASSERT_TRUE(lazy->run_frame(true));
        ASSERT_TRUE(eager->run_frame(true));
        if (index % 13 == 0) EXPECT_EQ(lazy->state_crc(), eager->state_crc()) << "frame " << index;
        else if (index % 5 == 0)
            EXPECT_EQ(lazy->native_pixels(), eager->native_pixels()) << "frame " << index;
    }
    EXPECT_EQ(lazy->state_crc(), eager->state_crc());
}

// The chunked raster must equal the whole-plane raster for every geometry: same "first write wins"
// order inside each chunk, same culling, and chunk rows cleared exactly once.
TEST(GameCompositor, ChunkedSpriteRasterMatchesWholePlane) {
    std::mt19937_64 rng(12345);
    std::vector<uint8_t> sprite_tiles(8 * 256);
    for (auto &byte : sprite_tiles) byte = uint8_t(rng());
    std::vector<f3rt::SceneSprite> sprites;
    const auto range = [&rng](unsigned lo, unsigned hi) {
        return unsigned(rng() % (hi - lo + 1)) + lo;
    };
    for (unsigned i = 0; i < 120; ++i) {
        f3rt::SceneSprite sprite;
        sprite.x = int32_t(int(rng() % (512 * 256)) - 64 * 256);
        sprite.y = int32_t(int(rng() % (256 * 256)) - 32 * 256);
        sprite.scale_x = uint16_t(range(8, 640));
        sprite.scale_y = uint16_t(range(8, 640));
        sprite.tile = uint32_t(range(0, 7));
        sprite.palette = uint8_t((range(0, 3) << 6) | range(0, 0x3f));
        sprite.flip_x = range(0, 1) != 0;
        sprite.flip_y = range(0, 1) != 0;
        sprites.push_back(sprite);
    }
    for (const unsigned border : {0u, 40u}) {
        f3rt::GameVideoOptions options;
        options.scale = 1;
        options.border = border;
        std::vector<uint16_t> expected(f3rt::sprite_plane_size(options));
        f3rt::raster_sprites(sprites, 0x1f, sprite_tiles, expected, {options});
        f3rt::SpriteBins bins;
        bins.bin(sprites, {options});
        f3rt::IndexedScratch scratch;
        scratch.configure(options);
        // The chunk raster must not depend on what the plane held before.
        std::fill(scratch.plane.begin(), scratch.plane.end(), uint16_t(0xabcd));
        EXPECT_EQ(bins.chunk_count(), (f3rt::compose_chunk_count()));
        auto coverage = scratch.coverage(options);
        for (unsigned chunk = 0; chunk < bins.chunk_count(); ++chunk)
            f3rt::raster_sprite_chunk(bins, sprites, chunk, 0x1f, sprite_tiles, scratch.plane,
                                      {options}, coverage);
        const auto written = covered_plane(scratch.plane, options, coverage);
        size_t mismatches = 0, first = SIZE_MAX;
        for (size_t i = 0; i < expected.size(); ++i)
            if (expected[i] != written[i]) {
                if (first == SIZE_MAX) first = i;
                ++mismatches;
            }
        EXPECT_EQ(mismatches, 0u) << "border=" << border << " first=" << first;
    }
}

// The unscaled-sprite raster path must equal the general per-column raster. This reference is the
// general algorithm written out plainly: nominal-rectangle cull, per-column and per-row fixed-point
// positions, and first write wins.
namespace {
std::vector<uint16_t> reference_sprite_raster(std::span<const f3rt::SceneSprite> sprites,
                                        uint8_t pen_mask, std::span<const uint8_t> tiles,
                                        f3rt::GameVideoOptions options) {
    const bool expanded = options.expanded();
    const int width = expanded ? int(options.width()) : 432;
    const int origin_x = expanded ? 46 - int(options.border) : 0;
    const int origin_y = expanded ? int(f3rt::geometry::FIRST_LINE) : 0;
    const int left = expanded ? 0 : 46, right = expanded ? width : 366;
    const int top = expanded ? 0 : int(f3rt::geometry::FIRST_LINE);
    const int bottom = expanded ? int(options.height()) : int(f3rt::geometry::END_LINE);
    std::vector<uint16_t> plane(f3rt::sprite_plane_size(options), 0);
    for (size_t i = sprites.size(); i; --i) {
        const auto &sprite = sprites[i - 1];
        if (sprite.x + sprite.scale_x * 16 <= (46 - int(options.border)) * 256 ||
            sprite.x > (365 + int(options.border)) * 256 ||
            sprite.y + sprite.scale_y * 16 <= int(f3rt::geometry::FIRST_LINE) * 256 ||
            sprite.y > 255 * 256)
            continue;
        const uint8_t *pixels = tiles.data() + size_t(sprite.tile % (tiles.size() / 256)) * 256;
        for (int y = 0; y < 16; ++y) {
            const int position_y = sprite.y + y * sprite.scale_y + 255;
            const int start_y = (position_y >> 8) - origin_y;
            const int end_y =
                std::max(start_y + 1, ((position_y + sprite.scale_y) >> 8) - origin_y);
            if (end_y <= top || start_y >= bottom) continue;
            for (int x = 0; x < 16; ++x) {
                const int position_x = sprite.x + x * sprite.scale_x + 128;
                const int start_x = (position_x >> 8) - origin_x;
                const int end_x = ((position_x + sprite.scale_x) >> 8) - origin_x;
                if (start_x == end_x || end_x <= left || start_x >= right) continue;
                const uint8_t pen =
                    pixels[(y ^ (sprite.flip_y ? 15 : 0)) * 16 + (x ^ (sprite.flip_x ? 15 : 0))] &
                    pen_mask;
                if (!pen) continue;
                const uint16_t color = uint16_t(0x1000 + (unsigned(sprite.palette) << 4) + pen);
                const int first_x = std::max(left, start_x), last_x = std::min(right, end_x);
                for (int dy = std::max(top, start_y); dy < std::min(bottom, end_y); ++dy)
                    for (int dx = first_x; dx < last_x; ++dx)
                        if (!plane[size_t(dy) * width + dx]) plane[size_t(dy) * width + dx] = color;
            }
        }
    }
    return plane;
}

// The coverage a plane implies: per row and sprite layer, the picture columns whose plane value
// belongs to the layer, and the mask of layers present.
size_t coverage_mismatches(std::span<const uint16_t> plane, f3rt::GameVideoOptions options,
                           const f3rt::SpriteCoverage &coverage) {
    const bool expanded = options.expanded();
    const unsigned width = expanded ? options.width() : 432;
    const unsigned left = expanded ? 0 : 46;
    const unsigned rows = expanded ? options.height() : 256;
    size_t mismatches = 0;
    for (unsigned row = 0; row < rows; ++row) {
        uint8_t layers = 0;
        for (unsigned layer = 0; layer < f3rt::SpriteCoverage::LAYERS; ++layer) {
            std::vector<uint32_t> want(coverage.words, 0);
            for (unsigned x = left; x < left + options.width(); ++x) {
                const uint16_t value = plane[size_t(row) * width + x];
                if (!value || unsigned(f3rt::sprite_layer(value)) - unsigned(f3rt::LayerId::Sp0) != layer)
                    continue;
                want[(x - left) / 32] |= 1u << ((x - left) % 32);
                layers |= uint8_t(1u << layer);
            }
            const auto got = coverage.row(row, layer);
            if (!std::equal(want.begin(), want.end(), got.begin())) ++mismatches;
        }
        if (layers != coverage.layers[row]) ++mismatches;
    }
    return mismatches;
}
} // namespace

TEST(GameCompositor, SpriteRasterMatchesGeneralReference) {
    std::mt19937_64 rng(777);
    const auto range = [&rng](unsigned lo, unsigned hi) {
        return unsigned(rng() % (hi - lo + 1)) + lo;
    };
    std::vector<uint8_t> tiles(8 * 256);
    for (auto &byte : tiles) byte = range(0, 2) ? uint8_t(rng() & 0x3f) : uint8_t(0);
    // Border 0 is the fixed native plane; a border selects the expanded plane. Both serve scale 1.
    for (const unsigned border : {0u, 40u}) {
        f3rt::GameVideoOptions geometry;
        geometry.scale = 1;
        geometry.border = border;
        std::vector<f3rt::SceneSprite> sprites;
        for (unsigned i = 0; i < 400; ++i) {
            f3rt::SceneSprite sprite;
            sprite.x = int32_t(int(rng() % (560 * 256)) - 100 * 256) + int32_t(range(0, 255));
            sprite.y = int32_t(int(rng() % (300 * 256)) - 50 * 256) + int32_t(range(0, 255));
            const bool unscaled = range(0, 9) < 7;
            sprite.scale_x = unscaled ? 256 : uint16_t(range(200, 330));
            sprite.scale_y = unscaled ? 256 : uint16_t(range(200, 330));
            sprite.tile = uint32_t(range(0, 11));
            sprite.palette = uint8_t(rng());
            sprite.flip_x = range(0, 1) != 0;
            sprite.flip_y = range(0, 1) != 0;
            sprites.push_back(sprite);
        }
        const std::vector<uint16_t> expected = reference_sprite_raster(sprites, 0x3f, tiles, geometry);
        std::vector<uint16_t> whole(f3rt::sprite_plane_size(geometry));
        f3rt::raster_sprites(sprites, 0x3f, tiles, whole, {geometry});
        EXPECT_EQ(whole, expected) << "border=" << border << " whole plane";
        f3rt::SpriteBins bins;
        bins.bin(sprites, {geometry});
        f3rt::IndexedScratch scratch;
        scratch.configure(geometry);
        std::fill(scratch.plane.begin(), scratch.plane.end(), uint16_t(0xabcd));
        auto coverage = scratch.coverage(geometry);
        for (unsigned chunk = 0; chunk < bins.chunk_count(); ++chunk)
            f3rt::raster_sprite_chunk(bins, sprites, chunk, 0x3f, tiles, scratch.plane, {geometry},
                                      coverage);
        EXPECT_EQ(covered_plane(scratch.plane, geometry, coverage), expected)
            << "border=" << border << " chunked plane";
        EXPECT_EQ(coverage_mismatches(expected, geometry, coverage), 0u)
            << "border=" << border << " coverage";
    }
}

// Priority ties across blend modes are what stop the first-visible-layer shortcut, but a sprite layer
// that drew nothing on a row takes no part in it. A frame whose sprites all sit on sp0 therefore
// stays on the indexed kernel even though sp0 and sp1 tie in priority with different modes; one
// sp1 pixel makes exactly its row fall back to the canonical row kernel. Both must match the
// canonical composition.
namespace {
f3rt::CapturedFrame priority_tie_frame(bool with_sp1_pixel, std::vector<uint8_t> &sprite_tiles) {
    f3rt::CapturedFrame frame;
    frame.fallback = false;
    frame.pen_mask = 0x0f;
    for (unsigned i = 0; i < frame.colors.size(); ++i)
        frame.colors[i] = (i * 2654435761u) & 0xffffffu;
    for (auto &row : frame.rows) {
        row.background = 7;
        row.blend = {4, 4, 4, 4};
        auto &sp0 = row.layer(f3rt::LayerId::Sp0);
        sp0.enabled = true;
        sp0.priority = 5;
        sp0.blend_mode = 0;
        auto &sp1 = row.layer(f3rt::LayerId::Sp1);
        sp1.enabled = true;
        sp1.priority = 5;
        sp1.blend_mode = 1;
    }
    // Tile 0 is solid; tile 1 has a single opaque texel (top-left).
    sprite_tiles.assign(2 * 256, 0);
    std::fill_n(sprite_tiles.begin(), 256, uint8_t(1));
    sprite_tiles[256] = 1;
    const int32_t top = int32_t(f3rt::geometry::FIRST_LINE + 40) * 256;
    auto &solid = frame.sprites[0];
    solid.x = 150 * 256;
    solid.y = top;
    solid.scale_x = solid.scale_y = 256;
    solid.tile = 0;
    solid.palette = 1; // pens land on sp0
    frame.sprite_count = 1;
    if (with_sp1_pixel) {
        auto &single = frame.sprites[1];
        single.x = 250 * 256;
        single.y = top;
        single.scale_x = single.scale_y = 256;
        single.tile = 1;
        single.palette = 0x40; // pens land on sp1
        frame.sprite_count = 2;
    }
    return frame;
}
} // namespace

TEST(GameCompositor, IndexedPriorityTieRowsFallBackOnlyWhenBothLayersDraw) {
    std::vector<uint8_t> tile_pixels(256, 0), sprite_tiles;
    for (const unsigned border : {0u, 40u}) {
        f3rt::IndexedScratch scratch;
        const auto sp0_only = priority_tie_frame(false, sprite_tiles);
        const auto before =
            compare_indexed_frame(sp0_only, tile_pixels, sprite_tiles, border, scratch);
        EXPECT_EQ(before.fallback_rows, 0u) << "border=" << border;
        EXPECT_EQ(before.mismatches, 0u) << "border=" << border;
        EXPECT_EQ(before.plane_mismatches, 0u) << "border=" << border;
        const auto with_sp1 = priority_tie_frame(true, sprite_tiles);
        const auto after =
            compare_indexed_frame(with_sp1, tile_pixels, sprite_tiles, border, scratch);
        EXPECT_EQ(after.fallback_rows, 1u) << "border=" << border;
        EXPECT_EQ(after.mismatches, 0u) << "border=" << border;
        EXPECT_EQ(after.plane_mismatches, 0u) << "border=" << border;
    }
}

// RapidCheck property: random synthetic scenes exercise what Land Maker never does - clip windows
// (including inverted and multi-range), mosaic, blur, 15-bit words, flipscreen, alternate maps,
// priority ties, all blend modes and weights that do not sum to eight.
RC_GTEST_PROP(GameCompositor, IndexedMatchesReferenceSynthetic, ()) {
    const auto seed = *rc::gen::arbitrary<uint64_t>();
    const auto border = *rc::gen::element(0u, 40u);
    std::mt19937_64 rng(seed);
    const f3rt::CapturedFrame frame = synthetic_frame(rng);
    std::vector<uint8_t> tile_pixels(4 * 256), sprite_tiles(4 * 256);
    // Transparent texels, whole transparent tile rows and whole opaque ones, as in real tile ROMs.
    for (size_t row = 0; row < tile_pixels.size() / 16; ++row) {
        const bool blank = rng() % 8 == 0, solid = !blank && rng() % 3 == 0;
        for (size_t i = 0; i < 16; ++i)
            tile_pixels[row * 16 + i] = blank                    ? uint8_t(0)
                                        : solid                  ? uint8_t(1 + rng() % 15)
                                        : rng() % 4 == 0 ? uint8_t(0)
                                                         : uint8_t(rng());
    }
    for (auto &byte : sprite_tiles) byte = uint8_t(rng());
    f3rt::IndexedScratch scratch;
    const auto result = compare_indexed_frame(frame, tile_pixels, sprite_tiles, border, scratch);
    RC_ASSERT(result.mismatches == 0);
    RC_ASSERT(result.plane_mismatches == 0);
}
#endif
