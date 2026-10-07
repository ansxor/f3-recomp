#include "../gpu/scene_layout.h"
// Host layout: runtime/renderer/gpu/scene_layout.h (shared with the C++ encoder). SDL uniforms are two tightly packed uvec4s.
layout(std140, set = UNIFORM_SET, binding = 0) uniform Parameters {
    uvec4 dimensions; // scale, border, width, height
    uvec4 controls;   // sprite count, pen mask, fallback, layer mask
} params;
// Region bases come from scene_layout.h; the raw cell/glyph/palette formats are
// documented there and mirrored by GameTiles::RowSampler / GameText::pixel.
const uint PF_CELLS = F3_SCENE_PF_CELLS;
const uint TEXT_CELLS = F3_SCENE_TEXT_CELLS;
const uint GLYPHS = F3_SCENE_GLYPHS;
const uint PALETTE = F3_SCENE_PALETTE;
const uint ROWS = F3_SCENE_ROWS;
const uint ROW_STRIDE = F3_SCENE_ROW_STRIDE;
const uint SPRITES = F3_SCENE_SPRITES;
// Layer numbering mirrors LayerId in runtime/renderer/game/scene.hpp: 0..3 PF0..PF3,
// 4..7 SP0..SP3, 8 text. Mask bit n selects layer n.
const uint LAYER_COUNT = 9u;
const uint LAYER_SP0 = 4u;
const uint LAYER_TEXT = 8u;
uint byte_pen(uint packed, uint offset) { return (packed >> ((offset & 3u) * 8u)) & 255u; }
int floor_divide(int n, int d) { return n >= 0 ? n / d : -1 - (-1 - n) / d; }
