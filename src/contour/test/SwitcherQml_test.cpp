// SPDX-License-Identifier: Apache-2.0
//
// The daemon and workspace switcher popup, offscreen: Switcher.qml in a window's scene over a stand-in for
// the WindowController that presents it, beside the workspace chip that opens it. Its controller's behaviour
// is SwitcherController_test's; this file holds what needs a window.

// The actions defined, not only declared: beside vthost::proto's ClosePane and ResizePane (through the switch
// vocabulary), ActionsFwd's declarations alone read as a namespace mix-up to clang-tidy.
#include <contour/config/Actions.hpp>
#include <contour/test/PopupWaits.hpp>
#include <contour/test/QmlChromeStyle.hpp>
#include <contour/test/QmlComponentsTestSupport.hpp>
#include <contour/test/QmlMessageCapture.hpp>
#include <contour/test/SwitcherFakes.hpp>
#include <contour/test/SwitcherHost.hpp>
#include <contour/window/StatusKinds.hpp>
#include <contour/window/SwitcherController.hpp>

#include <QtCore/QCoreApplication>
#include <QtGui/QColor>
#include <QtQml/QQmlEngine>
#include <QtQuick/QQuickItem>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <QtTest/QTest>

using namespace contour;
using contour::session::SwitchMode;
using contour::test::switcherLabelled;
using contour::test::switcherWorkspaceRef;
using contour::window::SwitcherWorkspaceState;
using contour::window::Urgency;

namespace
{

constexpr auto ThisWindow = vtworkspace::WorkspaceId { 4 };

[[nodiscard]] window::SwitcherWorkspace workspace(
    uint64_t id,
    std::string name,
    SwitcherWorkspaceState state = SwitcherWorkspaceState::Hidden,
    window::TabBadgeKind glyph = window::TabBadgeKind::None)
{
    auto status = window::SwitcherStatus {};
    status.glyph = glyph;
    status.urgency = glyph == window::TabBadgeKind::Error ? Urgency::Failed : Urgency::Idle;
    return window::SwitcherWorkspace { .ref = switcherWorkspaceRef(name),
                                       .position = static_cast<uint32_t>(id),
                                       .name = std::move(name),
                                       .tabs = 1,
                                       .state = state,
                                       .status = std::move(status),
                                       .id = id };
}

[[nodiscard]] std::vector<window::SwitcherWorkspace> workWorkspaces()
{
    return { workspace(1, "contour", SwitcherWorkspaceState::ThisWindow),
             workspace(2, "api-docs", SwitcherWorkspaceState::Hidden, window::TabBadgeKind::Error),
             workspace(3, "docs") };
}

/// A switcher, the window that hosts it, and the wiring the application does: a choice or a cancel withdraws
/// it from the window it is in; the chip opens it below itself.
struct Fixture
{
    test::QmlMessageCapture warnings;
    QQmlEngine engine;
    test::SwitcherWorld world;
    std::unique_ptr<window::SwitcherController> switcher;
    test::MockSwitcherHost host;
    std::unique_ptr<test::SwitcherWindow> scene;
    test::MockSwitcherHost* presentedIn = nullptr;

    explicit Fixture(config::ChromeStyle row = config::ChromeStyle::ContourLinux)
    {
        auto setup = test::defaultChromeSetup();
        setup.row = row;
        test::installChromeStyle(engine, setup);
        switcher = std::make_unique<window::SwitcherController>(world.services());
        QObject::connect(
            switcher.get(), &window::SwitcherController::finished, &host, [this] { withdraw(); });
        QObject::connect(
            switcher.get(), &window::SwitcherController::cancelled, &host, [this] { withdraw(); });
        host.onDropdown = [this](QRectF chip) {
            open(host, chip);
        };
        scene = std::make_unique<test::SwitcherWindow>(engine, host);
    }

    void withdraw()
    {
        if (presentedIn != nullptr)
            presentedIn->withdrawSwitcher();
        presentedIn = nullptr;
    }

    /// Opens the switcher in @p in: centred, or below @p chip as the chip's dropdown.
    void open(test::MockSwitcherHost& in, std::optional<QRectF> chip = std::nullopt)
    {
        if (presentedIn != nullptr && presentedIn != &in)
            presentedIn->withdrawSwitcher();
        presentedIn = &in;
        switcher->open(window::SwitcherOpening {
            .window = ThisWindow,
            .windowDaemon = switcherLabelled("work"),
            .mode = SwitchMode::Switch,
            .prompt = window::SwitcherPrompt::Browse,
            .pick = std::nullopt,
            .scope = chip ? window::SwitcherScope::OneDaemon : window::SwitcherScope::Everything,
            .windowWorkspaces = workWorkspaces(),
            .origin = chip ? window::SwitcherOrigin::Chip : window::SwitcherOrigin::Centred });
        in.presentSwitcher(switcher.get(), chip);
    }

