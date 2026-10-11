#include "f3rt/machine.hpp"
#include "f3rt/input.hpp"
#include "support.hpp"
#include <gtest/gtest.h>
#include <rapidcheck/gtest.h>
#include <array>
#include <vector>

using f3test::fixture;

namespace {
using Words = std::array<f3rt::LocalInputWord, f3rt::local_player_count>;
constexpr uint32_t ports[] = {0x4a0002, 0x4a0006, 0x4a0012, 0x4a0016};
constexpr uint16_t button_masks[4][4] = {
    {0x0001, 0x0002, 0x0004, 0x0008}, {0x0010, 0x0020, 0x0040, 0x0080},
    {0x0100, 0x0200, 0x0400, 0x0800}, {0x1000, 0x2000, 0x4000, 0x8000}};
constexpr uint16_t direction_masks[4][4] = {
    {0x0001, 0x0002, 0x0004, 0x0008}, {0x0010, 0x0020, 0x0040, 0x0080},
    {0x0001, 0x0002, 0x0004, 0x0008}, {0x0010, 0x0020, 0x0040, 0x0080}};
constexpr uint16_t start_masks[] = {0x1000, 0x2000, 0x4000, 0x8000};
constexpr uint8_t coin_masks[] = {0x10, 0x20, 0x40, 0x80};
constexpr uint16_t service_masks[] = {0x200, 0x400, 0x800, 0};
} // namespace

TEST(Input, LocalControlsActiveLowMapping) {
    for (bool six_button : {false, true}) {
        auto roms = fixture();
        roms.input.six_button = six_button;
        auto m = std::make_unique<f3rt::Machine>(std::move(roms));
        m->write16(0x4a0004, 0x0ba6);
        m->write16(0x4a0014, 0x0975);
        const auto expect = [&](const std::array<uint16_t, 4> &low, uint8_t system) {
            for (unsigned port = 0; port < low.size(); ++port)
                EXPECT_EQ(m->read16(ports[port]), low[port])
                    << "Local controls reach only their physical active-low MMIO lines";
            EXPECT_EQ(m->read8(0x4a0000) & 0xfe, system)
                << "Coins and test reach both system-byte MMIO lanes without pinning EEPROM DO";
            EXPECT_EQ(m->read8(0x4a0001) & 0xfe, system)
                << "Coins and test reach both system-byte MMIO lanes without pinning EEPROM DO";
            EXPECT_TRUE(m->read16(0x4a0004) == 0x0ba6 && m->read16(0x4a0014) == 0x0975)
                << "Applying controls preserves both readable coin-counter banks";
        };
        const unsigned players = six_button ? 2 : f3rt::local_player_count;
        for (unsigned slot = 0; slot < players; ++slot) {
            for (unsigned bit = 0; bit < f3rt::local_control_count; ++bit) {
                Words words{};
                words[slot] = f3rt::LocalInputWord(1u << bit);
                std::array<uint16_t, 4> low{0xffff, 0xffff, 0xffff, 0xffff};
                uint8_t system = 0xfe;
                if (bit < 4) low[slot < 2 ? 1 : 3] &= uint16_t(~direction_masks[slot][bit]);
                else if (bit < 7) low[slot < 2 ? 0 : 2] &= uint16_t(~button_masks[slot][bit - 4]);
                else if (bit == 7) low[0] &= uint16_t(~start_masks[slot]);
                else if (bit == 8) system &= uint8_t(~coin_masks[slot]);
                else if (bit == 9) low[0] &= uint16_t(~service_masks[slot]);
                else if (bit == 10) system &= uint8_t(~2u);
                else if (six_button) {
                    constexpr uint16_t extra_masks[2][3] = {{1, 2, 4}, {0x100, 0x200, 0x400}};
                    low[slot == 0 ? 3 : 2] &= uint16_t(~extra_masks[slot][bit - 11]);
                } else if (bit == 11) low[slot < 2 ? 0 : 2] &= uint16_t(~button_masks[slot][3]);
                f3rt::apply_local_inputs(*m, words);
                expect(low, system);
            }
        }
        if (six_button) {
            f3rt::apply_local_inputs(*m, {0, 0, f3rt::local_input_mask, f3rt::local_input_mask});
            expect({0xffff, 0xffff, 0xffff, 0xffff}, 0xfe);
            f3rt::apply_local_inputs(*m, {f3rt::local_input_mask, f3rt::local_input_mask, 0, 0});
            expect({uint16_t(~(0x0007u | 0x0070u | 0x1000u | 0x2000u | 0x0200u | 0x0400u)),
                    0xff00, 0xf8ff, 0xfff8}, uint8_t(0xfe & ~0x32u));
        }
        f3rt::apply_local_inputs(*m, {});
        expect({0xffff, 0xffff, 0xffff, 0xffff}, 0xfe);
    }
}

