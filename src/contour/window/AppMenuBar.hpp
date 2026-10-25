// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/command/MenuBarTable.hpp>
#include <contour/command/MenuTree.hpp>
#include <contour/command/Shortcut.hpp>
#include <contour/config/Actions.hpp>
#include <contour/platform/MenuBarIntegration.hpp>

#include <QtCore/QList>
#include <QtGui/QAction>
#include <QtCore/QKeyCombination>

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <span>

class QMenu;
class QMenuBar;

namespace contour::window
{

/// The Qt menu role of a row of @p role in @p menu.
///
/// A row without a role of its own is application-specific in the Application menu, so that macOS moves it
/// into the application menu beside About and Quit, and has no role anywhere else, so that Qt's text
/// heuristics never move an ordinary row there.
/// @param role The row's role.
/// @param menu The menu bar menu the row is in.
/// @return The Qt role.
[[nodiscard]] QAction::MenuRole qtMenuRoleFor(command::MenuItemRole role, command::MenuBarMenu menu) noexcept;

/// The Qt key combination of @p chord, spelled from the chord itself, never from its display text.
///
/// Control is Qt::ControlModifier and Super Qt::MetaModifier, as Qt names them under
/// Qt::AA_MacDontSwapCtrlAndMeta, which the app sets.
/// @param chord The chord a binding runs on.
/// @return The combination, or @c std::nullopt when Qt has no key for it: a chord holding Hyper or Meta, a
///         named key Qt has no code for, or a character outside the Basic Multilingual Plane.
[[nodiscard]] std::optional<QKeyCombination> qtKeyFor(command::ShortcutChord const& chord);

/// Whether @p chord may become a menu row's key equivalent: it holds Control or Super (⌃ or ⌘).
///
/// A row whose chord holds neither gets no shortcut, so AppKit cannot fire "Clear Selection" on Escape, or
/// scroll on ⇧↓, from a text field of the palette or of the settings page.
/// @param chord The chord.
/// @return True when it holds Control or Super.
[[nodiscard]] bool carriesKeyEquivalent(command::ShortcutChord const& chord) noexcept;

/// What the menu bar reads the world through, and runs its rows with.
struct AppMenuBarSources
{
    std::function<command::MenuBarState()> state {};    ///< The world the bar is drawn over.
    std::function<command::ChordIndex()> chords {};     ///< The chords the rows show.
    std::function<void(actions::Action const&)> run {}; ///< Runs a picked row's action.
};

/// The application's own menu bar: the six menus of buildMenuBar(), as a native QMenuBar.
///
/// On macOS it is the bar at the top of the screen, under every `ui_style` alike. Only the role rows'
/// standard keys (⌘Q, ⌘,) are the menu's to take from the terminal (display::MenuOwnedKeys): every other
/// chord a row shows stays the terminal's. ⌘H and ⌥⌘H are no exception, so they reach the terminal rather
/// than hiding the application.
class AppMenuBar
{
  public:
    /// Builds the bar and its menus, fills them, and hands the Window and Help menus to @p integration.
    /// @pre A QApplication exists, and this runs on the GUI thread.
    /// @param sources What the bar reads and runs through.
    /// @param integration The OS integration; it must outlive this bar.
    AppMenuBar(AppMenuBarSources sources, platform::IMenuBarIntegration& integration);
    ~AppMenuBar();

    AppMenuBar(AppMenuBar const&) = delete;
    AppMenuBar& operator=(AppMenuBar const&) = delete;
    AppMenuBar(AppMenuBar&&) = delete;
    AppMenuBar& operator=(AppMenuBar&&) = delete;

    /// Refills every menu from a fresh tree. The menus themselves stay the objects they were, so the OS
    /// keeps the ones it adopted. For a reload of the configuration.
    void rebuild();

    /// @return The bar.
    [[nodiscard]] QMenuBar& menuBar() noexcept { return *_menuBar; }

  private:
    /// The tree of the whole bar, over the sources' current state and chords.
    [[nodiscard]] command::MenuTree resolve() const;

    /// The menu bar menu @p which.
    [[nodiscard]] QMenu& menu(command::MenuBarMenu which) const;

    /// Adds a row per node of @p nodes to @p menu, ahead of @p before (at the end when null).
    /// @return The actions added to @p menu itself.
    QList<QAction*> fill(QMenu& menu,
                         command::MenuBarMenu which,
                         std::span<command::MenuNode const> nodes,
                         command::MenuTree const& tree,
                         QAction* before);

    /// Refreshes the state of @p which's rows from a fresh tree, as the menu opens.
    void refresh(command::MenuBarMenu which);

    /// Replaces the Window menu's tab list with the one of @p nodes.
    void refillTabList(std::span<command::MenuNode const> nodes, command::MenuTree const& tree);

    AppMenuBarSources _sources;
    std::unique_ptr<QMenuBar> _menuBar;
    std::array<QMenu*, command::MenuBarMenuCount> _menus {}; ///< Owned by _menuBar.
    QAction* _tabListEnd = nullptr;                          ///< Hidden; the tab list goes ahead of it.
    QList<QAction*> _tabList {};                             ///< The Window menu's tab rows.
};

} // namespace contour::window
