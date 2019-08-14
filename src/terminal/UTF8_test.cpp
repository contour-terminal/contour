#include <terminal/UTF8.h>
#include <util/testing.h>
#include <cstdlib>
#include <cassert>
using namespace std;

std::string binstr(unsigned n)
{
    string ss;
    for (int i = 7; i >= 0; --i)
        ss += ((n & (1 << i)) != 0) ? '1' : '0';
    return ss;
}

TEST(UTF8, bytes_1)
{
    uint8_t C = '[';

    // encode
    auto encoded = utf8::encode(C);
    ASSERT_TRUE(encoded == utf8::Bytes{C});

    // decode
    auto d = utf8::Decoder{};
    auto a = d.decode(encoded[0]);
    ASSERT_TRUE(holds_alternative<utf8::Decoder::Success>(a));
    wchar_t b = get<utf8::Decoder::Success>(a).value;
    logf("wchar_t : 0x{:04X}", (unsigned)b);
    ASSERT_TRUE(b == C);
}

TEST(UTF8, bytes_2)
{
    wchar_t C = U'ö'; // 0xC3 0xB6
    logf("C : 0x{:04X}", (unsigned)C);

    // encode
    auto encoded = utf8::encode(C);
    auto es = utf8::to_string(encoded);
    logf("es : '{}'", es);
    logf("   : {:X} {:X}", encoded[0], encoded[1]);
    ASSERT_TRUE((encoded == utf8::Bytes{0xC3, 0xB6}));

    // decode
    auto d = utf8::Decoder{};
    auto a = d.decode(encoded[0], encoded[1]);
    ASSERT_TRUE(holds_alternative<utf8::Decoder::Success>(a));
    wchar_t b = get<utf8::Decoder::Success>(a).value;
    logf("wchar_t : 0x{:04X} ==? 0x{:04X}", (unsigned)b, (unsigned)C);
    ASSERT_TRUE(b == C);
}

TEST(UTF8, bytes_3)
{
    // encode
    auto bytes = utf8::encode(0x20AC); // EURO sign: €
    ASSERT_TRUE((bytes == utf8::Bytes{0xE2, 0x82, 0xAC}));

    auto const& b3 = bytes;
    logf("{:02X} {:02X} {:02X}", b3[0], b3[1], b3[2]);
    logf("{} {} {}", binstr(b3[0]), binstr(b3[1]), binstr(b3[2]));

    // decode
    auto d = utf8::Decoder{};
    auto a = d.decode(b3[0], b3[1], b3[2]);
    ASSERT_TRUE(holds_alternative<utf8::Decoder::Success>(a));
    wchar_t b = get<utf8::Decoder::Success>(a).value;
    logf("wchar_t : 0x{:04X}", (unsigned)b);
    ASSERT_TRUE(b == 0x20AC);
}

TEST(UTF8, bytes_3_dash)
{
    auto decode = utf8::Decoder{};
    using Success = utf8::Decoder::Success;

    // Decode #1
    logf("wchar_t for |-: {}",
            static_cast<uint32_t>(
                get<Success>(
                    decode(0xE2, 0x94, 0x9C)).value));
    ASSERT_TRUE((
            utf8::encode(
                get<Success>(
                    decode(0xE2, 0x94, 0x9C)
                ).value
            )
            == utf8::Bytes{0xE2, 0x94, 0x9C}));

    // decode #2
    auto a = decode(0xE2, 0x94, 0x80);
    ASSERT_TRUE(holds_alternative<utf8::Decoder::Success>(a));
    wchar_t b = get<utf8::Decoder::Success>(a).value;
    logf("wchar_t : 0x{:04X}", (unsigned)b);

    // encode
    auto bytes = utf8::encode(b);
    ASSERT_TRUE((bytes == utf8::Bytes{0xE2, 0x94, 0x80}));

    auto const& b3 = bytes;
    logf("{:02X} {:02X} {:02X}", b3[0], b3[1], b3[2]);
    logf("{} {} {}", binstr(b3[0]), binstr(b3[1]), binstr(b3[2]));
}

TEST(UTF8, bytes_4)
{
    // TODO
}

TEST(UTF8, to_string_2)
{
    wchar_t C = U'ö'; // 0xF6 (UTF8: 0xC3 0xB6)

    auto const encoded = utf8::encode(C);
    EXPECT_EQ(2, encoded.size());
    EXPECT_EQ(0xC3, encoded[0]);
    EXPECT_EQ(0xB6, encoded[1]);
    logf("encoded: '{}'", to_string(encoded));
}
