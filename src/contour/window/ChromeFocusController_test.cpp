// SPDX-License-Identifier: Apache-2.0
//
// Keyboard focus in a window's chrome, at the controller: the focus actions, the keys of each area, the
// close question, and the way back to the terminal. Headless: sessions run over MockPty, and the Native2026
// bindings are loaded explicitly because TestApp is Classic.

#include <contour/config/Config.hpp>
#include <contour/session/TerminalSession.hpp>
#include <contour/session/TerminalSessionManager.hpp>
#include <contour/test/GuiTestFixtures.hpp>
#include <contour/window/ChromeFocus.hpp>
#include <contour/window/WindowController.hpp>

#include <vtbackend/input/InputGenerator.hpp>

#include <vtpty/MockPty.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <memory>
#include <string>

#include <QtTest/QSignalSpy>

namespace
{

using contour::window::ChromeArea;

constexpr auto ToolbarItems = 2;

/// A window with three tabs over mock PTYs, the Native2026 bindings loaded, and the chrome shown.
struct ChromeFixture
{
    contour::test::MockPtySessionFactory* factory = nullptr;
    std::unique_ptr<contour::test::TestApp> app;
    std::unique_ptr<contour::test::ScopedController> window;

    ChromeFixture():
        app(makeApp(factory)), window(std::make_unique<contour::test::ScopedController>(app->manager()))
    {
        app->app().config().inputMappings.value() =
            contour::config::defaultInputMappings(contour::config::DefaultsEdition::Native2026);
        [[maybe_unused]] auto const tabs = contour::test::createSessionTabs(app->manager(), **window, 3);
        (*window)->setChromePresence(true, ToolbarItems);
    }

    [[nodiscard]] contour::window::WindowController& controller() const { return **window; }

    [[nodiscard]] ChromeArea area() const { return static_cast<ChromeArea>(controller().chromeFocusArea()); }

    /// Presses @p key with @p modifiers on the active session, as the terminal receives it.
    void pressOnSession(vtbackend::Key key, vtbackend::Modifiers modifiers = {}) const
    {
        controller().activeSession()->sendKeyEvent(key,
                                                   vtbackend::KeyboardModifiers { modifiers },
                                                   vtbackend::KeyboardEventType::Press,
                                                   std::chrono::steady_clock::now());
    }

    /// Offers a Qt key to the chrome.
    [[nodiscard]] bool chromeKey(Qt::Key key, Qt::KeyboardModifiers modifiers = {}) const
    {
        return controller().chromeKey(static_cast<int>(key), static_cast<int>(modifiers.toInt()));
    }

  private:
    static std::unique_ptr<contour::test::TestApp> makeApp(contour::test::MockPtySessionFactory*& observed)
    {
        auto owned = std::make_unique<contour::test::MockPtySessionFactory>();
        observed = owned.get();
        return std::make_unique<contour::test::TestApp>(std::move(owned));
    }
};

} // namespace

TEST_CASE("Ctrl+F6 in the terminal focuses the active tab", "[windowcontroller][chromefocus]")
{
    auto const f = ChromeFixture {};
    REQUIRE(f.area() == ChromeArea::Terminal);

    f.pressOnSession(vtbackend::Key::F6, vtbackend::Modifiers { vtbackend::Modifier::Control });

    CHECK(f.area() == ChromeArea::Tabs);
    CHECK(f.controller().focusedTabIndex() == f.controller().activeTabIndex());
}

TEST_CASE("keys move, activate and reorder in the tab well", "[windowcontroller][chromefocus]")
{
    auto const f = ChromeFixture {};
    auto& controller = f.controller();
    controller.activateTab(1);
    controller.moveChromeFocus(contour::window::ChromeFocusAction::Next);
    REQUIRE(f.area() == ChromeArea::Tabs);
    REQUIRE(controller.focusedTabIndex() == 1);

    CHECK(f.chromeKey(Qt::Key_Right));
    CHECK(controller.focusedTabIndex() == 2);
    CHECK(controller.activeTabIndex() == 1); // moving focus activates nothing

    CHECK(f.chromeKey(Qt::Key_Return));
    CHECK(controller.activeTabIndex() == 2);

    // Alt+Left reorders the focused tab and keeps focus on it.
    CHECK(f.chromeKey(Qt::Key_Left, Qt::AltModifier));
    CHECK(controller.focusedTabIndex() == 1);
    CHECK(controller.activeTabIndex() == 1);
}

TEST_CASE("reordering at either end does nothing", "[windowcontroller][chromefocus]")
{
    auto const f = ChromeFixture {};
    auto& controller = f.controller();
    controller.activateTab(0);
    controller.moveChromeFocus(contour::window::ChromeFocusAction::Next);
    REQUIRE(controller.focusedTabIndex() == 0);

    CHECK(f.chromeKey(Qt::Key_Left, Qt::AltModifier));
    CHECK(controller.focusedTabIndex() == 0);
    CHECK(controller.activeTabIndex() == 0);

    CHECK(f.chromeKey(Qt::Key_Left)); // no wrap either
    CHECK(controller.focusedTabIndex() == 0);
}

