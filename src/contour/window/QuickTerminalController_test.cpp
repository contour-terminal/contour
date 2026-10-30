// SPDX-License-Identifier: Apache-2.0
//
// The quick terminal controller against plain QWindows on the offscreen platform: the hotkey, the panel and
// the screen are doubles, the window and its animation are real.

#include <contour/test/HotkeyFixtures.hpp>
#include <contour/test/QuickTerminalFixtures.hpp>
#include <contour/window/QuickTerminalController.hpp>
#include <contour/window/WindowLivenessQt.hpp>

#include <QtCore/QCoreApplication>
#include <QtGui/QGuiApplication>
#include <QtGui/QWindow>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <QtTest/QTest>

using contour::config::HotkeyChord;
using contour::config::MotionStyle;
using contour::config::MotionTokens;
using contour::config::QuickTerminalConfig;
using contour::config::QuickTerminalFocusLoss;
using contour::platform::HotkeyError;
using contour::test::RecordingGlobalHotkey;
using contour::test::RecordingQuickTerminalPanel;
using contour::window::PlacementControl;
using contour::window::QuickTerminalController;
using contour::window::QuickTerminalHost;
using contour::window::QuickTerminalState;
using contour::window::ScreenRect;

namespace
{

constexpr auto ControlGrave = HotkeyChord { .modifiers = vtbackend::Modifier::Control, .key = U'`' };
constexpr auto AltSpace = HotkeyChord { .modifiers = vtbackend::Modifier::Alt, .key = U' ' };
constexpr auto TheScreen = ScreenRect { .x = 0, .y = 24, .width = 800, .height = 600 };
constexpr auto WaitMilliseconds = 3000;
constexpr auto CornerRadius = 14;

/// A short, full-motion animation, so a case waits milliseconds rather than a design token's 220.
[[nodiscard]] MotionTokens quickMotion(MotionStyle style = MotionStyle::Full)
{
    auto motion = MotionTokens {};
    motion.emphasizedMs = 40;
    motion.style = style;
    return motion;
}

/// A controller over recording doubles and plain windows.
struct Fixture
{
    RecordingGlobalHotkey hotkey {};
    RecordingQuickTerminalPanel panel {};
    std::vector<std::unique_ptr<QWindow>> windows {};
    std::vector<HotkeyError> errors {};
    int bounds = 0;
    int offScreens = 0;
    std::vector<HotkeyChord> menuOwned {};
    std::vector<ScreenRect> others {};
    std::unique_ptr<QuickTerminalController> controller;

    explicit Fixture(QuickTerminalConfig config = { .hotkey = ControlGrave },
                     MotionTokens motion = quickMotion(),
                     PlacementControl placement = PlacementControl::Client)
    {
        controller = std::make_unique<QuickTerminalController>(
            QuickTerminalHost {
                .hotkey = hotkey,
                .panel = panel,
                .createWindow = [this]() -> QWindow* {
                    windows.push_back(std::make_unique<QWindow>());
                    // As the app's quick window is flagged: a framed one gets the offscreen platform's
                    // margins.
                    windows.back()->setFlags(Qt::Tool | Qt::FramelessWindowHint);
                    return windows.back().get();
                },
                .screenUnderPointer = [] { return TheScreen; },
                .otherScreens = [this] { return others; },
                .onHotkeyError = [this](HotkeyError error) { errors.push_back(error); },
                .onHotkeyBound = [this] { ++bounds; },
                .onOffScreen = [this] { ++offScreens; },
                .isMenuOwned =
                    [this](HotkeyChord const& chord) { return std::ranges::contains(menuOwned, chord); },
                .cornerRadius = [] { return CornerRadius; },
                .placement = placement },
            config,
            motion);
    }

    /// Pumps events until the controller reaches @p state.
    [[nodiscard]] bool waitFor(QuickTerminalState state) const
    {
        return QTest::qWaitFor([&] { return controller->state() == state; }, WaitMilliseconds);
    }
};

} // namespace

