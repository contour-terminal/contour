// SPDX-License-Identifier: Apache-2.0
#include <contour/Command.h>
#include <contour/ContextMenu.h>

#include <array>
#include <span>
#include <string_view>
#include <utility>

namespace contour
{

namespace
{
    // {{{ Predicates -- the only place a menu row's precondition is spelled out.
    bool always(ContextMenuState const& /*state*/) noexcept
    {
        return true;
    }

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
        return state.hasHyperlinkUnderCursor;
    }

    bool hasWorkingDirectory(ContextMenuState const& state) noexcept
    {
        return state.hasWorkingDirectory;
    }

    bool hasSplits(ContextMenuState const& state) noexcept
    {
        return state.hasSplits;
    }

    bool hasProfiles(ContextMenuState const& state) noexcept
    {
        return !state.profileNames.empty();
    }
    // }}}

    using Predicate = bool (*)(ContextMenuState const&) noexcept;
    using ChildBuilder = std::vector<ContextMenuEntry> (*)(ContextMenuState const&);

    /// One row of the menu TEMPLATE — the menu described as data.
    ///
    /// Adding an entry to the context menu is adding one of these to the table below, and nothing else:
    /// the title comes from the action's catalog name unless it is overridden here, and the id the GUI
    /// needs comes from the action itself. There is no second title table and no second command list to
    /// keep in step.
    struct Row
    {
        ContextMenuEntryKind kind = ContextMenuEntryKind::Command;
        actions::Action action {};
        std::string_view title {};       ///< Empty on a Command row => commandTitle(action).
        Predicate visible = always;      ///< When false, the row is not in the menu at all.
        Predicate enabled = always;      ///< When false, the row is in the menu but grayed out.
        ChildBuilder children = nullptr; ///< Submenu rows: what is inside.

        /// This row, shown only when @p predicate holds.
        [[nodiscard]] Row shownWhen(Predicate predicate) const
        {
            auto row = *this;
            row.visible = predicate;
            return row;
        }

        /// This row, pickable only when @p predicate holds.
        [[nodiscard]] Row enabledWhen(Predicate predicate) const
        {
            auto row = *this;
            row.enabled = predicate;
            return row;
        }
    };

    /// A row that runs @p action. @p title defaults to the action's own display name.
    [[nodiscard]] Row command(actions::Action action, std::string_view title = {})
    {
        return Row { .kind = ContextMenuEntryKind::Command,
                     .action = std::move(action),
                     .title = title,
                     .visible = always,
                     .enabled = always,
                     .children = nullptr };
    }

    [[nodiscard]] Row separator()
    {
        return Row { .kind = ContextMenuEntryKind::Separator,
                     .action = {},
                     .title = {},
                     .visible = always,
                     .enabled = always,
                     .children = nullptr };
    }

    [[nodiscard]] Row submenu(std::string_view title, ChildBuilder children)
    {
        return Row { .kind = ContextMenuEntryKind::Submenu,
                     .action = {},
                     .title = title,
                     .visible = always,
                     .enabled = always,
                     .children = children };
    }

