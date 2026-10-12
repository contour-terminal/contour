// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/screen/AsciiText.hpp>
#include <vtbackend/testing/Workloads.hpp>

#include <vtparser/Utf8Decoding.hpp>

#include <core/Escape.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <format>
#include <random>
#include <ranges>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

using namespace vtbackend;
using namespace std::string_view_literals;

TEST_CASE("isAsciiByte.splitsAtTheHighBit", "[AsciiText]")
{
    for (auto const byte: std::views::iota(0, 0x100))
    {
        INFO(std::format("byte 0x{:02X}", byte));
        CHECK(isAsciiByte(static_cast<char>(byte)) == (byte < 0x80));
    }
}

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

TEST_CASE("asciiPrefixLength.stopsAtTheFirstHighByte", "[AsciiText]")
{
    // Blocks are tested whole, so the first high byte must be found wherever it sits in its block, in
    // the first block and in later ones, and behind a second high byte later in the same block.
    auto misses = std::vector<std::string> {};
    for (auto const length: std::views::iota(size_t { 0 }, (3 * AsciiScanBlockSize) + 3))
    {
        if (auto const measured = asciiPrefixLength(std::string(length, 'a')); measured != length)
            misses.push_back(std::format("length {} without a high byte: {}", length, measured));
        for (auto const position: std::views::iota(size_t { 0 }, length))
        {
            auto text = std::string(length, 'a');
            text[position] = '\xC3';
            if (position + 1 < length)
                text[length - 1] = '\x80';
            if (auto const measured = asciiPrefixLength(text); measured != position)
                misses.push_back(std::format("length {}, high byte at {}: {}", length, position, measured));
        }
    }
    CHECK(misses.empty());
}

TEST_CASE("isDecoderIdleAt.knownBytes", "[AsciiText]")
{
    struct Row
    {
        std::string_view text; ///< Decoded from its first byte; asked about its end.
        bool idle;             ///< Whether the decoder is idle after all of it.
    };
    auto const rows = std::array {
        Row { .text = ""sv, .idle = true },
        Row { .text = "a"sv, .idle = true },
        Row { .text = "\xC3"sv, .idle = false },            // a two-byte lead
        Row { .text = "\xC3\xA4"sv, .idle = true },         // ... and its continuation
        Row { .text = "\xE2\x94"sv, .idle = false },        // a three-byte character, one short
        Row { .text = "\xE2\x94\x82"sv, .idle = true },     // ... complete
        Row { .text = "\xF0\x9F\x98"sv, .idle = false },    // a four-byte character, one short
        Row { .text = "\xF0\x9F\x98\x80"sv, .idle = true }, // ... complete
        Row { .text = "\xF0"
                      "ab"sv,
              .idle = false }, // ASCII taken as continuation bytes
        Row { .text = "\xF0"
                      "abc"sv,
              .idle = true },                           // ... until the character is complete
        Row { .text = "\xFF"sv, .idle = true },         // never a lead byte
        Row { .text = "\x80"sv, .idle = true },         // a stray continuation byte
        Row { .text = "\xC3\xE2"sv, .idle = false },    // a lead byte starts over
        Row { .text = "\xE2\xC3\xA4"sv, .idle = true }, // ... and completes on its own
        Row { .text = "abc\xE2\x94\x82"
                      "d"sv,
              .idle = true },
    };
    for (auto const& row: rows)
    {
        INFO(std::format("text \"{}\"", core::escape(row.text)));
        CHECK(isDecoderIdleAt(row.text, row.text.size()) == row.idle);
    }
}

