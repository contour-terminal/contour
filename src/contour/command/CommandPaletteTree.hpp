// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/command/Command.hpp>
#include <contour/command/Shortcut.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace contour::command
{

/// One group of the palette's top level: the category it collects and the header it shows.
struct PaletteGroupRow
{
    CommandCategory category = CommandCategory::Pane; ///< The commands this group collects.
    std::string_view title {};                        ///< Its header, in sentence case.
};

/// The palette's groups, in display order.
inline constexpr auto PaletteGroups = std::array {
    PaletteGroupRow { .category = CommandCategory::Pane, .title = "Pane" },
    PaletteGroupRow { .category = CommandCategory::Tab, .title = "Tab" },
    PaletteGroupRow { .category = CommandCategory::Window, .title = "Window" },
    PaletteGroupRow { .category = CommandCategory::Layouts, .title = "Layouts" },
    PaletteGroupRow { .category = CommandCategory::Blocks, .title = "Blocks" },
    PaletteGroupRow { .category = CommandCategory::Profiles, .title = "Profiles" },
    PaletteGroupRow { .category = CommandCategory::Hosts, .title = "Hosts" },
    PaletteGroupRow { .category = CommandCategory::Edit, .title = "Edit" },
    PaletteGroupRow { .category = CommandCategory::View, .title = "View" },
    PaletteGroupRow { .category = CommandCategory::Shell, .title = "Shell" },
    PaletteGroupRow { .category = CommandCategory::Contour, .title = "Contour" },
};
static_assert(PaletteGroups.size() == CommandCategoryCount,
              "every CommandCategory needs one PaletteGroups row");

/// The header of the group of recently used commands, above every other group.
inline constexpr std::string_view RecentGroupTitle = "Recently used";

/// Joins a parent's title and a child's argument when the child is shown outside its level.
inline constexpr std::string_view DrillInSeparator = " · ";

/// What a palette row does when picked.
enum class PaletteEntryKind : std::uint8_t
{
    Command = 0, ///< Runs a command.
    DrillIn,     ///< Opens a parameterised action's level, listing its children.
};

/// Where in the palette the user is.
enum class PaletteDepth : std::uint8_t
{
    Top = 0, ///< The grouped top level.
    Level,   ///< Inside one parameterised action's level.
};

/// One parameterised action collapsed into a single drill-in row.
struct PaletteParent
{
    std::string name {};                              ///< The action's catalogue name, e.g. "ChangeProfile".
    std::string title {};                             ///< Its display title, e.g. "Change Profile".
    std::string description {};                       ///< The catalogue's description of the action.
    CommandCategory category = CommandCategory::Pane; ///< The group the row is listed under.
    std::vector<std::size_t> children {}; ///< Indices into PaletteTree::commands, ordered by argument.
};

/// Every command the palette offers, split into plain commands and drill-in parents.
struct PaletteTree
{
    std::vector<Command> commands {};  ///< Every command, children included, as collected.
    std::vector<std::size_t> plain {}; ///< Indices into @c commands of the commands listed directly.
    std::vector<PaletteParent>
        parents {}; ///< One per parameterised action that has a child, in catalogue order.

    /// The command whose id is @p id, children included.
    /// @param id The command id.
    /// @return The command, or null when there is none.
    [[nodiscard]] Command const* find(std::string_view id) const noexcept;
};

/// Splits @p commands into plain commands and one parent per parameterised action.
///
/// A command whose action `actions::isParameterized` becomes a child of its action's parent, even when
/// it is the only one; every other command is plain. Parents come in catalogue order of their action.
/// @param commands Every command the sources offer (see collectCommands()).
/// @return The tree.
[[nodiscard]] PaletteTree buildPaletteTree(std::vector<Command> commands);

/// One row of the palette's listing.
struct PaletteEntry
{
    PaletteEntryKind kind = PaletteEntryKind::Command; ///< What the row does when picked.
    std::size_t index = 0; ///< Into PaletteTree::commands (Command) or PaletteTree::parents (DrillIn).
    std::string title {};  ///< What the row reads; a child outside its level is "Parent · argument".
    std::vector<int> titleMatches {}; ///< UTF-8 byte offsets into @c title the filter matched, ascending.
    std::string_view groupTitle {};   ///< The group's header on its first row; empty on every other row.

    [[nodiscard]] bool operator==(PaletteEntry const&) const = default;
};

/// What the palette is asked to list.
struct PaletteQuery
{
    std::string_view filter {}; ///< What the user typed; empty for the browsing order.
    std::optional<std::size_t>
        level {}; ///< The parent drilled into (into PaletteTree::parents); none at the top.
    std::span<std::string const> recent {}; ///< Recently run command ids, newest first.
};

/// Lists @p tree for @p query.
///
/// At the top with no filter: the recently used commands that still exist (a child qualified by its
/// parent's title), then each group of PaletteGroups that has a row, its rows by title. At the top
/// with a filter: plain commands (title, id as fallback), parents (title, then name) and children
/// (their argument only, shown qualified) that match, grouped by category; the groups ordered by
/// their best match, the rows by score, recency and title. Inside a level: the parent's children,
/// titled by argument and without headers; unfiltered the recently used first, else by score;
/// then recency and argument.
/// @param tree  The tree to list.
/// @param query What to list.
/// @return The rows, in display order.
[[nodiscard]] std::vector<PaletteEntry> listPalette(PaletteTree const& tree, PaletteQuery const& query);

/// The verbs the footer's key hints name for one kind of row at one depth.
struct PaletteHintRow
{
    PaletteEntryKind kind = PaletteEntryKind::Command; ///< The selected row's kind.
    PaletteDepth depth = PaletteDepth::Top;            ///< Where the palette is.
    std::string_view enterVerb {};                     ///< What Enter does.
    std::string_view escapeVerb {};                    ///< What Escape does.
};

/// The footer's key hints, one row per (kind, depth) that can occur; levels do not nest.
inline constexpr auto PaletteHintRows = std::array {
    PaletteHintRow { .kind = PaletteEntryKind::Command,
                     .depth = PaletteDepth::Top,
                     .enterVerb = "Run",
                     .escapeVerb = "Close" },
    PaletteHintRow { .kind = PaletteEntryKind::DrillIn,
                     .depth = PaletteDepth::Top,
                     .enterVerb = "Open",
                     .escapeVerb = "Close" },
    PaletteHintRow { .kind = PaletteEntryKind::Command,
                     .depth = PaletteDepth::Level,
                     .enterVerb = "Run",
                     .escapeVerb = "Back" },
};

/// The footer's key hints for a row of @p kind at @p depth, e.g. "↩ Run · ⎋ Close".
/// @param kind  The selected row's kind.
/// @param depth Where the palette is.
/// @param style How the keys are spelled.
/// @return "<Enter> <verb> · <Escape> <verb>", or empty for a pair no PaletteHintRows row names.
[[nodiscard]] std::string paletteFooterHints(PaletteEntryKind kind, PaletteDepth depth, ShortcutStyle style);

} // namespace contour::command