TEST(Input, DialNibblePackingAndDirectionCancellation) {
    using Words = std::array<f3rt::LocalInputWord, f3rt::local_player_count>;
    for (int run = 0; run < 2; ++run) {
        auto roms = fixture();
        roms.input.dial = true;
        auto m = std::make_unique<f3rt::Machine>(std::move(roms));
        const auto expect = [&](uint32_t first, uint32_t second) {
            EXPECT_TRUE(m->read32(0x4a0008) == first && m->read32(0x4a000c) == second)
                << "Both twelve-bit dial counters expose the native big-endian nibble-packed MMIO words";
        };
        expect(0xffff0000, 0xffff0000);
        f3rt::apply_local_inputs(*m, {4, 8, 0, 0});
        expect(0xffffe0ff, 0xffff2000);
        EXPECT_TRUE(m->read8(0x4a000a) == 0xe0 && m->read8(0x4a000b) == 0xff &&
                    m->read16(0x4a000e) == 0x2000 && m->read16(0x4a0006) == 0xff7b)
            << "Dial arrows also reach each player's real native joystick bits and byte lanes";
        f3rt::apply_local_inputs(*m, {});
        expect(0xffffe0ff, 0xffff2000);
        EXPECT_EQ(m->read16(0x4a0006), 0xffff)
            << "Neutral input releases joystick lines without clearing dial history";
        f3rt::apply_local_inputs(*m, {0xc, 0xc, 0, 0});
        expect(0xffffe0ff, 0xffff2000);
        EXPECT_EQ(m->read16(0x4a0006), 0xff33)
            << "Opposite arrows cancel counter motion while preserving native joystick inputs";
        f3rt::apply_local_inputs(*m, {8, 4, 0, 0});
        expect(0xffff0000, 0xffff0000);
        for (unsigned frame = 0; frame < 8; ++frame)
            f3rt::apply_local_inputs(*m, {8, 4, 0, 0});
        expect(0xffff0001, 0xffff00ff);
        f3rt::apply_local_inputs(*m, {0, 0, 4, 8});
        expect(0xffff0001, 0xffff00ff);
        m->reset();
        expect(0xffff0001, 0xffff00ff);
        f3rt::apply_local_inputs(*m, {});
        expect(0xffff0001, 0xffff00ff);

        const std::array<Words, 5> continuation{{
            Words{8, 0, 0, 0}, Words{0, 4, 0, 0}, Words{0xc, 8, 0, 0},
            Words{4, 8, 0, 0}, Words{}}};
        const auto replay = [&] {
            for (const auto &words : continuation) {
                f3rt::apply_local_inputs(*m, words);
                EXPECT_TRUE(m->run_frame()) << "Dial snapshot continuation executes a logical machine frame";
            }
            expect(0xffff0001, 0xffff20ff);
            return m->state_crc();
        };
        std::vector<uint8_t> full(m->state_size());
        m->save_state(full);
        const uint32_t expected_crc = replay();
        m->load_state(full);
        expect(0xffff0001, 0xffff00ff);
        EXPECT_EQ(replay(), expected_crc)
            << "Save states restore dial history and replay identical serialized machine continuation";
    }
}

TEST(Input, CoinCounterRisingEdge) {
    auto m = std::make_unique<f3rt::Machine>(fixture());
    m->write8(0x4a0004, 0x04);
    m->write8(0x4a0004, 0x04);
    EXPECT_EQ(m->coin_count[0], 1) << "Coin counter rising-edge only";
}

TEST(Input, StartButtonActiveLow) {
    auto m = std::make_unique<f3rt::Machine>(fixture());
    m->set_input(0, 0x1000, true);
    EXPECT_FALSE(m->read32(0x4a0000) & 0x1000) << "Active-low start input";
}

// RapidCheck property: Local input bit packing preserves active-low invariant on port lines.
RC_GTEST_PROP(Input, LocalInputActiveLowBitPacking, ()) {
    const auto slot = *rc::gen::inRange(0u, 4u);
    const auto button_index = *rc::gen::inRange(0u, 3u); // buttons 1..3
    const auto word = f3rt::LocalInputWord(1u << (4 + button_index));

    static auto m = std::make_unique<f3rt::Machine>(fixture());
    Words words{};
    words[slot] = word;
    f3rt::apply_local_inputs(*m, words);

    // The selected button's line must be pulled low (0)
    const uint32_t port = ports[slot < 2 ? 0 : 2];
    const uint16_t expected_mask = button_masks[slot][button_index];
    const uint16_t read_val = m->read16(port);
    RC_ASSERT((read_val & expected_mask) == 0);
}
