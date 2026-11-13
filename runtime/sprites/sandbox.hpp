#pragma once
#include "f3rt/cpu_abi.h"
#include <array>
#include <cstdint>
#include <vector>

namespace f3rt {
class Machine;

class Sandbox {
public:
    enum class Abort : uint8_t {
        Device,
        ForbiddenWrite,
        Untranslated,
        Exception,
        ResetDevices,
        Budget,
        Stopped,
        NoNative,
        Count
    };
    static const char *abort_name(Abort reason);

    struct Entry {
        uint16_t key = 0;
        std::array<uint8_t, 16> bytes{};
    };
    struct Result {
        bool ok = false;
        Abort reason = Abort::Count;
        std::vector<Entry> written;
        bool bank = false;
        uint16_t first = 0, last = 0;
        std::vector<uint8_t> dense;
        void clear();
    };

    explicit Sandbox(Machine &machine);
    Sandbox(const Sandbox &) = delete;
    Sandbox &operator=(const Sandbox &) = delete;

    bool owns(const f3_cpu *cpu) const { return cpu == &m_cpu; }

    uint32_t read(uint32_t address, unsigned width);
    void write(uint32_t address, uint32_t value, unsigned width);
    void abort(Abort reason);
    int unit_exit(uint32_t unit);

    void begin(uint32_t unit, uint32_t start_pc, uint32_t end_pc);
    Abort run();
    void collect(Result &out) const;

private:
    uint8_t read8(uint32_t address);
    void write8(uint32_t address, uint8_t value);
    uint8_t ram_get(uint32_t offset) const;
    void ram_put(uint32_t offset, uint8_t value);
    const f3_block *find_block(uint32_t pc) const;

    Machine &m_machine;
    f3_cpu m_cpu{};
    static constexpr uint32_t OVERLAY_PAGE = 256;
    std::array<int16_t, 0x20000 / OVERLAY_PAGE> m_page_slot{};
    std::vector<std::array<uint8_t, OVERLAY_PAGE>> m_page_pool;
    std::vector<uint16_t> m_touched_pages;
    std::array<int16_t, 0x800> m_capture_slot{};
    std::vector<Entry> m_capture;
    uint32_t m_unit = 0, m_end_pc = 0;
    bool m_done = false;
    Abort m_abort = Abort::Count;
};

}
