// SPDX-License-Identifier: Apache-2.0
#include <contour/command/Command.hpp>
#include <contour/command/ContextMenu.hpp>
#include <contour/command/ContextMenuTable.hpp>

#include <vtbackend/shell/CommandBlockAddress.hpp>

#include <algorithm>
#include <array>
#include <iterator>
#include <span>
#include <string_view>

namespace contour::command
{

namespace
{
    using Table = detail::MenuTable<ContextMenuState>;
    using Row = Table::Row;

    // {{{ Predicates -- the only place a menu row's precondition is spelled out.
    bool hasSelection(ContextMenuState const& state) noexcept
    {
        return state.hasSelection;
    }

    bool clipboardHasText(ContextMenuState const& state) noexcept
    {
        return state.clipboardHasText;
    }

    bool hasLastCommand(ContextMenuState const& state) noexcept
    {
        return state.hasLastCommand;
    }

    bool hasLastCommandLine(ContextMenuState const& state) noexcept
    {
        return state.hasLastCommand && state.lastCommandLine == CommandLinePresence::Present;
    }

    bool hasHyperlinkUnderCursor(ContextMenuState const& state) noexcept
    {
        return !state.hyperlinkUnderCursor.empty() || state.linkUnderCursor.has_value();
    }

    bool hasLocalWorkingDirectory(ContextMenuState const& state) noexcept
    {
        return state.hasLocalWorkingDirectory;
    }

    bool offersSidebar(ContextMenuState const& state) noexcept
    {
        return state.sidebarRecovery == SidebarRecovery::Offered;
    }

    bool hasSplits(ContextMenuState const& state) noexcept
    {
        return state.hasSplits;
    }

    bool hasProfiles(ContextMenuState const& state) noexcept
    {
        return !state.profileNames.empty();
    }

    bool inputProtected(ContextMenuState const& state) noexcept
    {
        return state.inputProtected;
    }

    bool mouseForwarded(ContextMenuState const& state) noexcept
    {
        return state.mouseForwarding == vtbackend::MouseForwarding::Forwarded;
    }

    bool canSpeakSelection(ContextMenuState const& state) noexcept
    {
        return state.canSpeak && state.hasSelection;
    }

    bool hasFoldUnderCursor(ContextMenuState const& state) noexcept
    {
        return state.foldLine.has_value();
    }
    // }}}

    /// A row acting on the hyperlink that was RIGHT-CLICKED, carried with the row rather than looked up
    /// again when it is picked — by then the pointer has left the link and is sitting on the menu itself.
    template <typename HyperlinkAction>
    [[nodiscard]] Row hyperlinkCommand(std::string_view title)
    {
        return Table::command(HyperlinkAction {}, title)
            .shownWhen(hasHyperlinkUnderCursor)
            .actionFrom([](ContextMenuState const& state) -> actions::Action {
                return HyperlinkAction { .uri = state.hyperlinkUnderCursor, .link = state.linkUnderCursor };
            });
    }

    /// The "Advanced" submenu — itself a table, so it goes through the very same row builder.
    [[nodiscard]] std::span<Row const> advancedTemplate()
    {
        static auto const rows = std::array {
            Table::command(actions::SoftReset {}, "Soft Reset Terminal"),
            Table::command(actions::ClearHistoryAndReset {}, "Hard Reset Terminal"),
        };
        return rows;
    }

    [[nodiscard]] std::vector<ContextMenuEntry> advancedRows(ContextMenuState const& state)
    {
        return Table::buildRows(advancedTemplate(), state);
    }

