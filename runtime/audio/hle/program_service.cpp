#include "audio/hle/program_service.hpp"

namespace f3rt::hle {
using sound_5a7e9117::program_slot;

ProgramService::ProgramService(const SoundBank &bank) : m_bank(bank) {
    m_layers.reserve(1024 * size_t(Program::layer_count));
    m_regions.reserve(1024 * size_t(Program::region_count));
    for (unsigned slot = 0; slot < 1024; ++slot) {
        const uint8_t *base = bank.program_data(slot).base;
        const Program program{base};
        if (base && program.enabled()) {
            for (unsigned layer = 0; layer < Program::layer_count; ++layer) {
                const Layer view = program.layer(layer);
                m_layers.emplace_back(view, bank, base, uint8_t(layer), view.delay_ticks(),
                                      view.starts_on_release());
            }
            for (unsigned region = 0; region < Program::region_count; ++region)
                m_regions.emplace_back(DirectRegion{}, bank, base, uint8_t(region));
        } else {
            for (unsigned layer = 0; layer < Program::layer_count; ++layer)
                m_layers.emplace_back(Layer{}, bank, base, uint8_t(layer), 0, false);
            for (unsigned region = 0; region < Program::region_count; ++region) {
                const DirectRegion view = base ? program.region(region) : DirectRegion{};
                m_regions.emplace_back(view, bank, base, uint8_t(region));
            }
        }
    }
}

PatchList ProgramService::resolve(const ProgramQuery &query) const {
    PatchList list;
    if (query.program == 0xffff) return list;
    const unsigned slot = program_slot(query.program);
    const uint8_t *base = m_bank.program_data(slot).base;
    if (!base) return list;
    const Program program{base};
    if (!program.enabled()) {
        const int key = f3rt::s8(query.key);
        for (unsigned region = 0; region < Program::region_count; ++region) {
            const DirectPatch &patch = m_regions[slot * Program::region_count + region];
            if (!patch.contains_key(key)) continue;
            list.patches[0] = &patch;
            list.count = 1;
            return list;
        }
        return list;
    }
    if (query.notes_suppressed) return list;
    const unsigned enabled = query.layer_mask ? query.layer_mask : program.enabled();
    // Descending layer order keeps the voice allocation and the driver start order of the layered onset.
    for (unsigned layer = Program::layer_count; layer--;) {
        if (!(enabled & (1u << layer))) continue;
        list.patches[list.count++] = &m_layers[slot * Program::layer_count + layer];
    }
    return list;
}

} // namespace f3rt::hle
