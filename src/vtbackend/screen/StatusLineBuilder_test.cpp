// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/core/CellFlags.hpp>
#include <vtbackend/core/Color.hpp>
#include <vtbackend/core/ColorPalette.hpp>
#include <vtbackend/grid/GraphemeClusters.hpp>
#include <vtbackend/screen/StatusLineBuilder.hpp>
#include <vtbackend/screen/Terminal.hpp>
#include <vtbackend/testing/InMemoryGitFileSystem.hpp>
#include <vtbackend/testing/MockTerm.hpp>
#include <vtbackend/testing/TestHelpers.hpp>

#include <core/Environment.hpp>
#include <core/Utils.hpp>
#include <core/async/testing/ManualExecutor.hpp>
#include <core/platform/testing/InMemoryFileSystem.hpp>
#include <core/testing/Environment.hpp>

#include <libunicode/convert.h>
#include <libunicode/width.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <format>
#include <ranges>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

using namespace vtbackend;
using namespace std::string_view_literals;

// parseStatusLineSegment() turns a template string into a sequence of typed items. Known placeholders
// become their concrete Item type; an unrecognized placeholder is echoed verbatim as literal Text (its
// exact original "{...}" slice), matching expandTabLabel()'s tab-strip handling so both surfaces treat an
// unknown placeholder identically instead of silently dropping it.

TEST_CASE("parseStatusLineSegment.knownPlaceholderBecomesTypedItem", "[statusline]")
{
    auto const segment = parseStatusLineSegment("{VTType}"sv);
    REQUIRE(segment.size() == 1);
    CHECK(std::holds_alternative<StatusLineDefinitions::VTType>(segment[0]));
}

TEST_CASE("parseStatusLineSegment.traceModeIsRecognized", "[statusline]")
{
    // {TraceMode} is a first-class status-line item (it has a type and a serializer). Regression guard: it
    // must parse to a TraceMode item, not fall through to the verbatim-echo path as an unknown placeholder.
    // The default indicator status line's `{TraceMode:Bold,Color=#FFFF00,Left= │ }` segment depends on it.
    auto const segment = parseStatusLineSegment("{TraceMode:Bold,Color=#FFFF00,Left= │ }"sv);
    REQUIRE(segment.size() == 1);
    CHECK(std::holds_alternative<StatusLineDefinitions::TraceMode>(segment[0]));
}

TEST_CASE("parseStatusLineSegment.unknownPlaceholderEchoesVerbatim", "[statusline]")
{
    SECTION("a plain unknown placeholder")
    {
        auto const segment = parseStatusLineSegment("{Bogus}"sv);
        REQUIRE(segment.size() == 1);
        REQUIRE(std::holds_alternative<StatusLineDefinitions::Text>(segment[0]));
        CHECK(std::get<StatusLineDefinitions::Text>(segment[0]).text == "{Bogus}");
    }

    SECTION("an unknown placeholder keeps its flags/attributes verbatim")
    {
        auto const segment = parseStatusLineSegment("{Bogus:Bold,Color=#FFFF00}"sv);
        REQUIRE(segment.size() == 1);
        REQUIRE(std::holds_alternative<StatusLineDefinitions::Text>(segment[0]));
        CHECK(std::get<StatusLineDefinitions::Text>(segment[0]).text == "{Bogus:Bold,Color=#FFFF00}");
    }

    SECTION("literal text, a known token and an unknown token coexist in order")
    {
        auto const segment = parseStatusLineSegment("pre {VTType} {Bogus} post"sv);
        // "pre ", {VTType}, " ", {Bogus}->verbatim, " post"
        REQUIRE(segment.size() == 5);
        CHECK(std::holds_alternative<StatusLineDefinitions::Text>(segment[0]));
        CHECK(std::get<StatusLineDefinitions::Text>(segment[0]).text == "pre ");
        CHECK(std::holds_alternative<StatusLineDefinitions::VTType>(segment[1]));
        CHECK(std::holds_alternative<StatusLineDefinitions::Text>(segment[2]));
        CHECK(std::get<StatusLineDefinitions::Text>(segment[2]).text == " ");
        REQUIRE(std::holds_alternative<StatusLineDefinitions::Text>(segment[3]));
        CHECK(std::get<StatusLineDefinitions::Text>(segment[3]).text == "{Bogus}");
        CHECK(std::holds_alternative<StatusLineDefinitions::Text>(segment[4]));
        CHECK(std::get<StatusLineDefinitions::Text>(segment[4]).text == " post");
    }
}

// serializeStatusLineSegment() is the inverse of parseStatusLineSegment(): it turns a parsed segment back
// into the template form a config file holds. The settings page relies on that pair round-tripping -- it
// parses a profile's template into editable items and serializes them back on save -- so anything the
// parser understands the serializer has to be able to write back in a form the parser still understands.
//
// The separator matters more than it looks. crispy::parseInterpolation splits a placeholder at its FIRST
// colon and splits what follows on commas, so writing "{InputMode,Bold}" where "{InputMode:Bold}" belongs
// makes "InputMode,Bold" the placeholder *name*. That name matches nothing, so the item is echoed back as
// literal text and its styling is gone -- a saved profile silently loses its status line.

namespace
{

/// The variant index of each item, i.e. the segment's shape as a comparable value. Item has no
/// operator==, and the type sequence is what these tests are actually asserting about.
[[nodiscard]] std::vector<size_t> shapeOf(StatusLineSegment const& segment)
{
    auto shape = std::vector<size_t> {};
    for (auto const& item: segment)
        shape.push_back(item.index());
    return shape;
}

/// The segment's canonical template form: parse then serialize.
[[nodiscard]] std::string canonical(std::string_view text)
{
    return serializeStatusLineSegment(parseStatusLineSegment(text));
}

} // namespace

TEST_CASE("serializeStatusLineSegment.firstAttributeIsColonSeparated", "[statusline]")
{
    // The regression this pair of asserts exists for: a comma here instead of a colon folds the whole
    // attribute list into the placeholder name and destroys the item on the next parse.
    auto const text = canonical("{InputMode:Bold,Color=#FFFF00}"sv);
    CHECK(text == "{InputMode:Bold,Color=#FFFF00}");

    // ... and it must still be an InputMode after a round trip, not literal text.
    auto const readBack = parseStatusLineSegment(text);
    REQUIRE(readBack.size() == 1);
    CHECK(std::holds_alternative<StatusLineDefinitions::InputMode>(readBack[0]));
}

TEST_CASE("serializeStatusLineSegment.roundTripsShippedDefaults", "[statusline]")
{
    // The three default indicator segments from config::IndicatorConfig. These are what a fresh install
    // shows and what the settings page parses on first open, so they are the round trip that has to hold.
    auto const defaults = std::array {
        " {InputMode:Bold,Color=#FFFF00}"
        "{TraceMode:Bold,Color=#FFFF00,Left= │ }"
        "{Tabs:ActiveColor=#FFFF00,Left= │ }"
        "{ProtectedMode:Bold,Left= │ }"sv,
        "« {Title} »"sv,
        "{HistoryLineCount:Faint,Color=#c0c0c0} │ {Clock:Bold}"sv,
    };

    // {SearchPrompt} is deliberately absent above: the find bar shows the same text, so it left the
    // shipped default. The item still parses and still round-trips, which is what this covers.
    CHECK(shapeOf(parseStatusLineSegment(canonical("{SearchPrompt:Left= │ }"sv)))
          == shapeOf(parseStatusLineSegment("{SearchPrompt:Left= │ }"sv)));

    for (auto const& original: defaults)
    {
        auto const once = canonical(original);

        // Every item keeps its type: nothing degraded into literal Text.
        CHECK(shapeOf(parseStatusLineSegment(once)) == shapeOf(parseStatusLineSegment(original)));

        // And the canonical form is a fixed point, so repeated opens and saves of the settings page
        // cannot drift the template.
        CHECK(canonical(once) == once);
    }
}

