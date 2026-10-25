// SPDX-License-Identifier: Apache-2.0
//
// The Cocoa menu bar integration hands the Window and Help menus to NSApp. A QMenu needs a QApplication and
// toNSMenu() needs the cocoa platform plugin, so every case runs in a display run (CONTOUR_TEST_DISPLAY=1)
// alone; offscreen, constructing a QMenu would abort the suite.

#include <contour/command/Shortcut.hpp>
#include <contour/config/Config.hpp>
#include <contour/platform/MenuBarIntegration.hpp>
#include <contour/test/TestEnvironment.hpp>
#include <contour/window/AppMenuBar.hpp>

#include <core/Environment.hpp>

#include <QtCore/QCoreApplication>
#include <QtWidgets/QApplication>
#include <QtWidgets/QMenu>
#include <QtWidgets/QMenuBar>

#include <catch2/catch_test_macros.hpp>

#include <memory>

#import <AppKit/AppKit.h>

namespace
{

#define REQUIRE_COCOA_OR_SKIP()                                                                  \
    do                                                                                           \
    {                                                                                            \
        if (qobject_cast<QApplication*>(QCoreApplication::instance()) == nullptr                 \
            || !contour::test::displayTestsEnabled(core::LiveEnvironment {}))                    \
            SKIP("Needs CONTOUR_TEST_DISPLAY=1 and a QApplication on the cocoa platform plugin"); \
    } while (false)

} // namespace

TEST_CASE("adopt hands the Window and Help menus to NSApp", "[display][menubar][cocoa]")
{
    REQUIRE_COCOA_OR_SKIP();
    auto bar = QMenuBar {};
    auto* const window = bar.addMenu(QStringLiteral("Window"));
    auto* const help = bar.addMenu(QStringLiteral("Help"));
    REQUIRE(window->toNSMenu() != nil);
    REQUIRE(help->toNSMenu() != nil);

    contour::platform::makeMenuBarIntegration()->adopt(*window, *help);

    CHECK(NSApp.windowsMenu == window->toNSMenu());
    CHECK(NSApp.helpMenu == help->toNSMenu());
}

TEST_CASE("a rebuild keeps the adopted menus", "[display][menubar][cocoa]")
{
    REQUIRE_COCOA_OR_SKIP();
    auto const config = contour::config::Config {};
    auto const integration = contour::platform::makeMenuBarIntegration();
    auto bar = contour::window::AppMenuBar {
        contour::window::AppMenuBarSources {
            .state = [&] { return contour::command::menuBarStateOf(config); },
            .chords = [&] { return contour::command::chordIndex(config); },
            .run = [](contour::actions::Action const&) {},
        },
        *integration,
    };
    NSMenu* const adopted = NSApp.windowsMenu;
    REQUIRE(adopted != nil);
    CHECK(adopted
          == bar.menuBar().actions().at(static_cast<qsizetype>(contour::command::MenuBarMenu::Window))
                 ->menu()
                 ->toNSMenu());

    bar.rebuild();

    CHECK(NSApp.windowsMenu == adopted);
}

TEST_CASE("a rebuild keeps the window list after the rows", "[display][menubar][cocoa]")
{
    REQUIRE_COCOA_OR_SKIP();
    auto const config = contour::config::Config {};
    auto const integration = contour::platform::makeMenuBarIntegration();
    auto bar = contour::window::AppMenuBar {
        contour::window::AppMenuBarSources {
            .state = [&] { return contour::command::menuBarStateOf(config); },
            .chords = [&] { return contour::command::chordIndex(config); },
            .run = [](contour::actions::Action const&) {},
        },
        *integration,
    };
    NSMenu* const windows = NSApp.windowsMenu;
    REQUIRE(windows != nil);
    // What AppKit appends for a listed window: an item of its own, after every row of ours.
    [windows addItem:[[NSMenuItem alloc] initWithTitle:@"Listed Window" action:nil keyEquivalent:@""]];
    REQUIRE([windows indexOfItemWithTitle:@"Listed Window"] == windows.numberOfItems - 1);

    bar.rebuild();

    REQUIRE(windows.numberOfItems > 0);
    CHECK([[windows itemAtIndex:0].title isEqualToString:@"Minimize"]);
    CHECK([windows indexOfItemWithTitle:@"Listed Window"] == windows.numberOfItems - 1);
}

TEST_CASE("the application menu holds the Contour menu's rows", "[display][menubar][cocoa]")
{
    REQUIRE_COCOA_OR_SKIP();
    auto const config = contour::config::Config {};
    auto integration = contour::platform::NullMenuBarIntegration {};
    auto const bar = contour::window::AppMenuBar {
        contour::window::AppMenuBarSources {
            .state = [&] { return contour::command::menuBarStateOf(config); },
            .chords = [&] { return contour::command::chordIndex(config); },
            .run = [](contour::actions::Action const&) {},
        },
        integration,
    };
    REQUIRE(NSApp.mainMenu.numberOfItems > 0);
    NSMenu* const applicationMenu = [NSApp.mainMenu itemAtIndex:0].submenu;
    REQUIRE(applicationMenu != nil);

    // Hidden, the Contour menu still hands every row to the menu macOS names after the app: the role rows
    // under Qt's own titles, with the standard keys, and the rest under theirs.
    for (NSString* const title in
         @[ @"Open Configuration File", @"Reload Configuration", @"Reset Configuration" ])
    {
        INFO(title.UTF8String);
        CHECK([applicationMenu indexOfItemWithTitle:title] >= 0);
    }
    for (NSString* const key in @[ @",", @"q" ])
    {
        INFO(key.UTF8String);
        auto const index =
            [applicationMenu.itemArray indexOfObjectPassingTest:^BOOL(NSMenuItem* item, NSUInteger, BOOL*) {
              return [item.keyEquivalent isEqualToString:key]
                     && item.keyEquivalentModifierMask == NSEventModifierFlagCommand;
            }];
        CHECK(index != NSNotFound);
    }
}

TEST_CASE("the Settings row is titled Settings… in the application menu", "[display][menubar][cocoa]")
{
    REQUIRE_COCOA_OR_SKIP();
    auto const config = contour::config::Config {};
    auto integration = contour::platform::NullMenuBarIntegration {};
    auto const bar = contour::window::AppMenuBar {
        contour::window::AppMenuBarSources {
            .state = [&] { return contour::command::menuBarStateOf(config); },
            .chords = [&] { return contour::command::chordIndex(config); },
            .run = [](contour::actions::Action const&) {},
        },
        integration,
    };
    REQUIRE(NSApp.mainMenu.numberOfItems > 0);
    NSMenu* const applicationMenu = [NSApp.mainMenu itemAtIndex:0].submenu;
    REQUIRE(applicationMenu != nil);

    // Qt titles a Preferences-role row from its own string, whatever the row's text: the bar must have
    // Qt say "Settings…", as macOS 13 and later do.
    auto const index = [applicationMenu indexOfItemWithTitle:@"Settings…"];
    CHECK(index >= 0);
    for (NSString* const qtTitle in @[ @"Preferences...", @"Preferences…" ])
    {
        INFO(qtTitle.UTF8String);
        CHECK([applicationMenu indexOfItemWithTitle:qtTitle] < 0);
    }
    if (index >= 0)
    {
        NSMenuItem* const item = [applicationMenu itemAtIndex:index];
        CHECK([item.keyEquivalent isEqualToString:@","]);
        CHECK_FALSE(item.hidden);
    }
}
