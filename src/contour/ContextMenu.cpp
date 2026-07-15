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

    using Predicate = bool (*)(ContextMenuState const&) noexcept;
    using ChildBuilder = std::vector<ContextMenuEntry> (*)(ContextMenuState const&);

    /// Fills in a row's action from the state, for the rows whose action carries something the menu only
    /// learns when it is opened.
    ///
    /// The row must CARRY what it will act on, not go looking for it again when it is picked: by then the
    /// pointer has travelled to the row, and every "…under the cursor" question answers about a different
    /// cell. Same reasoning as ContextMenuEntry holding an action rather than a name.
    using ActionBinder = actions::Action (*)(ContextMenuState const&);

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
        std::string_view title {};         ///< Empty on a Command row => commandTitle(action).
        Predicate visible = always;        ///< When false, the row is not in the menu at all.
        Predicate enabled = always;        ///< When false, the row is in the menu but grayed out.
        Predicate checked = nullptr;       ///< When set, the row is checkable; checked when it holds.
        ChildBuilder children = nullptr;   ///< Submenu rows: what is inside.
        ActionBinder bindAction = nullptr; ///< When set, supplies the action in place of @ref action.

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

        /// This row, drawn with a check column that is ticked exactly when @p predicate holds.
        [[nodiscard]] Row checkedWhen(Predicate predicate) const
        {
            auto row = *this;
            row.checked = predicate;
            return row;
        }

        /// This row, running the action @p binder builds from the state rather than a fixed one.
        [[nodiscard]] Row actionFrom(ActionBinder binder) const
        {
            auto row = *this;
            row.bindAction = binder;
            return row;
        }

        /// What this row runs, against @p state.
        [[nodiscard]] actions::Action actionFor(ContextMenuState const& state) const
        {
            return bindAction ? bindAction(state) : action;
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

    /// A row acting on the hyperlink that was RIGHT-CLICKED, carried with the row rather than looked up
    /// again when it is picked — by then the pointer has left the link and is sitting on the menu itself.
    template <typename HyperlinkAction>
    [[nodiscard]] Row hyperlinkCommand(std::string_view title)
    {
        return command(HyperlinkAction {}, title)
            .shownWhen(hasHyperlinkUnderCursor)
            .actionFrom([](ContextMenuState const& state) -> actions::Action {
                return HyperlinkAction { .uri = state.hyperlinkUnderCursor };
            });
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
            //
            // "Prompt", not "Command", and deliberately so: OSC 133 marks LINES, so what there is to copy
            // is the prompt line — prompt chrome, typed command and all ("user@host:~$ ls", or the whole
            // two-line banner of a powerlevel10k). The bare command exists only when the shell sends it
            // explicitly (OSC 133;C cmdline_url), which none of the bundled integrations do. A row named
            // "Copy Last Command" that hands back a prompt is a row that lies.
            command(CopyLastCommandPrompt {}, "Copy Last Prompt").shownWhen(hasLastCommand),
            command(CopyLastCommandOutput {}, "Copy Last Command Output").shownWhen(hasLastCommand),
            command(CopyLastCommandBlock {}, "Copy Last Prompt and Output").shownWhen(hasLastCommand),

            separator(),

            hyperlinkCommand<FollowHyperlink>("Open Link"),
            hyperlinkCommand<CopyHyperlink>("Copy Link Address"),
            command(OpenFileManager {}, "Open Current Folder").enabledWhen(hasLocalWorkingDirectory),

            separator(),

            command(SplitVertical {}, "Split Vertically"),
            command(SplitHorizontal {}, "Split Horizontally"),
            command(TogglePaneZoom {}, "Toggle Pane Zoom").shownWhen(hasSplits),
            command(ToggleSplitOrientation {}, "Toggle Split Orientation").shownWhen(hasSplits),
            command(ClosePane {}, "Close Pane"),
            command(CreateNewTab {}, "New Tab"),

            separator(),

            // Read-only == input protection (KAM). The check reflects the pane's state at open time;
            // picking it runs ToggleInputProtection, which flips it for the next open.
            command(ToggleInputProtection {}, "Read-Only Mode").checkedWhen(inputProtected),

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

                case ContextMenuEntryKind::Command: {
                    auto action = row.actionFor(state);
                    entries.emplace_back(ContextMenuEntry {
                        .kind = ContextMenuEntryKind::Command,
                        .title = row.title.empty() ? commandTitle(action) : std::string(row.title),
                        .action = std::move(action),
                        .enabled = row.enabled(state),
                        .checkable = row.checked != nullptr,
                        .checked = row.checked != nullptr && row.checked(state),
                        .children = {} });
                    break;
                }
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
