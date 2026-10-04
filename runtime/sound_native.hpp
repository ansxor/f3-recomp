#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <f3rt/cpu_abi.h>

namespace f3rt {
class StateWriter;
class StateReader;

class Machine;

class SoundNative {
public:
    // Generated table: exact sorted complement of excluded even ROM addresses.
    SoundNative(Machine &machine, const f3_block *blocks, size_t block_count,
                std::span<const f3_excluded_range> excluded);
    ~SoundNative();

    SoundNative(const SoundNative &) = delete;
    SoundNative &operator=(const SoundNative &) = delete;
    SoundNative(SoundNative &&) = delete;
    SoundNative &operator=(SoundNative &&) = delete;

    int run(int cycles);
    void reset(bool asserted);
    uint32_t pc() const;
    uint64_t instruction_count() const { return m_instruction_count; }
    size_t state_size() const;
    void save_state(StateWriter &writer) const;
    void load_state(StateReader &reader);

    f3_cpu &cpu() { return m_cpu; }
    const f3_cpu &cpu() const { return m_cpu; }
    Machine &machine() { return m_machine; }

    uint8_t read8(uint32_t address);
    uint16_t read16(uint32_t address);
    uint32_t read32(uint32_t address);
    void write8(uint32_t address, uint8_t value);
    void write16(uint32_t address, uint16_t value);
    void write32(uint32_t address, uint32_t value);

    void trace_sound(uint32_t address, uint32_t value, uint8_t width, bool write);

    void exception(unsigned vector, uint32_t return_pc);
    void set_sr(uint16_t sr);
    void rte();
    void stop(uint16_t new_sr, uint32_t next_pc);

private:
    void do_reset();
    void check_interrupts();
    void dispatch_one();
    size_t block_index(uint32_t pc) const;

    Machine &m_machine;
    const f3_block *m_blocks;
    std::span<const f3_excluded_range> m_excluded;

    f3_cpu m_cpu{};
    bool m_needs_reset = true;
    int m_reset_cycles = 0;
    uint64_t m_instruction_count = 0;

    static constexpr uint32_t ROM_BASE = 0xc00000;
    static constexpr uint32_t ROM_SIZE = 0x80000;
};

} // namespace f3rt
