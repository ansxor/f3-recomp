// f3rt-sprite-check: validates emit-unit declarations (config.toml [[video.emit_units]] /
// [video.frame_writers]) and sprite behaviours against the game's own native code.
//
// Two native strict headless machines run the same inputs frame by frame:
//   A: SpriteUnits in check mode (+ optional behaviours)
//   B: sprite units off
// Failures (nonzero exit):
//   * any unit invocation whose unpatched sandbox replay differs bit-exactly from the
//     entries the real span wrote, or any replay that aborts;
//   * a sprite-RAM write outside every unit from a PC outside [video.frame_writers];
//   * A and B diverging in cycles, native_blocks, or (every --compare-every frames and at
//     the end) state_crc()/sync_state_crc();
//   * behaviour invariants (below) or no splice produced although behaviours are enabled.
//
// Behaviour invariants are checked every frame on the presentation A would render, by
// decoding the sprite list with identities only (real entries) and with the splices
// applied, and comparing the entries that belong to each splice (matched by identity).
//   full-detail: the replacement keeps the object's screen footprint (see ANCHOR_TILES /
//   SCALE_STEP below).
#include "f3rt/audio.hpp"
#include "f3rt/machine.hpp"
#include "f3rt/rom.hpp"
#include "renderer/decode.hpp"
#include "renderer/sprite_behaviour.hpp"
#include "sprite_units.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef F3RT_GENERATED
#include "program.h"
#endif