TEST_CASE("serializeStatusLineSegment.preservesEveryCellFlag", "[statusline]")
{
    // The settings bridge once exposed four of these thirteen and silently dropped the rest on save.
    // Driving both directions off StatusLineDefinitions::CellFlagNames is what makes that impossible.
    for (auto const& [name, label, flag]: StatusLineDefinitions::CellFlagNames)
    {
        auto styles = StatusLineDefinitions::Styles {};
        styles.flags.enable(flag);

        auto const text = serializeStatusLineSegment({ StatusLineDefinitions::InputMode { styles } });
        auto const readBack = parseStatusLineSegment(text);

        REQUIRE(readBack.size() == 1);
        REQUIRE(std::holds_alternative<StatusLineDefinitions::InputMode>(readBack[0]));
        CHECK(std::get<StatusLineDefinitions::InputMode>(readBack[0]).flags.test(flag));
    }

    SECTION("all of them at once")
    {
        auto styles = StatusLineDefinitions::Styles {};
        for (auto const& [name, label, flag]: StatusLineDefinitions::CellFlagNames)
            styles.flags.enable(flag);

        auto const readBack =
            parseStatusLineSegment(serializeStatusLineSegment({ StatusLineDefinitions::Clock { styles } }));
        REQUIRE(readBack.size() == 1);
        REQUIRE(std::holds_alternative<StatusLineDefinitions::Clock>(readBack[0]));
        CHECK(std::get<StatusLineDefinitions::Clock>(readBack[0]).flags == styles.flags);
    }
}

TEST_CASE("serializeStatusLineSegment.preservesTabsAttributes", "[statusline]")
{
    // Tabs carries three fields beyond Styles. The settings bridge used to rebuild it as
    // `Tabs { styles, {}, {}, {} }`, so opening the settings page wiped the ActiveColor the shipped
    // default indicator sets.
    auto const tabs = StatusLineDefinitions::Tabs {
        StatusLineDefinitions::Styles {},
        RGBColor { 0xFF, 0xFF, 0x00 },
        RGBColor { 0x20, 0x30, 0x40 },
        std::string { " | " },
    };

    auto const readBack = parseStatusLineSegment(serializeStatusLineSegment({ tabs }));
    REQUIRE(readBack.size() == 1);
    REQUIRE(std::holds_alternative<StatusLineDefinitions::Tabs>(readBack[0]));

    auto const& got = std::get<StatusLineDefinitions::Tabs>(readBack[0]);
    CHECK(got.activeColor == tabs.activeColor);
    CHECK(got.activeBackground == tabs.activeBackground);
    CHECK(got.separator == tabs.separator);
}

TEST_CASE("parseStatusLineSegment.tabsSeparatorIsRead", "[statusline]")
{
    // serializeToVT() consumes Tabs::separator (falling back to "|"), but the parser used to hardcode
    // nullopt, so no config could ever set it.
    auto const segment = parseStatusLineSegment("{Tabs:Separator= · }"sv);
    REQUIRE(segment.size() == 1);
    REQUIRE(std::holds_alternative<StatusLineDefinitions::Tabs>(segment[0]));
    CHECK(std::get<StatusLineDefinitions::Tabs>(segment[0]).separator == " · ");
}

TEST_CASE("parseStatusLineSegment.bareTextPlaceholderIsEmptyNotThrowing", "[statusline]")
{
    // "{Text}" without a `text=` is a plausible hand-editing typo. This parses user-authored config, so
    // it must not throw std::out_of_range at the user.
    auto segment = StatusLineSegment {};
    REQUIRE_NOTHROW(segment = parseStatusLineSegment("{Text}"sv));
    REQUIRE(segment.size() == 1);
    REQUIRE(std::holds_alternative<StatusLineDefinitions::Text>(segment[0]));
    CHECK(std::get<StatusLineDefinitions::Text>(segment[0]).text.empty());
}

TEST_CASE("serializeStatusLineSegment.textKeepsBracesAndCommasVerbatim", "[statusline]")
{
    SECTION("braces are not doubled")
    {
        // A template has no brace escaping (see crispy::parseInterpolatedString), so emitting "{{" for
        // "{" would not read back as one brace -- the text would gain a brace on every save.
        auto const text = canonical("a{b"sv);
        CHECK(text == "a{b");
        CHECK(canonical(text) == text);
    }

    SECTION("a comma in styled text falls back to an unstyled literal")
    {
        // "{Text:text=a,b}" would read back as text "a" plus a stray flag "b". Keeping the content and
        // losing the styling round-trips exactly; the other way corrupts.
        auto styles = StatusLineDefinitions::Styles {};
        styles.flags.enable(CellFlag::Bold);

        auto const text = serializeStatusLineSegment({ StatusLineDefinitions::Text { styles, "a,b" } });
        CHECK(text == "a,b");

        auto const readBack = parseStatusLineSegment(text);
        REQUIRE(readBack.size() == 1);
        REQUIRE(std::holds_alternative<StatusLineDefinitions::Text>(readBack[0]));
        CHECK(std::get<StatusLineDefinitions::Text>(readBack[0]).text == "a,b");
    }

    SECTION("styled text without a comma or brace keeps its styling")
    {
        auto styles = StatusLineDefinitions::Styles {};
        styles.flags.enable(CellFlag::Bold);

        auto const readBack = parseStatusLineSegment(
            serializeStatusLineSegment({ StatusLineDefinitions::Text { styles, "plain" } }));
        REQUIRE(readBack.size() == 1);
        REQUIRE(std::holds_alternative<StatusLineDefinitions::Text>(readBack[0]));

        auto const& got = std::get<StatusLineDefinitions::Text>(readBack[0]);
        CHECK(got.text == "plain");
        CHECK(got.flags.test(CellFlag::Bold));
    }
}

TEST_CASE("serializeStatusLineSegment.everyPlaceholderNameRoundTrips", "[statusline]")
{
    // Walks the whole vocabulary from ItemTraits, so a newly added placeholder is covered here the moment
    // it is added rather than whenever someone remembers to extend this list.
    StatusLineDefinitions::forEachItemType([](auto tag) {
        using T = decltype(tag)::type;
        auto const name = std::string(StatusLineDefinitions::ItemTraits<T>::Name);

        // Text, Command and Extension need their payload attribute to be recognized at all.
        // Capture-default rather than naming `name`: the branch that uses it is discarded for Text and
        // Command, which would make an explicit capture unused for those instantiations.
        auto const templateText = [&] -> std::string {
            if constexpr (std::same_as<T, StatusLineDefinitions::Text>)
                return "{Text:text=x}";
            else if constexpr (std::same_as<T, StatusLineDefinitions::Command>)
                return "{Command:Program=true}";
            else if constexpr (std::same_as<T, StatusLineDefinitions::Extension>)
                return "{Extension:git}";
            else
                return "{" + name + "}";
        }();

        auto const segment = parseStatusLineSegment(templateText);
        INFO("placeholder: " << name);
        REQUIRE(segment.size() == 1);
        REQUIRE(std::holds_alternative<T>(segment[0]));

        // And its serialized form parses back to the same type.
        auto const readBack = parseStatusLineSegment(serializeStatusLineSegment(segment));
        REQUIRE(readBack.size() == 1);
        CHECK(std::holds_alternative<T>(readBack[0]));
    });
}

