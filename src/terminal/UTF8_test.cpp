/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019 Christian Parpart <christian@parpart.family>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <terminal/UTF8.h>
#include <catch2/catch.hpp>
#include <fmt/format.h>
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

TEST_CASE("bytes_1", "[utf8]")
{
    uint8_t C = '[';

    // encode
    auto encoded = utf8::encode(C);
    REQUIRE(encoded == utf8::Bytes{C});

    // decode
    auto d = utf8::Decoder{};
    auto a = d.decode(encoded[0]);
    REQUIRE(holds_alternative<utf8::Decoder::Success>(a));
    wchar_t b = get<utf8::Decoder::Success>(a).value;
    INFO(fmt::format("wchar_t : 0x{:04X}", (unsigned)b));
    REQUIRE(b == C);
}

TEST_CASE("bytes_2", "[utf8]")
{
    wchar_t C = U'ö'; // 0xC3 0xB6
    INFO(fmt::format("C : 0x{:04X}", (unsigned)C));

    // encode
    auto encoded = utf8::encode(C);
    auto es = utf8::to_string(encoded);
    INFO(fmt::format("es : '{}'", es));
    INFO(fmt::format("   : {:X} {:X}", encoded[0], encoded[1]));
    REQUIRE((encoded == utf8::Bytes{0xC3, 0xB6}));

    // decode
    auto d = utf8::Decoder{};
    auto a = d.decode(encoded[0], encoded[1]);
    REQUIRE(holds_alternative<utf8::Decoder::Success>(a));
    wchar_t b = get<utf8::Decoder::Success>(a).value;
    INFO(fmt::format("wchar_t : 0x{:04X} ==? 0x{:04X}", (unsigned)b, (unsigned)C));
    REQUIRE(b == C);
}

TEST_CASE("bytes_3", "[utf8]")
{
    // encode
    auto bytes = utf8::encode(0x20AC); // EURO sign: €
    REQUIRE((bytes == utf8::Bytes{0xE2, 0x82, 0xAC}));

    auto const& b3 = bytes;
    INFO(fmt::format("{:02X} {:02X} {:02X}", b3[0], b3[1], b3[2]));
    INFO(fmt::format("{} {} {}", binstr(b3[0]), binstr(b3[1]), binstr(b3[2])));

    // decode
    auto d = utf8::Decoder{};
    auto a = d.decode(b3[0], b3[1], b3[2]);
    REQUIRE(holds_alternative<utf8::Decoder::Success>(a));
    wchar_t b = get<utf8::Decoder::Success>(a).value;
    INFO(fmt::format("wchar_t : 0x{:04X}", (unsigned)b));
    REQUIRE(b == 0x20AC);
}

TEST_CASE("bytes_3_dash", "[utf8]")
{
    auto decode = utf8::Decoder{};
    using Success = utf8::Decoder::Success;

    // Decode #1
    INFO(fmt::format("wchar_t for |-: {}",
            static_cast<uint32_t>(
                get<Success>(
                    decode(0xE2, 0x94, 0x9C)).value)));
    REQUIRE((
            utf8::encode(
                get<Success>(
                    decode(0xE2, 0x94, 0x9C)
                ).value
            )
            == utf8::Bytes{0xE2, 0x94, 0x9C}));

    // decode #2
    auto a = decode(0xE2, 0x94, 0x80);
    REQUIRE(holds_alternative<utf8::Decoder::Success>(a));
    wchar_t b = get<utf8::Decoder::Success>(a).value;
    INFO(fmt::format("wchar_t : 0x{:04X}", (unsigned)b));

    // encode
    auto bytes = utf8::encode(b);
    REQUIRE(bytes == (utf8::Bytes{0xE2, 0x94, 0x80}));

    auto const& b3 = bytes;
    INFO(fmt::format("{:02X} {:02X} {:02X}", b3[0], b3[1], b3[2]));
    INFO(fmt::format("{} {} {}", binstr(b3[0]), binstr(b3[1]), binstr(b3[2])));
}

TEST_CASE("bytes_4", "[utf8]")
{
    // TODO
}

TEST_CASE("to_string_2", "[utf8]")
{
    wchar_t C = U'ö'; // 0xF6 (UTF8: 0xC3 0xB6)

    auto const encoded = utf8::encode(C);
    REQUIRE(2 == encoded.size());
    REQUIRE(0xC3 == encoded[0]);
    REQUIRE(0xB6 == encoded[1]);
    INFO(fmt::format("encoded: '{}'", to_string(encoded)));
}
