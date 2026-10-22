#pragma once
#include "audio/hle/patch.hpp"
#include "audio/hle/sound_bank.hpp"
#include <array>
#include <cstdint>
#include <vector>

namespace f3rt::hle {

// What a note asks the program table for. `key` is the kernel key after the track transpose; `layer_mask` is the track's
// controller layer mask, zero when the program's own enable mask applies.
struct ProgramQuery {
    uint16_t program = 0xffff;
    uint8_t key = 0;
    uint8_t layer_mask = 0;
    bool notes_suppressed = false;
};

// The patches one note starts, in voice-allocation order. The pointers are into ProgramService's table and stay
// valid for the service's lifetime.
struct PatchList {
    std::array<const Patch *, Program::layer_count> patches{};
    uint8_t count = 0;
    const Patch *const *begin() const noexcept { return patches.data(); }
    const Patch *const *end() const noexcept { return patches.data() + count; }
};

// The only place that knows a program's layout: a layered program yields up to three layer patches in descending
// layer order, a direct program yields the one key region containing the key.
class ProgramService {
public:
    explicit ProgramService(const SoundBank &bank);

    PatchList resolve(const ProgramQuery &query) const;

private:
    const SoundBank &m_bank;
    // PatchList hands out pointers into these tables: reserve the exact size once and never grow them.
    std::vector<LayerPatch> m_layers;
    std::vector<DirectPatch> m_regions;
};

} // namespace f3rt::hle
