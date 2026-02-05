// SPDX-License-Identifier: Apache-2.0
#include <vtparser/Parser.h>
#include <vtparser/ParserEvents.h>

#include <catch2/catch_test_macros.hpp>

#include <libunicode/convert.h>

using namespace std;

class MockParserEvents final: public vtparser::NullParserEvents
{
  public:
    std::string text;
    std::string apc;
    std::string pm;
    size_t maxCharCount = 80;

    void error(string_view const& msg) override { INFO(std::format("Parser error received. {}", msg)); }
    void print(char32_t ch) override { text += unicode::convert_to<char>(ch); }
    size_t print(std::string_view s, size_t cellCount) override
    {
        text += s;
        return maxCharCount -= cellCount;
    }

    void startAPC() override { apc += "{"; }
    void putAPC(char ch) override { apc += ch; }
    void dispatchAPC() override { apc += "}"; }

    void startPM() override { pm += "{"; }
    void putPM(char ch) override { pm += ch; }
    void dispatchPM() override { pm += "}"; }
};

TEST_CASE("Parser.utf8_single", "[Parser]")
{
    MockParserEvents textListener;
    auto p = vtparser::Parser<vtparser::ParserEvents>(textListener);

    p.parseFragment("\xC3\xB6"); // ö

    REQUIRE(textListener.text == "\xC3\xB6");
}

TEST_CASE("Parser.PM")
{
    MockParserEvents listener;
    auto p = vtparser::Parser<vtparser::ParserEvents>(listener);
    REQUIRE(p.state() == vtparser::State::Ground);
    // Also include ✅ in the payload to ensure such codepoints work, too.
    p.parseFragment("ABC\033^hello ✅ world\033\\DEF"sv);
    CHECK(p.state() == vtparser::State::Ground);
    CHECK(listener.pm == "{hello ✅ world}");
    CHECK(listener.text == "ABCDEF");
}

TEST_CASE("Parser.APC")
{
    MockParserEvents listener;
    auto p = vtparser::Parser<vtparser::ParserEvents>(listener);
    REQUIRE(p.state() == vtparser::State::Ground);
    p.parseFragment("ABC\033\\\033_Gi=1,a=q;\033\\DEF"sv);
    REQUIRE(p.state() == vtparser::State::Ground);
    REQUIRE(listener.apc == "{Gi=1,a=q;}");
    REQUIRE(listener.text == "ABCDEF");
}

TEST_CASE("Parser.BulkText_IncompleteUtf8_SplitAcrossCalls", "[Parser]")
{
    // This test reproduces a bug where text before incomplete UTF-8 was not printed.
    // The box-drawing character "│" (U+2502) is E2 94 82 in UTF-8 (3 bytes).
    // When split across parse calls, the text before the incomplete sequence must still be printed.

    MockParserEvents listener;
    auto p = vtparser::Parser<vtparser::ParserEvents>(listener);

    // First parse call: "ABC" followed by first 2 bytes of "│" (E2 94)
    // The text "ABC" should be printed even though UTF-8 is incomplete.
    p.parseFragment("ABC\xE2\x94"sv);

    // BUG (before fix): listener.text would be empty because the entire text was skipped
    // when incomplete UTF-8 was detected. This caused visual corruption in terminal output.
    CHECK(listener.text == "ABC");

    // Second parse call: remaining byte of "│" (82) followed by "DEF"
    p.parseFragment("\x82"
                    "DEF"sv);

    // The complete string should now include the box-drawing character
    CHECK(listener.text
          == "ABC\xE2\x94\x82"
             "DEF");
}

TEST_CASE("Parser.BulkText_IncompleteUtf8_SingleLeadingByte", "[Parser]")
{
    // Test with just a single leading byte of a multi-byte UTF-8 sequence.
    // E2 starts a 3-byte UTF-8 sequence (U+2000-U+2FFF range).

    MockParserEvents listener;
    auto p = vtparser::Parser<vtparser::ParserEvents>(listener);

    // "Hello" followed by just the first byte of a 3-byte sequence
    p.parseFragment("Hello\xE2"sv);
    CHECK(listener.text == "Hello");

    // Complete the sequence with remaining 2 bytes of "├" (U+251C = E2 94 9C)
    p.parseFragment("\x94\x9C"
                    "World"sv);
    CHECK(listener.text
          == "Hello\xE2\x94\x9C"
             "World");
}

TEST_CASE("Parser.BulkText_MultipleIncompleteUtf8Splits", "[Parser]")
{
    // Test multiple incomplete UTF-8 sequences in succession.
    // This simulates rapid PTY reads that frequently split multi-byte characters.

    MockParserEvents listener;
    auto p = vtparser::Parser<vtparser::ParserEvents>(listener);

    // "A" + first byte of "│" (E2)
    p.parseFragment("A\xE2"sv);
    CHECK(listener.text == "A");

    // Second byte of "│" (94) - still incomplete
    p.parseFragment("\x94"sv);
    CHECK(listener.text == "A");

    // Third byte of "│" (82) + "B" + first 2 bytes of "├" (E2 94)
    p.parseFragment("\x82"
                    "B\xE2\x94"sv);
    CHECK(listener.text
          == "A\xE2\x94\x82"
             "B");

    // Complete "├" (9C) + "C"
    p.parseFragment("\x9C"
                    "C"sv);
    CHECK(listener.text
          == "A\xE2\x94\x82"
             "B\xE2\x94\x9C"
             "C");
}
