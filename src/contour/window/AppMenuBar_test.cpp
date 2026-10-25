// SPDX-License-Identifier: Apache-2.0
//
// The native menu bar. The key and role tables need only QtGui; the bar itself is Qt Widgets, which only a
// QApplication can stand up, so those cases run in a display run (CONTOUR_TEST_DISPLAY=1) alone.

#include <contour/command/Command.hpp>
#include <contour/command/MenuBarTable.hpp>
#include <contour/command/MenuTree.hpp>
#include <contour/command/Shortcut.hpp>
#include <contour/config/Actions.hpp>
#include <contour/config/Config.hpp>
#include <contour/test/MenuFixtures.hpp>
#include <contour/window/AppMenuBar.hpp>

#include <QtCore/QCoreApplication>
#include <QtGui/QAction>
#include <QtGui/QKeySequence>
#include <QtWidgets/QApplication>
#include <QtWidgets/QMenu>
#include <QtWidgets/QMenuBar>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <optional>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

using namespace contour;
using command::MenuBarMenu;
using command::MenuItemRole;
using command::ShortcutChord;
using contour::window::carriesKeyEquivalent;
using contour::window::qtKeyFor;
using contour::window::qtMenuRoleFor;
using vtbackend::Key;
using vtbackend::Modifier;
using vtbackend::Modifiers;

namespace
{

/// The action titled @p title in @p menu or its submenus, or nullptr.
[[nodiscard]] QAction* findAction(QMenu const& menu, QString const& title)
{
    for (auto* const action: menu.actions())
    {
        if (action->menu() != nullptr)
        {
            if (auto* const nested = findAction(*action->menu(), title); nested != nullptr)
                return nested;
        }
        else if (action->text() == title)
            return action;
    }
    return nullptr;
}

/// The command rows of @p menu, counted into its submenus.
[[nodiscard]] std::size_t commandActionCount(QMenu const& menu)
{
    auto count = std::size_t { 0 };
    for (auto const* const action: menu.actions())
    {
        if (action->isSeparator())
            continue;
        count += action->menu() != nullptr ? commandActionCount(*action->menu()) : 1;
    }
    return count;
}

/// The command nodes of @p nodes, counted into their submenus.
[[nodiscard]] std::size_t commandNodeCount(std::vector<command::MenuNode> const& nodes)
{
    auto count = std::size_t { 0 };
    for (auto const& node: nodes)
    {
        if (node.kind == command::ContextMenuEntryKind::Command)
            ++count;
        count += commandNodeCount(node.children);
    }
    return count;
}

/// A window of @p titles tabs, the first one active.
[[nodiscard]] command::MenuBarState windowWithTabs(std::vector<std::string> titles)
{
    auto state = command::MenuBarState {};
    state.window = command::WindowPresence::Present;
    state.tabCount = static_cast<int>(titles.size());
    state.activeTab = 0;
    state.paneCount = 1;
    state.tabTitles = std::move(titles);
    return state;
}

/// A bar over a state the test changes, recording what it runs.
struct BarFixture
{
    config::Config config {};
    command::MenuBarState state = windowWithTabs({ "one" });
    std::vector<actions::Action> ran {};
    test::RecordingMenuBarIntegration integration {};
    window::AppMenuBar bar {
        window::AppMenuBarSources {
            .state = [this] { return state; },
            .chords = [this] { return command::chordIndex(config); },
            .run = [this](actions::Action const& action) { ran.push_back(action); },
        },
        integration,
    };

    /// @return The top-level menu @p which.
    [[nodiscard]] QMenu& menu(MenuBarMenu which)
    {
        auto* const menu = bar.menuBar().actions().at(static_cast<qsizetype>(which))->menu();
        REQUIRE(menu != nullptr);
        return *menu;
    }

    /// @return The action titled @p title anywhere in the bar.
    [[nodiscard]] QAction& action(QString const& title)
    {
        auto* found = static_cast<QAction*>(nullptr);
        for (auto const* const top: bar.menuBar().actions())
            if (found == nullptr)
                found = findAction(*top->menu(), title);
        INFO("no action titled " << title.toStdString());
        REQUIRE(found != nullptr);
        return *found;
    }
};

#define REQUIRE_WIDGETS_OR_SKIP()                                                       \
    do                                                                                  \
    {                                                                                   \
        if (qobject_cast<QApplication*>(QCoreApplication::instance()) == nullptr)       \
            SKIP("Needs a QApplication: run with CONTOUR_TEST_DISPLAY=1 on a display"); \
    } while (false)

} // namespace

