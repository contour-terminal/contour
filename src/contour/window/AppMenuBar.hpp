// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/command/MenuBarTable.hpp>
#include <contour/command/MenuTree.hpp>
#include <contour/command/Shortcut.hpp>
#include <contour/config/Actions.hpp>
#include <contour/config/Config.hpp>
#include <contour/platform/MenuBarIntegration.hpp>

#include <QtCore/QKeyCombination>
#include <QtCore/QList>
#include <QtGui/QAction>

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>

class QMenu;
class QMenuBar;
class QTranslator;

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

/// The chords of @p mappings that the menu bar takes from the terminal: one per key or character binding
/// whose chord is one of display::MenuOwnedKeys, decided as the terminal's ShortcutOverride claim decides
/// it, so the binding never runs.
/// @param mappings The configured input mappings.
/// @param placement Where this host's menu bar lives; only a global one takes a chord.
/// @return The bindings' chords, in configuration order (key bindings, then character bindings); empty
///         without a global menu bar.
[[nodiscard]] std::vector<command::ShortcutChord> menuOwnedBindings(config::InputMappings const& mappings,
                                                                    command::MenuBarPlacement placement);

/// What the menu bar reads the world through, and runs its rows with.
struct AppMenuBarSources
{
    std::function<command::MenuBarState()> state {}; ///< The world the bar is drawn over.
    std::function<command::ChordIndex()> chords {};  ///< The chords the rows show.
    /// Runs a picked row's action, with what the row acts on: see command::ActionScope.
    std::function<void(actions::Action const&, command::ActionScope)> run {};
};

/// The application's own menu bar: the six menus of buildMenuBar(), as a native QMenuBar.
///
/// On macOS it is the bar at the top of the screen, under every `ui_style` alike. Only the chords of
/// display::MenuOwnedKeys are the menu's to take from the terminal: the role rows' keys (⌘, ⌘Q and ⌘M) and
/// the application menu's own Hide (⌘H) and Hide Others (⌥⌘H). Every other chord a row shows stays the
/// terminal's.
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

    /// Brings every menu's rows up to date with the sources' state, as each would be on opening. For a
    /// change of the focused window: a menu-owned key (⌘M) fires its row with no menu ever opened, so the
    /// row must already be enabled for the window that is there now.
    void refreshAll();

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

    /// Brings @p which's rows up to date from a fresh tree, as the menu opens: refilled when their shape
    /// changed and the menu may be refilled, refreshed in place otherwise (see MenuOnOpen).
    void refresh(command::MenuBarMenu which);

    /// Replaces the Window menu's tab list with the one of @p nodes.
    void refillTabList(std::span<command::MenuNode const> nodes, command::MenuTree const& tree);

    AppMenuBarSources _sources;
    /// Installed for the bar's life, and ahead of it: it has Qt title the Preferences-role row "Settings…",
    /// which no QAction text can, as Qt titles that row from its own string.
    std::unique_ptr<QTranslator> _applicationMenuText;
    std::unique_ptr<QMenuBar> _menuBar;
    std::array<QMenu*, command::MenuBarMenuCount> _menus {}; ///< Owned by _menuBar.
    /// Hidden, and kept for the bar's life: the Window menu's rows go ahead of it, the window list AppKit
    /// appends to that menu after it, and no rebuild may move one past the other.
    QAction* _windowRowsEnd = nullptr;
    QList<QAction*> _tabList {}; ///< The Window menu's tab rows.
};

} // namespace contour::window
