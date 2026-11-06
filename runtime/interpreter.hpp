#pragma once
#include <cstdint>
#include <vector>
namespace f3rt {
class Machine;
class Interpreter {
public:
    explicit Interpreter(Machine &machine);
    int run_main(int cycles);
    void sync_main_from_cpu();

private:
    Machine &m_machine;
    std::vector<uint64_t> m_main_context;
};
} // namespace f3rt
