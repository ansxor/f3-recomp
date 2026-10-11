#include "sprite_check.hpp"
#include "runner.hpp"
#include "inputs.hpp"
#include "f3rt/machine.hpp"
#include "f3rt/audio.hpp"
#include "renderer/decode.hpp"
#include "sprites/behaviour.hpp"
#include "sprites/units.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>

namespace f3rt::runner {

namespace {

constexpr int32_t SCALE_STEP = 16;
constexpr double ANCHOR_TILES = 0.0;

struct Box {
    bool any = false;
    int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    int32_t tile_w = 0, tile_h = 0;
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
    uint64_t splices_seen = 0;
    uint64_t compared = 0;
    uint64_t violations = 0;
    double worst_anchor = 0;
    uint64_t shared_prefix_splices = 0;
    uint64_t shared_prefix_entries = 0;
    uint64_t identical_splices = 0;
    uint64_t coarser = 0;
    uint64_t grown_entries = 0;
    uint64_t replacement_entries = 0, real_entries = 0;
    std::string first_violation, worst_anchor_detail;
};

struct FlickerFrame {
    uint32_t entries = 0, objects = 0, gate_set = 0;
};

struct FlickerInvariants {
    uint64_t frames = 0;
    uint64_t frames_with_shadow = 0;
    uint64_t game_emitted_frames = 0;
    uint64_t synthesized_frames = 0;
    uint64_t attract_frames_with_shadow = 0;
    uint64_t decoded_tagged = 0, splice_tagged = 0;
    uint64_t real_decode_tagged = 0;
    uint64_t violations = 0;
    std::string first_violation;
    std::vector<FlickerFrame> per_frame;
};

enum Key { UP, DOWN, LEFT, RIGHT, BTN1, BTN2, BTN3, COIN, START };

void apply_key(Machine &m, Key key, bool pressed) {
    switch (key) {
    case UP:    m.set_input(1, 1, pressed); break;
    case DOWN:  m.set_input(1, 2, pressed); break;
    case LEFT:  m.set_input(1, 4, pressed); break;
    case RIGHT: m.set_input(1, 8, pressed); break;
    case BTN1:  m.set_input(0, 1, pressed); break;
    case BTN2:  m.set_input(0, 2, pressed); break;
    case BTN3:  m.set_input(0, 4, pressed); break;
    case START: m.set_input(0, 0x1000, pressed); break;
    case COIN:
        if (pressed) m.system_inputs &= ~0x10u; else m.system_inputs |= 0x10u;
        break;
    }
}

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

std::vector<DecodedSpriteEntry> decode(const Machine &m, const SpritePresentation &presentation, bool splices,
                                       SpriteRamState &state) {
    std::vector<DecodedSpriteEntry> out(0x800);
    SpritePresentation view{presentation.identity,
                            splices ? presentation.splices : std::span<const SpriteSplice>{},
                            presentation.object, presentation.flags};
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
            const bool flicker_splice = std::any_of(splice.flags.begin(), splice.flags.end(),
                                                    [](uint8_t f) { return (f & sprite_flag_shadow) != 0; });
            if (shared == count && count == splice.real.size() / 16 && !flicker_splice) {
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
        for (const auto &e : real)
            if (ids.count(e.identity) && !(e.flags & sprite_flag_shadow)) { before.add(e); ++before_n; }
        for (const auto &e : spliced)
            if (ids.count(e.identity) && !(e.flags & sprite_flag_shadow)) { after.add(e); ++after_n; }
        stats.real_entries += before_n;
        stats.replacement_entries += after_n;
        if (after_n > before_n) stats.grown_entries += after_n - before_n;
        if (!before.any || !after.any) continue;
        ++stats.compared;
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

void check_flicker_frame(const Machine &m, const SpriteUnits &units, FlickerInvariants &stats, uint64_t frame,
                         SpriteRamState &latch, uint64_t attract_end) {
    const SpritePresentation presentation = units.presentation();
    const SpriteUnits::FrameShadow shadow = units.last_frame_shadow();
    stats.per_frame.push_back({shadow.entries, shadow.objects, shadow.gate_set_objects});
    ++stats.frames;
    const auto fail = [&](const std::string &what) {
        ++stats.violations;
        if (stats.first_violation.empty()) stats.first_violation = "frame " + std::to_string(frame) + ": " + what;
    };
    SpriteRamState spliced_state = latch;
    const auto real = decode(m, presentation, false, latch);
    uint64_t real_tagged = 0;
    for (const auto &e : real) real_tagged += (e.flags & sprite_flag_shadow) != 0;
    stats.real_decode_tagged += real_tagged;
    uint64_t carried = 0, decoded = 0;
    if (!presentation.splices.empty()) {
        const auto spliced = decode(m, presentation, true, spliced_state);
        for (const auto &e : spliced) decoded += (e.flags & sprite_flag_shadow) != 0;
        for (const SpriteSplice &splice : presentation.splices) {
            if (!splice.flags.empty() && splice.flags.size() != splice.replacement.size() / 16)
                fail("splice flags are not parallel to the replacement entries");
            for (uint8_t flag : splice.flags) carried += (flag & sprite_flag_shadow) != 0;
        }
    }
    stats.decoded_tagged += decoded;
    stats.splice_tagged += carried;
    if (decoded > carried + real_tagged) fail("decoded more tagged entries than the splices and real list carry");
    if (shadow.entries) {
        ++stats.frames_with_shadow;
        ++(shadow.gate_set_objects ? stats.game_emitted_frames : stats.synthesized_frames);
        if (frame < attract_end) ++stats.attract_frames_with_shadow;
        if (!carried) fail("shadowed objects were detected but the presentation carries no tagged entry");
    }
}

} // namespace

SpriteCheckResult run_sprite_check(const SpriteCheckArgs &args, std::ostream *out) {
    SpriteCheckResult result;
    const auto registered = registered_sprite_behaviours();
    SpriteUnits::Options options;
    options.check = true;
    for (const std::string &name : args.behaviours) {
        const SpriteBehaviour *found = nullptr;
        for (const SpriteBehaviour *candidate : registered) {
            if (name == candidate->name) found = candidate;
        }
        if (!found) {
            std::string message = "unknown --behaviour '" + name + "'; registered:";
            for (const SpriteBehaviour *candidate : registered) message += std::string(" ") + candidate->name;
            if (registered.empty()) message += " (none)";
            throw std::runtime_error(message);
        }
        options.behaviours.push_back(found);
    }
    if (args.flicker) {
        const auto sources = registered_flicker_shadows();
        options.flicker_shadows.assign(sources.begin(), sources.end());
    }
    const bool full_detail = std::find(args.behaviours.begin(), args.behaviours.end(), "full-detail") !=
                             args.behaviours.end();

    auto on = create_strict_machine(args.rom_dir, args.set, false);
    auto off = create_strict_machine(args.rom_dir, args.set, false);

    on->sprite_units = std::make_unique<SpriteUnits>(*on, SpriteUnits::game_table(), options);
    if (!on->sprite_units->active()) {
        result.error = "game declares no emit units";
        return result;
    }
    on->sprite_units->set_flicker_shadows(args.flicker);
    const bool flicker = on->sprite_units->flicker_shadows_enabled();
    FlickerInvariants shadows;
    constexpr uint64_t attract_end = 700;

    Inputs inputs_on(args.seed), inputs_off(args.seed);
    InvariantStats invariants;
    std::string error;
    uint64_t state_compares = 0;
    SpriteRamState latch, flicker_latch;

    const auto diverged = [&](uint64_t frame, const char *what) {
        std::ostringstream ss;
        ss << "units on/off diverged at frame " << frame << " (" << what << "): on cycles=" << on->cpu.cycles
           << " native_blocks=" << on->native_blocks << std::hex
           << " state_crc=0x" << on->state_crc() << "; off cycles=" << std::dec << off->cpu.cycles
           << " native_blocks=" << off->native_blocks << std::hex
           << " state_crc=0x" << off->state_crc();
        error = ss.str();
    };

    for (uint64_t frame = 0; frame < args.frames; ++frame) {
        if (args.inputs) {
            inputs_on.step(frame, [&](Key k, bool p) { apply_key(*on, k, p); });
            inputs_off.step(frame, [&](Key k, bool p) { apply_key(*off, k, p); });
        }
        if (!run_one(*on, frame, "units on", error) || !run_one(*off, frame, "units off", error)) {
            break;
        }
        if (full_detail) check_full_detail(*on, *on->sprite_units, invariants, frame, latch);
        if (flicker) check_flicker_frame(*on, *on->sprite_units, shadows, frame, flicker_latch, attract_end);

        if ((frame + 1) % args.compare_every == 0 || frame + 1 == args.frames) {
            ++state_compares;
            if (on->cpu.cycles != off->cpu.cycles) { diverged(frame, "cycles"); break; }
            if (on->native_blocks != off->native_blocks) { diverged(frame, "native_blocks"); break; }
            if (on->state_crc() != off->state_crc()) { diverged(frame, "state_crc"); break; }
        }
    }

    const SpriteUnits &units = *on->sprite_units;
    if (out) units.write_report(*out);
    const auto rep = units.report();

    bool failed = !error.empty();
    if (!error.empty() && out) *out << "FAIL: " << error << '\n';
    if (!units.passed()) { failed = true; if (out) *out << "FAIL: unit replay/frame-writer check\n"; }
    if (rep.invocations == 0) { failed = true; if (out) *out << "FAIL: no unit invocations observed\n"; }
    if (!options.behaviours.empty()) {
        if (out) {
            *out << "behaviours:";
            for (const SpriteBehaviour *b : options.behaviours) *out << ' ' << b->name;
            *out << "\n  patched replays/splices: spliced=" << rep.spliced << " aborted=" << rep.aborted << '\n';
        }
        if (rep.spliced == 0) { failed = true; if (out) *out << "FAIL: behaviours enabled but no splice produced\n"; }
    }
    if (full_detail) {
        if (out) {
            *out << "  full-detail: splice-frames=" << invariants.splices_seen << " compared=" << invariants.compared
                 << " real_entries=" << invariants.real_entries << " replacement_entries="
                 << invariants.replacement_entries << " " << std::fixed
                 << std::setprecision(2) << "worst_anchor_gap=" << invariants.worst_anchor
                 << " coarse tiles (tolerance " << ANCHOR_TILES << ") coarser=" << invariants.coarser
                 << " identical_splices=" << invariants.identical_splices << " shadow_prefix_splices="
                 << invariants.shared_prefix_splices << " shadow_prefix_entries="
                 << invariants.shared_prefix_entries << " violations=" << invariants.violations << '\n';
            if (!invariants.worst_anchor_detail.empty()) *out << "  worst anchor gap: " << invariants.worst_anchor_detail << '\n';
        }
        if (invariants.violations || invariants.identical_splices) {
            failed = true;
            if (out) *out << "FAIL: behaviour invariant: " << invariants.first_violation << '\n';
        }
        if (invariants.compared == 0) { failed = true; if (out) *out << "FAIL: no splice footprints compared\n"; }
    }
    if (flicker) {
        const auto &f = rep.flicker;
        if (out) {
            *out << "  flicker-shadow: frames=" << shadows.frames << " frames_with_shadow=" << shadows.frames_with_shadow
                 << " game_emitted=" << shadows.game_emitted_frames << " synthesized_only=" << shadows.synthesized_frames
                 << " attract_frames_with_shadow=" << shadows.attract_frames_with_shadow
                 << " tagged_entries(splice/decoded)=" << shadows.splice_tagged << '/' << shadows.decoded_tagged
                 << " real_tagged_decoded=" << shadows.real_decode_tagged
                 << " real_entries=" << f.real_entries << " violations=" << shadows.violations + f.violations << '\n';
            if (!shadows.first_violation.empty()) *out << "  first violation: " << shadows.first_violation << '\n';
        }
        if (shadows.violations || f.violations) {
            failed = true;
            if (out) *out << "FAIL: flicker-shadow invariant\n";
        }
        if (shadows.frames_with_shadow == 0 || f.invocations == 0) {
            failed = true;
            if (out) *out << "FAIL: no flicker shadow detected\n";
        }
        if (shadows.attract_frames_with_shadow == 0 || f.gate_clear == 0) {
            failed = true;
            if (out) *out << "FAIL: no flicker shadow detected in attract mode\n";
        }
        if (!shadows.game_emitted_frames || !shadows.synthesized_frames) {
            failed = true;
            if (out) *out << "FAIL: shadows seen on frames of one game parity only\n";
        }
        if (args.shadow_trace && out) {
            size_t i = 0;
            while (i < shadows.per_frame.size() && !shadows.per_frame[i].entries) ++i;
            *out << "  shadow trace (frame entries objects game_emitted_objects):\n";
            for (size_t n = 0; n < args.shadow_trace && i < shadows.per_frame.size(); ++n, ++i) {
                *out << "    " << i + 1 << ' ' << shadows.per_frame[i].entries << ' ' << shadows.per_frame[i].objects << ' '
                     << shadows.per_frame[i].gate_set << '\n';
            }
        }
    }

    if (out) {
        *out << std::dec << "frames=" << on->frame << " cycles=" << on->cpu.cycles << " native_blocks="
             << on->native_blocks << " state_compares=" << state_compares << " set=" << args.set << '\n'
             << (failed ? "FAIL" : "PASS") << '\n';
    }

    result.passed = !failed;
    result.frames = on->frame;
    result.cycles = on->cpu.cycles;
    result.native_blocks = on->native_blocks;
    result.state_compares = state_compares;
    result.invocations = rep.invocations;
    result.unaccounted_writes = rep.stray_unaccounted;
    result.mismatches = rep.mismatches.size();
    result.aborted = rep.aborted;
    result.violations = invariants.violations + shadows.violations;
    result.first_violation = !invariants.first_violation.empty() ? invariants.first_violation : shadows.first_violation;
    result.error = error;
    return result;
}

} // namespace f3rt::runner