TEST_CASE("serializeStatusLineSegment.preservesBothSideAdornments", "[statusline]")
{
    // Left= and Right= wrap an item's rendered value. The shipped defaults only use Left=, so Right= would
    // otherwise go untested on both sides of the round trip.
    auto styles = StatusLineDefinitions::Styles {};
    styles.textLeft = " | ";
    styles.textRight = " > ";

    auto const readBack =
        parseStatusLineSegment(serializeStatusLineSegment({ StatusLineDefinitions::Clock { styles } }));
    REQUIRE(readBack.size() == 1);
    REQUIRE(std::holds_alternative<StatusLineDefinitions::Clock>(readBack[0]));

    auto const& got = std::get<StatusLineDefinitions::Clock>(readBack[0]);
    CHECK(got.textLeft == " | ");
    CHECK(got.textRight == " > ");
}

TEST_CASE("parseStatusLineSegment.commandWithoutProgramIsNotAnItem", "[statusline]")
{
    // A {Command} that says nothing to run is not a usable item, so it takes the unknown-placeholder path
    // and is echoed verbatim rather than becoming a Command that would run an empty program.
    auto const segment = parseStatusLineSegment("{Command}"sv);
    REQUIRE(segment.size() == 1);
    REQUIRE(std::holds_alternative<StatusLineDefinitions::Text>(segment[0]));
    CHECK(std::get<StatusLineDefinitions::Text>(segment[0]).text == "{Command}");
}

TEST_CASE("parseStatusLineSegment.commandInterval", "[statusline]")
{
    using StatusLineDefinitions::Command;
    auto const segment = parseStatusLineSegment("{Command:Program=git status,Interval=30}");
    REQUIRE(segment.size() == 1);
    REQUIRE(std::holds_alternative<Command>(segment[0]));
    CHECK(std::get<Command>(segment[0]).command == "git status");
    CHECK(std::get<Command>(segment[0]).interval == std::chrono::seconds { 30 });
}

TEST_CASE("parseStatusLineSegment.commandIntervalDefaultsWhenAbsentOrInvalid", "[statusline]")
{
    using StatusLineDefinitions::Command;
    for (auto const* text: { "{Command:Program=date}",
                             "{Command:Program=date,Interval=0}",
                             "{Command:Program=date,Interval=-3}",
                             "{Command:Program=date,Interval=soon}" })
    {
        CAPTURE(text);
        auto const segment = parseStatusLineSegment(text);
        REQUIRE(segment.size() == 1);
        REQUIRE(std::holds_alternative<Command>(segment[0]));
        CHECK(std::get<Command>(segment[0]).interval == StatusLineDefinitions::DefaultCommandInterval);
    }
}

TEST_CASE("parseStatusLineSegment.commandShorthand", "[statusline]")
{
    using StatusLineDefinitions::Command;
    auto const segment = parseStatusLineSegment("[{Command:git log -1 --format=%h,%s}]");
    REQUIRE(segment.size() == 3);
    REQUIRE(std::holds_alternative<Command>(segment[1]));
    auto const& command = std::get<Command>(segment[1]);
    CHECK(command.command == "git log -1 --format=%h,%s");
    CHECK(command.interval == StatusLineDefinitions::DefaultCommandInterval);
    CHECK(command.flags == CellFlags {});
}

TEST_CASE("parseStatusLineSegment.commandIntervalWithoutProgramIsNotAnItem", "[statusline]")
{
    auto const segment = parseStatusLineSegment("{Command:Interval=5}");
    REQUIRE(segment.size() == 1);
    REQUIRE(std::holds_alternative<StatusLineDefinitions::Text>(segment[0]));
    CHECK(std::get<StatusLineDefinitions::Text>(segment[0]).text == "{Command:Interval=5}");
}

TEST_CASE("serializeStatusLineSegment.commandInterval", "[statusline]")
{
    CHECK(serializeStatusLineSegment(parseStatusLineSegment("{Command:Program=date}"))
          == "{Command:Program=date}");
    CHECK(serializeStatusLineSegment(parseStatusLineSegment("{Command:Program=date,Interval=30}"))
          == "{Command:Program=date,Interval=30}");
    CHECK(serializeStatusLineSegment(parseStatusLineSegment("{Command:date}")) == "{Command:Program=date}");
}

TEST_CASE("serializeStatusLineSegment.commandWithCommaRoundTripsAsShorthand", "[statusline]")
{
    using StatusLineDefinitions::Command;
    auto const segment = parseStatusLineSegment("{Command:git log -1 --format=%h,%s}");
    REQUIRE(segment.size() == 1);
    REQUIRE(std::holds_alternative<Command>(segment[0]));

    auto const text = serializeStatusLineSegment(segment);
    CHECK(text == "{Command:git log -1 --format=%h,%s}");

    auto const readBack = parseStatusLineSegment(text);
    REQUIRE(readBack.size() == 1);
    REQUIRE(std::holds_alternative<Command>(readBack[0]));
    CHECK(std::get<Command>(readBack[0]).command == "git log -1 --format=%h,%s");
    CHECK(std::get<Command>(readBack[0]).interval == StatusLineDefinitions::DefaultCommandInterval);
}

TEST_CASE("serializeStatusLineSegment.commandWithCommaAndIntervalStaysProgramForm", "[statusline]")
{
    // The shorthand cannot carry an interval, so a non-default one keeps the Program= form (lossy for the
    // comma, as before the shorthand existed).
    auto item = StatusLineDefinitions::Command { StatusLineDefinitions::Styles {}, "a,b" };
    item.interval = std::chrono::seconds { 30 };
    CHECK(serializeStatusLineSegment({ item }) == "{Command:Program=a,b,Interval=30}");
}

TEST_CASE("parseStatusLineSegment.commandShorthandWhitespaceOnlyIsNotAnItem", "[statusline]")
{
    auto const segment = parseStatusLineSegment("{Command: }");
    REQUIRE(segment.size() == 1);
    REQUIRE(std::holds_alternative<StatusLineDefinitions::Text>(segment[0]));
    CHECK(std::get<StatusLineDefinitions::Text>(segment[0]).text == "{Command: }");
}

namespace
{
/// What the status line's pointer items say, in this order: CellSGR, CellTextUtf8, CellTextUtf32, Hyperlink.
using PointerItemTexts = std::array<std::string, 4>;

/// What the pointer items say about the cell under @p terminal's pointer.
/// @param terminal The terminal whose status line it is.
/// @return One text per item, in PointerItemTexts' order.
[[nodiscard]] PointerItemTexts pointerItemTexts(Terminal const& terminal)
{
    auto const items = std::array<StatusLineDefinitions::Item, 4> { StatusLineDefinitions::CellSGR {},
                                                                    StatusLineDefinitions::CellTextUtf8 {},
                                                                    StatusLineDefinitions::CellTextUtf32 {},
                                                                    StatusLineDefinitions::Hyperlink {} };
    auto texts = PointerItemTexts {};
    std::ranges::transform(items, texts.begin(), [&](StatusLineDefinitions::Item const& item) {
        return serializeToVT(terminal, StatusLineSegment { item }, StatusLineStyling::Disabled);
    });
    return texts;
}

/// What the pointer items say about a cell.
/// @param flags     The cell's graphics flags.
/// @param text      Its text.
/// @param codepoint Its text's codepoint, as CellTextUtf32 spells it.
/// @param uri       The link it carries.
/// @return One text per item, in PointerItemTexts' order.
[[nodiscard]] PointerItemTexts describing(CellFlags flags,
                                          std::string_view text,
                                          std::string_view codepoint,
                                          std::string_view uri)
{
    return { std::format("{}", flags), std::string(text), std::string(codepoint), std::string(uri) };
}

/// Four rows of distinct cells, each styled and linked: H scrolls into the history, and A, B and C are left
/// on the three-row page, A in the top-left cell.
constexpr auto LinkedRows =
    std::string_view { "\033]8;;https://h.example/\033\\\033[3mH\033[m\033]8;;\033\\\r\n"
                       "\033]8;;https://a.example/\033\\\033[1mA\033[m\033]8;;\033\\\r\n"
                       "\033]8;;https://b.example/\033\\\033[4mB\033[m\033]8;;\033\\\r\n"
                       "C" };

constexpr auto PageWithHistory = PageSize { LineCount(3), ColumnCount(10) };

/// The cell at @p line, column 0 of the page as it is shown.
[[nodiscard]] constexpr CellLocation shownCell(int line)
{
    return CellLocation { .line = LineOffset(line), .column = ColumnOffset(0) };
}
} // namespace

