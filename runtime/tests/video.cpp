#include "f3rt/machine.hpp"
#include "f3rt/video.hpp"
#include "f3rt/game_video.hpp"
#include "f3rt/input.hpp"
#include "f3rt/memory_map.hpp"
#include "f3rt/renderer/graphics_assets.hpp"
#include "renderer/game/scene.hpp"
#include "renderer/game/scene_snapshot.hpp"
#include "renderer/game/video.hpp"
#include "renderer/game/oracle.hpp"
#include "f3rt/renderer/game/enhancements.hpp"
#include "sprites/sprite_system.hpp"
#ifdef F3RT_GPU
#include "renderer/gpu/video.hpp"
#include <SDL3/SDL.h>
#endif
#include "runner.hpp"
#include "support.hpp"
#include <gtest/gtest.h>
#include <rapidcheck/gtest.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <random>
#include <sstream>
#include <string_view>
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

using f3rt::SceneClip;

void expect_clip_ranges(const std::array<SceneClip, 4> &windows, unsigned c, unsigned i, bool invert,
                        const std::vector<SceneClip> &expected) {
    SCOPED_TRACE(::testing::Message() << "c=" << c << " I=" << invert << " i=" << i);
    const auto ranges = f3rt::clip_ranges(windows, c, i, invert, 46, 66);
    EXPECT_EQ(std::vector<SceneClip>(ranges.begin(), ranges.end()), expected);
}

TEST(ClipWindows, AllWindowsEnabled) {
    const std::array<SceneClip, 4> windows{{{47, 49}, {51, 53}, {55, 57}, {59, 61}}};
    expect_clip_ranges(windows, 15, 15, false, {{47, 49}, {51, 53}, {55, 57}, {59, 61}});
    expect_clip_ranges(windows, 15, 15, true, {{46, 47}, {49, 51}, {53, 55}, {57, 59}, {61, 66}});
    expect_clip_ranges(windows, 15, 14, false, {{46, 47}, {49, 66}});

    const std::array<SceneClip, 4> mixed{{{47, 51}, {52, 55}, {56, 60}, {46, 63}}};
    expect_clip_ranges(mixed, 15, 7, false, {{47, 51}, {52, 55}, {56, 60}, {63, 66}});
    const std::array<SceneClip, 4> five_ranges{{{49, 51}, {52, 55}, {56, 60}, {47, 63}}};
    expect_clip_ranges(five_ranges, 15, 7, false,
                       {{46, 47}, {49, 51}, {52, 55}, {56, 60}, {63, 66}});
}

TEST(ClipWindows, SingleWindowModes) {
    const std::array<SceneClip, 4> windows{{{50, 60}}};
    expect_clip_ranges(windows, 1, 0, false, {{46, 50}, {60, 66}});
    expect_clip_ranges(windows, 1, 0, true, {{50, 60}});
    expect_clip_ranges(windows, 1, 1, false, {{50, 60}});
    expect_clip_ranges(windows, 1, 1, true, {{46, 50}, {60, 66}});
}

TEST(ClipWindows, UnionThenComplement) {
    const std::array<SceneClip, 4> disjoint{{{50, 53}, {56, 60}}};
    expect_clip_ranges(disjoint, 3, 3, false, {{50, 53}, {56, 60}});
    expect_clip_ranges(disjoint, 3, 3, true, {{46, 50}, {53, 56}, {60, 66}});
    expect_clip_ranges(disjoint, 3, 0, false, {{46, 66}});
    expect_clip_ranges(disjoint, 3, 0, true, {});

    const std::array<SceneClip, 4> overlap{{{50, 57}, {55, 60}}};
    expect_clip_ranges(overlap, 3, 3, false, {{50, 60}});
    expect_clip_ranges(overlap, 3, 3, true, {{46, 50}, {60, 66}});
    expect_clip_ranges(overlap, 3, 0, false, {{46, 55}, {57, 66}});
    expect_clip_ranges(overlap, 3, 0, true, {{55, 57}});
}

