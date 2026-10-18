#pragma once

// Song discovery for the web-player exporter: which sound-test codes are music, and which sound-test BANK values
// select which sound ROM page. Everything comes from the game's own routines (runner/sound/capture.hpp) run in
// the interpreter from the frozen post-boot machine; nothing renders audio.

#include "sound/capture.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace f3rt::webplayer {

// One song of the sound test: the lowest code that starts it plus every code that starts the same sequence
// (different volume or effect-preset prelude).
struct Song {
    uint32_t sequence = 0; // sequence id of the start packet (mailbox 0x80/0x81 argument)
    uint32_t code = 0;     // canonical = lowest code
    std::vector<uint32_t> codes; // ascending, includes `code`
    runner::SoundTestCapture capture; // what the sound test requests for `code`: prelude calls then the code
};

struct CodeScan {
    std::vector<Song> songs; // ascending canonical code
    uint32_t codes_scanned = 0;
    uint32_t music_codes = 0;
    // Codes that did not classify (routine failed, setup consumed emulated time, music without a start packet).
    std::vector<std::pair<uint32_t, std::string>> failures;
};

// Scans codes 1..profile.last_code. A code is music when the sound test's selector descriptor is type 0 (start a
// sequence at a volume) and the request carries a sequence-start packet (0x80/0x81). Songs are keyed by the
// sequence id of that packet; the volume variants and the different effect preludes of the same sequence are
// aliases. The machine must be frozen at the post-boot snapshot.
CodeScan scan_music_codes(Machine &m, const runner::PostBootSnapshot &snapshot, const runner::SoundProfile &profile);

struct BankPage {
    uint32_t bank = 0; // sound-test BANK value (lowest of the banks that select this page)
    uint32_t page = 0; // value Machine::write16 hands to Audio::set_sound_bank
    std::vector<uint32_t> aliases; // other requested banks that select the same page, ascending
};

struct RejectedBank {
    uint32_t bank = 0;
    uint32_t page = 0;
    std::string error;
};

struct BankResolution {
    std::vector<BankPage> pages;
    std::vector<RejectedBank> rejected;
};

// Maps the sound-test BANK values `banks` (1..31) to the pages the sound ROM bank switch selects, merging banks
// that select the same page, and testing each distinct page against a scratch Core. Unsupported banks are
// listed in `rejected`.
BankResolution resolve_bank_pages(std::span<const uint8_t> sound_rom, std::span<const uint16_t> sample_words,
                                  const std::vector<uint32_t> &banks);

} // namespace f3rt::webplayer
