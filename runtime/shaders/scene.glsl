// Host layout: runtime/gpu_scene.hpp. SDL uniforms are two tightly packed uvec4s.
layout(std140, set = UNIFORM_SET, binding = 0) uniform Parameters {
    uvec4 dimensions; // scale, border, width, height
    uvec4 controls;   // sprite count, pen mask, fallback, layer mask
} params;
// PF_CELLS: 4 layers * 2048 raw cells, two words per slot. Word 0 is the raw
// big-endian video-RAM cell (attributes<<16 | tile code); word 1 is unused.
// attributes: bits 0-8 palette code (base = code*16), bit 9 blend selector,
// bits 10-11 extra pen planes, bit 14 flip X, bit 15 flip Y; the tile code's
// low 15 bits index a 256-byte tile. Mirrored by GameTiles::RowSampler.
const uint PF_CELLS = 0u;
// TEXT_CELLS: 4096 raw big-endian text-map words (bits 0-7 glyph, bit 8 flip X,
// bits 9-14 palette code, bit 15 flip Y), mirrored by GameText::pixel.
// GLYPHS: 4096-word region; the raw 0x2000 glyph-RAM bytes are byte-packed
// four per word, so only the first 2048 words are used. Pixel (x, y) of a glyph
// is nibble (x & 1) of byte y*4 + (3 - x/2).
const uint TEXT_CELLS = 16384u;
const uint GLYPHS = 20480u;
const uint PALETTE = 24576u;
const uint ROWS = 32768u;
const uint ROW_STRIDE = 352u;
const uint SPRITES = 122880u;
uint byte_pen(uint packed, uint offset) { return (packed >> ((offset & 3u) * 8u)) & 255u; }
int floor_divide(int n, int d) { return n >= 0 ? n / d : -1 - (-1 - n) / d; }
