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

constexpr auto TakenAt = std::chrono::local_days { 2026y / 10 / 22 } + 14h + 3min;

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

TEST_CASE("restoredMarker spells the restore time", "[vthost][snapshot][seed]")
{
    CHECK(restoredMarker(TakenAt) == "\x1b[2m── restored · snapshot of 2026-10-22 14:03 ──\x1b[m\r\n");
}

TEST_CASE("seeded rows sit in the history above a blank page", "[vthost][snapshot][seed]")
{
    auto const history = capturedRows(30);
    REQUIRE(history.lines.size() == 30);
    auto fresh = freshTerminal();

    seedRestoredHistory(fresh.terminal, history, restoredMarker(TakenAt));

    auto const& grid = fresh.terminal.primaryScreen().grid();
    CHECK(grid.historyLineCount() >= LineCount(30));
    CHECK(grid.lineText(LineOffset(-30)).starts_with("r1 "));
    CHECK(grid.lineText(LineOffset(-1)).starts_with("r30"));
    CHECK(grid.lineText(LineOffset(0)).contains("restored · snapshot of 2026-10-22 14:03"));
    CHECK(fresh.terminal.primaryScreen().cursor().position
          == vtbackend::CellLocation { .line = LineOffset(1), .column = vtbackend::ColumnOffset(0) });
    CHECK(grid.lineTextTrimmed(LineOffset(5)).empty());
}

TEST_CASE("fewer rows than the page leave no blank rows above them", "[vthost][snapshot][seed]")
{
    auto const history = capturedRows(5);
    auto fresh = freshTerminal();

    seedRestoredHistory(fresh.terminal, history, restoredMarker(TakenAt));

    auto const& grid = fresh.terminal.primaryScreen().grid();
    CHECK(grid.historyLineCount() == LineCount(5));
    CHECK(grid.lineText(LineOffset(-5)).starts_with("r1 "));
    CHECK(grid.lineText(LineOffset(-1)).starts_with("r5 "));
}

TEST_CASE("exactly a page of rows lands in the history whole", "[vthost][snapshot][seed]")
{
    auto const history = capturedRows(23); // what a 24-line page above the cursor can hold
    auto fresh = freshTerminal();
    seedRestoredHistory(fresh.terminal, history, restoredMarker(TakenAt));
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
    seedRestoredHistory(fresh.terminal, history, restoredMarker(TakenAt));

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
    seedRestoredHistory(fresh.terminal, history, restoredMarker(TakenAt));

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
    seedRestoredHistory(fresh.terminal, history, restoredMarker(TakenAt));

    auto const& grid = fresh.terminal.primaryScreen().grid();
    CHECK(grid.historyLineCount() == LineCount(1));
    CHECK(grid.lineText(LineOffset(-1)) == std::string(80, 'x'));
}
