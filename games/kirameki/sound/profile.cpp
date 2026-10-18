// Kirameki Star Road main-CPU sound request path, for the sound-extract and music-export tools.
//
// SOUND TEST handler 0x0035aa plays a code at 0x003640..0x003690: for categories <= 1 it first
// requests the two setup codes of the prelude (effect preset and parameters), then the code.
#include "sound/profile.hpp"

namespace f3rt::runner {
namespace {

// Highest code the request routine accepts; sound-extract returns no packets for 0x552 and above.
constexpr uint32_t LAST_SOUND_CODE = 0x551;
constexpr uint32_t SOUND_REQUEST_PC = 0x0018e0;
constexpr uint32_t SOUND_CATEGORY_PC = 0x00394c;
constexpr uint32_t SOUND_PRELUDE_PC = 0x02e8c0;
constexpr uint32_t SELECTOR_TABLE_BASE_PTR = 0x407cc8;
constexpr uint32_t SOUND_BANK_WORD_A = 0x416114;
constexpr uint32_t SOUND_BANK_WORD_B = 0x416112;

constexpr SoundProfile PROFILE{"kirameki",
                               "Kirameki Star Road (Ver 2.10J 1997/08/29)",
                               LAST_SOUND_CODE,
                               SOUND_REQUEST_PC,
                               SOUND_CATEGORY_PC,
                               SOUND_PRELUDE_PC,
                               SELECTOR_TABLE_BASE_PTR,
                               {SOUND_BANK_WORD_A, SOUND_BANK_WORD_B}};

const SoundProfileRegistrar REGISTRAR{PROFILE};

} // namespace
} // namespace f3rt::runner