    void openCentred()
    {
        open(host);
        test::waitForPopupOpened(*scene->popup);
    }

    [[nodiscard]] QObject* colors() { return test::qmlSupport::providerOf(engine).colors(); }
};

} // namespace

TEST_CASE("Switcher: the switcher opens in the window's scene, centred, and the chip's hangs below the chip",
          "[contour][switcher][qml]")
{
    auto f = Fixture {};
    f.openCentred();
    auto* popupItem = qobject_cast<QQuickItem*>(f.scene->popup->property("contentItem").value<QObject*>());
    REQUIRE(popupItem != nullptr);
    CHECK(popupItem->window() == f.scene->window);
    auto const placement = test::qmlSupport::providerOf(f.engine).palettePlacement(f.scene->window->width(),
                                                                                   f.scene->window->height());
    CHECK(f.scene->popup->property("x").toReal() == Catch::Approx(placement["x"].toReal()));
    CHECK(f.scene->popup->property("y").toReal() == Catch::Approx(placement["y"].toReal()));
    CHECK(f.host.isWorkspaceDropdownOpen() == false);

    QTest::keyClick(f.scene->window, Qt::Key_Escape);
    test::waitForPopupClosed(*f.scene->popup);

    // The chip's: below the chip, by the s1 gap.
    QTest::mouseClick(f.scene->window,
                      Qt::LeftButton,
                      {},
                      test::SwitcherWindow::sceneRectOf(*f.scene->chip).center().toPoint());
    REQUIRE(f.host.dropdowns == 1);
    test::waitForPopupOpened(*f.scene->popup);
    auto const chip = test::SwitcherWindow::sceneRectOf(*f.scene->chip);
    auto const gap = test::qmlSupport::providerOf(f.engine).space()->property("s1").toReal();
    CHECK(f.scene->popup->property("y").toReal() == Catch::Approx(chip.bottom() + gap));
    CHECK(f.host.isWorkspaceDropdownOpen());
    CHECK(f.scene->chip->property("isOpen").toBool());
    CHECK(f.warnings.count(test::isQmlDiagnostic) == 0);
}

TEST_CASE("Switcher: every row shows the shared status glyph", "[contour][switcher][qml]")
{
    for (auto const row: { config::ChromeStyle::MacOS,
                           config::ChromeStyle::Fluent,
                           config::ChromeStyle::ContourLinux,
                           config::ChromeStyle::Terminal })
    {
        INFO("row " << static_cast<int>(row));
        auto f = Fixture { row };
        f.openCentred();
        // api-docs, the second row, failed.
        auto* item = f.scene->rowAt(1);
        REQUIRE(item != nullptr);
        auto* glyph = item->findChild<QQuickItem*>(QStringLiteral("switcherRowGlyph"));
        REQUIRE(glyph != nullptr);
        CHECK(glyph->property("kind").toInt() == static_cast<int>(window::TabBadgeKind::Error));
        CHECK(glyph->property("known").toBool());
        auto* calm = f.scene->rowAt(2)->findChild<QQuickItem*>(QStringLiteral("switcherRowGlyph"));
        REQUIRE(calm != nullptr);
        CHECK_FALSE(calm->property("known").toBool());
        CHECK(f.warnings.count(test::isQmlDiagnostic) == 0);
    }
}

TEST_CASE("Switcher: the selected row wears the platform's selection", "[contour][switcher][qml]")
{
    using config::ChromeStyle;
    for (auto const row:
         { ChromeStyle::MacOS, ChromeStyle::Fluent, ChromeStyle::ContourLinux, ChromeStyle::Terminal })
    {
        INFO("row " << static_cast<int>(row));
        auto f = Fixture { row };
        f.openCentred();
        auto const current = f.switcher->currentIndex();
        REQUIRE(current == 1); // the first workspace this window does not show
        auto* selected = f.scene->rowAt(current);
        REQUIRE(selected != nullptr);
        auto* fill = selected->findChild<QQuickItem*>(QStringLiteral("switcherSelection"));
        REQUIRE(fill != nullptr);
        CHECK(fill->isVisible());
        auto const emphasized = row == ChromeStyle::MacOS || row == ChromeStyle::Terminal;
        CHECK(fill->property("color").value<QColor>()
              == f.colors()
                     ->property(emphasized ? "sidebarSelectionFill" : "sidebarSelectionFillInactive")
                     .value<QColor>());
        auto* pill = selected->findChild<QQuickItem*>(QStringLiteral("switcherAccentPill"));
        REQUIRE(pill != nullptr);
        CHECK(pill->isVisible() == (row == ChromeStyle::Fluent));
        // The keyboard is in the field: no row wears the focus ring (ruling M3).
        auto* ring = selected->findChild<QQuickItem*>(QStringLiteral("focusRing"));
        REQUIRE(ring != nullptr);
        CHECK_FALSE(ring->isVisible());

        // The arrows move it, and the fill follows.
        QTest::keyClick(f.scene->window, Qt::Key_Down);
        CHECK(f.switcher->currentIndex() == current + 1);
        CHECK(f.scene->part(QStringLiteral("switcherList"))->property("currentIndex").toInt() == current + 1);
        CHECK_FALSE(f.scene->rowAt(current)
                        ->findChild<QQuickItem*>(QStringLiteral("switcherSelection"))
                        ->isVisible());
        CHECK(f.warnings.count(test::isQmlDiagnostic) == 0);
    }
}

