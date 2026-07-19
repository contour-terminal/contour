// SPDX-License-Identifier: Apache-2.0
#include <contour/Command.h>
#include <contour/ContextMenu.h>
#include <contour/ContextMenuTable.h>

#include <array>
#include <span>
#include <string_view>

namespace contour
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

    bool hasHyperlinkUnderCursor(ContextMenuState const& state) noexcept
    {
        return !state.hyperlinkUnderCursor.empty();
    }

    bool hasLocalWorkingDirectory(ContextMenuState const& state) noexcept
    {
        return state.hasLocalWorkingDirectory;
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
    // }}}

    /// A row acting on the hyperlink that was RIGHT-CLICKED, carried with the row rather than looked up
    /// again when it is picked — by then the pointer has left the link and is sitting on the menu itself.
    template <typename HyperlinkAction>
    [[nodiscard]] Row hyperlinkCommand(std::string_view title)
    {
        return Table::command(HyperlinkAction {}, title)
            .shownWhen(hasHyperlinkUnderCursor)
            .actionFrom([](ContextMenuState const& state) -> actions::Action {
                return HyperlinkAction { .uri = state.hyperlinkUnderCursor };
            });
    }

    /// One ChangeProfile row per configured profile, with the active one check-marked.
    [[nodiscard]] std::vector<ContextMenuEntry> profileRows(ContextMenuState const& state)
    {
        auto entries = std::vector<ContextMenuEntry> {};
        entries.reserve(state.profileNames.size());

        for (auto const& name: state.profileNames)
            entries.emplace_back(ContextMenuEntry { .kind = ContextMenuEntryKind::Command,
                                                    .title = name,
                                                    .action = actions::ChangeProfile { .name = name },
                                                    .enabled = true,
                                                    .checkable = true,
                                                    .checked = name == state.activeProfile,
                                                    .children = {} });

        return entries;
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
            Table::command(CopySelection {}, "Copy").enabledWhen(hasSelection),
            Table::command(PasteClipboard {}, "Paste").enabledWhen(clipboardHasText),
            Table::command(SelectAll {}),

            Table::separator(),

            // Hidden rather than grayed out when the shell emits no OSC 133 marks: a permanently dead
            // "Copy Last Command Output" teaches the user the feature is broken, when in truth their
            // shell simply never told the terminal where one command ended and the next began.
            //
            // "Prompt", not "Command", and deliberately so: OSC 133 marks LINES, so what there is to copy
            // is the prompt line — prompt chrome, typed command and all ("user@host:~$ ls", or the whole
            // two-line banner of a powerlevel10k). The bare command exists only when the shell sends it
            // explicitly (OSC 133;C cmdline_url), which none of the bundled integrations do. A row named
            // "Copy Last Command" that hands back a prompt is a row that lies.
            Table::command(CopyLastCommandPrompt {}, "Copy Last Prompt").shownWhen(hasLastCommand),
            Table::command(CopyLastCommandOutput {}, "Copy Last Command Output").shownWhen(hasLastCommand),
            Table::command(CopyLastCommandBlock {}, "Copy Last Prompt and Output").shownWhen(hasLastCommand),

            Table::separator(),

            hyperlinkCommand<FollowHyperlink>("Open Link"),
            hyperlinkCommand<CopyHyperlink>("Copy Link Address"),
            Table::command(OpenFileManager {}, "Open Current Folder").enabledWhen(hasLocalWorkingDirectory),

            Table::separator(),

            Table::command(SplitVertical {}, "Split Vertically"),
            Table::command(SplitHorizontal {}, "Split Horizontally"),
            Table::command(TogglePaneZoom {}, "Toggle Pane Zoom").shownWhen(hasSplits),
            Table::command(ToggleSplitOrientation {}, "Toggle Split Orientation").shownWhen(hasSplits),
            Table::command(ClosePane {}, "Close Pane"),
            Table::command(CreateNewTab {}, "New Tab"),

            Table::separator(),

            // Read-only == input protection (KAM). The check reflects the pane's state at open time;
            // picking it runs ToggleInputProtection, which flips it for the next open.
            Table::command(ToggleInputProtection {}, "Read-Only Mode").checkedWhen(inputProtected),

            Table::separator(),

            Table::submenu("Change Profile", profileRows).shownWhen(hasProfiles),
            Table::submenu("Advanced", advancedRows),
        };

        return rows;
    }

} // namespace

std::vector<ContextMenuEntry> buildContextMenu(ContextMenuState const& state)
{
    auto entries = Table::buildRows(contextMenuTemplate(), state);
    detail::dropRedundantSeparators(entries);
    return entries;
}

} // namespace contour
