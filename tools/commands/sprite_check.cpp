#include "commands.hpp"
#include "../runner/sprite_check.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

namespace f3rt::tool {

int cmd_sprite_check(int argc, char **argv) try {
    runner::SpriteCheckArgs args;
#ifdef F3RT_DEFAULT_ROM_DIR
    args.rom_dir = F3RT_DEFAULT_ROM_DIR;
#endif
#ifdef F3RT_DEFAULT_SET
    args.set = F3RT_DEFAULT_SET;
#endif
    for (int i = 0; i < argc; ++i) {
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
        else if (arg == "--no-flicker-shadows") args.flicker = false;
        else if (arg == "--shadow-trace") args.shadow_trace = std::stoull(value());
        else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: f3rt-tool sprite-check [--rom-dir DIR] [--set SET] [--frames N] [--compare-every N]\n"
                         "       [--seed N] [--no-inputs] [--behaviour NAME]... [--no-flicker-shadows]\n"
                         "       [--shadow-trace N]\n"
                         "Exits nonzero unless every unit replay is bit-exact, every sprite write is accounted\n"
                         "for, and emulated state is identical with sprite units on and off.\n";
            return 0;
        } else throw std::runtime_error("Unknown argument: " + arg);
    }
    if (args.rom_dir.empty()) throw std::runtime_error("ROM directory must be specified via --rom-dir");
    if (args.set.empty()) throw std::runtime_error("--set is required");
    if (!args.frames || !args.compare_every) throw std::runtime_error("--frames and --compare-every must be positive");

    const auto res = runner::run_sprite_check(args, &std::cout);
    return res.passed ? 0 : 1;
} catch (const std::exception &e) {
    std::cerr << "sprite-check error: " << e.what() << '\n';
    return 2;
}

} // namespace f3rt::tool