TEST_CASE("serializeToVT.pointerItemsSayNothingBeforeAnyPointerEvent", "[statusline]")
{
    // The pointer is on no cell until an event names one -- not on the top-left one, styled, lettered and
    // linked here, which a fresh terminal's pointer position reads as.
    auto mc = MockTerm { PageWithHistory, LineCount(10) };
    mc.writeToScreen(LinkedRows);

    CHECK(pointerItemTexts(mc.terminal) == PointerItemTexts {});
}

TEST_CASE("serializeToVT.pointerItemsDescribeTheCellAMoveNamed", "[statusline]")
{
    auto mc = MockTerm { PageWithHistory, LineCount(10) };
    mc.writeToScreen(LinkedRows);

    mc.terminal.sendMouseMoveEvent(Modifier::None, shownCell(1), PixelCoordinate {}, false);
    CHECK(pointerItemTexts(mc.terminal)
          == describing(CellFlags { CellFlag::Underline }, "B", "U+0042", "https://b.example/"));
}

TEST_CASE("serializeToVT.pointerItemsSayNothingOnceThePointerHasLeft", "[statusline]")
{
    // Off the grid the pointer is on no cell, and the last one it was on is not described any more.
    auto mc = MockTerm { PageWithHistory, LineCount(10) };
    mc.writeToScreen(LinkedRows);
    mc.terminal.sendMouseMoveEvent(Modifier::None, shownCell(1), PixelCoordinate {}, false);
    REQUIRE(pointerItemTexts(mc.terminal) != PointerItemTexts {});

    mc.terminal.sendMouseLeaveEvent();
    CHECK(pointerItemTexts(mc.terminal) == PointerItemTexts {});
}

TEST_CASE("serializeToVT.pointerItemsDescribeTheCellAScrolledViewShowsUnderThePointer", "[statusline]")
{
    // Scrolled back one row, the view shows H on its top row, where the live page has A: the cell under the
    // pointer is H's.
    auto mc = MockTerm { PageWithHistory, LineCount(10) };
    mc.writeToScreen(LinkedRows);
    REQUIRE(mc.terminal.viewport().scrollUp(LineCount(1)));

    mc.terminal.sendMouseMoveEvent(Modifier::None, shownCell(0), PixelCoordinate {}, false);
    CHECK(pointerItemTexts(mc.terminal)
          == describing(CellFlags { CellFlag::Italic }, "H", "U+0048", "https://h.example/"));
}

TEST_CASE("serializeToVT.pointerItemsDescribeTheCellOfThePageOnDisplay", "[statusline]")
{
    // DECPCCM reset: page 3 is on display while the cursor writes to page 1, whose top-left cell is A. The
    // pointer is over page 3's top-left cell.
    auto mc = MockTerm { PageWithHistory, LineCount(10) };
    mc.writeToScreen(LinkedRows);
    mc.writeToScreen(
        "\033[3 P\033[H\033]8;;https://p.example/\033\\\033[7mP\033[m\033]8;;\033\\"); // PPA 3, P at home
    mc.writeToScreen("\033[?64l\033[1 P"); // DECPCCM reset, then the cursor back to page 1
    REQUIRE(mc.terminal.displayedPageIndex() == PageIndex(2));

    mc.terminal.sendMouseMoveEvent(Modifier::None, shownCell(0), PixelCoordinate {}, false);
    CHECK(pointerItemTexts(mc.terminal)
          == describing(CellFlags { CellFlag::Inverse }, "P", "U+0050", "https://p.example/"));
}

TEST_CASE("serializeToVT.historyItemCountsThePageOnDisplay", "[statusline]")
{
    // DECPCCM reset: page 1 is on display, with history, while the cursor writes to page 3, which has none.
    // The item counts what the viewport onto the page on display can scroll.
    auto mc = MockTerm { PageWithHistory, LineCount(100) };
    for (auto const i: std::views::iota(0, 20))
        mc.writeToScreen(std::format("line {}\r\n", i));
    mc.writeToScreen("\033[?64l\033[3 P");
    REQUIRE(mc.terminal.displayedPageIndex() == PageIndex(0));
    auto const scrollable = mc.terminal.viewport().scrollableLineCount();
    REQUIRE(scrollable > LineCount(0));
    auto const history = StatusLineSegment { StatusLineDefinitions::HistoryLineCount {} };

    CHECK(serializeToVT(mc.terminal, history, StatusLineStyling::Disabled) == std::format("{}", scrollable));
}

TEST_CASE("serializeToVT.clockReadsTheTerminalsWallClockInItsTimeZone", "[statusline]")
{
    // UTC+05:30, a zone no machine's own clock is likely to share with UTC.
    auto mc = MockTerm { PageSize { LineCount(3), ColumnCount(20) }, &fixedOffsetLocalTime<330> };
    mc.wallClock.advance(std::chrono::hours { 4 } + std::chrono::minutes { 5 }); // 04:05 UTC
    auto const clock = StatusLineSegment { StatusLineDefinitions::Clock {} };

    CHECK(serializeToVT(mc.terminal, clock, StatusLineStyling::Disabled) == "09:35");

    mc.wallClock.advance(std::chrono::hours { 1 } + std::chrono::minutes { 2 });
    CHECK(serializeToVT(mc.terminal, clock, StatusLineStyling::Disabled) == "10:37");
}

TEST_CASE("StatusLineBuilder.extension.parse", "[statusline][extensions]")
{
    using StatusLineDefinitions::Extension;

    SECTION("shorthand")
    {
        auto const segment = parseStatusLineSegment("{Extension:git}"sv);
        REQUIRE(segment.size() == 1);
        REQUIRE(std::holds_alternative<Extension>(segment[0]));
        auto const& item = std::get<Extension>(segment[0]);
        CHECK(item.name == "git");
        CHECK_FALSE(item.foregroundColor.has_value());
        CHECK_FALSE(item.flags.any());
        CHECK(item.textLeft.empty());
    }

    SECTION("attributes keep their styles")
    {
        auto const segment = parseStatusLineSegment("{Extension:Name=git,Color=#ff0000,Left=[}"sv);
        REQUIRE(segment.size() == 1);
        REQUIRE(std::holds_alternative<Extension>(segment[0]));
        auto const& item = std::get<Extension>(segment[0]);
        CHECK(item.name == "git");
        REQUIRE(item.foregroundColor.has_value());
        CHECK(*item.foregroundColor == StatusLineDefinitions::ItemColor { RGBColor { 0xFF, 0x00, 0x00 } });
        CHECK(item.textLeft == "[");
    }

    SECTION("a shorthand that spells a flag is the name, not a style")
    {
        auto const segment = parseStatusLineSegment("{Extension:Bold}"sv);
        REQUIRE(segment.size() == 1);
        REQUIRE(std::holds_alternative<Extension>(segment[0]));
        CHECK(std::get<Extension>(segment[0]).name == "Bold");
        CHECK_FALSE(std::get<Extension>(segment[0]).flags.any());
    }

    SECTION("without a name it is no item")
    {
        for (auto const text: { "{Extension}"sv, "{Extension:Color=#ff0000}"sv, "{Extension: }"sv })
        {
            INFO(text);
            auto const segment = parseStatusLineSegment(text);
            REQUIRE(segment.size() == 1);
            REQUIRE(std::holds_alternative<StatusLineDefinitions::Text>(segment[0]));
            CHECK(std::get<StatusLineDefinitions::Text>(segment[0]).text == text);
        }
    }

    SECTION("both forms round-trip")
    {
        for (auto const text: { "{Extension:git}"sv, "{Extension:Name=git,Bold,Color=#FF0000,Left=[}"sv })
        {
            INFO(text);
            auto const segment = parseStatusLineSegment(text);
            CHECK(serializeStatusLineSegment(segment) == text);
        }
    }
}

