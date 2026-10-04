// Diagnostic-only callbacks injected into private copies of generated C.
#include <array>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {
std::array<uint8_t, 0x200000> main_access{};
std::array<uint8_t, 0x80000> sound_access{};
constexpr uint8_t fetched = 1, data = 2, video_data = 4;

void record(unsigned cpu, uint32_t address, unsigned width, uint8_t kind) {
    const uint32_t base = cpu ? 0xc00000u : 0u;
    const size_t size = cpu ? sound_access.size() : main_access.size();
    if (address < base || address - base >= size) return;
    const size_t offset = address - base;
    auto *bytes = cpu ? sound_access.data() : main_access.data();
    for (size_t i = offset; i < size && i - offset < width; ++i) bytes[i] |= kind;
}

void intervals(std::ostream &out, const uint8_t *bytes, size_t size,
               uint32_t base, uint8_t mask, bool known = false) {
    bool first = true;
    out << '[';
    for (size_t i = 0; i < size;) {
        if (!(bytes[i] & mask)) { ++i; continue; }
        const size_t start = i++;
        while (i < size && (bytes[i] & mask)) ++i;
        if (!first) out << ',';
        first = false;
        if (known) {
            out << "{\"start\":" << base + start << ",\"end\":" << base + i
                << ",\"reason\":\"Observed tile-descriptor data\",\"evidence\":"
                   "\"Native ROM reads by documented tile-copy/erase helpers "
                   "0x55c2/0x5614/0x56ae (docs/VIDEO-HLE.md); "
                   "sampled gameplay evidence, not a universal no-execution proof\"}";
        } else {
            out << '[' << base + start << ',' << base + i << ']';
        }
    }
    out << ']';
}

void section(std::ostream &out, const uint8_t *bytes, size_t size, uint32_t base) {
    out << "{\"fetched\":";
    intervals(out, bytes, size, base, fetched);
    out << ",\"data_reads\":";
    intervals(out, bytes, size, base, data);
    out << ",\"known_data\":";
    intervals(out, bytes, size, base, video_data, true);
    out << '}';
}
}

extern "C" void f3_profile_fetch(unsigned cpu, uint32_t pc, unsigned size) {
    record(cpu, pc, size, fetched);
}
extern "C" void f3_profile_data(unsigned cpu, uint32_t address, unsigned width, uint32_t pc) {
    // ROM-established descriptor helpers. Only their actual bus-read bytes gain
    // this label; bytes between descriptors do not inherit the evidence.
    const bool descriptor = !cpu && pc >= 0x55c2 && pc < 0x56e6;
    record(cpu, address, width, data | (descriptor ? video_data : 0));
}

int f3_gameplay_main(int argc, char **argv);
int main(int argc, char **argv) try {
    const char *output = nullptr, *seed = nullptr, *frames = nullptr;
    int argument_count = 1;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--profile-output") {
            if (++i == argc) throw std::runtime_error("Missing profile output");
            output = argv[i];
        } else {
            argv[argument_count++] = argv[i];
            if (arg == "--seed" && i + 1 < argc) seed = argv[i + 1];
            if (arg == "--frames" && i + 1 < argc) frames = argv[i + 1];
        }
    }
    if (!output || !*output || !seed || !*seed || !frames || !*frames)
        throw std::runtime_error("Profile output, seed and frames are required");
    argv[argument_count] = nullptr;
    const int result = f3_gameplay_main(argument_count, argv);
    if (result) return result;
    std::ofstream out(output);
    if (!out) throw std::runtime_error("Cannot write ROM-access profile");
    out << "{\"runs\":[{\"seed\":" << seed << ",\"frames\":" << frames
        << ",\"main\":\"native\",\"sound\":\"native\"}],\"main\":";
    section(out, main_access.data(), main_access.size(), 0);
    out << ",\"sound\":";
    section(out, sound_access.data(), sound_access.size(), 0xc00000);
    out << "}\n";
    if (!out) throw std::runtime_error("ROM-access profile write failed");
    return 0;
} catch (const std::exception &error) {
    std::cerr << "ROM profile: " << error.what() << '\n';
    return 1;
}