TEST(ClipWindows, AllWindowsDisabled) {
    const std::array<SceneClip, 4> windows{{{50, 53}, {56, 60}}};
    for (unsigned i = 0; i < 16; ++i) {
        expect_clip_ranges(windows, 0, i, false, {});
        expect_clip_ranges(windows, 0, i, true, {{46, 66}});
    }
}

TEST(ClipWindows, EmptyAndReversedWindows) {
    for (const auto window : {SceneClip{55, 55}, SceneClip{55, 52}}) {
        const std::array<SceneClip, 4> windows{{window}};
        expect_clip_ranges(windows, 1, 0, false, {{46, 66}});
        expect_clip_ranges(windows, 1, 0, true, {});
        expect_clip_ranges(windows, 1, 1, false, {});
        expect_clip_ranges(windows, 1, 1, true, {{46, 66}});
    }
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
    set_word(graphics, PF_ALT_ENABLE_0, 0x3001);
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
    set_word(graphics, PF_ALT_ENABLE_1, 0x3001);
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
        set_word(graphics, PF_ALT_ENABLE_0, pf == 2 ? 0x3001 : 0);
        set_word(graphics, PF_ALT_ENABLE_1, pf == 3 ? 0x3001 : 0);
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


namespace {
class GpuReference {
public:
    GpuReference(std::span<const uint8_t> playfield, std::span<const uint8_t> sprites)
        : m_playfield(playfield), m_sprites(sprites) {}
    bool available() { return render_at(0) != nullptr; }
    std::optional<std::vector<uint32_t>> render(const f3rt::SceneSnapshot &frame,
                                                f3rt::GameVideoOptions options) {
        std::vector<uint32_t> output(size_t(options.width()) * options.height());
#ifdef F3RT_GPU
        auto *gpu = render_at(options.border);
        if (!gpu) return std::nullopt;
        gpu->render(frame, output);
        return output;
#else
        (void)frame;
        return std::nullopt;
#endif
    }

private:
#ifdef F3RT_GPU
    f3rt::GpuVideo *render_at(unsigned border) {
        if (m_failed) return nullptr;
        auto found = m_gpu.find(border);
        if (found != m_gpu.end()) return found->second.get();
        try {
            if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) throw std::runtime_error(SDL_GetError());
            f3rt::GameVideoOptions options;
            options.border = border;
            return (m_gpu[border] = std::make_unique<f3rt::GpuVideo>(nullptr, options, m_playfield,
                                                                     m_sprites, false, false))
                .get();
        } catch (const std::exception &) {
            m_failed = true;
            return nullptr;
        }
    }
    std::map<unsigned, std::unique_ptr<f3rt::GpuVideo>> m_gpu;
    bool m_failed = false;
#else
    void *render_at(unsigned) { return nullptr; }
#endif
    std::span<const uint8_t> m_playfield, m_sprites;
};

struct IndexedComparison {
    size_t pixels = 0, mismatches = 0;
    size_t first_pixel = SIZE_MAX;
};

// Compose `frame` with the CPU indexed compositor in both output byte orders and compare it with
// the GPU scene pass: the same scene snapshot must produce identical pixels.
IndexedComparison compare_indexed_frame(const f3rt::SceneSnapshot &frame,
                                        const f3rt::GraphicsAssets &assets, unsigned border,
                                        GpuReference &gpu) {
    f3rt::GameVideoOptions options;
    options.scale = 1;
    options.border = border;
    const size_t pixel_count = size_t(options.width()) * options.height();
    auto expected = gpu.render(frame, options);
    if (!expected) throw std::runtime_error("GPU reference unavailable");
    IndexedComparison result;
    result.pixels = pixel_count;
    std::vector<uint32_t> indexed(pixel_count);
    frame.compose(indexed, options.width(), f3rt::PixelFormat::Argb8888, border, assets);
    for (size_t i = 0; i < pixel_count; ++i)
        if (indexed[i] != (*expected)[i]) {
            if (result.first_pixel == SIZE_MAX) result.first_pixel = i;
            ++result.mismatches;
        }
    std::vector<uint32_t> staging(pixel_count);
    frame.compose(staging, options.width(), f3rt::PixelFormat::Rgba8888, border, assets);
    for (size_t i = 0; i < pixel_count; ++i)
        if (staging[i] != f3rt::to_pixel_format((*expected)[i], f3rt::PixelFormat::Rgba8888)) {
            if (result.first_pixel == SIZE_MAX) result.first_pixel = i;
            ++result.mismatches;
        }
    return result;
}
} // namespace

