#include "f3rt/machine.hpp"
#include "eeprom.hpp"
#include "support.hpp"
#include <gtest/gtest.h>
#include <rapidcheck/gtest.h>
#include <chrono>
#include <filesystem>

using f3test::fixture;

namespace {
void send_bit(f3rt::Eeprom &e, bool bit, uint64_t now) {
    uint8_t pins = 0x10 | (bit ? 4 : 0);
    e.pins(pins, now);
    e.pins(pins | 8, now);
}

void command(f3rt::Eeprom &e, unsigned word, uint64_t now) {
    e.pins(0, now);
    for (int bit = 8; bit >= 0; --bit)
        send_bit(e, (word >> bit) & 1, now);
}

void serial_write(f3rt::Eeprom &e, unsigned address, uint16_t value, uint64_t now) {
    command(e, 0x140 | address, now);
    for (int bit = 15; bit >= 0; --bit)
        send_bit(e, (value >> bit) & 1, now);
    e.pins(0, now);
}

uint16_t read_word(f3rt::Eeprom &e, uint64_t now) {
    uint16_t value = 0;
    for (int i = 0; i < 16; ++i) {
        send_bit(e, false, now);
        value = uint16_t((value << 1) | e.output(now));
    }
    return value;
}

void send_bit(f3rt::Machine &m, bool bit) {
    const uint8_t pins = 0x10 | (bit ? 4 : 0);
    m.write8(0x4a0013, pins);
    m.write8(0x4a0013, pins | 8);
}

void command(f3rt::Machine &m, unsigned word) {
    m.write8(0x4a0013, 0);
    for (int bit = 8; bit >= 0; --bit)
        send_bit(m, (word >> bit) & 1);
}

void serial_write(f3rt::Machine &m, unsigned address, uint16_t value) {
    command(m, 0x140 | address);
    for (int bit = 15; bit >= 0; --bit)
        send_bit(m, (value >> bit) & 1);
    m.write8(0x4a0013, 0);
}

uint16_t read_word(f3rt::Machine &m) {
    uint16_t value = 0;
    for (int i = 0; i < 16; ++i) {
        send_bit(m, false);
        value = uint16_t((value << 1) | (m.read8(0x4a0000) & 1));
    }
    return value;
}

struct TemporaryImage {
    std::filesystem::path path;
    ~TemporaryImage() {
        std::error_code error;
        std::filesystem::remove(path, error);
    }
};
} // namespace

TEST(Eeprom, FactoryDefaultsAndSerialWrap) {
    auto roms = fixture();
    roms.factory_eeprom.assign(128, 0xff);
    roms.factory_eeprom[0] = 0x12;
    roms.factory_eeprom[1] = 0x34;
    roms.factory_eeprom[2] = 0x89;
    roms.factory_eeprom[3] = 0xab;
    roms.factory_eeprom[126] = 0xfe;
    roms.factory_eeprom[127] = 0xdc;
    auto m = std::make_unique<f3rt::Machine>(std::move(roms));
    command(*m, 0x1bf);
    EXPECT_FALSE(m->read8(0x4a0000) & 1)
        << "Factory EEPROM read exposes the serial dummy bit through the input port";
    const uint16_t w0 = read_word(*m);
    const uint16_t w1 = read_word(*m);
    const uint16_t w2 = read_word(*m);
    EXPECT_TRUE(w0 == 0xfedc && w1 == 0x1234 && w2 == 0x89ab)
        << "Factory EEPROM seeds all addresses big-endian before initial reset, including serial wrap";

    command(*m, 0x130);
    m->write8(0x4a0013, 0); // EWEN
    serial_write(*m, 0, 0xa65c);
    m->cpu.cycles += 28000;
    m->reset();
    command(*m, 0x180);
    const uint16_t rw0 = read_word(*m);
    const uint16_t rw1 = read_word(*m);
    EXPECT_TRUE(rw0 == 0xa65c && rw1 == 0x89ab)
        << "Machine reset preserves guest EEPROM writes instead of reseeding factory defaults";
}

