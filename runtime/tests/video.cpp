#include "f3rt/machine.hpp"
#include "f3rt/video.hpp"
#include "renderer/game/tiles.hpp"
#include "renderer/game/text.hpp"
#include "renderer/game/sprites.hpp"
#include "renderer/game/lines.hpp"
#include "support.hpp"
#include <gtest/gtest.h>
#include <rapidcheck/gtest.h>
#include <algorithm>
#include <array>
#include <vector>

namespace {
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
    for (unsigned i = 0; i < 4; ++i)
        bytes[at + i] = uint8_t(value >> (24 - 8 * i));
}
} // namespace

TEST(Video, RomPlaneLoading4BppAndRayForceVisibleScanline) {
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
    std::vector<uint32_t> pixels(320 * 224);
    set_color(palette, 4, 0x00112233);
    set_color(palette, 8, 0x00445566);
    set_word(graphics, 0x20400 + 31 * 2, 12);
    set_word(graphics, 0x26600 + 31 * 2, 1);
    set_word(graphics, 0x26400 + 31 * 2, 0x6000);
    set_word(graphics, 0x20400 + 32 * 2, 8);
    set_word(graphics, 0x26600 + 32 * 2, 2);
    set_word(graphics, 0, 1);
    set_word(graphics, 4, 46);
    set_word(graphics, 6, 31);
    set_word(graphics, 8, 1);
    set_word(graphics, 16 + 12, 0x8001);
    video.render_frame(palette, graphics, control, pixels);
    EXPECT_TRUE(pixels[0] == 0xff112233 && pixels[320] == 0xff445566)
        << "RayForce's first visible scanline is 31, not Land Maker's 24";
}

TEST(Video, SpriteLagAndDelayedPositionSnapshot) {
    std::vector<uint8_t> sprites(256, 0x21), tiles(128, 0x43);
    f3rt::Video video;
    f3rt::VideoConfig config{90, 2, 31, 224};
    video.load_roms(sprites, {}, tiles, {}, config);

    std::array<uint8_t, 0x8000> palette{};
    std::array<uint8_t, 0x40000> graphics{};
    std::array<uint8_t, 0x20> control{};
    std::vector<uint32_t> pixels(320 * 224);
    set_word(graphics, 0, 1);
    set_word(graphics, 4, 46);
    set_word(graphics, 6, 31);
    set_word(graphics, 8, 1);
    set_word(graphics, 16 + 12, 0x8001);
    video.render_frame(palette, graphics, control, pixels);
    EXPECT_EQ(video.sprite_plane()[31 * 432 + 46], 0)
        << "Lag 2 delays a newly parsed sprite list";

    std::vector<uint8_t> snapshot(video.state_size());
    video.save_state(snapshot);
    set_word(graphics, 4, 50);
    video.render_frame(palette, graphics, control, pixels);
    EXPECT_TRUE(video.sprite_plane()[31 * 432 + 46] == 0x1011 && video.sprite_plane()[31 * 432 + 50] == 0x1011)
        << "Lag 2 draws the previous parsed position, not current sprite RAM";
    video.render_frame(palette, graphics, control, pixels);
    EXPECT_TRUE(video.sprite_plane()[31 * 432 + 46] == 0 && video.sprite_plane()[31 * 432 + 50] == 0x1011)
        << "Lag 2 advances the captured list on the next frame";
    video.load_state(snapshot);
    video.render_frame(palette, graphics, control, pixels);
    EXPECT_EQ(video.sprite_plane()[31 * 432 + 46], 0x1011)
        << "Snapshots preserve the delayed parsed list, not only the framebuffer";
}

