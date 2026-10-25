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
#include <contour/test/CoreFixtures.hpp>
#include <contour/test/MenuFixtures.hpp>
#include <contour/test/ScopedApplicationAttribute.hpp>
#include <contour/window/AppMenuBar.hpp>

#include <QtCore/QCoreApplication>
#include <QtGui/QAction>
#include <QtGui/QGuiApplication>
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
#include <variant>
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
    std::vector<command::ActionScope> scopes {}; ///< The scope each run of @ref ran was handed.
    test::RecordingMenuBarIntegration integration {};
    window::AppMenuBar bar {
        window::AppMenuBarSources {
            .state = [this] { return state; },
            .chords = [this] { return command::chordIndex(config); },
            .run =
                [this](actions::Action const& action, command::ActionScope scope) {
                    ran.push_back(action);
                    scopes.push_back(scope);
                },
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
        Row { MenuItemRole::Minimize, QAction::NoRole, QAction::NoRole },
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
TEST_CASE("the user's bindings of menu-owned chords are named under a global menu bar alone",
          "[menubar][keys]")
{
    // As the app runs: ⌘ is Qt::MetaModifier, as the menu-owned key table spells it.
    auto const unswapped = test::ScopedApplicationAttribute(Qt::AA_MacDontSwapCtrlAndMeta);
    auto const config = test::loadConfigFromYaml(R"(
input_mapping:
    - { mods: [Super], key: 'Q', action: Quit }
    - { mods: [Super], key: ',', action: OpenConfiguration }
    - { mods: [Super], key: 'M', action: MinimizeWindow }
    - { mods: [Super], key: 'H', action: ScrollToTop }
    - { mods: [Super, Alt], key: 'H', action: ScrollToBottom }
    - { mods: [Super], key: 'W', action: CloseTab }
    - { mods: [Control], key: 'Q', action: Quit }
)");
    auto const& mappings = config.inputMappings.value();

    auto const owned = window::menuOwnedBindings(mappings, command::MenuBarPlacement::Global);

    auto const spelled =
        std::ranges::to<std::vector>(owned | std::views::transform([](auto const& chord) {
                                         return command::shortcutText(chord, command::ShortcutStyle::Text);
                                     }));
    // One per binding, in config order; ⌘W and ⌃Q stay the terminal's. Quit's and Settings…'s keys are the
    // platform theme's standard keys, which only the macOS theme binds to ⌘Q and ⌘, -- offscreen, Quit is
    // Qt::Key_Exit -- so those two are owned on the cocoa platform alone (CONTOUR_TEST_DISPLAY=1).
    auto expected = std::vector<std::string> { "Super+M", "Super+H", "Alt+Super+H" };
    if (QGuiApplication::platformName() == QStringLiteral("cocoa"))
        expected.insert(expected.begin(), { "Super+Q", "Super+," });
    CHECK(spelled == expected);
    CHECK(window::menuOwnedBindings(mappings, command::MenuBarPlacement::None).empty());
}

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

TEST_CASE("the Window menu starts with Minimize and Zoom, and Minimize carries ⌘M", "[display][menubar]")
{
    REQUIRE_WIDGETS_OR_SKIP();
    auto fixture = BarFixture {};

    auto const rows = fixture.menu(MenuBarMenu::Window).actions();
    REQUIRE(rows.size() >= 3);
    CHECK(rows.at(0)->text() == QStringLiteral("Minimize"));
    CHECK(rows.at(1)->text() == QStringLiteral("Zoom"));
    CHECK(rows.at(2)->isSeparator());

    // ⌘ is Qt::MetaModifier under Qt::AA_MacDontSwapCtrlAndMeta, which the app sets.
    CHECK(rows.at(0)->shortcut() == QKeySequence(QKeyCombination(Qt::MetaModifier, Qt::Key_M)));
    CHECK(rows.at(1)->shortcut().isEmpty());
}

TEST_CASE("Minimize and Zoom run their actions", "[display][menubar]")
{
    REQUIRE_WIDGETS_OR_SKIP();
    auto fixture = BarFixture {};

    fixture.action(QStringLiteral("Minimize")).trigger();
    fixture.action(QStringLiteral("Zoom")).trigger();
    QCoreApplication::processEvents();

    REQUIRE(fixture.ran.size() == 2);
    CHECK(std::holds_alternative<actions::MinimizeWindow>(fixture.ran[0]));
    CHECK(std::holds_alternative<actions::ZoomWindow>(fixture.ran[1]));
}

TEST_CASE("the bar has Qt title its Preferences-role row Settings…", "[display][menubar]")
{
    REQUIRE_WIDGETS_OR_SKIP();
    // Qt's cocoa plugin titles the row from this context and source, whatever the action's text says.
    auto const qtTitle = [] {
        return QCoreApplication::translate("MAC_APPLICATION_MENU", "Preferences...");
    };
    REQUIRE(qtTitle() == QStringLiteral("Preferences..."));

    {
        auto fixture = BarFixture {};
        CHECK(qtTitle() == QStringLiteral("Settings…"));
        // Only that one string: the other application menu strings stay Qt's.
        CHECK(QCoreApplication::translate("MAC_APPLICATION_MENU", "Quit %1") == QStringLiteral("Quit %1"));
        CHECK(QCoreApplication::translate("QMenuBar", "Preferences...") == QStringLiteral("Preferences..."));
    }

    CHECK(qtTitle() == QStringLiteral("Preferences..."));
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
    CHECK(fixture.scopes.front() == command::ActionScope::Window);
}

TEST_CASE("a triggered row runs in its row's scope", "[display][menubar]")
{
    REQUIRE_WIDGETS_OR_SKIP();
    auto fixture = BarFixture {};

    fixture.action(QStringLiteral("New Window")).trigger();
    fixture.action(QStringLiteral("Paste")).trigger();
    QCoreApplication::processEvents();

    REQUIRE(fixture.scopes.size() == 2);
    CHECK(fixture.scopes[0] == command::ActionScope::Application);
    CHECK(fixture.scopes[1] == command::ActionScope::Window);
}

/// Checks that every command row of the Application menu that @p state enables is enabled in @p bar, and
/// that the menu holding them is too: Qt hands a disabled menu's state to every row it merges on macOS.
void checkApplicationRowsEnabled(window::AppMenuBar& bar, command::MenuBarState const& state)
{
    auto* const application = bar.menuBar().actions().at(static_cast<qsizetype>(MenuBarMenu::Application));
    CHECK(application->isEnabled());
    auto const tree = command::buildMenuTree(command::buildMenuBar(state), {}, command::ShortcutStyle::Text);
    auto const& rows = tree.nodes[static_cast<std::size_t>(MenuBarMenu::Application)].children;
    REQUIRE(commandNodeCount(rows) == commandActionCount(*application->menu()));
    for (auto const& row: rows)
    {
        if (row.kind != command::ContextMenuEntryKind::Command)
            continue;
        INFO(row.title);
        REQUIRE(row.availability == command::RowAvailability::Enabled);
        auto const* const action = findAction(*application->menu(), QString::fromStdString(row.title));
        REQUIRE(action != nullptr);
        CHECK(action->isEnabled());
    }
}

TEST_CASE("the Application menu's rows are enabled with no window, and after one gains focus",
          "[display][menubar]")
{
    REQUIRE_WIDGETS_OR_SKIP();
    auto const config = config::Config {};
    auto state = command::MenuBarState {}; // as at startup: no window yet
    auto integration = test::RecordingMenuBarIntegration {};
    auto bar = window::AppMenuBar {
        window::AppMenuBarSources {
            .state = [&] { return state; },
            .chords = [&] { return command::chordIndex(config); },
            .run = [](actions::Action const&, command::ActionScope) {},
        },
        integration,
    };

    checkApplicationRowsEnabled(bar, state);

    state = windowWithTabs({ "one" });
    bar.refreshAll();
    checkApplicationRowsEnabled(bar, state);
}

TEST_CASE("Minimize follows the focused window with its menu never opened", "[display][menubar]")
{
    REQUIRE_WIDGETS_OR_SKIP();
    auto const config = config::Config {};
    auto state = command::MenuBarState {};
    auto integration = test::RecordingMenuBarIntegration {};
    auto bar = window::AppMenuBar {
        window::AppMenuBarSources {
            .state = [&] { return state; },
            .chords = [&] { return command::chordIndex(config); },
            .run = [](actions::Action const&, command::ActionScope) {},
        },
        integration,
    };
    auto& windows = *bar.menuBar().actions().at(static_cast<qsizetype>(MenuBarMenu::Window))->menu();
    REQUIRE(findAction(windows, QStringLiteral("Minimize")) != nullptr);
    CHECK_FALSE(findAction(windows, QStringLiteral("Minimize"))->isEnabled());

    state = windowWithTabs({ "one" });
    bar.refreshAll();
    CHECK(findAction(windows, QStringLiteral("Minimize"))->isEnabled());

    state = command::MenuBarState {};
    bar.refreshAll();
    CHECK_FALSE(findAction(windows, QStringLiteral("Minimize"))->isEnabled());
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

TEST_CASE("a menu whose rows changed is refilled as it opens", "[display][menubar]")
{
    REQUIRE_WIDGETS_OR_SKIP();
    auto fixture = BarFixture {};
    auto& editMenu = fixture.menu(MenuBarMenu::Edit);
    REQUIRE(fixture.state.speech == command::SpeechAvailability::Unavailable);
    CHECK(findAction(editMenu, QStringLiteral("Start Speaking")) == nullptr);

    fixture.state.speech = command::SpeechAvailability::Available;
    emit editMenu.aboutToShow();
    CHECK(findAction(editMenu, QStringLiteral("Start Speaking")) != nullptr);

    fixture.state.speech = command::SpeechAvailability::Unavailable;
    emit editMenu.aboutToShow();
    CHECK(findAction(editMenu, QStringLiteral("Start Speaking")) == nullptr);
}

TEST_CASE("an unchanged menu keeps its rows as it opens", "[display][menubar]")
{
    REQUIRE_WIDGETS_OR_SKIP();
    auto fixture = BarFixture {};
    auto& editMenu = fixture.menu(MenuBarMenu::Edit);
    auto* const copy = &fixture.action(QStringLiteral("Copy"));
    CHECK_FALSE(copy->isEnabled());

    fixture.state.selection = command::SelectionPresence::Present;
    emit editMenu.aboutToShow();

    CHECK(&fixture.action(QStringLiteral("Copy")) == copy);
    CHECK(copy->isEnabled());
}

TEST_CASE("the Application menu is never hidden", "[display][menubar]")
{
    REQUIRE_WIDGETS_OR_SKIP();
    auto fixture = BarFixture {};

    // A hidden QAction is a disabled one, and on macOS every row Qt merged from the menu would follow it.
    // That the native menu does not show twice is the cocoa suite's to check ("Contour appears once").
    auto const* const application = fixture.menu(MenuBarMenu::Application).menuAction();
    CHECK(application->isVisible());
    CHECK(application->isEnabled());
}
// }}}
