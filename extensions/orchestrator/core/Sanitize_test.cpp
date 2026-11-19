// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <orchestrator/core/Sanitize.hpp>

using namespace std::string_view_literals;

namespace
{

struct DisplayRow
{
    std::string_view input;
    std::size_t maxChars;
    std::string_view expected;
};

/// The array's characters as a string_view.
template <std::size_t N>
constexpr auto viewOf(std::array<char, N> const& text)
{
    return std::string_view { text.data(), text.size() };
}

/// The brief's "abc\u202e" "def", spelled as bytes: a string literal holding an unterminated override is
/// exactly what misc-misleading-bidirectional rejects in source.
constexpr auto RightToLeftOverride = std::array { 'a', 'b', 'c', '\xE2', '\x80', '\xAE', 'd', 'e', 'f' };

constexpr auto DisplayRows = std::array {
    DisplayRow { .input = "plain title", .maxChars = 40, .expected = "plain title" },
    DisplayRow { .input = "\x1b[31mred\x1b[0m", .maxChars = 40, .expected = "[31mred[0m" },
    DisplayRow { .input = "title\x1b]0;pwned\x07", .maxChars = 40, .expected = "title]0;pwned" },
    DisplayRow { .input = "a\r\nb\tc", .maxChars = 40, .expected = "a b c" },
    DisplayRow { .input = "a\u2028b\u2029c", .maxChars = 40, .expected = "a b c" },
    DisplayRow { .input = "  spaced   out  ", .maxChars = 40, .expected = "spaced out" },
    DisplayRow { .input = "\u009b"
                          "31m csi",
                 .maxChars = 40,
                 .expected = "31m csi" },
    DisplayRow { .input = viewOf(RightToLeftOverride), .maxChars = 40, .expected = "abcdef" },
    DisplayRow { .input = "abc\u200f\u061c"
                          "def",
                 .maxChars = 40,
                 .expected = "abcdef" },
    DisplayRow { .input = "abcdefghij", .maxChars = 5, .expected = "abcd…" },
    DisplayRow { .input = "ünïcödé", .maxChars = 3, .expected = "ün…" },
    DisplayRow { .input = "ab\xC3", .maxChars = 40, .expected = "ab" },
    DisplayRow { .input = "", .maxChars = 5, .expected = "" },
    // Beyond the brief's table: the edges of the rules above.
    DisplayRow { .input = "a\x7f\x80"
                          "b",
                 .maxChars = 40,
                 .expected = "ab" },
    DisplayRow { .input = "a\u2066b\u2069c\u200e", .maxChars = 40, .expected = "abc" },
    DisplayRow { .input = "a\xFF\xC3z", .maxChars = 40, .expected = "az" },
    DisplayRow { .input = "a\ufffdb", .maxChars = 40, .expected = "a\ufffdb" },
    DisplayRow { .input = "abcde", .maxChars = 5, .expected = "abcde" },
    DisplayRow { .input = "abc  \t ", .maxChars = 3, .expected = "abc" },
    DisplayRow { .input = "ab   cd", .maxChars = 4, .expected = "ab…" },
    DisplayRow { .input = "a bc", .maxChars = 3, .expected = "a…" },
    DisplayRow { .input = "abc", .maxChars = 1, .expected = "…" },
    DisplayRow { .input = "abc", .maxChars = 0, .expected = "" },
    DisplayRow { .input = "\x1b\r\n\t ", .maxChars = 40, .expected = "" },
};

/// What a note row expects of singleLineNote.
enum class NoteExpectation : uint8_t
{
    Refused,
    Accepted,
};

struct NoteRow
{
    std::string_view input;
    NoteExpectation expectation;
    std::string_view expected;     ///< The note, when accepted.
    orchestrator::NoteError error; ///< The reason, when refused.
};

/// @p Count copies of 'x', then @p Tail copies of @p tail.
template <std::size_t Count, std::size_t Tail = 0>
constexpr auto xsThen(char tail = 'x')
{
    auto text = std::array<char, Count + Tail> {};
    text.fill('x');
    for (auto& c: std::span(text).last(Tail))
        c = tail;
    return text;
}

constexpr auto Longest = xsThen<orchestrator::MaxNoteChars>();
constexpr auto TooLong = xsThen<orchestrator::MaxNoteChars + 1>();
// Controls do not shorten the input that is measured: 500 characters plus one ESC is too long.
constexpr auto TooLongWithControl = xsThen<orchestrator::MaxNoteChars, 1>('\x1b');

constexpr auto NoteRows = std::array {
    NoteRow { .input = "keep the order",
              .expectation = NoteExpectation::Accepted,
              .expected = "keep the order",
              .error = {} },
    NoteRow { .input = "  keep\r\nit  ",
              .expectation = NoteExpectation::Accepted,
              .expected = "keep it",
              .error = {} },
    NoteRow { .input = viewOf(Longest),
              .expectation = NoteExpectation::Accepted,
              .expected = viewOf(Longest),
              .error = {} },
    NoteRow { .input = "-p evil",
              .expectation = NoteExpectation::Refused,
              .expected = {},
              .error = orchestrator::NoteError::StartsWithDash },
    NoteRow { .input = "\x1b-p",
              .expectation = NoteExpectation::Refused,
              .expected = {},
              .error = orchestrator::NoteError::StartsWithDash },
    NoteRow { .input = viewOf(TooLong),
              .expectation = NoteExpectation::Refused,
              .expected = {},
              .error = orchestrator::NoteError::TooLong },
    NoteRow { .input = viewOf(TooLongWithControl),
              .expectation = NoteExpectation::Refused,
              .expected = {},
              .error = orchestrator::NoteError::TooLong },
};

} // namespace

