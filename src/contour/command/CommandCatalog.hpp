// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/command/Command.hpp>
#include <contour/config/Config.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
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

/// Whether a window has a daemon behind it to switch between.
enum class DaemonSwitching : uint8_t
{
    Unavailable = 0, ///< Its sessions run in-process: every switching action is hidden and a no-op.
    Available,       ///< A daemon hosts its sessions.
};

/// A local daemon, as the daemon rows list it.
struct LocalDaemonRow
{
    std::string label;                     ///< Its socket label.
    std::optional<std::size_t> workspaces; ///< How many workspaces it hosts, when known.
};

/// A saved host, as the daemon rows list it.
struct HostRow
{
    std::string name; ///< Its name under `hosts:`.
    std::string hint; ///< Where it is (the row's footer).
};

/// A workspace of the window's daemon, as the daemon rows list it.
/// Whether a workspace row is the one the asking window shows.
enum class WorkspaceRowRole : uint8_t
{
    Other = 0, ///< Another window shows it, or none does.
    Current,   ///< The window shows it: no row switches to it.
};

struct WorkspaceRow
{
    uint32_t position = 0; ///< 1-based position among the daemon's workspaces.
    std::string name;      ///< Label, else the active tab's title, else "workspace N".
    WorkspaceRowRole role = WorkspaceRowRole::Other; ///< Whether the window shows it.
};

/// What the daemon rows are made of, for one window at one moment.
struct DaemonRowsState
{
    DaemonSwitching switching = DaemonSwitching::Unavailable; ///< Whether there is a daemon to switch.
    std::optional<actions::DaemonRef> current; ///< The daemon the window is on; none when unknown.
    std::vector<LocalDaemonRow> localDaemons;  ///< The running local daemons, from the cached discovery.
    std::vector<HostRow> hosts;                ///< The saved hosts.
    std::vector<WorkspaceRow> workspaces;      ///< The workspaces of the window's daemon.
};

/// Where the daemon rows' state comes from. An interface so DaemonCommandSource is tested without a window
/// or a daemon; the window implements it. Never blocks: it answers from caches.
class DaemonRowsProvider
{
  public:
    DaemonRowsProvider() = default;
    DaemonRowsProvider(DaemonRowsProvider const&) = delete;
    DaemonRowsProvider(DaemonRowsProvider&&) = delete;
    DaemonRowsProvider& operator=(DaemonRowsProvider const&) = delete;
    DaemonRowsProvider& operator=(DaemonRowsProvider&&) = delete;
    virtual ~DaemonRowsProvider() = default;

    /// @return Whether the window has a daemon behind it. Cheap: no caches are refreshed.
    [[nodiscard]] virtual DaemonSwitching switchingAvailability() const = 0;

    /// @return The state now. The palette asks each time it opens, which is also when the caches behind
    ///         it start refreshing.
    [[nodiscard]] virtual DaemonRowsState daemonRows() const = 0;
};

/// The daemon and workspace switching rows: switch to or open every other daemon (local, then saved
/// host) and every other workspace of this window's daemon, step to the next or previous daemon or
/// workspace, show the switcher, and start a new daemon.
///
/// Empty for a window with no daemon behind it.
class DaemonCommandSource final: public CommandSource
{
  public:
    /// @param rows Where the state comes from. Must outlive this source.
    explicit DaemonCommandSource(DaemonRowsProvider const& rows) noexcept: _rows { rows } {}

    [[nodiscard]] std::vector<Command> commands() const override;

  private:
    DaemonRowsProvider const& _rows;
};

/// Passes on another source's commands, minus the daemon and workspace switching actions when the window
/// has no daemon behind it. Wraps the sources that offer actions without knowing the window: the bound
/// ones and the catalog's floor.
class SwitchingGatedSource final: public CommandSource
{
  public:
    /// @param inner The source to pass on. Must outlive this one.
    /// @param rows Tells whether the window has a daemon. Must outlive this source.
    SwitchingGatedSource(CommandSource const& inner, DaemonRowsProvider const& rows) noexcept:
        _inner { inner }, _rows { rows }
    {
    }

    [[nodiscard]] std::vector<Command> commands() const override;

  private:
    CommandSource const& _inner;
    DaemonRowsProvider const& _rows;
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
