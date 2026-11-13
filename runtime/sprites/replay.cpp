#include "sprites/replay.hpp"
#include "sprites/groups.hpp"
#include "f3rt/machine.hpp"
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace f3rt {

BehaviourReplay::BehaviourReplay(Machine &machine, Sandbox &sandbox, const SpriteGroupTracker &tracker,
                                 const SpriteUnitTable &units,
                                 std::vector<const SpriteBehaviour *> behaviours)
    : m_machine(machine), m_sandbox(sandbox), m_tracker(tracker),
      m_units(units.units.begin(), units.units.end()), m_behaviours(m_units.size()),
      m_stats{} {
    m_stats.units.resize(m_units.size());
    for (size_t i = 0; i < m_units.size(); ++i) {
        const EmitUnit &unit = *m_units[i];
        if (unit.id != i || unit.starts.size() != unit.ends.size())
            throw std::invalid_argument("Malformed emit-unit table");
        m_stats.units[i].name = unit.name;
        for (size_t s = 0; s < unit.starts.size(); ++s) {
            m_hooks[unit.starts[s]].enters.push_back(unit.id);
            m_hooks[unit.ends[s]].exits.push_back(unit.id);
        }
    }
    for (const SpriteBehaviour *behaviour : behaviours) {
        size_t target = m_units.size();
        for (size_t i = 0; i < m_units.size(); ++i)
            if (behaviour->unit == m_units[i] ||
                std::strcmp(behaviour->unit->name, m_units[i]->name) == 0)
                target = i;
        if (target == m_units.size())
            throw std::invalid_argument(std::string("Sprite behaviour '") + behaviour->name +
                                        "' targets a unit this game does not declare");
        m_behaviours[target].push_back(behaviour);
    }
    m_stack.reserve(MAX_OPEN);
}

// ---- Real-CPU hooks --------------------------------------------------------------------

void BehaviourReplay::unit_enter(f3_cpu *cpu, uint32_t unit) {
    if (cpu != &m_machine.cpu || unit >= m_units.size())
        return; // the sandbox never opens invocations
    enter_real(unit);
}

int BehaviourReplay::unit_exit(f3_cpu *cpu, uint32_t unit) {
    if (m_sandbox.owns(cpu)) return m_sandbox.unit_exit(unit);
    if (cpu != &m_machine.cpu || unit >= m_units.size()) return 0;
    return exit_real(unit);
}

void BehaviourReplay::interpreted_instruction(uint32_t pc) {
    const auto it = m_hooks.find(pc);
    if (it == m_hooks.end()) return;
    // A translated block already fired the hooks for this label before falling back.
    if (pc == m_last_hook_pc && m_machine.cpu.cycles == m_last_hook_cycles) return;
    m_last_hook_pc = pc;
    m_last_hook_cycles = m_machine.cpu.cycles;
    for (uint32_t unit : it->second.exits) exit_real(unit);
    for (uint32_t unit : it->second.enters) enter_real(unit);
}

void BehaviourReplay::enter_real(uint32_t id) {
    const EmitUnit &unit = *m_units[id];
    const uint32_t pc = m_machine.cpu.pc;
    m_last_hook_pc = pc;
    m_last_hook_cycles = m_machine.cpu.cycles;
    for (size_t start = 0; start < unit.starts.size(); ++start)
        if (unit.starts[start] == pc) {
            open_invocation(unit, start);
            return;
        }
}

void BehaviourReplay::open_invocation(const EmitUnit &unit, size_t start) {
    // A previous invocation of the same start that never exited is stale.
    for (size_t i = m_stack.size(); i-- > 0;)
        if (m_stack[i].unit == unit.id && m_stack[i].start_index == start) {
            while (m_stack.size() > i) {
                m_pool.push_back(std::move(m_stack.back()));
                m_stack.pop_back();
                ++m_stats.discarded;
            }
            break;
        }
    if (m_stack.size() >= MAX_OPEN) {
        m_pool.push_back(std::move(m_stack.front()));
        m_stack.erase(m_stack.begin());
        ++m_stats.discarded;
    }
    Invocation invocation;
    if (!m_pool.empty()) {
        invocation = std::move(m_pool.back());
        m_pool.pop_back();
    }
    invocation.unit = unit.id;
    invocation.start_index = uint32_t(start);
    invocation.address = unit_register_value(m_machine.cpu, unsigned(unit.reg)) & 0xffffff;
    invocation.frame = m_machine.frame;
    invocation.keys.clear();
    invocation.patched.clear();
    ++m_stats.units[unit.id].invocations;
    ++m_stats.invocations;
    if (!m_behaviours[unit.id].empty()) run_replay(unit, invocation, invocation.patched);
    m_stack.push_back(std::move(invocation));
}

int BehaviourReplay::exit_real(uint32_t id) {
    const EmitUnit &unit = *m_units[id];
    const uint32_t pc = m_machine.cpu.pc;
    m_last_hook_pc = pc;
    m_last_hook_cycles = m_machine.cpu.cycles;
    for (size_t i = m_stack.size(); i-- > 0;) {
        Invocation &invocation = m_stack[i];
        if (invocation.unit != id || unit.ends[invocation.start_index] != pc) continue;
        while (m_stack.size() > i + 1) {
            m_pool.push_back(std::move(m_stack.back()));
            m_stack.pop_back();
            ++m_stats.discarded;
        }
        finish(m_stack[i]);
        m_pool.push_back(std::move(m_stack[i]));
        m_stack.erase(m_stack.begin() + std::ptrdiff_t(i));
        return 0;
    }
    ++m_stats.unmatched_exits;
    return 0;
}

void BehaviourReplay::sprite_group_write(const SpriteRamWrite &write) {
    if (m_stack.empty()) return;
    // Extent of the invocation's real writes (splice range); block members included.
    std::vector<uint16_t> &keys = m_stack.back().keys;
    for (unsigned i = 0; i < write.width; ++i) {
        const uint32_t offset = write.start + i;
        if (offset >= SPRITE_RAM_SIZE) break;
        if (!sprite_slot_listed(offset)) continue;
        const uint16_t slot = sprite_slot_for(offset);
        if (keys.empty() || keys.back() != slot) keys.push_back(slot);
    }
}

void BehaviourReplay::finish(Invocation &invocation) {
    UnitStats &stats = m_stats.units[invocation.unit];
    if (invocation.keys.empty()) return; // nothing drawn this time
    std::sort(invocation.keys.begin(), invocation.keys.end());
    invocation.keys.erase(std::unique(invocation.keys.begin(), invocation.keys.end()),
                          invocation.keys.end());
    const bool first_bank = invocation.keys.front() >= SPRITE_ENTRIES;
    uint16_t lo[2] = {0xffff, 0xffff}, hi[2] = {0, 0};
    for (uint16_t key : invocation.keys) {
        const unsigned bank = key >= SPRITE_ENTRIES;
        const uint16_t entry = key & (SPRITE_ENTRIES - 1);
        lo[bank] = std::min(lo[bank], entry);
        hi[bank] = std::max(hi[bank], entry);
    }
    // Replace splices overlapping what the real span just drew.
    for (unsigned bank = 0; bank < 2; ++bank) {
        if (lo[bank] == 0xffff) continue;
        std::erase_if(m_splices, [&](const Splice &s) {
            return s.bank == bool(bank) && !(s.last < lo[bank] || s.first > hi[bank]);
        });
    }
    if (!invocation.patched.ok) return;
    const unsigned bank = first_bank;
    Splice splice;
    splice.bank = bool(bank);
    splice.first = lo[bank];
    splice.last = hi[bank];
    splice.frame = m_machine.frame;
    // Group of the unit's own record: a group write whose base is the unit address.
    for (uint16_t entry = splice.first; entry <= splice.last; ++entry) {
        const SpriteGroupSlot &slot = m_tracker.slot(size_t(bank) * SPRITE_ENTRIES + entry);
        if (!slot.key || SpriteGroupTracker::key_base(slot.key) != invocation.address) continue;
        splice.group = slot.key;
        splice.group_x = slot.x;
        splice.group_y = slot.y;
        splice.group_pc = slot.pc;
        break;
    }
    const uint8_t *real =
        m_machine.graphics.data() + size_t(bank) * SPRITE_BANK_STRIDE + size_t(splice.first) * 16;
    splice.real.assign(real, real + (size_t(splice.last) - splice.first + 1) * 16);
    splice.replacement = invocation.patched.dense;
    const auto at = std::lower_bound(
        m_splices.begin(), m_splices.end(), splice, [](const Splice &a, const Splice &b) {
            return a.bank != b.bank ? a.bank < b.bank : a.first < b.first;
        });
    m_splices.insert(at, std::move(splice));
    ++stats.spliced;
    ++m_stats.spliced;
}

void BehaviourReplay::frame_end() {
    for (size_t i = 0; i < m_stack.size();) {
        if (m_stack[i].frame + 1 < m_machine.frame) {
            m_pool.push_back(std::move(m_stack[i]));
            m_stack.erase(m_stack.begin() + std::ptrdiff_t(i));
            ++m_stats.discarded;
        } else
            ++i;
    }
    std::erase_if(m_splices,
                  [&](const Splice &s) { return s.frame + SPLICE_FRAMES < m_machine.frame; });
}

void BehaviourReplay::reset() {
    while (!m_stack.empty()) {
        m_pool.push_back(std::move(m_stack.back()));
        m_stack.pop_back();
    }
    m_splices.clear();
    m_last_hook_cycles = UINT64_MAX;
}

void BehaviourReplay::presentation() {
    m_view.clear();
    for (const Splice &splice : m_splices) {
        SpriteSplice out;
        out.bank = splice.bank;
        out.first = splice.first;
        out.last = splice.last;
        out.real = splice.real;
        out.replacement = splice.replacement;
        out.group = splice.group;
        out.group_x = splice.group_x;
        out.group_y = splice.group_y;
        out.group_pc = splice.group_pc;
        m_view.push_back(out);
    }
}

// ---- Sandbox replay --------------------------------------------------------------------

void BehaviourReplay::run_replay(const EmitUnit &unit, const Invocation &invocation,
                                 Sandbox::Result &out) {
    out.clear();
    if (!m_machine.block_count) {
        out.reason = Sandbox::Abort::NoNative;
        ++m_stats.no_native_skips;
        return;
    }
    m_sandbox.begin(unit.id, unit.starts[invocation.start_index],
                    unit.ends[invocation.start_index]);
    {
        UnitView view(
            &m_sandbox,
            [](void *self, uint32_t address, unsigned width) {
                return static_cast<Sandbox *>(self)->read(address, width);
            },
            invocation.address, unit.size);
        PatchView patch(
            &m_sandbox,
            [](void *self, uint32_t address, uint32_t value, unsigned width) {
                auto *sandbox = static_cast<Sandbox *>(self);
                if ((address & 0xffffff) - RAM_BASE < RAM_END - RAM_BASE)
                    sandbox->write(address, value, width);
            },
            invocation.address, unit.size);
        bool any = false;
        for (const SpriteBehaviour *behaviour : m_behaviours[unit.id])
            any |= behaviour->apply(view, patch);
        if (!any) return; // nothing to show differently: keep real entries
    }
    ++m_stats.replays;
    ++m_stats.units[unit.id].replays;

    const Sandbox::Abort abort = m_sandbox.run();
    if (abort != Sandbox::Abort::Count) {
        out.reason = abort;
        record_abort(unit.id, abort);
        return;
    }
    m_sandbox.collect(out);
    out.ok = true;
    ++m_stats.units[unit.id].completed;
    ++m_stats.completed;
}

void BehaviourReplay::record_abort(uint32_t unit, Sandbox::Abort reason) {
    ++m_stats.units[unit].aborted;
    ++m_stats.units[unit].aborts[size_t(reason)];
    ++m_stats.aborted;
}

} // namespace f3rt