TEST_CASE("Sanitize", "[orchestrator][sanitize]")
{
    SECTION("displayText")
    {
        for (auto const& row: DisplayRows)
        {
            CAPTURE(row.input, row.maxChars, row.expected);
            CHECK(orchestrator::displayText(row.input, row.maxChars) == row.expected);
        }
    }

    SECTION("singleLineNote")
    {
        for (auto const& row: NoteRows)
        {
            CAPTURE(row.input, row.expectation, row.expected, row.error);
            auto const note = orchestrator::singleLineNote(row.input);
            REQUIRE(note.has_value() == (row.expectation == NoteExpectation::Accepted));
            if (note)
                CHECK(*note == row.expected);
            else
                CHECK(note.error() == row.error);
        }
    }
}

TEST_CASE("holdsInvisible", "[orchestrator][sanitize]")
{
    // One code point of each range of the table, spelled in UTF-8, after a visible prefix.
    for (auto const invisible: { "\u00AD"sv,
                                 "\u034F"sv,
                                 "\u115F"sv,
                                 "\u1160"sv,
                                 "\u180E"sv,
                                 "\u200B"sv,
                                 "\u200C"sv,
                                 "\u200D"sv,
                                 "\u2060"sv,
                                 "\u2064"sv,
                                 "\u3164"sv,
                                 "\uFE00"sv,
                                 "\uFE0F"sv,
                                 "\uFEFF"sv,
                                 "\uFFA0"sv,
                                 "\U000E0001"sv,
                                 "\U000E007F"sv,
                                 "\U000E0100"sv,
                                 "\U000E01EF"sv })
    {
        auto const text = std::string { "/w/a" } + std::string { invisible };
        CAPTURE(text);
        CHECK(orchestrator::holdsInvisible(text));
        // displayText() keeps them: they are part of text people write.
        CHECK(orchestrator::displayText(text, 100) == text);
    }
    for (auto const visible:
         { ""sv, "/w/it's a test"sv, "語"sv, "\u200A"sv, "\u2065"sv, "\uFE10"sv, "\xff"sv })
    {
        CAPTURE(visible);
        CHECK(!orchestrator::holdsInvisible(visible));
    }
}

TEST_CASE("displayLines keeps indentation and drops controls", "[orchestrator][sanitize]")
{
    CHECK(orchestrator::displayLines("  - a\n\t- b\r\n\x1b[31mc", 10, 80)
          == std::vector<std::string> { "  - a", "    - b", "[31mc" });
    // A final LF ends the last line; an empty line stays one; a line of spaces is empty.
    CHECK(orchestrator::displayLines("a\n\n   \nb\n", 10, 80)
          == std::vector<std::string> { "a", "", "", "b" });
    // A lone CR is dropped, as is any other control character.
    CHECK(orchestrator::displayLines("a\rb\x07", 10, 80) == std::vector<std::string> { "ab" });
    // Indentation is capped; the rest is cut as displayText() cuts it.
    CHECK(orchestrator::displayLines("\t\t\t\t\t\t\t\t\t\t\tx", 10, 80)
          == std::vector<std::string> { std::string(orchestrator::MaxIndentColumns, ' ') + "x" });
    CHECK(orchestrator::displayLines("  abcdef", 10, 4) == std::vector<std::string> { "  abc…" });
    CHECK(orchestrator::displayLines("", 10, 80).empty());
}

TEST_CASE("displayLines cuts at maxLines", "[orchestrator][sanitize]")
{
    CHECK(orchestrator::displayLines("a\nb\nc", 2, 80) == std::vector<std::string> { "a", "…" });
    CHECK(orchestrator::displayLines("a\nb", 2, 80) == std::vector<std::string> { "a", "b" });
    CHECK(orchestrator::displayLines("a\nb\n", 2, 80) == std::vector<std::string> { "a", "b" });
    CHECK(orchestrator::displayLines("a\nb", 0, 80).empty());
}
