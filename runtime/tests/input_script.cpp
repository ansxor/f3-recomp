#include "debug/input_script.hpp"
#include "f3rt/input.hpp"
#include "f3rt/machine.hpp"
#include "support.hpp"
#include <gtest/gtest.h>
#include <algorithm>
#include <memory>
#include <vector>

using f3test::fixture;

TEST(InputScript, RangeAndMashTiming) {
    auto script = f3rt::InputScript::parse(
        "# coin pulse\n700+3 coin\n710-712 p2 right+b1 # comment\n20-end mash seed=5 period=4 keys=left\n", "t");
    const auto coin = [&](uint64_t frame) { return script.words(frame)[0] & 0x100; };
    EXPECT_TRUE(coin(699) == 0 && coin(700) == 0x100 && coin(702) == 0x100 && coin(703) == 0 &&
                script.words(710)[1] == 0x18 && script.words(713)[1] == 0)
        << "Input script ranges are inclusive; +COUNT spans COUNT frames; pN selects the player";

    std::vector<f3rt::LocalInputWord> forward;
    for (uint64_t frame = 20; frame < 120; ++frame)
        forward.push_back(script.words(frame)[0] & ~0x100);
    EXPECT_TRUE(std::count(forward.begin(), forward.end(), 0x4) && std::count(forward.begin(), forward.end(), 0))
        << "Mash presses and releases only its keys";
    EXPECT_EQ(script.words(57)[0] & ~0x100, forward[57 - 20])
        << "Going back to an earlier frame replays the same mash sequence";
}

TEST(InputScript, MalformedLinesRejected) {
    for (const char *bad : {"0 coin\n", "10-5 coin\n", "5 p5 coin\n", "5 jump\n", "5 left right\n",
                            "5 mash period=0\n", "5 mash speed=3\n", "5\n", "5 poke 0x401f54=1\n",
                            "5 poke 0x401f54.w=0x10000\n", "5 poke 0x41ffff.w=1\n", "5 poke 0x660000.b=1\n",
                            "5 p2 poke 0x401f54.b=1\n"}) {
        bool rejected = false;
        try { f3rt::InputScript::parse(bad, "t"); }
        catch (const std::runtime_error &) { rejected = true; }
        EXPECT_TRUE(rejected) << "Malformed input script lines are rejected";
    }
}

TEST(InputScript, MemoryPokes) {
    auto pokes = f3rt::InputScript::parse("5-6 poke 0x401f54.w=0x1234\n7 poke 0x41fffc.l=4294967295\n", "t");
    auto m = std::make_unique<f3rt::Machine>(fixture());
    pokes.poke(*m, 4);
    EXPECT_EQ(m->read16(0x401f54), 0) << "Pokes wait for their first frame";
    pokes.poke(*m, 6);
    pokes.poke(*m, 7);
    EXPECT_TRUE(m->read16(0x401f54) == 0x1234 && m->read32(0x41fffc) == 0xffffffff)
        << "Pokes write big-endian main RAM, hex or decimal, up to the last RAM byte";
}
