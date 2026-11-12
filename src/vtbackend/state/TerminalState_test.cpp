// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/state/TerminalState.hpp>
#include <vtbackend/state/TerminalStateCodec.hpp>
#include <vtbackend/testing/MockTerm.hpp>
#include <vtbackend/testing/Workloads.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <format>
#include <memory>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

using namespace vtbackend;
using namespace std::string_view_literals;

namespace
{
using Mock = MockTerm<vtpty::MockPty>;

constexpr auto TestPage = PageSize { LineCount(6), ColumnCount(20) };
constexpr auto TestHistory = LineCount(10);

std::unique_ptr<Mock> makeTerminal()
{
    auto mock = std::make_unique<Mock>(TestPage, TestHistory);
    mock->terminal.setCellPixelSize(ImageSize { Width(10), Height(20) });
    return mock;
}

/// The bytes a capture of @p state encodes to: the identity two terminals are the same state by.
std::vector<uint8_t> encoded(TerminalState const& state)
{
    return encodeTerminalState(state);
}

/// @p state, after the byte codec.
TerminalState throughBytes(TerminalState const& state)
{
    auto decoded = decodeTerminalState(encoded(state));
    REQUIRE(decoded.has_value());
    return *std::move(decoded);
}

/// A fresh terminal that is @p source restored through the byte codec.
std::unique_ptr<Mock> restoredCopyOf(Mock const& source)
{
    auto target = makeTerminal();
    auto const result = target->terminal.restoreState(throughBytes(source.terminal.captureState()));
    INFO(std::format("restore failed with error {}", result ? -1 : static_cast<int>(result.error())));
    REQUIRE(result.has_value());
    return target;
}

/// Feeds @p suffix to both and checks they are still the same terminal.
void requireSameAfter(Mock& source, Mock& restored, std::string_view suffix)
{
    source.writeToScreen(suffix);
    restored.writeToScreen(suffix);
    CHECK(source.terminal.primaryScreen().renderMainPageText()
          == restored.terminal.primaryScreen().renderMainPageText());
    CHECK(source.terminal.peekInput() == restored.terminal.peekInput());
    CHECK(encoded(source.terminal.captureState()) == encoded(restored.terminal.captureState()));
}

constexpr auto SomeCell = CellLocation { .line = LineOffset(1), .column = ColumnOffset(2) };
} // namespace

TEST_CASE("a fresh terminal restores onto an identical fresh terminal", "[TerminalState]")
{
    auto source = makeTerminal();
    auto restored = restoredCopyOf(*source);
    CHECK(encoded(source->terminal.captureState()) == encoded(restored->terminal.captureState()));
}

TEST_CASE("mouse SGR tracking survives -- a click after restore reports SGR", "[TerminalState]")
{
    auto source = makeTerminal();
    source->writeToScreen("\033[?1000h\033[?1006h"sv);
    auto restored = restoredCopyOf(*source);

    static_cast<void>(restored->terminal.sendMousePressEvent(
        Modifiers {}, MouseButton::Left, SomeCell, PixelCoordinate {}, false));
    CHECK(restored->mockPty().stdinBuffer() == "\033[<0;3;2M"sv);
    static_cast<void>(source->terminal.sendMousePressEvent(
        Modifiers {}, MouseButton::Left, SomeCell, PixelCoordinate {}, false));
    CHECK(source->mockPty().stdinBuffer() == restored->mockPty().stdinBuffer());
}

TEST_CASE("bracketed paste survives", "[TerminalState]")
{
    auto source = makeTerminal();
    source->writeToScreen("\033[?2004h"sv);
    auto restored = restoredCopyOf(*source);
    restored->terminal.sendPaste("abc");
    CHECK(restored->mockPty().stdinBuffer() == "\033[200~abc\033[201~"sv);
}

TEST_CASE("kitty keyboard stack of three pushes pops back correctly after restore", "[TerminalState]")
{
    auto source = makeTerminal();
    source->writeToScreen("\033[>1u\033[>3u\033[>7u"sv);
    auto restored = restoredCopyOf(*source);

    // Two pops land on the first push, flags 1.
    restored->writeToScreen("\033[<2u\033[?u"sv);
    CHECK(restored->terminal.peekInput() == "\033[?1u"sv);
    source->writeToScreen("\033[<2u\033[?u"sv);
    CHECK(source->terminal.peekInput() == restored->terminal.peekInput());
    CHECK(encoded(source->terminal.captureState()) == encoded(restored->terminal.captureState()));
}