TEST_CASE("QuickTerminalController.theHotkeyCreatesOneWindowAtTheTop", "[quickterminal][controller]")
{
    auto fixture = Fixture {};
    REQUIRE(fixture.controller->state() == QuickTerminalState::Absent);
    CHECK(fixture.hotkey.registered == std::vector { ControlGrave });

    fixture.hotkey.press("tok");
    CHECK(fixture.controller->state() == QuickTerminalState::Showing);
    REQUIRE(fixture.windows.size() == 1);
    CHECK(fixture.panel.adopts == 1);
    CHECK(fixture.panel.presents == std::vector<std::string> { "tok" });

    REQUIRE(fixture.waitFor(QuickTerminalState::Shown));
    auto const shown = contour::window::quickTerminalFrames(TheScreen).shown;
    auto const geometry = fixture.windows.front()->geometry();
    CHECK(geometry.x() == shown.x);
    CHECK(geometry.y() == shown.y);
    CHECK(geometry.width() == shown.width);
    CHECK(geometry.height() == shown.height);
    CHECK(shown.height == 240); // 40 percent of the 600 available.

    // A press while it is shown and has the focus hides it, and the window stays for the next show.
    REQUIRE(QTest::qWaitFor([&] { return fixture.windows.front()->isActive(); }, WaitMilliseconds));
    fixture.hotkey.press();
    CHECK(fixture.controller->state() == QuickTerminalState::Hiding);
    REQUIRE(fixture.waitFor(QuickTerminalState::Hidden));
    CHECK(fixture.panel.dismisses == 1);
    CHECK(fixture.windows.size() == 1);

    fixture.hotkey.press();
    CHECK(fixture.controller->state() == QuickTerminalState::Showing);
    CHECK(fixture.windows.size() == 1);
}

TEST_CASE("QuickTerminalController.aPressDuringTheSlideReversesIt", "[quickterminal][controller]")
{
    auto motion = quickMotion();
    motion.emphasizedMs = 400; // Long enough to press in the middle of.
    auto fixture = Fixture { { .hotkey = ControlGrave }, motion };
    fixture.hotkey.press();
    REQUIRE(fixture.controller->state() == QuickTerminalState::Showing);
    QTest::qWait(150);
    auto const midway = fixture.windows.front()->y();
    CHECK(midway > contour::window::quickTerminalFrames(TheScreen).hidden.y);

    fixture.hotkey.press();
    CHECK(fixture.controller->state() == QuickTerminalState::Hiding);
    REQUIRE(fixture.waitFor(QuickTerminalState::Hidden));
    // It went back to where it came from, rather than finishing the slide down first.
    CHECK(fixture.windows.front()->y() == contour::window::quickTerminalFrames(TheScreen).hidden.y);

    fixture.hotkey.press();
    CHECK(fixture.controller->state() == QuickTerminalState::Showing);
    fixture.hotkey.press();
    CHECK(fixture.controller->state() == QuickTerminalState::Hiding);
}

TEST_CASE("QuickTerminalController.focusLossHidesItUnlessConfiguredToStay", "[quickterminal][controller]")
{
    struct Row
    {
        QuickTerminalFocusLoss rule;
        QuickTerminalState expected;
    };
    for (auto const& row:
         { Row { .rule = QuickTerminalFocusLoss::Hide, .expected = QuickTerminalState::Hidden },
           Row { .rule = QuickTerminalFocusLoss::Stay, .expected = QuickTerminalState::Shown } })
    {
        auto fixture = Fixture { { .hotkey = ControlGrave, .focusLoss = row.rule } };
        fixture.hotkey.press();
        REQUIRE(fixture.waitFor(QuickTerminalState::Shown));
        REQUIRE(QTest::qWaitFor([&] { return fixture.windows.front()->isActive(); }, WaitMilliseconds));

        // Another window takes the focus, as a click on another application would.
        auto other = QWindow {};
        other.setGeometry(500, 500, 100, 100);
        other.show();
        other.requestActivate();
        REQUIRE(QTest::qWaitFor([&] { return !fixture.windows.front()->isActive(); }, WaitMilliseconds));

        if (row.rule == QuickTerminalFocusLoss::Hide)
            REQUIRE(fixture.waitFor(QuickTerminalState::Hidden));
        else
            QTest::qWait(100);
        CHECK(fixture.controller->state() == row.expected);

        if (row.rule == QuickTerminalFocusLoss::Stay)
        {
            // The hotkey, pressed while it is shown without the focus, focuses it rather than hiding it.
            auto const presentsBefore = fixture.panel.presents.size();
            fixture.hotkey.press("again");
            CHECK(fixture.controller->state() == QuickTerminalState::Shown);
            REQUIRE(fixture.panel.presents.size() == presentsBefore + 1);
            CHECK(fixture.panel.presents.back() == "again");
        }
    }
}

