#pragma once
// Behaviour replay: emit units, their invocation stack and the splices they produce.
//
// Emit units declared by the game (generated sprite_units.hpp) survive only as behaviour
// anchors: at a unit start the game's own native code is replayed in a sandbox
// (runtime/sprites/sandbox.hpp) and the result is spliced over the real entries when a
// behaviour patched it. There is no invocation identity, IRQ attribution or per-entry id;
// groups come from the write table (runtime/sprites/groups.hpp).
//
// Render-only: a replay runs on a sandbox CPU and never touches emulated state.
#include "f3rt/emit_unit.hpp"
#include "sprites/behaviour.hpp"
#include "sprites/presentation.hpp"
#include "sprites/sandbox.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace f3rt {
class Machine;
class SpriteGroupTracker;

// Emit units declared by the game (generated sprite_units.hpp): behaviour anchors only.
struct SpriteUnitTable {
    std::span<const EmitUnit *const> units;
};

class BehaviourReplay final : public SpriteRamWriteListener {
public:
    struct UnitStats {
        std::string name;
        uint64_t invocations = 0;
        uint64_t replays = 0;   // sandbox replays started
        uint64_t completed = 0; // replays that reached the unit end
        uint64_t aborted = 0;
        uint64_t spliced = 0; // splices produced by behaviours
        std::array<uint64_t, size_t(Sandbox::Abort::Count)> aborts{};
    };
    struct Stats {
        std::vector<UnitStats> units;
        uint64_t invocations = 0, replays = 0, completed = 0, aborted = 0, spliced = 0;
        uint64_t unmatched_exits = 0; // exits without an open invocation
        uint64_t discarded = 0;       // invocations dropped (nesting/frame policy)
        uint64_t no_native_skips = 0; // replays skipped because no native blocks exist
    };

    BehaviourReplay(Machine &machine, Sandbox &sandbox, const SpriteGroupTracker &tracker,
                    const SpriteUnitTable &units,
                    std::vector<const SpriteBehaviour *> behaviours);
    BehaviourReplay(const BehaviourReplay &) = delete;
    BehaviourReplay &operator=(const BehaviourReplay &) = delete;

    bool active() const { return !m_units.empty(); }

    // ---- Hooks (cpu_abi.cpp, machine.cpp) ----
    void unit_enter(f3_cpu *cpu, uint32_t unit);
    int unit_exit(f3_cpu *cpu, uint32_t unit); // nonzero only for the sandbox at its unit end
    // Drops invocations that did not exit and splices that were not refreshed.
    void frame_end();
    // Drops all transient state (machine reset, state load); counters are kept.
    void reset();

    // ---- Presentation ----
    // Rebuilds the render view of the live splices (sorted by bank, first).
    void presentation();
    std::span<const SpriteSplice> splices() const { return m_view; }

    const Stats &stats() const { return m_stats; }

    // ---- SpriteRamWriteListener (every sprite-RAM store) ----
    // Records the slots an invocation's real span drew (the splice range).
    void sprite_group_write(const SpriteRamWrite &write) override;

private:
    struct Invocation {
        uint32_t unit = 0, start_index = 0, address = 0;
        uint64_t frame = 0;
        std::vector<uint16_t> keys; // sprite entries written by the real span, write order
        Sandbox::Result patched;
    };
    struct Splice {
        bool bank = false;
        uint16_t first = 0, last = 0;
        uint64_t frame = 0, group = 0;
        int32_t group_x = 0, group_y = 0;
        uint32_t group_pc = 0;
        std::vector<uint8_t> real, replacement;
    };

    static constexpr size_t MAX_OPEN = 8;
    static constexpr uint64_t SPLICE_FRAMES = 4; // frames a splice survives without refresh

    void enter_real(uint32_t unit);
    int exit_real(uint32_t unit);
    void open_invocation(const EmitUnit &unit, size_t start_index);
    void finish(Invocation &invocation);
    void run_replay(const EmitUnit &unit, const Invocation &invocation, Sandbox::Result &out);
    void record_abort(uint32_t unit, Sandbox::Abort reason);

    Machine &m_machine;
    Sandbox &m_sandbox;
    const SpriteGroupTracker &m_tracker;
    std::vector<const EmitUnit *> m_units;
    std::vector<std::vector<const SpriteBehaviour *>> m_behaviours; // per unit id

    std::vector<Invocation> m_stack, m_pool;
    std::vector<Splice> m_splices;
    std::vector<SpriteSplice> m_view;

    Stats m_stats;
};

} // namespace f3rt
