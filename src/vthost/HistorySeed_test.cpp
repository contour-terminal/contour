// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/shell/CommandBlock.hpp>
#include <vtbackend/testing/MockTerm.hpp>
#include <vtbackend/testing/TestHelpers.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <format>
#include <ranges>
#include <string>
#include <vector>

#include <vthost/HistorySeed.hpp>
#include <vthost/WorkspaceCapture.hpp>

using namespace std::chrono_literals;
using namespace vthost;
using vtbackend::ColumnCount;
using vtbackend::LineCount;
using vtbackend::LineOffset;
using vtbackend::PageSize;

namespace
{

constexpr auto TakenAt = std::chrono::local_days { 2026y / 10 / 22 } + 14h + 3min + 31s;
constexpr auto DividerOn =
    RestoreDivider { .sessionEndedAt = TakenAt, .today = std::chrono::local_days { 2026y / 10 / 22 } };
constexpr auto DividerNextDay =
    RestoreDivider { .sessionEndedAt = TakenAt, .today = std::chrono::local_days { 2026y / 10 / 23 } };

/// A terminal that has printed @p rows numbered rows and has its cursor under them: what a pane captured.
[[nodiscard]] proto::Delta capturedRows(int rows, int columns = 80)
{
    auto source = vtbackend::MockTerm<> { PageSize { LineCount(24), ColumnCount(columns) }, LineCount(200) };
    for (auto const number: std::views::iota(1, rows + 1))
        source.terminal.writeToScreen(std::format("r{}\r\n", number));
    return capturePaneHistory(source.terminal, 1000);
}

[[nodiscard]] vtbackend::MockTerm<> freshTerminal(int columns = 80)
{
    return vtbackend::MockTerm<> { PageSize { LineCount(24), ColumnCount(columns) }, LineCount(200) };
}

} // namespace

TEST_CASE("the divider's words name the day only when it is not today", "[vthost][snapshot][seed]")
{
    CHECK(restoredDividerText(DividerOn) == "Restored · session ended 14:03");
    CHECK(restoredDividerText(DividerNextDay) == "Restored · session ended 2026-10-22 14:03");
}

