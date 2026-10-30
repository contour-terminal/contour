// SPDX-License-Identifier: Apache-2.0
//
// The Cocoa quick-terminal panel on a real NSPanel (ruling S1, candidate B): a Qt::Tool | Qt::FramelessWindowHint
// window becomes a non-activating panel that takes the keyboard while the application the user came from stays
// frontmost. Needs the cocoa platform plugin, so every case runs in a display run (CONTOUR_TEST_DISPLAY=1).

#include <contour/platform/QuickTerminalPanel.hpp>
#include <contour/test/TestEnvironment.hpp>

#include <core/Environment.hpp>

#include <QtGui/QGuiApplication>
#include <QtGui/QWindow>

#include <catch2/catch_test_macros.hpp>

#include <memory>

#import <AppKit/AppKit.h>
#include <QtTest/QTest>

namespace
{

#define REQUIRE_COCOA_OR_SKIP()                                                 \
    do                                                                          \
    {                                                                           \
        if (QGuiApplication::platformName() != QStringLiteral("cocoa")          \
            || !contour::test::displayTestsEnabled(core::LiveEnvironment {}))   \
            SKIP("Needs CONTOUR_TEST_DISPLAY=1 and the cocoa platform plugin"); \
    } while (false)

/// The NSPanel behind @p window, or nil.
NSPanel* panelBehind(QWindow& window)
{
    // Under the cocoa plugin a WId is the NSView*; ARC wants the bridge from an untyped pointer.
    auto* view = (__bridge NSView*) reinterpret_cast<void*>(window.winId());
    NSWindow* nsWindow = view != nil ? [view window] : nil;
    return [nsWindow isKindOfClass:[NSPanel class]] ? (NSPanel*) nsWindow : nil;
}

} // namespace

TEST_CASE("the Cocoa panel makes a tool window a non-activating panel", "[quickterminal][cocoa][display]")
{
    REQUIRE_COCOA_OR_SKIP();
    auto const panel = contour::platform::makeQuickTerminalPanel();
    auto window = QWindow {};
    window.setFlags(Qt::Tool | Qt::FramelessWindowHint);
    window.resize(400, 200);

    panel->present(window, {});
    REQUIRE(QTest::qWaitForWindowExposed(&window));
    NSPanel* nsPanel = panelBehind(window);
    REQUIRE(nsPanel != nil);
    CHECK((nsPanel.styleMask & NSWindowStyleMaskNonactivatingPanel) != 0);
    CHECK(nsPanel.floatingPanel);
    CHECK_FALSE(nsPanel.becomesKeyOnlyIfNeeded);
    CHECK((nsPanel.collectionBehavior & NSWindowCollectionBehaviorFullScreenAuxiliary) != 0);
    panel->dismiss(window);
}

TEST_CASE("the Cocoa panel takes the keyboard while the frontmost application stays", "[quickterminal][cocoa][display]")
{
    REQUIRE_COCOA_OR_SKIP();
    auto* const workspace = NSWorkspace.sharedWorkspace;
    NSRunningApplication* const before = workspace.frontmostApplication;

    auto const panel = contour::platform::makeQuickTerminalPanel();
    auto window = QWindow {};
    window.setFlags(Qt::Tool | Qt::FramelessWindowHint);
    window.resize(400, 200);
    panel->present(window, {});
    REQUIRE(QTest::qWaitForWindowExposed(&window));

    NSPanel* nsPanel = panelBehind(window);
    REQUIRE(nsPanel != nil);
    CHECK(nsPanel.keyWindow);
    // Contour did not become the frontmost application: the one the user came from still is.
    CHECK(workspace.frontmostApplication.processIdentifier == before.processIdentifier);
    panel->dismiss(window);
}
