#include "f3rt/machine.hpp"
#include "f3rt/input.hpp"
#include "f3rt/netplay.hpp"
#include "f3rt/audio.hpp"
#include "f3rt/video.hpp"
#include "eeprom.hpp"
#include "interpreter.hpp"
#include "third_party/audio/mc68681.hpp"
#include "third_party/audio/es5510.hpp"
#include "state_io.hpp"
#include "renderer/game/tiles.hpp"
#include "renderer/game/text.hpp"
#include "renderer/game/sprites.hpp"
#include "renderer/game/lines.hpp"
#include "sprite_units.hpp"
#include "renderer/sprite_behaviour.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <initializer_list>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool ok,const char *why) { if(!ok)throw std::runtime_error(why); }
void check_game_rom_video() {
    const auto word=[](auto &bytes, size_t at, uint16_t value) {
        bytes[at]=uint8_t(value>>8); bytes[at+1]=uint8_t(value);
    };
    const auto color=[](auto &bytes, size_t at, uint32_t value) {
        for(unsigned i=0;i<4;++i)bytes[at+i]=uint8_t(value>>(24-8*i));
    };
    std::vector<uint8_t> sprites(256,0x21), tiles(128,0x43);
    f3rt::Video video;
    f3rt::VideoConfig config{90,2,31,224};
    require(video.load_roms(sprites,{},tiles,{},config), "4-bpp ROMs need no fabricated high planes");
    require(video.sprite_tiles()[256]==1 && video.sprite_tiles()[257]==2 &&
            video.playfield_tiles()[0]==3 && video.playfield_tiles()[1]==4,
            "Absent high planes preserve packed low pens across independent ROM geometries");
    std::array<uint8_t,0x8000> palette{};
    std::array<uint8_t,0x40000> graphics{};
    std::array<uint8_t,0x20> control{};
    std::vector<uint32_t> pixels(320*224);
    color(palette,4,0x00112233);color(palette,8,0x00445566);
    word(graphics,0x20400+31*2,12);word(graphics,0x26600+31*2,1);
    word(graphics,0x26400+31*2,0x6000);
    word(graphics,0x20400+32*2,8);word(graphics,0x26600+32*2,2);
    word(graphics,0,1);word(graphics,4,46);word(graphics,6,31);
    word(graphics,8,1);word(graphics,16+12,0x8001);
    video.render_frame(palette,graphics,control,pixels);
    require(pixels[0]==0xff112233 && pixels[320]==0xff445566,
            "RayForce's first visible scanline is 31, not Land Maker's 24");
    require(video.sprite_plane()[31*432+46]==0, "Lag 2 delays a newly parsed sprite list");
    std::vector<uint8_t> snapshot(video.state_size());video.save_state(snapshot);
    word(graphics,4,50);
    video.render_frame(palette,graphics,control,pixels);
    require(video.sprite_plane()[31*432+46]==0x1011 && video.sprite_plane()[31*432+50]==0x1011,
            "Lag 2 draws the previous parsed position, not current sprite RAM");
    video.render_frame(palette,graphics,control,pixels);
    require(video.sprite_plane()[31*432+46]==0 && video.sprite_plane()[31*432+50]==0x1011,
            "Lag 2 advances the captured list on the next frame");
    video.load_state(snapshot);video.render_frame(palette,graphics,control,pixels);
    require(video.sprite_plane()[31*432+46]==0x1011,
            "Snapshots preserve the delayed parsed list, not only the framebuffer");

    config={0,1,32,224};
    require(video.load_roms(sprites,{},tiles,{},config), "Riding Fight geometry loads");
    video.reset();graphics.fill(0);palette.fill(0);
    color(palette,0,0x00abcdee);color(palette,4,0x0000abc8);
    color(palette,8,0x0000abc4);color(palette,12,0x0000abc2);
    color(palette,16,0x0000fffe);color(palette,20,0x00abcdef);
    const auto line=[&](unsigned y,uint16_t latch,uint16_t mode,uint16_t background) {
        word(graphics,0x20400+y*2,latch);
        word(graphics,0x26400+y*2,mode);
        word(graphics,0x26600+y*2,background);
    };
    line(32,12,0x2000,1);line(33,8,0x6000,2);line(34,12,0x6000,3);
    line(35,0x48,0x6000,4);word(graphics,0x26c00+35*2,0x2000);
    line(36,8,0,0);line(37,8,0,5);line(38,12,0,1);
    video.render_frame(palette,graphics,control,pixels);
    require(pixels[0]==0xffa8b0c0, "15-bit palette includes red bit 3, without 5-to-8-bit replication");
    require(pixels[320]==0xffa0b8c0, "Unlatched mode changes are ignored while green bit 2 is preserved");
    require(pixels[640]==0xff00abc2, "FDA bit 14 switches to full 24-bit color on the next latched line");
    require(pixels[960]==0xfff8f8f8, "Alternate-bank latches restore 15-bit mode with a maximum channel of 248");
    require(pixels[1280]==0xffc8d8e8 && pixels[1600]==0xffc8d8e8,
            "15-bit color includes blue bit 1 and ignores upper bits and unused bit 0");
    require(pixels[1920]==0xff545860 && pixels[1921]==0xffa8b0c0,
            "FDA forward blur averages with the unfiltered preceding pixel, starting from black");

    sprites.assign(0x800000,0);tiles.assign(0x200000,0);
    std::vector<uint8_t> high(0x400000,0);
    sprites[0x400000]=0x65;high[0x200000]=3;
    require(video.load_roms(sprites,high,tiles,{},config), "Command War's second sprite bank loads");
    require(video.sprite_tiles()[32768*256]==0x35 && video.sprite_tiles()[32768*256+1]==6,
            "Command War's bank-1 pens include its own high planes, without wrapping to bank 0");
}
void check_fdp_geometry() {
    const auto word=[](auto &bytes,size_t at,uint16_t value) {
        bytes[at]=uint8_t(value>>8);bytes[at+1]=uint8_t(value);
    };
    std::vector<uint8_t> sprites(3*128),tiles(5*128);
    for(unsigned tile=0;tile<3;++tile)
        std::fill_n(sprites.begin()+tile*128,128,uint8_t((tile+1)*0x11));
    for(unsigned tile=0;tile<5;++tile)
        std::fill_n(tiles.begin()+tile*128,128,uint8_t((tile+1)*0x11));
    f3rt::Video video;
    f3rt::VideoConfig config{0,0,0,256,false};
    require(video.load_roms(sprites,{},tiles,{},config),"Independent non-power-of-two FDP assets load");
    std::array<uint8_t,0x40000> graphics{};
    std::array<uint8_t,0x8000> palette{};
    std::array<uint8_t,0x20> control{};
    std::vector<uint32_t> pixels(320*256+1,0xdeadbeef);
    for(unsigned map=0;map<8;++map)
        for(unsigned cell=0;cell<1024;++cell)
            word(graphics,0x10000+map*0x1000+cell*4+2,uint16_t(map+5));
    auto line=video.inspect_playfield_line(4,511,graphics);
    require(line.palette.size()==512 && line.palette[0]==5 && line.palette[511]==5,
            "Nonextended alternate map 4 uses its own 32-column base and count-correct tile wrapping");
    line=video.inspect_playfield_line(7,0,graphics);
    require(line.palette[0]==3,"Last nonextended physical map remains independently addressable");
    // Latch PF2 to ordinary map 2, then alternate map 4, then ordinary again.
    // Only the alternate map has a nonblank row: usage must follow selection too.
    for(unsigned cell=0;cell<1024;++cell) word(graphics,0x12000+cell*4+2,0);
    for(unsigned y=0;y<3;++y) {
        word(graphics,0x20000+y*2,4);
        word(graphics,0x24400+y*2,y==1?0x200:0);
    }
    word(graphics,0x20400,6);word(graphics,0x26200,0xff00);word(graphics,0x26400,0x6000);
    word(graphics,0x20e00,4);word(graphics,0x2b400,0x2001);
    palette[5*4+1]=0x12;palette[5*4+2]=0x34;palette[5*4+3]=0x56;
    video.render_frame(palette,graphics,control,pixels);
    require(pixels[0]==0xff000000 && pixels[320]==0xff123456 && pixels[640]==0xff000000,
            "Latched alternate-map selection changes visible pixels and row-usage gating per scanline");
    require(pixels[320*256]==0xdeadbeef,"Full-height crop writes exactly 256 rows");
    word(graphics,0x20e00,12);word(graphics,0x2b400,0);word(graphics,0x2b600,0x2001);
    word(graphics,0x20000,8);word(graphics,0x24600,0x200);
    palette[4+1]=0x65;palette[4+2]=0x43;palette[4+3]=0x21;
    video.render_frame(palette,graphics,control,pixels);
    require(pixels[0]==0xff654321,"PF3 alternate selection reads map 5 rather than map 4 or 7");
    graphics.fill(0);
    word(graphics,0,5);word(graphics,4,46);word(graphics,6,0);
    word(graphics,16+12,0x8001);
    video.render_frame(palette,graphics,control,pixels);
    require(video.sprite_plane()[46]==0x1003,"Non-power-of-two sprite code wraps by count at scanout row zero");
    config={0,0,255,1,false};
    require(video.load_roms(sprites,{},tiles,{},config),"Bottom-only crop loads");
    video.reset();graphics.fill(0);word(graphics,0,4);word(graphics,4,365);word(graphics,6,255);
    word(graphics,16+12,0x8001);
    pixels.assign(321,0xdeadbeef);video.render_frame(palette,graphics,control,pixels);
    require(video.sprite_plane()[255*432+365]==0x1002 && video.sprite_plane()[254*432+365]==0,
            "Bottom/right crop includes its final pixel without painting outside vertical bounds");
    require(pixels[320]==0xdeadbeef,"One-row crop preserves the output boundary sentinel");
    config={0,0,24,232,true};
    require(video.load_roms(sprites,{},tiles,{},config),"Extended geometry accepts independent counts");
    graphics.fill(0);word(graphics,0x10000+63*4+2,9);
    word(graphics,0x10000+64*4+2,7);
    line=video.inspect_playfield_line(0,0,graphics);
    require(line.palette.size()==1024 && line.palette[1008]==5,"Extended map spans all 64 columns");
    line=video.inspect_playfield_line(0,16,graphics);
    require(line.palette[0]==3,"Extended row stride is 64 descriptors, not 32");
    require(!config.extended_alt_maps && video.inspect_playfield_line(4,0,graphics).palette.empty(),
            "Ordinary extended profiles retain four physical maps by default");

    config={0,0,0,256,true,true};
    require(video.load_roms(sprites,{},tiles,{},config),"Extended alternate-map layout loads");
    // Restore the full-height destination after the one-row crop above.
    // Native scanout rejects an output span smaller than the configured crop.
    pixels.assign(320*256+1,0xdeadbeef);
    graphics.fill(0);control.fill(0);palette.fill(0);
    // Native 64-column maps occupy 0x18000/0x1a000, not the 32-column bases.
    for(unsigned map=4;map<6;++map)
        for(unsigned cell=0;cell<2048;++cell)
            word(graphics,0x10000+map*0x2000+cell*4+2,uint16_t(map+5));
    word(graphics,0x1a000+63*4+2,7);
    word(graphics,0x1a000+64*4+2,8);
    word(graphics,0x1a000+(31*64+63)*4+2,9);
    line=video.inspect_playfield_line(5,0,graphics);
    require(line.palette.size()==1024 && line.palette[0]==1 && line.palette[1008]==3,
            "Extended map 5 has its own native 64-column base and count wrapping");
    line=video.inspect_playfield_line(5,16,graphics);
    require(line.palette[0]==4,"Extended alternate row stride stays 64 columns");
    line=video.inspect_playfield_line(5,511,graphics);
    require(line.palette[1023]==5,"Extended alternate final row/column stays within PF RAM");
    require(video.inspect_playfield_line(6,0,graphics).palette.empty(),
            "Extended alternates stop at map 5 before text/character RAM, never alias pivot RAM");
    for(unsigned pen=1;pen<=5;++pen) palette[pen*4+3]=uint8_t(pen*0x11);
    // PF2 and PF3 each switch normal -> alternate -> retained alternate -> normal.
    for(unsigned pf=2;pf<=3;++pf) {
        word(graphics,0x20e00,uint16_t(1u<<pf));
        word(graphics,0x2b400,pf==2?0x2001:0);
        word(graphics,0x2b600,pf==3?0x2001:0);
        for(unsigned y=0;y<4;++y) {
            word(graphics,0x20000+y*2,y==2?0:uint16_t(1u<<pf));
            word(graphics,0x24000+pf*0x200+y*2,y==1?0x200:0);
        }
        word(graphics,0x20400,6);word(graphics,0x26200,0xff00);word(graphics,0x26400,0x6000);
        video.render_frame(palette,graphics,control,pixels);
        const uint32_t alternate=pf==2?0xff000055:0xff000011;
        require(pixels[0]==0xff000000 && pixels[320]==alternate &&
                pixels[640]==alternate && pixels[960]==0xff000000,
                "Extended PF2/PF3 selection and row usage follow per-line latches including retention and return");
        config.extended_alt_maps=false;
        require(video.load_roms(sprites,{},tiles,{},config),"Default extended layout reloads");
        video.render_frame(palette,graphics,control,pixels);
        require(pixels[320]==0xff000000,"JP/default extended mode ignores alternate selector bit");
        config.extended_alt_maps=true;
        require(video.load_roms(sprites,{},tiles,{},config),"Extended alternate layout reloads");
    }
    // Unit scale, x=1016 and y=511: wrap to column zero at x=8, then row zero.
    word(control,6,uint16_t((1016-28)*64));
    word(control,14,uint16_t(-128));
    word(graphics,0x20800,8);word(graphics,0x28600,0);
    word(graphics,0x20000,8);word(graphics,0x24600,0x3ff);
    word(graphics,0x20002,8);word(graphics,0x24602,0x200);
    video.render_frame(palette,graphics,control,pixels);
    require(pixels[0]==0xff000055 && pixels[8]==0xff000011 && pixels[320]==0xff000033,
            "Extended alternate scanout wraps x at 1024 and y at 512 without a 32-column mask");
}
void check_game_tile_observation() {
    // Raw PF snapshot: layer 1 cells at graphics 0x12000, 4 bytes each with the
    // attributes word first (big-endian), the tile code second.
    std::vector<uint8_t> graphics(0x40000, 0);
    std::array<uint8_t, 0x20> control{};
    const auto store = [&](unsigned cell, uint16_t attributes, uint16_t code) {
        const unsigned at = 0x12000 + cell * 4;
        graphics[at] = uint8_t(attributes >> 8); graphics[at + 1] = uint8_t(attributes);
        graphics[at + 2] = uint8_t(code >> 8); graphics[at + 3] = uint8_t(code);
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
    require(plain.palette == 48 + (assets[5 * 256] & 15) && plain.flags == 0x11,
            "Raw cell decodes palette base, pen mask and blend selector");
    // Flip X: output texel 0 reads source column 15 and output texel 15 reads 0.
    const auto flip0 = scene.playfield_pixel(1, 16, 0, false, assets);
    const auto flip15 = scene.playfield_pixel(1, 31, 0, false, assets);
    require(flip0.palette == 64 + (assets[6 * 256 + 15] & 31) &&
            flip15.palette == 64 + (assets[6 * 256] & 31),
            "Raw cell flip X mirrors the texel column and applies the extra-plane mask");
    // Palette bit 0 set clears the matching extra plane: mask 15, not 31.
    const auto masked = scene.playfield_pixel(1, 32, 0, false, assets);
    require(masked.palette == 80 + (assets[7 * 256] & 15),
            "Extra pen plane is masked off when the palette's low bit is set");
    // Flip Y: output row 0 reads source row 15.
    const auto flip_y = scene.playfield_pixel(1, 48, 0, false, assets);
    require(flip_y.palette == 96 + (assets[8 * 256 + 15 * 16] & 15) && flip_y.flags == 0x10,
            "Raw cell flip Y mirrors the texel row");
}
void check_game_tile_row_sampling() {
    // Fill all four layers with raw VRAM cells, then sample with wrapping,
    // repeats, reverse jumps and global screen flip.
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
            graphics[at] = uint8_t(cell.attributes >> 8); graphics[at + 1] = uint8_t(cell.attributes);
            graphics[at + 2] = uint8_t(cell.code >> 8); graphics[at + 3] = uint8_t(cell.code);
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
                    require(pixel.palette == uint16_t(palette_code * 16 + pen) &&
                            pixel.flags == uint8_t((pen ? 0x10 : 0) | ((cell.attributes >> 9) & 1)),
                            "Raw row sampling preserves palette base, mask, flips and wrapped X jumps");
                }
            }
}
void check_game_sprite_vram_block_chaining() {
    // Raw sprite display list: entry 0 uses block control 00 to position the
    // block, entry 1 uses block control 11 so each axis advances one 16-pixel
    // block from the previous position (the hardware chaining rule).
    std::vector<uint8_t> graphics(0x40000, 0);
    std::array<uint8_t, 0x20> control{};
    const auto word = [&](unsigned offset, uint16_t value) {
        graphics[offset] = uint8_t(value >> 8); graphics[offset + 1] = uint8_t(value);
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
    require(scene.sprites().empty(), "Decoded sprites are submitted, not visible before the latch");
    scene.latch();
    const auto sprites = scene.sprites();
    require(sprites.size() == 2, "Two sprite-list entries decode to two sprites");
    require(sprites[0].x == 50 * 256 && sprites[0].y == 30 * 256,
            "Sprite-list coordinates are scanout coordinates, with no extra origin");
    require(sprites[1].x == (50 + 16) * 256 && sprites[1].y == (30 + 16) * 256,
            "Block-chained sprite advances one 16-pixel block on each axis");
    require(sprites[0].tile == 1 && sprites[1].tile == 2 && sprites[0].palette == 5 &&
            sprites[0].scale_x == 256 && sprites[0].scale_y == 256,
            "Tile code, colour and zoom survive VRAM decode");
}
void check_game_line_vram_carry_forward() {
    // Line RAM section 6000 subsection 3 holds the background palette; its latch
    // word for scanline y is at lineram 0x400 + y*2 and its value at 0x6600 + y*2.
    std::vector<uint8_t> graphics(0x40000, 0);
    std::array<uint8_t, 0x20> control{};
    const auto word = [&](unsigned offset, uint16_t value) {
        graphics[offset] = uint8_t(value >> 8); graphics[offset + 1] = uint8_t(value);
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
    require(scene.row(0).background == 0x1234, "Line 0 decodes its latched background palette");
    require(scene.row(1).background == 0x1234,
            "Line 1 carries the previous line's unlatched background palette forward");
    require(scene.row(2).background == 0x00ff, "Line 2 adopts its own latched background palette");
}
void check_game_text_vram_decode() {
    std::vector<uint8_t> graphics(0x40000, 0);
    std::array<uint8_t, 0x20> control{};
    const auto word = [&](unsigned offset, uint16_t value) {
        graphics[offset] = uint8_t(value >> 8); graphics[offset + 1] = uint8_t(value);
    };
    // Cell 0: tile 0x12, palette 3, no flips.
    word(0x1c000, uint16_t((3 << 9) | 0x12));
    // Glyph 0x12, texel (0,0): the char-RAM decode reads byte row[3]'s low nibble.
    graphics[0x1e000 + 0x12 * 32 + 3] = 0x05;
    f3rt::GameText scene;
    f3rt::VideoRam vram{graphics, control, 0};
    scene.decode(vram);
    const auto px = scene.pixel(0, 0, false);
    require(px.palette == 3 * 16 + 5 && px.flags == 0x10,
            "Text map and glyph RAM decode palette base and pen");
    // Cell 1 (x=8): tile 0x21, palette 2, flip_x bit 8 and flip_y bit 15 set.
    word(0x1c000 + 2, uint16_t((2 << 9) | 0x0100 | 0x8000 | 0x21));
    // Glyph 0x21 row 6 (bytes 24..27): texel x=7 reads byte 24's high nibble.
    graphics[0x1e000 + 0x21 * 32 + 24] = 0x9a;
    scene.decode(vram);
    // (8,1) -> cell 1, tx = 7 (flip X), ty = 6 (flip Y), pen = nibble 9.
    const auto flipped = scene.pixel(8, 1, false);
    require(flipped.palette == 2 * 16 + 9 && flipped.flags == 0x10,
            "Text flip bits and glyph nibble order decode from raw words");
}
f3rt::RomSet fixture() {
    f3rt::RomSet r;
    r.main.resize(0x200000);r.sprites.resize(0x400000);r.sprites_hi.resize(0x200000);
    r.tiles.resize(0x400000);r.tiles_hi.resize(0x200000);r.sound.resize(0x80000);r.samples.resize(0x1000000);
    r.main[1]=0x41;r.main[2]=0xff;r.main[3]=0xf0; // SSP 41fff0
    r.main[6]=1; // PC 100
    r.main[0x100]=0x70;r.main[0x101]=0x2a; // moveq #42,d0
    r.main[0x102]=0x60;r.main[0x103]=0xfe; // bra self
    r.samples[0x2468a]=0x45;r.samples[0x2468b]=0x67; // OTIS word 0x12345, above old truncated mask
    return r;
}
void check_local_inputs() {
    using Words = std::array<f3rt::LocalInputWord, f3rt::local_player_count>;
    constexpr uint32_t ports[] = {0x4a0002, 0x4a0006, 0x4a0012, 0x4a0016};
    constexpr uint16_t button_masks[4][4] = {
        {0x0001, 0x0002, 0x0004, 0x0008}, {0x0010, 0x0020, 0x0040, 0x0080},
        {0x0100, 0x0200, 0x0400, 0x0800}, {0x1000, 0x2000, 0x4000, 0x8000}};
    constexpr uint16_t direction_masks[4][4] = {
        {0x0001, 0x0002, 0x0004, 0x0008}, {0x0010, 0x0020, 0x0040, 0x0080},
        {0x0001, 0x0002, 0x0004, 0x0008}, {0x0010, 0x0020, 0x0040, 0x0080}};
    constexpr uint16_t start_masks[] = {0x1000, 0x2000, 0x4000, 0x8000};
    constexpr uint8_t coin_masks[] = {0x10, 0x20, 0x40, 0x80};
    constexpr uint16_t service_masks[] = {0x200, 0x400, 0x800, 0};
    for (bool kaiser : {false, true}) {
        auto roms = fixture();
        roms.name = kaiser ? "kaiserknj" : "landmakrj";
        auto m = std::make_unique<f3rt::Machine>(std::move(roms));
        m->write16(0x4a0004, 0x0ba6);
        m->write16(0x4a0014, 0x0975);
        const auto expect = [&](const std::array<uint16_t, 4> &low, uint8_t system) {
            for (unsigned port = 0; port < low.size(); ++port)
                require(m->read16(ports[port]) == low[port],
                        "Local controls reach only their physical active-low MMIO lines");
            require((m->read8(0x4a0000) & 0xfe) == system &&
                    (m->read8(0x4a0001) & 0xfe) == system,
                    "Coins and test reach both system-byte MMIO lanes without pinning EEPROM DO");
            require(m->read16(0x4a0004) == 0x0ba6 && m->read16(0x4a0014) == 0x0975,
                    "Applying controls preserves both readable coin-counter banks");
        };
        const unsigned players = kaiser ? 2 : f3rt::local_player_count;
        for (unsigned slot = 0; slot < players; ++slot) {
            for (unsigned bit = 0; bit < f3rt::local_control_count; ++bit) {
                Words words{};
                words[slot] = f3rt::LocalInputWord(1u << bit);
                std::array<uint16_t, 4> low{0xffff, 0xffff, 0xffff, 0xffff};
                uint8_t system = 0xfe;
                if (bit < 4) low[slot < 2 ? 1 : 3] &= uint16_t(~direction_masks[slot][bit]);
                else if (bit < 7) low[slot < 2 ? 0 : 2] &= uint16_t(~button_masks[slot][bit - 4]);
                else if (bit == 7) low[0] &= uint16_t(~start_masks[slot]);
                else if (bit == 8) system &= uint8_t(~coin_masks[slot]);
                else if (bit == 9) low[0] &= uint16_t(~service_masks[slot]);
                else if (bit == 10) system &= uint8_t(~2u);
                else if (kaiser) {
                    constexpr uint16_t extra_masks[2][3] = {{1, 2, 4}, {0x100, 0x200, 0x400}};
                    low[slot == 0 ? 3 : 2] &= uint16_t(~extra_masks[slot][bit - 11]);
                } else if (bit == 11) low[slot < 2 ? 0 : 2] &= uint16_t(~button_masks[slot][3]);
                f3rt::apply_local_inputs(*m, words);
                expect(low, system);
                if (!kaiser && slot < 2 && bit < 11) {
                    std::array<f3rt::netplay::InputWord, 2> online{};
                    online[slot] = words[slot];
                    f3rt::netplay::apply_inputs(*m, online);
                    expect(low, system);
                }
            }
        }
        if (kaiser) {
            f3rt::apply_local_inputs(*m, {0, 0, f3rt::local_input_mask, f3rt::local_input_mask});
            expect({0xffff, 0xffff, 0xffff, 0xffff}, 0xfe);
            f3rt::apply_local_inputs(*m, {f3rt::local_input_mask, f3rt::local_input_mask, 0, 0});
            expect({uint16_t(~(0x0007u | 0x0070u | 0x1000u | 0x2000u | 0x0200u | 0x0400u)),
                    0xff00, 0xf8ff, 0xfff8}, uint8_t(0xfe & ~0x32u));
        } else {
            f3rt::apply_local_inputs(*m, {0, 0, f3rt::local_input_mask, f3rt::local_input_mask});
            f3rt::netplay::apply_inputs(*m, {});
            expect({0xffff, 0xffff, 0xffff, 0xffff}, 0xfe);
            for (unsigned slot = 0; slot < 2; ++slot) {
                for (unsigned bit = 11; bit < 16; ++bit) {
                    f3rt::apply_local_inputs(*m, {0x800, 0x100, 0x80, 0x10});
                    const auto before = m->inputs;
                    const uint8_t before_system = m->system_inputs;
                    std::array<f3rt::netplay::InputWord, 2> online{0x10, 0x20};
                    online[slot] = f3rt::netplay::InputWord(1u << bit);
                    bool rejected = false;
                    try { f3rt::netplay::apply_inputs(*m, online); }
                    catch (const std::runtime_error &) { rejected = true; }
                    require(rejected && m->inputs == before && m->system_inputs == before_system,
                            "Network words reject every bit above the original eleven before applying either player");
                }
            }
        }
        f3rt::apply_local_inputs(*m, {});
        expect({0xffff, 0xffff, 0xffff, 0xffff}, 0xfe);
    }
}
void check_input_script() {
    auto script = f3rt::InputScript::parse(
        "# coin pulse\n700+3 coin\n710-712 p2 right+b1 # comment\n20-end mash seed=5 period=4 keys=left\n", "t");
    const auto coin = [&](uint64_t frame) { return script.words(frame)[0] & 0x100; };
    require(coin(699) == 0 && coin(700) == 0x100 && coin(702) == 0x100 && coin(703) == 0 &&
                script.words(710)[1] == 0x18 && script.words(713)[1] == 0,
            "Input script ranges are inclusive; +COUNT spans COUNT frames; pN selects the player");
    std::vector<f3rt::LocalInputWord> forward;
    for (uint64_t frame = 20; frame < 120; ++frame) forward.push_back(script.words(frame)[0] & ~0x100);
    require(std::count(forward.begin(), forward.end(), 0x4) && std::count(forward.begin(), forward.end(), 0),
            "Mash presses and releases only its keys");
    require((script.words(57)[0] & ~0x100) == forward[57 - 20],
            "Going back to an earlier frame replays the same mash sequence");
    for (const char *bad : {"0 coin\n", "10-5 coin\n", "5 p5 coin\n", "5 jump\n", "5 left right\n",
                            "5 mash period=0\n", "5 mash speed=3\n", "5\n", "5 poke 0x401f54=1\n",
                            "5 poke 0x401f54.w=0x10000\n", "5 poke 0x41ffff.w=1\n", "5 poke 0x660000.b=1\n",
                            "5 p2 poke 0x401f54.b=1\n"}) {
        bool rejected = false;
        try { f3rt::InputScript::parse(bad, "t"); } catch (const std::runtime_error &) { rejected = true; }
        require(rejected, "Malformed input script lines are rejected");
    }
    auto pokes = f3rt::InputScript::parse("5-6 poke 0x401f54.w=0x1234\n7 poke 0x41fffc.l=4294967295\n", "t");
    auto m = std::make_unique<f3rt::Machine>(fixture());
    pokes.poke(*m, 4);
    require(m->read16(0x401f54) == 0, "Pokes wait for their first frame");
    pokes.poke(*m, 6);
    pokes.poke(*m, 7);
    require(m->read16(0x401f54) == 0x1234 && m->read32(0x41fffc) == 0xffffffff,
            "Pokes write big-endian main RAM, hex or decimal, up to the last RAM byte");
}
void check_dial_inputs() {
    using Words = std::array<f3rt::LocalInputWord, f3rt::local_player_count>;
    for (const char *name : {"arkretrnj", "puchicarj"}) {
        auto roms = fixture();
        roms.name = name;
        auto m = std::make_unique<f3rt::Machine>(std::move(roms));
        const auto expect = [&](uint32_t first, uint32_t second) {
            require(m->read32(0x4a0008) == first && m->read32(0x4a000c) == second,
                    "Both twelve-bit dial counters expose the native big-endian nibble-packed MMIO words");
        };
        expect(0xffff0000, 0xffff0000);
        f3rt::apply_local_inputs(*m, {4, 8, 0, 0});
        expect(0xffffe0ff, 0xffff2000);
        require(m->read8(0x4a000a) == 0xe0 && m->read8(0x4a000b) == 0xff &&
                m->read16(0x4a000e) == 0x2000 && m->read16(0x4a0006) == 0xff7b,
                "Dial arrows also reach each player's real native joystick bits and byte lanes");
        f3rt::apply_local_inputs(*m, {});
        expect(0xffffe0ff, 0xffff2000);
        require(m->read16(0x4a0006) == 0xffff,
                "Neutral input releases joystick lines without clearing dial history");
        f3rt::apply_local_inputs(*m, {0xc, 0xc, 0, 0});
        expect(0xffffe0ff, 0xffff2000);
        require(m->read16(0x4a0006) == 0xff33,
                "Opposite arrows cancel counter motion while preserving native joystick inputs");
        f3rt::apply_local_inputs(*m, {8, 4, 0, 0});
        expect(0xffff0000, 0xffff0000);
        for (unsigned frame = 0; frame < 8; ++frame)
            f3rt::apply_local_inputs(*m, {8, 4, 0, 0});
        expect(0xffff0001, 0xffff00ff);
        f3rt::apply_local_inputs(*m, {0, 0, 4, 8});
        expect(0xffff0001, 0xffff00ff);
        m->reset();
        expect(0xffff0001, 0xffff00ff);
        f3rt::apply_local_inputs(*m, {});
        expect(0xffff0001, 0xffff00ff);

        const std::array<Words, 5> continuation{{
            Words{8, 0, 0, 0}, Words{0, 4, 0, 0}, Words{0xc, 8, 0, 0},
            Words{4, 8, 0, 0}, Words{}}};
        const auto replay = [&] {
            for (const auto &words : continuation) {
                f3rt::apply_local_inputs(*m, words);
                require(m->run_frame(), "Dial snapshot continuation executes a logical machine frame");
            }
            expect(0xffff0001, 0xffff20ff);
            return m->sync_state_crc();
        };
        std::vector<uint8_t> full(m->state_size()), sync(m->sync_state_size());
        m->save_state(full);
        m->save_sync_state(sync);
        const uint32_t expected_crc = replay();
        m->load_state(full);
        expect(0xffff0001, 0xffff00ff);
        require(replay() == expected_crc,
                "Full snapshots restore dial history and replay identical serialized machine continuation");
        m->load_sync_state(sync);
        expect(0xffff0001, 0xffff00ff);
        require(replay() == expected_crc,
                "Portable snapshots restore dial history without any frontend counter cache");
    }
}
void send_bit(f3rt::Eeprom &e,bool bit,uint64_t now) { uint8_t pins=0x10|(bit?4:0);e.pins(pins,now);e.pins(pins|8,now); }
void command(f3rt::Eeprom &e,unsigned word,uint64_t now) { e.pins(0,now);for(int bit=8;bit>=0;--bit)send_bit(e,(word>>bit)&1,now); }
void serial_write(f3rt::Eeprom &e,unsigned address,uint16_t value,uint64_t now) {
    command(e,0x140|address,now);for(int bit=15;bit>=0;--bit)send_bit(e,(value>>bit)&1,now);e.pins(0,now);
}
uint16_t read_word(f3rt::Eeprom &e,uint64_t now) { uint16_t value=0;for(int i=0;i<16;++i) { send_bit(e,false,now);value=uint16_t((value<<1)|e.output(now)); }return value; }
void send_bit(f3rt::Machine &m,bool bit) {
    const uint8_t pins=0x10|(bit?4:0);
    m.write8(0x4a0013,pins);m.write8(0x4a0013,pins|8);
}
void command(f3rt::Machine &m,unsigned word) {
    m.write8(0x4a0013,0);for(int bit=8;bit>=0;--bit)send_bit(m,(word>>bit)&1);
}
void serial_write(f3rt::Machine &m,unsigned address,uint16_t value) {
    command(m,0x140|address);
    for(int bit=15;bit>=0;--bit)send_bit(m,(value>>bit)&1);
    m.write8(0x4a0013,0);
}
uint16_t read_word(f3rt::Machine &m) {
    uint16_t value=0;
    for(int i=0;i<16;++i) {
        send_bit(m,false);value=uint16_t((value<<1)|(m.read8(0x4a0000)&1));
    }
    return value;
}
void check_factory_eeprom() {
    auto roms=fixture();
    roms.factory_eeprom.assign(128,0xff);
    roms.factory_eeprom[0]=0x12;roms.factory_eeprom[1]=0x34;
    roms.factory_eeprom[2]=0x89;roms.factory_eeprom[3]=0xab;
    roms.factory_eeprom[126]=0xfe;roms.factory_eeprom[127]=0xdc;
    auto m=std::make_unique<f3rt::Machine>(std::move(roms));
    command(*m,0x1bf);
    require(!(m->read8(0x4a0000)&1),"Factory EEPROM read exposes the serial dummy bit through the input port");
    require(read_word(*m)==0xfedc && read_word(*m)==0x1234 && read_word(*m)==0x89ab,
            "Factory EEPROM seeds all addresses big-endian before initial reset, including serial wrap");
    command(*m,0x130);m->write8(0x4a0013,0); // EWEN
    serial_write(*m,0,0xa65c);m->cpu.cycles+=28000;
    m->reset();command(*m,0x180);
    require(read_word(*m)==0xa65c && read_word(*m)==0x89ab,
            "Machine reset preserves guest EEPROM writes instead of reseeding factory defaults");

    struct TemporaryImage {
        std::filesystem::path path;
        ~TemporaryImage() { std::error_code error;std::filesystem::remove(path,error); }
    } image{std::filesystem::temp_directory_path()/
        ("f3rt-check-eeprom-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".nv")};
    f3rt::Eeprom user;
    user.words.fill(0x579b);user.words[0]=0xc318;user.words[63]=0x2468;
    user.save(image.path); // Only this explicitly chosen temporary user path is written.
    m->load_eeprom(image.path);m->reset();command(*m,0x1bf);
    require(read_word(*m)==0x2468 && read_word(*m)==0xc318 && read_word(*m)==0x579b,
            "Explicit user EEPROM image overrides factory and prior guest contents across reset");
    command(*m,0x130);m->write8(0x4a0013,0);
    serial_write(*m,0,0xd42e);m->cpu.cycles+=28000;
    m->reset();command(*m,0x180);
    require(read_word(*m)==0xd42e,"Loaded user EEPROM remains writable and persists through reset");
    roms=std::move(m->roms);m.reset();
    auto factory_machine=std::make_unique<f3rt::Machine>(std::move(roms));
    command(*factory_machine,0x1bf);
    require(read_word(*factory_machine)==0xfedc && read_word(*factory_machine)==0x1234 &&
            read_word(*factory_machine)==0x89ab,
            "A new machine consumes the unchanged ROM seed after user-image load and guest writes");
}
void native(f3_cpu *cpu) { cpu->d[0]=99;cpu->pc+=2;cpu->cycles+=4; }
void native_other(f3_cpu *cpu) { cpu->d[0]=17;cpu->pc+=2;cpu->cycles+=4; }
void check_native_lookup() {
    auto m=std::make_unique<f3rt::Machine>(fixture());
    auto &cpu=m->cpu;
    m->allow_main_fallback=false;
    const auto hit=[&](uint32_t pc,uint32_t value) {
        cpu.pc=pc;cpu.sr=0x2700;cpu.d[0]=0;
        const auto count=m->native_blocks;
        require(f3_dispatch(&cpu) && cpu.pc==pc+2 && cpu.d[0]==value && m->native_blocks==count+1,
                "Indexed dispatch executes the correct registered entry");
    };
    const auto miss=[&](uint32_t pc) {
        cpu.pc=pc;cpu.sr=0x2700;
        const auto count=m->native_blocks;
        bool rejected=false;
        try { f3_dispatch(&cpu); }
        catch(const std::runtime_error &e) { rejected=std::string(e.what()).find("Untranslated")!=std::string::npos; }
        require(rejected && cpu.pc==pc && m->native_blocks==count && !m->fallback_instructions,
                "Page gaps, odd PCs and aliases must not execute a nearby entry");
    };
    const f3_block sparse[]={{0x100,native},{0x104,native_other},{0xffe,native},
                            {0x1000,native_other},{0x1ffffe,native},{0x200000,native_other}};
    require(f3_register_blocks(&cpu,sparse,std::size(sparse)),"Sparse ROM table registers");
    hit(0x100,99);hit(0x104,17);hit(0xffe,99);hit(0x1000,17);hit(0x1ffffe,99);
    for(uint32_t pc : {0x101u,0x102u,0xffcu,0x1002u,0x1100u,0x1ffffcu,0x200000u,0xff000100u}) miss(pc);
    const f3_block duplicate[]={{0x100,native},{0x100,native_other}};
    const f3_block unsorted[]={{0x104,native},{0x100,native}};
    const f3_block odd[]={{0x101,native}}, missing[]={{0x100,nullptr}};
    require(!f3_register_blocks(&cpu,duplicate,2) && !f3_register_blocks(&cpu,unsorted,2) &&
            !f3_register_blocks(&cpu,odd,1) && !f3_register_blocks(&cpu,missing,1),
            "Invalid replacement tables are rejected atomically");
    hit(0x104,17);
    const f3_block dense[]={{0x200,native},{0x202,native_other},{0x204,native}};
    require(f3_register_blocks(&cpu,dense,3),"Dense partial page replaces sparse table");
    hit(0x200,99);hit(0x202,17);hit(0x204,99);
    miss(0x100);miss(0x1fe);miss(0x201);miss(0x206);
    m->reset();hit(0x202,17);
    std::vector<uint8_t> saved(m->state_size());m->save_state(saved);
    hit(0x204,99);m->load_state(saved);hit(0x202,17);
    cpu.pc=0x202;cpu.sr=0xa700;
    bool traced=false;
    try { f3_dispatch(&cpu); } catch(const std::runtime_error &) { traced=true; }
    require(traced && cpu.pc==0x202,"Trace mode still rejects block execution in strict-native mode");
    require(f3_register_blocks(&cpu,nullptr,0),"Empty table replaces indexed registration");
    miss(0x202);
}
// Emit-unit replay fixtures: unit 0 bumps a RAM counter and draws entries 3-4 of bank 0;
// unit 1 touches I/O and must abort. Blocks follow the generated hook order (hook first).
constexpr uint32_t synth_starts0[]={0x100}, synth_ends0[]={0x108};
constexpr uint32_t synth_starts1[]={0x200}, synth_ends1[]={0x208};
constexpr f3rt::EmitUnit synth_unit0{0,"synth_bump",synth_starts0,synth_ends0,f3rt::UnitRegister::A3,0x20,f3rt::UnitOwner::Unit};
constexpr f3rt::EmitUnit synth_unit1{1,"synth_io",synth_starts1,synth_ends1,f3rt::UnitRegister::A3,0,f3rt::UnitOwner::Unit};
constexpr std::array<const f3rt::EmitUnit *,2> synth_units{&synth_unit0,&synth_unit1};
F3RT_SPRITE_BEHAVIOUR(synth_bump_behaviour,synth_unit0,"Adds 5 to the unit's first word",
    [](const f3rt::Unit<synth_unit0> &u,f3rt::Patch<synth_unit0> &p) {
        p.u16<0>(uint16_t(u.u16<0>()+5));
        return true;
    });
void synth_start(f3_cpu *cpu) {
    f3_unit_enter(cpu,0);
    const uint16_t v=f3_read16(cpu,cpu->a[3]);
    f3_write16(cpu,cpu->a[3]+2,uint16_t(v+1));
    f3_write16(cpu,0x600030,0xabcd);f3_write16(cpu,0x600032,v);
    cpu->pc=0x104;cpu->cycles+=8;
}
void synth_mid(f3_cpu *cpu) { f3_write16(cpu,0x600040,0x1111);cpu->pc=0x108;cpu->cycles+=4; }
void synth_end(f3_cpu *cpu) { if(f3_unit_exit(cpu,0))return;cpu->pc=0x10a;cpu->cycles+=4; }
void synth_io(f3_cpu *cpu) { f3_unit_enter(cpu,1);cpu->d[2]=f3_read32(cpu,0x4a0000);cpu->pc=0x204;cpu->cycles+=4; }
void synth_io_mid(f3_cpu *cpu) { cpu->pc=0x208;cpu->cycles+=4; }
void synth_io_end(f3_cpu *cpu) { if(f3_unit_exit(cpu,1))return;cpu->pc=0x20a;cpu->cycles+=4; }
const f3_block synth_blocks[]={{0x100,synth_start},{0x104,synth_mid},{0x108,synth_end},
                              {0x200,synth_io},{0x204,synth_io_mid},{0x208,synth_io_end}};
std::unique_ptr<f3rt::Machine> synth_machine(bool units,bool check,bool behaviour) {
    auto m=std::make_unique<f3rt::Machine>(fixture());
    m->allow_main_fallback=false;
    require(f3_register_blocks(&m->cpu,synth_blocks,std::size(synth_blocks)),"Synthetic unit blocks register");
    if(units) {
        f3rt::SpriteUnits::Options options;
        options.check=check;
        if(behaviour)options.behaviours.push_back(&synth_bump_behaviour);
        m->sprite_units=std::make_unique<f3rt::SpriteUnits>(*m,f3rt::SpriteUnitTable{synth_units,{}},std::move(options));
    }
    m->write16(0x400100,0x1234);
    m->cpu.sr=0x2700;m->cpu.a[3]=0x400100;
    return m;
}
void synth_span(f3rt::Machine &m) {
    m.cpu.pc=0x100;
    for(int i=0;i<3;++i)require(f3_dispatch(&m.cpu),"Synthetic unit span dispatches");
}
struct SynthSnapshot {
    std::array<uint8_t,0x20000> ram;std::array<uint8_t,0x40000> graphics;std::array<uint8_t,0x8000> palette;
    std::array<uint32_t,8> d,a;uint32_t pc;uint16_t sr;uint64_t cycles,native_blocks;uint32_t crc,sync_crc;
    explicit SynthSnapshot(f3rt::Machine &m):ram(m.ram),graphics(m.graphics),palette(m.palette),pc(m.cpu.pc),sr(m.cpu.sr),
        cycles(m.cpu.cycles),native_blocks(m.native_blocks),crc(m.state_crc()),sync_crc(m.sync_state_crc()) {
        std::copy(std::begin(m.cpu.d),std::end(m.cpu.d),d.begin());std::copy(std::begin(m.cpu.a),std::end(m.cpu.a),a.begin());
    }
    bool operator==(const SynthSnapshot &o) const {
        return ram==o.ram && graphics==o.graphics && palette==o.palette && d==o.d && a==o.a && pc==o.pc && sr==o.sr &&
               cycles==o.cycles && native_blocks==o.native_blocks && crc==o.crc && sync_crc==o.sync_crc;
    }
};
void check_sprite_unit_sandbox() {
    require(std::string(synth_bump_behaviour.name)=="synth-bump-behaviour","Behaviour names replace underscores with hyphens");
    {   // A completed replay leaves every piece of machine state untouched.
        auto m=synth_machine(true,true,false);
        const SynthSnapshot before(*m);
        m->cpu.pc=0x100;
        const SynthSnapshot at_unit(*m);
        f3_unit_enter(&m->cpu,0);
        require(SynthSnapshot(*m)==at_unit,"Sandbox replay leaves RAM, graphics, cycles, native_blocks and CPU untouched");
        const auto report=m->sprite_units->report();
        require(report.replays==1 && report.completed==1 && report.aborted==0,"Unit replay runs to its end PC");
        (void)before;
    }
    {   // Device access aborts the replay without touching the device.
        auto m=synth_machine(true,true,false);
        m->cpu.pc=0x200;
        const SynthSnapshot at_unit(*m);
        f3_unit_enter(&m->cpu,1);
        require(SynthSnapshot(*m)==at_unit,"Aborted replay leaves machine state untouched");
        const auto report=m->sprite_units->report();
        require(report.replays==1 && report.completed==0 && report.aborted==1 &&
                report.units[1].aborts[size_t(f3rt::SpriteUnits::Abort::Device)]==1,
                "I/O read aborts the replay");
    }
    {   // Unpatched replay matches the real span bit-exactly; the feature never changes machine state.
        auto on=synth_machine(true,true,false),off=synth_machine(false,false,false);
        synth_span(*on);synth_span(*off);
        require(SynthSnapshot(*on)==SynthSnapshot(*off),"Replay on/off yields identical state, cycles and native_blocks");
        require(on->ram[0x102]==0x12 && on->ram[0x103]==0x35,"Real span still executes against unpatched RAM");
        const auto report=on->sprite_units->report();
        require(report.matched==1 && report.mismatched==0 && report.aborted==0 && report.stray_writes==0 &&
                on->sprite_units->passed(),"Check mode matches the real written entries");
        const auto presented=on->sprite_units->presentation();
        require(presented.identity.size()==0x800 && presented.splices.empty(),"No behaviour means no splice");
        require(presented.identity[3] && presented.identity[4] && presented.identity[3]!=presented.identity[4] &&
                !presented.identity[2] && !presented.identity[5],"Written sprite entries receive per-entry identities");
        on->write16(0x600030,0x0001);
        require(!on->sprite_units->presentation().identity[3] && on->sprite_units->presentation().identity[4],
                "A write outside any unit clears that slot's identity");
        require(on->sprite_units->report().stray_unaccounted==2,"Check mode records unaccounted writers");
    }
    {   // A behaviour patches the replay's overlay only and the result is spliced over the real entries.
        auto m=synth_machine(true,false,true);
        synth_span(*m);
        require(m->ram[0x102]==0x12 && m->ram[0x103]==0x35 && m->graphics[0x32]==0x12 && m->graphics[0x33]==0x34,
                "Patched replay never reaches real RAM or sprite RAM");
        const auto presented=m->sprite_units->presentation();
        require(presented.splices.size()==1,"Behaviour produces one splice");
        const auto &splice=presented.splices[0];
        require(!splice.bank && splice.first==3 && splice.last==4 && splice.real.size()==32 && splice.replacement.size()==32 &&
                std::equal(splice.real.begin(),splice.real.end(),m->graphics.begin()+0x30) &&
                splice.replacement[0]==0xab && splice.replacement[1]==0xcd &&
                splice.replacement[2]==0x12 && splice.replacement[3]==0x39 &&
                splice.replacement[16]==0x11 && splice.identity,
                "Splice holds the real range and the patched replay's entries");
        require(m->sprite_units->report().spliced==1,"Splice is counted");
    }
}
void check_wide_bus_boundaries() {
    auto wide=std::make_unique<f3rt::Machine>(fixture());
    auto bytes=std::make_unique<f3rt::Machine>(fixture());
    const auto seed=[](auto &region) {
        for(size_t i=0;i<region.size();++i) region[i]=uint8_t(i*37+11);
    };
    for(auto *m : {wide.get(),bytes.get()}) {
        seed(m->roms.main);seed(m->ram);seed(m->palette);seed(m->graphics);seed(m->shared);
    }
    for(uint32_t boundary : {0u,0x200000u,0x400000u,0x420000u,0x440000u,0x448000u,
                            0x4a0004u,0x4a0013u,0x4c0000u,0x600000u,0x640000u,0x660000u,
                            0xc00000u,0xc00800u,0xc80000u,0xc80100u,0x1000000u}) {
        for(uint32_t alias : {0u,0x5a000000u}) for(unsigned delta=0;delta<7;++delta) {
            const uint32_t a=boundary-3+delta+alias;
            const uint16_t expected16=uint16_t(uint16_t(bytes->read8(a))<<8 | bytes->read8(a+1));
            uint32_t expected32=0;
            for(unsigned i=0;i<4;++i) expected32=(expected32<<8)|bytes->read8(a+i);
            require(wide->read16(a)==expected16 && wide->read32(a)==expected32,
                    "Wide reads preserve byte order across regions, mirrors and address rollover");
            wide->write16(a,0xa1b2);bytes->write8(a,0xa1);bytes->write8(a+1,0xb2);
            wide->write32(a,0x31415926);
            for(unsigned i=0;i<4;++i) bytes->write8(a+i,uint8_t(0x31415926u>>(24-i*8)));
            require(wide->ram==bytes->ram && wide->palette==bytes->palette &&
                    wide->graphics==bytes->graphics && wide->shared==bytes->shared &&
                    wide->control==bytes->control && wide->coin_count==bytes->coin_count &&
                    wide->coin_word==bytes->coin_word && wide->timer_control==bytes->timer_control,
                    "Wide writes preserve ordered byte effects at memory and MMIO boundaries");
        }
    }
    std::vector<uint8_t> a(wide->state_size()),b(bytes->state_size());
    wide->save_state(a);bytes->save_state(b);
    require(a==b,"Wide access preserves complete device state, including reset and EEPROM side effects");
}
void check_main_sound_ordering() {
    const auto sound_machine=[] {
        auto roms=fixture();
        roms.sound[2]=0x80;roms.sound[6]=1; // SSP 8000, PC 100.
        auto m=std::make_unique<f3rt::Machine>(std::move(roms));
        // Real sound CPU: snapshot mailbox byte zero, post a reply, then loop.
        const uint16_t code[]={0x13f9,0x0014,0x0000,0x0000,0x0600,
                               0x13fc,0x005a,0x0014,0x0002,0x60fe};
        for (unsigned i=0;i<std::size(code);++i) m->audio->write16(0x100+2*i,code[i]);
        m->write8(0xc00000,0x11);
        return m;
    };
    {
        auto m=sound_machine();
        m->cpu.cycles=2048;
        f3_write16(&m->cpu,0xc80000,0);
        m->boundary();
        require(m->audio->read8(0x600)==0 && m->shared[1]==0,
                "Sound reset release cannot execute the CPU during preceding main-block time");
        m->cpu.cycles+=1024;m->boundary();
        require(m->audio->read8(0x600)==0x11 && m->shared[1]==0x5a,
                "Sound CPU executes the mailbox program after reset release");
    }
    for (unsigned width : {1,2,4}) {
        auto m=sound_machine();
        m->audio->set_reset(false);m->cpu.cycles=1024;
        if (width==1) f3_write8(&m->cpu,0xc00000,0x22);
        else if (width==2) f3_write16(&m->cpu,0xc00000,0x2233);
        else f3_write32(&m->cpu,0xbfffff,0x99223344); // Unaligned access enters shared RAM.
        m->boundary();
        require(m->audio->read8(0x600)==0x11 && m->shared[0]==0x22,
                "Mailbox writes become visible only after preceding sound execution");
    }
    for (unsigned width : {1,2,4}) {
        auto m=sound_machine();
        m->audio->set_reset(false);m->cpu.cycles=1024;
        const uint32_t value=width==1?f3_read8(&m->cpu,0xc00001):
            width==2?f3_read16(&m->cpu,0xc00000):f3_read32(&m->cpu,0xbfffff);
        require(value==(width==1?0x5au:width==2?0x115au:0xff115a00u),
                "Mailbox reads observe sound replies produced before the current main instruction");
    }
    for (bool reset_instruction : {false,true}) {
        auto m=sound_machine();
        m->audio->set_reset(false);m->cpu.cycles=1024;
        if (reset_instruction) f3_reset_devices(&m->cpu);
        else f3_write8(&m->cpu,0xc80100,0);
        m->boundary();
        require(m->audio->is_reset() && m->audio->read8(0x600)==0x11 && m->shared[1]==0x5a,
                "Reset assertion preserves sound execution preceding the reset instruction");
    }
}
void check_audio_partitioning() {
    const auto run=[](unsigned quantum) {
        auto m=std::make_unique<f3rt::Machine>(fixture());
        m->audio->write32(0,0xff00);m->audio->write32(4,0x1000);m->audio->write32(0x100,0x2000);
        // Foreground counts iterations; each timer IRQ records the interrupted count.
        const uint16_t foreground[]={0x7000,0x227c,0,0x600,0x46fc,0x2000,0x5280,0x60fc};
        const uint16_t handler[]={0x22c0,0x1239,0x0028,0x001f,0x4e73};
        for (unsigned i=0;i<std::size(foreground);++i) m->audio->write16(0x1000+2*i,foreground[i]);
        for (unsigned i=0;i<std::size(handler);++i) m->audio->write16(0x2000+2*i,handler[i]);
        m->audio->write8(0x280019,64);m->audio->write8(0x28000b,8);
        m->audio->write8(0x28000d,0);m->audio->write8(0x28000f,125);m->audio->write8(0x280009,0x60);
        m->audio->set_reset(false);
        for (unsigned elapsed=0;elapsed<10050;) {
            const unsigned step=quantum<10050-elapsed?quantum:10050-elapsed;
            m->audio->advance(step);elapsed+=step;
        }
        std::array<uint32_t,11> state{};
        for (unsigned i=0;i<10;++i) {
            state[i]=m->audio->read32(0x600+4*i);
            require(state[i]>(i?state[i-1]:0),"Each periodic IRQ observes further real foreground execution");
        }
        require(m->audio->read32(0x628)==0,"Ten elapsed timer deadlines produce exactly ten interrupt records");
        state[10]=m->interpreter->sound_pc();
        return state;
    };
    const auto single_clock=run(1);
    for (unsigned quantum : {7,64,511,4096})
        require(run(quantum)==single_clock,
                "Sound IRQ recognition and CPU state are independent of main-block time partitioning");
}
void check_duart_counter() {
    const auto preset=[](f3rt::MC68681 &d,unsigned count) { d.write(6,count>>8);d.write(7,count); };
    f3rt::MC68681 restart;
    preset(restart,3);restart.write(4,0x30);restart.read(14);restart.advance(17);
    restart.read(14);restart.advance(47);
    require((restart.read(5)&8)==0,"Restarting the counter discards the preceding divider phase");
    restart.advance(1);
    require((restart.read(5)&8)!=0 && restart.read(6)==0xff && restart.read(7)==0xff,
            "Counter expires at the new deadline and reloads the reference 0xffff period");
    restart.advance(16);
    require(restart.read(7)==0xfe,"Counter underflow reload is independent of the programmed preset");
    restart.read(15);restart.advance(1000000);
    require((restart.read(5)&8)==0,"Stop-counter read acknowledges and cancels counter-mode expiration");

    f3rt::MC68681 mode;
    preset(mode,3);mode.write(4,0x30);mode.read(14);mode.advance(17);mode.read(15);
    mode.write(4,0x60);mode.advance(5);
    require((mode.read(5)&8)==0,"Entering timer mode starts a fresh full period without old divider residue");
    mode.advance(1);
    require((mode.read(5)&8)!=0,"Timer ready asserts after both half-periods");
    mode.read(15);mode.advance(6);
    require((mode.read(5)&8)!=0,"Timer-mode acknowledgement does not stop periodic interrupts");

    f3rt::MC68681 source;
    preset(source,2);source.write(4,0x70);source.advance(8);source.write(4,0x60);
    source.advance(23);
    require(source.read(7)==1 && (source.read(5)&8)==0,"Clock-source change preserves the armed duration");
    source.advance(1);source.advance(1);
    require((source.read(5)&8)==0,"The next half-period uses the newly selected source");
    source.advance(1);
    require((source.read(5)&8)!=0,"Reload adopts the new clock without rescaling elapsed time");

    f3rt::MC68681 reset;
    reset.advance(1000000);
    require(reset.read(5)==0,"A cold DUART has no scheduled counter event");
    preset(reset,100);reset.write(4,0x60);reset.advance(37);reset.reset();
    require(reset.read(5)==0 && !reset.irq_pending(),"Board reset clears the visible IRQ registers");
    reset.advance(62);
    require(reset.read(5)==0,"Board reset preserves the remaining deadline, not a restarted period");
    reset.advance(1);
    require(reset.read(5)==8 && !reset.irq_pending(),"Retained expiration latches counter-ready while reset IMR masks IRQ");
    reset.write(12,64);reset.write(5,8);
    require(reset.irq_pending() && reset.get_irq_vector()==64 && reset.irq_pending(),
            "Unmasking retained counter-ready asserts IRQ; IACK supplies vector without clearing it");
    reset.read(15);
    require(!reset.irq_pending(),"Counter acknowledge clears a retained post-reset interrupt");
}
void check_duart_tx() {
    for (unsigned channel : {0,1}) {
        f3rt::MC68681 duart;
        const unsigned base=channel*8,half=channel?64:32;
        const uint8_t mask=channel?0x10:0x01;
        bool irq=false;
        duart.set_irq_callback([&](bool asserted) { irq=asserted; });
        const auto configure=[&] {
            duart.write(base,0x13);duart.write(base,0x0f); // 8N2
            duart.write(base+1,0xee); // F3 external clock / 16
            duart.write(5,mask);duart.write(base+2,4);
        };
        require((duart.read(base+1)&0x0c)==0,"Reset disables the UART transmitter");
        configure();
        require((duart.read(base+1)&0x0c)==0x0c && irq,"Enabling an idle transmitter asserts ready/empty and its IRQ");
        duart.write(base+3,0x80);
        require((duart.read(base+1)&0x0c)==0 && !irq,"THR write clears ready and empty");
        duart.advance(3*half-1);
        require((duart.read(base+1)&0x0c)==0,"TX ready waits through the start-bit time");
        duart.advance(1);
        require((duart.read(base+1)&0x0c)==4 && irq,"The second rising edge releases THR and asserts TX ready");
        duart.advance(18*half-1);
        require((duart.read(base+1)&8)==0,"TX empty stays clear until the last framed bit");
        duart.advance(1);
        require((duart.read(base+1)&0x0c)==0x0c,"First 8N2 frame completes after 21 half-bit clocks");
        duart.write(base+3,0x90);duart.advance(4*half);
        duart.write(base+3,0x7f);duart.write(base+3,0x44); // One holding slot; third byte is rejected.
        require((duart.read(base+1)&0x0c)==0 && !irq,"A queued byte occupies THR while the current byte shifts");
        duart.advance(18*half);
        require((duart.read(base+1)&0x0c)==4 && irq,"Buffered transfer restores ready without asserting empty");
        duart.advance(22*half-1);
        require((duart.read(base+1)&8)==0,"Queued frame retains the continuous serial edge phase");
        duart.advance(1);
        require((duart.read(base+1)&0x0c)==0x0c,"Holding-register overflow does not enqueue an extra frame");
        duart.reset();configure();duart.write(base+3,0x80);duart.advance(22*half-1);
        require((duart.read(base+1)&8)==0,"Board reset retains the stopped serial clock edge state");
        duart.advance(1);
        require((duart.read(base+1)&8)!=0,"Post-reset frame uses a full first bit period");
        duart.write(base+3,0x55);duart.advance(4*half);duart.write(base+2,0x30);duart.advance(100*half);
        require((duart.read(base+1)&0x0c)==0 && !irq,"Transmitter reset aborts the frame and clears its IRQ");
        duart.write(base+2,0x10);duart.write(base,0);duart.write(base,7); // 5E1
        duart.write(base+1,0xef);duart.write(base+2,4);duart.write(base+3,0x15);
        const unsigned fast_half=half/16;
        duart.advance(16*fast_half-1);
        require((duart.read(base+1)&8)==0,"Word length, parity and direct external clock determine frame duration");
        duart.advance(1);
        require((duart.read(base+1)&8)!=0,"5E1 frame completes at its eight-bit boundary");
    }
    f3rt::MC68681 counter_clock;
    counter_clock.write(6,0);counter_clock.write(7,1);counter_clock.write(4,0x60);
    counter_clock.write(8,0x13);counter_clock.write(8,0x0f);
    counter_clock.write(9,0xed);counter_clock.write(10,4);counter_clock.write(11,0x80);
    counter_clock.read(14);
    counter_clock.advance(46);
    require((counter_clock.read(9)&0x0c)==0,"Counter-derived TX clock retains the divide-by-16 prescaler");
    counter_clock.advance(1);
    require((counter_clock.read(9)&0x0c)==4,"Counter-derived TX ready follows the second serial rising edge");
    counter_clock.advance(287);
    require((counter_clock.read(9)&8)==0,"Counter-derived TX empty waits for the full frame");
    counter_clock.advance(1);
    require((counter_clock.read(9)&0x0c)==0x0c,"Counter-derived 8N2 frame completes at its eleventh serial edge");
}
void check_sound_cycles(f3rt::Machine &m) {
    struct Result { int cycles;uint32_t d0,a1;uint16_t sr; };
    const auto execute = [&](std::initializer_list<uint16_t> instruction,uint32_t source=0,uint32_t d0=0) {
        m.audio->set_reset(true);
        m.audio->write32(0,0xff00);m.audio->write32(4,0x1000);m.audio->write32(0x4000,source<<16);
        uint32_t pc=0x1000;
        const auto emit = [&](uint16_t word) { m.audio->write16(pc,word);pc+=2; };
        for (uint16_t word : {uint16_t(0x203c),uint16_t(d0>>16),uint16_t(d0),
                              uint16_t(0x223c),uint16_t(source>>16),uint16_t(source),
                              uint16_t(0x227c),uint16_t(0),uint16_t(0x4000),uint16_t(0x46fc),uint16_t(0x271b)})
            emit(word);
        for (auto word : instruction) emit(word);
        // Save SR before the MOVE instructions used to observe D0/A1 change flags.
        for (uint16_t word : {0x40f8,0x1508,0x21c0,0x1500,0x21c9,0x1504}) emit(word);
        m.audio->set_reset(false);
        for (int i=0;i<5;++i) m.interpreter->run_audio(1); // Reset plus register/SR setup.
        const int cycles=m.interpreter->run_audio(1);
        for (int i=0;i<3;++i) m.interpreter->run_audio(1);
        const Result result{cycles,m.audio->read32(0x1500),m.audio->read32(0x1504),m.audio->read16(0x1508)};
        m.audio->set_reset(true);
        return result;
    };
    const auto quick=execute({0x5049});
    require(quick.cycles==8 && quick.a1==0x4008 && quick.sr==0x271b,
            "68000 ADDQ.W to an address register takes eight cycles and preserves flags");
    require(execute({0x544f}).cycles==8,"68000 ADDQ.W stack adjustment has the same full cost");
    require(execute({0xd2fc,10}).cycles==12 && execute({0x92fc,10}).cycles==12,
            "68000 immediate word address arithmetic has no long-operand surcharge");
    require(execute({0xd3fc,0,10}).cycles==16 && execute({0x93fc,0,10}).cycles==16,
            "68000 immediate long address arithmetic retains its surcharge");
    for (uint16_t family : {0xd000,0x9000,0xc000,0x8000}) {
        require(execute({uint16_t(family|0x3c),1}).cycles==8 &&
                execute({uint16_t(family|0x7c),1}).cycles==8 &&
                execute({uint16_t(family|0xbc),0,1}).cycles==16,
                "68000 immediate EA arithmetic charges byte, word and long operands distinctly");
        require(execute({uint16_t(family|0x81)}).cycles==8,
                "68000 long register arithmetic takes eight cycles");
        if (family==0xd000 || family==0x9000)
            require(execute({uint16_t(family|0x89)}).cycles==8,"68000 long arithmetic accepts address-register sources");
    }
    for (uint16_t opcode : {0xd3c0,0xd3c8,0x93c0,0x93c8})
        require(execute({opcode}).cycles==8,"68000 long address arithmetic has an eight-cycle register cost");
    const auto tas=execute({0x4ad1});
    require(tas.cycles==14 && tas.sr==0x2714 && m.audio->read8(0x4000)==0x80,
            "68000 memory TAS charges its read-modify-write once and reports the original byte");
    require(execute({0x4ac0}).cycles==4 && execute({0x4ad9}).cycles==14 &&
            execute({0x4ae1}).cycles==16 && execute({0x4ae9,16}).cycles==18 &&
            execute({0x4af8,0x4000}).cycles==18 && execute({0x4af9,0,0x4000}).cycles==22,
            "68000 TAS retains register and effective-address timing distinctions");
    for (unsigned kind=1;kind<=3;++kind) for (unsigned bit : {0,2,15,16,31,32,47,48,63}) {
        const int cycles=(kind==2?8:6)+((bit&31)>=16?2:0);
        const uint32_t result=kind==2?0:1u<<(bit&31);
        const auto reg=execute({uint16_t(0x0300|(kind<<6))},bit);
        const auto immediate=execute({uint16_t(0x0800|(kind<<6)),uint16_t(bit)});
        require(reg.cycles==cycles && immediate.cycles==cycles+4,
                "68000 register bit mutations distinguish low/high halves after modulo-32 selection");
        require(reg.d0==result && immediate.d0==result && reg.sr==0x271f && immediate.sr==0x271f,
                "Bit timing preserves result, old-bit Z and untouched X/N/V/C flags");
    }
    const struct { uint32_t dividend;uint16_t divisor;int cycles; } divisions[]={
        {0,1,136},{0xffff,1,106},{0x10000,1,10},{0x10000,2,134},
        {0xff1234,0x100,116},{0x80000000,0xffff,132},{0xfffeffff,0xffff,76}
    };
    for (const auto &test : divisions) {
        const auto reg=execute({0x80c1},test.divisor,test.dividend);
        const auto immediate=execute({0x80fc,test.divisor},test.divisor,test.dividend);
        const auto memory=execute({0x80d1},test.divisor,test.dividend);
        require(reg.cycles==test.cycles && immediate.cycles==test.cycles+4 && memory.cycles==test.cycles+4,
                "68000 DIVU.W timing follows the operand-dependent subtract stages and EA cost");
        const auto quotient=test.dividend/test.divisor;
        const bool overflow=quotient>0xffff;
        const auto result=overflow?test.dividend:((test.dividend%test.divisor)<<16)|quotient;
        const uint16_t flags=0x10|(overflow?2:((quotient==0?4:0)|(quotient&0x8000?8:0)));
        const uint16_t mask=overflow?0x13:0x1f; // N/Z are undefined on overflow.
        require(reg.d0==result && immediate.d0==result && memory.d0==result &&
                (reg.sr&mask)==flags && (immediate.sr&mask)==flags && (memory.sr&mask)==flags,
                "DIVU timing preserves packed remainder/quotient, overflow destination and defined flags");
    }
    const struct { uint16_t source;int cycles; } multiplications[]={
        {0,38},{1,42},{0x12,46},{0x5555,70},{0x7fff,42},{0x8000,40},{0xffff,40},{0xaaaa,68}
    };
    for (const auto &test : multiplications) {
        const auto reg=execute({0xc1c1},test.source,0xfffffffd);
        const auto immediate=execute({0xc1fc,test.source},test.source,0xfffffffd);
        const auto memory=execute({0xc1d1},test.source,0xfffffffd);
        require(reg.cycles==test.cycles && immediate.cycles==test.cycles+4 && memory.cycles==test.cycles+4,
                "68000 MULS.W charges every Booth transition including the positive source's final transition");
        const int32_t product=int32_t(int16_t(test.source))*-3;
        const uint16_t sr=0x2710|(product==0?4:product<0?8:0);
        require(reg.d0==uint32_t(product) && immediate.d0==uint32_t(product) && memory.d0==uint32_t(product) &&
                reg.sr==sr && immediate.sr==sr && memory.sr==sr,
                "Signed multiply preserves the full product and X while replacing N/Z/V/C");
    }
}
void check_sound_irq(f3rt::Machine &m) {
    // These core-cycle checks step the CPU explicitly, separately from the timer.
    m.audio->set_cpu_runner({});
    for (unsigned vector : {15,30,64,255}) {
        m.audio->reset_board();
        m.audio->write32(0,0xff00);m.audio->write32(4,0x1000);m.audio->write32(vector*4,0x2000);
        m.audio->write16(0x1000,0x4e72);m.audio->write16(0x1002,0x2000);m.audio->write16(0x2000,0x4e71);
        m.audio->set_reset(false);m.interpreter->run_audio(1);m.interpreter->run_audio(1);
        m.audio->write8(0x280019,vector);m.audio->write8(0x28000d,0);m.audio->write8(0x28000f,1);
        m.audio->write8(0x28000b,8);m.audio->write8(0x280009,0x60);m.audio->advance(8);
        require(m.audio->irq_level()==6,"DUART timer wakes the stopped sound CPU on IRQ6");
        require(m.interpreter->run_audio(1)==48 && m.interpreter->sound_pc()==0x2002,
                "68000 IRQ entry costs 44 cycles independently of vector number, plus the handler NOP");
        require(m.audio->read16(0xfefa)==0x2000 && m.audio->read32(0xfefc)==0x1004,
                "Vectored IRQ entry preserves the stopped CPU's SR and return PC");
    }
    for (int budget : {1,2,3,4,8,16,32,47,48,52}) {
        m.audio->reset_board();
        m.audio->write32(0,0xff00);m.audio->write32(4,0x1000);m.audio->write32(0x100,0x2000);
        m.audio->write16(0x1000,0x4e72);m.audio->write16(0x1002,0x2000);m.audio->write16(0x2000,0x4e71);
        m.audio->set_reset(false);m.interpreter->run_audio(1);
        m.audio->write8(0x280019,64);m.audio->write8(0x28000b,1);m.audio->write8(0x280005,4);
        require(m.audio->irq_level()==6,"TX-ready IRQ is pending while reset SR masks interrupts");
        require(m.interpreter->run_audio(budget)==52 && m.interpreter->sound_pc()==0x2000,
                "STOP unmasking an IRQ retains four instruction clocks, a four-clock poll and 44 entry clocks");
        require(m.audio->read16(0xfefa)==0x2000 && m.audio->read32(0xfefc)==0x1004 &&
                m.interpreter->run_audio(1)==4 && m.interpreter->sound_pc()==0x2002,
                "Immediate STOP wake stacks the next PC and resumes the handler without remaining stopped");
    }
    m.audio->reset_board();
    m.audio->set_cpu_runner([&m](int cycles) { return m.interpreter->run_audio(cycles); });
}
void check_trap_cycles(f3rt::Machine &m) {
    const auto old_vbr=m.cpu.vbr;
    for (unsigned trap=0;trap<16;++trap) for (bool reference : {false,true}) {
        m.cpu.pc=0x400700;m.cpu.sr=0x2700;m.cpu.a[7]=0x401000;m.cpu.vbr=0x400000;
        m.write16(0x400700,uint16_t(0x4e40|trap));
        m.write32(m.cpu.vbr+(32+trap)*4,0x400720);
        const auto before=m.cpu.cycles;
        if (reference) m.interpreter->run_main(1);
        else f3_exception(&m.cpu,32+trap,0x400702);
        require(m.cpu.cycles-before==24 && m.cpu.pc==0x400720,
                "TRAP #n charges 24 cycles before entering its vector");
        require(m.cpu.a[7]==0x400ff8 && m.read16(m.cpu.a[7])==0x2700 &&
                m.read32(m.cpu.a[7]+2)==0x400702 && m.read16(m.cpu.a[7]+6)==(32+trap)*4,
                "TRAP #n stacks the resume PC in a format-0 frame");
    }
    m.cpu.vbr=old_vbr;
}
void check_movem(f3rt::Machine &m) {
    for (uint16_t opcode : {0x4891,0x48d1,0x48a1,0x48e1,0x4c99,0x4cd9}) {
        const bool load=opcode&0x0400, wide=opcode&0x0040, predec=(opcode&0x0038)==0x20;
        const unsigned size=wide?4:2;
        const auto execute = [&](uint16_t mask) {
            m.write16(0x400600,opcode);m.write16(0x400602,mask);
            m.cpu.pc=0x400600;m.cpu.sr=0x2700;m.cpu.a[1]=0x400a20;
            m.cpu.d[0]=0x12345678;m.cpu.d[1]=0x89abcdef;
            if (load) {
                if (wide) { m.write32(0x400a20,0x12345678);m.write32(0x400a24,0x89abcdef); }
                else { m.write16(0x400a20,0x5678);m.write16(0x400a22,0xcdef); }
            }
            const auto before=m.cpu.cycles;
            m.interpreter->run_main(1);
            require(m.cpu.pc==0x400604,"MOVEM executes exactly one instruction");
            return m.cpu.cycles-before;
        };
        const auto base=execute(0);
        const auto cycles=execute(predec?0xc000:3);
        require(cycles-base==(load?8u:6u),"EC020 MOVEM charges three cycles/store and four/load per register");
        if (load) {
            require(m.cpu.d[0]==(wide?0x12345678u:0x5678u) &&
                    m.cpu.d[1]==(wide?0x89abcdefu:0xffffcdefu) &&
                    m.cpu.a[1]==0x400a20+2*size,"MOVEM load width, sign extension and postincrement");
        } else {
            const uint32_t address=predec?0x400a20-2*size:0x400a20;
            require((wide?m.read32(address):m.read16(address))==(wide?0x12345678u:0x5678u) &&
                    (wide?m.read32(address+size):m.read16(address+size))==(wide?0x89abcdefu:0xcdefu) &&
                    m.cpu.a[1]==address,"MOVEM store width, register order and predecrement");
        }
    }
}
void check_rotate_cycles(f3rt::Machine &m) {
    const auto execute = [&](uint16_t opcode,unsigned count) {
        m.write16(0x400700,opcode);
        m.cpu.pc=0x400700;m.cpu.sr=0x2710;m.cpu.d[0]=0x12345678;m.cpu.d[1]=count;
        const auto before=m.cpu.cycles;
        m.interpreter->run_main(1);
        require(m.cpu.pc==0x400702,"Shift/rotate executes one instruction");
        return m.cpu.cycles-before;
    };
    for (unsigned kind=0;kind<4;++kind) for(unsigned left=0;left<2;++left) for(unsigned size=0;size<3;++size) {
        const uint16_t form=uint16_t(0xe000|(kind<<3)|(left<<8)|(size<<6));
        const uint16_t reg=uint16_t(form|0x0220);
        const auto base=execute(reg,0);
        for(unsigned count : {1,8,9,16,17,31,32,33,63})
            require(execute(reg,count)==base,"EC020 register shifts/rotates have no count-dependent cycle surcharge");
        require(execute(uint16_t(form|0x0200),0)==execute(form,0),
                "EC020 immediate counts one and eight have identical timing");
    }
    execute(0xe898,0); // ROR.L #4,D0, used by the ROM's early boot path.
    require(m.cpu.d[0]==0x81234567 && (m.cpu.sr&0x1f)==0x19,"ROR.L result, carry and preserved extend flag");
}
void check_dsp_boundaries() {
    f3rt::ES5510 dsp;
    std::vector<uint8_t> saved(dsp.state_size());
    const auto capture=[&] {
        f3rt::StateWriter writer(saved);dsp.save_state(writer);
        f3rt::CanonicalES5510Registers state{};
        f3rt::StateReader reader(saved);reader.read(state);return state;
    };
    const auto restore=[&](const f3rt::CanonicalES5510Registers &state) {
        f3rt::StateWriter writer(saved);writer.write(state);
        f3rt::StateReader reader(saved);dsp.load_state(reader);
    };
    for(bool halted : {false,true}) for(int end : {-1,0,1,159}) {
        dsp.reset();
        if(end>=0) dsp.instr_at(end)=0xf000;
        auto state=capture();state.state=halted?1:0;restore(state);
        dsp.run_once();state=capture();
        const unsigned expected=end<0?(halted?200:201):end==0?(halted?1:161):unsigned(end+1);
        require(state.pc==expected && state.state==(end<0?0:1) && state.halt_asserted,
                "DSP END and safety budget preserve wake-up and first-cycle HALT ordering");
    }
    for(unsigned pc : {159u,160u,255u}) {
        dsp.reset();auto state=capture();state.pc=uint8_t(pc);restore(state);
        dsp.run_once();state=capture();
        require(state.pc==uint8_t(pc+201) && state.state==0,
                "DSP no-END safety cutoff preserves the 8-bit PC wrap above the instruction store");
    }
    for(int address : {-1,0,16,17,33,34,127}) {
        dsp.reset();dsp.instr_at(1)=0xf000;
        auto state=capture();state.pc=1;state.dbase=address;
        state.dlength=16;state.memincrement=1;state.memshift=0;state.memmask=0xffffff;
        restore(state);dsp.run_once();state=capture();
        require(state.ram_p.address==((address%17)&0xffffff),
                "DSP delay addressing preserves signed remainder and zero, one and multiple wraps");
    }
}
void check_audio_mixer() {
    f3rt::Audio audio;
    const std::array<uint8_t,4> rom{0x40,0,0x40,0};
    audio.load_sample_rom(rom);
    audio.write16(0x20001e,0x20);
    for (unsigned reg=1;reg<=6;++reg) audio.write16(0x200000+reg*2,0x4000);
    audio.write16(0x20001e,0);
    audio.write16(0x200010,0xff00);audio.write16(0x200012,0xff00);
    audio.write16(0x200000,0x0c00); // Constant sample, all poles lowpass, auxiliary pair.
    std::array<int16_t,2> pcm{};
    const auto sample = [&] {
        audio.advance(538);
        require(audio.render(pcm.data(),1)==1,"One complete audio sample is available");
    };
    sample();
    // DC 0x4000, OTIS volume 15.5, /2^19, board 0.18, two 100/32
    // gain stages, auxiliary route 0.5, and signed PCM scale 32768.
    require(pcm[0]==13950 && pcm[1]==13950,"Board gain and signed PCM normalization");
    audio.write8(0x340000,2);audio.write8(0x340002,0);
    sample();
    require(pcm[0]==0 && pcm[1]==13950,"Left volume mute preserves the right channel");
    audio.write8(0x340000,7);audio.write8(0x340002,0x33);
    sample();
    require(pcm[0]==0 && pcm[1]==3487,"Minus-six-dB control applies both baseline gain stages");
    audio.set_gain_model(f3rt::Audio::GainModel::SingleStage);
    sample();
    require(pcm[0]==0 && pcm[1]==715,"Single-stage gain is distinct and preserves channel mute");
    audio.set_reset(false);audio.set_reset(true);
    sample();
    require(pcm[0]==0 && pcm[1]==715,"CPU-line reset preserves attenuation and playing OTIS voices");
    audio.reset_board();
    sample();
    require(pcm[0]==1428 && pcm[1]==1428,"Board reset restores volume without resetting OTIS voices");
}
}
int main() try {
    check_game_rom_video();
    check_fdp_geometry();
#ifdef F3RT_GAME_VIDEO
    // The per-game scene decoders only exist for a game with games/<game>/video/.
    check_game_tile_observation();
    check_game_tile_row_sampling();
    check_game_sprite_vram_block_chaining();
    check_game_line_vram_carry_forward();
    check_game_text_vram_decode();
#endif
    check_dsp_boundaries();
    check_audio_mixer();
    check_native_lookup();
    check_sprite_unit_sandbox();
    check_wide_bus_boundaries();
    check_local_inputs();
    check_dial_inputs();
    check_input_script();
    check_factory_eeprom();
    check_main_sound_ordering();
    check_audio_partitioning();
    f3rt::Audio clock_audio;
    std::array<int16_t, 128> clock_samples{};
    uint64_t sample_count=0;
    for (unsigned i=0;i<10000;++i) {
        clock_audio.advance(16000);
        sample_count+=clock_audio.render(clock_samples.data(),clock_samples.size()/2);
    }
    require(sample_count==uint64_t(clock_audio.sample_rate())*10,
            "Ten seconds of audio match the advertised stream rate without clock drift");
    clock_audio.advance(638);
    clock_audio.reset_board();
    clock_audio.advance(438);
    require(clock_audio.render(clock_samples.data(),clock_samples.size()/2)==2,
            "Board reset preserves queued audio and fractional sample-clock phase");
    auto m=std::make_unique<f3rt::Machine>(fixture());
    const uint32_t main_crc=f3rt::crc32(m->roms.main.data(),m->roms.main.size());
    require(f3_validate_main_rom(&m->cpu,m->roms.main.size(),main_crc),
            "Native main image binding accepts the loaded revision");
    m->roms.main[0x104]=1;
    bool mismatched=false;
    try { f3_validate_main_rom(&m->cpu,m->roms.main.size(),main_crc); }
    catch(const std::runtime_error &) { mismatched=true; }
    require(mismatched && !m->blocks,"Wrong native image is rejected before dispatch registration");
    m->roms.main[0x104]=0;
    mismatched=false;
    try { f3_validate_main_rom(&m->cpu,0x100000,main_crc); }
    catch(const std::runtime_error &) { mismatched=true; }
    require(mismatched,"Native image length is bound even when its CRC matches");
    require(m->cpu.cycles==4 && m->cpu.pc==0x100 && m->cpu.d[0]==0,
            "Cold reset charges four cycles without executing the first opcode");
    require(m->cpu.dispatch_deadline==0,"Reset requires a fresh scheduling boundary");
    const auto raster_tick = [](uint64_t pixels) {
        return (pixels*f3rt::Machine::main_clock+f3rt::Machine::pixel_clock-1)/f3rt::Machine::pixel_clock;
    };
    const auto boundary_at = [&](uint64_t tick) { m->cpu.cycles=tick;m->boundary(); };
    const auto first_vblank=raster_tick(f3rt::Machine::frame_pixels);
    boundary_at(first_vblank-1);
    require(m->frame==0 && m->pending_irqs==0,"First vblank waits one full frame from the VBSTART epoch");
    require(m->cpu.dispatch_deadline==first_vblank,"Native deadline is the first vblank event");
    boundary_at(first_vblank);
    require(m->frame==1 && m->pending_irqs==(1<<2),"First vblank renders and requests IRQ2 at its deadline");
    require(m->cpu.dispatch_deadline==first_vblank+10000,"Delayed IRQ3 becomes the next native deadline");
    boundary_at(first_vblank+9999);
    require(m->pending_irqs==(1<<2),"IRQ3 is not requested before its 10000-cycle delay");
    boundary_at(first_vblank+10000);
    require(m->pending_irqs==((1<<2)|(1<<3)),"IRQ3 is requested at its delayed deadline");
    require(m->cpu.dispatch_deadline==raster_tick(2ull*f3rt::Machine::frame_pixels),
            "Serviced timer deadlines advance to the next absolute frame");
    m->pending_irqs=0;
    const auto second_vblank=raster_tick(2ull*f3rt::Machine::frame_pixels);
    boundary_at(second_vblank-1);
    require(m->frame==1 && m->pending_irqs==0,"Next vblank retains the absolute raster phase");
    boundary_at(second_vblank);
    require(m->frame==2 && m->pending_irqs==(1<<2),"Second vblank uses the full-frame epoch");
    m->pending_irqs=0;
    check_movem(*m);
    check_rotate_cycles(*m);
    check_trap_cycles(*m);
    check_sound_cycles(*m);
    check_sound_irq(*m);
    check_duart_tx();
    check_duart_counter();
    m->write32(0x400001,0x12345678);
    require(m->read32(0x420001)==0x12345678,"BE misaligned work RAM mirror");
    m->write32(0x41fffe,0xaabbccdd);
    require(m->read16(0x400000)==0xccdd && m->read16(0x41fffe)==0xaabb,"Work RAM wrap across mirror");
    m->write8(0x100,0xff);require(m->read16(0x100)==0x702a,"ROM is read-only");
    m->write8(0xc00010,0x75);require(m->audio->read16(0x140020)==0x75ff,"DPRAM sound high-byte lane");
    m->audio->write16(0x140020,0xaabb);require(m->read8(0xc00010)==0xaa,"DPRAM reverse lane");
    m->audio->write16(0x20001e,0); // Voice 0, register page
    m->audio->write16(0x200014,0x0246);m->audio->write16(0x200016,0x8a00);
    m->audio->write16(0x20001e,0x20); // Stopped voice sample-ROM readback
    require(m->audio->read16(0x20000c)==0x4567,"OTIS stopped voice reads full 20-bit sample address");
    m->audio->write16(0x600,0xa55a);m->audio->set_reset(false);
    m->audio->set_reset(true);m->audio->set_reset(false);
    require(m->audio->read16(0x600)==0xa55a,"Sound CPU RESET preserves board work RAM");
    m->write8(0x4a0004,0x04);m->write8(0x4a0004,0x04);require(m->coin_count[0]==1,"Coin counter rising-edge only");
    m->set_input(0,0x1000,true);require(!(m->read32(0x4a0000)&0x1000),"Active-low start input");
    f3rt::Eeprom e;
    uint64_t now=100;
    serial_write(e,63,0x1234,now);require(e.words[63]==0xffff,"EEPROM write disabled at power-on");
    command(e,0x130,now);e.pins(0,now); // EWEN
    serial_write(e,63,0x1234,now);
    require(e.output(now),"Deselected EEPROM DO is pulled high while programming");
    e.pins(0x10,now);
    require(!e.output(now),"Raising CS exposes EEPROM programming busy");
    serial_write(e,0,0xdead,now);
    require(e.words[0]==0xffff,"EEPROM ignores new commands while programming");
    e.pins(0x10,now);
    now+=27999;require(!e.output(now),"EEPROM write remains busy before 1750us deadline");
    ++now;require(e.output(now),"EEPROM write finishes without another clock edge");
    serial_write(e,0,0xabcd,now);now+=28000;
    command(e,0x1bf,now);require(!e.output(now),"EEPROM read dummy bit");
    require(read_word(e,now)==0x1234 && read_word(e,now)==0xabcd,"EEPROM sequential read wraps 63 to 0");
    e.pins(0,now);command(e,0x100,now);e.pins(0,now);serial_write(e,0,0x4321,now);
    e.pins(0x10,now);
    require(e.words[0]==0xabcd && e.output(now),"EEPROM EWDS protects contents without becoming busy");
    command(e,0x130,now);command(e,0x1ff,now); // EWEN; erase word 63
    e.pins(0,now);e.pins(0x10,now);
    now+=15999;require(!e.output(now),"EEPROM erase remains busy before 1000us deadline");
    ++now;require(e.output(now) && e.words[63]==0xffff,"EEPROM single-word erase completes");
    command(e,0x120,now);e.pins(0,now);e.pins(0x10,now); // ERAL
    now+=127999;require(!e.output(now),"EEPROM erase-all remains busy before 8000us deadline");
    ++now;require(e.output(now) && e.words[0]==0xffff,"EEPROM erase-all completes");
    command(e,0x110,now); // WRAL
    for(int bit=15;bit>=0;--bit)send_bit(e,(0x5a5a>>bit)&1,now);
    e.pins(0,now);e.pins(0x10,now);
    now+=127999;require(!e.output(now),"EEPROM write-all remains busy before 8000us deadline");
    ++now;require(e.output(now) && e.words[0]==0x5a5a && e.words[63]==0x5a5a,"EEPROM write-all completes");
    e.reset();e.pins(0x10,0);
    require(e.output(0) && e.words[0]==0x5a5a,"Power reset clears serial timing but preserves EEPROM contents");
    auto &cpu=m->cpu;
    cpu.usp=0x400800;cpu.a[7]=0x401000;cpu.sr=0x2600;
    require(m->boundary()==0 && cpu.dispatch_deadline>cpu.cycles,"Runnable boundary publishes a future deadline");
    const auto masked_deadline=cpu.dispatch_deadline;
    f3_set_sr(&cpu,0x2715);
    require(cpu.dispatch_deadline==masked_deadline,"Raising the IRQ mask preserves the event deadline");
    f3_set_sr(&cpu,0);require(cpu.a[7]==0x400800 && cpu.ssp==0x401000,"Supervisor to user stack switch");
    require(cpu.dispatch_deadline==0,"Lowering the IRQ mask invalidates the cached deadline");
    m->write8(0x4a0000,0);
    const auto watchdog_deadline=cpu.cycles+3ull*f3rt::Machine::main_clock;
    require(cpu.dispatch_deadline==0,"Watchdog strobe cannot suppress an outstanding IRQ recheck");
    m->write32(0x400000+26*4,0x400300);cpu.vbr=0x400000;cpu.pc=0x100;cpu.stopped=1;
    m->pending_irqs=1<<2;require(f3_boundary(&cpu)!=0,"IRQ redirects boundary");
    require(cpu.pc==0x400300 && !cpu.stopped && (cpu.sr&0x2700)==0x2200,"IRQ releases STOP and raises mask");
    require(cpu.dispatch_deadline==0,"IRQ redirect requires lookup through a fresh boundary");
    require(cpu.a[7]==0x400ff8 && m->read32(cpu.a[7]+2)==0x100 && m->read16(cpu.a[7]+6)==104,"68020 interrupt frame");
    m->write32(cpu.vbr+5*4,0x400310);m->write16(0x400310,0x4e73); // RTE handler
    cpu.pc=0x100;cpu.sr=0x2700;
    const auto exception_cycles=cpu.cycles;const auto exception_sp=cpu.a[7];
    f3_exception(&cpu,5,0x102);
    require(cpu.cycles-exception_cycles==38 && m->read16(cpu.a[7]+6)==0x2014 &&
            m->read32(cpu.a[7]+8)==0x100,"Divide-by-zero full charge and format-2 instruction PC");
    require(f3_fallback(&cpu) && cpu.pc==0x102 && cpu.a[7]==exception_sp && cpu.sr==0x2700,
            "Real RTE restores the format-2 resume PC and stack");
    require(m->boundary()==0 && cpu.dispatch_deadline>cpu.cycles,"Restored masked CPU refreshes its event deadline");
    m->write16(0x400320,0x46fc);m->write16(0x400322,0x2000);cpu.pc=0x400320;
    m->interpreter->run_main(1);
    require(cpu.sr==0x2000 && cpu.dispatch_deadline==0,"Interpreted SR lowering also invalidates the native deadline");
    const f3_block blocks[]={{0x100,native}};
    require(f3_register_blocks(&cpu,blocks,1)==1,"Valid block table");
    cpu.pc=0x100;cpu.sr=0x2700;require(f3_dispatch(&cpu) && cpu.d[0]==99,"Native dispatch executes matching block");
    cpu.pc=0x100;cpu.sr=0x2700;require(f3_fallback(&cpu) && cpu.d[0]==42 && cpu.pc==0x102,"Fallback executes exactly one real instruction");
    cpu.d[0]=0xdeadbeef;cpu.sr=0x2015;
    const auto reset_cycles=cpu.cycles;
    m->interpreter->reset_main();
    require(cpu.d[0]==0xdeadbeef && cpu.sr==0x2715 && cpu.pc==0x100,
            "Reset preserves canonical native D/CCR, not stale fallback context");
    require(cpu.cycles-reset_cycles==4,"Warm reset charges its four-cycle latency exactly once");
    m->allow_main_fallback=false;const auto fallback_count=m->fallback_instructions;
    bool rejected=false;
    try { f3_fallback(&cpu); } catch(const std::runtime_error &e) { rejected=std::string(e.what()).find("0x100")!=std::string::npos; }
    require(rejected && cpu.pc==0x100 && cpu.d[0]==0xdeadbeef && m->fallback_instructions==fallback_count,
            "Native-only fallback rejection reports PC without executing or counting an instruction");
    const f3_block bad[]={{0x100,native},{0x100,native}};
    require(!f3_register_blocks(&cpu,bad,2),"Duplicate PCs rejected");
    const f3_excluded_range excluded[]={{0x200,0x204,"synthetic data","fixture-owned words"}};
    require(f3_register_exclusions(&cpu,excluded,1),"Exclusion complement registers");
    const f3_block overlapping[]={{0x200,native}};
    require(!f3_register_blocks(&cpu,overlapping,1),"Native entries cannot bypass exclusions");
    m->allow_main_fallback=true;
    for(uint32_t pc : {0x200u,0x201u,0x202u,0x203u,0xff000200u,0xff000203u}) {
        cpu.pc=pc;cpu.halted=0;
        bool excluded_rejected=false;
        try { f3_fallback(&cpu); }
        catch(const std::runtime_error &e) {
            excluded_rejected=std::string(e.what()).find("Excluded main CPU")!=std::string::npos;
        }
        require(excluded_rejected && cpu.halted && m->fallback_instructions==fallback_count,
                "Even, odd and physical-alias excluded targets fail before enabled interpretation");
    }
    cpu.pc=0x204;cpu.halted=0;
    const auto before_end_fallback=m->fallback_instructions;
    require(f3_fallback(&cpu) && m->fallback_instructions==before_end_fallback+1,
            "Exclusive end remains executable");
    require(f3_read16(&cpu,0x200)==0,"Excluded instruction bytes remain readable as data");
    require(f3_register_exclusions(&cpu,nullptr,0),"Clearing immutable exclusion metadata");
    cpu.halted=0;cpu.pc=0x100;
    m->audio->set_reset(true);
    m->audio->write8(0x280019,0x66);
    m->audio->write8(0x260001,0x12);m->audio->write8(0x260003,0x34);
    m->audio->write8(0x260005,0x56);m->audio->write8(0x260141,0);
    m->audio->set_reset(false);m->audio->set_reset(true);
    m->audio->write8(0x260101,0);
    require(m->audio->read8(0x260001)==0x12 && m->audio->read8(0x280019)==0x66,
            "CPU-line reset preserves DSP registers and DUART configuration");
    m->audio->write32(0,0xabcdef12);
    cpu.sr=0x2700;cpu.cycles=watchdog_deadline-1;
    require(m->boundary()==0 && cpu.dispatch_deadline==watchdog_deadline,
            "Watchdog expiry can precede the next raster interrupt");
    cpu.stopped=1;
    require(m->boundary()!=0 && cpu.cycles==watchdog_deadline && cpu.dispatch_deadline==0,
            "STOP advances to watchdog expiry without skipping it");
    require(m->boundary()!=0 && m->audio->is_reset(),"Watchdog holds the sound CPU in reset");
    require(cpu.dispatch_deadline==0,"Watchdog reset invalidates the native deadline");
    require(m->audio->read8(0x280019)==0x0f,"Watchdog restores the DUART interrupt vector");
    require(m->audio->read16(0x600)==0xa55a,"Whole-board reset preserves sound work RAM");
    require(m->audio->read32(0)==0,"Whole-board reset reloads boot vectors from sound ROM");
    m->audio->write8(0x260101,0);
    require(m->audio->read8(0x260001)==0 && m->audio->read8(0x260003)==0 &&
            m->audio->read8(0x260005)==0,"Watchdog clears DSP general-purpose registers");
    std::cout<<"PASS memory/lanes, input/coin, EEPROM protocol, IRQ/stack, native dispatch and real interpreter\n";
    return 0;
} catch(const std::exception &e) { std::cerr<<"FAIL "<<e.what()<<'\n';return 1; }
