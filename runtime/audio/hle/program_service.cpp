#include "audio/hle/program_service.hpp"

namespace f3rt::hle {
using sound_5a7e9117::program_slot;

ProgramService::ProgramService(const SoundBank &bank) : m_bank(bank) {
    for (unsigned slot = 0; slot < 1024; ++slot) {
        const uint8_t *base = bank.program_data(slot).base;
        if (!base) continue;
        const Program program{base};
        if (program.enabled()) {
            for (unsigned layer = 0; layer < Program::layer_count; ++layer) {
                const Layer view = program.layer(layer);
                m_layers[slot * Program::layer_count + layer] =
                    Patch{view.bytes, base, uint8_t(layer), false, view.delay_ticks(),
                          view.starts_on_release()};
            }
        } else {
            for (unsigned region = 0; region < Program::region_count; ++region)
                m_regions[slot * Program::region_count + region] =
                    Patch{program.region(region).bytes, base, uint8_t(region), true};
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
            const Patch &patch = m_regions[slot * Program::region_count + region];
            if (!patch.direct().contains(key)) continue;
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
