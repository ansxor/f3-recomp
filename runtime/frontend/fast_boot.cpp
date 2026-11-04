#include "frontend/fast_boot.hpp"
#include "frontend/state.hpp"
#include "f3rt/audio.hpp"
#include "f3rt/game_boot.hpp"
#include "f3rt/input.hpp"
#include "f3rt/machine.hpp"
#include <array>
#include <chrono>
#include <iostream>
#include <stdexcept>

namespace f3rt {
FastBootResult run_fast_boot(Machine &machine, const GameBoot &boot,
                             const FastBootRequest &request) {
    FastBootResult result;
    bool eeprom_enabled = false, eeprom_loaded = false, eeprom_generated = false;
    if (!request.eeprom_init.empty()) {
        eeprom_enabled = true;
        if (std::filesystem::exists(request.eeprom_init)) {
            try {
                machine.load_eeprom(request.eeprom_init);
                eeprom_loaded = true;
            } catch (const std::exception &error) {
                std::cerr << "f3rt: ignoring initialised EEPROM " << request.eeprom_init << ": "
                          << error.what() << '\n';
            }
        }
    }
    if (!request.boot_cache.empty() && std::filesystem::exists(request.boot_cache)) {
        try {
            frontend_state_slot(machine, request.boot_cache, request.snapshot.scale,
                                request.snapshot.border, false);
            result.cache_loaded = true;
        } catch (const std::exception &error) {
            std::cerr << "f3rt: boot cache unusable, using turbo boot: " << error.what() << '\n';
        }
    }
    result.source = result.cache_loaded ? "cache" : "turbo";
    if (!result.cache_loaded) {
        std::array<int16_t, 8192> samples{};
        const uint64_t executed_at_start = machine.frame;
        const auto start = std::chrono::steady_clock::now();
        while ((request.frame_budget == 0 || executed_at_start + result.frames < request.frame_budget) &&
               result.frames < boot.frame_limit) {
            if (boot.complete(machine)) {
                result.complete = true;
                break;
            }
            apply_local_inputs(machine, {});
            if (!machine.run_frame(request.translated))
                throw std::runtime_error("CPU halted at " + std::to_string(machine.cpu.pc));
            ++result.frames;
            size_t count;
            while ((count = machine.audio->render(samples.data(), samples.size() / 2)) != 0)
                if (request.audio)
                    request.audio(std::span<const int16_t>(samples.data(), count * 2));
            if (request.cancelled && result.frames % 30 == 0 && request.cancelled()) break;
        }
        result.ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                .count();
        if (!result.complete && result.frames >= boot.frame_limit)
            std::cerr << "f3rt: fast boot reached the " << boot.frame_limit
                      << "-frame limit without a completion signal; continuing normally\n";
    }
    if (eeprom_enabled && !eeprom_loaded && result.complete) {
        std::error_code ec;
        std::filesystem::create_directories(request.eeprom_init.parent_path(), ec);
        machine.save_eeprom(request.eeprom_init);
        eeprom_generated = true;
    }
    if (!request.boot_cache.empty() && !result.cache_loaded && result.complete) {
        try {
            frontend_state_slot(machine, request.boot_cache, request.snapshot.scale,
                                request.snapshot.border, true);
        } catch (const std::exception &error) {
            std::cerr << "f3rt: cannot write boot cache " << request.boot_cache << ": "
                      << error.what() << '\n';
        }
    }
    result.eeprom = !eeprom_enabled    ? "none"
                    : eeprom_generated ? "generated"
                    : eeprom_loaded    ? "loaded"
                                       : "none";
    return result;
}
} // namespace f3rt