TEST(GameCompositor, IndexedMatchesGpu) {
    const char *rom_env = std::getenv("F3_ROM_DIR");
#ifdef F3RT_DEFAULT_ROM_DIR
    if (!rom_env) rom_env = F3RT_DEFAULT_ROM_DIR;
#endif
    if (!rom_env || !*rom_env)
        GTEST_SKIP() << "F3_ROM_DIR not set; skipping ROM-backed indexed compositor check";
    size_t frames = 0, mismatches = 0;
    for (const bool play : {false, true}) {
        std::unique_ptr<f3rt::Machine> machine;
        try {
            machine = f3rt::runner::create_strict_machine(std::filesystem::path(rom_env),
                                                          F3RT_DEFAULT_SET, true);
        } catch (const std::exception &error) {
            GTEST_SKIP() << "Strict-native machine unavailable: " << error.what();
        }
        machine->sprites = f3rt::make_game_sprites(*machine);
        f3rt::runner::SeededScheduleInputSource schedule(
            1, f3rt::runner::ScheduleConfig{.versus = false});
        GpuReference gpu(machine->graphics_assets.playfield_tiles(),
                         machine->graphics_assets.sprite_tiles());
        if (!gpu.available()) GTEST_SKIP() << "No GPU device for the reference render";
        for (unsigned index = 0; index < 3600; ++index) {
            if (play) schedule.apply(*machine, index);
            else f3rt::apply_local_inputs(*machine, {});
            ASSERT_TRUE(machine->run_frame());
            if (index < 600 || index % 30) continue;
            const auto &captured = machine->game_video->scene_snapshot();
            ASSERT_FALSE(captured.fallback_frame()) << "frame " << index << " must be a native scanout";
            for (const unsigned border : {0u, 40u}) {
                const auto result = compare_indexed_frame(
                    captured, machine->graphics_assets, border, gpu);
                ++frames;
                mismatches += result.mismatches;
                EXPECT_EQ(result.mismatches, 0u)
                    << "frame " << index << " border " << border << " first pixel "
                    << result.first_pixel;
            }
        }
    }
    std::printf("indexed compositor: frames=%zu mismatches=%zu\n", frames, mismatches);
    EXPECT_EQ(mismatches, 0u);
}

namespace {
constexpr std::array<uint8_t, 128> LANDMAKR_INIT_EEPROM{
    0x54, 0x41, 0x49, 0x54, 0x4f, 0x01, 0x00, 0x07, 0xa2, 0x06, 0xe0, 0x01, 0x11, 0x11, 0x30, 0x20,
    0x00, 0x00, 0x02, 0x01, 0x88, 0x88, 0x88, 0x88, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x25, 0x1a,
    0x00, 0x00, 0xc3, 0x50, 0x05, 0x00, 0x54, 0x53, 0x55, 0x00, 0x00, 0x00, 0x00, 0x00, 0x9c, 0x40,
    0x04, 0x00, 0x54, 0x45, 0x52, 0x00, 0x00, 0x00, 0x00, 0x00, 0x75, 0x30, 0x03, 0x00, 0x54, 0x4d,
    0x52, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4e, 0x20, 0x02, 0x00, 0x48, 0x41, 0x53, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x27, 0x10, 0x01, 0x00, 0x4e, 0x4f, 0x42, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x85, 0xac};

struct BenchWindow {
    const char *name;
    unsigned first, last, stride;
};
constexpr std::array<BenchWindow, 6> BENCH_WINDOWS{{
    // The event scheduler moves the attract-mode cross-fade earlier.
    {"portrait cross-fade", 1323, 1471, 1},
    {"attract to tutorial", 4844, 5045, 1},
    {"map and VS screens", 14001, 14036, 1},
    {"gameplay priority ties", 15399, 15403, 1},
    {"gameplay sprite layers", 16993, 17000, 1},
    {"dense gameplay", 17144, 17150, 1},
}};
constexpr unsigned BENCH_FRAMES = 18580, BENCH_BACKGROUND_STRIDE = 37;
} // namespace


