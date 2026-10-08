#include "sprite_units.hpp"
#include "f3rt/machine.hpp"
#include "generated_config/sprite_units.hpp" // not runtime/sprite_units.hpp: the configure-time table
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ostream>
#include <stdexcept>

namespace f3rt {
namespace {
// Main bus map subset used by the sandbox (mirrors machine.cpp; sizes are exclusive spans).
constexpr uint32_t ROM_END = 0x200000;
constexpr uint32_t RAM_BASE = 0x400000, RAM_SPAN = 0x40000, RAM_MASK = 0x1ffff;
constexpr uint32_t PALETTE_BASE = 0x440000, PALETTE_SPAN = 0x8000;
constexpr uint32_t GRAPHICS_BASE = 0x600000, GRAPHICS_SPAN = 0x40000;
constexpr uint32_t SPRITE_SPAN = 0x10000;       // two 0x8000 banks; only entries 0..0x3ff of each are listed
constexpr uint32_t BANK_BYTES = 0x8000, ENTRIES = 0x400;
constexpr uint64_t REPLAY_BUDGET_CYCLES = 1000000;
constexpr uint32_t REPLAY_BUDGET_BLOCKS = 500000;
constexpr size_t MAX_OPEN = 8;
constexpr uint64_t SPLICE_FRAMES = 4;          // frames a splice survives without refresh
constexpr size_t MAX_MISMATCHES = 8;

constexpr uint16_t sprite_key(uint32_t offset) { // offset within 0x600000..0x610000
    return uint16_t(((offset >> 15) & 1) * ENTRIES + ((offset & 0x7fff) >> 4));
}
constexpr bool listed(uint32_t offset) { return ((offset & 0x7fff) >> 4) < ENTRIES; }
}

const SpriteUnitTable &SpriteUnits::game_table() {
    static const SpriteUnitTable table{
        std::span<const EmitUnit *const>(sprite_units::all),
        std::span<const PcRange>(sprite_units::frame_writers)};
    return table;
}

const char *SpriteUnits::abort_name(Abort reason) {
    switch (reason) {
    case Abort::Device: return "device-access";
    case Abort::ForbiddenWrite: return "forbidden-write";
    case Abort::Untranslated: return "untranslated-pc";
    case Abort::Exception: return "exception";
    case Abort::ResetDevices: return "reset-devices";
    case Abort::Budget: return "budget";
    case Abort::Stopped: return "stopped";
    case Abort::NoNative: return "no-native-blocks";
    case Abort::Count: break;
    }
    return "none";
}

SpriteUnits::SpriteUnits(Machine &machine, const SpriteUnitTable &table, Options options)
    : machine_(machine), options_(std::move(options)),
      units_(table.units.begin(), table.units.end()), frame_writers_(table.frame_writers) {
    sandbox_cpu_.runtime = &machine;
    page_slot_.fill(-1);
    capture_slot_.fill(-1);
    behaviours_.resize(units_.size());
    report_.units.resize(units_.size());
    bool writer_owned = false;
    for (size_t i = 0; i < units_.size(); ++i) {
        const EmitUnit &unit = *units_[i];
        if (unit.id != i || unit.starts.size() != unit.ends.size())
            throw std::invalid_argument("Malformed emit-unit table");
        report_.units[i].name = unit.name;
        writer_owned |= unit.owner == UnitOwner::Writer;
        for (size_t s = 0; s < unit.starts.size(); ++s) {
            hooks_[unit.starts[s]].enters.push_back(unit.id);
            hooks_[unit.ends[s]].exits.push_back(unit.id);
        }
    }
    for (const SpriteBehaviour *behaviour : options_.behaviours) {
        size_t target = units_.size();
        for (size_t i = 0; i < units_.size(); ++i)
            if (behaviour->unit == units_[i] || std::strcmp(behaviour->unit->name, units_[i]->name) == 0)
                target = i;
        if (target == units_.size())
            throw std::invalid_argument(std::string("Sprite behaviour '") + behaviour->name +
                                        "' targets a unit this game does not declare");
        behaviours_[target].push_back(behaviour);
    }
    if (writer_owned) writer_pc_.assign(0x10000, 0);
    stack_.reserve(MAX_OPEN);
}
SpriteUnits::~SpriteUnits() = default;

uint32_t SpriteUnits::unit_register(const EmitUnit &unit) const {
    const auto &cpu = machine_.cpu;
    const unsigned index = unsigned(unit.reg);
    return index < 8 ? cpu.d[index] : cpu.a[index - 8];
}

uint32_t SpriteUnits::irq_depth() {
    const unsigned mask = (machine_.cpu.sr >> 8) & 7;
    while (!irq_levels_.empty() && mask < irq_levels_.back()) irq_levels_.pop_back();
    return uint32_t(irq_levels_.size());
}
void SpriteUnits::note_irq(unsigned level) { irq_levels_.push_back(level); }

void SpriteUnits::reset() {
    while (!stack_.empty()) { pool_.push_back(std::move(stack_.back())); stack_.pop_back(); } // not a policy discard
    irq_levels_.clear();
    identity_.fill(0);
    object_.fill(0);
    splices_.clear();
    generations_.clear();
    ordinals_.clear();
    std::fill(writer_pc_.begin(), writer_pc_.end(), 0u);
    last_hook_cycles_ = UINT64_MAX;
}

void SpriteUnits::discard_from(size_t index) {
    while (stack_.size() > index) {
        pool_.push_back(std::move(stack_.back()));
        stack_.pop_back();
        ++report_.discarded;
    }
}
void SpriteUnits::close_invocation(size_t index) {
    pool_.push_back(std::move(stack_[index]));
    stack_.erase(stack_.begin() + std::ptrdiff_t(index));
}

// ---- Real-CPU hooks -------------------------------------------------------------------

void SpriteUnits::unit_enter(f3_cpu *cpu, uint32_t unit) {
    if (cpu != &machine_.cpu || unit >= units_.size()) return; // the sandbox never opens invocations
    enter_real(unit);
}
int SpriteUnits::unit_exit(f3_cpu *cpu, uint32_t unit) {
    if (cpu == &sandbox_cpu_) {
        if (unit != sandbox_unit_ || cpu->pc != sandbox_end_pc_ || sandbox_abort_ != Abort::Count) return 0;
        sandbox_done_ = true;
        return 1;
    }
    if (cpu != &machine_.cpu || unit >= units_.size()) return 0;
    return exit_real(unit);
}
void SpriteUnits::interpreted_instruction(uint32_t pc) {
    const auto it = hooks_.find(pc);
    if (it == hooks_.end()) return;
    // A translated block already fired the hooks for this label before falling back.
    if (pc == last_hook_pc_ && machine_.cpu.cycles == last_hook_cycles_) return;
    last_hook_pc_ = pc;
    last_hook_cycles_ = machine_.cpu.cycles;
    for (uint32_t unit : it->second.exits) exit_real(unit);
    for (uint32_t unit : it->second.enters) enter_real(unit);
}

void SpriteUnits::enter_real(uint32_t id) {
    const EmitUnit &unit = *units_[id];
    const uint32_t pc = machine_.cpu.pc;
    last_hook_pc_ = pc;
    last_hook_cycles_ = machine_.cpu.cycles;
    size_t start = unit.starts.size();
    for (size_t i = 0; i < unit.starts.size(); ++i)
        if (unit.starts[i] == pc) { start = i; break; }
    if (start == unit.starts.size()) return;
    open_invocation(unit, start);
}

void SpriteUnits::open_invocation(const EmitUnit &unit, size_t start) {
    const uint32_t depth = irq_depth();
    while (!stack_.empty() && stack_.back().irq_depth > depth) discard_from(stack_.size() - 1);
    for (size_t i = stack_.size(); i-- > 0;)
        if (stack_[i].unit == unit.id && stack_[i].start_index == start && stack_[i].irq_depth == depth) {
            discard_from(i); // previous invocation never exited
            break;
        }
    if (stack_.size() >= MAX_OPEN) {
        pool_.push_back(std::move(stack_.front()));
        stack_.erase(stack_.begin());
        ++report_.discarded;
    }
    Invocation invocation;
    if (!pool_.empty()) { invocation = std::move(pool_.back()); pool_.pop_back(); }
    invocation.unit = unit.id;
    invocation.start_index = uint32_t(start);
    invocation.start_pc = unit.starts[start];
    invocation.address = unit_register(unit) & 0xffffff;
    invocation.irq_depth = depth;
    invocation.frame = machine_.frame;
    invocation.keys.clear();
    invocation.check.clear();
    invocation.patched.clear();
    invocation.has_patched = false;

    if (unit.owner == UnitOwner::Unit) {
        // A new generation starts when the address had no invocation in the previous frame.
        const uint64_t key = uint64_t(unit.id) << 32 | invocation.address;
        auto [it, fresh] = generations_.try_emplace(key, Generation{machine_.frame, 1});
        if (!fresh) {
            if (machine_.frame > it->second.last_frame + 1) ++it->second.generation;
            it->second.last_frame = machine_.frame;
        }
        invocation.identity = sprite_identity_mix(key, it->second.generation);
    } else {
        const uint32_t word = (invocation.address & RAM_MASK) >> 1;
        const uint32_t writer = writer_pc_.empty() ? 0 : writer_pc_[word];
        // Never-written records fall back to their address so they stay distinguishable.
        const uint64_t key = uint64_t(unit.id) << 32 | (writer ? writer : invocation.address | 0x80000000u);
        invocation.identity = sprite_identity_mix(key, ordinals_[key]++);
    }
    ++report_.units[unit.id].invocations;
    ++report_.invocations;
    if (options_.check) run_replay(unit, invocation, false, invocation.check);
    if (!behaviours_[unit.id].empty()) {
        run_replay(unit, invocation, true, invocation.patched);
        invocation.has_patched = true;
    }
    stack_.push_back(std::move(invocation));
}

int SpriteUnits::exit_real(uint32_t id) {
    const EmitUnit &unit = *units_[id];
    const uint32_t pc = machine_.cpu.pc;
    last_hook_pc_ = pc;
    last_hook_cycles_ = machine_.cpu.cycles;
    const uint32_t depth = irq_depth();
    while (!stack_.empty() && stack_.back().irq_depth > depth) discard_from(stack_.size() - 1);
    for (size_t i = stack_.size(); i-- > 0;) {
        Invocation &invocation = stack_[i];
        if (invocation.unit != id || invocation.irq_depth != depth ||
            unit.ends[invocation.start_index] != pc) continue;
        discard_from(i + 1);
        finish(stack_[i]);
        close_invocation(i);
        return 0;
    }
    ++report_.unmatched_exits;
    return 0;
}

// ---- Real sprite writes ---------------------------------------------------------------

void SpriteUnits::note_write(uint32_t address, unsigned width) {
    const uint32_t a = address & 0xffffff;
    if (a - RAM_BASE < RAM_SPAN) {
        if (writer_pc_.empty()) return;
        const uint32_t offset = a & RAM_MASK;
        const uint32_t last = std::min(offset + width - 1, RAM_MASK);
        for (uint32_t word = offset >> 1; word <= (last >> 1); ++word) writer_pc_[word] = machine_.cpu.pc;
        return;
    }
    for (unsigned i = 0; i < width; ++i)
        if (a + i - GRAPHICS_BASE < SPRITE_SPAN) sprite_write(a + i);
}

void SpriteUnits::sprite_write(uint32_t address) {
    const uint32_t offset = address - GRAPHICS_BASE;
    if (!listed(offset)) return;
    const uint16_t key = sprite_key(offset);
    const uint32_t depth = irq_depth();
    while (!stack_.empty() && stack_.back().irq_depth > depth) discard_from(stack_.size() - 1);
    if (!stack_.empty() && stack_.back().irq_depth == depth) {
        auto &keys = stack_.back().keys;
        if (keys.empty() || keys.back() != key) keys.push_back(key);
        return;
    }
    identity_[key] = 0;
    object_[key] = 0;
    if (!options_.check) return;
    const uint32_t pc = machine_.cpu.pc;
    auto [it, fresh] = stray_.try_emplace(pc);
    StrayWriter &stray = it->second;
    if (fresh) {
        stray.pc = pc;
        stray.first_frame = machine_.frame;
        stray.first_address = address;
        for (const PcRange &range : frame_writers_) stray.accounted |= contains(range, pc);
    }
    ++stray.count;
}

// ---- Invocation exit ------------------------------------------------------------------

void SpriteUnits::finish(Invocation &invocation) {
    UnitStats &stats = report_.units[invocation.unit];
    if (invocation.keys.empty()) {
        // Nothing drawn this time: any older splice over this unit stays until it ages out.
        if (invocation.check.ok) compare(invocation, invocation.keys);
        return;
    }
    const bool first_bank = invocation.keys.front() >= ENTRIES;
    std::vector<uint16_t> keys = invocation.keys;
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    stats.real_entries += keys.size();
    uint16_t lo[2] = {0xffff, 0xffff}, hi[2] = {0, 0};
    for (uint16_t key : keys) {
        const unsigned bank = key >= ENTRIES;
        const uint16_t entry = key & (ENTRIES - 1);
        lo[bank] = std::min(lo[bank], entry);
        hi[bank] = std::max(hi[bank], entry);
    }
    for (uint16_t key : keys) {
        const unsigned bank = key >= ENTRIES;
        identity_[key] = sprite_identity_mix(invocation.identity, (key & (ENTRIES - 1)) - lo[bank]);
        object_[key] = invocation.identity;
    }
    if (invocation.check.ok) compare(invocation, keys);

    // Replace splices overlapping what the real span just drew, then add this invocation's.
    for (unsigned bank = 0; bank < 2; ++bank) {
        if (lo[bank] == 0xffff) continue;
        std::erase_if(splices_, [&](const Splice &s) {
            return s.bank == bool(bank) && !(s.last < lo[bank] || s.first > hi[bank]);
        });
    }
    if (!invocation.has_patched || !invocation.patched.ok) return;
    const unsigned bank = first_bank;
    Splice splice;
    splice.bank = bool(bank);
    splice.first = lo[bank];
    splice.last = hi[bank];
    splice.identity = invocation.identity;
    splice.frame = machine_.frame;
    const uint8_t *real = machine_.graphics.data() + bank * BANK_BYTES + size_t(splice.first) * 16;
    splice.real.assign(real, real + (size_t(splice.last) - splice.first + 1) * 16);
    splice.replacement = invocation.patched.dense;
    const auto at = std::lower_bound(splices_.begin(), splices_.end(), splice, [](const Splice &a, const Splice &b) {
        return a.bank != b.bank ? a.bank < b.bank : a.first < b.first;
    });
    splices_.insert(at, std::move(splice));
    ++stats.spliced;
    ++report_.spliced;
}

void SpriteUnits::compare(Invocation &invocation, const std::vector<uint16_t> &real_keys) {
    UnitStats &stats = report_.units[invocation.unit];
    const auto &replay = invocation.check.written;
    char detail[200] = "";
    size_t i = 0, j = 0;
    const auto bytes_at = [&](uint16_t key) {
        return machine_.graphics.data() + (key >= ENTRIES ? BANK_BYTES : 0) + size_t(key & (ENTRIES - 1)) * 16;
    };
    const auto hex = [](const uint8_t *p) {
        char text[40];
        for (int k = 0; k < 16; ++k) std::snprintf(text + k * 2, 3, "%02x", p[k]);
        return std::string(text, 32);
    };
    bool equal = true;
    while (equal && (i < real_keys.size() || j < replay.size())) {
        const uint32_t rk = i < real_keys.size() ? real_keys[i] : 0xffffffffu;
        const uint32_t pk = j < replay.size() ? replay[j].key : 0xffffffffu;
        const uint32_t key = std::min(rk, pk);
        if (rk == pk) {
            if (std::memcmp(bytes_at(uint16_t(rk)), replay[j].bytes.data(), 16) != 0) {
                std::snprintf(detail, sizeof detail, "bank%u entry 0x%03x real=%s replay=%s",
                              unsigned(key >= ENTRIES), key & (ENTRIES - 1), hex(bytes_at(uint16_t(rk))).c_str(),
                              hex(replay[j].bytes.data()).c_str());
                equal = false;
            }
            ++i; ++j;
        } else if (rk < pk) {
            std::snprintf(detail, sizeof detail, "bank%u entry 0x%03x written by real span, missing in replay real=%s",
                          unsigned(key >= ENTRIES), key & (ENTRIES - 1), hex(bytes_at(uint16_t(rk))).c_str());
            equal = false;
        } else {
            std::snprintf(detail, sizeof detail, "bank%u entry 0x%03x written by replay only replay=%s",
                          unsigned(key >= ENTRIES), key & (ENTRIES - 1), hex(replay[j].bytes.data()).c_str());
            equal = false;
        }
    }
    if (equal) {
        ++stats.matched;
        ++report_.matched;
        return;
    }
    ++stats.mismatched;
    ++report_.mismatched;
    if (report_.mismatches.size() < MAX_MISMATCHES)
        report_.mismatches.push_back({invocation.unit, invocation.start_pc, invocation.address,
                                      machine_.frame, detail});
}

// ---- Frame / presentation ---------------------------------------------------------------

void SpriteUnits::frame_end() {
    ++report_.frames;
    ordinals_.clear();
    for (size_t i = 0; i < stack_.size();) {
        if (stack_[i].frame + 1 < machine_.frame) {
            // Keep stack order: drop this entry only.
            pool_.push_back(std::move(stack_[i]));
            stack_.erase(stack_.begin() + std::ptrdiff_t(i));
            ++report_.discarded;
        } else ++i;
    }
    std::erase_if(splices_, [&](const Splice &s) { return s.frame + SPLICE_FRAMES < machine_.frame; });
    if ((machine_.frame & 0x3ff) == 0)
        std::erase_if(generations_, [&](const auto &kv) { return kv.second.last_frame + 600 < machine_.frame; });
}

SpritePresentation SpriteUnits::presentation() const {
    view_.clear();
    for (const Splice &splice : splices_) {
        SpriteSplice out;
        out.bank = splice.bank;
        out.first = splice.first;
        out.last = splice.last;
        out.real = splice.real;
        out.replacement = splice.replacement;
        out.identity = splice.identity;
        view_.push_back(out);
    }
    return {std::span<const uint64_t>(identity_), std::span<const SpriteSplice>(view_), std::span<const uint64_t>(object_)};
}

// ---- Sandbox ------------------------------------------------------------------------

const f3_block *SpriteUnits::find_block(uint32_t pc) const {
    if (pc >= ROM_END || (pc & 1)) return nullptr;
    const auto &page = machine_.native_pages[pc >> 12];
    if (!page.count) return nullptr;
    if (page.dense) {
        const uint32_t offset = pc - page.address;
        return !(offset & 1) && offset < 2u * page.count ? page.first + (offset >> 1) : nullptr;
    }
    const f3_block *end = page.first + page.count;
    const auto *entry = std::lower_bound(page.first, end, pc,
        [](const f3_block &block, uint32_t value) { return block.address < value; });
    return entry != end && entry->address == pc ? entry : nullptr;
}

uint8_t SpriteUnits::ram_get(uint32_t offset) const {
    const int16_t slot = page_slot_[offset / overlay_page];
    return slot >= 0 ? page_pool_[size_t(slot)][offset % overlay_page] : machine_.ram[offset];
}
void SpriteUnits::ram_put(uint32_t offset, uint8_t value) {
    int16_t &slot = page_slot_[offset / overlay_page];
    if (slot < 0) {
        slot = int16_t(touched_pages_.size());
        if (page_pool_.size() <= size_t(slot)) page_pool_.emplace_back();
        const uint32_t base = offset - offset % overlay_page;
        std::memcpy(page_pool_[size_t(slot)].data(), machine_.ram.data() + base, overlay_page);
        touched_pages_.push_back(uint16_t(offset / overlay_page));
    }
    page_pool_[size_t(slot)][offset % overlay_page] = value;
}

void SpriteUnits::sandbox_abort(Abort reason) {
    if (sandbox_abort_ != Abort::Count) return;
    sandbox_abort_ = reason;
    sandbox_cpu_.halted = 1; // stops chained native blocks at the next guard
}

uint8_t SpriteUnits::sandbox_read8(uint32_t address) {
    const uint32_t a = address & 0xffffff;
    if (a < ROM_END) return a < machine_.roms.main.size() ? machine_.roms.main[a] : 0xff;
    if (a - RAM_BASE < RAM_SPAN) return ram_get(a & RAM_MASK);
    if (a - PALETTE_BASE < PALETTE_SPAN) return machine_.palette[a - PALETTE_BASE];
    if (a - GRAPHICS_BASE < GRAPHICS_SPAN) {
        const uint32_t offset = a - GRAPHICS_BASE;
        if (offset < SPRITE_SPAN && listed(offset)) {
            const int16_t slot = capture_slot_[sprite_key(offset)];
            if (slot >= 0) return capture_[size_t(slot)].bytes[offset & 15];
        }
        return machine_.graphics[offset];
    }
    sandbox_abort(Abort::Device); // I/O, control, shared RAM, sound, EEPROM, unmapped
    return 0;
}

void SpriteUnits::sandbox_write8(uint32_t address, uint8_t value) {
    const uint32_t a = address & 0xffffff;
    if (a - RAM_BASE < RAM_SPAN) { ram_put(a & RAM_MASK, value); return; }
    if (a - GRAPHICS_BASE < SPRITE_SPAN) {
        const uint32_t offset = a - GRAPHICS_BASE;
        if (!listed(offset)) return; // never displayed, never compared
        const uint16_t key = sprite_key(offset);
        int16_t &slot = capture_slot_[key];
        if (slot < 0) {
            // Copy-on-first-touch of the whole entry so untouched words match hardware.
            Entry entry{key, {}};
            std::memcpy(entry.bytes.data(), machine_.graphics.data() + (offset & ~15u), 16);
            slot = int16_t(capture_.size());
            capture_.push_back(entry);
        }
        capture_[size_t(slot)].bytes[offset & 15] = value;
        return;
    }
    if (a < ROM_END || a - PALETTE_BASE < PALETTE_SPAN || a - GRAPHICS_BASE < GRAPHICS_SPAN)
        sandbox_abort(Abort::ForbiddenWrite);
    else
        sandbox_abort(Abort::Device);
}

uint32_t SpriteUnits::sandbox_read(uint32_t address, unsigned width) {
    if (sandbox_abort_ != Abort::Count) return 0;
    uint32_t value = 0;
    for (unsigned i = 0; i < width; ++i) value = value << 8 | sandbox_read8(address + i);
    return value;
}
void SpriteUnits::sandbox_write(uint32_t address, uint32_t value, unsigned width) {
    if (sandbox_abort_ != Abort::Count) return;
    for (unsigned i = 0; i < width; ++i)
        sandbox_write8(address + i, uint8_t(value >> (8 * (width - 1 - i))));
}

void SpriteUnits::record_abort(uint32_t unit, Abort reason) {
    ++report_.units[unit].aborted;
    ++report_.units[unit].aborts[size_t(reason)];
    ++report_.aborted;
}

void SpriteUnits::collect(ReplayResult &out, bool dense) {
    out.written = capture_;
    std::sort(out.written.begin(), out.written.end(),
              [](const Entry &a, const Entry &b) { return a.key < b.key; });
    if (!dense || capture_.empty()) return;
    out.bank = capture_.front().key >= ENTRIES; // bank of the first write
    uint16_t lo = 0xffff, hi = 0;
    for (const Entry &entry : capture_) {
        if ((entry.key >= ENTRIES) != out.bank) continue;
        lo = std::min<uint16_t>(lo, entry.key & (ENTRIES - 1));
        hi = std::max<uint16_t>(hi, entry.key & (ENTRIES - 1));
    }
    out.first = lo;
    out.last = hi;
    out.dense.resize((size_t(hi) - lo + 1) * 16);
    for (uint16_t entry = lo; entry <= hi; ++entry) {
        const uint16_t key = uint16_t((out.bank ? ENTRIES : 0) + entry);
        const int16_t slot = capture_slot_[key];
        const uint8_t *source = slot >= 0 ? capture_[size_t(slot)].bytes.data()
            : machine_.graphics.data() + (out.bank ? BANK_BYTES : 0) + size_t(entry) * 16;
        std::memcpy(out.dense.data() + size_t(entry - lo) * 16, source, 16);
    }
}

void SpriteUnits::run_replay(const EmitUnit &unit, const Invocation &invocation, bool patched, ReplayResult &out) {
    out.clear();
    if (!machine_.block_count) {
        out.reason = Abort::NoNative;
        ++report_.no_native_skips;
        return;
    }
    // Reset scratch state, then clone the (flushed) real CPU. The clone is the only CPU
    // that ever executes; Machine state, counters and the real f3_cpu are never touched.
    for (uint16_t page : touched_pages_) page_slot_[page] = -1;
    touched_pages_.clear();
    for (const Entry &entry : capture_) capture_slot_[entry.key] = -1;
    capture_.clear();
    sandbox_cpu_ = machine_.cpu;
    sandbox_cpu_.runtime = &machine_;
    sandbox_cpu_.pc = unit.starts[invocation.start_index];
    sandbox_cpu_.stopped = sandbox_cpu_.halted = 0;
    sandbox_unit_ = unit.id;
    sandbox_end_pc_ = unit.ends[invocation.start_index];
    sandbox_done_ = false;
    sandbox_abort_ = Abort::Count;

    if (patched) {
        UnitView view(this, [](void *self, uint32_t address, unsigned width) {
            return static_cast<SpriteUnits *>(self)->sandbox_read(address, width);
        }, invocation.address, unit.size);
        PatchView patch(this, [](void *self, uint32_t address, uint32_t value, unsigned width) {
            auto *units = static_cast<SpriteUnits *>(self);
            if ((address & 0xffffff) - RAM_BASE < RAM_SPAN) units->sandbox_write(address, value, width);
        }, invocation.address, unit.size);
        bool any = false;
        for (const SpriteBehaviour *behaviour : behaviours_[unit.id]) any |= behaviour->apply(view, patch);
        if (!any) return; // nothing to show differently: keep real entries, not a replay
    }
    ++report_.replays;
    ++report_.units[unit.id].replays;

    const uint64_t limit = sandbox_cpu_.cycles + REPLAY_BUDGET_CYCLES;
    for (uint32_t blocks = 0; sandbox_abort_ == Abort::Count && !sandbox_done_; ++blocks) {
        if (blocks >= REPLAY_BUDGET_BLOCKS || sandbox_cpu_.cycles >= limit) { sandbox_abort(Abort::Budget); break; }
        if (sandbox_cpu_.halted || sandbox_cpu_.stopped || (sandbox_cpu_.sr & 0xc000)) {
            sandbox_abort(Abort::Stopped);
            break;
        }
        const f3_block *block = find_block(sandbox_cpu_.pc);
        if (!block) { sandbox_abort(Abort::Untranslated); break; }
        sandbox_cpu_.dispatch_deadline = limit; // chained blocks yield here, never at device events
        block->execute(&sandbox_cpu_);
    }
    if (sandbox_abort_ != Abort::Count) {
        out.reason = sandbox_abort_;
        record_abort(unit.id, out.reason);
        return;
    }
    collect(out, patched);
    out.ok = true;
    ++report_.units[unit.id].completed;
    ++report_.completed;
}

// ---- Reporting ------------------------------------------------------------------------

SpriteUnits::Report SpriteUnits::report() const {
    Report result = report_;
    for (const auto &[pc, stray] : stray_) {
        result.stray.push_back(stray);
        result.stray_writes += stray.count;
        if (!stray.accounted) result.stray_unaccounted += stray.count;
    }
    std::sort(result.stray.begin(), result.stray.end(),
              [](const StrayWriter &a, const StrayWriter &b) { return a.pc < b.pc; });
    return result;
}

bool SpriteUnits::passed(bool allow_aborts) const {
    const Report r = report();
    return r.mismatched == 0 && r.stray_unaccounted == 0 && (allow_aborts || r.aborted == 0);
}

void SpriteUnits::write_summary(std::ostream &out) const {
    out << "sprite_units: invocations=" << report_.invocations << " replays=" << report_.replays
        << " completed=" << report_.completed << " splices=" << report_.spliced
        << " aborts=" << report_.aborted << " discarded=" << report_.discarded
        << " frames=" << report_.frames << '\n';
}

void SpriteUnits::write_report(std::ostream &out) const {
    const Report r = report();
    out << "sprite units: invocations=" << r.invocations << " replays=" << r.replays
        << " completed=" << r.completed << " matched=" << r.matched << " mismatched=" << r.mismatched
        << " aborted=" << r.aborted << " spliced=" << r.spliced << " unmatched_exits=" << r.unmatched_exits
        << " discarded=" << r.discarded << " no_native_skips=" << r.no_native_skips << '\n';
    for (const UnitStats &unit : r.units) {
        out << "  unit " << unit.name << ": invocations=" << unit.invocations << " replays=" << unit.replays
            << " completed=" << unit.completed << " matched=" << unit.matched
            << " mismatched=" << unit.mismatched << " aborted=" << unit.aborted
            << " spliced=" << unit.spliced << " real_entries=" << unit.real_entries << '\n';
        for (size_t i = 0; i < unit.aborts.size(); ++i)
            if (unit.aborts[i]) out << "    abort " << abort_name(Abort(i)) << ": " << unit.aborts[i] << '\n';
    }
    char line[96];
    for (const Mismatch &mismatch : r.mismatches) {
        std::snprintf(line, sizeof line, "pc=0x%06x frame=%llu unit_address=0x%06x", mismatch.start_pc,
                      static_cast<unsigned long long>(mismatch.frame), mismatch.unit_address);
        out << "  mismatch " << r.units[mismatch.unit].name << ' ' << line << ' ' << mismatch.detail << '\n';
    }
    out << "  sprite writes outside units: " << r.stray_writes << " (unaccounted " << r.stray_unaccounted << ")\n";
    for (const StrayWriter &stray : r.stray) {
        std::snprintf(line, sizeof line, "pc=0x%06x writes=%llu first_frame=%llu first_address=0x%06x",
                      stray.pc, static_cast<unsigned long long>(stray.count),
                      static_cast<unsigned long long>(stray.first_frame), stray.first_address);
        out << "    " << line << (stray.accounted ? " frame_writer" : " UNACCOUNTED") << '\n';
    }
    out << "  result: " << (passed() ? "PASS" : "FAIL") << '\n';
}

} // namespace f3rt