// {{{ Role colours
// A colour attribute names either a hex colour, drawn as written, or one of five scheme roles, resolved
// against the terminal's palette each time the line is drawn -- so a role follows a scheme change.

TEST_CASE("statusLine.colors.everyRoleParsesAndRoundTrips", "[statusline][colors]")
{
    using StatusLineDefinitions::ItemColor;
    for (auto const& row: StatusLineDefinitions::ColorRoleNames)
    {
        INFO(row.name);
        CHECK(StatusLineDefinitions::parseItemColor(row.name) == ItemColor { row.role });
        auto const upper = core::toUpper(row.name);
        CHECK(StatusLineDefinitions::parseItemColor(upper) == ItemColor { row.role });
        CHECK(StatusLineDefinitions::formatItemColor(ItemColor { row.role }) == row.name);
    }

    CHECK(StatusLineDefinitions::parseItemColor("#FF0000") == ItemColor { RGBColor { 0xFF, 0, 0 } });
    CHECK_FALSE(StatusLineDefinitions::parseItemColor("magenta").has_value());
    CHECK_FALSE(StatusLineDefinitions::parseItemColor("Yellow").has_value());
    CHECK_FALSE(StatusLineDefinitions::parseItemColor("").has_value());
}

TEST_CASE("statusLine.colors.hexAndRolesMix", "[statusline][colors]")
{
    constexpr auto Mixed =
        "{VTType:Color=#C0C030}{Clock:Color=muted}{Title:Color=#000000,BackgroundColor=accent}"sv;
    CHECK(serializeStatusLineSegment(parseStatusLineSegment(Mixed)) == Mixed);

    // Written lowercase, whatever the spelling it was read with.
    CHECK(canonical("{Clock:Color=Accent}"sv) == "{Clock:Color=accent}");

    // A word that is neither a role nor a colour leaves the colour unset rather than turning it black.
    auto const unknown = parseStatusLineSegment("{Clock:Color=magenta}"sv);
    REQUIRE(unknown.size() == 1);
    REQUIRE(std::holds_alternative<StatusLineDefinitions::Clock>(unknown[0]));
    CHECK_FALSE(std::get<StatusLineDefinitions::Clock>(unknown[0]).foregroundColor.has_value());
}

namespace
{
/// The SGR that selects @p color as the text colour.
[[nodiscard]] std::string textSgrOf(RGBColor color)
{
    return std::format("\033[38:2:{}:{}:{}m", color.red, color.green, color.blue);
}

/// Draws every status-line bar of @p palette, whatever the focus and the input mode, on black.
void paintEveryBarBlack(ColorPalette& palette)
{
    for (auto* const pair: { &palette.indicatorStatusLineInactive,
                             &palette.indicatorStatusLineInsertMode,
                             &palette.indicatorStatusLineNormalMode,
                             &palette.indicatorStatusLineVisualMode })
        pair->background = 0x000000_rgb;
}
} // namespace

TEST_CASE("serializeToVT.roleIsResolvedWhenDrawn", "[statusline][colors]")
{
    auto mc = MockTerm { PageSize { LineCount(3), ColumnCount(20) } };
    paintEveryBarBlack(mc.terminal.colorPalette());
    mc.terminal.defaultColorPalette().palette[4] = 0x5599FF_rgb;
    auto const segment = parseStatusLineSegment("{VTType:Color=accent}"sv);

    CHECK(serializeToVT(mc.terminal, segment, StatusLineStyling::Enabled).contains(textSgrOf(0x5599FF_rgb)));

    // A scheme change reaches the next frame: nothing resolved the role when the template was parsed.
    mc.terminal.defaultColorPalette().palette[4] = 0xFFAA00_rgb;
    CHECK(serializeToVT(mc.terminal, segment, StatusLineStyling::Enabled).contains(textSgrOf(0xFFAA00_rgb)));

    // A role the bar would swallow gives way to the bar's legible ink: the scheme's better end on it.
    mc.terminal.defaultColorPalette().palette[4] = 0x123456_rgb;
    CHECK(serializeToVT(mc.terminal, segment, StatusLineStyling::Enabled)
              .contains(textSgrOf(legibleInkOn(mc.terminal.defaultColorPalette(), 0x000000_rgb))));

    // A hex colour is the user's, drawn as written even where it does not read.
    CHECK(serializeToVT(
              mc.terminal, parseStatusLineSegment("{VTType:Color=#123456}"sv), StatusLineStyling::Enabled)
              .contains(textSgrOf(0x123456_rgb)));
}

TEST_CASE("serializeToVT.rolesIgnoreTheProgramsPalette", "[statusline][colors]")
{
    // The status line is the host's chrome: OSC 4 recolours the program's own text, never the furniture.
    auto mc = MockTerm { PageSize { LineCount(3), ColumnCount(20) } };
    paintEveryBarBlack(mc.terminal.colorPalette());
    mc.terminal.defaultColorPalette().palette[4] = 0x5599FF_rgb;
    mc.terminal.defaultColorPalette().palette[8] = 0xA0A0A0_rgb;
    mc.terminal.resetColorPalette();
    paintEveryBarBlack(mc.terminal.colorPalette());

    mc.writeToScreen("\033]4;4;rgb:ff/ff/00\033\\\033]4;8;rgb:00/00/00\033\\");
    REQUIRE(mc.terminal.colorPalette().palette[4] == 0xFFFF00_rgb);

    auto const line = serializeToVT(mc.terminal,
                                    parseStatusLineSegment("{VTType:Color=accent}{Clock:Color=muted}"sv),
                                    StatusLineStyling::Enabled);
    CHECK(line.contains(textSgrOf(0x5599FF_rgb)));
    CHECK(line.contains(textSgrOf(0xA0A0A0_rgb)));
    CHECK_FALSE(line.contains(textSgrOf(0xFFFF00_rgb)));
}

TEST_CASE("serializeToVT.roleReadsOnTheBarOfTheModeItIsDrawnIn", "[statusline][colors]")
{
    // The ground is the bar the line is drawn on now: vi Normal mode's, not the idle one's.
    auto mc = MockTerm { PageSize { LineCount(3), ColumnCount(20) } };
    REQUIRE(defaultColorPalettes("contour-dark", mc.terminal.defaultColorPalette()));
    mc.terminal.resetColorPalette();
    mc.terminal.inputHandler().setMode(ViMode::Normal);

    auto const& palette = mc.terminal.defaultColorPalette();
    auto const accent = StatusLineDefinitions::ItemColor { StatusLineDefinitions::ColorRole::Accent };
    auto const onNormal = StatusLineDefinitions::resolveItemColor(
        accent, palette, palette.indicatorStatusLineNormalMode.background);
    auto const onInactive = StatusLineDefinitions::resolveItemColor(
        accent, palette, palette.indicatorStatusLineInactive.background);
    REQUIRE(onNormal != onInactive); // contour-dark's accent is its Normal bar: the fallback must show

    auto const line = serializeToVT(
        mc.terminal, parseStatusLineSegment("{VTType:Color=accent}"sv), StatusLineStyling::Enabled);
    CHECK(line.contains(textSgrOf(onNormal)));
    CHECK_FALSE(line.contains(textSgrOf(onInactive)));
}

