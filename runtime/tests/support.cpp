#include "support.hpp"

namespace {
constexpr size_t RESET_SP_VECTOR = 0;        // initial SSP longword (big-endian)
constexpr size_t RESET_PC_VECTOR = 4;        // initial PC longword (big-endian)
constexpr size_t TEST_PROGRAM_PC = 0x100;    // moveq #42,d0; bra self
constexpr size_t OTIS_SAMPLE_WORD = 0x2468a; // OTIS word 0x12345
} // namespace

namespace f3test {
f3rt::RomSet fixture() {
    f3rt::RomSet r;
    r.main.resize(0x200000);
    r.sprites.resize(0x400000);
    r.sprites_hi.resize(0x200000);
    r.tiles.resize(0x400000);
    r.tiles_hi.resize(0x200000);
    r.sound.resize(0x80000);
    r.samples.resize(0x1000000);
    r.main[RESET_SP_VECTOR + 1] = 0x41;
    r.main[RESET_SP_VECTOR + 2] = 0xff;
    r.main[RESET_SP_VECTOR + 3] = 0xf0; // SSP 41fff0
    r.main[RESET_PC_VECTOR + 2] = 1;    // PC 100
    r.main[TEST_PROGRAM_PC] = 0x70;
    r.main[TEST_PROGRAM_PC + 1] = 0x2a; // moveq #42,d0
    r.main[TEST_PROGRAM_PC + 2] = 0x60;
    r.main[TEST_PROGRAM_PC + 3] = 0xfe; // bra self
    r.samples[OTIS_SAMPLE_WORD] = 0x45;
    r.samples[OTIS_SAMPLE_WORD + 1] = 0x67; // OTIS word 0x12345, above old truncated mask
    return r;
}
} // namespace f3test