// {{{ [menubar][keys]
TEST_CASE("every role maps", "[menubar][keys]")
{
    struct Row
    {
        MenuItemRole role;
        QAction::MenuRole application;
        QAction::MenuRole elsewhere;
    };
    auto constexpr Rows = std::array {
        Row { MenuItemRole::None, QAction::ApplicationSpecificRole, QAction::NoRole },
        Row { MenuItemRole::About, QAction::AboutRole, QAction::AboutRole },
        Row { MenuItemRole::Preferences, QAction::PreferencesRole, QAction::PreferencesRole },
        Row { MenuItemRole::Quit, QAction::QuitRole, QAction::QuitRole },
    };
    for (auto const& row: Rows)
    {
        CHECK(qtMenuRoleFor(row.role, MenuBarMenu::Application) == row.application);
        CHECK(qtMenuRoleFor(row.role, MenuBarMenu::Edit) == row.elsewhere);
    }
}

TEST_CASE("every default chord maps", "[menubar][keys]")
{
    for (auto const edition: { config::DefaultsEdition::Classic, config::DefaultsEdition::Native2026 })
    {
        auto const config = config::Config { .defaultsEdition = edition };
        auto const chords = command::chordIndex(config);
        REQUIRE_FALSE(chords.empty());
        for (auto const& [id, chord]: chords)
        {
            INFO(id << ": " << command::shortcutText(chord, command::ShortcutStyle::Text));
            CHECK(qtKeyFor(chord).has_value());
        }

        auto const vi = chords.find("ViNormalMode");
        REQUIRE(vi != chords.end());
        auto const key = qtKeyFor(vi->second);
        REQUIRE(key.has_value());
        CHECK(key->key() == Qt::Key_Space);
        CHECK(key->keyboardModifiers() == (Qt::ControlModifier | Qt::ShiftModifier));
    }
}

TEST_CASE("a chord Qt has no key for is dropped", "[menubar][keys]")
{
    CHECK_FALSE(qtKeyFor({ .modifiers = Modifiers { Modifier::Hyper }, .input = U'é' }).has_value());
    CHECK_FALSE(qtKeyFor({ .modifiers = Modifiers { Modifier::Meta }, .input = U'é' }).has_value());
}

TEST_CASE("a chord maps its modifiers and its key", "[menubar][keys]")
{
    auto const superT = qtKeyFor({ .modifiers = Modifiers { Modifier::Super }, .input = U't' });
    REQUIRE(superT.has_value());
    CHECK(*superT == QKeyCombination(Qt::MetaModifier, Qt::Key_T));

    auto const altEnter = qtKeyFor({ .modifiers = Modifiers { Modifier::Alt }, .input = Key::Enter });
    REQUIRE(altEnter.has_value());
    CHECK(*altEnter == QKeyCombination(Qt::AltModifier, Qt::Key_Return));
}

TEST_CASE("only ⌃ and ⌘ chords are key equivalents", "[menubar][keys]")
{
    CHECK(carriesKeyEquivalent({ .modifiers = Modifiers { Modifier::Control }, .input = U'C' }));
    CHECK(carriesKeyEquivalent({ .modifiers = Modifiers { Modifier::Super }, .input = U'T' }));
    CHECK_FALSE(carriesKeyEquivalent({ .modifiers = Modifiers {}, .input = Key::Escape }));
    CHECK_FALSE(
        carriesKeyEquivalent({ .modifiers = Modifiers { Modifier::Shift }, .input = Key::DownArrow }));
}
// }}}

// {{{ [display][menubar]
TEST_CASE("the bar holds the six menus", "[display][menubar]")
{
    REQUIRE_WIDGETS_OR_SKIP();
    auto fixture = BarFixture {};

    auto const tops = fixture.bar.menuBar().actions();
    auto const topCount = static_cast<std::size_t>(tops.size());
    REQUIRE(topCount == command::MenuBarMenuCount);
    auto const entries = command::buildMenuBar(fixture.state);
    for (auto const index: std::views::iota(std::size_t { 0 }, command::MenuBarMenuCount))
        CHECK(tops.at(static_cast<qsizetype>(index))->text().toStdString() == entries[index].title);

    REQUIRE(fixture.integration.adoptions.size() == 1);
    CHECK(fixture.integration.adoptions.front().windows == &fixture.menu(MenuBarMenu::Window));
    CHECK(fixture.integration.adoptions.front().help == &fixture.menu(MenuBarMenu::Help));
}