TEST_CASE("the divider is centred between rules and never wider than the pane", "[vthost][snapshot][seed]")
{
    auto const text = restoredDividerText(DividerOn);
    auto const textWidth = static_cast<int>(std::ranges::count_if(
        text, [](char byte) { return (static_cast<unsigned char>(byte) & 0b1100'0000U) != 0b1000'0000U; }));
    REQUIRE(textWidth == 30);

    SECTION("a wide pane")
    {
        constexpr auto Columns = 40;
        auto const row = restoredDivider(DividerOn, ColumnCount(Columns));
        auto const left = (Columns - textWidth - 2) / 2;
        auto const right = Columns - textWidth - 2 - left;
        auto repeated = [](int count) {
            auto out = std::string {};
            for (auto const _: std::views::iota(0, count))
                out += "─";
            return out;
        };
        CHECK(row == std::format("{} {} {}", repeated(left), text, repeated(right)));
        CHECK(row.starts_with("──── Restored"));
    }
    SECTION("a pane as wide as the text plus its two spaces")
    {
        CHECK(restoredDivider(DividerOn, ColumnCount(textWidth + 2)) == std::format(" {} ", text));
    }
    SECTION("a pane narrower than that holds the text alone")
    {
        CHECK(restoredDivider(DividerOn, ColumnCount(20)) == text);
        CHECK_FALSE(restoredDivider(DividerOn, ColumnCount(20)).contains("─"));
    }
}

TEST_CASE("the seeded divider is one dim row that a resize cuts instead of wrapping",
          "[vthost][snapshot][seed]")
{
    auto const history = capturedRows(3, 40);
    auto fresh = freshTerminal(40);
    seedRestoredHistory(fresh.terminal, history, DividerOn);

    auto& screen = fresh.terminal.primaryScreen();
    CHECK(screen.grid().lineText(LineOffset(0)).starts_with("──── Restored · session ended 14:03"));
    CHECK(screen.grid().lineAt(LineOffset(0)).storage().sgr[0].flags.contains(vtbackend::CellFlag::Faint));
    CHECK_FALSE(screen.grid().lineAt(LineOffset(0)).wrappable());

    for (auto const columns: { 30, 60 })
    {
        fresh.terminal.resizeScreen(PageSize { LineCount(24), ColumnCount(columns) });
        fresh.terminal.writeToScreen("$ ");
        auto const& grid = fresh.terminal.primaryScreen().grid();
        CAPTURE(columns);
        auto const rows =
            std::views::iota(-3, 8)
            | std::views::filter([&](int row) { return grid.lineText(LineOffset(row)).contains("Restored"); })
            | std::ranges::to<std::vector>();
        REQUIRE(rows.size() == 1);
        auto const dividerRow = LineOffset(rows.front());
        CHECK(grid.lineText(dividerRow).starts_with("──── Restored"));
        CHECK(grid.lineText(dividerRow + 1).starts_with("$ "));
    }
}

TEST_CASE("the divider's dim rendition does not reach what is written after the seed",
          "[vthost][snapshot][seed]")
{
    auto fresh = freshTerminal(40);
    seedRestoredHistory(fresh.terminal, capturedRows(3, 40), DividerOn);
    fresh.terminal.writeToScreen("$ ");

    auto const& screen = fresh.terminal.primaryScreen();
    REQUIRE(screen.grid().lineText(LineOffset(1)).starts_with("$ "));
    CHECK_FALSE(
        screen.at(LineOffset(1), vtbackend::ColumnOffset(0)).isFlagEnabled(vtbackend::CellFlag::Faint));
    CHECK_FALSE(
        screen.at(LineOffset(1), vtbackend::ColumnOffset(1)).isFlagEnabled(vtbackend::CellFlag::Faint));
}

TEST_CASE("on a one-line page the divider that scrolls into the history is the row left unwrappable",
          "[vthost][snapshot][seed]")
{
    auto fresh = vtbackend::MockTerm<> { PageSize { LineCount(1), ColumnCount(40) }, LineCount(200) };
    seedRestoredHistory(fresh.terminal, proto::Delta {}, DividerOn);

    auto const& grid = fresh.terminal.primaryScreen().grid();
    CHECK(grid.lineText(LineOffset(-1)).starts_with("──── Restored"));
    CHECK_FALSE(grid.lineAt(LineOffset(-1)).wrappable());
    CHECK(grid.lineAt(LineOffset(0)).wrappable());
}

TEST_CASE("seeded rows sit in the history above a blank page", "[vthost][snapshot][seed]")
{
    auto const history = capturedRows(30);
    REQUIRE(history.lines.size() == 30);
    auto fresh = freshTerminal();

    seedRestoredHistory(fresh.terminal, history, DividerOn);

    auto const& grid = fresh.terminal.primaryScreen().grid();
    CHECK(grid.historyLineCount() >= LineCount(30));
    CHECK(grid.lineText(LineOffset(-30)).starts_with("r1 "));
    CHECK(grid.lineText(LineOffset(-1)).starts_with("r30"));
    CHECK(grid.lineText(LineOffset(0)).contains("Restored · session ended 14:03"));
    CHECK(fresh.terminal.primaryScreen().cursor().position
          == vtbackend::CellLocation { .line = LineOffset(1), .column = vtbackend::ColumnOffset(0) });
    CHECK(grid.lineTextTrimmed(LineOffset(5)).empty());
}

TEST_CASE("fewer rows than the page leave no blank rows above them", "[vthost][snapshot][seed]")
{
    auto const history = capturedRows(5);
    auto fresh = freshTerminal();

    seedRestoredHistory(fresh.terminal, history, DividerOn);

    auto const& grid = fresh.terminal.primaryScreen().grid();
    CHECK(grid.historyLineCount() == LineCount(5));
    CHECK(grid.lineText(LineOffset(-5)).starts_with("r1 "));
    CHECK(grid.lineText(LineOffset(-1)).starts_with("r5 "));
}

TEST_CASE("exactly a page of rows lands in the history whole", "[vthost][snapshot][seed]")
{
    auto const history = capturedRows(23); // what a 24-line page above the cursor can hold
    auto fresh = freshTerminal();
    seedRestoredHistory(fresh.terminal, history, DividerOn);
    auto const& grid = fresh.terminal.primaryScreen().grid();
    CHECK(grid.historyLineCount() == LineCount(static_cast<int>(history.lines.size())));
    CHECK(grid.lineText(LineOffset(-1)).starts_with("r23"));
}

TEST_CASE("seeded command blocks are found and new ones get higher ids", "[vthost][snapshot][seed]")
{
    namespace test = vtbackend::test;
    auto source = freshTerminal();
    source.terminal.writeToScreen(test::commandRun("make", { "out" }, 2));
    source.terminal.writeToScreen(test::commandRun("ls", { "a", "b" }, 0));
    source.terminal.writeToScreen(std::string { test::PromptStart });
    auto const history = capturePaneHistory(source.terminal, 100);
    REQUIRE(history.commandBlocks.size() == 2);
    auto const first = history.commandBlocks[0].id;
    auto const second = history.commandBlocks[1].id;

    auto fresh = freshTerminal();
    seedRestoredHistory(fresh.terminal, history, DividerOn);

    auto const& store = fresh.terminal.commandBlocks();
    REQUIRE(store.find(vtbackend::CommandBlockId { first }) != nullptr);
    CHECK(store.find(vtbackend::CommandBlockId { first })->exitCode == 2);
    CHECK(store.find(vtbackend::CommandBlockId { second })->exitCode == 0);

    fresh.terminal.writeToScreen("\033]133;A\033\\");
    CHECK(store.currentId().value > second);
}

TEST_CASE("a seeded hyperlink keeps its URI", "[vthost][snapshot][seed]")
{
    auto source = freshTerminal();
    source.terminal.writeToScreen("\033]8;;https://contour-terminal.org\033\\link\033]8;;\033\\\r\n");
    auto const history = capturePaneHistory(source.terminal, 100);
    REQUIRE(history.hyperlinks.size() == 1);

    auto fresh = freshTerminal();
    seedRestoredHistory(fresh.terminal, history, DividerOn);

    auto const& grid = fresh.terminal.primaryScreen().grid();
    auto const id = grid.lineAt(LineOffset(-1)).storage().hyperlinks[0];
    REQUIRE(id != vtbackend::HyperlinkId {});
    auto const info = fresh.terminal.hyperlinks().hyperlinkById(id);
    REQUIRE(info != nullptr);
    CHECK(info->uri == "https://contour-terminal.org");
}

TEST_CASE("rows wider than the terminal are cut, not wrapped", "[vthost][snapshot][seed]")
{
    auto source = vtbackend::MockTerm<> { PageSize { LineCount(24), ColumnCount(120) }, LineCount(50) };
    source.terminal.writeToScreen(std::string(110, 'x') + "\r\n");
    auto const history = capturePaneHistory(source.terminal, 100);
    REQUIRE(history.lines.size() == 1);

    auto fresh = freshTerminal(80);
    seedRestoredHistory(fresh.terminal, history, DividerOn);

    auto const& grid = fresh.terminal.primaryScreen().grid();
    CHECK(grid.historyLineCount() == LineCount(1));
    CHECK(grid.lineText(LineOffset(-1)) == std::string(80, 'x'));
}