TEST_CASE("alternate screen active with primary preserved underneath", "[TerminalState]")
{
    auto source = makeTerminal();
    source->writeToScreen("primary text\033[?1049hon the alternate"sv);
    REQUIRE(source->terminal.isAlternateScreen());
    auto restored = restoredCopyOf(*source);

    CHECK(restored->terminal.isAlternateScreen());
    CHECK(restored->terminal.alternateScreen().renderMainPageText()
          == source->terminal.alternateScreen().renderMainPageText());
    requireSameAfter(*source, *restored, "\033[?1049l more"sv);
    CHECK_FALSE(restored->terminal.isAlternateScreen());
    CHECK(restored->terminal.primaryScreen().renderMainPageText().starts_with("primary text more"));
}

TEST_CASE("DECSLRM margins and tab stops survive", "[TerminalState]")
{
    auto source = makeTerminal();
    source->writeToScreen("\033[?69h\033[3;12s\033[2;5r\033[3g\033[1;6H\033H\033[1;11H\033H"sv);
    auto restored = restoredCopyOf(*source);
    CHECK(restored->terminal.captureState().margins.tabs == source->terminal.captureState().margins.tabs);
    requireSameAfter(*source, *restored, "\033[2;3Habc\tdef\tghi\r\nx\ty\ty\ty\033[5;1H\n\n\nzz"sv);
}

TEST_CASE("a tab stop set among the initial ones survives along with them", "[TerminalState]")
{
    auto source = makeTerminal();
    REQUIRE(source->terminal.captureState().margins.tabs.empty()); // none listed: the initial stops
    REQUIRE(restoredCopyOf(*source)->terminal.tabs().isInitial());

    source->writeToScreen("\033[1;4H\033H"sv);
    auto restored = restoredCopyOf(*source);
    CHECK(restored->terminal.tabs().within(TestPage.columns)
          == std::vector { ColumnOffset(0), ColumnOffset(3), ColumnOffset(8), ColumnOffset(16) });
    requireSameAfter(*source, *restored, "\033[1;1H\ta\tb\tc\td"sv);
}

TEST_CASE("title stack push/pop survives", "[TerminalState]")
{
    auto source = makeTerminal();
    source->writeToScreen("\033]2;first\033\\\033[22;2t\033]2;second\033\\"sv);
    auto restored = restoredCopyOf(*source);
    CHECK(restored->terminal.windowTitle() == "second");
    restored->writeToScreen("\033[23;2t"sv);
    CHECK(restored->terminal.windowTitle() == "first");
}

TEST_CASE("palette set by OSC 4 and its XTPUSHCOLORS stack survive", "[TerminalState]")
{
    auto source = makeTerminal();
    source->writeToScreen("\033]4;1;rgb:ff/00/00\033\\\033[#P\033]4;1;rgb:00/ff/00\033\\"sv);
    auto restored = restoredCopyOf(*source);
    CHECK(restored->terminal.colorPalette().palette.at(1) == RGBColor { 0x00, 0xff, 0x00 });
    restored->writeToScreen("\033[#Q"sv); // XTPOPCOLORS: back to the red one.
    CHECK(restored->terminal.colorPalette().palette.at(1) == RGBColor { 0xff, 0x00, 0x00 });
    source->writeToScreen("\033[#Q"sv);
    CHECK(encoded(source->terminal.captureState()) == encoded(restored->terminal.captureState()));
}

TEST_CASE("hyperlink LRU evicts the same entry on both sides", "[TerminalState]")
{
    auto source = makeTerminal();
    auto const capacity = source->terminal.captureState().hyperlinks.entries.size() + 1024;
    for (auto const index: std::views::iota(size_t { 0 }, capacity))
        source->writeToScreen(std::format("\033]8;;http://h/{}\033\\x\033]8;;\033\\", index));
    // Touch an old one so the recency order is not the creation order.
    source->writeToScreen("\033]8;id=a;http://h/touched\033\\y\033]8;;\033\\"sv);
    auto restored = restoredCopyOf(*source);
    CHECK(restored->terminal.captureState().hyperlinks == source->terminal.captureState().hyperlinks);

    requireSameAfter(
        *source,
        *restored,
        "\033]8;;http://h/new1\033\\n\033]8;;\033\\\033]8;;http://h/new2\033\\n\033]8;;\033\\"sv);
}