TEST_CASE("QuickTerminalController.closingItsLastTabEndsItAndTheNextPressMakesANewOne",
          "[quickterminal][controller]")
{
    auto fixture = Fixture {};
    fixture.hotkey.press();
    REQUIRE(fixture.waitFor(QuickTerminalState::Shown));

    fixture.windows.front().reset(); // The QML root goes with the last tab.
    CHECK(fixture.controller->state() == QuickTerminalState::Absent);

    fixture.hotkey.press();
    CHECK(fixture.controller->state() == QuickTerminalState::Showing);
    CHECK(fixture.windows.size() == 2);
    CHECK(fixture.windows.back() != nullptr);
}

TEST_CASE("QuickTerminalController.anUnchangedChordIsNotRegisteredAgain", "[quickterminal][controller]")
{
    auto fixture = Fixture {};
    REQUIRE(fixture.hotkey.registered.size() == 1);

    fixture.controller->reconfigure({ .hotkey = ControlGrave, .focusLoss = QuickTerminalFocusLoss::Stay },
                                    quickMotion());
    CHECK(fixture.hotkey.registered.size() == 1);
    CHECK(fixture.hotkey.unregisters == 0);

    fixture.controller->reconfigure({ .hotkey = AltSpace }, quickMotion());
    CHECK(fixture.hotkey.unregisters == 0); // Registering replaces, as the interface says.
    REQUIRE(fixture.hotkey.registered.size() == 2);
    CHECK(fixture.hotkey.registered.back() == AltSpace);

    fixture.controller->reconfigure({}, quickMotion());
    CHECK(fixture.hotkey.unregisters == 1);
    CHECK(fixture.hotkey.registered.size() == 2);
}

TEST_CASE("QuickTerminalController.aRefusedHotkeyIsReported", "[quickterminal][controller]")
{
    auto hotkey = RecordingGlobalHotkey {};
    hotkey.result = std::unexpected(HotkeyError::AlreadyTaken);
    auto panel = RecordingQuickTerminalPanel {};
    auto errors = std::vector<HotkeyError> {};
    auto const controller = QuickTerminalController {
        QuickTerminalHost { .hotkey = hotkey,
                            .panel = panel,
                            .createWindow = [] { return static_cast<QWindow*>(nullptr); },
                            .screenUnderPointer = [] { return TheScreen; },
                            .otherScreens = {},
                            .onHotkeyError = [&](HotkeyError error) { errors.push_back(error); },
                            .onHotkeyBound = {},
                            .onOffScreen = {},
                            .isMenuOwned = {},
                            .cornerRadius = {},
                            .placement = PlacementControl::Client },
        { .hotkey = ControlGrave },
        quickMotion()
    };
    CHECK(errors == std::vector { HotkeyError::AlreadyTaken });
    CHECK(hotkey.registered.empty());
}

