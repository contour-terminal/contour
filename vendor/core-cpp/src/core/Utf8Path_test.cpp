// SPDX-License-Identifier: Apache-2.0
//
// core::pathFromUtf8() and core::pathToUtf8() carry a path between core-cpp's UTF-8 strings and
// std::filesystem::path. What each case tells them apart from is the narrow constructor and
// path::string(), which on Windows read and write the ANSI code page: those agree with UTF-8 on
// ASCII and on nothing else, so every case that can fail spells a name beyond it. A path built from
// a u8 literal is UTF-8 by type on every platform, which is what the cases compare against.
#include <core/Escape.hpp>
#include <core/Utf8Path.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

using namespace std::string_view_literals;

namespace
{

/// "Jürgen": U+00FC is two bytes in UTF-8, which a CP-1252 host reads as two characters.
constexpr auto Juergen = "J\xC3\xBCrgen"sv;

/// "Ω-日本-😀": a Greek letter, two CJK ideographs and a character beyond the BMP, which UTF-16
/// spells as a surrogate pair. No ANSI code page spells all of them.
constexpr auto Mixed = "\xCE\xA9-\xE6\x97\xA5\xE6\x9C\xAC-\xF0\x9F\x98\x80"sv;

/// A code point spelled in UTF-8, beside the same code point in a u8 literal.
struct WellFormed
{
    std::string_view bytes;
    std::u8string_view expected;
};

/// The first and last code point of each sequence length, and the code points either side of the
/// surrogates, which a strict decoder refuses and must not refuse a neighbour of.
constexpr auto WellFormedCases = std::array {
    WellFormed { .bytes = "\xC2\x80"sv, .expected = u8"\u0080" },             // 2 bytes, first
    WellFormed { .bytes = "\xC3\xBC"sv, .expected = u8"\u00FC" },             // ü
    WellFormed { .bytes = "\xDF\xBF"sv, .expected = u8"\u07FF" },             // 2 bytes, last
    WellFormed { .bytes = "\xE0\xA0\x80"sv, .expected = u8"\u0800" },         // 3 bytes, first
    WellFormed { .bytes = "\xE6\x97\xA5"sv, .expected = u8"\u65E5" },         // 日
    WellFormed { .bytes = "\xED\x9F\xBF"sv, .expected = u8"\uD7FF" },         // below the surrogates
    WellFormed { .bytes = "\xEE\x80\x80"sv, .expected = u8"\uE000" },         // above them
    WellFormed { .bytes = "\xEF\xBF\xBD"sv, .expected = u8"\uFFFD" },         // the replacement character
    WellFormed { .bytes = "\xF0\x90\x80\x80"sv, .expected = u8"\U00010000" }, // 4 bytes, first
    WellFormed { .bytes = "\xF0\x9F\x98\x80"sv, .expected = u8"\U0001F600" }, // 😀
    WellFormed { .bytes = "\xF4\x8F\xBF\xBF"sv, .expected = u8"\U0010FFFF" }, // 4 bytes, last
};

/// Text that is not UTF-8, one way each. Every one is followed by ASCII, so a decoder that only
/// checks where a sequence ends cannot pass it by running out of input.
constexpr auto IllFormedCases = std::array {
    "J\xFCrgen"sv,              // "Jürgen" in CP-1252: 0xFC starts no sequence
    "a\x80z"sv,                 // a lone continuation byte
    "a\xE6\x97z"sv,             // a 3-byte sequence cut short
    "a\xE6\x97"sv,              // ... at the end of the text
    "a\xED\xA0\x80z"sv,         // U+D800, a high surrogate, encoded
    "a\xED\xBF\xBFz"sv,         // U+DFFF, a low surrogate, encoded
    "a\xC0\xAFz"sv,             // '/' as an overlong 2-byte form
    "a\xE0\x80\xAFz"sv,         // ... as an overlong 3-byte form
    "a\xF0\x80\x80\xAFz"sv,     // ... as an overlong 4-byte form
    "a\xF4\x90\x80\x80z"sv,     // U+110000, past the last code point
    "a\xF8\x88\x80\x80\x80z"sv, // a 5-byte form, which UTF-8 no longer has
};

} // namespace

TEST_CASE("pathFromUtf8 reads its text as UTF-8", "[Utf8Path]")
{
    CHECK(core::pathFromUtf8(Juergen) == std::filesystem::path { u8"Jürgen" });
    CHECK(core::pathFromUtf8(Mixed) == std::filesystem::path { u8"Ω-日本-😀" });
    CHECK(core::pathFromUtf8("C:/Users/J\xC3\xBCrgen/state")
          == std::filesystem::path { u8"C:/Users/Jürgen/state" });
}