TEST_CASE("statusLine.colors.rolesReadOnContourSchemes", "[statusline][colors]")
{
    // The roles' own colours, before any fallback: the Contour schemes are designed for them on the idle bar.
    for (auto const scheme: { "contour-dark"sv, "contour-light"sv })
    {
        auto palette = ColorPalette {};
        REQUIRE(defaultColorPalettes(scheme, palette));
        auto const background = palette.indicatorStatusLineInactive.background;
        for (auto const& row: StatusLineDefinitions::ColorRoleNames)
        {
            INFO(scheme << ": " << row.name);
            auto const ratio = contrastRatio(StatusLineDefinitions::roleColor(row.role, palette), background);
            auto const minimum = row.role == StatusLineDefinitions::ColorRole::Muted ? 3.0 : 4.5;
            CHECK(ratio >= minimum);
        }
    }
}

TEST_CASE("statusLine.colors.rolesReadOnEveryBar", "[statusline][colors]")
{
    // Resolved against the bar it is drawn on, every role reads on every bundled scheme's every bar.
    for (auto const scheme: defaultColorPaletteNames())
    {
        auto palette = ColorPalette {};
        REQUIRE(defaultColorPalettes(scheme, palette));
        for (auto const& bar: { palette.indicatorStatusLineInactive,
                                palette.indicatorStatusLineNormalMode,
                                palette.indicatorStatusLineInsertMode,
                                palette.indicatorStatusLineVisualMode })
            for (auto const& row: StatusLineDefinitions::ColorRoleNames)
            {
                INFO(scheme << ": " << row.name << " on " << formatColor(bar.background));
                auto const ink = StatusLineDefinitions::resolveItemColor(
                    StatusLineDefinitions::ItemColor { row.role }, palette, bar.background);
                CHECK(contrastRatio(ink, bar.background) >= MinimumTextContrast);
            }
    }
}
// }}}

// {{{ {Cwd}
namespace
{
/// The home directory every `{Cwd}` case below runs with, whatever the machine's own.
constexpr auto TestHome = "/home/bob"sv;

/// What `{Cwd}` shows once the shell has reported @p url (OSC 7), with @ref TestHome as the home.
[[nodiscard]] std::string cwdAfterReporting(std::string_view url)
{
    auto const environment = core::testing::FakeEnvironment { { { "HOME", std::string { TestHome } } } };
    auto mc = MockTerm { PageSize { LineCount(3), ColumnCount(80) }, environment };
    mc.writeToScreen(std::format("\033]7;{}\033\\", url));
    return serializeToVT(mc.terminal, parseStatusLineSegment("{Cwd}"sv), StatusLineStyling::Disabled);
}

/// The width of @p text in columns, as the grid lays it out (mode 2027, which Contour keeps set).
[[nodiscard]] int columnsOf(std::string_view text)
{
    return unbox<int>(columnsOnTheGrid(text, ClusterWidthPolicy::ClusterAware));
}

/// @p text repeated @p count times.
[[nodiscard]] std::string repeated(std::string_view text, int count)
{
    auto result = std::string {};
    for ([[maybe_unused]] auto const _: std::views::iota(0, count))
        result += text;
    return result;
}

constexpr auto Family = "\U0001F468\u200D\U0001F469\u200D\U0001F467"sv;
constexpr auto Heart = "\u2764\uFE0F"sv;
constexpr auto Flag = "\U0001F1E9\U0001F1EA"sv;
constexpr auto DecomposedE = "e\u0301"sv;
} // namespace

TEST_CASE("serializeToVT.cwdShowsWhereTheShellIs", "[statusline][cwd]")
{
    auto mc = MockTerm { PageSize { LineCount(3), ColumnCount(80) } };
    CHECK(serializeToVT(mc.terminal, parseStatusLineSegment("{Cwd}"sv), StatusLineStyling::Disabled).empty());

    CHECK(cwdAfterReporting("file:///srv/app") == "/srv/app");
    // Shown though it is another machine's: the status line says where the user is.
    CHECK(cwdAfterReporting("file://elsewhere.example/srv/app") == "/srv/app");

    CHECK(cwdAfterReporting("file:///home/bob/repo/contour") == "~/repo/contour");
    // This machine's home says nothing about another machine's paths, as the pane header has it.
    CHECK(cwdAfterReporting("file://elsewhere.example/home/bob/x") == "/home/bob/x");
}

TEST_CASE("serializeToVT.cwdTakesTheHomeItIsGiven", "[statusline][cwd]")
{
    // The composition root's home (the GUI's QDir::homePath(), the pane header's) wins over the
    // environment's, so the status line and the header above it abbreviate alike.
    auto const environment = core::testing::FakeEnvironment { { { "HOME", "/elsewhere" } } };
    auto settings = MockTerm<>::createSettings(PageSize { LineCount(3), ColumnCount(80) }, {}, 1024);
    settings.homeDirectory = "/home/bob";
    auto mc = MockTerm { std::move(settings), environment };
    CHECK(mc.terminal.homeDirectory() == "/home/bob");
    mc.writeToScreen("\033]7;file:///home/bob/src\033\\");
    CHECK(serializeToVT(mc.terminal, parseStatusLineSegment("{Cwd}"sv), StatusLineStyling::Disabled)
          == "~/src");

    // Without one, the environment's.
    auto plain = MockTerm { PageSize { LineCount(3), ColumnCount(80) }, environment };
    CHECK(plain.terminal.homeDirectory() == "/elsewhere");
}

TEST_CASE("serializeToVT.cwdFindsAWindowsHome", "[statusline][cwd]")
{
    // Windows names the home USERPROFILE, not HOME.
    auto const environment = core::testing::FakeEnvironment { { { "USERPROFILE", "/Users/bob" } } };
    auto mc = MockTerm { PageSize { LineCount(3), ColumnCount(80) }, environment };
    CHECK(mc.terminal.homeDirectory() == "/Users/bob");
}

TEST_CASE("serializeToVT.cwdWithATrailingSlash", "[statusline][cwd]")
{
    // A trailing separator names no further directory, so a long path elides to its last real one rather
    // than to "/…/" or to nothing.
    auto deep = std::string { "file://" };
    for (auto const level: std::views::iota(0, 60))
        deep += std::format("/d{}", level);
    CHECK(cwdAfterReporting(deep + "/") == "/…/d59");
    CHECK(cwdAfterReporting("file:///" + std::string(300, 'a') + "/").starts_with("…a"));
    CHECK(cwdAfterReporting("file:///") == "/");
}

TEST_CASE("serializeToVT.cwdOfADeepOrBrokenPath", "[statusline][cwd]")
{
    auto deep = std::string { "file://" };
    for (auto const level: std::views::iota(0, 60))
        deep += std::format("/d{}", level);
    CHECK(cwdAfterReporting(deep) == "/…/d59");

    auto const wide = cwdAfterReporting("file:///" + std::string(300, 'a'));
    CHECK(columnsOf(wide) <= 40);
    CHECK(wide.starts_with("…"));

    CHECK(cwdAfterReporting("file:///tmp/a%FFb") == "/tmp/a\uFFFDb");
    CHECK_FALSE(cwdAfterReporting("file:///tmp/a%1Bb").contains('\033'));
}

