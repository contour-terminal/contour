// SPDX-License-Identifier: Apache-2.0
//
// The Cocoa menu bar integration hands the Window and Help menus to NSApp. A QMenu needs a QApplication and
// toNSMenu() needs the cocoa platform plugin, so every case runs in a display run (CONTOUR_TEST_DISPLAY=1)
// alone; offscreen, constructing a QMenu would abort the suite.

#include <contour/command/MenuBarTable.hpp>
#include <contour/command/MenuTree.hpp>
#include <contour/command/Shortcut.hpp>
#include <contour/config/Actions.hpp>
#include <contour/config/Config.hpp>
#include <contour/platform/MenuBarIntegration.hpp>
#include <contour/test/ScopedApplicationAttribute.hpp>
#include <contour/test/TestEnvironment.hpp>
#include <contour/window/AppMenuBar.hpp>

#include <core/Environment.hpp>

#include <QtCore/QCoreApplication>
#include <QtWidgets/QApplication>
#include <QtWidgets/QMenu>
#include <QtWidgets/QMenuBar>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <memory>
#include <ranges>
#include <variant>
#include <vector>

#import <AppKit/AppKit.h>
#import <Carbon/Carbon.h>

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
            .run = [](contour::actions::Action const&, contour::command::ActionScope) {},
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
            .run = [](contour::actions::Action const&, contour::command::ActionScope) {},
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
            .run = [](contour::actions::Action const&, contour::command::ActionScope) {},
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
            .run = [](contour::actions::Action const&, contour::command::ActionScope) {},
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

namespace
{

/// A ⌘-chord key-down AppKit would hand the main menu, for @p key with @p modifiers.
NSEvent* keyDown(NSString* key, unsigned short keyCode, NSEventModifierFlags modifiers)
{
    return [NSEvent keyEventWithType:NSEventTypeKeyDown
                            location:NSZeroPoint
                       modifierFlags:modifiers
                           timestamp:0
                        windowNumber:0
                             context:nil
                          characters:key
         charactersIgnoringModifiers:key
                           isARepeat:NO
                             keyCode:keyCode];
}


/// Whether AppKit would let @p item fire now: what its target, found as AppKit finds it, says.
bool validates(NSMenuItem* item)
{
    id const target = [NSApp targetForAction:item.action to:item.target from:item];
    return target != nil
           && (![target respondsToSelector:@selector(validateMenuItem:)] || [target validateMenuItem:item]);
}

/// The application menu item Qt made of the Contour menu's @p row: a role row under Qt's own title, found by
/// its role's mark, and any other row under its own title.
NSMenuItem* applicationMenuItemFor(NSMenu* menu, contour::command::MenuNode const& row)
{
    auto const firstWhere = [&](auto const& matches) -> NSMenuItem* {
        for (NSMenuItem* const item in menu.itemArray)
            if (!item.hidden && item.action == @selector(qt_itemFired:) && matches(item))
                return item;
        return nil;
    };
    switch (row.role)
    {
        case contour::command::MenuItemRole::About:
            return firstWhere([](NSMenuItem* item) { return [item.title hasPrefix:@"About "]; });
        case contour::command::MenuItemRole::Preferences:
            return firstWhere([](NSMenuItem* item) { return [item.keyEquivalent isEqualToString:@","]; });
        case contour::command::MenuItemRole::Quit:
            return firstWhere([](NSMenuItem* item) { return [item.keyEquivalent isEqualToString:@"q"]; });
        case contour::command::MenuItemRole::None:
        case contour::command::MenuItemRole::Minimize: {
            NSString* const title = [NSString stringWithUTF8String:row.title.c_str()];
            return firstWhere([&](NSMenuItem* item) { return [item.title isEqualToString:title]; });
        }
    }
    return nil;
}

/// Checks that every item of the application menu fires: one per command row of the Contour menu's table,
/// and the ones AppKit adds itself.
void checkApplicationMenuFires(contour::command::MenuBarState const& state)
{
    NSMenu* const applicationMenu = [NSApp.mainMenu itemAtIndex:0].submenu;
    REQUIRE(applicationMenu != nil);
    auto const tree = contour::command::buildMenuTree(
        contour::command::buildMenuBar(state), {}, contour::command::ShortcutStyle::Text);
    auto const& rows = tree.nodes[static_cast<std::size_t>(contour::command::MenuBarMenu::Application)].children;
    auto commandRows = 0;
    for (auto const& row: rows)
    {
        if (row.kind != contour::command::ContextMenuEntryKind::Command)
            continue;
        ++commandRows;
        INFO(row.title);
        auto* const item = applicationMenuItemFor(applicationMenu, row);
        REQUIRE(item != nil);
        CHECK(validates(item));
    }
    CHECK(commandRows > 0);

    for (NSMenuItem* const item in applicationMenu.itemArray)
    {
        // Services opens a submenu; Show All is AppKit's to enable, while some application is hidden.
        if (item.separatorItem || item.hidden || item.hasSubmenu
            || item.action == @selector(unhideAllApplications:))
            continue;
        INFO(item.title.UTF8String);
        CHECK(validates(item));
    }
}

/// Hands a ⌘ chord to the main menu as AppKit does a key-down, and runs what it queued.
/// @return Whether a menu item took the chord.
bool performKeyEquivalent(NSString* key, unsigned short keyCode)
{
    // What AppKit's run loop does between events, which a test has none of: the menus re-validate, so a
    // row's cached enabled state is the one its validator gives now.
    for (NSMenuItem* const top in NSApp.mainMenu.itemArray)
        [top.submenu update];
    auto const handled = [NSApp.mainMenu performKeyEquivalent:keyDown(key, keyCode, NSEventModifierFlagCommand)];
    // Qt queues the item's activation, and the bar posts the run: two turns of the loop.
    for (auto const _: std::views::iota(0, 3))
        QCoreApplication::processEvents();
    return handled;
}

} // namespace