TEST(Video, RidingFightGeometryAndFdaPaletteBlur) {
    std::vector<uint8_t> sprites(256, 0x21), tiles(128, 0x43);
    f3rt::Video video;
    f3rt::VideoConfig config{0, 1, 32, 224};
    EXPECT_TRUE(video.load_roms(sprites, {}, tiles, {}, config)) << "Riding Fight geometry loads";
    video.reset();
    std::array<uint8_t, 0x40000> graphics{};
    std::array<uint8_t, 0x8000> palette{};
    std::array<uint8_t, 0x20> control{};
    std::vector<uint32_t> pixels(320 * 224);
    set_color(palette, 0, 0x00abcdee);
    set_color(palette, 4, 0x0000abc8);
    set_color(palette, 8, 0x0000abc4);
    set_color(palette, 12, 0x0000abc2);
    set_color(palette, 16, 0x0000fffe);
    set_color(palette, 20, 0x00abcdef);

    const auto line = [&](unsigned y, uint16_t latch, uint16_t mode, uint16_t background) {
        set_word(graphics, 0x20400 + y * 2, latch);
        set_word(graphics, 0x26400 + y * 2, mode);
        set_word(graphics, 0x26600 + y * 2, background);
    };
    line(32, 12, 0x2000, 1);
    line(33, 8, 0x6000, 2);
    line(34, 12, 0x6000, 3);
    line(35, 0x48, 0x6000, 4);
    set_word(graphics, 0x26c00 + 35 * 2, 0x2000);
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
    sprites[0x400000] = 0x65;
    high[0x200000] = 3;
    EXPECT_TRUE(video.load_roms(sprites, high, tiles, {}, config))
        << "Command War's second sprite bank loads";
    EXPECT_TRUE(video.sprite_tiles()[32768 * 256] == 0x35 && video.sprite_tiles()[32768 * 256 + 1] == 6)
        << "Command War's bank-1 pens include its own high planes, without wrapping to bank 0";
}

