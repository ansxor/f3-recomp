#include "state_io.hpp"
#include "third_party/musashi/m68k.h"
#include <iostream>
#include <limits>
#include <vector>

namespace {
unsigned accepted = 0;
template<class T> void reject(const T &value, const char *name) {
    f3rt::StateReader reader({reinterpret_cast<const uint8_t *>(&value), sizeof(value)});
    T restored{};
    try { reader.read(restored); }
    catch (const std::invalid_argument &) { return; }
    std::cerr << "ACCEPTED unsafe snapshot: " << name << '\n';
    ++accepted;
}
}
int main() try {
    // Use the actual Musashi producer, not a duplicated model of its encodings.
    m68k_init();
    m68k_set_cpu_type(M68K_CPU_TYPE_68000);
    std::vector<uint8_t> context(m68k_context_size());
    m68k_get_context(context.data());
    f3rt_sound_oracle_state sound{};
    f3rt_sound_core_export(context.data(), &sound);
    auto bad = sound;
    bad.s_flag = 0x40000000; bad.nmi_pending = 1;
    reject(bad, "sound supervisor stack index");
    bad = sound; bad.m_flag = 0x40000000;
    reject(bad, "sound master stack index");
    bad = sound; bad.ir = 0x10000;
    reject(bad, "sound opcode table index");
    bad = sound; bad.int_level = 0xffffffff;
    reject(bad, "sound interrupt level");
    bad = sound; bad.cpu_type = 0x80000000;
    reject(bad, "sound CPU model");
    bad = sound; bad.cyc_movem_w = 0xffffffff;
    reject(bad, "sound instruction timing");
    f3rt::CanonicalAudioCore audio{};
    audio.sample_accum = 16000000;
    reject(audio, "sample deadline outside fractional range");
    audio.sample_accum = uint64_t(1) << 63;
    reject(audio, "unbounded sample generation");
    audio.sample_accum = 0; audio.cpu_accum = std::numeric_limits<int64_t>::min();
    reject(audio, "sound deadline arithmetic underflow");
    audio.cpu_accum = std::numeric_limits<int64_t>::max();
    reject(audio, "sound deadline arithmetic overflow");
    f3rt::CanonicalES5505 otis{};
    otis.master_clock = 15238090; otis.active_voices = 31;
    reject(otis, "zero OTIS rate");
    otis.sample_rate = 1;
    reject(otis, "OTIS crystal/divider disagreement");
    if (accepted) return 1;
    std::cout << "PASS malformed sound contexts and audio scheduler snapshots rejected\n";
    return 0;
} catch (const std::exception &e) {
    std::cerr << "FAIL " << e.what() << '\n';
    return 1;
}