TEST(GameCompositor, IndexedMatchesGpuOnBenchTimeline) {
    const char *rom_env = std::getenv("F3_ROM_DIR");
#ifdef F3RT_DEFAULT_ROM_DIR
    if (!rom_env) rom_env = F3RT_DEFAULT_ROM_DIR;
#endif
    if (!rom_env || !*rom_env)
        GTEST_SKIP() << "F3_ROM_DIR not set; skipping ROM-backed bench timeline check";
    if (std::string_view(F3RT_DEFAULT_SET) != "landmakrj")
        GTEST_SKIP() << "The bench timeline is Land Maker's";
    const auto script_path =
        std::filesystem::path(__FILE__).parent_path() / "data" / "landmakrj-timeline.txt";
    std::unique_ptr<f3rt::Machine> machine;
    try {
        machine = f3rt::runner::create_strict_machine(std::filesystem::path(rom_env),
                                                      F3RT_DEFAULT_SET, true);
    } catch (const std::exception &error) {
        GTEST_SKIP() << "Strict-native machine unavailable: " << error.what();
    }
    machine->sprites = f3rt::make_game_sprites(*machine);
    GpuReference gpu(machine->graphics_assets.playfield_tiles(),
                     machine->graphics_assets.sprite_tiles());
    if (!gpu.available()) GTEST_SKIP() << "No GPU device for the reference render";
    const auto eeprom_path = std::filesystem::temp_directory_path() / "f3rt-bench-timeline.eeprom";
    {
        std::ofstream file(eeprom_path, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char *>(LANDMAKR_INIT_EEPROM.data()),
                   std::streamsize(LANDMAKR_INIT_EEPROM.size()));
    }
    machine->load_eeprom(eeprom_path);
    std::filesystem::remove(eeprom_path);
    auto input = f3rt::runner::ScriptInputSource::from_file(script_path.string());

    std::array<size_t, BENCH_WINDOWS.size() + 1> frames{}, mismatches{};
    for (unsigned index = 0; index < BENCH_FRAMES; ++index) {
        input->apply(*machine, index);
        ASSERT_TRUE(machine->run_frame());
        const unsigned frame = index + 1;
        size_t slot = BENCH_WINDOWS.size();
        bool sampled = frame % BENCH_BACKGROUND_STRIDE == 0 && frame >= 479;
        for (size_t w = 0; w < BENCH_WINDOWS.size(); ++w) {
            const auto &window = BENCH_WINDOWS[w];
            if (frame >= window.first && frame <= window.last &&
                (frame - window.first) % window.stride == 0) {
                slot = w;
                sampled = true;
            }
        }
        if (!sampled) continue;
        const auto &captured = machine->game_video->scene_snapshot();
        ASSERT_FALSE(captured.fallback_frame()) << "frame " << frame << " must be a native scanout";
        for (const unsigned border : {0u, 40u}) {
            const auto result =
                compare_indexed_frame(captured, machine->graphics_assets, border, gpu);
            ++frames[slot];
            mismatches[slot] += result.mismatches;
            EXPECT_EQ(result.mismatches, 0u)
                << "frame " << frame << " border " << border << " first pixel "
                << result.first_pixel;
        }
    }
    for (size_t w = 0; w <= BENCH_WINDOWS.size(); ++w) {
        const char *name = w < BENCH_WINDOWS.size() ? BENCH_WINDOWS[w].name : "background";
        std::printf("bench timeline %-24s frames=%zu mismatches=%zu\n", name, frames[w],
                    mismatches[w]);
        EXPECT_GT(frames[w], 0u) << name;
    }
    EXPECT_EQ(std::accumulate(mismatches.begin(), mismatches.end(), size_t(0)), 0u);
}


