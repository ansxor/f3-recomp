// Land Maker (landmakrj) game-specific video code.
//
// - video_writer_known: the known store-PC lists per scene component, used by
//   `--discovery-log` to suppress documented writers.
//
// The sprite, tile, text and line-RAM decoders are shared and live in runtime/.
#include "debug/discovery.hpp"
#include "f3rt/emit_unit.hpp"
#include "f3rt/memory_map.hpp"

namespace f3rt {

namespace {



// Tiles covered write PCs (tile-block copy/erase loops and per-layer clear stores)
constexpr uint32_t LANDMAKR_PC_TILES_COPY_ERASE_55FC = 0x55fc;
constexpr uint32_t LANDMAKR_PC_TILES_COPY_ERASE_5646 = 0x5646;
constexpr uint32_t LANDMAKR_PC_TILES_COPY_ERASE_56D6 = 0x56d6;
constexpr uint32_t LANDMAKR_PC_TILES_CLEAR_PF0_5A2E = 0x5a2e;
constexpr uint32_t LANDMAKR_PC_TILES_CLEAR_PF1_5A6A = 0x5a6a;
constexpr uint32_t LANDMAKR_PC_TILES_CLEAR_PF2_5AA6 = 0x5aa6;
constexpr uint32_t LANDMAKR_PC_TILES_CLEAR_PF3_5B00 = 0x5b00;
constexpr uint32_t LANDMAKR_PC_TILES_STORE_9BD08 = 0x9bd08;
constexpr uint32_t LANDMAKR_PC_TILES_STORE_9BD0A = 0x9bd0a;
constexpr uint32_t LANDMAKR_PC_TILES_STORE_9BD0C = 0x9bd0c;
constexpr uint32_t LANDMAKR_PC_TILES_STORE_9BD0E = 0x9bd0e;
constexpr uint32_t LANDMAKR_PC_TILES_STORE_9EC66 = 0x9ec66;
constexpr uint32_t LANDMAKR_PC_TILES_STORE_9EC6A = 0x9ec6a;
constexpr uint32_t LANDMAKR_PC_TILES_STORE_9EC6E = 0x9ec6e;
constexpr uint32_t LANDMAKR_PC_TILES_STORE_9EC72 = 0x9ec72;

// Text covered write PCs
constexpr uint32_t LANDMAKR_PC_TEXT_STORE_570E = 0x570e;
constexpr uint32_t LANDMAKR_PC_TEXT_STORE_5712 = 0x5712;
constexpr uint32_t LANDMAKR_PC_TEXT_STORE_5756 = 0x5756;
constexpr uint32_t LANDMAKR_PC_TEXT_STORE_5758 = 0x5758;
constexpr uint32_t LANDMAKR_PC_TEXT_STORE_578E = 0x578e;
constexpr uint32_t LANDMAKR_PC_TEXT_STORE_57BC = 0x57bc;
constexpr uint32_t LANDMAKR_PC_TEXT_STORE_57BE = 0x57be;
constexpr uint32_t LANDMAKR_PC_TEXT_STORE_580C = 0x580c;
constexpr uint32_t LANDMAKR_PC_TEXT_STORE_580E = 0x580e;
constexpr uint32_t LANDMAKR_PC_TEXT_STORE_583A = 0x583a;
constexpr uint32_t LANDMAKR_PC_TEXT_STORE_5840 = 0x5840;
constexpr uint32_t LANDMAKR_PC_TEXT_STORE_5846 = 0x5846;
constexpr uint32_t LANDMAKR_PC_TEXT_STORE_58C2 = 0x58c2;
constexpr uint32_t LANDMAKR_PC_TEXT_STORE_59D0 = 0x59d0;
constexpr uint32_t LANDMAKR_PC_TEXT_STORE_5A08 = 0x5a08;
constexpr uint32_t LANDMAKR_PC_TEXT_STORE_5BF8 = 0x5bf8;
constexpr uint32_t LANDMAKR_PC_TEXT_STORE_5C20 = 0x5c20;
constexpr uint32_t LANDMAKR_PC_TEXT_STORE_5B8A = 0x5b8a;
constexpr uint32_t LANDMAKR_PC_TEXT_STORE_5B9A = 0x5b9a;
constexpr uint32_t LANDMAKR_PC_TEXT_STORE_5BB6 = 0x5bb6;
constexpr uint32_t LANDMAKR_PC_TEXT_STORE_5BC6 = 0x5bc6;
constexpr uint32_t LANDMAKR_PC_TEXT_STORE_5BD8 = 0x5bd8;
constexpr uint32_t LANDMAKR_PC_TEXT_STORE_8DE60 = 0x8de60;
constexpr uint32_t LANDMAKR_PC_TEXT_STORE_8E0B2 = 0x8e0b2;
constexpr uint32_t LANDMAKR_PC_TEXT_STORE_8E0C2 = 0x8e0c2;
constexpr uint32_t LANDMAKR_PC_TEXT_STORE_8E0E6 = 0x8e0e6;
constexpr uint32_t LANDMAKR_PC_TEXT_STORE_8E0F4 = 0x8e0f4;
constexpr uint32_t LANDMAKR_PC_TEXT_STORE_8E9D2 = 0x8e9d2;
constexpr uint32_t LANDMAKR_PC_TEXT_STORE_9B53A = 0x9b53a;
constexpr uint32_t LANDMAKR_PC_TEXT_STORE_9B562 = 0x9b562;
constexpr uint32_t LANDMAKR_PC_TEXT_STORE_9B564 = 0x9b564;
constexpr uint32_t LANDMAKR_PC_TEXT_STORE_9B586 = 0x9b586;
constexpr uint32_t LANDMAKR_PC_TEXT_STORE_A1176 = 0xa1176;
constexpr uint32_t LANDMAKR_PC_TEXT_STORE_A117C = 0xa117c;
constexpr uint32_t LANDMAKR_PC_TEXT_STORE_A1184 = 0xa1184;
constexpr uint32_t LANDMAKR_PC_TEXT_STORE_A118C = 0xa118c;
constexpr uint32_t LANDMAKR_PC_TEXT_STORE_A1194 = 0xa1194;
constexpr uint32_t LANDMAKR_PC_TEXT_STORE_A119C = 0xa119c;
constexpr uint32_t LANDMAKR_PC_TEXT_STORE_A11A4 = 0xa11a4;
constexpr uint32_t LANDMAKR_PC_TEXT_STORE_A11AC = 0xa11ac;

// Sprite covered write PC ranges
constexpr PcRange LANDMAKR_PC_SPRITES_INIT_CLEAR{0x41d0, 0x4380};
constexpr PcRange LANDMAKR_PC_SPRITES_SCROLL{0x43b0, 0x43de};                 // sprite_scroll
constexpr PcRange LANDMAKR_PC_SPRITES_COMMAND{0x43e0, 0x43fe};                // sprite_command
constexpr PcRange LANDMAKR_PC_SPRITES_LIST_TERMINATOR{0x4422, 0x447e};        // list terminator
constexpr PcRange LANDMAKR_PC_SPRITES_SINGLE_COMPILER{0x4688, 0x46be};        // single sprite compiler
constexpr PcRange LANDMAKR_PC_SPRITES_GRID_COMPILER{0x46c0, 0x480a};          // grid sprite compiler
constexpr PcRange LANDMAKR_PC_SPRITES_SCALED_GRID_COMPILER{0x480c, 0x4a36};   // scaled grid compiler
constexpr PcRange LANDMAKR_PC_SPRITES_OBJ_GRID{0xa8f38, 0xa8f82};             // obj_grid
constexpr PcRange LANDMAKR_PC_SPRITES_OBJ_3TILE{0xa8f84, 0xa8fce};            // obj_3tile
constexpr PcRange LANDMAKR_PC_SPRITES_OBJ_GRID_FLIP_X{0xa9036, 0xa9076};      // obj_grid flip_x
constexpr PcRange LANDMAKR_PC_SPRITES_OBJ_4TILE{0xa90f4, 0xa913a};            // obj_4tile
constexpr PcRange LANDMAKR_PC_SPRITES_OBJ_SCALED{0xa913c, 0xa93a2};           // obj_scaled

// Lines covered write PC ranges
constexpr PcRange LANDMAKR_PC_DISPLAY_REGISTER_UPLOADER{0x00136e, 0x00145e}; // Display register uploader (0x660000..0x66001e)
constexpr PcRange LANDMAKR_PC_PF0_CLEAR_ROWSCROLL{0x005a2e, 0x005a42};       // PF0 clear rowscroll (0x62a000)
constexpr PcRange LANDMAKR_PC_PF1_CLEAR_ROWSCROLL{0x005a6a, 0x005a7e};       // PF1 clear rowscroll (0x62a200)
constexpr PcRange LANDMAKR_PC_PF2_CLEAR_ROWSCROLL_ZOOM_COLSCROLL{0x005aa6, 0x005ad8}; // PF2 clear rowscroll, zoom, colscroll
constexpr PcRange LANDMAKR_PC_PF3_CLEAR_ROWSCROLL_ZOOM_COLSCROLL{0x005b00, 0x005b32}; // PF3 clear rowscroll, zoom, colscroll
constexpr PcRange LANDMAKR_PC_LINE_RAM_PROFILE_INIT{0x005d30, 0x005d6c};     // Line RAM profile init (0x624000..0x62b000, 0x620000)
constexpr PcRange LANDMAKR_PC_BOOT_CONTROL_REGISTERS{0x010044, 0x01007c};    // Boot control registers (0x660000..0x66001a)
constexpr PcRange LANDMAKR_PC_BOOT_LINE_RAM_CLEAR{0x0100ba, 0x0100c4};       // Boot line RAM clear
constexpr PcRange LANDMAKR_PC_ALPHA_BLEND_SAVE{0x08cfd2, 0x08cfda};          // Alpha blend save (0x626200)
constexpr PcRange LANDMAKR_PC_ALPHA_BLEND_RESTORE{0x08cff2, 0x08cff8};       // Alpha blend restore (0x626200)
constexpr PcRange LANDMAKR_PC_WATER_EFFECT_SELECT_INIT_1{0x0914b8, 0x0914c2}; // Water effect select init
constexpr PcRange LANDMAKR_PC_WATER_EFFECT_SELECT_INIT_2{0x09152e, 0x091538}; // Water effect select init
constexpr PcRange LANDMAKR_PC_WATER_EFFECT_MAIN_LINES{0x0915dc, 0x091610};   // Water effect main lines
constexpr PcRange LANDMAKR_PC_WATER_CLIP_WINDOW_ANIMATION{0x09187a, 0x091882}; // Water clip window animation
constexpr PcRange LANDMAKR_PC_SELECTION_TRANSITION_SPRITE_PRIORITIES{0x09218a, 0x09218a}; // Selection transition sprite priorities
constexpr PcRange LANDMAKR_PC_FULL_SCREEN_ALPHA_PROFILE{0x098dc8, 0x098dc8}; // Full-screen alpha profile (0x626200)
constexpr PcRange LANDMAKR_PC_ATTRACT_SPRITE_MODES_PRIORITIES{0x099b72, 0x099b74}; // Attract sprite modes and priorities
constexpr PcRange LANDMAKR_PC_ATTRACT_REVERSE_NORMAL_BLEND_TRANSITION{0x099fc2, 0x099fca}; // Attract reverse/normal blend transition
constexpr PcRange LANDMAKR_PC_ATTRACT_ALPHA_FADE_D2{0x09a25c, 0x09a25c};    // Attract alpha fade from task D2
constexpr PcRange LANDMAKR_PC_ATTRACT_PF1_PF3_BLEND_RESTORATION{0x09a2ac, 0x09a2b0}; // Attract PF1/PF3 blend restoration
constexpr PcRange LANDMAKR_PC_ATTRACT_ALPHA_FADE_D2_SECOND{0x09a300, 0x09a300}; // Second attract alpha fade from task D2
constexpr PcRange LANDMAKR_PC_ATTRACT_PF2_BLEND_PRIORITY{0x09a6f4, 0x09a6f4}; // Attract PF2 blend/priority (0x62b400)
constexpr PcRange LANDMAKR_PC_ATTRACT_SPRITE_PRIORITIES{0x09a8ec, 0x09a8ec}; // Attract sprite priorities (0x627600)
constexpr PcRange LANDMAKR_PC_ATTRACT_PF2_BLENDING_ON{0x09accc, 0x09accc};   // Attract PF2 blending on
constexpr PcRange LANDMAKR_PC_ATTRACT_PF2_BLENDING_OFF{0x09ad4c, 0x09ad4c};  // Attract PF2 blending off
constexpr PcRange LANDMAKR_PC_GAMEBOARD_PF2_PALETTE_ADD_GRADIENT{0x09d684, 0x09d6a0}; // Gameboard PF2 palette add gradient
constexpr PcRange LANDMAKR_PC_GAMEBOARD_PF2_ZOOM_ROWSCROLL{0x09d75e, 0x09d7aa}; // Gameboard PF2 zoom & rowscroll
constexpr PcRange LANDMAKR_PC_GAMEBOARD_PF2_COL_SCROLL{0x09d7d6, 0x09d812};  // Gameboard PF2 column scroll
constexpr PcRange LANDMAKR_PC_PF0_WAVY_ROWSCROLL{0x09ecde, 0x09ecee};        // PF0 wavy rowscroll (0x62a000)

// Land Maker's tile-block copy/erase loops and the per-layer clear stores.
bool tiles_covered_write(uint32_t pc) {
    switch (pc) {
    case LANDMAKR_PC_TILES_COPY_ERASE_55FC: case LANDMAKR_PC_TILES_COPY_ERASE_5646: case LANDMAKR_PC_TILES_COPY_ERASE_56D6:
    case LANDMAKR_PC_TILES_CLEAR_PF0_5A2E: case LANDMAKR_PC_TILES_CLEAR_PF1_5A6A: case LANDMAKR_PC_TILES_CLEAR_PF2_5AA6: case LANDMAKR_PC_TILES_CLEAR_PF3_5B00:
    case LANDMAKR_PC_TILES_STORE_9BD08: case LANDMAKR_PC_TILES_STORE_9BD0A: case LANDMAKR_PC_TILES_STORE_9BD0C: case LANDMAKR_PC_TILES_STORE_9BD0E:
    case LANDMAKR_PC_TILES_STORE_9EC66: case LANDMAKR_PC_TILES_STORE_9EC6A: case LANDMAKR_PC_TILES_STORE_9EC6E: case LANDMAKR_PC_TILES_STORE_9EC72:
        return true;
    default:
        return false;
    }
}

bool text_covered_write(uint32_t pc) {
    switch (pc) {
    case LANDMAKR_PC_TEXT_STORE_570E: case LANDMAKR_PC_TEXT_STORE_5712: case LANDMAKR_PC_TEXT_STORE_5756: case LANDMAKR_PC_TEXT_STORE_5758: case LANDMAKR_PC_TEXT_STORE_578E:
    case LANDMAKR_PC_TEXT_STORE_57BC: case LANDMAKR_PC_TEXT_STORE_57BE: case LANDMAKR_PC_TEXT_STORE_580C: case LANDMAKR_PC_TEXT_STORE_580E:
    case LANDMAKR_PC_TEXT_STORE_583A: case LANDMAKR_PC_TEXT_STORE_5840: case LANDMAKR_PC_TEXT_STORE_5846: case LANDMAKR_PC_TEXT_STORE_58C2:
    case LANDMAKR_PC_TEXT_STORE_59D0: case LANDMAKR_PC_TEXT_STORE_5A08: case LANDMAKR_PC_TEXT_STORE_5BF8: case LANDMAKR_PC_TEXT_STORE_5C20:
    case LANDMAKR_PC_TEXT_STORE_5B8A: case LANDMAKR_PC_TEXT_STORE_5B9A: case LANDMAKR_PC_TEXT_STORE_5BB6: case LANDMAKR_PC_TEXT_STORE_5BC6: case LANDMAKR_PC_TEXT_STORE_5BD8:
    case LANDMAKR_PC_TEXT_STORE_8DE60: case LANDMAKR_PC_TEXT_STORE_8E0B2: case LANDMAKR_PC_TEXT_STORE_8E0C2: case LANDMAKR_PC_TEXT_STORE_8E0E6: case LANDMAKR_PC_TEXT_STORE_8E0F4:
    case LANDMAKR_PC_TEXT_STORE_8E9D2: case LANDMAKR_PC_TEXT_STORE_9B53A: case LANDMAKR_PC_TEXT_STORE_9B562: case LANDMAKR_PC_TEXT_STORE_9B564: case LANDMAKR_PC_TEXT_STORE_9B586:
    case LANDMAKR_PC_TEXT_STORE_A1176: case LANDMAKR_PC_TEXT_STORE_A117C: case LANDMAKR_PC_TEXT_STORE_A1184: case LANDMAKR_PC_TEXT_STORE_A118C:
    case LANDMAKR_PC_TEXT_STORE_A1194: case LANDMAKR_PC_TEXT_STORE_A119C: case LANDMAKR_PC_TEXT_STORE_A11A4: case LANDMAKR_PC_TEXT_STORE_A11AC:
        return true;
    default:
        return false;
    }
}

// Store PCs Land Maker uses to build the sprite display list. Ranges mirror the
// retired hook coverage: init_sprites, clear_spriteram, sprite_scroll,
// sprite_command, list terminator and the single/grid/scaled/object compilers.
bool sprites_covered_write(uint32_t pc) {
    static constexpr PcRange ranges[] = {
        LANDMAKR_PC_SPRITES_INIT_CLEAR,
        LANDMAKR_PC_SPRITES_SCROLL,
        LANDMAKR_PC_SPRITES_COMMAND,
        LANDMAKR_PC_SPRITES_LIST_TERMINATOR,
        LANDMAKR_PC_SPRITES_SINGLE_COMPILER,
        LANDMAKR_PC_SPRITES_GRID_COMPILER,
        LANDMAKR_PC_SPRITES_SCALED_GRID_COMPILER,
        LANDMAKR_PC_SPRITES_OBJ_GRID,
        LANDMAKR_PC_SPRITES_OBJ_3TILE,
        LANDMAKR_PC_SPRITES_OBJ_GRID_FLIP_X,
        LANDMAKR_PC_SPRITES_OBJ_4TILE,
        LANDMAKR_PC_SPRITES_OBJ_SCALED,
    };
    for (const auto &r : ranges)
        if (contains(r, pc)) return true;
    return false;
}

bool lines_covered_write(uint32_t pc) {
    static constexpr PcRange ranges[] = {
        LANDMAKR_PC_DISPLAY_REGISTER_UPLOADER,
        LANDMAKR_PC_PF0_CLEAR_ROWSCROLL,
        LANDMAKR_PC_PF1_CLEAR_ROWSCROLL,
        LANDMAKR_PC_PF2_CLEAR_ROWSCROLL_ZOOM_COLSCROLL,
        LANDMAKR_PC_PF3_CLEAR_ROWSCROLL_ZOOM_COLSCROLL,
        LANDMAKR_PC_LINE_RAM_PROFILE_INIT,
        LANDMAKR_PC_BOOT_CONTROL_REGISTERS,
        LANDMAKR_PC_BOOT_LINE_RAM_CLEAR,
        LANDMAKR_PC_ALPHA_BLEND_SAVE,
        LANDMAKR_PC_ALPHA_BLEND_RESTORE,
        LANDMAKR_PC_WATER_EFFECT_SELECT_INIT_1,
        LANDMAKR_PC_WATER_EFFECT_SELECT_INIT_2,
        LANDMAKR_PC_WATER_EFFECT_MAIN_LINES,
        LANDMAKR_PC_WATER_CLIP_WINDOW_ANIMATION,
        LANDMAKR_PC_SELECTION_TRANSITION_SPRITE_PRIORITIES,
        LANDMAKR_PC_FULL_SCREEN_ALPHA_PROFILE,
        LANDMAKR_PC_ATTRACT_SPRITE_MODES_PRIORITIES,
        LANDMAKR_PC_ATTRACT_REVERSE_NORMAL_BLEND_TRANSITION,
        LANDMAKR_PC_ATTRACT_ALPHA_FADE_D2,
        LANDMAKR_PC_ATTRACT_PF1_PF3_BLEND_RESTORATION,
        LANDMAKR_PC_ATTRACT_ALPHA_FADE_D2_SECOND,
        LANDMAKR_PC_ATTRACT_PF2_BLEND_PRIORITY,
        LANDMAKR_PC_ATTRACT_SPRITE_PRIORITIES,
        LANDMAKR_PC_ATTRACT_PF2_BLENDING_ON,
        LANDMAKR_PC_ATTRACT_PF2_BLENDING_OFF,
        LANDMAKR_PC_GAMEBOARD_PF2_PALETTE_ADD_GRADIENT,
        LANDMAKR_PC_GAMEBOARD_PF2_ZOOM_ROWSCROLL,
        LANDMAKR_PC_GAMEBOARD_PF2_COL_SCROLL,
        LANDMAKR_PC_PF0_WAVY_ROWSCROLL,
    };
    for (const auto &r : ranges)
        if (contains(r, pc)) return true;
    return false;
}

} // namespace

bool video_writer_known(VideoLayer layer, uint32_t pc) {
    switch (layer) {
    case VideoLayer::Sprites: return sprites_covered_write(pc);
    case VideoLayer::Pf0: case VideoLayer::Pf1: case VideoLayer::Pf2: case VideoLayer::Pf3:
        return tiles_covered_write(pc);
    case VideoLayer::Text: return text_covered_write(pc);
    case VideoLayer::Lines: case VideoLayer::Control: return lines_covered_write(pc);
    default: return false; // alternate map banks were not observed before; no list
    }
}

} // namespace f3rt