namespace {

using namespace f3rt;

// Footprint invariants for full-detail (24.8 fixed point, like the decoder). The real entries
// are the coarse mip level (LoD class k) drawn at the compensated zoom ((z - T[k]) << k),
// the replacement is the L0 level at the projected zoom z, so the two footprints are the
// same object seen through different grids, not identical rectangles:
//  * Scale: an L0 tile is never larger on screen than the coarse tile it replaces
//    ((z - T[k]) << k <= z for z in the class range), up to one zoom step (16 in 24.8).
//  * Anchor: the replacement lies where the object is: its bounding box must come within
//    ANCHOR_TILES coarse tiles of the real bounding box (0: the boxes must overlap; the
//    measured worst gap over 6000 attract frames is 0, so any displacement is a bug).
//    The coarse grid is padded to whole tiles and L0 culls blank tiles, so the boxes
//    differ by up to a tile per side; a stale class (the selector at 0x2fed6 runs before
//    projection) additionally lets the real draw clamp at 1:1 while L0 keeps growing, so
//    L0 may cover the real box and far more. This is a position check (right object,
//    right place), not a size equality.
//  * Shadow: 0x9b32 emits the shadow entries first (inputs $40, $42-$44, byte1 bit7 -- none
//    of which full-detail patches), then 0x9d32 the main entries (inputs $2, $4/$5, class).
//    So the raw entries of a splice share a bit-identical leading run (the shadow) and
//    must differ right after it. A splice whose real and replacement bytes are identical
//    throughout (patch had no effect) or which differs from its first entry on is counted;
//    the former is a violation.
constexpr int32_t SCALE_STEP = 16;
constexpr double ANCHOR_TILES = 0.0;

struct Box {
    bool any = false;
    int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    int32_t tile_w = 0, tile_h = 0; // largest single-entry extent
    void add(const DecodedSpriteEntry &e) {
        const int32_t w = e.scale_x * 16, h = e.scale_y * 16;
        const int32_t ax0 = std::min(e.x, e.x + w), ax1 = std::max(e.x, e.x + w);
        const int32_t ay0 = std::min(e.y, e.y + h), ay1 = std::max(e.y, e.y + h);
        tile_w = std::max(tile_w, ax1 - ax0);
        tile_h = std::max(tile_h, ay1 - ay0);
        if (!any) { x0 = ax0; x1 = ax1; y0 = ay0; y1 = ay1; any = true; return; }
        x0 = std::min(x0, ax0); x1 = std::max(x1, ax1);
        y0 = std::min(y0, ay0); y1 = std::max(y1, ay1);
    }
};

struct InvariantStats {
    uint64_t splices_seen = 0;       // splice-frames inspected
    uint64_t compared = 0;           // with both real and replacement footprints
    uint64_t violations = 0;
    double worst_anchor = 0;         // coarse tiles: gap between replacement and real boxes
    uint64_t shared_prefix_splices = 0; // real/replacement start with identical (shadow) entries
    uint64_t shared_prefix_entries = 0;
    uint64_t identical_splices = 0;     // replacement bit-identical to the real entries: patch had no effect
    uint64_t coarser = 0;            // replacement tiles larger than the real ones
    uint64_t grown_entries = 0;      // replacement entries beyond the real count
    uint64_t replacement_entries = 0, real_entries = 0;
    std::string first_violation, worst_anchor_detail;
};

struct Args {
    std::filesystem::path rom_dir;
    std::string set;
    uint64_t frames = 6000;
    uint64_t compare_every = 60;
    uint64_t seed = 12345;
    bool inputs = true;
    std::vector<std::string> behaviours;
};

enum Key { UP, DOWN, LEFT, RIGHT, BTN1, BTN2, BTN3, COIN, START };

void apply_key(Machine &m, Key key, bool pressed) {
    switch (key) {
    case UP: m.set_input(1, 1, pressed); break;
    case DOWN: m.set_input(1, 2, pressed); break;
    case LEFT: m.set_input(1, 4, pressed); break;
    case RIGHT: m.set_input(1, 8, pressed); break;
    case BTN1: m.set_input(0, 1, pressed); break;
    case BTN2: m.set_input(0, 2, pressed); break;
    case BTN3: m.set_input(0, 4, pressed); break;
    case START: m.set_input(0, 0x1000, pressed); break;
    case COIN:
        if (pressed) m.system_inputs &= ~0x10u; else m.system_inputs |= 0x10u;
        break;
    }
}

// Same schedule as tools/gameplay_regression.cpp (attract, coin, start pulses, then seeded
// button mashing), applied identically to both machines.
struct Inputs {
    uint64_t rng;
    explicit Inputs(uint64_t seed) : rng(seed) {}
    template<class F> void step(uint64_t f, F &&apply) {
        constexpr uint32_t coin = 700, coin_len = 20, start_begin = 800, start_end = 2400, period = 90,
                           pulse = 5, mash_begin = 1200, mash_period = 6;
        static constexpr Key keys[] = {UP, DOWN, LEFT, RIGHT, BTN1, BTN2, BTN3};
        if (f == coin) apply(COIN, true);
        if (f == coin + coin_len) apply(COIN, false);
        if (f >= start_begin && f < start_end && f % period == 0) apply(START, true);
        if (f >= start_begin && f < start_end && f % period == pulse) apply(START, false);
        if (f >= mash_begin && f % mash_period == 0) {
            rng = rng * 6364136223846793005ULL + 1442695040888963407ULL;
            apply(keys[(rng >> 33) % 7], ((rng >> 20) & 1) != 0);
        }
    }
};

std::unique_ptr<Machine> make_machine(const Args &args) {
    auto machine = std::make_unique<Machine>(RomSet::load(args.rom_dir, args.set));
    machine->allow_main_fallback = false;
#ifdef F3RT_GENERATED
    if (!f3_generated_register(&machine->cpu))
        throw std::runtime_error("f3_generated_register failed to register recompiled blocks");
#else
    throw std::runtime_error("sprite-check requires compiled generated blocks (F3RT_GENERATED)");
#endif
    return machine;
}

void drain_audio(Machine &m) {
    std::array<int16_t, 8192> buffer;
    while (m.audio->render(buffer.data(), buffer.size() / 2) != 0) {}
}

bool run_one(Machine &m, uint64_t frame, const char *label, std::string &error) {
    bool ok = false;
    try {
        ok = m.run_frame(true);
    } catch (const std::exception &e) {
        error = std::string(label) + ": exception at frame " + std::to_string(frame) + ": " + e.what();
        return false;
    }
    if (!ok || m.cpu.halted) {
        std::ostringstream ss;
        ss << label << ": CPU halted at frame " << frame << " pc=0x" << std::hex << m.cpu.pc;
        error = ss.str();
        return false;
    }
    if (m.fallback_instructions) {
        error = std::string(label) + ": fallback instruction executed under strict native mode at frame " +
                std::to_string(frame);
        return false;
    }
    drain_audio(m);
    return true;
}

// `state` is the latched hardware state (bank etc.), carried across frames like the renderer does.
std::vector<DecodedSpriteEntry> decode(const Machine &m, const SpritePresentation &presentation, bool splices,
                                       SpriteRamState &state) {
    std::vector<DecodedSpriteEntry> out(0x800);
    SpritePresentation view{presentation.identity,
                            splices ? presentation.splices : std::span<const SpriteSplice>{},
                            presentation.object};
    const size_t n = decode_sprite_list(m.graphics.data(), int(m.roms.video.visible_y),
                                        int(m.roms.video.visible_height), out, state, false, &view);
    out.resize(n);
    return out;
}

void check_full_detail(const Machine &m, const SpriteUnits &units, InvariantStats &stats, uint64_t frame,
                       SpriteRamState &latch) {
    const SpritePresentation presentation = units.presentation();
    SpriteRamState spliced_state = latch;
    const auto real = decode(m, presentation, false, latch);
    if (presentation.splices.empty()) return;
    const auto spliced = decode(m, presentation, true, spliced_state);
    for (const SpriteSplice &splice : presentation.splices) {
        ++stats.splices_seen;
        const size_t count = splice.replacement.size() / 16;
        {
            size_t shared = 0;
            const size_t limit = std::min(count, splice.real.size() / 16);
            while (shared < limit && std::memcmp(&splice.real[shared * 16], &splice.replacement[shared * 16], 16) == 0)
                ++shared;
            if (shared) { ++stats.shared_prefix_splices; stats.shared_prefix_entries += shared; }
            if (shared == count && count == splice.real.size() / 16) {
                ++stats.identical_splices;
                if (stats.first_violation.empty())
                    stats.first_violation = "frame " + std::to_string(frame) +
                                            ": splice replacement is bit-identical to the real entries";
            }
        }
        std::set<uint64_t> ids;
        for (size_t k = 0; k < count; ++k) ids.insert(sprite_identity_mix(splice.identity, uint32_t(k)));
        Box before, after;
        uint64_t before_n = 0, after_n = 0;
        for (const auto &e : real) if (ids.count(e.identity)) { before.add(e); ++before_n; }
        for (const auto &e : spliced) if (ids.count(e.identity)) { after.add(e); ++after_n; }
        stats.real_entries += before_n;
        stats.replacement_entries += after_n;
        if (after_n > before_n) stats.grown_entries += after_n - before_n;
        if (!before.any || !after.any) continue;
        ++stats.compared;
        // Distance between the two boxes' intervals along an axis (0 when they overlap).
        const auto gap = [](int32_t a0, int32_t a1, int32_t b0, int32_t b1) {
            return std::max({b0 - a1, a0 - b1, 0});
        };
        const double anchor = std::max(
            double(gap(before.x0, before.x1, after.x0, after.x1)) / std::max(before.tile_w, 1),
            double(gap(before.y0, before.y1, after.y0, after.y1)) / std::max(before.tile_h, 1));
        const bool coarser = after.tile_w > before.tile_w + SCALE_STEP || after.tile_h > before.tile_h + SCALE_STEP;
        const auto describe = [&]() {
            std::ostringstream ss;
            ss << "frame " << frame << " splice bank" << int(splice.bank) << " entries " << splice.first << ".."
               << splice.last << " anchor_gap=" << anchor << " coarser=" << coarser << " real bbox (" << before.x0 / 256.0
               << "," << before.y0 / 256.0 << ")-(" << before.x1 / 256.0 << "," << before.y1 / 256.0
               << ") replacement bbox (" << after.x0 / 256.0 << "," << after.y0 / 256.0 << ")-("
               << after.x1 / 256.0 << "," << after.y1 / 256.0 << ")";
            const auto list = [&](const char *label, const std::vector<DecodedSpriteEntry> &entries) {
                ss << "\n    " << label << ':';
                for (const auto &e : entries)
                    if (ids.count(e.identity))
                        ss << "\n      tile=0x" << std::hex << e.tile << std::dec << " x=" << e.x / 256.0
                           << " y=" << e.y / 256.0 << " size=" << e.scale_x * 16 / 256.0 << "x"
                           << e.scale_y * 16 / 256.0;
            };
            list("real", real);
            list("replacement", spliced);
            return ss.str();
        };
        if (anchor >= stats.worst_anchor && anchor > 0) stats.worst_anchor_detail = describe();
        stats.worst_anchor = std::max(stats.worst_anchor, anchor);
        stats.coarser += coarser;
        if (anchor > ANCHOR_TILES || coarser) {
            ++stats.violations;
            if (stats.first_violation.empty()) stats.first_violation = describe();
        }
    }
}

int run(const Args &args) {
    const auto registered = registered_sprite_behaviours();
    SpriteUnits::Options options;
    options.check = true;
    for (const std::string &name : args.behaviours) {
        const SpriteBehaviour *found = nullptr;
        for (const SpriteBehaviour *candidate : registered) if (name == candidate->name) found = candidate;
        if (!found) {
            std::string message = "unknown --behaviour '" + name + "'; registered:";
            for (const SpriteBehaviour *candidate : registered) message += std::string(" ") + candidate->name;
            if (registered.empty()) message += " (none)";
            throw std::runtime_error(message);
        }
        options.behaviours.push_back(found);
    }
    const bool full_detail = std::find(args.behaviours.begin(), args.behaviours.end(), "full-detail") !=
                             args.behaviours.end();

    auto on = make_machine(args);
    auto off = make_machine(args);
    on->sprite_units = std::make_unique<SpriteUnits>(*on, SpriteUnits::game_table(), options);
    if (!on->sprite_units->active()) throw std::runtime_error("game declares no emit units");

    Inputs inputs_on(args.seed), inputs_off(args.seed);
    InvariantStats invariants;
    std::string error;
    uint64_t state_compares = 0;
    SpriteRamState latch;
    const auto diverged = [&](uint64_t frame, const char *what) {
        std::ostringstream ss;
        ss << "units on/off diverged at frame " << frame << " (" << what << "): on cycles=" << on->cpu.cycles
           << " native_blocks=" << on->native_blocks << " sync_crc=0x" << std::hex << on->sync_state_crc()
           << " state_crc=0x" << on->state_crc() << "; off cycles=" << std::dec << off->cpu.cycles
           << " native_blocks=" << off->native_blocks << " sync_crc=0x" << std::hex << off->sync_state_crc()
           << " state_crc=0x" << off->state_crc();
        error = ss.str();
    };

    while (on->frame < args.frames) {
        const uint64_t frame = on->frame;
        if (args.inputs) {
            inputs_on.step(frame, [&](Key k, bool p) { apply_key(*on, k, p); });
            inputs_off.step(frame, [&](Key k, bool p) { apply_key(*off, k, p); });
        }
        if (!run_one(*on, frame, "units on", error) || !run_one(*off, frame, "units off", error)) break;
        if (on->cpu.cycles != off->cpu.cycles) { diverged(frame, "cycles"); break; }
        if (on->native_blocks != off->native_blocks) { diverged(frame, "native_blocks"); break; }
        if (on->frame % args.compare_every == 0) {
            ++state_compares;
            if (on->sync_state_crc() != off->sync_state_crc()) { diverged(frame, "sync_state_crc"); break; }
            if (on->state_crc() != off->state_crc()) { diverged(frame, "state_crc"); break; }
        }
        if (full_detail) check_full_detail(*on, *on->sprite_units, invariants, frame, latch);
    }
    if (error.empty()) {
        ++state_compares;
        if (on->sync_state_crc() != off->sync_state_crc()) diverged(on->frame, "sync_state_crc final");
        else if (on->state_crc() != off->state_crc()) diverged(on->frame, "state_crc final");
    }

    const SpriteUnits &units = *on->sprite_units;
    units.write_report(std::cout);
    const auto report = units.report();
    bool failed = !error.empty();
    if (!error.empty()) std::cout << "FAIL: " << error << '\n';
    if (!units.passed()) { failed = true; std::cout << "FAIL: unit replay/frame-writer check\n"; }
    if (report.invocations == 0) { failed = true; std::cout << "FAIL: no unit invocations observed\n"; }
    if (!options.behaviours.empty()) {
        std::cout << "behaviours:";
        for (const SpriteBehaviour *b : options.behaviours) std::cout << ' ' << b->name;
        std::cout << "\n  patched replays/splices: spliced=" << report.spliced << " aborted=" << report.aborted << '\n';
        if (report.spliced == 0) { failed = true; std::cout << "FAIL: behaviours enabled but no splice produced\n"; }
    }
    if (full_detail) {
        std::cout << "  full-detail: splice-frames=" << invariants.splices_seen << " compared=" << invariants.compared
                  << " real_entries=" << invariants.real_entries << " replacement_entries="
                  << invariants.replacement_entries << " " << std::fixed
                  << std::setprecision(2) << "worst_anchor_gap=" << invariants.worst_anchor
                  << " coarse tiles (tolerance " << ANCHOR_TILES << ") coarser=" << invariants.coarser
                  << " identical_splices=" << invariants.identical_splices << " shadow_prefix_splices="
                  << invariants.shared_prefix_splices << " shadow_prefix_entries="
                  << invariants.shared_prefix_entries << " violations=" << invariants.violations << '\n';
        if (!invariants.worst_anchor_detail.empty()) std::cout << "  worst anchor gap: " << invariants.worst_anchor_detail << '\n';
        if (invariants.violations || invariants.identical_splices) {
            failed = true;
            std::cout << "FAIL: behaviour invariant: " << invariants.first_violation << '\n';
        }
        if (invariants.compared == 0) { failed = true; std::cout << "FAIL: no splice footprints compared\n"; }
    }
    std::cout << std::dec << "frames=" << on->frame << " cycles=" << on->cpu.cycles << " native_blocks="
              << on->native_blocks << " state_compares=" << state_compares << " set=" << args.set << '\n'
              << (failed ? "FAIL" : "PASS") << '\n';
    return failed ? 1 : 0;
}

} // namespace