TEST_CASE("isDecoderIdleAt.agreesWithTheDecoderAtEveryOffset", "[AsciiText]")
{
    // Random bytes, biased towards lead and continuation bytes, against the decoder the text paths
    // decode with: after every prefix, idle exactly when the decoder expects no further byte.
    constexpr auto Alphabet = std::string_view { "ab\x80\xA4\xBF\xC3\xD8\xE2\xEF\xF0\xF7\xF8\xFF" };
    auto misses = std::vector<std::string> {};
    for (auto const seed: std::views::iota(1u, 2001u))
    {
        auto rng = std::mt19937 { seed };
        auto text = std::string {};
        for ([[maybe_unused]] auto const byte: std::views::iota(0, test::detail::drawBetween(rng, 1, 12)))
            text += Alphabet[static_cast<size_t>(
                test::detail::drawBetween(rng, 0, static_cast<int>(Alphabet.size()) - 1))];
        auto decoder = unicode::utf8_decoder_state {};
        for (auto const offset: std::views::iota(size_t { 0 }, text.size() + 1))
        {
            if (isDecoderIdleAt(text, offset) != (decoder.expectedLength == 0))
                misses.push_back(std::format("seed {}: \"{}\" at {}", seed, core::escape(text), offset));
            if (offset < text.size())
                std::ignore = vtparser::decodeUtf8Byte(decoder, static_cast<uint8_t>(text[offset]));
        }
    }
    CHECK(misses.empty());
}

TEST_CASE("isTooDenseForBulkAscii.rowsAtTheBoundary", "[AsciiText]")
{
    struct Row
    {
        size_t byteCount;
        size_t cellCount;
        bool dense;
    };
    auto const rows = std::array {
        Row { .byteCount = 40, .cellCount = 40, .dense = false }, // ASCII only
        Row { .byteCount = 6, .cellCount = 5, .dense = true },    // every fifth character a two-byte one
        Row { .byteCount = 42, .cellCount = 40, .dense = false }, // one box-drawing character in 40
        Row { .byteCount = 7, .cellCount = 6, .dense = false },   // exactly a stretch per character
        Row { .byteCount = 8, .cellCount = 6, .dense = true },    // ... and one byte more
        Row { .byteCount = 3, .cellCount = 4, .dense = false },   // more columns than bytes
    };
    for (auto const& row: rows)
    {
        INFO(std::format("{} bytes, {} columns", row.byteCount, row.cellCount));
        CHECK(isTooDenseForBulkAscii(row.byteCount, row.cellCount) == row.dense);
    }
}

namespace
{
/// The interiors forEachBulkAsciiInterior finds, as "begin-end" offsets.
std::string interiorsOf(std::string_view text, size_t cellCount)
{
    auto out = std::string {};
    forEachBulkAsciiInterior(text, cellCount, [&](size_t begin, size_t end) {
        out += std::format("{}{}-{}", out.empty() ? "" : " ", begin, end);
    });
    return out;
}
} // namespace