TEST_CASE("an unsent input-generator sequence is still sent", "[TerminalState]")
{
    auto source = makeTerminal();
    source->writeToScreen("\033[c"sv); // DA1: the reply waits in the input generator.
    auto const pending = source->terminal.peekInput();
    REQUIRE_FALSE(pending.empty());

    auto restored = restoredCopyOf(*source);
    CHECK(restored->terminal.peekInput() == pending);
    restored->terminal.flushInput();
    CHECK(restored->mockPty().stdinBuffer() == pending);
}

TEST_CASE("a state that does not fit the terminal is refused", "[TerminalState]")
{
    auto source = makeTerminal();
    source->writeToScreen("hello"sv);
    auto const good = source->terminal.captureState();

    auto const refused = [&](auto&& tamper) {
        auto bad = good;
        tamper(bad);
        auto target = makeTerminal();
        return target->terminal.restoreState(bad);
    };
    CHECK_FALSE(refused([](TerminalState& s) { s.pages.pop_back(); }).has_value());
    CHECK_FALSE(refused([](TerminalState& s) { s.paging.cursorPage = PageIndex { 99 }; }).has_value());
    CHECK_FALSE(refused([](TerminalState& s) { s.modes.dec.push_back(7777); }).has_value());
    CHECK_FALSE(refused([](TerminalState& s) { s.input.keyboard.currentStackTop = 99; }).has_value());
    CHECK_FALSE(
        refused([](TerminalState& s) { s.margins.tabs = { ColumnOffset(9), ColumnOffset(3) }; }).has_value());
    CHECK_FALSE(refused([](TerminalState& s) { s.session.pointerShapes.clear(); }).has_value());
}

TEST_CASE("a terminal with a gutter restores onto one with the same gutter", "[TerminalState]")
{
    // The indicator status line is as wide as the page and the gutter together, once it was laid out: what a
    // daemon's terminal comes to as soon as its session resizes. A live upgrade hands that state over.
    auto const withGutter = [] {
        auto settings = Mock::createSettings(TestPage, HistoryLimits::plain(TestHistory), 1024);
        settings.gutter.exitStatus = true;
        auto mock = std::make_unique<Mock>(std::move(settings));
        mock->terminal.setCellPixelSize(ImageSize { Width(10), Height(20) });
        return mock;
    };
    auto source = withGutter();
    source->terminal.resizeScreen(TestPage);
    source->writeToScreen("hello"sv);
    auto const state = source->terminal.captureState();
    REQUIRE(state.indicatorStatusLine.grid.pageSize.columns > TestPage.columns);

    auto target = withGutter();
    auto const result = target->terminal.restoreState(throughBytes(state));
    INFO(std::format("restore failed with error {}", result ? -1 : static_cast<int>(result.error())));
    CHECK(result.has_value());

    // Still not an indicator that is narrower than the page, nor taller than a status line.
    auto narrow = state;
    narrow.indicatorStatusLine.grid.pageSize.columns = TestPage.columns - ColumnCount(1);
    CHECK_FALSE(withGutter()->terminal.restoreState(narrow).has_value());

    // Nor one wider than the page beside the widest gutter any settings lay out: no terminal writes it. The
    // rows come from a terminal with a wider page, whose indicator is wide enough for either width.
    auto wider = [&] {
        auto const page = PageSize { TestPage.lines, TestPage.columns + ColumnCount(2 * MaxGutterColumns) };
        auto settings = Mock::createSettings(page, HistoryLimits::plain(TestHistory), 1024);
        settings.gutter.exitStatus = true;
        auto mock = std::make_unique<Mock>(std::move(settings));
        mock->terminal.setCellPixelSize(ImageSize { Width(10), Height(20) });
        mock->terminal.resizeScreen(page);
        return mock->terminal.captureState().indicatorStatusLine.grid;
    }();
    auto const indicatorOf = [&](ColumnCount columns) {
        auto grafted = state;
        grafted.indicatorStatusLine.grid = wider;
        grafted.indicatorStatusLine.grid.pageSize.columns = columns;
        return grafted;
    };
    CHECK(withGutter()
              ->terminal.restoreState(indicatorOf(TestPage.columns + ColumnCount(MaxGutterColumns)))
              .has_value());
    CHECK_FALSE(withGutter()
                    ->terminal.restoreState(indicatorOf(TestPage.columns + ColumnCount(MaxGutterColumns + 1)))
                    .has_value());
}

