#pragma once
// Sandbox bus and CPU used by behaviour replay (runtime/sprites/replay.hpp).
//
// A replay never executes on the real f3_cpu: the sandbox clones it, runs the game's own
// native blocks on the clone over a copy-on-write overlay of work RAM and a captured copy of
// sprite RAM, and drops everything afterwards. Device, ROM, palette and non-sprite graphics
// accesses abort the run instead of reaching the machine, so a replay can never change
// emulated state. Nothing here is serialized and nothing is reachable outside a replay.
#include "f3rt/cpu_abi.h"
#include <array>
#include <cstdint>
#include <vector>

namespace f3rt {
class Machine;

class Sandbox {
public:
    // Why a sandbox replay was abandoned (real entries are then kept).
    enum class Abort : uint8_t {
        Device,         // I/O, control, shared RAM, sound, EEPROM, unmapped bus access
        ForbiddenWrite, // ROM, palette or non-sprite graphics write
        Untranslated,   // PC without a native block (fallback / excluded code)
        Exception,      // f3_exception raised
        ResetDevices,   // f3_reset_devices
        Budget,         // cycle or block budget exceeded
        Stopped,        // STOP, halt, trace bits or boundary request
        NoNative,       // no native blocks registered (interpreted execution)
        Count
    };
    static const char *abort_name(Abort reason);

    // One captured sprite-RAM entry: the raw 16 bytes as the replay left them.
    struct Entry {
        uint16_t key = 0;
        std::array<uint8_t, 16> bytes{};
    };
    // What a completed replay wrote: the captured entries sorted by key, plus the dense
    // byte image of the first bank it touched (replacement for a splice range).
    struct Result {
        bool ok = false;
        Abort reason = Abort::Count; // Count: not run
        std::vector<Entry> written;  // sorted by key
        bool bank = false;
        uint16_t first = 0, last = 0;
        std::vector<uint8_t> dense;
        void clear();
    };

    explicit Sandbox(Machine &machine);
    Sandbox(const Sandbox &) = delete;
    Sandbox &operator=(const Sandbox &) = delete;

    // True for the sandbox clone only (never the real CPU or another device CPU).
    bool owns(const f3_cpu *cpu) const { return cpu == &m_cpu; }

    // Sandbox bus (cpu_abi.cpp): reads/writes on behalf of the sandbox CPU.
    uint32_t read(uint32_t address, unsigned width);
    void write(uint32_t address, uint32_t value, unsigned width);
    void abort(Abort reason);
    // Unit-end hook for a sandbox CPU: 1 when this replay reached its unit end.
    int unit_exit(uint32_t unit);

    // Prepares a run of one emit-unit span: drops the previous overlay and capture, clones the
    // flushed real CPU and positions the clone at the unit start.
    void begin(uint32_t unit, uint32_t start_pc, uint32_t end_pc);
    // Runs native blocks until the unit end, an abort or a budget. Returns the abort reason
    // (Abort::Count when the unit end was reached).
    Abort run();
    // Captures the run's result (only meaningful after run() returned Abort::Count).
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
    std::vector<Entry> m_capture; // first-touch order
    uint32_t m_unit = 0, m_end_pc = 0;
    bool m_done = false;
    Abort m_abort = Abort::Count;
};

} // namespace f3rt
