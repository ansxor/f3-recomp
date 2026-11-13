#include "sprites/sprite_system.hpp"
#include "f3rt/machine.hpp"
#include <ostream>
#include <utility>

// The recompiler emits the group table next to the generated program (F3_GENERATED_DIR is on
// f3rt's include path when set). Older/pre-generated programs have no table: groups then stay
// unresolved and the renderer snaps, while emit-unit behaviour replay keeps working.
#if __has_include("sprite_groups.hpp")
#include "sprite_groups.hpp"
#else
namespace f3rt::sprite_groups {
inline constexpr SpriteGroupTable table{};
} // namespace f3rt::sprite_groups
#endif
#include "generated_config/sprite_units.hpp" // configured emit units (behaviour anchors)

namespace f3rt {

const SpriteGroupTable &SpriteSystem::game_table() { return sprite_groups::table; }

const SpriteUnitTable &SpriteSystem::game_units() {
    static const SpriteUnitTable TABLE{std::span<const EmitUnit *const>(sprite_units::all)};
    return TABLE;
}

std::unique_ptr<SpriteSystem> make_game_sprites(Machine &machine) {
    const auto &group_table = SpriteSystem::game_table();
    const auto &unit_table = SpriteSystem::game_units();
    if (group_table.writes.empty() && unit_table.units.empty()) return nullptr;
    SpriteSystem::Options options;
    const auto registered = registered_sprite_behaviours();
    options.behaviours.assign(registered.begin(), registered.end());
    return std::make_unique<SpriteSystem>(machine, group_table, unit_table, std::move(options));
}

SpriteSystem::SpriteSystem(Machine &machine, const SpriteGroupTable &table,
                           const SpriteUnitTable &units, Options options)
    : m_tracker(machine, table), m_sandbox(machine),
      m_replay(machine, m_sandbox, m_tracker, units, std::move(options.behaviours)) {
    m_tracker.add_listener(&m_replay);
}

// ---- Presentation ---------------------------------------------------------------------

SpritePresentation SpriteSystem::presentation() {
    m_tracker.count_ambiguity();
    m_replay.presentation();

    SpritePresentation out;
    if (m_tracker.groups_active()) out.groups = m_tracker.slots();
    out.splices = m_replay.splices();
    return out;
}

void SpriteSystem::frame_end() {
    ++m_frames;
    m_replay.frame_end();
}

void SpriteSystem::reset() {
    m_replay.reset();
    m_tracker.reset();
}

// ---- Reporting -------------------------------------------------------------------------

SpriteSystem::Report SpriteSystem::report() const {
    const SpriteGroupTracker::Report writes = m_tracker.report();
    const BehaviourReplay::Stats &replay = m_replay.stats();
    Report r;
    r.units = replay.units;
    r.frames = m_frames;
    r.sprite_writes = writes.sprite_writes;
    r.grouped_writes = writes.grouped_writes;
    r.unresolved_writes = writes.unresolved_writes;
    r.first_unresolved_pc = writes.first_unresolved_pc;
    r.generation_bumps = writes.generation_bumps;
    r.periodic_resets = writes.periodic_resets;
    r.ambiguous_key_frames = writes.ambiguous_key_frames;
    r.ambiguous_keys = writes.ambiguous_keys;
    r.ambiguous_slots = writes.ambiguous_slots;
    r.max_ambiguous_keys_frame = writes.max_ambiguous_keys_frame;
    r.max_ambiguous_slots_frame = writes.max_ambiguous_slots_frame;
    r.top_bump_pcs.reserve(writes.top_bump_pcs.size());
    for (const SpriteGroupTracker::Report::PcCount &pc : writes.top_bump_pcs)
        r.top_bump_pcs.push_back({pc.pc, pc.count});
    r.classes = writes.classes;
    r.class_writes = writes.class_writes;
    r.invocations = replay.invocations;
    r.replays = replay.replays;
    r.completed = replay.completed;
    r.aborted = replay.aborted;
    r.spliced = replay.spliced;
    r.unmatched_exits = replay.unmatched_exits;
    r.discarded = replay.discarded;
    r.no_native_skips = replay.no_native_skips;
    return r;
}

void SpriteSystem::write_summary(std::ostream &out) const {
    const SpriteGroupTracker::Report writes = m_tracker.report();
    const BehaviourReplay::Stats &replay = m_replay.stats();
    out << "sprite_groups: classes=" << writes.classes
        << " invocations=" << replay.invocations << " replays=" << replay.replays
        << " spliced=" << replay.spliced << " aborted=" << replay.aborted
        << " frames=" << m_frames << '\n';
}

void SpriteSystem::write_report(std::ostream &out) const {
    const SpriteGroupTracker::Report writes = m_tracker.report();
    const BehaviourReplay::Stats &replay = m_replay.stats();
    out << "sprite groups: write_rules=" << m_tracker.write_rules()
        << " reset_rules=" << m_tracker.reset_rules() << " classes=" << writes.classes
        << " frames=" << m_frames << " sprite_writes=" << writes.sprite_writes
        << " grouped_writes=" << writes.grouped_writes
        << " unresolved_writes=" << writes.unresolved_writes << " first_unresolved_pc=0x"
        << std::hex << writes.first_unresolved_pc << std::dec << '\n';
    out << "  ambiguity: key_frames=" << writes.ambiguous_key_frames
        << " keys=" << writes.ambiguous_keys << " slots=" << writes.ambiguous_slots
        << " max_keys_frame=" << writes.max_ambiguous_keys_frame
        << " max_slots_frame=" << writes.max_ambiguous_slots_frame << '\n';
    // The text report has never listed the top bump PCs (report() exposes them); keep the line
    // byte-for-byte.
    out << "  generations: bumps=" << writes.generation_bumps
        << " periodic_suppressed=" << writes.periodic_resets << " top_pcs:\n";
    out << "  class writes:";
    for (size_t cls = 1; cls < writes.class_writes.size(); ++cls)
        if (writes.class_writes[cls])
            out << " class" << cls << "=" << writes.class_writes[cls];
    out << '\n';
    out << "sprite groups replay: invocations=" << replay.invocations
        << " replays=" << replay.replays << " completed=" << replay.completed
        << " aborted=" << replay.aborted << " spliced=" << replay.spliced
        << " unmatched_exits=" << replay.unmatched_exits << " discarded=" << replay.discarded
        << " no_native_skips=" << replay.no_native_skips << '\n';
    for (const BehaviourReplay::UnitStats &unit : replay.units) {
        out << "  unit " << unit.name << ": invocations=" << unit.invocations
            << " replays=" << unit.replays << " completed=" << unit.completed
            << " aborted=" << unit.aborted << " spliced=" << unit.spliced << '\n';
        for (size_t i = 0; i < unit.aborts.size(); ++i)
            if (unit.aborts[i])
                out << "    abort " << Sandbox::abort_name(Sandbox::Abort(i)) << ": "
                    << unit.aborts[i] << '\n';
    }
}

} // namespace f3rt