TEST_CASE("forEachBulkAsciiInterior.findsTheInteriors", "[AsciiText]")
{
    struct Row
    {
        std::string_view name;      ///< What the row covers, for the failure message.
        std::string_view text;      ///< The run.
        size_t cellCount;           ///< The columns it takes; 0 for as many as it has bytes.
        std::string_view interiors; ///< What the planner must find, as "begin-end" offsets.
    };
    auto const rows = std::array {
        Row { .name = "a stretch at the start",
              .text = "abcdefgh\xC3\xA4"sv,
              .cellCount = 9,
              .interiors = "1-8"sv },
        Row { .name = "a stretch at the end",
              .text = "\xC3\xA4"
                      "abcdefgh"sv,
              .cellCount = 9,
              .interiors = "3-10"sv },
        Row { .name = "two stretches",
              .text = "abcdefgh\xC3\xA4ijklmnop"sv,
              .cellCount = 17,
              .interiors = "1-8 11-18"sv },
        Row { .name = "a stretch longer than a block",
              .text = "\xC3\xA4"
                      "abcdefghijklmnopqrstuvwxyz0123456789ABCDEFGH\xC3\xA4"sv,
              .cellCount = 46,
              .interiors = "3-46"sv },
        Row { .name = "an interior of exactly the minimum",
              .text = "\xC3\xA4"
                      "abcde\xC3\xA4"sv,
              .cellCount = 0,
              .interiors = "3-7"sv },
        Row { .name = "an interior one byte short of the minimum",
              .text = "\xC3\xA4"
                      "abcd\xC3\xA4"sv,
              .cellCount = 0,
              .interiors = ""sv },
        Row { .name = "a Prepend character before a stretch: its first byte is held back",
              .text = "\xD8\x80"
                      "12345678"sv,
              .cellCount = 0,
              .interiors = "3-10"sv },
        Row { .name = "a combining mark after a stretch: its last byte is not held back",
              .text = "\xC3\xA4"
                      "abcdefgh\xCC\x81"sv,
              .cellCount = 0,
              .interiors = "3-10"sv },
        Row { .name = "VS16 after a stretch",
              .text = "abcdefgh\xEF\xB8\x8F"sv,
              .cellCount = 0,
              .interiors = "1-8"sv },
        Row { .name = "a two-byte lead right before a stretch",
              .text = "\xC3"
                      "abcdefgh"sv,
              .cellCount = 0,
              .interiors = ""sv },
        Row { .name = "a three-byte character one byte short before a stretch",
              .text = "\xE2\x94"
                      "abcdefgh"sv,
              .cellCount = 0,
              .interiors = ""sv },
        Row { .name = "a four-byte character one byte short before a stretch",
              .text = "\xF0\x9F\x98"
                      "abcdefgh"sv,
              .cellCount = 0,
              .interiors = ""sv },
        Row { .name = "a stretch whose first byte completes a character",
              .text = "\xF0"
                      "a\x80"
                      "bcdefghij"sv,
              .cellCount = 0,
              .interiors = ""sv },
        Row { .name = "a stretch after a character that ASCII completed",
              .text = "\xF0"
                      "ab\x80"
                      "cdefghij"sv,
              .cellCount = 0,
              .interiors = "5-12"sv },
        Row { .name = "invalid bytes on each side",
              .text = "\xFF"
                      "abcdefgh\xFF"sv,
              .cellCount = 0,
              .interiors = "2-9"sv },
        Row { .name = "stray continuation bytes on each side",
              .text = "\x80"
                      "abcdefgh\x80"sv,
              .cellCount = 0,
              .interiors = "2-9"sv },
        Row { .name = "a lead byte after a stretch",
              .text = "\xC3\xA4"
                      "abcdefgh\xE2"sv,
              .cellCount = 0,
              .interiors = "3-10"sv },
        Row { .name = "dense text, even with a long stretch in it",
              .text = "\xE4\xB8\xAD\xE4\xB8\xAD\xE4\xB8\xAD\xE4\xB8\xAD"
                      "abcdefgh\xE4\xB8\xAD\xE4\xB8\xAD\xE4\xB8\xAD\xE4\xB8\xAD"sv,
              .cellCount = 24,
              .interiors = ""sv },
        Row { .name = "the same text measured as sparse",
              .text = "\xE4\xB8\xAD\xE4\xB8\xAD\xE4\xB8\xAD\xE4\xB8\xAD"
                      "abcdefgh\xE4\xB8\xAD\xE4\xB8\xAD\xE4\xB8\xAD\xE4\xB8\xAD"sv,
              .cellCount = 0,
              .interiors = "13-20"sv },
        Row { .name = "every fifth character a two-byte one",
              .text = "abcd\xC3\xA4"
                      "efgh\xC3\xA4ijkl\xC3\xA4"sv,
              .cellCount = 15,
              .interiors = ""sv },
    };
    for (auto const& row: rows)
    {
        INFO(std::format("{}: \"{}\"", row.name, core::escape(row.text)));
        auto const cellCount = row.cellCount != 0 ? row.cellCount : row.text.size();
        CHECK(interiorsOf(row.text, cellCount) == row.interiors);
    }
}
