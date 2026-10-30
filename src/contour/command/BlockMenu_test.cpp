// SPDX-License-Identifier: Apache-2.0
#include <contour/command/BlockMenu.hpp>
#include <contour/command/ContextMenu.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <ranges>
#include <string>
#include <variant>
#include <vector>

using namespace contour;
using namespace contour::command;

namespace
{

/// A block with everything its rows could want: output, a command line, a prompt to rerun at, an open fold.
[[nodiscard]] BlockMenuState fullState()
{
    return BlockMenuState { .id = vtbackend::CommandBlockId(5),
                            .header = { "ctest -R contour_gui_test",
                                        "Failed · exit 8 · 12.4 s",
                                        "~/repo/contour",
                                        "Started 14:02:11" },
                            .output = OutputPresence::Present,
                            .commandLine = CommandLinePresence::Present,
                            .livePrompt = PromptPresence::Present,
                            .fold = FoldPresence::Expanded };
}

/// The rows below the header and its separator.
[[nodiscard]] std::vector<ContextMenuEntry> rowsOf(std::vector<ContextMenuEntry> const& menu)
{
    auto const separator = std::ranges::find(menu, ContextMenuEntryKind::Separator, &ContextMenuEntry::kind);
    return { separator == menu.end() ? menu.begin() : std::next(separator), menu.end() };
}

[[nodiscard]] ContextMenuEntry const* rowTitled(std::vector<ContextMenuEntry> const& rows,
                                                std::string const& title)
{
    auto const found = std::ranges::find(rows, title, &ContextMenuEntry::title);
    return found == rows.end() ? nullptr : &*found;
}

} // namespace

TEST_CASE("blockMenu.headerThenFiveRows", "[blockmenu]")
{
    auto const state = fullState();
    auto const menu = buildBlockMenu(state);

    REQUIRE(menu.size() == state.header.size() + 1 + BlockMenuRowCount);
    for (auto const index: std::views::iota(std::size_t { 0 }, state.header.size()))
    {
        CHECK(menu[index].kind == ContextMenuEntryKind::Header);
        CHECK(menu[index].title == state.header[index]);
        CHECK(menu[index].availability == RowAvailability::Disabled);
    }
    CHECK(menu[state.header.size()].kind == ContextMenuEntryKind::Separator);

    auto const rows = rowsOf(menu);
    REQUIRE(rows.size() == BlockMenuRowCount);
    auto const titles = std::ranges::to<std::vector>(rows | std::views::transform(&ContextMenuEntry::title));
    CHECK(titles
          == std::vector<std::string> {
              "Copy Output", "Copy Command", "Rerun…", "Open in Pager", "Collapse Output" });
}

TEST_CASE("blockMenu.noHeaderLinesNoSeparator", "[blockmenu]")
{
    auto state = fullState();
    state.header.clear();
    auto const menu = buildBlockMenu(state);
    REQUIRE(menu.size() == BlockMenuRowCount);
    CHECK(menu.front().kind == ContextMenuEntryKind::Command);
}

TEST_CASE("blockMenu.rowsArePinned", "[blockmenu]")
{
    // The pointer has left the block for the menu when a row is picked: every row names its block.
    auto const state = fullState();
    for (auto const& row: rowsOf(buildBlockMenu(state)))
    {
        INFO(row.title);
        REQUIRE(row.kind == ContextMenuEntryKind::Command);
        std::visit(
            [&](auto const& action) {
                if constexpr (requires { action.block; })
                {
                    CHECK(action.block == state.id);
                    CHECK(action.target == vtbackend::CommandBlockTarget::Pointer);
                }
                else
                    FAIL("a row that does not address a block");
            },
            row.action);
    }
}

TEST_CASE("blockMenu.availability", "[blockmenu]")
{
    auto const enabled = [](BlockMenuState const& state, std::string const& title) {
        auto const rows = rowsOf(buildBlockMenu(state));
        auto const* row = rowTitled(rows, title);
        REQUIRE(row != nullptr);
        return row->availability == RowAvailability::Enabled;
    };

    SECTION("everything there: every row can be picked")
    {
        for (auto const* title:
             { "Copy Output", "Copy Command", "Rerun…", "Open in Pager", "Collapse Output" })
        {
            INFO(title);
            CHECK(enabled(fullState(), title));
        }
    }

    SECTION("no output: nothing to copy, open or fold")
    {
        auto state = fullState();
        state.output = OutputPresence::Absent;
        state.fold = FoldPresence::None;
        CHECK_FALSE(enabled(state, "Copy Output"));
        CHECK_FALSE(enabled(state, "Open in Pager"));
        CHECK_FALSE(enabled(state, "Collapse Output"));
        CHECK(enabled(state, "Copy Command"));
    }

    SECTION("no command line: nothing to copy or rerun")
    {
        auto state = fullState();
        state.commandLine = CommandLinePresence::Absent;
        CHECK_FALSE(enabled(state, "Copy Command"));
        CHECK_FALSE(enabled(state, "Rerun…"));
        CHECK(enabled(state, "Copy Output"));
    }

    SECTION("no live prompt: typed while a command runs, a rerun would be its input")
    {
        auto state = fullState();
        state.livePrompt = PromptPresence::Absent;
        CHECK_FALSE(enabled(state, "Rerun…"));
        CHECK(enabled(state, "Copy Command"));
    }

    SECTION("a collapsed fold offers to expand")
    {
        auto state = fullState();
        state.fold = FoldPresence::Collapsed;
        auto const rows = rowsOf(buildBlockMenu(state));
        CHECK(rowTitled(rows, "Collapse Output") == nullptr);
        REQUIRE(rows.size() == BlockMenuRowCount);
        CHECK(enabled(state, "Expand Output"));
    }
}