TEST_CASE("the rows are the tree's", "[display][menubar]")
{
    REQUIRE_WIDGETS_OR_SKIP();
    auto fixture = BarFixture {};

    auto const tree = command::buildMenuTree(command::buildMenuBar(fixture.state),
                                             command::chordIndex(fixture.config),
                                             command::platformShortcutStyle());
    REQUIRE(tree.nodes.size() == command::MenuBarMenuCount);
    for (auto const index: std::views::iota(std::size_t { 0 }, command::MenuBarMenuCount))
    {
        INFO(tree.nodes[index].title);
        CHECK(commandActionCount(fixture.menu(static_cast<MenuBarMenu>(index)))
              == commandNodeCount(tree.nodes[index].children));
    }

    CHECK(fixture.action(QStringLiteral("About Contour")).menuRole() == QAction::AboutRole);
    CHECK(fixture.action(QStringLiteral("Settings…")).menuRole() == QAction::PreferencesRole);
    CHECK(fixture.action(QStringLiteral("Quit Contour")).menuRole() == QAction::QuitRole);
}

TEST_CASE("the Quit and Settings… actions carry the standard keys", "[display][menubar]")
{
    REQUIRE_WIDGETS_OR_SKIP();
    auto fixture = BarFixture {};

    CHECK(fixture.action(QStringLiteral("Quit Contour")).shortcut() == QKeySequence(QKeySequence::Quit));
    CHECK(fixture.action(QStringLiteral("Settings…")).shortcut() == QKeySequence(QKeySequence::Preferences));
}

TEST_CASE("a chord without ⌃ or ⌘ is no key equivalent", "[display][menubar]")
{
    REQUIRE_WIDGETS_OR_SKIP();
    auto fixture = BarFixture {};

    CHECK(fixture.action(QStringLiteral("Clear Selection")).shortcut().isEmpty());
    CHECK_FALSE(fixture.action(QStringLiteral("Copy")).shortcut().isEmpty());
}

TEST_CASE("the role rows survive a rebuild", "[display][menubar]")
{
    REQUIRE_WIDGETS_OR_SKIP();
    auto fixture = BarFixture {};

    fixture.bar.rebuild();

    CHECK(fixture.action(QStringLiteral("About Contour")).menuRole() == QAction::AboutRole);
    CHECK(fixture.action(QStringLiteral("Settings…")).menuRole() == QAction::PreferencesRole);
    CHECK(fixture.action(QStringLiteral("Quit Contour")).menuRole() == QAction::QuitRole);
    CHECK(fixture.integration.adoptions.size() == 1);
}

TEST_CASE("a triggered row runs its action", "[display][menubar]")
{
    REQUIRE_WIDGETS_OR_SKIP();
    auto fixture = BarFixture {};

    fixture.action(QStringLiteral("New Tab")).trigger();
    CHECK(fixture.ran.empty()); // posted, not run inside the trigger
    QCoreApplication::processEvents();

    REQUIRE(fixture.ran.size() == 1);
    CHECK(command::commandId(fixture.ran.front()) == "CreateNewTab");
}

TEST_CASE("a menu opens with the current state", "[display][menubar]")
{
    REQUIRE_WIDGETS_OR_SKIP();
    auto fixture = BarFixture {};
    auto& windowMenu = fixture.menu(MenuBarMenu::Window);
    auto* const closeOthers = &fixture.action(QStringLiteral("Close Other Tabs"));
    CHECK_FALSE(closeOthers->isEnabled());
    CHECK(findAction(windowMenu, QStringLiteral("two")) == nullptr);

    fixture.state = windowWithTabs({ "one", "two" });
    emit windowMenu.aboutToShow();

    // The same action, refreshed in place, and the tab list grown by a row.
    CHECK(&fixture.action(QStringLiteral("Close Other Tabs")) == closeOthers);
    CHECK(closeOthers->isEnabled());
    auto* const second = findAction(windowMenu, QStringLiteral("two"));
    REQUIRE(second != nullptr);
    CHECK_FALSE(second->isChecked());
    CHECK(fixture.action(QStringLiteral("one")).isChecked());
    CHECK(commandActionCount(windowMenu)
          == commandNodeCount(command::buildMenuTree(command::buildMenuBar(fixture.state), {}, {})
                                  .nodes[static_cast<std::size_t>(MenuBarMenu::Window)]
                                  .children));
}

#ifdef __APPLE__
TEST_CASE("Contour appears once", "[display][menubar]")
{
    REQUIRE_WIDGETS_OR_SKIP();
    auto fixture = BarFixture {};

    CHECK_FALSE(fixture.menu(MenuBarMenu::Application).menuAction()->isVisible());
}
#endif
// }}}
