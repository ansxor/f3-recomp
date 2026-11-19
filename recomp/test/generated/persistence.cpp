#include "debug/profile.hpp"
#include "f3rt/block_profile.h"
#include <stdexcept>
#include <filesystem>

// The persistence fixture supplies the ROM identity without linking ROM loading.
uint32_t f3rt::crc32(const uint8_t *data, size_t size) {
    if (size != 2 || data[0] != 0x4e || data[1] != 0x71)
        throw std::logic_error("unexpected persistence fixture ROM");
    return 0x082131deu;
}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    const auto root = std::filesystem::absolute(argv[1]);
    std::filesystem::create_directories(root / "moved");
    std::filesystem::current_path(root);
    f3rt::RomSet roms;
    roms.main = {0x4e, 0x71};
    roms.sound = roms.main;
    {
        f3rt::BlockProfileSession session(roms, "profiles/run.profile");
        f3_profile_main_counts[0] = 7;
        f3_profile_sound_counts[0] = 3;
        std::filesystem::current_path(root / "moved");
        session.flush();
    }
    {
        f3rt::BlockProfileSession session(roms, "../profiles/run.profile");
        f3_profile_main_counts[0] += 2;
        ++f3_profile_sound_counts[0];
        std::filesystem::current_path(root);
        session.flush();
    }
    return 0;
}
