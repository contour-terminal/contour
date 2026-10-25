// SPDX-License-Identifier: Apache-2.0
//
// Every route to OpenConfiguration ends in the app's one Settings window: a session's action, the menu bar's
// Settings… row with the last terminal window minimised, and the same row with no terminal window at all.

#include <contour/command/MenuBarTable.hpp>
#include <contour/command/MenuTree.hpp>
#include <contour/command/Shortcut.hpp>
#include <contour/config/Actions.hpp>
#include <contour/session/TerminalSessionManager.hpp>
#include <contour/test/CoreFixtures.hpp>
#include <contour/test/GuiTestFixtures.hpp>
#include <contour/test/MenuFixtures.hpp>
#include <contour/window/SettingsController.hpp>
#include <contour/window/SettingsWindowHost.hpp>
#include <contour/window/WindowController.hpp>

#include <QtCore/QString>
#include <QtCore/QVariantMap>
#include <QtQuick/QQuickWindow>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <memory>

#include <QtTest/QTest>

namespace
{

using contour::test::createSessionTabs;
using contour::test::runMenuBarRow;
using contour::test::ScopedController;
using contour::test::TestApp;
namespace actions = contour::actions;

/// @return Whether @p controller lists a profile named @p name.
[[nodiscard]] bool listsProfile(contour::window::SettingsController const& controller, QString const& name)
{
    auto const profiles = controller.profiles();
    return std::ranges::any_of(profiles, [&name](auto const& row) {
        return row.toMap()[QStringLiteral("name")].toString() == name;
    });
}

} // namespace

TEST_CASE("OpenConfiguration from a session opens the one Settings window",
          "[contour][settingswindow][routing]")
{
    TestApp app { std::make_unique<contour::test::MockPtySessionFactory>() };
    ScopedController const window { app.manager() };
    std::ignore = createSessionTabs(app.manager(), *window, 1);
    auto* const session = window->activeSession();
    REQUIRE(session != nullptr);

    CHECK((*session)(actions::OpenConfiguration {}));
    auto* const settings = app.app().settingsWindow().window();
    REQUIRE(settings != nullptr);
    CHECK(settings->isVisible());

    CHECK((*session)(actions::OpenConfiguration {}));
    CHECK(app.app().settingsWindow().window() == settings);
    CHECK(settings->isVisible());

    contour::test::closeAllTabs(*window);
}

TEST_CASE("Settings… opens Settings while the last focused window is minimised",
          "[contour][settingswindow][routing]")
{
    TestApp app { std::make_unique<contour::test::MockPtySessionFactory>() };
    contour::test::ScopedControllerWithOsWindow const window { app.manager() };
    window->bindWindow(window.osWindow.get());
    std::ignore = createSessionTabs(app.manager(), *window, 1);
    app.manager().setFocusedWindow(window->windowId());
    window.osWindow->setWindowStates(Qt::WindowMinimized);
    auto const tree = contour::command::buildMenuTree(
        contour::command::buildMenuBar(window->menuBarState()), {}, contour::command::ShortcutStyle::Text);

    // As after ⌘M: the minimised window no longer holds focus.
    app.manager().clearFocusedWindow(window->windowId());
    REQUIRE_FALSE(app.manager().focusedWindow().has_value());

    runMenuBarRow(app, tree, "Settings…");

    auto* const settings = app.app().settingsWindow().window();
    REQUIRE(settings != nullptr);
    CHECK(settings->isVisible());
    CHECK(QTest::qWaitForWindowActive(settings));
    // Settings opens over nothing: the terminal window stays where the user put it.
    CHECK(window->isMinimized());

    contour::test::closeAllTabs(*window);
}

TEST_CASE("Settings… opens Settings with no terminal window at all", "[contour][settingswindow][routing]")
{
    TestApp app { std::make_unique<contour::test::MockPtySessionFactory>() };

    app.app().runMenuBarAction(actions::OpenConfiguration {}, contour::command::ActionScope::Application);

    auto* const settings = app.app().settingsWindow().window();
    REQUIRE(settings != nullptr);
    CHECK(settings->isVisible());
    CHECK(app.launcher().openedUrls.empty());
}

TEST_CASE("Open Configuration File still opens the file", "[contour][settingswindow][routing]")
{
    TestApp app { std::make_unique<contour::test::MockPtySessionFactory>() };
    ScopedController const window { app.manager() };
    std::ignore = createSessionTabs(app.manager(), *window, 1);
    auto* const session = window->activeSession();
    REQUIRE(session != nullptr);

    CHECK((*session)(actions::OpenConfiguration { .target = actions::ConfigurationTarget::Editor }));

    CHECK(app.launcher().openedUrls.size() == 1);
    CHECK(app.app().settingsWindow().window() == nullptr);

    contour::test::closeAllTabs(*window);
}

TEST_CASE("a reload refreshes the app's Settings", "[contour][settingswindow][routing]")
{
    contour::test::ConfiguredTestApp app { std::make_unique<contour::test::MockPtySessionFactory>() };
    contour::test::writeConfigFile(app.configFile(), contour::test::profileConfigYaml("main"));
    REQUIRE(app.manager().reloadAllSessions().has_value());
    app.app().openSettings(nullptr);
    REQUIRE(app.app().settingsWindow().window() != nullptr);
    REQUIRE_FALSE(listsProfile(app.app().settingsController(), QStringLiteral("second")));

    contour::test::writeConfigFile(app.configFile(),
                                   contour::test::profileConfigYaml("main")
                                       + "    second:\n"
                                         "        shell: /bin/sh\n");
    REQUIRE(app.manager().reloadAllSessions().has_value());

    CHECK(listsProfile(app.app().settingsController(), QStringLiteral("second")));
}