TEST_CASE("serializeToVT.cwdElidesByGraphemeCluster", "[statusline][cwd]")
{
    // A long last component is cut at its start, a whole cluster at a time: never a base without its mark,
    // never half a ZWJ sequence or flag, and measured as the grid draws it.
    struct Row
    {
        std::string_view name;
        std::string_view cluster;
        int keptOf40; ///< Clusters kept after the "…" at the default MaxWidth of 40.
    };
    for (auto const& row: { Row { "CJK", "漢"sv, 19 },
                            Row { "decomposed é", DecomposedE, 39 },
                            Row { "ZWJ family", Family, 19 },
                            Row { "flag", Flag, 19 },
                            Row { "VS16 heart", Heart, 19 } })
    {
        INFO(row.name);
        auto const shown = cwdAfterReporting("file:///" + repeated(row.cluster, 60));
        CHECK(shown == "…" + repeated(row.cluster, row.keptOf40));
        CHECK(columnsOf(shown) <= 40);
    }

    // Measured as the grid draws it, a path that fits is not elided because its codepoints would not.
    auto const families = "/f/" + repeated(Family, 7);
    CHECK(cwdAfterReporting("file://" + families) == families);
}

TEST_CASE("parseStatusLineSegment.maxWidthIsGeneric", "[statusline][cwd]")
{
    CHECK(canonical("{Cwd:MaxWidth=20}"sv) == "{Cwd:MaxWidth=20}");
    auto const narrow = parseStatusLineSegment("{Cwd:MaxWidth=20}"sv);
    REQUIRE(narrow.size() == 1);
    REQUIRE(std::holds_alternative<StatusLineDefinitions::Cwd>(narrow[0]));
    CHECK(std::get<StatusLineDefinitions::Cwd>(narrow[0]).maxWidth == ColumnCount(20));

    auto const plain = parseStatusLineSegment("{Cwd}"sv);
    REQUIRE(plain.size() == 1);
    REQUIRE(std::holds_alternative<StatusLineDefinitions::Cwd>(plain[0]));
    CHECK(std::get<StatusLineDefinitions::Cwd>(plain[0]).maxWidth == ColumnCount(40));
    CHECK(serializeStatusLineSegment(plain) == "{Cwd}");

    auto const zero = parseStatusLineSegment("{Cwd:MaxWidth=0}"sv);
    REQUIRE(zero.size() == 1);
    REQUIRE(std::holds_alternative<StatusLineDefinitions::Cwd>(zero[0]));
    CHECK(std::get<StatusLineDefinitions::Cwd>(zero[0]).maxWidth == ColumnCount(40));
}
// }}}

// {{{ {Git}
namespace
{
/// The indicator status line's text, trailing blanks dropped, after a fresh frame.
[[nodiscard]] std::string indicatorTextAfterRefresh(Terminal& terminal)
{
    terminal.refreshRenderBuffer();
    auto text = terminal.indicatorStatusLineDisplay().renderMainPageText();
    while (!text.empty() && (text.back() == ' ' || text.back() == '\n'))
        text.pop_back();
    return text;
}
} // namespace

namespace
{
/// A terminal showing `{Git}`, whose lookups read in-memory files and run when a test drains their executor.
struct GitTerm
{
    core::platform::testing::InMemoryFileSystem files;
    test::InMemoryGitFileSystem git { files };
    core::async::testing::ManualExecutor executor;
    MockTerm<> mc { PageSize { LineCount(3), ColumnCount(40) },
                    files,
                    GitHeadServices { .executor = executor, .fileSystem = git } };

    GitTerm()
    {
        files.addFile("/r/.git/HEAD", "ref: refs/heads/main\n");
        REQUIRE(files.createDirectories("/r/src"));
        mc.terminal.setStatusLineDefinition(parseStatusLineDefinition("{Git}", "", ""));
        mc.terminal.setStatusDisplay(StatusDisplayType::Indicator);
    }

    /// The line once every lookup a frame asked for has run and the frame its answer asks for is drawn.
    [[nodiscard]] std::string settledLine()
    {
        mc.terminal.refreshRenderBuffer();
        executor.drain();
        return indicatorTextAfterRefresh(mc.terminal);
    }
};
} // namespace

TEST_CASE("serializeToVT.gitFollowsTheShell", "[statusline][git]")
{
    auto term = GitTerm {};
    auto& mc = term.mc;
    mc.writeToScreen("\033]7;file:///r/src\033\\");
    CHECK(term.settledLine() == "main");

    // A frame asks for nothing: only a command's end or a directory report makes it look again.
    REQUIRE(term.files.writeFile("/r/.git/HEAD", "ref: refs/heads/next\n"));
    CHECK(term.settledLine() == "main");
    CHECK(term.settledLine() == "main");

    test::typeAtPrompt(mc, "git switch next");
    mc.writeToScreen("\033]133;C\033\\");
    mc.writeToScreen("\033]133;D;0\033\\");
    CHECK(term.settledLine() == "next");

    // Another machine's directory has no branch here, whatever this machine holds at that path.
    mc.writeToScreen("\033]7;file://elsewhere.example/r/src\033\\");
    CHECK(term.settledLine().empty());
}

TEST_CASE("serializeToVT.gitLooksAgainWhenTheShellReReportsItsDirectory", "[statusline][git]")
{
    // A shell re-reports its directory at every prompt; a branch switched by a program that marked no command
    // shows at the next one.
    auto term = GitTerm {};
    term.mc.writeToScreen("\033]7;file:///r/src\033\\");
    CHECK(term.settledLine() == "main");

    REQUIRE(term.files.writeFile("/r/.git/HEAD", "ref: refs/heads/next\n"));
    term.mc.writeToScreen("\033]7;file:///r/src\033\\");
    CHECK(term.settledLine() == "next");
}

TEST_CASE("serializeToVT.gitNeverReadsOnTheFrame", "[statusline][git]")
{
    auto term = GitTerm {};
    auto& terminal = term.mc.terminal;
    term.mc.writeToScreen("\033]7;file:///r/src\033\\");

    // The frame, under the terminal's lock, only queues a lookup: nothing is read until the executor runs it.
    CHECK(indicatorTextAfterRefresh(terminal).empty());
    CHECK(indicatorTextAfterRefresh(terminal).empty());
    CHECK(term.git.calls() == 0);
    CHECK(term.executor.pending() == 1);

    // Its answer asks for a frame, which shows it.
    terminal.ensureFreshRenderBuffer();
    REQUIRE(terminal.renderBufferState() == RenderBufferState::WaitingForRefresh);
    term.executor.drain();
    CHECK(term.git.calls() > 0);
    CHECK(terminal.renderBufferState() == RenderBufferState::RefreshBuffersAndTrySwap);
    CHECK(indicatorTextAfterRefresh(terminal) == "main");
}

TEST_CASE("serializeToVT.gitFollowsADaemonClientsCommands", "[statusline][git]")
{
    // A daemon client runs no parser: a command's end reaches it as an adopted record, never as OSC 133 D.
    auto term = GitTerm {};
    auto& terminal = term.mc.terminal;
    term.mc.writeToScreen("\033]7;file:///r/src\033\\");
    terminal.bindReplica(
        ReplicaRole::SameMachine,
        HostIdentity { .machineId = "3deb5353d3ba43d08201c136a47ead7b", .hostname = "zeta" });
    CHECK(term.settledLine() == "main");

    REQUIRE(term.files.writeFile("/r/.git/HEAD", "ref: refs/heads/next\n"));
    terminal.adoptCommandBlock(test::mirroredRecord(5, CommandBlockState::Running), AdoptMode::Live);
    CHECK(term.settledLine() == "main");

    terminal.adoptCommandBlock(test::mirroredFinish(5, 0, CommandBlockEnd::Reported), AdoptMode::Live);
    CHECK(term.settledLine() == "next");
}