TEST(Eeprom, UserImageOverrideAndPersistence) {
    auto roms = fixture();
    roms.factory_eeprom.assign(128, 0xff);
    roms.factory_eeprom[0] = 0x12;
    roms.factory_eeprom[1] = 0x34;
    roms.factory_eeprom[2] = 0x89;
    roms.factory_eeprom[3] = 0xab;
    roms.factory_eeprom[126] = 0xfe;
    roms.factory_eeprom[127] = 0xdc;
    auto m = std::make_unique<f3rt::Machine>(std::move(roms));

    TemporaryImage image{std::filesystem::temp_directory_path() /
        ("f3rt-check-eeprom-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".nv")};
    f3rt::Eeprom user;
    user.words.fill(0x579b);
    user.words[0] = 0xc318;
    user.words[63] = 0x2468;
    user.save(image.path);

    m->load_eeprom(image.path);
    m->reset();
    command(*m, 0x1bf);
    const uint16_t uw0 = read_word(*m);
    const uint16_t uw1 = read_word(*m);
    const uint16_t uw2 = read_word(*m);
    EXPECT_TRUE(uw0 == 0x2468 && uw1 == 0xc318 && uw2 == 0x579b)
        << "Explicit user EEPROM image overrides factory and prior guest contents across reset";

    command(*m, 0x130);
    m->write8(0x4a0013, 0);
    serial_write(*m, 0, 0xd42e);
    m->cpu.cycles += 28000;
    m->reset();
    command(*m, 0x180);
    EXPECT_EQ(read_word(*m), 0xd42e)
        << "Loaded user EEPROM remains writable and persists through reset";

    roms = std::move(m->roms);
    m.reset();
    auto factory_machine = std::make_unique<f3rt::Machine>(std::move(roms));
    command(*factory_machine, 0x1bf);
    const uint16_t fw0 = read_word(*factory_machine);
    const uint16_t fw1 = read_word(*factory_machine);
    const uint16_t fw2 = read_word(*factory_machine);
    EXPECT_TRUE(fw0 == 0xfedc && fw1 == 0x1234 && fw2 == 0x89ab)
        << "A new machine consumes the unchanged ROM seed after user-image load and guest writes";
}

TEST(Eeprom, WriteDisabledAtPowerOn) {
    f3rt::Eeprom e;
    uint64_t now = 100;
    serial_write(e, 63, 0x1234, now);
    EXPECT_EQ(e.words[63], 0xffff) << "EEPROM write disabled at power-on";
}

TEST(Eeprom, WriteTimingAndBusyStatus) {
    f3rt::Eeprom e;
    uint64_t now = 100;
    command(e, 0x130, now);
    e.pins(0, now); // EWEN
    serial_write(e, 63, 0x1234, now);
    EXPECT_TRUE(e.output(now)) << "Deselected EEPROM DO is pulled high while programming";
    e.pins(0x10, now);
    EXPECT_FALSE(e.output(now)) << "Raising CS exposes EEPROM programming busy";
    serial_write(e, 0, 0xdead, now);
    EXPECT_EQ(e.words[0], 0xffff) << "EEPROM ignores new commands while programming";
    e.pins(0x10, now);
    now += 27999;
    EXPECT_FALSE(e.output(now)) << "EEPROM write remains busy before 1750us deadline";
    ++now;
    EXPECT_TRUE(e.output(now)) << "EEPROM write finishes without another clock edge";
}

TEST(Eeprom, SequentialReadWrap) {
    f3rt::Eeprom e;
    uint64_t now = 100;
    command(e, 0x130, now);
    e.pins(0, now);
    serial_write(e, 63, 0x1234, now);
    now += 28000;
    serial_write(e, 0, 0xabcd, now);
    now += 28000;
    command(e, 0x1bf, now);
    EXPECT_FALSE(e.output(now)) << "EEPROM read dummy bit";
    const uint16_t r0 = read_word(e, now);
    const uint16_t r1 = read_word(e, now);
    EXPECT_TRUE(r0 == 0x1234 && r1 == 0xabcd) << "EEPROM sequential read wraps 63 to 0";
}

TEST(Eeprom, EwdsWriteProtection) {
    f3rt::Eeprom e;
    uint64_t now = 100;
    command(e, 0x130, now);
    e.pins(0, now);
    serial_write(e, 0, 0xabcd, now);
    now += 28000;
    e.pins(0, now);
    command(e, 0x100, now);
    e.pins(0, now);
    serial_write(e, 0, 0x4321, now);
    e.pins(0x10, now);
    EXPECT_TRUE(e.words[0] == 0xabcd && e.output(now))
        << "EEPROM EWDS protects contents without becoming busy";
}

TEST(Eeprom, SingleWordEraseTiming) {
    f3rt::Eeprom e;
    uint64_t now = 100;
    command(e, 0x130, now);
    e.pins(0, now);
    serial_write(e, 63, 0x1234, now);
    now += 28000;
    command(e, 0x130, now);
    command(e, 0x1ff, now); // EWEN; erase word 63
    e.pins(0, now);
    e.pins(0x10, now);
    now += 15999;
    EXPECT_FALSE(e.output(now)) << "EEPROM erase remains busy before 1000us deadline";
    ++now;
    EXPECT_TRUE(e.output(now) && e.words[63] == 0xffff)
        << "EEPROM single-word erase completes";
}

TEST(Eeprom, EraseAllTiming) {
    f3rt::Eeprom e;
    uint64_t now = 100;
    command(e, 0x130, now);
    command(e, 0x120, now);
    e.pins(0, now);
    e.pins(0x10, now); // ERAL
    now += 127999;
    EXPECT_FALSE(e.output(now)) << "EEPROM erase-all remains busy before 8000us deadline";
    ++now;
    EXPECT_TRUE(e.output(now) && e.words[0] == 0xffff) << "EEPROM erase-all completes";
}

TEST(Eeprom, WriteAllTiming) {
    f3rt::Eeprom e;
    uint64_t now = 100;
    command(e, 0x130, now);
    command(e, 0x110, now); // WRAL
    for (int bit = 15; bit >= 0; --bit)
        send_bit(e, (0x5a5a >> bit) & 1, now);
    e.pins(0, now);
    e.pins(0x10, now);
    now += 127999;
    EXPECT_FALSE(e.output(now)) << "EEPROM write-all remains busy before 8000us deadline";
    ++now;
    EXPECT_TRUE(e.output(now) && e.words[0] == 0x5a5a && e.words[63] == 0x5a5a)
        << "EEPROM write-all completes";
}

TEST(Eeprom, PowerResetPreservesContents) {
    f3rt::Eeprom e;
    uint64_t now = 100;
    command(e, 0x130, now);
    command(e, 0x110, now); // WRAL
    for (int bit = 15; bit >= 0; --bit)
        send_bit(e, (0x5a5a >> bit) & 1, now);
    e.pins(0, now);
    e.pins(0x10, now);
    now += 128000;
    e.reset();
    e.pins(0x10, 0);
    EXPECT_TRUE(e.output(0) && e.words[0] == 0x5a5a)
        << "Power reset clears serial timing but preserves EEPROM contents";
}

// RapidCheck property: writing any 16-bit word at any 0..63 address under EWEN can be read back.
RC_GTEST_PROP(Eeprom, SerialWriteReadRoundTrip, ()) {
    const auto address = *rc::gen::inRange(0u, 64u);
    const auto value = *rc::gen::arbitrary<uint16_t>();
    f3rt::Eeprom e;
    uint64_t now = 1000;
    command(e, 0x130, now); // EWEN
    e.pins(0, now);
    serial_write(e, address, value, now);
    now += 28000;
    command(e, 0x180 | address, now); // READ address
    RC_ASSERT(!e.output(now)); // dummy bit
    const uint16_t read_back = read_word(e, now);
    RC_ASSERT(read_back == value);
    RC_ASSERT(e.words[address] == value);
}

// RapidCheck property: EWDS disables writes for all addresses and values.
RC_GTEST_PROP(Eeprom, WriteDisabledRejectsWrites, ()) {
    const auto address = *rc::gen::inRange(0u, 64u);
    const auto value = *rc::gen::arbitrary<uint16_t>();
    f3rt::Eeprom e;
    uint64_t now = 1000;
    // Power-on state has write disabled
    serial_write(e, address, value, now);
    RC_ASSERT(e.words[address] == 0xffff);

    // Explicit EWDS
    command(e, 0x130, now); // EWEN
    e.pins(0, now);
    command(e, 0x100, now); // EWDS
    e.pins(0, now);
    serial_write(e, address, value, now);
    RC_ASSERT(e.words[address] == 0xffff);
}

// RapidCheck property: State save and load round-trip preserves words.
RC_GTEST_PROP(Eeprom, StateSaveLoadRoundTrip, ()) {
    f3rt::Eeprom original;
    for (size_t i = 0; i < original.words.size(); ++i) {
        original.words[i] = *rc::gen::arbitrary<uint16_t>();
    }
    std::vector<uint8_t> buffer(original.state_size());
    f3rt::StateWriter writer(buffer);
    original.save_state(writer);

    f3rt::Eeprom restored;
    f3rt::StateReader reader(buffer);
    restored.load_state(reader);

    RC_ASSERT(original.words == restored.words);
}

// RapidCheck property: File save and load round-trip preserves words.
RC_GTEST_PROP(Eeprom, FileSaveLoadRoundTrip, ()) {
    f3rt::Eeprom original;
    for (size_t i = 0; i < original.words.size(); ++i) {
        original.words[i] = *rc::gen::arbitrary<uint16_t>();
    }
    TemporaryImage temp{std::filesystem::temp_directory_path() /
        ("f3rt-rc-eeprom-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".nv")};
    original.save(temp.path);

    f3rt::Eeprom restored;
    restored.load(temp.path);
    RC_ASSERT(original.words == restored.words);
}