    /// The context menu, as data.
    ///
    /// A function over a build-once static rather than a constexpr table, for the same reason
    /// actions::actionCatalog() is one: ChangeProfile and friends carry a std::string, and a string
    /// cannot survive constant evaluation into a constexpr object.
    [[nodiscard]] std::span<Row const> contextMenuTemplate()
    {
        using namespace actions;

        static auto const rows = std::array {
            Table::command(CopySelection {}, "Copy").shownWhen(hasSelection),
            Table::command(PasteClipboard {}, "Paste").shownWhen(clipboardHasText),
            Table::command(SelectAll {}),
            // Hidden rather than greyed when there is no speech engine: a permanently dead row teaches
            // the user the feature is broken, when in truth this build or this machine simply has no
            // voice to speak with.
            Table::command(SpeakSelection {}, "Read Aloud").shownWhen(canSpeakSelection),

            Table::separator(),

            // Hidden rather than grayed out when the shell emits no OSC 133 marks: a permanently dead
            // "Copy Last Command Output" teaches the user the feature is broken, when in truth their
            // shell simply never told the terminal where one command ended and the next began.
            //
            // "Prompt", not "Command", and deliberately so: OSC 133 marks LINES, so what there is to copy
            // is the prompt line — prompt chrome, typed command and all ("user@host:~$ ls", or the whole
            // two-line banner of a powerlevel10k). The bare command exists only when the shell sends it
            // (OSC 133;C cmdline_url, which the bundled bash, zsh, fish and PowerShell integrations do)
            // or the terminal reads it back from the grid -- and these rows copy lines, not records. A
            // row named "Copy Last Command" that hands back a prompt is a row that lies.
            //
            // Hence its own row for the bare command, from the block's record rather than its lines, and
            // only where that record holds one: a shell that reports none would copy nothing.
            Table::command(CopyCommandLine {}, "Copy Last Command").shownWhen(hasLastCommandLine),
            Table::command(CopyLastCommandPrompt {}, "Copy Last Prompt").shownWhen(hasLastCommand),
            Table::command(CopyLastCommandOutput {}, "Copy Last Command Output").shownWhen(hasLastCommand),
            Table::command(CopyLastCommandBlock {}, "Copy Last Prompt and Output").shownWhen(hasLastCommand),

            // Carries the clicked line rather than reading the viewport when picked: by then the pointer
            // has left that row for the menu, and ToggleFold's own anchor is the top of the viewport.
            Table::command(ToggleFoldAt {}, "Toggle Output Fold")
                .shownWhen(hasFoldUnderCursor)
                .actionFrom([](ContextMenuState const& state) -> actions::Action {
                    return ToggleFoldAt { .line = state.foldLine ? state.foldLine->value : 0 };
                }),

            Table::separator(),

            hyperlinkCommand<FollowHyperlink>("Open Link"),
            hyperlinkCommand<CopyHyperlink>("Copy Link Address"),
            Table::command(OpenFileManager {}, "Open Current Folder").shownWhen(hasLocalWorkingDirectory),

            Table::separator(),

            Table::command(SplitVertical {}, "Split Vertically"),
            Table::command(SplitHorizontal {}, "Split Horizontally"),
            Table::command(TogglePaneZoom {}, "Toggle Pane Zoom").shownWhen(hasSplits),
            Table::command(ToggleSplitOrientation {}, "Toggle Split Orientation").shownWhen(hasSplits),
            Table::command(ClosePane {}, "Close Pane"),
            Table::command(CreateNewTab {}, "New Tab"),
            // The way back to a sidebar hidden while no band holds its toggle: the pane is all that is left
            // under the pointer, so whatever hid it can be undone from here (a band, when there is one,
            // holds the toggle instead).
            Table::command(ToggleSidebar {}, "Show Sidebar").shownWhen(offersSidebar),

            Table::separator(),

            // Read-only == the user's read-only lock (not the host's KAM). The check reflects the pane's
            // state at open time; picking it runs ToggleInputProtection, which flips it for the next open.
            Table::command(ToggleInputProtection {}, "Read-Only Mode").checkedWhen(inputProtected),
            // Checked while the pane's mouse events reach an application that tracks the mouse.
            Table::command(ToggleMouseForwarding {}, "Mouse Forwarding").checkedWhen(mouseForwarded),

            Table::separator(),

            Table::changeProfileSubmenu().shownWhen(hasProfiles),
            Table::attachToHostSubmenu(),
            Table::submenu("Advanced", advancedRows),
        };

        return rows;
    }

} // namespace

std::vector<ContextMenuEntry> buildContextMenu(ContextMenuState const& state)
{
    // The block the click landed on comes first: its facts and what the menu can do for it, then the pane's.
    auto entries = std::vector<ContextMenuEntry> {};
    if (state.blockUnderCursor)
    {
        auto blockEntries = buildBlockMenu(*state.blockUnderCursor);
        if (state.blockMenuPlacement == MenuChildPlacement::Nested && !blockEntries.empty())
            entries.push_back(ContextMenuEntry { .kind = ContextMenuEntryKind::Submenu,
                                                 .title = "Command Block",
                                                 .children = std::move(blockEntries) });
        else
            entries = std::move(blockEntries);
        if (!entries.empty())
            entries.push_back(ContextMenuEntry { .kind = ContextMenuEntryKind::Separator });
    }
    std::ranges::move(Table::buildRows(contextMenuTemplate(), state), std::back_inserter(entries));
    detail::dropRedundantSeparators(entries);
    return entries;
}

} // namespace contour::command