TEST(Video, FdpGeometryAndNonPowerOfTwoAssets) {
    std::vector<uint8_t> sprites(3 * 128), tiles(5 * 128);
    for (unsigned tile = 0; tile < 3; ++tile)
        std::fill_n(sprites.begin() + tile * 128, 128, uint8_t((tile + 1) * 0x11));
    for (unsigned tile = 0; tile < 5; ++tile)
        std::fill_n(tiles.begin() + tile * 128, 128, uint8_t((tile + 1) * 0x11));
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
            set_word(graphics, 0x10000 + map * 0x1000 + cell * 4 + 2, uint16_t(map + 5));
    auto line = video.inspect_playfield_line(4, 511, graphics);
    EXPECT_TRUE(line.palette.size() == 512 && line.palette[0] == 5 && line.palette[511] == 5)
        << "Nonextended alternate map 4 uses its own 32-column base and count-correct tile wrapping";
    line = video.inspect_playfield_line(7, 0, graphics);
    EXPECT_EQ(line.palette[0], 3)
        << "Last nonextended physical map remains independently addressable";

    for (unsigned cell = 0; cell < 1024; ++cell) set_word(graphics, 0x12000 + cell * 4 + 2, 0);
    for (unsigned y = 0; y < 3; ++y) {
        set_word(graphics, 0x20000 + y * 2, 4);
        set_word(graphics, 0x24400 + y * 2, y == 1 ? 0x200 : 0);
    }
    set_word(graphics, 0x20400, 6);
    set_word(graphics, 0x26200, 0xff00);
    set_word(graphics, 0x26400, 0x6000);
    set_word(graphics, 0x20e00, 4);
    set_word(graphics, 0x2b400, 0x2001);
    palette[5 * 4 + 1] = 0x12;
    palette[5 * 4 + 2] = 0x34;
    palette[5 * 4 + 3] = 0x56;
    video.render_frame(palette, graphics, control, pixels);
    EXPECT_TRUE(pixels[0] == 0xff000000 && pixels[320] == 0xff123456 && pixels[640] == 0xff000000)
        << "Latched alternate-map selection changes visible pixels and row-usage gating per scanline";
    EXPECT_EQ(pixels[320 * 256], 0xdeadbeef) << "Full-height crop writes exactly 256 rows";

    set_word(graphics, 0x20e00, 12);
    set_word(graphics, 0x2b400, 0);
    set_word(graphics, 0x2b600, 0x2001);
    set_word(graphics, 0x20000, 8);
    set_word(graphics, 0x24600, 0x200);
    palette[4 + 1] = 0x65;
    palette[4 + 2] = 0x43;
    palette[4 + 3] = 0x21;
    video.render_frame(palette, graphics, control, pixels);
    EXPECT_EQ(pixels[0], 0xff654321) << "PF3 alternate selection reads map 5 rather than map 4 or 7";

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
    EXPECT_TRUE(video.sprite_plane()[255 * 432 + 365] == 0x1002 && video.sprite_plane()[254 * 432 + 365] == 0)
        << "Bottom/right crop includes its final pixel without painting outside vertical bounds";
    EXPECT_EQ(pixels[320], 0xdeadbeef) << "One-row crop preserves the output boundary sentinel";

    config = {0, 0, 24, 232, true};
    EXPECT_TRUE(video.load_roms(sprites, {}, tiles, {}, config)) << "Extended geometry accepts independent counts";
    graphics.fill(0);
    set_word(graphics, 0x10000 + 63 * 4 + 2, 9);
    set_word(graphics, 0x10000 + 64 * 4 + 2, 7);
    line = video.inspect_playfield_line(0, 0, graphics);
    EXPECT_TRUE(line.palette.size() == 1024 && line.palette[1008] == 5)
        << "Extended map spans all 64 columns";
    line = video.inspect_playfield_line(0, 16, graphics);
    EXPECT_EQ(line.palette[0], 3) << "Extended row stride is 64 descriptors, not 32";
    EXPECT_TRUE(!config.extended_alt_maps && video.inspect_playfield_line(4, 0, graphics).palette.empty())
        << "Ordinary extended profiles retain four physical maps by default";

    config = {0, 0, 0, 256, true, true};
    EXPECT_TRUE(video.load_roms(sprites, {}, tiles, {}, config)) << "Extended alternate-map layout loads";
    pixels.assign(320 * 256 + 1, 0xdeadbeef);
    graphics.fill(0);
    control.fill(0);
    palette.fill(0);
    for (unsigned map = 4; map < 6; ++map)
        for (unsigned cell = 0; cell < 2048; ++cell)
            set_word(graphics, 0x10000 + map * 0x2000 + cell * 4 + 2, uint16_t(map + 5));
    set_word(graphics, 0x1a000 + 63 * 4 + 2, 7);
    set_word(graphics, 0x1a000 + 64 * 4 + 2, 8);
    set_word(graphics, 0x1a000 + (31 * 64 + 63) * 4 + 2, 9);
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
        set_word(graphics, 0x20e00, uint16_t(1u << pf));
        set_word(graphics, 0x2b400, pf == 2 ? 0x2001 : 0);
        set_word(graphics, 0x2b600, pf == 3 ? 0x2001 : 0);
        for (unsigned y = 0; y < 4; ++y) {
            set_word(graphics, 0x20000 + y * 2, y == 2 ? 0 : uint16_t(1u << pf));
            set_word(graphics, 0x24000 + pf * 0x200 + y * 2, y == 1 ? 0x200 : 0);
        }
        set_word(graphics, 0x20400, 6);
        set_word(graphics, 0x26200, 0xff00);
        set_word(graphics, 0x26400, 0x6000);
        video.render_frame(palette, graphics, control, pixels);
        const uint32_t alternate = pf == 2 ? 0xff000055 : 0xff000011;
        EXPECT_TRUE(pixels[0] == 0xff000000 && pixels[320] == alternate &&
                    pixels[640] == alternate && pixels[960] == 0xff000000)
            << "Extended PF2/PF3 selection and row usage follow per-line latches including retention and return";
        config.extended_alt_maps = false;
        EXPECT_TRUE(video.load_roms(sprites, {}, tiles, {}, config)) << "Default extended layout reloads";
        video.render_frame(palette, graphics, control, pixels);
        EXPECT_EQ(pixels[320], 0xff000000) << "JP/default extended mode ignores alternate selector bit";
        config.extended_alt_maps = true;
        EXPECT_TRUE(video.load_roms(sprites, {}, tiles, {}, config)) << "Extended alternate layout reloads";
    }

    set_word(control, 6, uint16_t((1016 - 28) * 64));
    set_word(control, 14, uint16_t(-128));
    set_word(graphics, 0x20800, 8);
    set_word(graphics, 0x28600, 0);
    set_word(graphics, 0x20000, 8);
    set_word(graphics, 0x24600, 0x3ff);
    set_word(graphics, 0x20002, 8);
    set_word(graphics, 0x24602, 0x200);
    video.render_frame(palette, graphics, control, pixels);
    EXPECT_TRUE(pixels[0] == 0xff000055 && pixels[8] == 0xff000011 && pixels[320] == 0xff000033)
        << "Extended alternate scanout wraps x at 1024 and y at 512 without a 32-column mask";
}