TEST_CASE("a truncated terminal stream is refused by the codec", "[TerminalState]")
{
    auto source = makeTerminal();
    auto bytes = encoded(source->terminal.captureState());
    bytes.resize(bytes.size() - 5);
    CHECK_FALSE(decodeTerminalState(bytes).has_value());
}

TEST_CASE("a terminal restored at every line boundary of the Workloads corpus keeps behaving identically",
          "[TerminalState][StateRoundTrip]")
{
    // Cut after a line feed: no escape sequence is in flight there (the parser's tail is not part of this
    // state).
    constexpr auto CorpusBytes = size_t { 12 } * 1024;
    constexpr auto Stride = size_t { 3 } * 1024;
    constexpr auto Page =
        PageSize { LineCount(test::WorkloadPageLines), ColumnCount(test::WorkloadPageColumns) };
    for (auto const& workload: test::Workloads)
    {
        DYNAMIC_SECTION(workload.name)
        {
            auto const corpus = test::makeWorkload(workload, CorpusBytes);
            auto const cutAfterLineFeed = [&](size_t target) {
                auto const lineFeed = corpus.find('\n', std::min(target, corpus.size()));
                return lineFeed == std::string::npos ? corpus.size() : lineFeed + 1;
            };
            auto source = std::make_unique<Mock>(Page, LineCount(100));
            auto restored = std::unique_ptr<Mock> {}; // Restored at the previous cut, one chunk behind.
            auto from = size_t { 0 };
            while (from < corpus.size())
            {
                auto const to = cutAfterLineFeed(from + Stride);
                source->writeToScreen(std::string_view { corpus }.substr(from, to - from));
                if (restored)
                {
                    // Restored before this chunk and fed it since: the same terminal as the source.
                    REQUIRE(source->terminal.primaryScreen().renderMainPageText()
                            == restored->terminal.primaryScreen().renderMainPageText());
                    REQUIRE(encoded(source->terminal.captureState())
                            == encoded(restored->terminal.captureState()));
                }
                restored = std::make_unique<Mock>(Page, LineCount(100));
                auto const captured = throughBytes(source->terminal.captureState());
                REQUIRE(restored->terminal.restoreState(captured).has_value());
                REQUIRE(encoded(restored->terminal.captureState()) == encoded(captured));
                auto const next = cutAfterLineFeed(to + Stride);
                restored->writeToScreen(std::string_view { corpus }.substr(to, next - to));
                from = to;
            }
        }
    }
}

TEST_CASE("a running command block keeps its elapsed time", "[TerminalState]")
{
    using namespace std::chrono_literals;
    auto source = makeTerminal();
    source->writeToScreen("\033]133;A\033\\$ \033]133;B\033\\ls\r\n\033]133;C\033\\"sv);
    source->steadyClock.advance(5s);

    // The new process's clock reads something else entirely: the start is rebased onto it.
    auto restored = makeTerminal();
    restored->steadyClock.advance(100s);
    REQUIRE(restored->terminal.restoreState(throughBytes(source->terminal.captureState())).has_value());
    CHECK(encoded(restored->terminal.captureState()) == encoded(source->terminal.captureState()));

    restored->steadyClock.advance(2s);
    restored->writeToScreen("\033]133;D;0\033\\"sv);
    auto const& records = restored->terminal.commandBlocks().records();
    REQUIRE_FALSE(records.empty());
    REQUIRE(records.back().duration.has_value());
    CHECK(*records.back().duration == 7s);
}

