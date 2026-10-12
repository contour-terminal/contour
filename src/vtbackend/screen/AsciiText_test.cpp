// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/screen/AsciiText.hpp>

#include <catch2/catch_test_macros.hpp>

#include <format>
#include <ranges>
#include <string>
#include <vector>

using namespace vtbackend;

TEST_CASE("isAsciiOnly.acceptsEveryAsciiByte", "[AsciiText]")
{
    CHECK(isAsciiOnly(""));
    auto all = std::string {};
    for (auto const byte: std::views::iota(0, 0x80))
        all.push_back(static_cast<char>(byte));
    CHECK(isAsciiOnly(all));
}

TEST_CASE("isAsciiOnly.findsAHighByteAtEveryPosition", "[AsciiText]")
{
    // The reduction has no early exit, so it must see a high byte wherever it sits: in the vectorized
    // body and in the scalar tail alike. Lengths up to 200 cover both at every vector width the
    // compiler may pick.
    auto misses = std::vector<std::string> {};
    for (auto const length: std::views::iota(size_t { 1 }, size_t { 201 }))
        for (auto const position: std::views::iota(size_t { 0 }, length))
            for (auto const highByte: { '\x80', '\xC3', '\xFF' })
            {
                auto text = std::string(length, 'a');
                text[position] = highByte;
                if (isAsciiOnly(text))
                    misses.push_back(std::format("length {}, byte 0x{:02X} at {}",
                                                 length,
                                                 static_cast<unsigned char>(highByte),
                                                 position));
            }
    CHECK(misses.empty());
}