TEST_CASE("pathFromUtf8 reads every sequence length", "[Utf8Path]")
{
    for (auto const& [bytes, expected]: WellFormedCases)
    {
        auto const path = core::pathFromUtf8(bytes);
        REQUIRE(path.has_value());
        CHECK(*path == std::filesystem::path { expected });
        CHECK(core::pathToUtf8(*path) == bytes);
    }
}

TEST_CASE("pathFromUtf8 keeps ASCII and the empty path as they are", "[Utf8Path]")
{
    CHECK(core::pathFromUtf8("").value().empty());
    CHECK(core::pathFromUtf8("logs/daemon.log") == std::filesystem::path { "logs/daemon.log" });
}

TEST_CASE("pathToUtf8 spells a path in UTF-8", "[Utf8Path]")
{
    CHECK(core::pathToUtf8(std::filesystem::path { u8"Jürgen" }) == Juergen);
    CHECK(core::pathToUtf8(std::filesystem::path { u8"Ω-日本-😀" }) == Mixed);
    CHECK(core::pathToUtf8(std::filesystem::path {}).empty());
}

TEST_CASE("pathToUtf8 spells any path without throwing", "[Utf8Path]")
{
#ifdef _WIN32
    // A Windows name is a sequence of UTF-16 code units, not of code points, and NTFS takes one
    // with a lone surrogate, which UTF-8 cannot spell. Such a name comes from outside the program
    // -- a directory listing, a path a peer sent -- so spelling it in a message is no defect, and
    // it reads as U+FFFD, as the rest of core-cpp's wide reads do. path::u8string() throws here.
    auto const replacement = std::string { "\xEF\xBF\xBD" };
    auto const high = wchar_t { 0xD800 };
    auto const low = wchar_t { 0xDC00 };
    CHECK(core::pathToUtf8(std::filesystem::path { std::wstring { L'a', high, L'b' } })
          == "a" + replacement + "b");
    CHECK(core::pathToUtf8(std::filesystem::path { std::wstring { L'a', low, L'b' } })
          == "a" + replacement + "b");
    // At the end of the name, and a pair in the wrong order, which is two lone surrogates.
    CHECK(core::pathToUtf8(std::filesystem::path { std::wstring { L'a', high } }) == "a" + replacement);
    CHECK(core::pathToUtf8(std::filesystem::path { std::wstring { low, high } })
          == replacement + replacement);
#else
    // A POSIX name is bytes, which are not always UTF-8; they pass through unchanged.
    CHECK(core::pathToUtf8(std::filesystem::path { "J\xFCrgen" }) == "J\xFCrgen"sv);
    CHECK(core::pathToUtf8(std::filesystem::path { "a\xED\xA0\x80z" }) == "a\xED\xA0\x80z"sv);
#endif
}

TEST_CASE("pathFromUtf8 and pathToUtf8 are each other's inverse", "[Utf8Path]")
{
    // Separators included: neither function rewrites one, so a path keeps its own.
    for (auto const text: { "a/b"sv, "a\\b"sv, Juergen, Mixed })
        CHECK(core::pathToUtf8(core::pathFromUtf8(text).value()) == text);
}

TEST_CASE("pathFromUtf8 on text that is not UTF-8", "[Utf8Path]")
{
    for (auto const text: IllFormedCases)
    {
        INFO("text: " << core::escape(text));
#ifdef _WIN32
        // A Windows path is UTF-16, so the bytes must be converted, and they cannot be. Text from
        // outside the program is often not UTF-8, so that is an answer, not an exception: one
        // thrown here fails the case.
        auto const path = core::pathFromUtf8(text);
        REQUIRE_FALSE(path.has_value());
        CHECK(path.error() == std::errc::illegal_byte_sequence);
#else
        // A POSIX path is bytes, so they are kept as they are.
        CHECK(core::pathFromUtf8(text).value().native() == text);
#endif
    }
}

TEST_CASE("pathFromTrustedUtf8 reads what pathFromUtf8 reads", "[Utf8Path]")
{
    CHECK(core::pathFromTrustedUtf8(Mixed) == std::filesystem::path { u8"Ω-日本-😀" });
    for (auto const& [bytes, expected]: WellFormedCases)
        CHECK(core::pathFromTrustedUtf8(bytes) == std::filesystem::path { expected });
}

TEST_CASE("pathFromTrustedUtf8 on text that is not UTF-8", "[Utf8Path]")
{
    for (auto const text: IllFormedCases)
    {
        INFO("text: " << core::escape(text));
#ifdef _WIN32
        // Text the program spelled itself that is not UTF-8 is a defect at the call site.
        CHECK_THROWS_AS(core::pathFromTrustedUtf8(text), std::system_error);
#else
        CHECK(core::pathFromTrustedUtf8(text).native() == text);
#endif
    }
}
