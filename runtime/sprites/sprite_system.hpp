#pragma once
#include "f3rt/sprite_group.hpp"
#include "sprites/behaviour.hpp"
#include "sprites/groups.hpp"
#include "sprites/presentation.hpp"
#include "sprites/replay.hpp"
#include "sprites/sandbox.hpp"
#include <array>
#include <cstdint>
#include <iosfwd>
#include <memory>
#include <vector>

namespace f3rt {
class Machine;

class SpriteSystem {
public:
    struct Options {
        std::vector<const SpriteBehaviour *> behaviours;
    };
    struct Report {
        std::vector<BehaviourReplay::UnitStats> units;
        uint64_t frames = 0;
        uint64_t sprite_writes = 0;
        uint64_t grouped_writes = 0;
        uint64_t unresolved_writes = 0;
        uint32_t first_unresolved_pc = 0;
        uint64_t generation_bumps = 0, periodic_resets = 0;
        uint64_t ambiguous_key_frames = 0, ambiguous_keys = 0, ambiguous_slots = 0;
        uint64_t max_ambiguous_keys_frame = 0, max_ambiguous_slots_frame = 0;
        using PcCount = SpriteGroupTracker::Report::PcCount;
        std::vector<PcCount> top_bump_pcs;
        uint64_t classes = 0;
        std::array<uint64_t, 256> class_writes{};
        uint64_t invocations = 0, replays = 0, completed = 0, aborted = 0, spliced = 0;
        uint64_t unmatched_exits = 0;
        uint64_t discarded = 0;
        uint64_t no_native_skips = 0;
    };

    SpriteSystem(Machine &machine, const SpriteGroupTable &table, const SpriteUnitTable &units,
                 Options options);
    SpriteSystem(const SpriteSystem &) = delete;
    SpriteSystem &operator=(const SpriteSystem &) = delete;

    static const SpriteGroupTable &game_table();
    static const SpriteUnitTable &game_units();

    bool active() const { return m_tracker.groups_active() || m_replay.active(); }
    bool groups_active() const { return m_tracker.groups_active(); }

    SpritePresentation presentation();

    bool is_sandbox(const f3_cpu *cpu) const { return m_sandbox.owns(cpu); }
    void unit_enter(f3_cpu *cpu, uint32_t unit) { m_replay.unit_enter(cpu, unit); }
    int unit_exit(f3_cpu *cpu, uint32_t unit) { return m_replay.unit_exit(cpu, unit); }
    void note_write(uint32_t address, unsigned width) { m_tracker.note_write(address, width); }
    void frame_end();
    void reset();
    uint32_t sandbox_read(uint32_t address, unsigned width) {
        return m_sandbox.read(address, width);
    }
    void sandbox_write(uint32_t address, uint32_t value, unsigned width) {
        m_sandbox.write(address, value, width);
    }
    void sandbox_abort(Sandbox::Abort reason) { m_sandbox.abort(reason); }

    Report report() const;
    void write_report(std::ostream &out) const;
    void write_summary(std::ostream &out) const;

private:
    SpriteGroupTracker m_tracker;
    Sandbox m_sandbox;
    BehaviourReplay m_replay;
    uint64_t m_frames = 0;
};

std::unique_ptr<SpriteSystem> make_game_sprites(Machine &machine);

}