namespace {
std::unique_ptr<f3rt::Machine> strict_machine_or_skip(bool game_video) {
    const char *rom_env = std::getenv("F3_ROM_DIR");
#ifdef F3RT_DEFAULT_ROM_DIR
    if (!rom_env) rom_env = F3RT_DEFAULT_ROM_DIR;
#endif
    if (!rom_env || !*rom_env) return nullptr;
    try {
        return f3rt::runner::create_strict_machine(std::filesystem::path(rom_env), F3RT_DEFAULT_SET,
                                                   game_video);
    } catch (const std::exception &) {
        return nullptr;
    }
}
} // namespace

TEST(GameVideo, LazyNativeFrameMatchesFdpOracle) {
    std::vector<std::vector<uint32_t>> game_frames, oracle_frames;
    for (const bool game_video : {true, false}) {
        auto machine = strict_machine_or_skip(game_video);
        if (!machine) GTEST_SKIP() << "ROM-backed strict-native machine unavailable";
        std::optional<f3rt::Video> oracle;
        if (game_video) {
            machine->sprites = f3rt::make_game_sprites(*machine);
        } else {
            oracle.emplace(machine->graphics_assets, machine->roms.video);
            machine->inspect_scanout = [&](std::span<uint32_t> pixels) {
                oracle->render_frame(machine->palette, machine->graphics, machine->control,
                                     pixels);
            };
        }
        f3rt::runner::SeededScheduleInputSource input(
            1, f3rt::runner::ScheduleConfig{.versus = false});
        auto &frames = game_video ? game_frames : oracle_frames;
        for (unsigned index = 0; index < 1800; ++index) {
            input.apply(*machine, index);
            ASSERT_TRUE(machine->run_frame());
            if (index % 5 == 0) frames.push_back(machine->native_pixels());
        }
    }
    ASSERT_EQ(game_frames.size(), oracle_frames.size());
    for (size_t i = 0; i < game_frames.size(); ++i)
        EXPECT_EQ(game_frames[i], oracle_frames[i]) << "frame " << i * 5;
}

TEST(GameVideo, SaveLoadPreservesNativeFrameAndFutureFrames) {
    struct Observation {
        std::vector<uint32_t> pixels;
        uint32_t crc;
    };
    const auto observe = [](f3rt::Machine &m) { return Observation{m.native_pixels(), m.state_crc()}; };
    std::vector<uint8_t> state;
    Observation at_save;
    std::vector<Observation> future;
    {
        auto original = strict_machine_or_skip(true);
        if (!original) GTEST_SKIP() << "ROM-backed strict-native machine unavailable";
        f3rt::runner::SeededScheduleInputSource input(
            1, f3rt::runner::ScheduleConfig{.versus = false});
        for (unsigned index = 0; index < 900; ++index) {
            input.apply(*original, index);
            ASSERT_TRUE(original->run_frame());
        }
        state.resize(original->state_size());
        original->save_state(state);
        at_save = observe(*original);
        for (unsigned index = 900; index < 1200; ++index) {
            input.apply(*original, index);
            ASSERT_TRUE(original->run_frame());
            if (index % 7 == 0) future.push_back(observe(*original));
        }
    }
    // The audio board and EEPROM are process-wide: the restored machine replaces the original.
    auto restored = strict_machine_or_skip(true);
    ASSERT_TRUE(restored) << "ROM-backed strict-native machine unavailable";
    restored->load_state(state);
    EXPECT_EQ(restored->native_pixels(), at_save.pixels);
    EXPECT_EQ(restored->state_crc(), at_save.crc);
    f3rt::runner::SeededScheduleInputSource input(1, f3rt::runner::ScheduleConfig{.versus = false});
    for (unsigned index = 0; index < 900; ++index) input.step(index); // replay the schedule up to the save point
    size_t next = 0;
    for (unsigned index = 900; index < 1200; ++index) {
        input.apply(*restored, index);
        ASSERT_TRUE(restored->run_frame());
        if (index % 7 == 0) {
            const Observation now = observe(*restored);
            EXPECT_EQ(now.pixels, future[next].pixels) << "frame " << index;
            EXPECT_EQ(now.crc, future[next].crc) << "frame " << index;
            ++next;
        }
    }
}

