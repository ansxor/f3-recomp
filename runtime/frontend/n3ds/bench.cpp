#include "bench.hpp"
#include "f3rt/machine.hpp"
#include "log.hpp"
#include <exception>
#include <utility>

namespace f3rt::n3ds {
std::unique_ptr<Bench> Bench::load(const std::string &path) {
    try {
        InputScript script = InputScript::load(path);
        log_line("n3ds: bench script %s rules=%zu", path.c_str(), script.rules());
        return std::unique_ptr<Bench>(new Bench(std::move(script)));
    } catch (const std::exception &error) {
        log_line("n3ds: bench script rejected: %s", error.what());
        return nullptr;
    }
}
std::array<LocalInputWord, LOCAL_PLAYER_COUNT> Bench::begin_frame(Machine &machine) {
    const uint64_t frame = machine.frame + 1;
    if (const std::string *scenario = m_script.scenario_at(frame))
        log_line("n3ds: scenario %s frame=%llu", scenario->c_str(),
                 static_cast<unsigned long long>(frame));
    m_script.poke(machine, frame);
    return m_script.words(frame);
}
} // namespace f3rt::n3ds
