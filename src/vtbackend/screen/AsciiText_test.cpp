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

TEST_CASE("nonAsciiByteCount.countsEveryHighByte", "[AsciiText]")
{
    CHECK(nonAsciiByteCount("") == 0);
    CHECK(nonAsciiByteCount("plain ASCII") == 0);
    CHECK(nonAsciiByteCount("\xC3\xA4 and \xE2\x94\x82") == 5);

    auto all = std::string {};
    for (auto const byte: std::views::iota(0, 0x100))
        all.push_back(static_cast<char>(byte));
    CHECK(nonAsciiByteCount(all) == 0x80);
}

TEST_CASE("nonAsciiByteCount.countsAcrossItsBlocks", "[AsciiText]")
{
    // The count is kept in a byte per block of text, so every length around a block boundary must
    // still count every high byte: here every third one, and every one.
    for (auto const length: { 254uz, 255uz, 256uz, 509uz, 510uz, 511uz, 765uz, 1000uz })
    {
        INFO(std::format("length {}", length));
        auto everyThird = std::string(length, 'a');
        for (auto const position: std::views::iota(0uz, length) | std::views::stride(3))
            everyThird[position] = '\xFF';
        CHECK(nonAsciiByteCount(everyThird) == (length + 2) / 3);
        CHECK(nonAsciiByteCount(std::string(length, '\x80')) == length);
    }
}
