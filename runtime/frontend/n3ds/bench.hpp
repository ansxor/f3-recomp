#pragma once
#include "debug/input_script.hpp"
#include "f3rt/input.hpp"
#include <array>
#include <cstdint>
#include <memory>
#include <string>

namespace f3rt {
class Machine;
}

namespace f3rt::n3ds {
// Dev-only scripted run (CMake F3_N3DS_BENCH, built by `nix run .#3ds-bench`): drives the game
// through its scenarios by frame number so a perf log can be attributed per scenario. The script
// is a desktop `--inputs` file (runtime/debug/input_script.hpp) extended with `FRAME scenario
// NAME` markers; its frames are Machine::frame + 1, so a script explored on the desktop with
// `--fast-boot off --eeprom <init.bin> --inputs` plays the same on the 3DS after fast boot.
class Bench {
public:
    // Parses the script at `path`; a missing or malformed script is logged and yields nullptr.
    static std::unique_ptr<Bench> load(const std::string &path);

    // Applies the pokes of the frame the machine is about to run and logs its scenario marker
    // (`n3ds: scenario NAME frame=N`); returns the scripted input to OR into the live input.
    std::array<LocalInputWord, LOCAL_PLAYER_COUNT> begin_frame(Machine &machine);

private:
    explicit Bench(InputScript script) : m_script(std::move(script)) {}
    InputScript m_script;
};
} // namespace f3rt::n3ds