TEST(SceneSnapshot, PresentsEnhancedSpritesTruthTable) {
    auto scene = std::make_unique<f3rt::SceneSnapshot>();
    scene->sprite_count = 1;
    EXPECT_FALSE(scene->presents_enhanced_sprites());

    scene->fallback = false;
    scene->canonical_sprite_count = 1;
    EXPECT_FALSE(scene->presents_enhanced_sprites());

    scene->sprite_count = 2;
    EXPECT_TRUE(scene->presents_enhanced_sprites());

    scene->sprite_count = 1;
    scene->sprites[0].blend_weight = 0.5f;
    ASSERT_EQ(scene->sprites[0], scene->canonical_sprites[0]);
    EXPECT_TRUE(scene->presents_enhanced_sprites());
}

TEST(SceneSnapshot, SpriteListSelectsCanonicalOrPresented) {
    auto scene = std::make_unique<f3rt::SceneSnapshot>();
    scene->sprite_count = 2;
    scene->canonical_sprite_count = 1;
    const auto presented = scene->sprite_list(f3rt::SpriteList::Presented);
    const auto canonical = scene->sprite_list(f3rt::SpriteList::Canonical);
    EXPECT_EQ(presented.data(), scene->sprites.data());
    EXPECT_EQ(presented.size(), 2u);
    EXPECT_EQ(canonical.data(), scene->canonical_sprites.data());
    EXPECT_EQ(canonical.size(), 1u);
}

TEST(SceneSnapshot, CaptureFallbackFillsBlackOrCopiesNative) {
    auto scene = std::make_unique<f3rt::SceneSnapshot>();
    scene->fallback = false;
    scene->native_pixels.fill(0x12345678);
    EXPECT_EQ(&scene->capture_fallback(), scene.get());
    EXPECT_TRUE(scene->fallback);
    EXPECT_TRUE(std::ranges::all_of(scene->native_pixels, [](uint32_t p) { return p == 0xff000000u; }));

    std::vector<uint32_t> native(f3rt::geometry::NATIVE_PIXELS);
    for (size_t i = 0; i < native.size(); ++i) native[i] = 0xff000000u | uint32_t(i);
    scene->fallback = false;
    scene->capture_fallback(native);
    EXPECT_TRUE(scene->fallback);
    EXPECT_TRUE(std::ranges::equal(scene->native_pixels, native));
}

TEST(SceneSnapshot, ComposeFallbackPlacesNativeFrameInsideBorder) {
    auto scene = std::make_unique<f3rt::SceneSnapshot>();
    std::vector<uint32_t> native(f3rt::geometry::NATIVE_PIXELS);
    for (size_t i = 0; i < native.size(); ++i) native[i] = 0xff000000u | uint32_t(i);
    scene->capture_fallback(native);
    const f3rt::GraphicsAssets assets;
    for (const unsigned border : {0u, 40u}) {
        f3rt::GameVideoOptions options;
        options.scale = 1;
        options.border = border;
        std::vector<uint32_t> serial(size_t(options.width()) * options.height(), 1u),
            parallel(serial.size(), 2u);
        scene->compose(serial, options.width(), f3rt::PixelFormat::Argb8888, border, assets);
        scene->compose(parallel, options.width(), f3rt::PixelFormat::Argb8888, border, assets);
        EXPECT_EQ(serial, parallel) << "border=" << border;
        for (unsigned y = 0; y < options.height(); ++y) {
            for (unsigned x = 0; x < options.width(); ++x) {
                const bool inside = x >= border && x < border + f3rt::geometry::NATIVE_WIDTH;
                const uint32_t expected =
                    inside ? native[y * f3rt::geometry::NATIVE_WIDTH + x - border] : 0xff000000u;
                ASSERT_EQ(serial[size_t(y) * options.width() + x], expected)
                    << "border=" << border << " x=" << x << " y=" << y;
            }
        }
    }
}

