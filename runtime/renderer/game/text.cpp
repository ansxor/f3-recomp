#include "renderer/game/text.hpp"
#include "f3rt/memory_map.hpp"
#include <cstring>

namespace f3rt {

void GameText::reset() {
    m_map.fill(0);
    m_glyph_ram.fill(0);
    m_pivot_ram.fill(0);
    m_scroll_y = 0;
    m_row_used = 0;
    touch();
}

void GameText::decode(const VideoRam &vram) {
    const bool map_changed = m_shadow_map.changed(&vram.graphics[OFFS_TEXTRAM_MAP]);
    const bool glyphs_changed = m_shadow_glyphs.changed(&vram.graphics[OFFS_TEXTRAM_GLYPHS]);
    if (map_changed) {
        m_row_used = 0;
        for (unsigned i = 0; i < m_map.size(); ++i) {
            m_map[i] = vram.u16(OFFS_TEXTRAM_MAP + i * 2);
            if (m_map[i] & 0xff) {
                m_row_used |= 1ULL << (i >> 6);
            }
        }
    }
    if (glyphs_changed)
        std::memcpy(m_glyph_ram.data(), &vram.graphics[OFFS_TEXTRAM_GLYPHS], m_glyph_ram.size());
    const bool pivot_changed = m_shadow_pivot.changed(&vram.graphics[OFFS_PIVOT]);
    if (pivot_changed)
        std::memcpy(m_pivot_ram.data(), &vram.graphics[OFFS_PIVOT], m_pivot_ram.size());
    const uint16_t scroll_y = vram.control_u16(CONTROL_TEXT_SCROLL_Y_WORD);
    if (map_changed || glyphs_changed || pivot_changed || scroll_y != m_scroll_y)
        m_version = fresh_content_version();
    m_scroll_y = scroll_y;
}

} // namespace f3rt
