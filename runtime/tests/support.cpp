#include "support.hpp"

namespace f3test {
f3rt::RomSet fixture() {
    f3rt::RomSet r;
    r.main.resize(0x200000);r.sprites.resize(0x400000);r.sprites_hi.resize(0x200000);
    r.tiles.resize(0x400000);r.tiles_hi.resize(0x200000);r.sound.resize(0x80000);r.samples.resize(0x1000000);
    r.main[1]=0x41;r.main[2]=0xff;r.main[3]=0xf0; // SSP 41fff0
    r.main[6]=1; // PC 100
    r.main[0x100]=0x70;r.main[0x101]=0x2a; // moveq #42,d0
    r.main[0x102]=0x60;r.main[0x103]=0xfe; // bra self
    r.samples[0x2468a]=0x45;r.samples[0x2468b]=0x67; // OTIS word 0x12345, above old truncated mask
    return r;
}
}