TEST_CASE("Ctrl+F6 cycles from inside the chrome", "[windowcontroller][chromefocus]")
{
    auto const f = ChromeFixture {};
    f.controller().moveChromeFocus(contour::window::ChromeFocusAction::Next);
    REQUIRE(f.area() == ChromeArea::Tabs);

    CHECK(f.chromeKey(Qt::Key_F6, Qt::ControlModifier));
    CHECK(f.area() == ChromeArea::Toolbar);
    CHECK(f.controller().focusedToolbarIndex() == 0);

    CHECK(f.chromeKey(Qt::Key_F6, Qt::ControlModifier));
    CHECK(f.area() == ChromeArea::Terminal);
    CHECK(f.controller().focusedTabIndex() == -1);
}

TEST_CASE("Escape and leaveChrome return focus to the terminal", "[windowcontroller][chromefocus]")
{
    auto const f = ChromeFixture {};
    auto& controller = f.controller();

    controller.moveChromeFocus(contour::window::ChromeFocusAction::Next);
    REQUIRE(f.area() == ChromeArea::Tabs);
    CHECK(f.chromeKey(Qt::Key_Escape));
    CHECK(f.area() == ChromeArea::Terminal);

    controller.moveChromeFocus(contour::window::ChromeFocusAction::Toolbar);
    REQUIRE(f.area() == ChromeArea::Toolbar);
    controller.leaveChrome();
    CHECK(f.area() == ChromeArea::Terminal);

    // With the terminal focused, the chrome keys are not its to answer.
    CHECK_FALSE(f.chromeKey(Qt::Key_Escape));
}

TEST_CASE("the tab well closing under focus returns it to the terminal", "[windowcontroller][chromefocus]")
{
    auto const f = ChromeFixture {};
    f.controller().moveChromeFocus(contour::window::ChromeFocusAction::Next);
    REQUIRE(f.area() == ChromeArea::Tabs);

    f.controller().setChromePresence(false, ToolbarItems);

    CHECK(f.area() == ChromeArea::Terminal);
}

TEST_CASE("Backspace on an idle tab closes it at once", "[windowcontroller][chromefocus]")
{
    auto const f = ChromeFixture {};
    auto& controller = f.controller();
    auto asked =
        QSignalSpy { &controller, &contour::window::WindowController::closeTabConfirmationRequested };
    controller.moveChromeFocus(contour::window::ChromeFocusAction::Next);
    REQUIRE(f.area() == ChromeArea::Tabs);
    REQUIRE(controller.count() == 3);

    // A MockPty names no foreground process: the pane's activity is Unknown, which never asks.
    CHECK(f.chromeKey(Qt::Key_Backspace));

    // The model prunes the tab asynchronously, through the session's exit watcher, which headless sessions
    // never start; the synchronous contract is that the focused tab's PTY is closed and no other is.
    REQUIRE(f.factory->createdPtys.size() == 3);
    CHECK(f.factory->createdPtys[2]->isClosed());
    CHECK_FALSE(f.factory->createdPtys[0]->isClosed());
    CHECK_FALSE(f.factory->createdPtys[1]->isClosed());
    CHECK(asked.isEmpty());
}

TEST_CASE("Backspace on a tab running a program asks first", "[windowcontroller][chromefocus]")
{
    auto const f = ChromeFixture {};
    auto& controller = f.controller();
    auto asked =
        QSignalSpy { &controller, &contour::window::WindowController::closeTabConfirmationRequested };
    controller.activateTab(0);
    controller.moveChromeFocus(contour::window::ChromeFocusAction::Next);
    REQUIRE(controller.focusedTabIndex() == 0);
    REQUIRE(f.factory->createdPtys.size() == 3);
    f.factory->createdPtys[0]->setForegroundProcessName("make");

    CHECK(f.chromeKey(Qt::Key_Backspace));

    // Nothing closed, focus stays, and the question names the row and the program.
    REQUIRE(asked.count() == 1);
    CHECK(asked.first().at(0).toInt() == 0);
    CHECK(asked.first().at(2).toString() == QStringLiteral("make"));
    CHECK_FALSE(f.factory->createdPtys[0]->isClosed());
    CHECK(f.area() == ChromeArea::Tabs);

    controller.confirmCloseTab(0);
    CHECK(f.factory->createdPtys[0]->isClosed());
}

TEST_CASE("a bare F6 is written to the PTY", "[windowcontroller][chromefocus]")
{
    auto const f = ChromeFixture {};

    f.pressOnSession(vtbackend::Key::F6);

    CHECK(contour::test::mockPtyOf(*f.controller().activeSession()).stdinBuffer() == "\x1b[17~");
    CHECK(f.area() == ChromeArea::Terminal);
}
