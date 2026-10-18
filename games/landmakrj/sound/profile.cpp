// Land Maker (Japan) main-CPU sound request path, for the sound-extract and music-export tools.
//
// The SOUND TEST SOUND NO item calls the request routine with the selector code directly: there
// is no category or prelude step (effect preset/parameters are type 3 selectors sent at boot).
#include "sound/profile.hpp"

namespace f3rt::runner {
namespace {

// Highest code the request routine accepts; sound-extract returns no packets for 0x60 and above.
constexpr uint32_t LAST_SOUND_CODE = 0x5f;
constexpr uint32_t SOUND_REQUEST_PC = 0x00308c;
constexpr uint32_t SELECTOR_TABLE_BASE_PTR = 0x40014c;

constexpr SoundProfile PROFILE{"landmakrj",
                               "Land Maker (Japan)",
                               LAST_SOUND_CODE,
                               SOUND_REQUEST_PC,
                               0,
                               0,
                               SELECTOR_TABLE_BASE_PTR,
                               {}};

const SoundProfileRegistrar REGISTRAR{PROFILE};

} // namespace
} // namespace f3rt::runner