    [[nodiscard]] std::vector<ContextMenuEntry> buildRows(std::span<Row const> rows,
                                                          ContextMenuState const& state);

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
            command(actions::SoftReset {}, "Soft Reset Terminal"),
            command(actions::ClearHistoryAndReset {}, "Hard Reset Terminal"),
        };
        return rows;
    }

    [[nodiscard]] std::vector<ContextMenuEntry> advancedRows(ContextMenuState const& state)
    {
        return buildRows(advancedTemplate(), state);
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
            command(CopySelection {}, "Copy").enabledWhen(hasSelection),
            command(PasteClipboard {}, "Paste").enabledWhen(clipboardHasText),
            command(SelectAll {}),

            separator(),

            // Hidden rather than grayed out when the shell emits no OSC 133 marks: a permanently dead
            // "Copy Last Command Output" teaches the user the feature is broken, when in truth their
            // shell simply never told the terminal where one command ended and the next began.
            command(CopyLastCommandPrompt {}, "Copy Last Command").shownWhen(hasLastCommand),
            command(CopyLastCommandOutput {}, "Copy Last Command Output").shownWhen(hasLastCommand),
            command(CopyLastCommandBlock {}, "Copy Last Command and Output").shownWhen(hasLastCommand),

            separator(),

            command(FollowHyperlink {}, "Open Link").shownWhen(hasHyperlinkUnderCursor),
            command(CopyHyperlink {}, "Copy Link Address").shownWhen(hasHyperlinkUnderCursor),
            command(OpenFileManager {}, "Open Current Folder").enabledWhen(hasWorkingDirectory),

            separator(),

            command(SplitVertical {}, "Split Vertically"),
            command(SplitHorizontal {}, "Split Horizontally"),
            command(TogglePaneZoom {}, "Toggle Pane Zoom").shownWhen(hasSplits),
            command(ToggleSplitOrientation {}, "Toggle Split Orientation").shownWhen(hasSplits),
            command(ClosePane {}, "Close Pane"),
            command(CreateNewTab {}, "New Tab"),

            separator(),

            submenu("Change Profile", profileRows).shownWhen(hasProfiles),
            submenu("Advanced", advancedRows),
        };

        return rows;
    }

    std::vector<ContextMenuEntry> buildRows(std::span<Row const> rows, ContextMenuState const& state)
    {
        auto entries = std::vector<ContextMenuEntry> {};
        entries.reserve(rows.size());

        for (auto const& row: rows)
        {
            if (!row.visible(state))
                continue;

            switch (row.kind)
            {
                case ContextMenuEntryKind::Separator:
                    entries.emplace_back(ContextMenuEntry { .kind = ContextMenuEntryKind::Separator,
                                                            .title = {},
                                                            .action = {},
                                                            .enabled = true,
                                                            .checkable = false,
                                                            .checked = false,
                                                            .children = {} });
                    break;

                case ContextMenuEntryKind::Submenu: {
                    auto children = row.children ? row.children(state) : std::vector<ContextMenuEntry> {};
                    // An empty submenu is noise: it opens onto nothing.
                    if (children.empty())
                        break;
                    entries.emplace_back(ContextMenuEntry { .kind = ContextMenuEntryKind::Submenu,
                                                            .title = std::string(row.title),
                                                            .action = {},
                                                            .enabled = true,
                                                            .checkable = false,
                                                            .checked = false,
                                                            .children = std::move(children) });
                    break;
                }

                case ContextMenuEntryKind::Command:
                    entries.emplace_back(ContextMenuEntry {
                        .kind = ContextMenuEntryKind::Command,
                        .title = row.title.empty() ? commandTitle(row.action) : std::string(row.title),
                        .action = row.action,
                        .enabled = row.enabled(state),
                        .checkable = false,
                        .checked = false,
                        .children = {} });
                    break;
            }
        }

        return entries;
    }

    /// Drops the separators that hiding rows left stranded: leading, trailing, and doubled-up.
    void dropRedundantSeparators(std::vector<ContextMenuEntry>& entries)
    {
        auto kept = std::vector<ContextMenuEntry> {};
        kept.reserve(entries.size());

        for (auto& entry: entries)
        {
            auto const isSeparator = entry.kind == ContextMenuEntryKind::Separator;
            auto const followsSeparator =
                !kept.empty() && kept.back().kind == ContextMenuEntryKind::Separator;

            if (isSeparator && (kept.empty() || followsSeparator))
                continue;

            kept.emplace_back(std::move(entry));
        }

        if (!kept.empty() && kept.back().kind == ContextMenuEntryKind::Separator)
            kept.pop_back();

        entries = std::move(kept);
    }

} // namespace

std::vector<ContextMenuEntry> buildContextMenu(ContextMenuState const& state)
{
    auto entries = buildRows(contextMenuTemplate(), state);
    dropRedundantSeparators(entries);
    return entries;
}

} // namespace contour
