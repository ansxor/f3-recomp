#include "relay.hpp"
#include <atomic>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {

std::atomic<bool> g_stop_requested{false};

void signal_handler(int) {
    g_stop_requested = true;
}

void print_usage(const char *prog) {
    std::cout << "Usage: " << prog << " [flags]\n\n"
              << "Two-player netplay relay (protocol v3).\n\n"
              << "Flags:\n"
              << "  --port <port>             Listen port for HTTP token endpoint (TCP) and game socket (UDP) (default 7845)\n"
              << "  --public-address <addr>   Advertised public address HOST:PORT (default 127.0.0.1:<port>)\n"
              << "  --logdir <dir>            Directory for per-session JSON logs (default ./netplay-logs)\n"
              << "  --max-rooms <n>           Maximum number of concurrent rooms (1 to 32, default 32)\n";
}

} // namespace

int main(int argc, char *argv[]) {
    f3rt::netplay::RelayConfig config;
    bool explicit_public_address = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return 0;
        } else if (arg == "--port" && i + 1 < argc) {
            config.port = static_cast<uint16_t>(std::atoi(argv[++i]));
        } else if (arg == "--public-address" && i + 1 < argc) {
            config.public_address = argv[++i];
            explicit_public_address = true;
        } else if (arg == "--logdir" && i + 1 < argc) {
            config.logdir = argv[++i];
        } else if (arg == "--max-rooms" && i + 1 < argc) {
            config.max_rooms = std::clamp(std::atoi(argv[++i]), 1, 32);
        } else {
            std::cerr << "Unknown flag: " << arg << "\n";
            print_usage(argv[0]);
            return 2;
        }
    }

    if (!explicit_public_address) {
        config.public_address = "127.0.0.1:" + std::to_string(config.port);
    }

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    f3rt::netplay::RelayServer server(config);
    if (!server.start()) {
        std::cerr << "Failed to start netplay relay server on port " << config.port << "\n";
        return 1;
    }

    const std::string shown_public = explicit_public_address
        ? config.public_address
        : "127.0.0.1:" + std::to_string(server.bound_port());
    std::cout << "netplay-server: port " << server.bound_port()
              << ", public-address " << shown_public
              << ", logdir " << config.logdir
              << ", max-rooms " << config.max_rooms << std::endl;

    server.run(g_stop_requested);

    server.stop();
    f3rt::netplay::shutdown_netplay();
    return 0;
}
