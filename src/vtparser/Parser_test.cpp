// SPDX-License-Identifier: Apache-2.0
#include <vtparser/BulkScan.hpp>
#include <vtparser/Parser.hpp>
#include <vtparser/ParserEvents.hpp>
#include <vtparser/Utf8Decoding.hpp>

#include <libunicode/convert.h>
#include <libunicode/utf8.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <optional>
#include <random>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

using namespace std;

namespace
{
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

/// @p text decoded the way the state machine decodes it, a byte that is not UTF-8 as U+FFFD. The bulk
/// text path hands such a byte on as it came, inside its run of text.
std::u32string decodedAsTheStateMachineDoes(std::string_view text)
{
    auto state = unicode::utf8_decoder_state {};
    auto codepoints = std::u32string {};
    for (auto const ch: text)
        if (auto const codepoint = vtparser::decodeUtf8Byte(state, static_cast<uint8_t>(ch)))
            codepoints += *codepoint;
    return codepoints;
}
} // namespace

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

TEST_CASE("Parser.String_MalformedUtf8_DoesNotSwallow8bitST", "[Parser]")
{
    // Regression: inside an OSC/DCS/APC/PM string body an 8-bit ST (0x9C) terminates the string, but
    // only at a UTF-8 character boundary (0x9C is also a valid continuation byte -- the middle byte of
    // U+2705, for example). The boundary was tracked by counting the continuation bytes a lead byte
    // promises, WITHOUT checking each following byte is actually a continuation byte (0x80-0xBF). A
    // malformed lead -- a lead byte NOT followed by continuation bytes -- therefore left the pending
    // count > 0, so a subsequent 8-bit ST was miscounted as a continuation byte and swallowed as
    // content: the string never terminated and every later byte was eaten too, never rendered.
    MockParserEvents listener;
    auto p = vtparser::Parser<vtparser::ParserEvents>(listener);
    REQUIRE(p.state() == vtparser::State::Ground);

    // PM body "X" + a bare 3-byte lead (0xE2) + a NON-continuation byte 'Y' (0x59) + an 8-bit ST
    // (0x9C). The 0x9C sits at a character boundary once the malformed lead is abandoned, so it must
    // terminate the PM string; "DEF" must then print as ordinary text rather than be swallowed.
    p.parseFragment("ABC\033^X\xE2Y\x9C"
                    "DEF"sv);

    CHECK(p.state() == vtparser::State::Ground);
    CHECK(listener.pm == "{X\xE2Y}");
    CHECK(listener.text == "ABCDEF");
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

TEST_CASE("Parser.BulkText_IncompleteUtf8_NothingCompleteBeforeIt", "[Parser]")
{
    // A read that ends inside a character with no complete character before it in the run: at the
    // start of a read, right after a control sequence, or right after an invalid byte. scan_text()
    // takes the partial character into its pending state; falling back to the state machine fed it to
    // the decoder a second time, which reported it invalid and printed U+FFFD ahead of it.
    auto const [input, printed] = GENERATE(table<std::string_view, std::u32string_view>({
        { "\xE2\x94\x82"sv, U"\u2502"sv },             // U+2502 at the start of a read
        { "\033[0m\xC3\xA4"sv, U"\u00E4"sv },          // U+00E4 right after a control sequence
        { "\xFF\xE2\x94\x82X"sv, U"\uFFFD\u2502X"sv }, // U+2502 right after an invalid byte
    }));

    for (auto const split: std::views::iota(size_t { 0 }, input.size() + 1))
    {
        MockParserEvents listener;
        auto parser = vtparser::Parser<vtparser::ParserEvents>(listener);
        parser.parseFragment(input.substr(0, split));
        parser.parseFragment(input.substr(split));

        INFO(std::format("split at byte {}", split));
        CHECK(decodedAsTheStateMachineDoes(listener.text) == printed);
        CHECK(parser.precedingGraphicCharacter() == printed.back());
    }
}

TEST_CASE("Utf8Decoding.anAsciiByteDecodesAsFromUtf8Does", "[Parser]")
{
    // decodeUtf8Byte answers an ASCII byte on an idle decoder itself, as from_utf8's first branch
    // does. Should libunicode ever handle that byte differently, this fails rather than letting the
    // state machine and the bulk text path decode it two ways.
    for (auto const byte: std::views::iota(0x00, 0x80))
    {
        INFO(std::format("byte {:02X}", byte));
        auto viaFromUtf8 = unicode::utf8_decoder_state {};
        auto const expected = unicode::from_utf8(viaFromUtf8, static_cast<uint8_t>(byte));
        REQUIRE(std::holds_alternative<unicode::Success>(expected));

        auto viaHelper = unicode::utf8_decoder_state {};
        CHECK(vtparser::decodeUtf8Byte(viaHelper, static_cast<uint8_t>(byte))
              == std::get<unicode::Success>(expected).value);
        CHECK(viaHelper.expectedLength == 0);
        CHECK(viaFromUtf8.expectedLength == 0);
    }
}

TEST_CASE("Utf8Decoding.everythingElseIsFromUtf8sDecision", "[Parser]")
{
    auto state = unicode::utf8_decoder_state {};
    CHECK(vtparser::decodeUtf8Byte(state, 0xE2) == std::nullopt); // a character begins
    CHECK(vtparser::decodeUtf8Byte(state, 0x94) == std::nullopt);
    CHECK(vtparser::decodeUtf8Byte(state, 0x82) == U'│');
    CHECK(vtparser::decodeUtf8Byte(state, 0xFF) == vtparser::ReplacementCharacter); // never UTF-8
    CHECK(state.expectedLength == 0);
}

TEST_CASE("Utf8Decoding.idleOnlyAtACharacterBoundary", "[Parser]")
{
    auto state = unicode::utf8_decoder_state {};
    CHECK(vtparser::isDecoderIdle(state));
    std::ignore = vtparser::decodeUtf8Byte(state, 0xF0); // a 4-byte character begins
    CHECK_FALSE(vtparser::isDecoderIdle(state));
    std::ignore = vtparser::decodeUtf8Byte(state, 0x9F);
    std::ignore = vtparser::decodeUtf8Byte(state, 0x98);
    CHECK_FALSE(vtparser::isDecoderIdle(state));
    std::ignore = vtparser::decodeUtf8Byte(state, 0x80); // U+1F600 is complete
    CHECK(vtparser::isDecoderIdle(state));

    // Why idleness matters: after a lead byte, from_utf8 takes an ASCII byte as the continuation byte
    // and folds it into the pending character, so 0xC3 'a' decodes to U+00E1 rather than to 'a'.
    std::ignore = vtparser::decodeUtf8Byte(state, 0xC3);
    CHECK_FALSE(vtparser::isDecoderIdle(state));
    CHECK(vtparser::decodeUtf8Byte(state, 'a') == U'á');
    CHECK(vtparser::isDecoderIdle(state));
}

TEST_CASE("Parser.BulkText_ZeroWidthAfterAsciiBase", "[Parser]")
{
    // Zero COLUMNS is not zero BYTES. scan_text() splits a run at the ASCII/non-ASCII boundary, and a
    // zero-width codepoint that follows an ASCII base arrives in the non-ASCII half on its own,
    // joining the cluster the ASCII half left open without widening it. libunicode < 0.9.3 measured
    // that half's progress in columns: state.next moved past those bytes while the reported end did
    // not, so parseBulkText -- which prints [subStart, subEnd) and resumes at _scanState.next --
    // printed the base, consumed the rest, and never rendered it. Decomposed "é" arrived as "e".
    //
    // Every byte handed in must come back out, whatever the cluster measures.
    auto const input = GENERATE(values<std::string_view>({
        "e\xCC\x81"sv, // U+0301 COMBINING ACUTE on a lone ASCII base, at end of buffer
        "abcde\xCC\x81"
        "fgh"sv,           // ... mid-buffer, so the scan hands over and then returns to ASCII
        "a\xEF\xB8\x8E"sv, // U+FE0E VARIATION SELECTOR-15, three bytes, on a base with no
                           // variation sequence to select -- so it too adds no column
        "a\xE2\x80\x8D"sv, // U+200D ZERO WIDTH JOINER, which opens no cluster of its own
    }));

    INFO("input: " << input);
    MockParserEvents listener;
    auto p = vtparser::Parser<vtparser::ParserEvents>(listener);

    p.parseFragment(input);

    CHECK(listener.text == input);
}

TEST_CASE("Parser.BulkText_CombiningMarkSplitAcrossCalls", "[Parser]")
{
    // The base and its mark arriving in separate reads, as a PTY delivers them. The second fragment
    // opens with the mark, so the scan enters its non-ASCII half immediately rather than through the
    // ASCII handoff -- the arm that was always correct. Pinned so both routes to the same cluster
    // stay in agreement.

    MockParserEvents listener;
    auto p = vtparser::Parser<vtparser::ParserEvents>(listener);

    p.parseFragment("e"sv);
    CHECK(listener.text == "e");

    p.parseFragment("\xCC\x81"sv);
    CHECK(listener.text == "e\xCC\x81");
}

TEST_CASE("Parser.precedingGraphicCharacter.ascii", "[Parser]")
{
    MockParserEvents listener;
    auto p = vtparser::Parser<vtparser::ParserEvents>(listener);

    p.parseFragment("Hello"sv);
    CHECK(p.precedingGraphicCharacter() == U'o');
}

TEST_CASE("Parser.precedingGraphicCharacter.nonascii", "[Parser]")
{
    MockParserEvents listener;
    auto p = vtparser::Parser<vtparser::ParserEvents>(listener);

    // U+2502 "│" is E2 94 82 in UTF-8
    p.parseFragment("\xE2\x94\x82"sv);
    CHECK(p.precedingGraphicCharacter() == U'\u2502');
}

TEST_CASE("Parser.precedingGraphicCharacter.persists_through_escape", "[Parser]")
{
    MockParserEvents listener;
    auto p = vtparser::Parser<vtparser::ParserEvents>(listener);

    // Send "A" then ESC to start an escape sequence.
    // precedingGraphicCharacter should retain 'A' because GroundStart
    // only fires when entering Ground, not when leaving it.
    p.parseFragment("A\033"sv);
    CHECK(p.precedingGraphicCharacter() == U'A');
}

namespace
{
/// Records every parser event as one line of text, so two event logs can be compared.
///
/// Text is normalised to codepoints: the bulk text path reports a run with one
/// print(string_view), the state machine reports one print(char32_t) per codepoint, and both
/// describe the same text. A run is decoded as the state machine decodes it, so a byte that is not
/// UTF-8 counts as the U+FFFD the state machine prints for it.
class EventRecorder final: public vtparser::NullParserEvents
{
  public:
    /// @param bulkTextWidth What maxBulkTextSequenceWidth() reports. A small value makes the bulk
    ///                      text path stop and resume mid-run.
    explicit EventRecorder(size_t bulkTextWidth): _bulkTextWidth { bulkTextWidth } {}

    /// Ends the log with @p finalState and returns it.
    [[nodiscard]] std::vector<std::string> finish(vtparser::State finalState)
    {
        flushText();
        _events.push_back(std::format("state {}", finalState));
        return std::move(_events);
    }

    void error(std::string_view const& message) override { record(std::format("error {}", message)); }
    void print(char32_t codepoint) override { _text += codepoint; }
    size_t print(std::string_view chars, size_t /*cellCount*/) override
    {
        _text += decodedAsTheStateMachineDoes(chars);
        return 0;
    }
    void printEnd() override { record("printEnd"); }
    [[nodiscard]] size_t maxBulkTextSequenceWidth() const noexcept override { return _bulkTextWidth; }
    void execute(char ch) override { record("execute", ch); }
    void clear() override { record("clear"); }
    void collect(char ch) override { record("collect", ch); }
    void collectLeader(char ch) override { record("collectLeader", ch); }
    void param(char ch) override { record("param", ch); }
    void paramDigit(char ch) override { record("paramDigit", ch); }
    void paramSeparator() override { record("paramSeparator"); }
    void paramSubSeparator() override { record("paramSubSeparator"); }
    void dispatchESC(char ch) override { record("dispatchESC", ch); }
    void dispatchCSI(char ch) override { record("dispatchCSI", ch); }
    void dispatchVT52(char ch, unsigned line, unsigned column) override
    {
        record(std::format("dispatchVT52 {:02X} {} {}", static_cast<unsigned char>(ch), line, column));
    }
    void startOSC() override { record("startOSC"); }
    void putOSC(char ch) override { record("putOSC", ch); }
    void dispatchOSC() override { record("dispatchOSC"); }
    void hook(char ch) override { record("hook", ch); }
    void put(char ch) override { record("put", ch); }
    void unhook() override { record("unhook"); }
    void startAPC() override { record("startAPC"); }
    void putAPC(char ch) override { record("putAPC", ch); }
    void dispatchAPC() override { record("dispatchAPC"); }
    void startPM() override { record("startPM"); }
    void putPM(char ch) override { record("putPM", ch); }
    void dispatchPM() override { record("dispatchPM"); }

  private:
    void flushText()
    {
        if (_text.empty())
            return;
        _events.push_back("text " + unicode::convert_to<char>(std::u32string_view { _text }));
        _text.clear();
    }

    void record(std::string event)
    {
        flushText();
        _events.push_back(std::move(event));
    }

    void record(std::string_view name, char ch)
    {
        record(std::format("{} {:02X}", name, static_cast<unsigned char>(ch)));
    }

    size_t _bulkTextWidth;
    std::u32string _text;
    std::vector<std::string> _events;
};

/// The events @p input produces, cut into reads at @p cuts: sorted offsets, the last read
/// running to the end.
std::vector<std::string> eventsOf(vtparser::BulkScan bulkScan,
                                  std::string_view input,
                                  std::span<size_t const> cuts)
{
    auto recorder = EventRecorder { 7 };
    auto parser = vtparser::Parser<vtparser::ParserEvents>(recorder, bulkScan);
    auto begin = size_t { 0 };
    for (auto const cut: cuts)
    {
        parser.parseFragment(input.substr(begin, cut - begin));
        begin = cut;
    }
    parser.parseFragment(input.substr(begin));
    return recorder.finish(parser.state());
}

/// Where two event logs part ways, with three events of context on either side, or an empty
/// string when they agree.
std::string firstDifference(std::vector<std::string> const& expected, std::vector<std::string> const& actual)
{
    auto const [expectedAt, actualAt] = std::ranges::mismatch(expected, actual);
    if (expectedAt == expected.end() && actualAt == actual.end())
        return {};

    auto const at = static_cast<size_t>(std::distance(expected.begin(), expectedAt));
    auto const eventAt = [](std::vector<std::string> const& events, size_t i) {
        return i < events.size() ? events[i] : std::string { "<end>" };
    };
    auto report = std::format("first difference at event {}\n", at);
    for (auto const i: std::views::iota(at >= 3 ? at - 3 : 0, at + 4))
        report += std::format(
            "  {:>6}  state machine: {:<28}  bulk: {}\n", i, eventAt(expected, i), eventAt(actual, i));
    return report;
}

/// Requires the bulk scanners to reproduce the state machine's events for @p input cut into two
/// reads at every byte.
void checkEveryTwoWaySplit(std::string_view input)
{
    auto const reference = eventsOf(vtparser::BulkScan::Disabled, input, {});
    for (auto const split: std::views::iota(size_t { 0 }, input.size() + 1))
    {
        auto const cuts = std::array { split };
        auto const difference =
            firstDifference(reference, eventsOf(vtparser::BulkScan::Enabled, input, cuts));
        INFO(std::format("split at byte {}\n{}", split, difference));
        CHECK(difference.empty());
    }
}

/// A choice in [0, n). mt19937's output sequence is fixed by the standard, unlike the
/// distributions', so a failing seed reproduces on every platform.
size_t below(std::mt19937& rng, size_t n)
{
    return static_cast<size_t>(rng() % n);
}

template <typename T, size_t N>
T const& oneOf(std::mt19937& rng, std::array<T, N> const& choices)
{
    return choices[below(rng, N)];
}

char oneOf(std::mt19937& rng, std::string_view choices)
{
    return choices[below(rng, choices.size())];
}

void appendText(std::string& out, std::mt19937& rng)
{
    static constexpr auto Words = std::array {
        "hello"sv,
        "w\xC3\xB6rld"sv,
        "\xE2\x94\x82"sv,                                 // U+2502, three bytes
        "e\xCC\x81"sv,                                    // a combining acute on an ASCII base
        "\xF0\x9F\x98\x80"sv,                             // U+1F600, four bytes
        "\xE4\xB8\xAD"sv,                                 // U+4E2D, two columns wide
        "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9"sv, // two emoji joined by ZWJ
        " "sv,
        "x"sv,
    };
    for ([[maybe_unused]] auto const _: std::views::iota(size_t { 0 }, 1 + below(rng, 12)))
        out += oneOf(rng, Words);
}

void appendControl(std::string& out, std::mt19937& rng)
{
    out += oneOf(rng, "\r\n\t\b\a\x0E\x0F"sv);
}

/// Bytes that are not well-formed UTF-8, or that begin a sequence the next piece may not finish: stray
/// continuation bytes, lead bytes cut short by ASCII or by another lead, bytes no encoding uses, and
/// overlong, surrogate and out-of-range encodings.
void appendIllFormedUtf8(std::string& out, std::mt19937& rng)
{
    static constexpr auto Sequences = std::array {
        "\x80"sv,     // a stray continuation byte
        "\xBF"sv,     // the last one
        "\xC3"sv,     // a two-byte lead, one byte short
        "\xE2\x94"sv, // a three-byte lead, one byte short
        "\xF0\x9F"sv, // a four-byte lead, two bytes short
        "\xE2"
        "a"sv, // a three-byte lead cut short by ASCII
        "\xF0\x9F"
        "xyz"sv,                  // a four-byte lead cut short by ASCII
        "\xC3\xE2\x94\x82"sv,     // a lead cut short by another lead
        "\xE2\xF0\x9F\x98\x80"sv, // the same with a four-byte character behind it
        "\xC0\xAF"sv,             // 0xC0 and 0xC1 are overlong leads
        "\xC1\xBF"sv,
        "\xF5\x80\x80\x80"sv, // 0xF5..0xFF lead nothing
        "\xF8"sv,
        "\xFE"sv,
        "\xFF"sv,
        "\xE0\x80\xAF"sv,     // overlong: '/' in three bytes
        "\xF0\x80\x80\xAF"sv, // overlong: '/' in four bytes
        "\xED\xA0\x80"sv,     // a surrogate, U+D800
        "\xED\xBF\xBF"sv,     // a surrogate, U+DFFF
        "\xF4\x90\x80\x80"sv, // above U+10FFFF
    };
    out += oneOf(rng, Sequences);
}

/// One CSI sequence: usually well-formed, sometimes interrupted or malformed in the ways the state
/// machine must catch.
void appendCsi(std::string& out, std::mt19937& rng)
{
    static constexpr auto Introducers = std::array { "\033["sv, "\x9B"sv };
    static constexpr auto Leaders = std::array { ""sv, ""sv, "?"sv, ">"sv, "<"sv, "="sv };
    static constexpr auto Interruptions =
        std::array { "\x18"sv, "\x1A"sv, "\033"sv, "\n"sv, "\x7F"sv, "?"sv, "\x85"sv, "\033[2"sv };
    static constexpr auto Intermediates = std::array { ""sv, ""sv, ""sv, " "sv, "$"sv, "\""sv, "'"sv };

    out += oneOf(rng, Introducers);
    out += oneOf(rng, Leaders);
    for ([[maybe_unused]] auto const _: std::views::iota(size_t { 0 }, below(rng, 40)))
        out += oneOf(rng, "0123456789012345;;;:"sv);
    if (below(rng, 8) == 0)
        out += oneOf(rng, Interruptions);
    out += oneOf(rng, Intermediates);
    out += oneOf(rng, "mmmmHJKABCDfhlrstqu@PXLMbcdgnp~|}{"sv);
}

void appendDcs(std::string& out, std::mt19937& rng)
{
    static constexpr auto Payloads =
        std::array { "q#0;2;0;0;0#1!10~-"sv, "$qm"sv, "1;1|17/AB"sv, "+q544e"sv, "\xE2\x9C\x85"sv };
    static constexpr auto Interruptions = std::array { ""sv, ""sv, ""sv, "\n"sv, "\x7F"sv, "\x18"sv };
    static constexpr auto Terminators = std::array { "\033\\"sv, "\x9C"sv, "\x18"sv, "\a"sv };

    out += "\033P"sv;
    for ([[maybe_unused]] auto const _: std::views::iota(size_t { 0 }, below(rng, 5)))
        out += oneOf(rng, "0123456789;"sv);
    out += oneOf(rng, Payloads);
    out += oneOf(rng, Interruptions);
    out += oneOf(rng, Terminators);
}

void appendApc(std::string& out, std::mt19937& rng)
{
    static constexpr auto Payloads = std::array { "Gi=1,a=q;AAAA"sv, "Ga=d"sv, "x\xE2\x9C\x85y"sv };
    static constexpr auto Terminators = std::array { "\033\\"sv, "\x9C"sv, "\a"sv, "\x18"sv };
    out += "\033_"sv;
    out += oneOf(rng, Payloads);
    out += oneOf(rng, Terminators);
}

void appendOsc(std::string& out, std::mt19937& rng)
{
    static constexpr auto Payloads =
        std::array { "0;title"sv, "8;;http://example.com"sv, "133;A"sv, "52;c;Zm9v"sv };
    static constexpr auto Terminators = std::array { "\a"sv, "\033\\"sv, "\x9C"sv };
    out += "\033]"sv;
    out += oneOf(rng, Payloads);
    out += oneOf(rng, Terminators);
}

void appendOtherString(std::string& out, std::mt19937& rng)
{
    out += oneOf(rng, std::array { "\033^private\033\\"sv, "\033Xsos\x9C"sv, "\033^\xE2\x9C\x85\a"sv });
}

void appendEscape(std::string& out, std::mt19937& rng)
{
    out += oneOf(
        rng, std::array { "\0337"sv, "\0338"sv, "\033(B"sv, "\033(0"sv, "\033#8"sv, "\033="sv, "\033 F"sv });
}

using PieceWriter = void (*)(std::string&, std::mt19937&);

/// What a random stream is assembled from, weighted by repetition.
constexpr auto PieceWriters = std::array<PieceWriter, 12> {
    appendText, appendText, appendControl, appendCsi,         appendCsi,    appendCsi,
    appendDcs,  appendApc,  appendOsc,     appendOtherString, appendEscape, appendIllFormedUtf8,
};

std::string randomStream(std::mt19937& rng, size_t pieceCount)
{
    auto out = std::string {};
    for ([[maybe_unused]] auto const _: std::views::iota(size_t { 0 }, pieceCount))
        oneOf(rng, PieceWriters)(out, rng);
    return out;
}

/// @p count sorted offsets into an input of @p inputSize bytes, the way PTY reads split a stream.
std::vector<size_t> randomCuts(std::mt19937& rng, size_t inputSize, size_t count)
{
    auto cuts = std::vector<size_t> {};
    for ([[maybe_unused]] auto const _: std::views::iota(size_t { 0 }, count))
        cuts.push_back(below(rng, inputSize + 1));
    std::ranges::sort(cuts);
    return cuts;
}
} // namespace

TEST_CASE("Parser.BulkScan.DefaultIsEnabled", "[Parser][BulkScan]")
{
    MockParserEvents listener;
    CHECK(vtparser::Parser<vtparser::ParserEvents>(listener).bulkScan() == vtparser::BulkScan::Enabled);
    CHECK(vtparser::Parser<vtparser::ParserEvents>(listener, vtparser::BulkScan::Disabled).bulkScan()
          == vtparser::BulkScan::Disabled);
}

TEST_CASE("Parser.BulkScan.KnownStreamsMatchTheStateMachine", "[Parser][BulkScan]")
{
    auto const input = GENERATE(values<std::string_view>({
        "ABC\033^hello \xE2\x9C\x85 world\033\\DEF"sv, // PM
        "ABC\033\\\033_Gi=1,a=q;\033\\DEF"sv,          // APC, bulk payload
        "\033Pq#0;2;0;0;0#1!10~-\n#1!5~\033\\after"sv, // DCS, bulk payload with a C0 inside
        "abcde\xCC\x81"
        "fgh"sv,                                    // a mark joining an ASCII base mid-run
        "\033]0;title\aX\033]8;;http://x\033\\Y"sv, // OSC, both terminators
        "A\033[1;31mB\033[mC\r\nD\tE"sv,            // text, SGR, trailing C0s
        "\x9B"
        "2J\x9D"
        "0;t\x9C"
        "Z"sv,                                           // 8-bit CSI, 8-bit OSC and ST
        "\xE2\x94\x82\xE2\x94\x82 x \xF0\x9F\x98\x80"sv, // multi-byte text, split mid-character
    }));
    checkEveryTwoWaySplit(input);
}

TEST_CASE("Parser.BulkScan.ReadsEndingInsideIllFormedUtf8MatchTheStateMachine", "[Parser][BulkScan]")
{
    // A read that ends on an incomplete or ill-formed sequence, and the next one continues: the
    // text scan stops in front of the sequence (libunicode 0.9.4), and the state machine takes it
    // from there, in the middle of a stretch of ASCII, of mixed text, or on its own.
    auto const input = GENERATE(values<std::string_view>({
        "\xE2"
        "abcdefghijklmnopqrstuvwxyz tail"sv, // a lead byte, then ASCII it takes for continuation bytes
        "head abcdefghijklmnopqrstuvwxyz\xF0\x9F"
        " tail"sv,                                               // a stretch, then a four-byte lead cut short
        "w\xC3\xB6rld \xC3 abcdefghijklmnopqrstuvwxyz"sv,        // mixed text, a lead cut short, a stretch
        "abc\x80\xBF def\xC0\xAF ghi\xF5\x80\x80\x80 jkl\xFF"sv, // stray and unused bytes inside a stretch
        "ab\xE0\x80\xAF"
        "cd\xED\xA0\x80"
        "ef\xF4\x90\x80\x80"
        "gh"sv,                                                     // overlong, surrogate, too large
        "\xE2\x94\xE2\x94\x82 \xF0\x9F\x98 \xF0\x9F\x98\x80 end"sv, // a lead cut short by a lead
    }));
    checkEveryTwoWaySplit(input);
}

TEST_CASE("Parser.BulkScan.RandomStreamsMatchTheStateMachine", "[Parser][BulkScan]")
{
    auto const seed = GENERATE(range(1U, 129U));
    auto rng = std::mt19937 { seed };
    auto const input = randomStream(rng, 300);
    auto const cuts = randomCuts(rng, input.size(), 64);
    auto const difference = firstDifference(eventsOf(vtparser::BulkScan::Disabled, input, {}),
                                            eventsOf(vtparser::BulkScan::Enabled, input, cuts));
    INFO(std::format("seed {}\n{}", seed, difference));
    CHECK(difference.empty());
}

TEST_CASE("Parser.APC_MalformedUtf8_BulkRunStillEndsAt8bitST", "[Parser]")
{
    // A lead byte (E2) that no continuation byte follows: the state machine abandons it at 'A', so the
    // 8-bit ST after 'A' sits at a character boundary and ends the string. 'A' arrives through the
    // bulk APC scanner, which must abandon the sequence too.
    MockParserEvents listener;
    auto p = vtparser::Parser<vtparser::ParserEvents>(listener);
    p.parseFragment("\033_x\xE2"
                    "A\x9C"
                    "Z"sv);
    CHECK(p.state() == vtparser::State::Ground);
    CHECK(listener.apc
          == "{x\xE2"
             "A}");
    CHECK(listener.text == "Z");
}

TEST_CASE("Parser.BulkScan.MalformedUtf8InStringPayload", "[Parser][BulkScan]")
{
    auto const input = GENERATE(values<std::string_view>({
        "\033Px\xE2"
        "A\x9C"
        "Z"sv, // DCS: a three-byte lead, cut short by ASCII
        "\033_x\xE2"
        "A\x9C"
        "Z"sv, // APC: the same
        "\033_x\xF0\x9F"
        "AB\x9C"
        "Z"sv, // APC: a four-byte lead cut short after two bytes
    }));
    checkEveryTwoWaySplit(input);
}

TEST_CASE("Parser.CsiParameterBulk.TableAssumptions", "[Parser]")
{
    // parseBulkCsiParameters() stands in for the table on exactly these bytes. It is correct only
    // while the table says what this test pins: an event without a transition in CSI_Param, one
    // action-free transition from CSI_Entry, and ':' leaving CSI_Entry for CSI_Ignore.
    using vtparser::Action;
    using vtparser::State;
    auto const table = vtparser::ParserTable::get();
    auto const entry = static_cast<size_t>(State::CSI_Entry);
    auto const param = static_cast<size_t>(State::CSI_Param);

    for (auto const digit: std::views::iota(size_t { '0' }, size_t { '9' } + 1))
    {
        CHECK(table.transitions[param][digit] == State::Undefined);
        CHECK(table.events[param][digit] == Action::ParamDigit);
        CHECK(table.transitions[entry][digit] == State::CSI_Param);
        CHECK(table.events[entry][digit] == Action::ParamDigit);
    }
    CHECK(table.transitions[param][';'] == State::Undefined);
    CHECK(table.events[param][';'] == Action::ParamSeparator);
    CHECK(table.transitions[param][':'] == State::Undefined);
    CHECK(table.events[param][':'] == Action::ParamSubSeparator);
    CHECK(table.transitions[entry][';'] == State::CSI_Param);
    CHECK(table.events[entry][';'] == Action::ParamSeparator);
    CHECK(table.transitions[entry][':'] == State::CSI_Ignore);
    CHECK(table.exitEvents[entry] == Action::Undefined);
    CHECK(table.entryEvents[param] == Action::Undefined);

    // And the scanner's byte class, one range test, is exactly the set of bytes the table treats as
    // a parameter event in CSI_Param -- no byte more, no byte less.
    for (auto const byte: std::views::iota(size_t { 0 }, size_t { 256 }))
    {
        INFO(std::format("byte {:02X}", byte));
        auto const event = table.events[param][byte];
        auto const isParameterEvent = table.transitions[param][byte] == State::Undefined
                                      && (event == Action::ParamDigit || event == Action::ParamSeparator
                                          || event == Action::ParamSubSeparator);
        CHECK(vtparser::isCsiParameterByte(static_cast<char>(byte)) == isParameterEvent);
    }
}

TEST_CASE("Parser.CsiParameterBulk.ExplicitCases", "[Parser][BulkScan]")
{
    auto const input = GENERATE(values<std::string_view>({
        "\033[38;5;123m"sv, // cut mid-number at every byte
        "\033[1;2;3;4;5;6;7;8;9;10;11;12;13;14;15;16;17;18;19;20;"
        "21;22;23;24;25;26;27;28;29;30;31;32;33m"sv,     // more parameters than the builder holds
        "\033[99999999999999999999;65536;65535H"sv,      // values past the maximum
        "\033[38:2::255:128:0m\033[4:3m\033[58:5:12m"sv, // ':' sub-parameters
        "\033[:1m"sv,                                    // a leading ':' goes to CSI_Ignore
        "\033[;1m\033[;;m\033[m"sv,                      // empty parameters, and none
        "\033[H\033[2J\033[@\033[~x"sv,                  // final bytes, at both ends of the range
        "\033[1;2\x18"
        "3m"sv, // CAN mid-parameters
        "\033[1;2\x1A"
        "3m"sv,                // SUB mid-parameters
        "\033[1;2\033[3;4m"sv, // ESC restarts the sequence
        "\033[1;\n2m"sv,       // a C0 control executes inside
        "\033[1;\x7F"
        "2m"sv, // DEL is ignored inside
        "\033[1;2\x85"
        "3m"sv,                                // a byte the table has no action for
        "\033[1?2m\033[?25;1004h\033[>4;2m"sv, // leader mid-parameters, and leading
        "\033[1 q\033[2;3$}"sv,                // intermediates after parameters
        "\x9B"
        "12;34H"sv,     // 8-bit CSI
        "\033[12;34"sv, // cut short at the end of the input
    }));
    checkEveryTwoWaySplit(input);
}

TEST_CASE("Parser.CsiParameterBulk.Vt52ModeIsUntouched", "[Parser][BulkScan]")
{
    // VT52 mode consumes ESC before the ANSI table ever sees it. The bulk scanner must leave that path alone.
    // setVT52Mode() leaves the ANSI state as it is, so the second case switches VT52 mode on in the middle
    // of a control sequence's parameters: the ESC Y coordinates and the ESC 4 command that follow are
    // digits the bulk scanner would otherwise take for more parameters.
    auto const [ansiPrefix, input] = GENERATE(table<std::string_view, std::string_view>({
        { ""sv, "\033A\033Y  \033[12;3Hx"sv },
        { "\033[12"sv, "3\033Y12\03345;6m"sv },
    }));
    auto const eventsInVt52Mode = [&](vtparser::BulkScan bulkScan) {
        auto recorder = EventRecorder { 7 };
        auto parser = vtparser::Parser<vtparser::ParserEvents>(recorder, bulkScan);
        parser.parseFragment(ansiPrefix);
        parser.setVT52Mode(true);
        parser.parseFragment(input);
        return recorder.finish(parser.state());
    };
    auto const difference = firstDifference(eventsInVt52Mode(vtparser::BulkScan::Disabled),
                                            eventsInVt52Mode(vtparser::BulkScan::Enabled));
    INFO(difference);
    CHECK(difference.empty());
}

TEST_CASE("Parser.CsiParameterBulk.RandomCsiHeavyStreams", "[Parser][BulkScan]")
{
    static constexpr auto CsiHeavy =
        std::array<PieceWriter, 5> { appendCsi, appendCsi, appendCsi, appendText, appendControl };
    auto const seed = GENERATE(range(1000U, 1128U));
    auto rng = std::mt19937 { seed };
    auto input = std::string {};
    for ([[maybe_unused]] auto const _: std::views::iota(0, 300))
        oneOf(rng, CsiHeavy)(input, rng);
    auto const cuts = randomCuts(rng, input.size(), 64);
    auto const difference = firstDifference(eventsOf(vtparser::BulkScan::Disabled, input, {}),
                                            eventsOf(vtparser::BulkScan::Enabled, input, cuts));
    INFO(std::format("seed {}\n{}", seed, difference));
    CHECK(difference.empty());
}