TEST_CASE("the block ids, the mode 2034 session token and the mark owner survive", "[TerminalState]")
{
    auto source = makeTerminal();
    source->writeToScreen("\033]133;A\033\\$ \033]133;B\033\\ls\033]133;C\033\\out\r\n\033]133;D;1\033\\"sv);
    source->writeToScreen("\033]133;A\033\\$ \033]133;B\033\\pwd\033]133;C\033\\x\r\n\033]133;D;0\033\\"sv);
    source->writeToScreen("\033[?2034h"sv);
    auto const before = source->terminal.captureState().shell;
    REQUIRE(before.commandBlocks.records.size() == 2);
    REQUIRE(before.semanticBlocks.token.has_value());

    auto restored = restoredCopyOf(*source);
    auto const after = restored->terminal.captureState().shell;
    CHECK(after.semanticBlocks.token == before.semanticBlocks.token);
    CHECK(after.markArbiter.owner == before.markArbiter.owner);
    CHECK(after.commandBlocks.records == before.commandBlocks.records);
    // The next prompt gets the id the source would have given it.
    requireSameAfter(*source,
                     *restored,
                     "\033]133;A\033\\$ \033]133;B\033\\date\033]133;C\033\\y\r\n\033]133;D;0\033\\"sv);
}

TEST_CASE("vi-mode selection survives", "[TerminalState]")
{
    auto source = makeTerminal();
    source->writeToScreen("hello world\r\nsecond line"sv);
    source->terminal.inputHandler().setMode(ViMode::Normal);
    source->terminal.moveNormalModeCursorTo(
        CellLocation { .line = LineOffset(0), .column = ColumnOffset(1) });
    source->sendCharSequence("vll");
    REQUIRE(source->terminal.inputHandler().mode() == ViMode::Visual);
    REQUIRE(source->terminal.selector() != nullptr);

    auto restored = restoredCopyOf(*source);
    REQUIRE(restored->terminal.selector() != nullptr);
    CHECK(restored->terminal.inputHandler().mode() == ViMode::Visual);
    CHECK(restored->terminal.selector()->from() == source->terminal.selector()->from());
    CHECK(restored->terminal.selector()->to() == source->terminal.selector()->to());
    CHECK(restored->terminal.selector()->state() == source->terminal.selector()->state());
    CHECK(restored->terminal.normalModeCursorPosition() == source->terminal.normalModeCursorPosition());

    // The selection goes on extending, and yanks, exactly as the source's does.
    source->sendCharSequence("ly");
    restored->sendCharSequence("ly");
    CHECK(encoded(source->terminal.captureState()) == encoded(restored->terminal.captureState()));
    CHECK(source->clipboardData == restored->clipboardData);
    CHECK_FALSE(restored->clipboardData.empty());
}

TEST_CASE("the search pattern, its options and a word-wise selection with custom delimiters survive",
          "[TerminalState]")
{
    auto source = makeTerminal();
    source->writeToScreen("foo-bar baz"sv);
    REQUIRE(source->terminal.setNewSearchTerm(U"Ba", SearchOrigin::Typed).has_value());
    auto const options = SearchOptions { .caseSensitivity = SearchCaseSensitivity::Sensitive,
                                         .syntax = SearchSyntax::Literal,
                                         .wordBoundary = SearchWordBoundary::Any };
    REQUIRE(source->terminal.setSearchOptions(options).has_value());

    auto restored = restoredCopyOf(*source);
    CHECK(restored->terminal.search().pattern == U"Ba");
    CHECK(restored->terminal.search().options == options);
    CHECK(restored->terminal.search().query != nullptr);
}

TEST_CASE("an open synchronized-output batch keeps the time it has been open", "[TerminalState]")
{
    using namespace std::chrono_literals;
    auto source = makeTerminal();
    source->writeToScreen("\033[?2026h"sv);
    REQUIRE(source->terminal.isRenderingSuppressed());
    source->steadyClock.advance(100ms);

    auto restored = makeTerminal();
    restored->steadyClock.advance(50s);
    REQUIRE(restored->terminal.restoreState(throughBytes(source->terminal.captureState())).has_value());
    CHECK(restored->terminal.isRenderingSuppressed());

    // The batch is 100ms old already: it times out that much sooner than a batch begun now would.
    auto const timeout = restored->terminal.settings().synchronizedOutputTimeout;
    REQUIRE(timeout > 100ms);
    restored->steadyClock.advance(timeout - 100ms + 1ms);
    CHECK_FALSE(restored->terminal.isRenderingSuppressed());
}

