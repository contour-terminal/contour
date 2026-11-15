// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string>
#include <string_view>

#include <orchestrator/core/Utf8.hpp>

namespace
{

struct DecodeRow
{
    std::string_view name;
    std::string_view bytes;
    char32_t codepoint;
    std::size_t length;
};

// Each row's bytes are followed by a trailing 'z', which the decoder must not consume.
constexpr auto DecodeRows = std::array {
    DecodeRow { .name = "ASCII", .bytes = "az", .codepoint = U'a', .length = 1 },
    DecodeRow { .name = "two bytes", .bytes = "\xC3\xBCz", .codepoint = U'ü', .length = 2 },
    DecodeRow { .name = "three bytes", .bytes = "\xE2\x80\xA6z", .codepoint = U'…', .length = 3 },
    DecodeRow { .name = "four bytes", .bytes = "\xF0\x9F\x98\x80z", .codepoint = U'\U0001F600', .length = 4 },
    DecodeRow { .name = "truncated", .bytes = "\xC3", .codepoint = U'�', .length = 1 },
    DecodeRow { .name = "truncated before ASCII", .bytes = "\xC3z", .codepoint = U'�', .length = 1 },
    DecodeRow { .name = "stray continuation", .bytes = "\x80z", .codepoint = U'�', .length = 1 },
    DecodeRow { .name = "overlong", .bytes = "\xC0\xAFz", .codepoint = U'�', .length = 1 },
    DecodeRow { .name = "surrogate", .bytes = "\xED\xA0\x80z", .codepoint = U'�', .length = 1 },
    DecodeRow { .name = "above range", .bytes = "\xF4\x90\x80\x80z", .codepoint = U'�', .length = 1 },
    DecodeRow { .name = "invalid lead", .bytes = "\xFFz", .codepoint = U'�', .length = 1 },
    DecodeRow { .name = "empty", .bytes = "", .codepoint = U'�', .length = 0 },
};

struct EncodeRow
{
    std::string_view name;
    char32_t codepoint;
    std::string_view bytes;
    char32_t decoded; ///< What the bytes decode back to: the code point, or U+FFFD for a non-scalar value.
};

constexpr auto EncodeRows = std::array {
    EncodeRow { .name = "ASCII", .codepoint = U'a', .bytes = "a", .decoded = U'a' },
    EncodeRow { .name = "two bytes", .codepoint = U'ü', .bytes = "\xC3\xBC", .decoded = U'ü' },
    EncodeRow { .name = "three bytes", .codepoint = U'…', .bytes = "\xE2\x80\xA6", .decoded = U'…' },
    EncodeRow { .name = "four bytes",
                .codepoint = U'\U0001F600',
                .bytes = "\xF0\x9F\x98\x80",
                .decoded = U'\U0001F600' },
    EncodeRow { .name = "largest",
                .codepoint = U'\U0010FFFF',
                .bytes = "\xF4\x8F\xBF\xBF",
                .decoded = U'\U0010FFFF' },
    EncodeRow { .name = "surrogate",
                .codepoint = char32_t { 0xD800 },
                .bytes = "\xEF\xBF\xBD",
                .decoded = U'\uFFFD' },
    EncodeRow { .name = "above range",
                .codepoint = char32_t { 0x110000 },
                .bytes = "\xEF\xBF\xBD",
                .decoded = U'\uFFFD' },
};

struct CodepointsRow
{
    std::string_view name;
    std::string_view bytes;
    std::u32string_view codepoints;
};

constexpr auto CodepointsRows = std::array {
    CodepointsRow { .name = "empty", .bytes = "", .codepoints = U"" },
    CodepointsRow { .name = "ASCII", .bytes = "ab", .codepoints = U"ab" },
    CodepointsRow { .name = "every length",
                    .bytes = "a\xC3\xBC\xE2\x80\xA6\xF0\x9F\x98\x80",
                    .codepoints = U"a\u00FC\u2026\U0001F600" },
    CodepointsRow { .name = "invalid byte", .bytes = "a\xFFz", .codepoints = U"a\uFFFDz" },
    CodepointsRow { .name = "truncated at the end", .bytes = "a\xC3", .codepoints = U"a\uFFFD" },
    CodepointsRow { .name = "truncated before ASCII", .bytes = "\xE2\x80z", .codepoints = U"\uFFFD\uFFFDz" },
};

/// The code points Codepoints yields for @p bytes.
constexpr auto collect(std::string_view bytes)
{
    auto codepoints = std::u32string {};
    for (auto const decoded: orchestrator::Codepoints { bytes })
        codepoints.push_back(decoded.codepoint);
    return codepoints;
}

// Codepoints walks in constant expressions too.
static_assert(collect("a\xE2\x80\xA6") == U"a\u2026");

// The decoder is usable in constant expressions.
static_assert(orchestrator::decodeUtf8("\xE2\x80\xA6").codepoint == U'…');
static_assert(orchestrator::decodeUtf8("\x80").length == 1);

// So is the encoder, through C++20's constexpr std::string.
static_assert([] {
    auto encoded = std::string {};
    orchestrator::appendUtf8(encoded, U'\U0001F600');
    return encoded.size() == 4 && orchestrator::decodeUtf8(encoded).codepoint == U'\U0001F600';
}());

} // namespace

TEST_CASE("Utf8", "[orchestrator][utf8]")
{
    SECTION("decodeUtf8")
    {
        for (auto const& row: DecodeRows)
        {
            CAPTURE(row.name, row.bytes, static_cast<uint32_t>(row.codepoint), row.length);
            auto const decoded = orchestrator::decodeUtf8(row.bytes);
            CHECK(static_cast<uint32_t>(decoded.codepoint) == static_cast<uint32_t>(row.codepoint));
            CHECK(decoded.length == row.length);
        }
    }

    SECTION("appendUtf8")
    {
        for (auto const& row: EncodeRows)
        {
            CAPTURE(row.name, static_cast<uint32_t>(row.codepoint), row.bytes);
            auto encoded = std::string { "x" };
            orchestrator::appendUtf8(encoded, row.codepoint);
            CHECK(encoded == std::string { "x" } + std::string { row.bytes });
        }
    }

    SECTION("Codepoints")
    {
        for (auto const& row: CodepointsRows)
        {
            CAPTURE(row.name, row.bytes, row.codepoints.size());
            CHECK(collect(row.bytes) == row.codepoints);
        }
    }

    SECTION("appendUtf8 round-trips through decodeUtf8")
    {
        for (auto const& row: EncodeRows)
        {
            CAPTURE(row.name, static_cast<uint32_t>(row.codepoint), static_cast<uint32_t>(row.decoded));
            auto encoded = std::string {};
            orchestrator::appendUtf8(encoded, row.codepoint);
            auto const decoded = orchestrator::decodeUtf8(encoded);
            CHECK(static_cast<uint32_t>(decoded.codepoint) == static_cast<uint32_t>(row.decoded));
            CHECK(decoded.length == encoded.size());
        }
    }
}