TEST_CASE("the application menu fires its rows and keys with no window, and after one gains focus",
          "[display][menubar][cocoa]")
{
    REQUIRE_COCOA_OR_SKIP();
    // As the app runs: ⌘ is Qt::MetaModifier, and the Minimize row's fixed chord is spelled with it.
    auto const unswapped = contour::test::ScopedApplicationAttribute(Qt::AA_MacDontSwapCtrlAndMeta);
    auto const config = contour::config::Config {};
    auto state = contour::command::MenuBarState {}; // as at startup: no window yet
    auto integration = contour::platform::NullMenuBarIntegration {};
    auto ran = std::vector<contour::actions::Action> {};
    auto bar = contour::window::AppMenuBar {
        contour::window::AppMenuBarSources {
            .state = [&] { return state; },
            .chords = [&] { return contour::command::chordIndex(config); },
            .run = [&](contour::actions::Action const& action,
                       contour::command::ActionScope) { ran.push_back(action); },
        },
        integration,
    };
    QCoreApplication::processEvents();

    checkApplicationMenuFires(state);
    bar.rebuild();
    QCoreApplication::processEvents();
    checkApplicationMenuFires(state);
    CHECK(performKeyEquivalent(@"q", kVK_ANSI_Q));
    CHECK(performKeyEquivalent(@",", kVK_ANSI_Comma));
    REQUIRE(ran.size() == 2);
    CHECK(std::holds_alternative<contour::actions::Quit>(ran[0]));
    CHECK(std::holds_alternative<contour::actions::OpenConfiguration>(ran[1]));

    // A window gains focus, which has the bar look again: ⌘M now minimises it, with no menu ever opened.
    state.window = contour::command::WindowPresence::Present;
    state.tabCount = 1;
    state.tabTitles = { "one" };
    bar.refreshAll();
    checkApplicationMenuFires(state);
    NSMenu* const windows =
        bar.menuBar().actions().at(static_cast<qsizetype>(contour::command::MenuBarMenu::Window))->menu()->toNSMenu();
    CHECK([NSApp.mainMenu indexOfItemWithSubmenu:windows] >= 0);
    ran.clear();
    CHECK(performKeyEquivalent(@"q", kVK_ANSI_Q));
    CHECK(performKeyEquivalent(@"m", kVK_ANSI_M));
    REQUIRE(ran.size() == 2);
    CHECK(std::holds_alternative<contour::actions::Quit>(ran[0]));
    CHECK(std::holds_alternative<contour::actions::MinimizeWindow>(ran[1]));
}

TEST_CASE("Contour appears once", "[display][menubar][cocoa]")
{
    REQUIRE_COCOA_OR_SKIP();
    auto const config = contour::config::Config {};
    auto integration = contour::platform::NullMenuBarIntegration {};
    auto bar = contour::window::AppMenuBar {
        contour::window::AppMenuBarSources {
            .state = [&] { return contour::command::menuBarStateOf(config); },
            .chords = [&] { return contour::command::chordIndex(config); },
            .run = [](contour::actions::Action const&, contour::command::ActionScope) {},
        },
        integration,
    };
    QCoreApplication::processEvents();

    // Qt moved every row into the application menu, and hides the menu left with separators alone.
    NSMenu* const contourMenu =
        bar.menuBar().actions().at(static_cast<qsizetype>(contour::command::MenuBarMenu::Application))
            ->menu()
            ->toNSMenu();
    auto const index = [NSApp.mainMenu indexOfItemWithSubmenu:contourMenu];
    REQUIRE(index >= 0);
    CHECK([NSApp.mainMenu itemAtIndex:index].hidden);
}