TEST_CASE("QuickTerminalController.aMenuOwnedChordIsRefusedBeforeTheOsIsAsked", "[quickterminal][controller]")
{
    auto hotkey = RecordingGlobalHotkey {};
    auto panel = RecordingQuickTerminalPanel {};
    auto errors = std::vector<HotkeyError> {};
    auto const quit = HotkeyChord { .modifiers = vtbackend::Modifier::Super, .key = U'q' };
    auto const controller = QuickTerminalController {
        QuickTerminalHost { .hotkey = hotkey,
                            .panel = panel,
                            .createWindow = [] { return static_cast<QWindow*>(nullptr); },
                            .screenUnderPointer = [] { return TheScreen; },
                            .otherScreens = {},
                            .onHotkeyError = [&](HotkeyError error) { errors.push_back(error); },
                            .onHotkeyBound = {},
                            .onOffScreen = {},
                            .isMenuOwned = [&](HotkeyChord const& chord) { return chord == quit; },
                            .cornerRadius = {},
                            .placement = PlacementControl::Client },
        { .hotkey = quit },
        quickMotion()
    };
    CHECK(errors == std::vector { HotkeyError::MenuOwned });
    CHECK(hotkey.registered.empty());
}

TEST_CASE("QuickTerminalController.underWaylandOrReducedMotionItFadesInPlace", "[quickterminal][controller]")
{
    struct Row
    {
        MotionStyle style;
        PlacementControl placement;
    };
    for (auto const& row: { Row { .style = MotionStyle::Full, .placement = PlacementControl::Compositor },
                            Row { .style = MotionStyle::CrossFade, .placement = PlacementControl::Client } })
    {
        auto motion = quickMotion(row.style);
        motion.emphasizedMs = 400; // Long enough to look at the window in the middle of.
        auto fixture = Fixture { { .hotkey = ControlGrave }, motion, row.placement };
        fixture.hotkey.press();
        REQUIRE(fixture.controller->state() == QuickTerminalState::Showing);
        QTest::qWait(150);
        auto const shown = contour::window::quickTerminalFrames(TheScreen).shown;
        // In the middle of it the window has not travelled -- its frame is the shown one -- and is only
        // part way opaque.
        CHECK(fixture.windows.front()->y() == shown.y);
        CHECK(fixture.windows.front()->opacity() > 0.0);
        CHECK(fixture.windows.front()->opacity() < 1.0);

        REQUIRE(fixture.waitFor(QuickTerminalState::Shown));
        CHECK(fixture.windows.front()->opacity() == 1.0);
    }
}

TEST_CASE("QuickTerminalController.theMaskRoundsTheBottomCornersOnly", "[quickterminal][controller]")
{
    constexpr auto Width = 200;
    constexpr auto Height = 100;
    auto const mask = contour::window::roundedBottomMask(Width, Height, CornerRadius);
    // The top corners stay square, the bottom ones are cut, and the middle of the bottom edge is whole.
    CHECK(mask.contains(QPoint { 0, 0 }));
    CHECK(mask.contains(QPoint { Width - 1, 0 }));
    CHECK_FALSE(mask.contains(QPoint { 0, Height - 1 }));
    CHECK_FALSE(mask.contains(QPoint { Width - 1, Height - 1 }));
    CHECK(mask.contains(QPoint { Width / 2, Height - 1 }));
    CHECK(mask.contains(QPoint { 0, Height - CornerRadius - 1 }));
    // A radius of 0 is the whole rectangle, which is what the Terminal row asks for.
    CHECK(contour::window::roundedBottomMask(Width, Height, 0) == QRegion { 0, 0, Width, Height });
    // A radius the window cannot hold is limited rather than misdrawn.
    CHECK(contour::window::roundedBottomMask(Width, 10, 500).contains(QPoint { Width / 2, 5 }));
}

TEST_CASE("QuickTerminalController.theWindowIsMaskedAtItsRadius", "[quickterminal][controller]")
{
    auto fixture = Fixture {};
    fixture.hotkey.press();
    auto const shown = contour::window::quickTerminalFrames(TheScreen).shown;
    CHECK(fixture.windows.front()->mask()
          == contour::window::roundedBottomMask(shown.width, shown.height, CornerRadius));
}

