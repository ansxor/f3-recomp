#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

// Requires at least 2400 scheduled frames to include active Land Maker play.
// The probe resets/starts allocation counting on true, stops/returns it on false.
// Throws on any failed proof; returns zero after printing deterministic evidence.
int run_sync_snapshot_proof(const std::filesystem::path &romdir,
                            const std::string &set, const std::string &driver,
                            uint64_t seed, uint64_t frames,
                            uint64_t (*allocation_probe)(bool));
