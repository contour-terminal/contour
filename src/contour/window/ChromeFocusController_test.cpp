// SPDX-License-Identifier: Apache-2.0
//
// Keyboard focus in a window's chrome, at the controller: the focus actions, the keys of each area, the
// close question, and the way back to the terminal. Headless: sessions run over MockPty, and the Native2026
// bindings are loaded explicitly because TestApp is Classic.

#include <contour/config/ConfigFwd.hpp>
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
#include <optional>
#include <string>
#include <vector>

#include <QtTest/QSignalSpy>
#include <vtworkspace/SessionModel.hpp>

namespace
{

using contour::window::ChromeArea;

constexpr auto ToolbarItems = 2;
constexpr auto DefaultTabCount = 3;

/// A window with three tabs over mock PTYs, the Native2026 bindings loaded, and the chrome shown.
struct ChromeFixture
{
    contour::test::MockPtySessionFactory* factory = nullptr;
    std::unique_ptr<contour::test::TestApp> app;
    std::unique_ptr<contour::test::ScopedController> window;
    contour::test::SessionTabs tabs; ///< The tabs the window was opened with, in row order.

    /// @param extraBindings Adds bindings to the Native2026 defaults before the sessions copy the config.
    explicit ChromeFixture(void (*extraBindings)(contour::config::InputMappings&) = nullptr,
                           int tabCount = DefaultTabCount):
        app(makeApp(factory)), window(std::make_unique<contour::test::ScopedController>(app->manager()))
    {
        // Built afresh and moved in: copy-assigning the cached defaults instantiates the Action variant's
        // copy assignment, for which GCC at -O2 -g needs some 7 GB more memory and six times the time.
        app->app().config().inputMappings.value() =
            contour::config::makeDefaultInputMappings(contour::config::DefaultsEdition::Native2026);
        if (extraBindings != nullptr)
            extraBindings(app->app().config().inputMappings.value());
        tabs = contour::test::createSessionTabs(app->manager(), **window, tabCount);
        (*window)->setChromePresence(true, ToolbarItems);
    }

    [[nodiscard]] contour::window::WindowController& controller() const { return **window; }

    /// Closes the tab @p tab in the model at once, as the daemon does when a tab ends (closeTabAtIndex()
    /// waits for the session's exit watcher, which headless sessions never start).
    void closeNow(vtworkspace::TabId tab) const
    {
        app->manager().model().closeTab(controller().windowId(), tab);
    }

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

    // Return activates the tab, and the keyboard goes back to the terminal.
    CHECK(f.chromeKey(Qt::Key_Return));
    CHECK(controller.activeTabIndex() == 2);
    CHECK(f.area() == ChromeArea::Terminal);

