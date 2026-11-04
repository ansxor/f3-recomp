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
class Bench {
public:
    static std::unique_ptr<Bench> load(const std::string &path);

    std::array<LocalInputWord, LOCAL_PLAYER_COUNT> begin_frame(Machine &machine);

private:
    explicit Bench(InputScript script) : m_script(std::move(script)) {}
    InputScript m_script;
};
} // namespace f3rt::n3ds
