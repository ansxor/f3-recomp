#include "sprite_check.hpp"
#include "runner.hpp"
#include "inputs.hpp"
#include "f3rt/abi/audio.h"
#include "f3rt/machine.hpp"
#include "renderer/decode.hpp"
#include "renderer/game/scene.hpp"
#include "sprites/behaviour.hpp"
#include "sprites/sprite_system.hpp"

#include <array>
#include <algorithm>
#include <cstring>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

namespace f3rt::runner {

namespace {
enum Key { UP, DOWN, LEFT, RIGHT, BTN1, BTN2, BTN3, COIN, START };

void apply_key(Machine &m, Key key, bool pressed) {
    switch (key) {
    case UP:
        m.set_input(1, 1, pressed);
        break;
    case DOWN:
        m.set_input(1, 2, pressed);
        break;
    case LEFT:
        m.set_input(1, 4, pressed);
        break;
    case RIGHT:
        m.set_input(1, 8, pressed);
        break;
    case BTN1:
        m.set_input(0, 1, pressed);
        break;
    case BTN2:
        m.set_input(0, 2, pressed);
        break;
    case BTN3:
        m.set_input(0, 4, pressed);
        break;
    case START:
        m.set_input(0, 0x1000, pressed);
        break;
    case COIN:
        if (pressed) m.system_inputs &= ~0x10u;
        else m.system_inputs |= 0x10u;
        break;
    }
}

struct Inputs {
    uint64_t rng;
    explicit Inputs(uint64_t seed) : rng(seed) {}
    template <class F> void step(uint64_t f, F &&apply) {
        constexpr uint32_t COIN_FRAME = 700, COIN_LEN = 20, START_BEGIN = 800, START_END = 2400,
                           PERIOD = 90, PULSE = 5, MASH_BEGIN = 1200, MASH_PERIOD = 6;
        static constexpr Key KEYS[] = {UP, DOWN, LEFT, RIGHT, BTN1, BTN2, BTN3};
        if (f == COIN_FRAME) apply(COIN, true);
        if (f == COIN_FRAME + COIN_LEN) apply(COIN, false);
        if (f >= START_BEGIN && f < START_END && f % PERIOD == 0) apply(START, true);
        if (f >= START_BEGIN && f < START_END && f % PERIOD == PULSE) apply(START, false);
        if (f >= MASH_BEGIN && f % MASH_PERIOD == 0) {
            rng = rng * 6364136223846793005ULL + 1442695040888963407ULL;
            apply(KEYS[(rng >> 33) % 7], ((rng >> 20) & 1) != 0);
        }
    }
};

void drain_audio() {
    std::array<int16_t, 8192> buffer;
    while (f3rt_audio_render(buffer.data(), buffer.size() / 2) != 0) {
    }
}

bool run_one(Machine &m, uint64_t frame, const char *label, std::string &error) {
    bool ok = false;
    try {
        ok = m.run_frame();
    } catch (const std::exception &e) {
        error =
            std::string(label) + ": exception at frame " + std::to_string(frame) + ": " + e.what();
        return false;
    }
    if (!ok || m.cpu.halted) {
        std::ostringstream ss;
        ss << label << ": CPU halted at frame " << frame << " pc=0x" << std::hex << m.cpu.pc;
        error = ss.str();
        return false;
    }
    drain_audio();
    return true;
}

struct FrameCoverage {
    uint64_t groups = 0, entries = 0, unresolved = 0;
};

FrameCoverage cover(const Machine &m, const SpritePresentation &presentation,
                    SpriteRamState &state) {
    std::array<DecodedSpriteEntry, MAX_HARDWARE_SPRITES> decoded;
    const size_t n = decode_sprite_list(m.graphics.data(), int(m.roms.video.visible_y),
                                        int(m.roms.video.visible_height), decoded, state, false,
                                        &presentation);
    std::unordered_set<uint64_t> keys;
    FrameCoverage coverage;
    for (size_t i = 0; i < n; ++i) {
        if (!decoded[i].group) {
            ++coverage.unresolved;
            continue;
        }
        ++coverage.entries;
        keys.insert(decoded[i].group);
    }
    coverage.groups = keys.size();
    return coverage;
}

struct OffCheckpoint {
    uint64_t frame = 0;
    uint64_t cycles = 0;
    uint64_t native_blocks = 0;
    uint32_t state_crc = 0;
};

} // namespace

SpriteCheckResult run_sprite_check(const SpriteCheckArgs &args, std::ostream *out) {
    SpriteCheckResult result;
    SpriteSystem::Options options;
    const auto registered = registered_sprite_behaviours();
    for (const std::string &name : args.behaviours) {
        const SpriteBehaviour *found = nullptr;
        for (const SpriteBehaviour *candidate : registered)
            if (name == candidate->name) found = candidate;
        if (!found) {
            std::string message = "unknown --behaviour '" + name + "'; registered:";
            for (const SpriteBehaviour *candidate : registered)
                message += std::string(" ") + candidate->name;
            if (registered.empty()) message += " (none)";
            throw std::runtime_error(message);
        }
        options.behaviours.push_back(found);
    }

    if (SpriteSystem::game_table().writes.empty() &&
        (SpriteSystem::game_units().units.empty() || options.behaviours.empty())) {
        result.error = "game declares no sprite groups and no behaviour anchor";
        if (out) *out << "FAIL: " << result.error << '\n';
        return result;
    }

    std::string error;
    std::vector<OffCheckpoint> off_checkpoints;
    off_checkpoints.reserve(args.frames / args.compare_every + 1);
    {
        auto off = create_strict_machine(args.rom_dir, args.set, false);
        Inputs inputs_off(args.seed);
        for (uint64_t frame = 0; frame < args.frames; ++frame) {
            if (args.inputs) {
                inputs_off.step(frame, [&](Key k, bool p) { apply_key(*off, k, p); });
            }
            if (!run_one(*off, frame, "groups off", error)) break;
            if ((frame + 1) % args.compare_every == 0 || frame + 1 == args.frames) {
                off_checkpoints.push_back(
                    {frame, off->cpu.cycles, off->native_blocks, off->state_crc()});
            }
        }
    }

    auto on = create_strict_machine(args.rom_dir, args.set, false);
    on->sprites = std::make_unique<SpriteSystem>(*on, SpriteSystem::game_table(),
                                                 SpriteSystem::game_units(), options);
    const bool groups_available = on->sprites->groups_active();

    Inputs inputs_on(args.seed);
    uint64_t state_compares = 0, group_frames = 0, max_groups = 0;
    FrameCoverage total;
    SpriteRamState latch;
    size_t checkpoint_index = 0;

    const auto diverged = [&](uint64_t frame, const char *what, const OffCheckpoint &off) {
        std::ostringstream ss;
        ss << "groups on/off diverged at frame " << frame << " (" << what
           << "): on cycles=" << on->cpu.cycles << " native_blocks=" << on->native_blocks
           << std::hex << " state_crc=0x" << on->state_crc() << "; off cycles=" << std::dec
           << off.cycles << " native_blocks=" << off.native_blocks << std::hex << " state_crc=0x"
           << off.state_crc;
        error = ss.str();
    };

    for (uint64_t frame = 0; frame < args.frames && error.empty(); ++frame) {
        if (args.inputs) {
            inputs_on.step(frame, [&](Key k, bool p) { apply_key(*on, k, p); });
        }
        if (!run_one(*on, frame, "groups on", error)) break;
        if (groups_available) {
            // Render-only view of this frame; GameVideo would consume it the same way.
            const SpritePresentation presentation = on->sprites->presentation();
            SpriteRamState presented_state = latch;
            const FrameCoverage coverage = cover(*on, presentation, presented_state);
            latch = presented_state;
            total.groups += coverage.groups;
            total.entries += coverage.entries;
            total.unresolved += coverage.unresolved;
            if (coverage.groups) ++group_frames;
            max_groups = std::max(max_groups, coverage.groups);
        }
        if ((frame + 1) % args.compare_every == 0 || frame + 1 == args.frames) {
            const OffCheckpoint &off = off_checkpoints[checkpoint_index++];
            ++state_compares;
            if (on->cpu.cycles != off.cycles) {
                diverged(frame, "cycles", off);
                break;
            }
            if (on->native_blocks != off.native_blocks) {
                diverged(frame, "native_blocks", off);
                break;
            }
            if (on->state_crc() != off.state_crc) {
                diverged(frame, "state_crc", off);
                break;
            }
        }
    }

    const auto report = on->sprites->report();
    if (out) on->sprites->write_report(*out);

    bool failed = !error.empty();
    if (!error.empty() && out) *out << "FAIL: " << error << '\n';
    if (groups_available && total.entries == 0) {
        failed = true;
        if (out) *out << "FAIL: no sprite group resolved in any frame\n";
    }
    if (!failed && !options.behaviours.empty() && report.spliced == 0) {
        failed = true;
        if (out) *out << "FAIL: behaviours enabled but no splice produced\n";
    }
    if (out) {
        *out << "  group coverage: frames_with_groups=" << group_frames
             << " mean_groups_all_frames="
             << (args.frames ? double(total.groups) / double(args.frames) : 0.0)
             << " mean_groups_active_frames="
             << (group_frames ? double(total.groups) / double(group_frames) : 0.0)
             << " max_groups_frame=" << max_groups << " grouped_entries=" << total.entries
             << " unresolved_entries=" << total.unresolved << '\n';
    }
    result.passed = !failed;
    result.frames = on->frame;
    result.cycles = on->cpu.cycles;
    result.native_blocks = on->native_blocks;
    result.state_compares = state_compares;
    result.groups = max_groups;
    result.group_frames = group_frames;
    result.group_entries = total.entries;
    result.unresolved_entries = total.unresolved;
    result.max_groups_frame = max_groups;
    result.invocations = report.invocations;
    result.spliced = report.spliced;
    result.aborted = report.aborted;
    result.error = error;
    if (out) {
        *out << std::dec << "frames=" << on->frame << " cycles=" << on->cpu.cycles
             << " native_blocks=" << on->native_blocks << " state_compares=" << state_compares
             << " set=" << args.set << '\n'
             << (failed ? "FAIL" : "PASS") << '\n';
    }
    return result;
}

} // namespace f3rt::runner
