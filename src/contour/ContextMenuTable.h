// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/Actions.h>
#include <contour/Command.h>
#include <contour/ContextMenu.h>

#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace contour::detail
{

/// The machinery a context menu is described with, parameterized on the state that menu reads.
///
/// Each menu SURFACE answers to a different world: the terminal pane's menu asks about selections,
/// clipboards and hyperlinks; the title bar's asks about tab counts and tab bar modes. Sharing one
/// state struct between them would turn each into a bag of fields belonging to the other, and would
/// force every test of one surface to populate the other's. So the state is the template parameter
/// and everything below is written once.
///
/// A consumer names it once and then builds its table out of the factories:
/// @code
/// using Table = detail::MenuTable<MyState>;
/// static auto const rows = std::array {
///     Table::command(actions::Foo {}, "Foo").enabledWhen(hasFoo),
///     Table::separator(),
///     Table::submenu("Bar", barRows),
/// };
/// @endcode
template <typename State>
struct MenuTable
{
    /// A row's precondition: whether it is shown, pickable, or check-marked.
    using Predicate = bool (*)(State const&) noexcept;

    /// Builds a submenu's contents from the state.
    using ChildBuilder = std::vector<ContextMenuEntry> (*)(State const&);

    /// Fills in a row's action from the state, for the rows whose action carries something the menu
    /// only learns when it is opened.
    ///
    /// The row must CARRY what it will act on, not go looking for it again when it is picked: by then
    /// the pointer has travelled to the row, and every "…under the cursor" question answers about a
    /// different cell. Same reasoning as ContextMenuEntry holding an action rather than a name.
    using ActionBinder = actions::Action (*)(State const&);

    /// The precondition of a row that has none.
    [[nodiscard]] static bool always(State const& /*state*/) noexcept { return true; }

    /// One row of a menu TEMPLATE — the menu described as data.
    ///
    /// Adding an entry to a context menu is adding one of these to that menu's table, and nothing
    /// else: the title comes from the action's catalog name unless it is overridden here, and the id
    /// the GUI needs comes from the action itself. There is no second title table and no second
    /// command list to keep in step.
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
        [[nodiscard]] actions::Action actionFor(State const& state) const
        {
            return bindAction ? bindAction(state) : action;
        }
    };

    /// A row that runs @p action. @p title defaults to the action's own display name.
    [[nodiscard]] static Row command(actions::Action action, std::string_view title = {})
    {
        return Row { .kind = ContextMenuEntryKind::Command,
                     .action = std::move(action),
                     .title = title,
                     .visible = always,
                     .enabled = always,
                     .children = nullptr };
    }

    /// A divider between groups of rows.
    [[nodiscard]] static Row separator()
    {
        return Row { .kind = ContextMenuEntryKind::Separator,
                     .action = {},
                     .title = {},
                     .visible = always,
                     .enabled = always,
                     .children = nullptr };
    }

    /// A row named @p title that opens onto whatever @p children builds.
    [[nodiscard]] static Row submenu(std::string_view title, ChildBuilder children)
    {
        return Row { .kind = ContextMenuEntryKind::Submenu,
                     .action = {},
                     .title = title,
                     .visible = always,
                     .enabled = always,
                     .children = children };
    }

    /// Resolves @p rows against @p state: drops the rows whose precondition fails, grays out the ones
    /// merely unsatisfied, and expands the submenus.
    ///
    /// @param rows  The menu template.
    /// @param state The world the menu was opened over.
    /// @return The resolved rows, in table order.
    [[nodiscard]] static std::vector<ContextMenuEntry> buildRows(std::span<Row const> rows,
                                                                 State const& state)
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
};

/// Drops the separators that hiding rows left stranded: leading, trailing, and doubled-up.
///
/// State-agnostic, so it is a plain function rather than a member of MenuTable: by the time it runs,
/// every row has already been resolved against the state and only the shape is left to tidy.
///
/// @param entries The resolved rows, tidied in place.
inline void dropRedundantSeparators(std::vector<ContextMenuEntry>& entries)
{
    auto kept = std::vector<ContextMenuEntry> {};
    kept.reserve(entries.size());

    for (auto& entry: entries)
    {
        auto const isSeparator = entry.kind == ContextMenuEntryKind::Separator;
        auto const followsSeparator = !kept.empty() && kept.back().kind == ContextMenuEntryKind::Separator;

        if (isSeparator && (kept.empty() || followsSeparator))
            continue;

        kept.emplace_back(std::move(entry));
    }

    if (!kept.empty() && kept.back().kind == ContextMenuEntryKind::Separator)
        kept.pop_back();

    entries = std::move(kept);
}

} // namespace contour::detail