TEST_CASE("serializeToVT.gitShowsNoBranchForAReftableRepository", "[statusline][git]")
{
    // reftable keeps HEAD in .git/reftable/ and leaves .git/HEAD naming the stub branch ".invalid".
    auto term = GitTerm {};
    term.files.addFile("/t/.git/HEAD", "ref: refs/heads/.invalid\n");
    term.files.addDirectory("/t/.git/reftable");
    term.mc.writeToScreen("\033]7;file:///t\033\\");
    CHECK(term.settledLine().empty());
}

TEST_CASE("serializeToVT.gitShowsNothingWithoutGitServices", "[statusline][git]")
{
    // A terminal given no Git services -- a daemon's host terminal -- shows no branch and reads nothing.
    auto files = core::platform::testing::InMemoryFileSystem {};
    files.addFile("/r/.git/HEAD", "ref: refs/heads/main\n");
    auto mc = MockTerm { PageSize { LineCount(3), ColumnCount(40) }, files };
    mc.terminal.setStatusLineDefinition(parseStatusLineDefinition("{Git}", "", ""));
    mc.terminal.setStatusDisplay(StatusDisplayType::Indicator);
    mc.writeToScreen("\033]7;file:///r\033\\");
    CHECK(indicatorTextAfterRefresh(mc.terminal).empty());
}
// }}}

// {{{ {LastCommand}
namespace
{
/// What `{LastCommand…}` (@p placeholder) shows on @p mc now.
[[nodiscard]] std::string lastCommandOf(MockTerm<>& mc,
                                        StatusLineStyling styling = StatusLineStyling::Disabled,
                                        std::string_view placeholder = "{LastCommand}")
{
    return serializeToVT(mc.terminal, parseStatusLineSegment(placeholder), styling);
}

/// Runs @p command at a fresh prompt for @p duration, ending it with @p exitCode and a new prompt.
void runFor(MockTerm<>& mc, std::string_view command, std::chrono::milliseconds duration, int exitCode)
{
    test::typeAtPrompt(mc, command);
    mc.writeToScreen("\033]133;C\033\\");
    mc.steadyClock.advance(duration);
    mc.writeToScreen(test::commandEnd(exitCode));
    mc.writeToScreen("\033]133;A\033\\$ ");
}

/// The SGR that selects @p role's colour on @p mc's palette as the text colour.
[[nodiscard]] std::string roleSgr(MockTerm<> const& mc, StatusLineDefinitions::ColorRole role)
{
    auto const rgb =
        StatusLineDefinitions::resolveItemColor(StatusLineDefinitions::ItemColor { role },
                                                mc.terminal.defaultColorPalette(),
                                                mc.terminal.indicatorStatusLineColors().background);
    return std::format("\033[38:2:{}:{}:{}m", rgb.red, rgb.green, rgb.blue);
}
} // namespace

TEST_CASE("serializeToVT.lastCommandSaysHowItWent", "[statusline][lastcommand]")
{
    using namespace std::chrono_literals;
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(80) } };
    CHECK(lastCommandOf(mc).empty());

    runFor(mc, "ctest", 134s, 0);
    CHECK(lastCommandOf(mc) == "✓ ctest · 2m 14s");

    runFor(mc, "ctest -R gui", 12'400ms, 8);
    CHECK(lastCommandOf(mc) == "✗ exit 8 ctest -R gui · 12s");

    // While a command runs, the last one is no longer news.
    test::typeAtPrompt(mc, "sleep 9");
    mc.writeToScreen("\033]133;C\033\\");
    CHECK(lastCommandOf(mc).empty());

    // A block a newer prompt ended without ;D reported nothing about how it went.
    mc.writeToScreen("\033]133;A\033\\$ ");
    CHECK(lastCommandOf(mc).empty());
}

TEST_CASE("serializeToVT.lastCommandIsBoundedAndSafe", "[statusline][lastcommand]")
{
    using namespace std::chrono_literals;
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(200) } };

    auto const command = std::string(100, 'x');
    runFor(mc, command, 4ms, 0);
    auto const text = lastCommandOf(mc);
    auto constexpr Status = "✓ "sv;
    auto constexpr Duration = " · 4ms"sv;
    REQUIRE(text.starts_with(Status));
    REQUIRE(text.ends_with(Duration));
    auto const commandPart =
        std::string_view { text }.substr(Status.size(), text.size() - Status.size() - Duration.size());
    CHECK(columnsOf(commandPart) == 32);
    CHECK(commandPart.ends_with("…"));
    CHECK(commandPart.starts_with("xxx"));

    mc.writeToScreen(test::reportedCommandStart("evil%1B%5B2Jx"));
    mc.writeToScreen(test::commandEnd(0));
    CHECK_FALSE(lastCommandOf(mc).contains('\033'));
    CHECK(lastCommandOf(mc).contains("evil"));
}

TEST_CASE("serializeToVT.lastCommandFoldsLineBreaksToSpaces", "[statusline][lastcommand]")
{
    // A multi-line command reads on a one-line bar, rather than as a replacement character per break.
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(80) } };
    mc.writeToScreen(test::reportedCommandStart("for%20f%20in%20x%0Ado%09echo%0D%0Adone"));
    mc.writeToScreen(test::commandEnd(0));
    CHECK(lastCommandOf(mc) == "✓ for f in x do echo  done · 0ms");
}

TEST_CASE("serializeToVT.lastCommandElidesByGraphemeCluster", "[statusline][lastcommand]")
{
    using namespace std::chrono_literals;
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(200) } };
    for (auto const cluster: { Family, Heart })
    {
        runFor(mc, repeated(cluster, 30), 4ms, 0);
        auto const text = lastCommandOf(mc, StatusLineStyling::Disabled, "{LastCommand:MaxWidth=11}");
        // Five whole two-column clusters and the "…": the next would not fit the eleven columns.
        CHECK(text == "✓ " + repeated(cluster, 5) + "… · 4ms");
    }
}

TEST_CASE("serializeToVT.lastCommandColoursItsStatus", "[statusline][lastcommand]")
{
    using namespace std::chrono_literals;
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(80) } };
    runFor(mc, "ctest -R gui", 12'400ms, 8);

    auto const coloured = lastCommandOf(mc, StatusLineStyling::Enabled);
    CHECK(coloured.contains(roleSgr(mc, StatusLineDefinitions::ColorRole::Danger) + "✗"));
    CHECK(coloured.contains(roleSgr(mc, StatusLineDefinitions::ColorRole::Muted) + " · "));

    // The item's own colour wins: it is the only one drawn.
    auto const own = lastCommandOf(mc, StatusLineStyling::Enabled, "{LastCommand:Color=#102030}");
    CHECK(own.contains("38:2:16:32:48"));
    CHECK(std::ranges::distance(own | std::views::split("38:2"sv)) == 2);
}

TEST_CASE("parseStatusLineSegment.lastCommandMaxWidth", "[statusline][lastcommand]")
{
    CHECK(canonical("{LastCommand:MaxWidth=20}"sv) == "{LastCommand:MaxWidth=20}");
    CHECK(canonical("{LastCommand}"sv) == "{LastCommand}");
}
// }}}