int main(int argc, char **argv) try {
    Args args;
#ifdef F3RT_DEFAULT_ROM_DIR
    args.rom_dir = F3RT_DEFAULT_ROM_DIR;
#endif
#ifdef F3RT_DEFAULT_SET
    args.set = F3RT_DEFAULT_SET;
#endif
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto value = [&]() -> const char * {
            if (i + 1 >= argc) throw std::runtime_error("Missing value for argument: " + arg);
            return argv[++i];
        };
        if (arg == "--rom-dir") args.rom_dir = value();
        else if (arg == "--set") args.set = value();
        else if (arg == "--frames") args.frames = std::stoull(value());
        else if (arg == "--compare-every") args.compare_every = std::stoull(value());
        else if (arg == "--seed") args.seed = std::stoull(value());
        else if (arg == "--no-inputs") args.inputs = false;
        else if (arg == "--behaviour") args.behaviours.push_back(value());
        else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: " << argv[0] << " [--rom-dir DIR] [--set SET] [--frames N] [--compare-every N]\n"
                         "       [--seed N] [--no-inputs] [--behaviour NAME]...\n"
                         "Exits nonzero unless every unit replay is bit-exact, every sprite write is accounted\n"
                         "for, and emulated state is identical with sprite units on and off.\n";
            return 0;
        } else throw std::runtime_error("Unknown argument: " + arg);
    }
    if (args.rom_dir.empty()) throw std::runtime_error("ROM directory must be specified via --rom-dir");
    if (args.set.empty()) throw std::runtime_error("--set is required");
    if (!args.frames || !args.compare_every) throw std::runtime_error("--frames and --compare-every must be positive");
    return run(args);
} catch (const std::exception &e) {
    std::cerr << "sprite-check error: " << e.what() << '\n';
    return 2;
}
