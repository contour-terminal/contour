// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/command/Command.hpp>
#include <contour/command/ContextMenu.hpp>
#include <contour/config/Actions.hpp>

#include <vtbackend/core/Color.hpp>

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace contour::command::detail
{

/// A resolved, enabled command row: what a menu's dynamic rows (one per profile, host, tab, …) are made of.
/// @param title  The row's display text.
/// @param action What the row runs.
/// @param check  The row's check column, if any.
/// @param swatch The colour the row is drawn as, if it is a swatch.
/// @return The row.
[[nodiscard]] inline ContextMenuEntry commandEntry(std::string title,
                                                   actions::Action action,
                                                   RowCheck check = RowCheck::None,
                                                   std::optional<vtbackend::RGBColor> swatch = std::nullopt)
{
    return ContextMenuEntry { .kind = ContextMenuEntryKind::Command,
                              .title = std::move(title),
                              .action = std::move(action),
                              .availability = RowAvailability::Enabled,
                              .check = check,
                              .role = MenuItemRole::None,
                              .swatch = std::move(swatch),
                              .children = {} };
}

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

    /// Where a Submenu row's built children go: into a submenu of their own, or into the menu itself.
    enum class ChildPlacement : std::uint8_t
    {
        Nested = 0, ///< Under the row's title, as a submenu.
        Spliced,    ///< In place of the row: the menu's own rows, as a list that grows with the state.
    };

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
        std::string_view title {};              ///< Empty on a Command row => commandTitle(action).
        Predicate visible = always;             ///< When false, the row is not in the menu at all.
        Predicate enabled = always;             ///< When false, the row is in the menu but grayed out.
        Predicate checked = nullptr;            ///< When set, the row is checkable; checked when it holds.
        MenuItemRole role = MenuItemRole::None; ///< The role a native menu gives this row.
        ChildBuilder children = nullptr;        ///< Submenu rows: what is inside.
        ActionBinder bindAction = nullptr;      ///< When set, supplies the action in place of @ref action.
        ChildPlacement placement = ChildPlacement::Nested; ///< Submenu rows: nested, or spliced in place.
        ActionScope scope = ActionScope::Window;           ///< Command rows: what the row acts on.

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

        /// This row, carrying @p newRole for a native menu to place.
        [[nodiscard]] Row withRole(MenuItemRole newRole) const
        {
            auto row = *this;
            row.role = newRole;
            return row;
        }

        /// This row, running the action @p binder builds from the state rather than a fixed one.
        [[nodiscard]] Row actionFrom(ActionBinder binder) const
        {
            auto row = *this;
            row.bindAction = binder;
            return row;
        }

        /// This row, acting on @p newScope.
        [[nodiscard]] Row inScope(ActionScope newScope) const
        {
            auto row = *this;
            row.scope = newScope;
            return row;
        }

        /// Whether this row can be picked, against @p state.
        [[nodiscard]] RowAvailability availabilityFor(State const& state) const
        {
            return enabled(state) ? RowAvailability::Enabled : RowAvailability::Disabled;
        }

        /// What this row runs, against @p state.
        [[nodiscard]] actions::Action actionFor(State const& state) const
        {
            return bindAction ? bindAction(state) : action;
        }
    };

    /// A row that runs @p action. @p title defaults to the action's own display name.
    [[nodiscard]] static Row command(actions::Action const& action, std::string_view title = {})
    {
        return Row { .kind = ContextMenuEntryKind::Command,
                     .action = action,
                     .title = title,
                     .visible = always,
                     .enabled = always,
                     .checked = nullptr,
                     .role = MenuItemRole::None,
                     .children = nullptr,
                     .bindAction = nullptr,
                     .placement = ChildPlacement::Nested,
                     .scope = ActionScope::Window };
    }

    /// A divider between groups of rows.
    [[nodiscard]] static Row separator()
    {
        return Row { .kind = ContextMenuEntryKind::Separator,
                     .action = {},
                     .title = {},
                     .visible = always,
                     .enabled = always,
                     .checked = nullptr,
                     .role = MenuItemRole::None,
                     .children = nullptr,
                     .bindAction = nullptr,
                     .placement = ChildPlacement::Nested,
                     .scope = ActionScope::Window };
    }

    /// A row named @p title that opens onto whatever @p children builds.
    [[nodiscard]] static Row submenu(std::string_view title, ChildBuilder children)
    {
        return Row { .kind = ContextMenuEntryKind::Submenu,
                     .action = {},
                     .title = title,
                     .visible = always,
                     .enabled = always,
                     .checked = nullptr,
                     .role = MenuItemRole::None,
                     .children = children,
                     .bindAction = nullptr,
                     .placement = ChildPlacement::Nested,
                     .scope = ActionScope::Window };
    }

    /// A run of rows that @p children builds, spliced into the menu in place of this row.
    ///
    /// For a list that grows with the state (the Window menu's tabs), where a submenu would hide what
    /// the menu exists to show. Honours `shownWhen`; `enabledWhen` has no row to gray and is ignored.
    [[nodiscard]] static Row rows(ChildBuilder children)
    {
        return Row { .kind = ContextMenuEntryKind::Submenu,
                     .action = {},
                     .title = {},
                     .visible = always,
                     .enabled = always,
                     .checked = nullptr,
                     .role = MenuItemRole::None,
                     .children = children,
                     .bindAction = nullptr,
                     .placement = ChildPlacement::Spliced,
                     .scope = ActionScope::Window };
    }

    /// One AttachToHost row per saved host, in `State::hostNames` order.
    ///
    /// Shared by every menu that offers the hosts, so the pane's and the title bar's "Attach to Host"
    /// cannot drift apart.
    /// @param state The world the menu was opened over; reads its `hostNames`.
    /// @return The rows, none without saved hosts.
    [[nodiscard]] static std::vector<ContextMenuEntry> hostRows(State const& state)
    {
        return std::ranges::to<std::vector>(state.hostNames | std::views::transform([](auto const& name) {
                                                return commandEntry(name,
                                                                    actions::AttachToHost { .name = name });
                                            }));
    }

    /// The "Attach to Host" submenu, listing hostRows(); absent without saved hosts, as an empty submenu
    /// is dropped.
    [[nodiscard]] static Row attachToHostSubmenu() { return submenu("Attach to Host", hostRows); }

    /// One ChangeProfile row per configured profile, with the active one check-marked.
    ///
    /// Shared by the pane menu and the menu bar's Shell menu, so the two cannot drift apart.
    /// @param state The world the menu was opened over; reads `profileNames` and `activeProfile`.
    /// @return The rows, none without profiles.
    [[nodiscard]] static std::vector<ContextMenuEntry> changeProfileRows(State const& state)
    {
        return std::ranges::to<std::vector>(
            state.profileNames | std::views::transform([&](auto const& name) {
                return commandEntry(name,
                                    actions::ChangeProfile { .name = name },
                                    name == state.activeProfile ? RowCheck::Checked : RowCheck::Unchecked);
            }));
    }

    /// The "Change Profile" submenu, listing changeProfileRows(); absent without profiles.
    [[nodiscard]] static Row changeProfileSubmenu() { return submenu("Change Profile", changeProfileRows); }

    /// One CreateNewTab row per configured profile, titled with the profile's name.
    ///
    /// Shared by the title bar's and the menu bar's "New Tab with Profile". Deliberately NOT
    /// changeProfileRows(): that one switches the CURRENT pane's profile and check-marks the active one.
    /// Here each row opens a NEW tab under its profile, so nothing is "current" and nothing is checked -- a
    /// tick would claim a state this menu does not describe.
    /// @param state The world the menu was opened over; reads its `profileNames`.
    /// @return The rows, none without profiles.
    [[nodiscard]] static std::vector<ContextMenuEntry> newTabProfileRows(State const& state)
    {
        return std::ranges::to<std::vector>(state.profileNames | std::views::transform([](auto const& name) {
                                                return commandEntry(
                                                    name, actions::CreateNewTab { .profileName = name });
                                            }));
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
                                                            .availability = row.availabilityFor(state),
                                                            .check = RowCheck::None,
                                                            .role = MenuItemRole::None,
                                                            .swatch = std::nullopt,
                                                            .children = {} });
                    break;

                case ContextMenuEntryKind::Submenu: {
                    auto children = row.children ? row.children(state) : std::vector<ContextMenuEntry> {};
                    // An empty submenu is noise: it opens onto nothing.
                    if (children.empty())
                        break;
                    if (row.placement == ChildPlacement::Spliced)
                    {
                        std::ranges::move(children, std::back_inserter(entries));
                        break;
                    }
                    entries.emplace_back(ContextMenuEntry { .kind = ContextMenuEntryKind::Submenu,
                                                            .title = std::string(row.title),
                                                            .action = {},
                                                            .availability = row.availabilityFor(state),
                                                            .check = RowCheck::None,
                                                            .role = MenuItemRole::None,
                                                            .swatch = std::nullopt,
                                                            .children = std::move(children) });
                    break;
                }

                case ContextMenuEntryKind::Command: {
                    auto action = row.actionFor(state);
                    entries.emplace_back(ContextMenuEntry {
                        .kind = ContextMenuEntryKind::Command,
                        .title = row.title.empty() ? commandTitle(action) : std::string(row.title),
                        .action = std::move(action),
                        .availability = row.availabilityFor(state),
                        .check = row.checked == nullptr ? RowCheck::None
                                 : row.checked(state)   ? RowCheck::Checked
                                                        : RowCheck::Unchecked,
                        .role = row.role,
                        .swatch = std::nullopt,
                        .children = {},
                        .scope = row.scope });
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

} // namespace contour::command::detail
