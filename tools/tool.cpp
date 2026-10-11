#include "commands/commands.hpp"
#include <cstring>
#include <iostream>
#include <string_view>

namespace {

void print_general_help(const char *prog) {
    std::cout << "Usage: " << prog << " <subcommand> [options]\n\n"
              << "Taito F3 recompiled runtime tool and regression harness.\n\n"
              << "Subcommands:\n"
              << "  gameplay       Deterministic strict-native seeded gameplay regression harness\n"
              << "  gpu-compare    Strict-native GPU parity verification against FDP oracle\n"
              << "  motion         Strict-native seeded Land Maker temporal GPU proof\n"
              << "  sound-extract  Standalone sound extraction and stimulus injection tool\n"
              << "  sprite-check   Sprite emit-unit and behaviour verification against native code\n\n"
              << "Run '" << prog << " <subcommand> --help' for details on each subcommand.\n";
}

} // namespace

int main(int argc, char **argv) {
    if (argc < 2) {
        print_general_help(argv[0]);
        return 1;
    }

    const std::string_view subcmd = argv[1];
    if (subcmd == "--help" || subcmd == "-h" || subcmd == "help") {
        print_general_help(argv[0]);
        return 0;
    }

    const int sub_argc = argc - 2;
    char **sub_argv = argv + 2;

    if (subcmd == "gameplay") {
        return f3rt::tool::cmd_gameplay(sub_argc, sub_argv);
    }
    if (subcmd == "gpu-compare") {
        return f3rt::tool::cmd_gpu_compare(sub_argc, sub_argv);
    }
    if (subcmd == "motion") {
        return f3rt::tool::cmd_motion(sub_argc, sub_argv);
    }
    if (subcmd == "sound-extract") {
        return f3rt::tool::cmd_sound_extract(sub_argc, sub_argv);
    }
    if (subcmd == "sprite-check") {
        return f3rt::tool::cmd_sprite_check(sub_argc, sub_argv);
    }

    std::cerr << "Unknown subcommand: " << subcmd << "\n\n";
    print_general_help(argv[0]);
    return 1;
}