    // Alt+Left reorders the focused tab and keeps focus on it.
    controller.moveChromeFocus(contour::window::ChromeFocusAction::Next);
    REQUIRE(controller.focusedTabIndex() == 2);
    REQUIRE(f.tabs.tabs.size() == 3);
    CHECK(f.chromeKey(Qt::Key_Left, Qt::AltModifier));
    CHECK(f.area() == ChromeArea::Tabs);
    CHECK(controller.focusedTabIndex() == 1);
    CHECK(controller.activeTabIndex() == 1);
    // The tab that moved is the one the keyboard is still on, and the one it left behind took its row.
    CHECK(controller.focusedTab() == f.tabs.tabs[2]);
    CHECK(f.app->manager().model().window(controller.windowId())->indexOf(f.tabs.tabs[1]) == 2);
}

TEST_CASE("the focus stays on its tab when another tab is reordered or closed",
          "[windowcontroller][chromefocus]")
{
    auto const f = ChromeFixture {};
    auto& controller = f.controller();
    controller.activateTab(1);
    controller.moveChromeFocus(contour::window::ChromeFocusAction::Next);
    REQUIRE(controller.focusedTabIndex() == 1);

    SECTION("a tab moves from before it to after it")
    {
        controller.moveTab(0, 2);
        CHECK(f.area() == ChromeArea::Tabs);
        CHECK(controller.focusedTabIndex() == 0);
    }

    SECTION("a tab moves from after it to before it")
    {
        controller.moveTab(2, 0);
        CHECK(f.area() == ChromeArea::Tabs);
        CHECK(controller.focusedTabIndex() == 2);
    }
}

/// Five tabs with the keyboard on the third: tabs on both sides of it, so a close before it must shift the
/// row and a close after it must not, and no clamp to the last row can hide a missed shift.
TEST_CASE("the focus stays on its tab when the tabs around it close", "[windowcontroller][chromefocus]")
{
    constexpr auto Tabs = 5;
    constexpr auto Focused = 2;
    auto const f = ChromeFixture { nullptr, Tabs };
    auto& controller = f.controller();
    REQUIRE(f.tabs.tabs.size() == Tabs);
    controller.activateTab(Focused);
    controller.moveChromeFocus(contour::window::ChromeFocusAction::Next);
    REQUIRE(controller.focusedTabIndex() == Focused);

    SECTION("a tab before it closes")
    {
        f.closeNow(f.tabs.tabs[0]);
        CHECK(controller.count() == Tabs - 1);
        CHECK(f.area() == ChromeArea::Tabs);
        CHECK(controller.focusedTabIndex() == Focused - 1);
        CHECK(controller.focusedTab() == f.tabs.tabs[Focused]);
    }

    SECTION("a tab after it closes")
    {
        f.closeNow(f.tabs.tabs[Tabs - 1]);
        CHECK(controller.count() == Tabs - 1);
        CHECK(f.area() == ChromeArea::Tabs);
        CHECK(controller.focusedTabIndex() == Focused);
        CHECK(controller.focusedTab() == f.tabs.tabs[Focused]);
    }

    SECTION("the tabs before it close one by one")
    {
        f.closeNow(f.tabs.tabs[0]);
        f.closeNow(f.tabs.tabs[1]);
        CHECK(controller.count() == Tabs - 2);
        CHECK(f.area() == ChromeArea::Tabs);
        CHECK(controller.focusedTabIndex() == 0);
        CHECK(controller.focusedTab() == f.tabs.tabs[Focused]);
    }

    SECTION("every other tab closes")
    {
        controller.closeOtherTabs(Focused);
        CHECK(controller.count() == 1);
        CHECK(f.area() == ChromeArea::Tabs);
        CHECK(controller.focusedTabIndex() == 0);
        CHECK(controller.focusedTab() == f.tabs.tabs[Focused]);
    }
}

TEST_CASE("a tab inserted before the focused one leaves the focus on its tab",
          "[windowcontroller][chromefocus]")
{
    auto const f = ChromeFixture {};
    auto& controller = f.controller();
    auto other = contour::test::ScopedController { f.app->manager() };
    other->createNewTab();
    auto const incoming = other->activeTabIndex();
    REQUIRE(other->count() == 1);

    controller.activateTab(2);
    controller.moveChromeFocus(contour::window::ChromeFocusAction::Next);
    REQUIRE(controller.focusedTabIndex() == 2);
    REQUIRE(f.tabs.tabs.size() == 3);

    // What the focus names each time the window announces it: the model activates a tab it takes in, which
    // moves the keyboard to it (the next test), so the insert's own announcement is the one to watch.
    auto announced = std::vector<std::optional<vtworkspace::TabId>> {};
    auto const connection = QObject::connect(&controller,
                                             &contour::window::WindowController::chromeFocusChanged,
                                             &controller,
                                             [&] { announced.push_back(controller.focusedTab()); });

    // From another window, at row 0: the tab under the focus is now on row 3.
    controller.moveTabIntoThisWindow(other.id.value, incoming, 0);
    REQUIRE(controller.count() == 4);
    REQUIRE_FALSE(announced.empty());

    CHECK(announced.front() == f.tabs.tabs[2]);
    CHECK(f.app->manager().model().window(controller.windowId())->indexOf(f.tabs.tabs[2]) == 3);
    QObject::disconnect(connection);
}

TEST_CASE("activating a tab from outside the tab well moves the keyboard to it",
          "[windowcontroller][chromefocus]")
{
    auto const f = ChromeFixture {};
    auto& controller = f.controller();
    controller.activateTab(0);
    controller.moveChromeFocus(contour::window::ChromeFocusAction::Next);
    REQUIRE(controller.focusedTabIndex() == 0);

    // What the native menu, the palette and the daemon do: activate, with no key pressed in the chrome.
    controller.activateTab(2);

    CHECK(f.area() == ChromeArea::Tabs);
    CHECK(controller.focusedTabIndex() == 2);
    REQUIRE(f.tabs.tabs.size() == 3);
    CHECK(controller.focusedTab() == f.tabs.tabs[2]);
}

TEST_CASE("closing the focused tab ends the chrome focus", "[windowcontroller][chromefocus]")
{
    auto const f = ChromeFixture {};
    auto& controller = f.controller();
    controller.activateTab(0);
    controller.moveChromeFocus(contour::window::ChromeFocusAction::Next);
    REQUIRE(f.area() == ChromeArea::Tabs);
    REQUIRE(controller.focusedTabIndex() == 0);
    REQUIRE(f.chromeKey(Qt::Key_Right));
    REQUIRE(controller.focusedTabIndex() == 1);

    // Closes the tabs after row 0, the focused one among them.
    controller.closeTabsToRight(0);

    CHECK(controller.count() == 1);
    CHECK(f.area() == ChromeArea::Terminal);
    CHECK(controller.focusedTabIndex() == -1);
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

    // The foreground process is the pane's shell, so the pane is known to be idle.
    f.factory->createdPtys[2]->setForegroundProcessName(
        controller.activeSession()->profile().shell.value().program);
    CHECK(f.chromeKey(Qt::Key_Backspace));

    // The model prunes the tab asynchronously, through the session's exit watcher, which headless sessions
    // never start; the synchronous contract is that the focused tab's PTY is closed and no other is.
    REQUIRE(f.factory->createdPtys.size() == 3);
    CHECK(f.factory->createdPtys[2]->isClosed());
    CHECK_FALSE(f.factory->createdPtys[0]->isClosed());
    CHECK_FALSE(f.factory->createdPtys[1]->isClosed());
    CHECK(asked.isEmpty());
}

TEST_CASE("Backspace on a tab whose program cannot be known asks first", "[windowcontroller][chromefocus]")
{
    auto const f = ChromeFixture {};
    auto& controller = f.controller();
    auto asked =
        QSignalSpy { &controller, &contour::window::WindowController::closeTabConfirmationRequested };
    controller.moveChromeFocus(contour::window::ChromeFocusAction::Next);
    REQUIRE(f.area() == ChromeArea::Tabs);
    REQUIRE(f.factory->createdPtys.size() == 3);

    // A MockPty names no foreground process, as a daemon-hosted pane does: Unknown fails toward asking.
    CHECK(f.chromeKey(Qt::Key_Backspace));

    CHECK(asked.count() == 1);
    CHECK_FALSE(f.factory->createdPtys[2]->isClosed());
    CHECK(f.area() == ChromeArea::Tabs);
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

    // Nothing closed, focus stays, and the question names the tab and the program.
    REQUIRE(asked.count() == 1);
    auto const* tab = f.app->manager().model().window(controller.windowId())->tabAt(0);
    REQUIRE(tab != nullptr);
    CHECK(asked.first().at(0).toULongLong() == tab->id().value);
    CHECK(asked.first().at(2).toString() == QStringLiteral("make"));
    CHECK_FALSE(f.factory->createdPtys[0]->isClosed());
    CHECK(f.area() == ChromeArea::Tabs);

    controller.confirmCloseTab(tab->id().value);
    CHECK(f.factory->createdPtys[0]->isClosed());
}

TEST_CASE("a confirmed close reaches the tab that was asked about, not the row it had",
          "[windowcontroller][chromefocus]")
{
    auto const f = ChromeFixture {};
    auto& controller = f.controller();
    auto asked =
        QSignalSpy { &controller, &contour::window::WindowController::closeTabConfirmationRequested };
    auto withdrawn =
        QSignalSpy { &controller, &contour::window::WindowController::closeTabConfirmationWithdrawn };
    controller.activateTab(1);
    controller.moveChromeFocus(contour::window::ChromeFocusAction::Next);
    REQUIRE(controller.focusedTabIndex() == 1);
    REQUIRE(f.chromeKey(Qt::Key_Backspace));
    REQUIRE(asked.count() == 1);
    auto const askedAbout = asked.first().at(0).toULongLong();

    SECTION("a tab before it closes while the question is open")
    {
        auto& model = f.app->manager().model();
        auto const* first = f.app->manager().model().window(controller.windowId())->tabAt(0);
        REQUIRE(first != nullptr);
        auto const firstId = first->id();
        model.closeTab(controller.windowId(), firstId);
        REQUIRE(controller.count() == 2);

        controller.confirmCloseTab(askedAbout);

        // The asked-about tab moved up to row 0; the one that took row 1 is the third, and it survives.
        CHECK(f.factory->createdPtys[1]->isClosed());
        CHECK_FALSE(f.factory->createdPtys[2]->isClosed());
        // Only the closed tab's own question is withdrawn, and that one was not asked about.
        REQUIRE(withdrawn.count() == 1);
        CHECK(withdrawn.first().at(0).toULongLong() == firstId.value);
        CHECK(withdrawn.first().at(0).toULongLong() != askedAbout);
    }

    SECTION("the tab asked about goes while the question is open")
    {
        auto& model = f.app->manager().model();
        model.closeTab(controller.windowId(), vtworkspace::TabId { askedAbout });
        REQUIRE(controller.count() == 2);
        REQUIRE(withdrawn.count() == 1);
        CHECK(withdrawn.first().at(0).toULongLong() == askedAbout);

        controller.confirmCloseTab(askedAbout);

        CHECK_FALSE(f.factory->createdPtys[0]->isClosed());
        CHECK_FALSE(f.factory->createdPtys[2]->isClosed());
    }
}

TEST_CASE("a rebound focus chord, on a key that is not F1..F12, cycles from inside the chrome",
          "[windowcontroller][chromefocus]")
{
    using contour::config::CharInputMapping;
    using contour::config::KeyInputMapping;
    using vtbackend::Modifier;
    using vtbackend::Modifiers;

    auto const f = ChromeFixture { [](contour::config::InputMappings& mappings) {
        // A named key that is not a function key, a shifted Tab, and a character: none of them F1..F12.
        mappings.keyMappings.push_back(KeyInputMapping { .modes {},
                                                         .modifiers = Modifiers { Modifier::Alt },
                                                         .input = vtbackend::Key::Home,
                                                         .binding = { contour::actions::FocusNextArea {} } });
        mappings.keyMappings.push_back(
            KeyInputMapping { .modes {},
                              .modifiers = Modifiers { Modifier::Alt, Modifier::Shift },
                              .input = vtbackend::Key::Tab,
                              .binding = { contour::actions::FocusPreviousArea {} } });
        mappings.charMappings.push_back(
            CharInputMapping { .modes {},
                               .modifiers = Modifiers { Modifier::Alt },
                               .input = U'J',
                               .binding = { contour::actions::FocusToolbar {} } });
    } };
    auto& controller = f.controller();

    // From inside the tab well, each chord is a focus action and the chrome consumes it.
    controller.moveChromeFocus(contour::window::ChromeFocusAction::Next);
    REQUIRE(f.area() == ChromeArea::Tabs);
    CHECK(f.chromeKey(Qt::Key_Home, Qt::AltModifier));
    CHECK(f.area() == ChromeArea::Toolbar);
    CHECK(f.chromeKey(Qt::Key_Backtab, Qt::AltModifier | Qt::ShiftModifier));
    CHECK(f.area() == ChromeArea::Tabs);
    CHECK(f.chromeKey(Qt::Key_J, Qt::AltModifier));
    CHECK(f.area() == ChromeArea::Toolbar);

    // A chord bound to nothing is not the chrome's to take.
    CHECK_FALSE(f.chromeKey(Qt::Key_K, Qt::AltModifier));
}

TEST_CASE("Ctrl+F6 reaches the program when the window shows no chrome", "[windowcontroller][chromefocus]")
{
    auto const f = ChromeFixture {};
    f.controller().setChromePresence(false, 0);

    f.pressOnSession(vtbackend::Key::F6, vtbackend::Modifiers { vtbackend::Modifier::Control });

    // Nowhere to move focus to: the key is the terminal's, as an unbound one is.
    CHECK(f.area() == ChromeArea::Terminal);
    CHECK(contour::test::mockPtyOf(*f.controller().activeSession()).stdinBuffer() == "\x1b[17;5~");
}

TEST_CASE("a bare F6 is written to the PTY", "[windowcontroller][chromefocus]")
{
    auto const f = ChromeFixture {};

    f.pressOnSession(vtbackend::Key::F6);

    CHECK(contour::test::mockPtyOf(*f.controller().activeSession()).stdinBuffer() == "\x1b[17~");
    CHECK(f.area() == ChromeArea::Terminal);
}

TEST_CASE("an application binding works while the chrome has focus", "[windowcontroller][chromefocus]")
{
    auto const f = ChromeFixture {};
    auto& controller = f.controller();
    auto palette = QSignalSpy { &controller, &contour::window::WindowController::commandPaletteRequested };
    controller.moveChromeFocus(contour::window::ChromeFocusAction::Next);
    REQUIRE(f.area() == ChromeArea::Tabs);

    SECTION("Ctrl+Shift+P opens the command palette, and the chrome keeps its focus")
    {
        CHECK(f.chromeKey(Qt::Key_P, Qt::ControlModifier | Qt::ShiftModifier));
        CHECK(palette.count() == 1);
        CHECK(f.area() == ChromeArea::Tabs);
    }

    SECTION("an arrow key is the chrome's and opens nothing")
    {
        CHECK(f.chromeKey(Qt::Key_Right));
        CHECK(palette.count() == 0);
    }

    SECTION("plain typing is neither handled nor bound")
    {
        CHECK_FALSE(f.chromeKey(Qt::Key_P));
        CHECK(palette.count() == 0);
    }
}

TEST_CASE("a chord on a key the chrome consumes still runs a binding that names it with Ctrl",
          "[windowcontroller][chromefocus]")
{
    auto const f = ChromeFixture { [](contour::config::InputMappings& mappings) {
        using contour::config::KeyInputMapping;
        using vtbackend::Modifier;
        using vtbackend::Modifiers;
        mappings.keyMappings.push_back(
            KeyInputMapping { .modes {},
                              .modifiers = Modifiers { Modifier::Control },
                              .input = vtbackend::Key::RightArrow,
                              .binding = { contour::actions::OpenCommandPalette {} } });
    } };
    auto palette =
        QSignalSpy { &f.controller(), &contour::window::WindowController::commandPaletteRequested };
    f.controller().moveChromeFocus(contour::window::ChromeFocusAction::Next);

    CHECK(f.chromeKey(Qt::Key_Right, Qt::ControlModifier));
    CHECK(palette.count() == 1);
}
