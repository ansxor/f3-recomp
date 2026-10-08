#pragma once
// Emit-unit replay (render-only; never serialized, never part of Machine state).
//
// Games declare "emit units": code spans that draw one object/queue record into sprite
// RAM (config.toml [[video.emit_units]], compiled into the generated sprite_units.hpp).
// Generated code (and the interpreter, at the same instruction boundaries) calls
// f3_unit_enter/f3_unit_exit at the declared PCs. SpriteUnits then
//   1. attributes every sprite-RAM entry the real CPU writes inside a unit span to that
//      invocation's identity (stable across tile/animation changes);
//   2. at unit start, optionally replays the game's own native code for the span on a
//      copy of the CPU with a copy-on-write RAM overlay (a "sandbox") to see what it
//      would draw, optionally after behaviours patched unit fields;
//   3. at unit exit, splices the replay output over the real entries in a render-only
//      presentation list.
// Emulated state, cycles and native_blocks are identical with the feature off or on.
//
// Policies (deterministic, documented here because they are not obvious):
//  * Invocations nest on a stack. An exit closes the topmost invocation of that unit whose
//    end PC matches; invocations above it were never closed and are discarded. An exit with
//    no matching invocation is counted and ignored. Re-entering the same unit/start while
//    its previous invocation is still open at the same IRQ depth discards the stale one.
//    Invocations older than one frame are discarded at frame end; the stack is capped.
//  * Interrupts: Machine reports every IRQ entry (note_irq). While an IRQ handler runs the
//    "IRQ depth" is higher than at span entry (depth shrinks once SR's interrupt mask drops
//    below the level of the entry). A sprite write is attributed to the top invocation only
//    when the depths are equal; otherwise it counts as outside any unit. Invocations whose
//    depth exceeds the current depth (handler returned without exit) are discarded.
//  * Writes outside any unit clear the slot's identity (and are recorded in check mode).
//  * Splices persist until a later invocation's real writes overlap their entry range in
//    the bank, or they have not been refreshed for a few frames.
#include "f3rt/cpu_abi.h"
#include "f3rt/emit_unit.hpp"
#include "renderer/sprite_behaviour.hpp"
#include "renderer/sprite_presentation.hpp"
#include <array>
#include <cstdint>
#include <iosfwd>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace f3rt {
class Machine;

struct SpriteUnitTable {
    std::span<const EmitUnit *const> units;
    std::span<const PcRange> frame_writers;
};

class SpriteUnits {
public:
    struct Options {
        bool check = false;                              // compare unpatched replay with real writes
        std::vector<const SpriteBehaviour *> behaviours; // enabled behaviours (patched replay + splice)
    };
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

    struct Mismatch {
        uint32_t unit = 0;
        uint32_t start_pc = 0, unit_address = 0;
        uint64_t frame = 0;
        std::string detail; // first differing entry: bank/entry, real vs replay bytes
    };
    struct UnitStats {
        std::string name;
        uint64_t invocations = 0;   // real entries into the unit
        uint64_t replays = 0;       // sandbox replays started (check and patched)
        uint64_t completed = 0;     // replays that reached the unit end
        uint64_t matched = 0;       // check mode: bit-exact against the real writes
        uint64_t mismatched = 0;    // check mode: differs from the real writes
        uint64_t aborted = 0;
        uint64_t spliced = 0;       // splices produced by behaviours
        uint64_t real_entries = 0;  // sprite entries written by the real span
        std::array<uint64_t, size_t(Abort::Count)> aborts{};
    };
    struct StrayWriter {
        uint32_t pc = 0;
        uint64_t count = 0;         // sprite-RAM byte writes outside any unit
        uint64_t first_frame = 0;
        uint32_t first_address = 0;
        bool accounted = false;     // pc lies in a declared frame_writers range
    };
    struct Report {
        std::vector<UnitStats> units;
        std::vector<Mismatch> mismatches;   // first few only
        std::vector<StrayWriter> stray;     // check mode, sorted by pc
        uint64_t invocations = 0, replays = 0, completed = 0, matched = 0, mismatched = 0;
        uint64_t aborted = 0, spliced = 0, frames = 0;
        uint64_t unmatched_exits = 0;       // exits without an open invocation
        uint64_t discarded = 0;             // invocations dropped (nesting/IRQ/frame policy)
        uint64_t no_native_skips = 0;       // replays skipped because no native blocks exist
        uint64_t stray_writes = 0, stray_unaccounted = 0;
    };

    SpriteUnits(Machine &machine, const SpriteUnitTable &table, Options options);
    ~SpriteUnits();
    SpriteUnits(const SpriteUnits &) = delete;
    SpriteUnits &operator=(const SpriteUnits &) = delete;

    // Units/frame writers declared by the game (generated sprite_units.hpp).
    static const SpriteUnitTable &game_table();

    // True when the game declares at least one unit (identity tracking is meaningful).
    bool active() const { return !units_.empty(); }
    const Options &options() const { return options_; }
    // Render-only view; valid until the next enter/exit/frame_end/reset.
    SpritePresentation presentation() const;

