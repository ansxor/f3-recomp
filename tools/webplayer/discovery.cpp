#include "discovery.hpp"

#include "audio/hle/core.hpp"
#include "f3rt/audio.hpp"

#include <algorithm>
#include <exception>
#include <map>
#include <optional>

namespace f3rt::webplayer {

namespace {

// Mailbox opcodes that start a sequence (the sound driver treats them alike).
constexpr uint8_t OP_START_SEQUENCE_A = 0x80;
constexpr uint8_t OP_START_SEQUENCE_B = 0x81;
// Selector descriptor type "start a sequence at a volume" (the sound test's type 0).
constexpr int DESCRIPTOR_START_SEQUENCE = 0;

// Sequence id of the first sequence-start packet among a request's ring bytes.
std::optional<uint32_t> start_sequence(const std::vector<uint8_t> &ring_bytes) {
    for (const auto &packet : runner::split_packets(ring_bytes))
        if (packet.size() >= 3 && (packet[1] == OP_START_SEQUENCE_A || packet[1] == OP_START_SEQUENCE_B))
            return packet[2];
    return std::nullopt;
}

// Maps sound-test BANK value 1..31 to the sound ROM page selected by the main-bus 0x300000 bankswitch write.
// Matching Machine::write16: idx = (offset << 1) & 0x1e; if (low16) idx += 1; if (idx >= 8) idx -= 8.
uint32_t page_for_bank(uint32_t bank) {
    const uint32_t idx = bank & 0x1f;
    return idx >= 8 ? idx - 8 : idx;
}

} // namespace

CodeScan scan_music_codes(Machine &m, const runner::PostBootSnapshot &snapshot, const runner::SoundProfile &profile) {
    CodeScan scan;
    std::map<uint32_t, size_t> by_sequence; // sequence id -> index into scan.songs
    for (uint32_t code = 1; code <= profile.last_code; ++code) {
        ++scan.codes_scanned;
        runner::SoundTestCapture capture;
        try {
            runner::restore_for_capture(m, snapshot);
            const uint64_t clock = m.audio->clock_ticks();
            runner::capture_sound_test(m, &profile, profile.request, code, /*no_prelude=*/false, capture);
            if (m.audio->clock_ticks() != clock)
                throw std::runtime_error("the request advanced emulated time");
        } catch (const std::exception &e) {
            scan.failures.emplace_back(code, e.what());
            continue;
        }
        if (capture.descriptor_type != DESCRIPTOR_START_SEQUENCE) continue;
        std::optional<uint32_t> sequence;
        try {
            sequence = start_sequence(capture.calls.back().bytes);
        } catch (const std::exception &e) {
            scan.failures.emplace_back(code, e.what());
            continue;
        }
        if (!sequence) {
            scan.failures.emplace_back(code, "type 0 request without a sequence-start packet");
            continue;
        }
        ++scan.music_codes;
        const auto [it, fresh] = by_sequence.try_emplace(*sequence, scan.songs.size());
        if (fresh) {
            Song song;
            song.sequence = *sequence;
            song.code = code;
            song.codes.push_back(code);
            song.capture = std::move(capture);
            scan.songs.push_back(std::move(song));
        } else {
            scan.songs[it->second].codes.push_back(code);
        }
    }
    return scan; // songs are in canonical-code order: the scan is ascending and the first code of a sequence wins
}

BankResolution resolve_bank_pages(std::span<const uint8_t> sound_rom, std::span<const uint16_t> sample_words,
                                  const std::vector<uint32_t> &banks) {
    std::vector<uint32_t> sorted = banks;
    std::sort(sorted.begin(), sorted.end());
    sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
    BankResolution res;
    std::map<uint32_t, size_t> page_to_entry;
    std::map<uint32_t, std::string> page_to_error;

    for (const uint32_t bank : sorted) {
        const uint32_t page = page_for_bank(bank);
        // If this page was already tested and rejected:
        const auto err_it = page_to_error.find(page);
        if (err_it != page_to_error.end()) {
            res.rejected.push_back({bank, page, err_it->second});
            continue;
        }
        // If this page was already tested and accepted:
        const auto entry_it = page_to_entry.find(page);
        if (entry_it != page_to_entry.end()) {
            res.pages[entry_it->second].aliases.push_back(bank);
            continue;
        }
        // Test this page on a scratch Core:
        try {
            hle::Core core(sound_rom, sample_words);
            core.set_bank(uint8_t(page), 0);
            page_to_entry[page] = res.pages.size();
            res.pages.push_back({bank, page, {}});
        } catch (const std::exception &e) {
            page_to_error[page] = e.what();
            res.rejected.push_back({bank, page, e.what()});
        }
    }
    return res;
}

} // namespace f3rt::webplayer
