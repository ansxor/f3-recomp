#include <gtest/gtest.h>
#include "f3rt/bytes.hpp"
#include <array>

TEST(BytesHelper, ReadWriteBe) {
    const std::array<uint8_t, 8> data{0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0};

    EXPECT_EQ(f3rt::read_be16(data.data()), 0x1234);
    EXPECT_EQ(f3rt::read_be16(data, 1), 0x3456);
    EXPECT_EQ(f3rt::read_be32(data.data()), 0x12345678u);
    EXPECT_EQ(f3rt::read_be32(data, 2), 0x56789abcu);
    EXPECT_EQ(f3rt::read_be64(data.data()), 0x123456789abcdef0ull);

    std::array<uint8_t, 8> out{};
    f3rt::write_be<uint16_t>(out, 0, 0x1234);
    EXPECT_EQ(out[0], 0x12);
    EXPECT_EQ(out[1], 0x34);

    f3rt::write_be<uint32_t>(out, 2, 0xaabbccdd);
    EXPECT_EQ(out[2], 0xaa);
    EXPECT_EQ(out[3], 0xbb);
    EXPECT_EQ(out[4], 0xcc);
    EXPECT_EQ(out[5], 0xdd);

    EXPECT_EQ(f3rt::s16(0x10000), 0);
    EXPECT_EQ(f3rt::s16(0xffff), -1);
    EXPECT_EQ(f3rt::s16(0x7fff), 0x7fff);
    EXPECT_EQ(f3rt::s16(0x8000), -0x8000);
}
