#pragma once
// Sprite system: the render-only facade over group write tracking (sprites/groups.hpp) and
// behaviour replay (sprites/replay.hpp).
//
// Machine owns one SpriteSystem when the game's generated table declares group writers or
// emit units. It forwards the CPU hooks, assembles the per-frame SpritePresentation, and
// concatenates the per-component statistics for the report commands. Nothing here is
// serialized; enabling it must never change Machine::state_crc().
#include "f3rt/sprite_group.hpp"
#include "sprites/behaviour.hpp"
#include "sprites/groups.hpp"
#include "sprites/presentation.hpp"
#include "sprites/replay.hpp"
#include "sprites/sandbox.hpp"
#include <array>
#include <cstdint>
#include <iosfwd>
#include <vector>

namespace f3rt {
class Machine;

class SpriteSystem {
public:
    struct Options {
        std::vector<const SpriteBehaviour *> behaviours; // enabled behaviours (patched replay)
    };
    struct Report {
        std::vector<BehaviourReplay::UnitStats> units;
        uint64_t frames = 0;
        uint64_t sprite_writes = 0;      // sprite-RAM store instructions seen
        uint64_t grouped_writes = 0;     // ... that resolved a group
        uint64_t unresolved_writes = 0;  // ... whose PC/base/roots did not
        uint32_t first_unresolved_pc = 0;
        uint64_t generation_bumps = 0, periodic_resets = 0;
        // Ambiguity (render-only): one key with several origins in a frame cannot be paired;
        // the renderer snaps it. commandw's multi-part emit site produces this.
        uint64_t ambiguous_key_frames = 0, ambiguous_keys = 0, ambiguous_slots = 0;
        uint64_t max_ambiguous_keys_frame = 0, max_ambiguous_slots_frame = 0;
        struct PcCount {
            uint32_t pc = 0;
            uint64_t count = 0;
        };
        std::vector<PcCount> top_bump_pcs; // by bump count, descending, at most 8
        uint64_t classes = 0; // write-table classes in use
        std::array<uint64_t, 256> class_writes{}; // resolved sprite writes per class
        uint64_t invocations = 0, replays = 0, completed = 0, aborted = 0, spliced = 0;
        uint64_t unmatched_exits = 0; // exits without an open invocation
        uint64_t discarded = 0;       // invocations dropped (nesting/frame policy)
        uint64_t no_native_skips = 0; // replays skipped because no native blocks exist
    };

    SpriteSystem(Machine &machine, const SpriteGroupTable &table, const SpriteUnitTable &units,
                 Options options);
    SpriteSystem(const SpriteSystem &) = delete;
    SpriteSystem &operator=(const SpriteSystem &) = delete;

    // Tables of the game this executable was built for (empty when it declares none).
    static const SpriteGroupTable &game_table();
    static const SpriteUnitTable &game_units();

    // True when the presentation can differ from the plain decode (groups or splices).
    bool active() const { return m_tracker.groups_active() || m_replay.active(); }
    bool groups_active() const { return m_tracker.groups_active(); }

    // Render-only view for the next decode.
    SpritePresentation presentation();

    // ---- Hooks (cpu_abi.cpp, machine.cpp) ----
    bool is_sandbox(const f3_cpu *cpu) const { return m_sandbox.owns(cpu); }
    void unit_enter(f3_cpu *cpu, uint32_t unit) { m_replay.unit_enter(cpu, unit); }
    int unit_exit(f3_cpu *cpu, uint32_t unit) { return m_replay.unit_exit(cpu, unit); }
    // Interpreted execution: call before the instruction at `pc` runs (exit hooks, then enter).
    void interpreted_instruction(uint32_t pc) { m_replay.interpreted_instruction(pc); }
    // Real-CPU bus writes (Machine::write*).
    void note_write(uint32_t address, unsigned width) { m_tracker.note_write(address, width); }
    void frame_end();
    // Drops all transient state (machine reset, state load); counters are kept.
    void reset();
    // Sandbox bus (cpu_abi.cpp): reads/writes on behalf of the sandbox CPU.
    uint32_t sandbox_read(uint32_t address, unsigned width) {
        return m_sandbox.read(address, width);
    }
    void sandbox_write(uint32_t address, uint32_t value, unsigned width) {
        m_sandbox.write(address, value, width);
    }
    void sandbox_abort(Sandbox::Abort reason) { m_sandbox.abort(reason); }

    // ---- Reporting ----
    Report report() const;
    void write_report(std::ostream &out) const;
    void write_summary(std::ostream &out) const; // one line

private:
    SpriteGroupTracker m_tracker;
    Sandbox m_sandbox;
    BehaviourReplay m_replay;
    uint64_t m_frames = 0;
};

} // namespace f3rt