    // ---- Hooks (cpu_abi.cpp, machine.cpp) ----
    bool is_sandbox(const f3_cpu *cpu) const { return cpu == &sandbox_cpu_; }
    void unit_enter(f3_cpu *cpu, uint32_t unit);
    int unit_exit(f3_cpu *cpu, uint32_t unit); // nonzero only for the sandbox at its unit end
    // Interpreted execution: call before the instruction at `pc` runs (exit hooks, then enter).
    void interpreted_instruction(uint32_t pc);
    // Real-CPU bus writes (Machine::write*): RAM word writer PCs and sprite-RAM entries.
    void note_write(uint32_t address, unsigned width);
    bool tracks_writers() const { return !writer_pc_.empty(); }
    void note_irq(unsigned level);
    void frame_end();
    // Drops all transient state (machine reset, state load); counters are kept.
    void reset();
    // Sandbox bus (cpu_abi.cpp): reads/writes on behalf of the sandbox CPU.
    uint32_t sandbox_read(uint32_t address, unsigned width);
    void sandbox_write(uint32_t address, uint32_t value, unsigned width);
    void sandbox_abort(Abort reason);

    // ---- Reporting ----
    Report report() const;
    // Check mode: no mismatches and no unaccounted stray writes (and no aborts unless allowed).
    bool passed(bool allow_aborts = false) const;
    void write_report(std::ostream &out) const;
    void write_summary(std::ostream &out) const; // one line

private:
    struct Entry {
        uint16_t key;                // bank * 0x400 + entry
        std::array<uint8_t, 16> bytes;
    };
    struct ReplayResult {
        bool ok = false;
        Abort reason = Abort::Count; // Count: not run
        std::vector<Entry> written;  // sorted by key; every entry the replay wrote
        // Dense run of the first-written bank, first..last, gaps from unit-start hardware.
        bool bank = false;
        uint16_t first = 0, last = 0;
        std::vector<uint8_t> dense;
        void clear() { ok = false; reason = Abort::Count; written.clear(); dense.clear(); first = last = 0; bank = false; }
    };
    struct Invocation {
        uint32_t unit = 0, start_index = 0, start_pc = 0, address = 0, irq_depth = 0;
        uint64_t identity = 0, frame = 0;
        std::vector<uint16_t> keys; // sprite entries written by the real span, write order
        ReplayResult check, patched;
        bool has_patched = false;
    };
    struct Splice {
        bool bank = false;
        uint16_t first = 0, last = 0;
        uint64_t identity = 0, frame = 0;
        std::vector<uint8_t> real, replacement;
    };
    struct Hooks { std::vector<uint32_t> exits, enters; };
    struct Generation { uint64_t last_frame = 0; uint32_t generation = 0; };

    void enter_real(uint32_t unit);
    int exit_real(uint32_t unit);
    void open_invocation(const EmitUnit &unit, size_t start_index);
    void close_invocation(size_t index);
    void discard_from(size_t index);
    uint32_t irq_depth();
    void sprite_write(uint32_t address);
    void finish(Invocation &invocation);
    void compare(Invocation &invocation, const std::vector<uint16_t> &real_keys);
    void run_replay(const EmitUnit &unit, const Invocation &invocation, bool patched, ReplayResult &out);
    const f3_block *find_block(uint32_t pc) const;
    void collect(ReplayResult &out, bool dense);
    uint8_t ram_get(uint32_t offset) const;
    void ram_put(uint32_t offset, uint8_t value);
    uint8_t sandbox_read8(uint32_t address);
    void sandbox_write8(uint32_t address, uint8_t value);
    void record_abort(uint32_t unit, Abort reason);
    uint32_t unit_register(const EmitUnit &unit) const;

    Machine &machine_;
    Options options_;
    std::vector<const EmitUnit *> units_;
    std::span<const PcRange> frame_writers_;
    std::vector<std::vector<const SpriteBehaviour *>> behaviours_; // per unit id
    std::unordered_map<uint32_t, Hooks> hooks_;                    // by PC (interpreted execution)
    std::array<uint64_t, 0x800> identity_{};
    std::array<uint64_t, 0x800> object_{};                         // owning invocation identity per slot
    std::vector<uint32_t> writer_pc_;                              // per RAM word, only when needed
    std::unordered_map<uint64_t, Generation> generations_;
    std::unordered_map<uint64_t, uint32_t> ordinals_;              // (unit, writer pc) -> per-frame count
    std::vector<Invocation> stack_, pool_;
    std::vector<unsigned> irq_levels_;
    std::vector<Splice> splices_;
    mutable std::vector<SpriteSplice> view_;
    uint32_t last_hook_pc_ = 0;
    uint64_t last_hook_cycles_ = UINT64_MAX;

    // Sandbox state.
    f3_cpu sandbox_cpu_{};
    static constexpr uint32_t overlay_page = 256;
    std::array<int16_t, 0x20000 / overlay_page> page_slot_{};
    std::vector<std::array<uint8_t, overlay_page>> page_pool_;
    std::vector<uint16_t> touched_pages_;
    std::array<int16_t, 0x800> capture_slot_{};
    std::vector<Entry> capture_;                                   // first-touch order
    uint32_t sandbox_unit_ = 0, sandbox_end_pc_ = 0;
    bool sandbox_done_ = false;
    Abort sandbox_abort_ = Abort::Count;

    Report report_;
    std::unordered_map<uint32_t, StrayWriter> stray_;
};

} // namespace f3rt
