// SPDX-License-Identifier: Apache-2.0
//
// The Cocoa frame adapter on a real NSWindow. Qt's ExpandedClientAreaHint owns the full-size content view
// (Step 1d spike, D1 = Hint); the adapter only hides the title and keeps the shadow. Each case loads a
// window shaped as Main.qml's root -- an ApplicationWindow whose `flags` carry the hint as Main.qml's
// binding sets it -- so a Quick Controls padding for the safe area would show. Needs the cocoa platform
// plugin, so every case runs in a display run (CONTOUR_TEST_DISPLAY=1) alone.

#include <contour/platform/CocoaWindowFrame.hpp>
#include <contour/test/TestEnvironment.hpp>

#include <core/Environment.hpp>

#include <QtCore/QByteArray>
#include <QtCore/QCoreApplication>
#include <QtCore/QUrl>
#include <QtGui/QGuiApplication>
#include <QtQml/QQmlComponent>
#include <QtQml/QQmlEngine>
#include <QtQuick/QQuickItem>
#include <QtQuick/QQuickWindow>

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

/// A Main.qml-shaped root: an ApplicationWindow with a 38-pt band at its top, expanded into the title bar
/// while `expanded` is true, as Main.qml's `flags` expand it while the controller says so.
constexpr auto MainShapedRoot = R"(
import QtQuick
import QtQuick.Controls

ApplicationWindow {
    id: root
    property bool expanded: true
    width: 640
    height: 400
    color: "transparent"
    flags: Qt.Window | (expanded ? Qt.ExpandedClientAreaHint : 0)
    // Main.qml's: under the hint, the safe-area padding is zeroed so the band fills the title bar.
    Binding {
        target: root
        property: "topPadding"
        value: 0
        when: root.expanded
    }
    Rectangle {
        objectName: "band"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: 38
        color: "steelblue"
    }
}
)";

/// The window built from MainShapedRoot, shown and exposed, with the engine that owns its component.
struct ShownRoot
{
    QQmlEngine engine {};
    std::unique_ptr<QQuickWindow> window {};

    ShownRoot()
    {
        auto component = QQmlComponent { &engine };
        component.setData(QByteArray { MainShapedRoot }, QUrl {});
        INFO(component.errorString().toStdString());
        window.reset(qobject_cast<QQuickWindow*>(component.create()));
        REQUIRE(window != nullptr);
        window->show();
        REQUIRE(QTest::qWaitForWindowExposed(window.get()));
    }

    /// @return The NSWindow behind the shown window.
    [[nodiscard]] NSWindow* nsWindow() const
    {
        // Under the cocoa plugin a WId is the NSView*; ARC wants the bridge from an untyped pointer.
        auto* const handle = reinterpret_cast<void*>(window->winId());
        auto* const view = (__bridge NSView*) handle;
        return view.window;
    }

    /// @return Where @p item's top-left sits in the window, in logical pixels.
    [[nodiscard]] static qreal sceneTopOf(QQuickItem const& item) { return item.mapToScene({ 0, 0 }).y(); }
};

[[nodiscard]] bool hasFullSizeContentView(NSWindow* window)
{
    return (window.styleMask & NSWindowStyleMaskFullSizeContentView) != 0;
}

} // namespace

using contour::platform::WindowDecoration;

TEST_CASE("the client band keeps a full-size content view and the shadow", "[display][cocoa][frame]")
{
    REQUIRE_COCOA_OR_SKIP();
    auto root = ShownRoot {};
    auto const frame = contour::platform::makeCocoaWindowFrame();
    REQUIRE(frame != nullptr);

    frame->apply(*root.window, WindowDecoration::Client);

    NSWindow* const nsWindow = root.nsWindow();
    REQUIRE(nsWindow != nil);
    CHECK(hasFullSizeContentView(nsWindow));
    CHECK(nsWindow.titleVisibility == NSWindowTitleHidden);
    CHECK(nsWindow.hasShadow == YES);

    // The hint gives the band the title bar's area -- the bar is the window's safe area now -- and no Quick
    // Controls padding for that safe area takes it back.
    CHECK(root.window->safeAreaMargins().top() > 0);
    auto* const content = root.window->property("contentItem").value<QQuickItem*>();
    REQUIRE(content != nullptr);
    CHECK(ShownRoot::sceneTopOf(*content) == 0);
    auto* const band = root.window->findChild<QQuickItem*>(QStringLiteral("band"));
    REQUIRE(band != nullptr);
    CHECK(ShownRoot::sceneTopOf(*band) == 0);
}

TEST_CASE("the frame leaves the full-size content view to Qt's hint", "[display][cocoa][frame]")
{
    // One owner: without the hint, the client decoration no longer expands the content view by itself.
    REQUIRE_COCOA_OR_SKIP();
    auto root = ShownRoot {};
    root.window->setProperty("expanded", false);
    QCoreApplication::processEvents();
    NSWindow* const nsWindow = root.nsWindow();
    REQUIRE(nsWindow != nil);
    REQUIRE_FALSE(hasFullSizeContentView(nsWindow));

    contour::platform::makeCocoaWindowFrame()->apply(*root.window, WindowDecoration::Client);

    CHECK_FALSE(hasFullSizeContentView(nsWindow));
}

TEST_CASE("the native title bar is an ordinary one", "[display][cocoa][frame]")
{
    REQUIRE_COCOA_OR_SKIP();
    auto root = ShownRoot {};
    auto const frame = contour::platform::makeCocoaWindowFrame();
    REQUIRE(frame != nullptr);
    frame->apply(*root.window, WindowDecoration::Client);

    // As show_title_bar: true does it: the frame goes native, and Main.qml's binding drops the hint.
    frame->apply(*root.window, WindowDecoration::Server);
    root.window->setProperty("expanded", false);
    QCoreApplication::processEvents();

    NSWindow* const nsWindow = root.nsWindow();
    REQUIRE(nsWindow != nil);
    CHECK_FALSE(hasFullSizeContentView(nsWindow));
    CHECK(nsWindow.titleVisibility == NSWindowTitleVisible);
}