#ifdef F3RT_GAME_VIDEO
TEST(Video, GameTileObservation) {
    std::vector<uint8_t> graphics(0x40000, 0);
    std::array<uint8_t, 0x20> control{};
    const auto store = [&](unsigned cell, uint16_t attributes, uint16_t code) {
        const unsigned at = 0x12000 + cell * 4;
        graphics[at] = uint8_t(attributes >> 8);
        graphics[at + 1] = uint8_t(attributes);
        graphics[at + 2] = uint8_t(code >> 8);
        graphics[at + 3] = uint8_t(code);
    };
    store(0, uint16_t(3 | 0x200), 5);              // palette 3, blend
    store(1, uint16_t(4 | (1 << 10) | 0x4000), 6); // palette 4, extra plane, flip X
    store(2, uint16_t(5 | (1 << 10)), 7);          // palette low bit set: extra plane masked
    store(3, uint16_t(6 | 0x8000), 8);             // palette 6, flip Y
    std::array<uint8_t, 16 * 256> assets{};
    for (unsigned i = 0; i < assets.size(); ++i) assets[i] = uint8_t(i * 5 + 1);

    f3rt::GameTiles scene;
    f3rt::VideoRam vram{graphics, control, 0};
    scene.decode(vram);

    const auto plain = scene.playfield_pixel(1, 0, 0, false, assets);
    EXPECT_TRUE(plain.palette == 48 + (assets[5 * 256] & 15) && plain.flags == 0x11)
        << "Raw cell decodes palette base, pen mask and blend selector";
    const auto flip0 = scene.playfield_pixel(1, 16, 0, false, assets);
    const auto flip15 = scene.playfield_pixel(1, 31, 0, false, assets);
    EXPECT_TRUE(flip0.palette == 64 + (assets[6 * 256 + 15] & 31) &&
                flip15.palette == 64 + (assets[6 * 256] & 31))
        << "Raw cell flip X mirrors the texel column and applies the extra-plane mask";
    const auto masked = scene.playfield_pixel(1, 32, 0, false, assets);
    EXPECT_EQ(masked.palette, 80 + (assets[7 * 256] & 15))
        << "Extra pen plane is masked off when the palette's low bit is set";
    const auto flip_y = scene.playfield_pixel(1, 48, 0, false, assets);
    EXPECT_TRUE(flip_y.palette == 96 + (assets[8 * 256 + 15 * 16] & 15) && flip_y.flags == 0x10)
        << "Raw cell flip Y mirrors the texel row";
}