TEST_CASE("the session epoch row stamps count from survives", "[TerminalState]")
{
    using namespace std::chrono_literals;
    auto source = makeTerminal();
    source->wallClock.advance(1h);
    auto restored = makeTerminal();
    restored->wallClock.advance(5h);
    auto state = source->terminal.captureState();
    state.session.sessionEpoch += 7s;
    REQUIRE(restored->terminal.restoreState(throughBytes(state)).has_value());
    CHECK(restored->terminal.captureState().session.sessionEpoch == state.session.sessionEpoch);
}

TEST_CASE("an open hint-mode session survives -- the next filter key selects what it would have",
          "[TerminalState]")
{
    // More matches than one letter can label, so the labels are two letters wide and the first one typed
    // before the capture leaves a session open: the second, typed after it, picks among what the first left.
    constexpr auto MatchCount = 30;
    constexpr auto Page = PageSize { LineCount(MatchCount + 2), ColumnCount(20) };
    auto const make = [&] {
        auto mock = std::make_unique<Mock>(Page, TestHistory);
        mock->terminal.setCellPixelSize(ImageSize { Width(10), Height(20) });
        return mock;
    };
    auto source = make();
    for (auto const index: std::views::iota(0, MatchCount))
        source->writeToScreen(std::format("http://h/{}{}", index, index + 1 < MatchCount ? "\r\n" : ""));
    source->terminal.inputHandler().setMode(ViMode::Normal);
    source->terminal.activateHintMode(HintModeRequest { .patterns = {}, .action = HintAction::Copy });
    REQUIRE(source->terminal.hintMatches().size() == MatchCount);
    source->sendCharSequence("b");
    REQUIRE(source->terminal.isHintModeActive());
    REQUIRE(source->terminal.hintFilter() == "b");

    auto restored = make();
    REQUIRE(restored->terminal.restoreState(throughBytes(source->terminal.captureState())).has_value());
    CHECK(restored->terminal.isHintModeActive());
    CHECK(restored->terminal.inputHandler().mode() == ViMode::Hint);
    CHECK(restored->terminal.hintFilter() == "b");
    CHECK(restored->terminal.hintMatches().size() == source->terminal.hintMatches().size());
    CHECK(encoded(restored->terminal.captureState()) == encoded(source->terminal.captureState()));

    // "bc" is the 29th match: both copy it, close the session and go back to the mode it was opened from.
    source->sendCharSequence("c");
    restored->sendCharSequence("c");
    CHECK(source->clipboardData == "http://h/28");
    CHECK(restored->clipboardData == source->clipboardData);
    CHECK_FALSE(restored->terminal.isHintModeActive());
    CHECK(restored->terminal.inputHandler().mode() == ViMode::Normal);
    CHECK(encoded(restored->terminal.captureState()) == encoded(source->terminal.captureState()));
}

TEST_CASE("a restored hint session follows scrolling output as the source's does, by its scope",
          "[TerminalState]")
{
    auto const scope = GENERATE(HintScope::Visible, HintScope::Scrollback);
    INFO(std::format("scope {}", static_cast<int>(scope)));
    auto source = makeTerminal();
    source->writeToScreen("http://h/a\r\nhttp://h/b\r\nhttp://h/c"sv);
    source->terminal.activateHintMode(HintModeRequest {
        .patterns = {}, .action = HintAction::Copy, .scope = scope, .scrollbackLimit = TestHistory });
    REQUIRE(source->terminal.hintMatches().size() == 3);

    auto restored = restoredCopyOf(*source);
    CHECK(encoded(restored->terminal.captureState()) == encoded(source->terminal.captureState()));

    // Output scrolls the page: the scrollback session shifts its labels, the visible one re-scans.
    requireSameAfter(*source, *restored, "\r\nhttp://h/d\r\n\r\n\r\n"sv);
    CHECK(restored->terminal.hintMatches().size() == source->terminal.hintMatches().size());
    source->sendCharSequence("b");
    restored->sendCharSequence("b");
    CHECK(restored->clipboardData == source->clipboardData);
    CHECK(encoded(restored->terminal.captureState()) == encoded(source->terminal.captureState()));
}