TEST_CASE("QuickTerminalController.aScreenAboveMakesItFadeInsteadOfSlide", "[quickterminal][controller]")
{
    auto motion = quickMotion();
    motion.emphasizedMs = 400;
    auto fixture = Fixture { { .hotkey = ControlGrave }, motion };
    // Stacked above the target: a window parked over its top edge would show on that screen.
    fixture.others = { ScreenRect { .x = 0, .y = -1080, .width = 800, .height = 1080 } };
    fixture.hotkey.press();
    REQUIRE(fixture.controller->state() == QuickTerminalState::Showing);
    QTest::qWait(150);
    auto const shown = contour::window::quickTerminalFrames(TheScreen).shown;
    CHECK(fixture.windows.front()->y() == shown.y);
    CHECK(fixture.windows.front()->opacity() < 1.0);

    REQUIRE(fixture.waitFor(QuickTerminalState::Shown));
    CHECK(fixture.windows.front()->opacity() == 1.0);

    // With the screen beside it instead, the slide is back.
    fixture.hotkey.press();
    REQUIRE(fixture.waitFor(QuickTerminalState::Hidden));
    fixture.others = { ScreenRect { .x = 800, .y = 0, .width = 800, .height = 600 } };
    fixture.hotkey.press();
    QTest::qWait(150);
    CHECK(fixture.windows.front()->y() < shown.y);
}

TEST_CASE("QuickTerminalController.aScreenChangeMidSlideSettlesOnTheNewScreen", "[quickterminal][controller]")
{
    auto motion = quickMotion();
    motion.emphasizedMs = 600;
    auto fixture = Fixture { { .hotkey = ControlGrave }, motion };
    fixture.hotkey.press();
    REQUIRE(fixture.controller->state() == QuickTerminalState::Showing);
    QTest::qWait(100);

    // The offscreen platform's one screen has another geometry than the one the frame was laid out for,
    // as if the window had been carried to a screen of its own size.
    auto* const screen = QGuiApplication::primaryScreen();
    REQUIRE(screen != nullptr);
    auto const expected =
        contour::window::quickTerminalFrames(ScreenRect { .x = screen->availableGeometry().x(),
                                                          .y = screen->availableGeometry().y(),
                                                          .width = screen->availableGeometry().width(),
                                                          .height = screen->availableGeometry().height() })
            .shown;
    REQUIRE(expected != contour::window::quickTerminalFrames(TheScreen).shown);
    emit fixture.windows.front()->screenChanged(screen);

    // It does not finish a travel measured against the old screen: it is shown, in the new one's frame.
    CHECK(fixture.controller->state() == QuickTerminalState::Shown);
    auto const geometry = fixture.windows.front()->geometry();
    CHECK(geometry.x() == expected.x);
    CHECK(geometry.y() == expected.y);
    CHECK(geometry.width() == expected.width);
    CHECK(geometry.height() == expected.height);
}

TEST_CASE("QuickTerminalController.focusLostDuringTheSlideInHidesIt", "[quickterminal][controller]")
{
    auto motion = quickMotion();
    motion.emphasizedMs = 600;
    auto fixture = Fixture { { .hotkey = ControlGrave }, motion };
    fixture.hotkey.press();
    REQUIRE(fixture.controller->state() == QuickTerminalState::Showing);
    REQUIRE(QTest::qWaitFor([&] { return fixture.windows.front()->isActive(); }, WaitMilliseconds));

    auto other = QWindow {};
    other.setGeometry(500, 500, 100, 100);
    other.show();
    other.requestActivate();
    REQUIRE(QTest::qWaitFor([&] { return !fixture.windows.front()->isActive(); }, WaitMilliseconds));
    CHECK(fixture.controller->state() == QuickTerminalState::Hiding);
    REQUIRE(fixture.waitFor(QuickTerminalState::Hidden));
}

