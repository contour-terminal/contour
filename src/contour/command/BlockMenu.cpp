// SPDX-License-Identifier: Apache-2.0
#include <contour/command/BlockMenu.hpp>
#include <contour/command/ContextMenu.hpp>
#include <contour/command/ContextMenuTable.hpp>

#include <vtbackend/shell/CommandBlockAddress.hpp>

#include <array>
#include <ranges>
#include <span>

namespace contour::command
{

namespace
{
    using Table = detail::MenuTable<BlockMenuState>;
    using Row = Table::Row;

    // {{{ Predicates
    bool hasOutput(BlockMenuState const& state) noexcept
    {
        return state.output == OutputPresence::Present;
    }

    bool hasCommandLine(BlockMenuState const& state) noexcept
    {
        return state.commandLine == CommandLinePresence::Present;
    }

    /// A command line to run, and a prompt to run it at: typed while another command runs, it would be
    /// that command's input.
    bool canRerun(BlockMenuState const& state) noexcept
    {
        return hasCommandLine(state) && state.livePrompt == PromptPresence::Present;
    }

    bool foldIsCollapsed(BlockMenuState const& state) noexcept
    {
        return state.fold == FoldPresence::Collapsed;
    }

    bool foldIsExpanded(BlockMenuState const& state) noexcept
    {
        return state.fold == FoldPresence::Expanded;
    }
    // }}}

    /// A row acting on the block the menu was opened for, pinned by its id: the pointer has left the block
    /// for the menu by the time a row is picked, and the block may have scrolled away.
    template <typename BlockAction>
    [[nodiscard]] actions::Action pinnedAction(BlockMenuState const& state)
    {
        auto action = BlockAction {};
        action.target = vtbackend::CommandBlockTarget::Pointer;
        action.block = state.id;
        return action;
    }

    template <typename BlockAction>
    [[nodiscard]] Row blockRow(std::string_view title)
    {
        return Table::command(BlockAction {}, title).actionFrom(pinnedAction<BlockAction>);
    }

    /// The menu's rows, as data. A function over a build-once static for the reason contextMenuTemplate()
    /// is one: an Action carries strings.
    [[nodiscard]] std::span<Row const> blockMenuTemplate()
    {
        using namespace actions;

        static auto const rows = std::array {
            blockRow<CopyCommandBlock>("Copy Output").shownWhen(hasOutput),
            blockRow<CopyCommandLine>("Copy Command").shownWhen(hasCommandLine),
            blockRow<RerunCommand>("Rerun…").shownWhen(canRerun),
            blockRow<OpenCommandOutput>("Open in Pager").shownWhen(hasOutput),
            blockRow<ToggleCommandFold>("Collapse Output").shownWhen(foldIsExpanded),
            blockRow<ToggleCommandFold>("Expand Output").shownWhen(foldIsCollapsed),
        };
        return rows;
    }

    /// The header lines as disabled rows, ahead of the separator that closes them.
    [[nodiscard]] std::vector<ContextMenuEntry> headerRows(BlockMenuState const& state)
    {
        auto entries = std::vector<ContextMenuEntry> {};
        for (auto const& line: state.header)
            entries.push_back(ContextMenuEntry { .kind = ContextMenuEntryKind::Header,
                                                 .title = line,
                                                 .availability = RowAvailability::Disabled });
        return entries;
    }
} // namespace

std::vector<ContextMenuEntry> buildBlockMenu(BlockMenuState const& state)
{
    auto entries = headerRows(state);
    auto rows = Table::buildRows(blockMenuTemplate(), state);
    if (!entries.empty() && !rows.empty())
        entries.push_back(ContextMenuEntry { .kind = ContextMenuEntryKind::Separator });
    std::ranges::move(rows, std::back_inserter(entries));
    return entries;
}

} // namespace contour::command