TEST_CASE("Switcher: clicking the chip while its dropdown is open closes it and leaves it closed",
          "[contour][switcher][qml]")
{
    auto f = Fixture {};
    auto const chipCentre = test::SwitcherWindow::sceneRectOf(*f.scene->chip).center().toPoint();
    QTest::mouseClick(f.scene->window, Qt::LeftButton, {}, chipCentre);
    test::waitForPopupOpened(*f.scene->popup);
    REQUIRE(f.host.dropdowns == 1);
    CHECK(f.scene->chip->property("openDescription").toString() == "expanded");

    QTest::mouseClick(f.scene->window, Qt::LeftButton, {}, chipCentre);
    test::waitForPopupClosed(*f.scene->popup);
    QTest::qWait(50);
    CHECK_FALSE(f.scene->isOpen());
    CHECK(f.host.dropdowns == 1);
    CHECK_FALSE(f.host.isWorkspaceDropdownOpen());
    CHECK(f.scene->chip->property("openDescription").toString() == "collapsed");
    CHECK(f.warnings.count(test::isQmlDiagnostic) == 0);
}

TEST_CASE("Switcher: reopening over another window withdraws it from the first", "[contour][switcher][qml]")
{
    auto f = Fixture {};
    auto other = test::MockSwitcherHost {};
    auto otherScene = test::SwitcherWindow { f.engine, other };
    f.openCentred();
    auto const restoresBefore = f.scene->root->property("focusRestores").toInt();

    f.open(other);
    test::waitForPopupOpened(*otherScene.popup);
    test::waitForPopupClosed(*f.scene->popup);
    CHECK_FALSE(f.scene->isOpen());
    CHECK(otherScene.isOpen());
    CHECK(f.host.switcher() == nullptr);
    CHECK(other.switcher() == f.switcher.get());
    // The first window's keyboard went back to its terminal.
    CHECK(f.scene->root->property("focusRestores").toInt() == restoresBefore + 1);
    CHECK(f.warnings.count(test::isQmlDiagnostic) == 0);
}

TEST_CASE("Switcher: closing the window with the switcher open raises no QML error",
          "[contour][switcher][qml]")
{
    auto f = Fixture {};
    f.openCentred();
    f.scene.reset();
    QCoreApplication::processEvents();
    // The controller outlives it, and opens elsewhere afterwards.
    f.switcher->setFilter("doc");
    CHECK(f.warnings.count(test::isQmlDiagnostic) == 0);
}

TEST_CASE("Switcher: Escape closes and hands the focus back", "[contour][switcher][qml]")
{
    auto f = Fixture {};
    f.openCentred();
    auto* field = f.scene->part(QStringLiteral("switcherFilter"));
    REQUIRE(field != nullptr);
    CHECK(field->hasActiveFocus());
    auto const restores = f.scene->root->property("focusRestores").toInt();

    // Typed text is cleared first, then Escape closes.
    for (auto const character: std::string_view { "doc" })
        QTest::keyClick(f.scene->window, character);
    CHECK(f.switcher->filter() == "doc");
    QTest::keyClick(f.scene->window, Qt::Key_Escape);
    CHECK(f.switcher->filter().isEmpty());
    CHECK(f.scene->isOpen());
    QTest::keyClick(f.scene->window, Qt::Key_Escape);
    test::waitForPopupClosed(*f.scene->popup);
    CHECK(f.scene->root->property("focusRestores").toInt() == restores + 1);
    CHECK(f.host.closes >= 1);
    CHECK(f.world.requested.empty());
    CHECK(f.warnings.count(test::isQmlDiagnostic) == 0);
}