TEST(Video, GameTileRowSampling) {
    std::vector<uint8_t> graphics(0x40000, 0);
    std::array<uint8_t, 0x20> control{};
    std::array<uint8_t, 64 * 256> assets{};
    for (unsigned i = 0; i < assets.size(); ++i) assets[i] = uint8_t(i * 37 + i / 16);
    struct RawCell { uint16_t attributes, code; };
    std::array<std::array<RawCell, 2048>, 4> cells{};
    for (unsigned layer = 0; layer < cells.size(); ++layer)
        for (unsigned i = 0; i < cells[layer].size(); ++i) {
            auto &cell = cells[layer][i];
            cell.attributes = uint16_t((i & 0x1ff) | (uint16_t((i >> 9) & 1) << 9) |
                (uint16_t((i >> 2) & 1) << 10) | (uint16_t(i & 1) << 14) | (uint16_t((i >> 1) & 1) << 15));
            cell.code = uint16_t(0x8000 | (i & 63));
            const unsigned at = 0x10000 + layer * 0x2000 + i * 4;
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
                for (int x : {0, 15, 16, 17, 17, 31, 32, 1023, 1024, -1, -16,
                              -17, -1024, -1025, 511, 256, 0, 1023}) {
                    const unsigned sx = (unsigned(x) & 1023) ^ (flipped ? 1023 : 0);
                    const unsigned sy = (unsigned(y) & 511) ^ (flipped ? 511 : 0);
                    const auto &cell = cells[layer][(sy / 16) * 64 + sx / 16];
                    const unsigned palette_code = cell.attributes & 0x1ff;
                    const unsigned pen_mask = (((cell.attributes >> 10) & 3 & ~cell.attributes) << 4) | 15;
                    const unsigned tx = (sx & 15) ^ ((cell.attributes & 0x4000) ? 15 : 0);
                    const unsigned ty = (sy & 15) ^ ((cell.attributes & 0x8000) ? 15 : 0);
                    const uint8_t pen = assets[(cell.code & 0x7fff) * 256 + ty * 16 + tx] & pen_mask;
                    const auto pixel = sampler.pixel(x);
                    EXPECT_TRUE(pixel.palette == uint16_t(palette_code * 16 + pen) &&
                                pixel.flags == uint8_t((pen ? 0x10 : 0) | ((cell.attributes >> 9) & 1)))
                        << "Raw row sampling preserves palette base, mask, flips and wrapped X jumps";
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
    EXPECT_TRUE(scene.sprites().empty()) << "Decoded sprites are submitted, not visible before the latch";
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
    constexpr unsigned lineram = 0x20000;
    word(lineram + 0x400 + 0, 1 << 3);  // line 0 latches subsection 3
    word(lineram + 0x6600, 0x1234);
    word(lineram + 0x400 + 4, 1 << 3);  // line 2 latches it again with a new value
    word(lineram + 0x6604, 0x00ff);
    f3rt::GameLines scene;
    f3rt::VideoRam vram{graphics, control, 0};
    scene.decode(vram);
    scene.prepare(false);
    EXPECT_EQ(scene.row(0).background, 0x1234) << "Line 0 decodes its latched background palette";
    EXPECT_EQ(scene.row(1).background, 0x1234)
        << "Line 1 carries the previous line's unlatched background palette forward";
    EXPECT_EQ(scene.row(2).background, 0x00ff) << "Line 2 adopts its own latched background palette";
}

TEST(Video, GameTextVramDecode) {
    std::vector<uint8_t> graphics(0x40000, 0);
    std::array<uint8_t, 0x20> control{};
    const auto word = [&](unsigned offset, uint16_t value) {
        graphics[offset] = uint8_t(value >> 8);
        graphics[offset + 1] = uint8_t(value);
    };
    word(0x1c000, uint16_t((3 << 9) | 0x12));
    graphics[0x1e000 + 0x12 * 32 + 3] = 0x05;
    f3rt::GameText scene;
    f3rt::VideoRam vram{graphics, control, 0};
    scene.decode(vram);
    const auto px = scene.pixel(0, 0, false);
    EXPECT_TRUE(px.palette == 3 * 16 + 5 && px.flags == 0x10)
        << "Text map and glyph RAM decode palette base and pen";
    word(0x1c000 + 2, uint16_t((2 << 9) | 0x0100 | 0x8000 | 0x21));
    graphics[0x1e000 + 0x21 * 32 + 24] = 0x9a;
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
    const uint16_t map_word = uint16_t((palette_code << 9) | (flip_x ? 0x0100 : 0) | (flip_y ? 0x8000 : 0));
    graphics[0x1c000] = uint8_t(map_word >> 8);
    graphics[0x1c001] = uint8_t(map_word);

    // Populate glyph 0 in glyph RAM (0x1e000) with a unique non-zero pen for each (gx, gy) in 8x8:
    // Raw glyph RAM layout: pixel (gx, gy) is nibble (gx & 1) of byte gy*4 + (3 - gx/2).
    for (unsigned gy = 0; gy < 8; ++gy) {
        for (unsigned gx = 0; gx < 8; ++gx) {
            const uint8_t pen = uint8_t(((gy * 8 + gx) % 15) + 1);
            const size_t byte_idx = 0x1e000 + gy * 4 + (3 - gx / 2);
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
    std::vector<uint32_t> pixels(320 * 224);

    // Scanline 32 programmed to 15-bit mode (latch 12, mode 0x2000, background palette 1).
    set_word(graphics, 0x20400 + 32 * 2, 12);
    set_word(graphics, 0x26400 + 32 * 2, 0x2000);
    set_word(graphics, 0x26600 + 32 * 2, 0);

    // Background color 0 stored in lower 16 bits of 32-bit palette entry 0
    palette[0] = 0;
    palette[1] = 0;
    palette[2] = uint8_t(color_word >> 8);
    palette[3] = uint8_t(color_word);

    video.render_frame(palette, graphics, control, pixels);

    // Independent reference: FDA 15-bit channel mapping
    // Red: high 4 bits in 15..12, low bit in 3 -> 5 bits scaled to 8-bit (<< 3)
    const uint32_t expected_r = (((color_word >> 12) & 0x0fu) << 4) | (((color_word >> 3) & 1u) << 3);
    // Green: high 4 bits in 11..8, low bit in 2 -> 5 bits scaled to 8-bit (<< 3)
    const uint32_t expected_g = (((color_word >> 8) & 0x0fu) << 4) | (((color_word >> 2) & 1u) << 3);
    // Blue: high 4 bits in 7..4, low bit in 1 -> 5 bits scaled to 8-bit (<< 3)
    const uint32_t expected_b = (((color_word >> 4) & 0x0fu) << 4) | (((color_word >> 1) & 1u) << 3);
    const uint32_t expected_pixel = 0xff000000u | (expected_r << 16) | (expected_g << 8) | expected_b;

    RC_ASSERT(pixels[0] == expected_pixel);
}