TEST_CASE("QuickTerminalController.aRefusedChordIsTriedAgainByTheNextReconfiguration",
          "[quickterminal][controller]")
{
    auto fixture = Fixture { { .hotkey = ControlGrave } };
    REQUIRE(fixture.hotkey.registered.size() == 1);
    CHECK(fixture.bounds == 1);

    // The desktop refuses AltSpace; the chord is not remembered as registered.
    fixture.hotkey.result = std::unexpected(HotkeyError::Declined);
    fixture.controller->reconfigure({ .hotkey = AltSpace }, quickMotion());
    CHECK(fixture.errors == std::vector { HotkeyError::Declined });
    CHECK(fixture.hotkey.registered.size() == 1);

    // The same configuration again, the desktop now willing: it is asked again, and the bound notice follows.
    fixture.hotkey.result = {};
    fixture.controller->reconfigure({ .hotkey = AltSpace }, quickMotion());
    REQUIRE(fixture.hotkey.registered.size() == 2);
    CHECK(fixture.hotkey.registered.back() == AltSpace);
    CHECK(fixture.errors.size() == 1);
    CHECK(fixture.bounds == 2);

    // Once bound it is not asked a third time.
    fixture.controller->reconfigure({ .hotkey = AltSpace }, quickMotion());
    CHECK(fixture.hotkey.registered.size() == 2);
    CHECK(fixture.bounds == 2);
}

TEST_CASE("QuickTerminalController.clearingTheHotkeyAfterARefusalStillReleasesTheOldChord",
          "[quickterminal][controller]")
{
    auto fixture = Fixture { { .hotkey = ControlGrave } };
    fixture.hotkey.result = std::unexpected(HotkeyError::AlreadyTaken);
    fixture.controller->reconfigure({ .hotkey = AltSpace }, quickMotion());
    REQUIRE(fixture.errors.size() == 1);
    REQUIRE(fixture.hotkey.unregisters == 0);

    // A backend whose refusal leaves the earlier chord bound is told to let go of it.
    fixture.controller->reconfigure({}, quickMotion());
    CHECK(fixture.hotkey.unregisters == 1);
}

TEST_CASE("QuickTerminalController.itSaysWhenItLeavesTheScreenAndDoesNotKeepTheAppRunningThen",
          "[quickterminal][controller]")
{
    using contour::window::anyWindowKeepsAppAlive;
    using contour::window::quickTerminalKeepsAppAlive;
    using contour::window::windowFactsOf;
    using contour::window::WindowVisibility;

    auto fixture = Fixture {};
    fixture.hotkey.press();
    REQUIRE(fixture.waitFor(QuickTerminalState::Shown));
    CHECK(fixture.offScreens == 0);

    // As the app judges the last-window rule: the one window, counted by the state of the quick terminal.
    auto const aliveWith = [&](QWindow* window) {
        auto const visibility = quickTerminalKeepsAppAlive(fixture.controller->state())
                                    ? WindowVisibility::Shown
                                    : WindowVisibility::Hidden;
        return anyWindowKeepsAppAlive(windowFactsOf({ window }, fixture.controller->window(), visibility));
    };
    CHECK(aliveWith(fixture.windows.front().get()));

    REQUIRE(QTest::qWaitFor([&] { return fixture.windows.front()->isActive(); }, WaitMilliseconds));
    fixture.hotkey.press();
    REQUIRE(fixture.waitFor(QuickTerminalState::Hidden));
    CHECK(fixture.offScreens == 1);

    // A platform that parks the window without hiding it (a macOS panel is ordered out) still lists it as
    // visible; hidden by its state it keeps nothing running.
    fixture.windows.front()->show();
    REQUIRE(fixture.windows.front()->isVisible());
    CHECK_FALSE(aliveWith(fixture.windows.front().get()));

    // Gone for good is off the screen too.
    fixture.windows.front().reset();
    CHECK(fixture.controller->state() == QuickTerminalState::Absent);
    CHECK(fixture.offScreens == 2);
}