TEST(SceneSnapshot, ComposeRejectsOutOfRangeBorderAndShortBuffers) {
    auto scene = std::make_unique<f3rt::SceneSnapshot>();
    const f3rt::GraphicsAssets assets;
    f3rt::GameVideoOptions options;
    std::vector<uint32_t> output(size_t(options.width()) * options.height());
    EXPECT_THROW(scene->compose(output, options.width(), f3rt::PixelFormat::Argb8888,
                                f3rt::GameVideoOptions::MAX_BORDER + 1, assets),
                 std::runtime_error);
    output.resize(size_t(options.width()) * options.height() - 1);
    EXPECT_THROW(
        scene->compose(output, options.width(), f3rt::PixelFormat::Argb8888, 0, assets),
        std::runtime_error);
}

TEST(GameVideo, NullSnapshotTargetRestoresInternalSlot) {
    auto machine = strict_machine_or_skip(true);
    if (!machine) GTEST_SKIP() << "ROM-backed strict machine unavailable";
    auto external = std::make_unique<f3rt::SceneSnapshot>();
    machine->game_video->set_snapshot_target(external.get());
    ASSERT_TRUE(machine->run_frame());
    EXPECT_EQ(&machine->game_video->scene_snapshot(), external.get());

    machine->game_video->set_snapshot_target(nullptr);
    ASSERT_TRUE(machine->run_frame());
    EXPECT_NE(&machine->game_video->scene_snapshot(), external.get());
}

TEST(OracleVerifier, RejectsEmptyLayerMask) {
    auto machine = strict_machine_or_skip(true);
    if (!machine) GTEST_SKIP() << "ROM-backed strict machine unavailable";
    f3rt::OracleVerifier oracle(machine->graphics_assets, machine->roms.video);
    oracle.step(*machine);
    EXPECT_THROW(oracle.compare_layers(*machine, *machine->game_video, machine->frame, 0),
                 std::runtime_error);
}

TEST(OracleVerifier, ComparesLayersAndCompositeAgainstGameVideo) {
    auto machine = strict_machine_or_skip(true);
    if (!machine) GTEST_SKIP() << "ROM-backed strict machine unavailable";
    ASSERT_NE(machine->game_video, nullptr);
    f3rt::OracleVerifier oracle(machine->graphics_assets, machine->roms.video);
    for (unsigned frame = 0; frame < 900; ++frame) {
        f3rt::apply_local_inputs(*machine, {});
        ASSERT_TRUE(machine->run_frame());
        oracle.step(*machine);
    }
    const auto &captured = machine->game_video->scene_snapshot();
    ASSERT_FALSE(captured.fallback) << "Frame 900 must be a supported native scanout";
    ASSERT_EQ(oracle.oracle_pixels().size(), size_t(f3rt::geometry::NATIVE_PIXELS));

    EXPECT_NO_THROW(oracle.compare_layers(*machine, *machine->game_video, machine->frame,
                                          f3rt::ALL_LAYERS));
    EXPECT_NO_THROW(oracle.compare_composite(captured, machine->graphics_assets, machine->frame));
    std::ostringstream report;
    oracle.report(report);
    EXPECT_NE(report.str().find("layer=composite"), std::string::npos) << report.str();
    EXPECT_NE(report.str().find("pixel_mismatches=0"), std::string::npos) << report.str();

    auto broken = std::make_unique<f3rt::SceneSnapshot>(captured);
    broken->colors.fill(0x00123456);
    try {
        oracle.compare_composite(*broken, machine->graphics_assets, machine->frame);
        ADD_FAILURE() << "a recolored snapshot must not match the oracle";
    } catch (const std::runtime_error &error) {
        EXPECT_NE(std::string(error.what()).find(std::to_string(machine->frame)), std::string::npos)
            << error.what();
    }

    oracle.reset();
    std::ostringstream cleared;
    oracle.report(cleared);
    EXPECT_TRUE(cleared.str().empty()) << cleared.str();
}

