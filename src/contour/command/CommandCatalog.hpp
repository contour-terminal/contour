// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/command/Command.hpp>
#include <contour/config/Config.hpp>

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace contour::command
{

/// Merges the commands of every source in @p sources into one list.
///
/// De-duplicated by command id, first source wins. That ordering is what lets a specific source
/// override a generic one: a SwitchToTab bound in the config and the same tab's dynamically
/// synthesized entry collapse onto a single row rather than showing the user the command twice.
///
/// @param sources The sources to merge, in precedence order.
/// @return Every distinct command, in no particular order (the palette sorts it).
[[nodiscard]] std::vector<Command> collectCommands(std::span<CommandSource const* const> sources);

/// One row of the palette's category column: which group a catalogue action is listed under.
struct ActionCategory
{
    std::string_view name {};                         ///< The action's catalogue name, e.g. "SplitVertical".
    CommandCategory category = CommandCategory::Pane; ///< The group it is listed under.
};

/// The category column, one row per catalogue action, in catalogue order.
///
/// Kept beside the catalogue rather than in it: `config/` sits below `command/`, and the grouping is
/// the palette's vocabulary, not the key bindings'. A test pins row @c i to `actionCatalog()[i]`, so
/// an action added without a category fails the suite instead of vanishing from the palette.
/// @return The rows.
[[nodiscard]] std::span<ActionCategory const> actionCategories() noexcept;

/// The palette group @p action is listed under.
///
/// A direct index, as `actions::catalogEntry` is: the column is in the catalogue's own order.
/// @pre `actionCategories()` has a row for @p action (the coverage test guarantees it).
/// @param action The action to classify.
/// @return Its category.
[[nodiscard]] CommandCategory commandCategory(actions::Action const& action) noexcept;

/// Every action that is runnable without an argument, straight from the action catalog.
///
/// This is the palette's floor: it is what guarantees that an action nobody has bound to a key is
/// still reachable, which is the whole reason the palette exists. Actions that REQUIRE an argument
/// (ParameterizedActionConcept) are absent — they arrive through the sources below, which know what
/// argument to give them.
class ActionCommandSource final: public CommandSource
{
  public:
    [[nodiscard]] std::vector<Command> commands() const override;
};

/// Every action the user has bound to a key or a mouse button.
///
/// The point of this source is the ARGUMENTS: a binding carries a concrete instance, so it is what
/// makes `HintMode` (with its patterns), `SendChars` (with its characters) and `PasteClipboard`
/// (stripping) reachable from the palette at all — the catalog can only offer them empty.
///
/// Bindings that fire several actions at once contribute each of those actions individually: the
/// user can then run any one of them alone, which is strictly more than the chord offers.
class BoundCommandSource final: public CommandSource
{
  public:
    /// @param mappings The configured input mappings. Must outlive this source.
    explicit BoundCommandSource(config::InputMappings const& mappings) noexcept: _mappings { mappings } {}

    [[nodiscard]] std::vector<Command> commands() const override;

  private:
    config::InputMappings const& _mappings;
};

/// One ChangeProfile command per configured profile.
class ProfileCommandSource final: public CommandSource
{
  public:
    /// @param config The loaded configuration. Must outlive this source.
    explicit ProfileCommandSource(config::Config const& config) noexcept: _config { config } {}

    [[nodiscard]] std::vector<Command> commands() const override;

  private:
    config::Config const& _config;
};

/// One AttachToHost command per saved host.
class HostCommandSource final: public CommandSource
{
  public:
    /// @param config The loaded configuration. Must outlive this source.
    explicit HostCommandSource(config::Config const& config) noexcept: _config { config } {}

    [[nodiscard]] std::vector<Command> commands() const override;

  private:
    config::Config const& _config;
};

/// One LaunchLayout command per saved layout.
class LayoutCommandSource final: public CommandSource
{
  public:
    /// @param config The loaded configuration (its `layouts` already merged with layouts.yml).
    ///               Must outlive this source.
    explicit LayoutCommandSource(config::Config const& config) noexcept: _config { config } {}

    [[nodiscard]] std::vector<Command> commands() const override;

  private:
    config::Config const& _config;
};

/// The open tabs, as the command palette needs to see them.
///
/// An interface rather than a WindowController reference so TabCommandSource can be driven — and
/// tested — without a window, a Qt event loop, or a live session behind it.
class TabTitleProvider
{
  public:
    virtual ~TabTitleProvider() = default;

    /// The titles of the currently open tabs, in tab order.
    [[nodiscard]] virtual std::vector<std::string> tabTitles() const = 0;
};

/// One SwitchToTab command per open tab, titled with the tab's own title.
///
/// Re-queried each time the palette opens, so the rows track the tabs that exist NOW.
class TabCommandSource final: public CommandSource
{
  public:
    /// @param tabs The open tabs. Must outlive this source.
    explicit TabCommandSource(TabTitleProvider const& tabs) noexcept: _tabs { tabs } {}

    [[nodiscard]] std::vector<Command> commands() const override;

  private:
    TabTitleProvider const& _tabs;
};

} // namespace contour::command
