// SPDX-License-Identifier: Apache-2.0
//
// The daemon and workspace switcher popup, offscreen: Switcher.qml in a window's scene over a stand-in for
// the WindowController that presents it, beside the workspace chip that opens it. Its controller's behaviour
// is SwitcherController_test's; this file holds what needs a window.

// The actions defined, not only declared: beside vthost::proto's ClosePane and ResizePane (through the switch
// vocabulary), ActionsFwd's declarations alone read as a namespace mix-up to clang-tidy.
#include <contour/command/SwitcherMenus.hpp>
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
#include <QtGui/QAccessible>
#include <QtGui/QColor>
#include <QtGui/QFont>
#include <QtQml/QQmlEngine>
#include <QtQuick/QQuickItem>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <QtTest/QTest>
#include <vthost/SocketPath.hpp>

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
    /// The application's own wiring: a choice or a cancel withdraws it, a window gone abandons it.
    std::unique_ptr<window::SwitcherPresenter> presenter;
    test::MockSwitcherHost host;
    std::unique_ptr<test::SwitcherWindow> scene;

    explicit Fixture(config::ChromeStyle row = config::ChromeStyle::ContourLinux)
    {
        auto setup = test::defaultChromeSetup();
        setup.row = row;
        test::installChromeStyle(engine, setup);
        switcher = std::make_unique<window::SwitcherController>(world.services());
        presenter = std::make_unique<window::SwitcherPresenter>(*switcher);
        host.onDropdown = [this](QRectF chip) {
            open(host, chip);
        };
        scene = std::make_unique<test::SwitcherWindow>(engine, host);
    }

    /// Opens the switcher in @p in: centred, or below @p chip as the chip's dropdown.
    void open(test::MockSwitcherHost& in, std::optional<QRectF> chip = std::nullopt) const
    {
        presenter->open(in,
                        window::SwitcherOpening { .window = ThisWindow,
                                                  .windowDaemon = switcherLabelled("work"),
                                                  .mode = SwitchMode::Switch,
                                                  .prompt = window::SwitcherPrompt::Browse,
                                                  .pick = std::nullopt,
                                                  .scope = chip ? window::SwitcherScope::OneDaemon
                                                                : window::SwitcherScope::Everything,
                                                  .windowWorkspaces = workWorkspaces(),
                                                  .origin = chip ? window::SwitcherOrigin::Chip
                                                                 : window::SwitcherOrigin::Centred },
                        chip);
    }

    /// Builds the switcher afresh, on the world as the test set it up.
    void rebuild()
    {
        presenter.reset();
        switcher = std::make_unique<window::SwitcherController>(world.services());
        presenter = std::make_unique<window::SwitcherPresenter>(*switcher);
    }

    /// Opens the switcher centred on its New Daemon level, as the NewDaemon action does.
    void openNewDaemon()
    {
        presenter->open(host,
                        window::SwitcherOpening { .window = ThisWindow,
                                                  .windowDaemon = switcherLabelled("work"),
                                                  .mode = SwitchMode::Switch,
                                                  .prompt = window::SwitcherPrompt::NewDaemon,
                                                  .pick = std::nullopt,
                                                  .scope = window::SwitcherScope::Everything,
                                                  .windowWorkspaces = workWorkspaces(),
                                                  .origin = window::SwitcherOrigin::Centred },
                        std::nullopt);
        test::waitForPopupOpened(*scene->popup);
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

namespace
{
/// Moves the pointer over row @p index's entry, in two steps so the move counts as one.
void hoverRow(Fixture& f, int index)
{
    auto* item = f.scene->rowAt(index);
    REQUIRE(item != nullptr);
    auto* entry = item->findChild<QQuickItem*>(QStringLiteral("switcherRow"));
    REQUIRE(entry != nullptr);
    auto const centre = test::SwitcherWindow::sceneRectOf(*entry).center().toPoint();
    QTest::mouseMove(f.scene->window, centre - QPoint { 4, 0 });
    QTest::mouseMove(f.scene->window, centre);
    QCoreApplication::processEvents();
}

[[nodiscard]] QQuickItem* partOfRow(Fixture& f, int index, char const* name)
{
    auto* item = f.scene->rowAt(index);
    return item != nullptr ? item->findChild<QQuickItem*>(QString::fromLatin1(name)) : nullptr;
}
} // namespace

TEST_CASE("Switcher: hover fills the row under a moved pointer, shows its button, and leaves the selection",
          "[contour][switcher][qml]")
{
    auto f = Fixture {};
    f.openCentred();
    REQUIRE(f.switcher->currentIndex() == 1);
    // At rest after opening, nothing is hovered.
    CHECK_FALSE(partOfRow(f, 2, "switcherHover")->isVisible());
    CHECK_FALSE(partOfRow(f, 2, "switcherNewWindowButton")->isVisible());
    // The selected row wears its button already.
    CHECK(partOfRow(f, 1, "switcherNewWindowButton")->isVisible());

    hoverRow(f, 2);
    auto* hover = partOfRow(f, 2, "switcherHover");
    REQUIRE(hover != nullptr);
    CHECK(hover->isVisible());
    CHECK(hover->property("color").value<QColor>()
          == f.colors()->property("paletteHoverFill").value<QColor>());
    CHECK(partOfRow(f, 2, "switcherNewWindowButton")->isVisible());
    CHECK(f.switcher->currentIndex() == 1);
    CHECK(f.warnings.count(test::isQmlDiagnostic) == 0);
}

TEST_CASE("Switcher: on every native row the hover fill is lighter than the selection's",
          "[contour][switcher][qml]")
{
    // libadwaita's rule, which every native row follows: a hovered row never reads as the selected one.
    for (auto const style:
         { config::ChromeStyle::ContourLinux, config::ChromeStyle::Fluent, config::ChromeStyle::MacOS })
    {
        CAPTURE(static_cast<int>(style));
        auto f = Fixture { style };
        f.openCentred();
        REQUIRE(f.switcher->currentIndex() == 1);
        hoverRow(f, 2);
        auto const* hover = partOfRow(f, 2, "switcherHover");
        auto const* selection = partOfRow(f, 1, "switcherSelection");
        REQUIRE(hover != nullptr);
        REQUIRE(selection != nullptr);
        REQUIRE(hover->isVisible());
        REQUIRE(selection->isVisible());
        auto const hoverFill = hover->property("color").value<QColor>();
        auto const selectionFill = selection->property("color").value<QColor>();
        CHECK(hoverFill != selectionFill);
        CHECK(hoverFill.alpha() > 0);
        // Lighter: a fainter wash of the same ink, or (macOS) a neutral one under the accent.
        CHECK((hoverFill.alpha() < selectionFill.alpha()
               || hoverFill.hslSaturation() < selectionFill.hslSaturation()));
        CHECK(f.warnings.count(test::isQmlDiagnostic) == 0);
    }
}

TEST_CASE("Switcher: the Terminal row draws no hover fill", "[contour][switcher][qml]")
{
    auto f = Fixture { config::ChromeStyle::Terminal };
    f.openCentred();
    hoverRow(f, 2);
    auto const* hover = partOfRow(f, 2, "switcherHover");
    REQUIRE(hover != nullptr);
    CHECK(hover->property("color").value<QColor>().alpha() == 0);
    CHECK(f.warnings.count(test::isQmlDiagnostic) == 0);
}

TEST_CASE("Switcher: the row's button opens that workspace in a new window", "[contour][switcher][qml]")
{
    auto f = Fixture {};
    f.openCentred();
    auto* button = partOfRow(f, 1, "switcherNewWindowButton");
    REQUIRE(button != nullptr);
    REQUIRE(button->isVisible());
    QTest::mouseClick(
        f.scene->window, Qt::LeftButton, {}, test::SwitcherWindow::sceneRectOf(*button).center().toPoint());
    REQUIRE(f.world.requested.size() == 1);
    CHECK(f.world.requested[0].target == switcherLabelled("work", switcherWorkspaceRef("api-docs")));
    CHECK(f.world.requested[0].mode == SwitchMode::Open);
    test::waitForPopupClosed(*f.scene->popup);
}

TEST_CASE("Switcher: a click switches on release inside the row; a drag out of it does nothing",
          "[contour][switcher][qml]")
{
    auto f = Fixture {};
    f.openCentred();
    auto* entry = partOfRow(f, 2, "switcherRow");
    REQUIRE(entry != nullptr);
    auto const rect = test::SwitcherWindow::sceneRectOf(*entry);

    SECTION("dragged out")
    {
        QTest::mousePress(f.scene->window, Qt::LeftButton, {}, rect.center().toPoint());
        QTest::mouseMove(f.scene->window,
                         rect.center().toPoint() + QPoint { 0, static_cast<int>(rect.height() * 3) });
        QTest::mouseRelease(f.scene->window,
                            Qt::LeftButton,
                            {},
                            rect.center().toPoint() + QPoint { 0, static_cast<int>(rect.height() * 3) });
        CHECK(f.world.requested.empty());
    }
    SECTION("released inside")
    {
        QTest::mouseClick(f.scene->window, Qt::LeftButton, {}, rect.center().toPoint());
        REQUIRE(f.world.requested.size() == 1);
        CHECK(f.world.requested[0].target == switcherLabelled("work", switcherWorkspaceRef("docs")));
        CHECK(f.world.requested[0].mode == SwitchMode::Switch);
    }
}

TEST_CASE("Switcher: right-click, Menu and Shift+F10 open the row's menu from the sidebar's rows",
          "[contour][switcher][qml]")
{
    auto f = Fixture {};
    f.openCentred();
    auto* menu = f.scene->popup->findChild<QObject*>(QStringLiteral("switcherRowMenu"));
    REQUIRE(menu != nullptr);
    auto const expected = [&] {
        auto titles = QStringList {};
        for (auto const& row: command::buildSwitcherRowMenu(
                 command::SwitcherRowMenuState { .name = "api-docs",
                                                 .owner = command::SwitcherMenuOwner::WindowDaemon,
                                                 .workspace = f.world.services().workspaceMenu(ThisWindow, 2),
                                                 .returnMode = command::SwitcherMenuReturn::Switch,
                                                 .shown = command::SwitcherMenuShown::Elsewhere },
                 f.world.style))
            titles << QString::fromStdString(std::visit([](auto const& each) { return each.title; }, row));
        return titles;
    }();
    auto const shownTitles = [&] {
        auto titles = QStringList {};
        for (auto const& entry: menu->property("entries").toList())
            titles << entry.toMap()["title"].toString();
        return titles;
    };

    SECTION("Shift+F10")
    {
        QTest::keyClick(f.scene->window, Qt::Key_F10, Qt::ShiftModifier);
    }
    SECTION("Menu")
    {
        QTest::keyClick(f.scene->window, Qt::Key_Menu);
    }
    SECTION("right-click")
    {
        auto* entry = partOfRow(f, 1, "switcherRow");
        REQUIRE(entry != nullptr);
        QTest::mouseClick(f.scene->window,
                          Qt::RightButton,
                          {},
                          test::SwitcherWindow::sceneRectOf(*entry).center().toPoint());
    }
    REQUIRE(QTest::qWaitFor([&] { return menu->property("visible").toBool(); }, test::PopupSettleMs));
    CHECK(shownTitles() == expected);
    CHECK(f.warnings.count(test::isQmlDiagnostic) == 0);
}

TEST_CASE("Switcher: New Daemon shows the verdict, the equivalent command and one action row",
          "[contour][switcher][qml]")
{
    auto f = Fixture {};
    f.world.found = { { .label = "work", .socket = "/run/contour/work", .workspaces = 3 } };
    f.rebuild();
    f.openNewDaemon();
    REQUIRE(test::switcherWaitUntil([&] { return !f.switcher->isDiscovering(); }));
    auto const typeLabel = [&](std::string_view label) {
        f.switcher->setFilter({});
        for (auto const character: label)
            QTest::keyClick(f.scene->window, character);
        QCoreApplication::processEvents();
    };
    auto const text = [&](char const* name) {
        auto* part = f.scene->part(QString::fromLatin1(name));
        return part != nullptr && part->isVisible() ? part->property("text").toString() : QString {};
    };

    typeLabel("research");
    CHECK(text("switcherLabelVerdict") == "Available");
    CHECK(text("switcherLabelCommand") == "contour daemon --label research");
    auto* command = f.scene->part(QStringLiteral("switcherLabelCommand"));
    REQUIRE(command != nullptr);
    CHECK(command->property("font").value<QFont>().family()
          == test::qmlSupport::providerOf(f.engine)
                 .type()
                 ->property("monospaceFootnote")
                 .value<QFont>()
                 .family());
    REQUIRE(f.switcher->rowCount() == 1);
    CHECK(f.switcher->currentIndex() == 0);
    CHECK(partOfRow(f, 0, "switcherSelection")->isVisible());

    typeLabel("work");
    CHECK(text("switcherLabelVerdict").contains("already running"));
    REQUIRE(f.switcher->rowCount() == 1);
    CHECK(f.switcher->rows().front().title == "Switch this window to daemon “work”");

    typeLabel("a b");
    CHECK(text("switcherLabelVerdict").toStdString()
          == vthost::describe(vthost::SocketLabelError::InvalidCharacter));
    CHECK(text("switcherLabelCommand").isEmpty());
    CHECK(f.switcher->rowCount() == 0);
    CHECK(f.warnings.count(test::isQmlDiagnostic) == 0);
}

namespace
{
/// The Accessible attached object of @p item; null where it has none.
[[nodiscard]] QObject* accessibleOf(QObject* item)
{
    if (item == nullptr)
        return nullptr;
    for (auto* child: item->findChildren<QObject*>(Qt::FindDirectChildrenOnly))
        if (child->inherits("QQuickAccessibleAttached"))
            return child;
    return nullptr;
}
} // namespace

TEST_CASE("Switcher: the field, rows, buttons and headers declare their roles; glyphs and pills are ignored",
          "[contour][switcher][qml][a11y]")
{
    auto f = Fixture {};
    f.openCentred();
    auto const roleOf = [](QObject* item) {
        auto const* attached = accessibleOf(item);
        return attached != nullptr ? attached->property("role").toInt() : -1;
    };
    auto const nameOf = [](QObject* item) {
        auto const* attached = accessibleOf(item);
        return attached != nullptr ? attached->property("name").toString() : QString {};
    };

    auto* field = f.scene->part(QStringLiteral("switcherFilter"));
    CHECK(roleOf(field) == QAccessible::EditableText);
    CHECK(nameOf(field) == "Switch workspace or daemon");

    auto* selected = partOfRow(f, 1, "switcherRow");
    CHECK(roleOf(selected) == QAccessible::ListItem);
    CHECK(nameOf(selected) == "api-docs");
    auto const* attached = accessibleOf(selected);
    REQUIRE(attached != nullptr);
    CHECK(attached->property("description").toString() == "workspace, failed, 1 tab");
    CHECK(attached->property("selected").toBool());
    auto* current = partOfRow(f, 0, "switcherRow");
    CHECK(accessibleOf(current)->property("description").toString() == "workspace, current, 1 tab");

    auto* button = partOfRow(f, 1, "switcherNewWindowButton");
    CHECK(roleOf(button) == QAccessible::Button);
    CHECK(nameOf(button) == "Open api-docs in a new window");

    auto* header = partOfRow(f, 0, "switcherGroupHeader");
    CHECK(roleOf(header) == QAccessible::Heading);

    for (auto const* name: { "switcherRowGlyph", "switcherPill", "switcherDrillIn" })
    {
        INFO(name);
        for (auto const index: { 0, 3 })
            if (auto* part = partOfRow(f, index, name); part != nullptr)
            {
                auto const* ignored = accessibleOf(part);
                REQUIRE(ignored != nullptr);
                CHECK(ignored->property("ignored").toBool());
            }
    }
    CHECK(f.warnings.count(test::isQmlDiagnostic) == 0);
}

TEST_CASE(
    "Switcher: reopened while a row menu's pick closes it, the pick is dropped and a dismissal closes it",
    "[contour][switcher][qml]")
{
    // Reopened during its exit, a popup cancels the exit without closed() (QQuickPopup's transitionEnter):
    // the pick then never ran, and must neither run on the next close nor keep that close from dismissing it.
    auto f = Fixture {};
    f.openCentred();
    auto* const popup = f.scene->popup;
    auto const closed = QSignalSpy { popup, SIGNAL(closed()) };
    auto const picked = QSignalSpy { popup, SIGNAL(picked(QVariant)) };
    popup->setProperty("_closingForPick", true);
    auto const pick =
        QVariant { QVariantMap { { QStringLiteral("row"), 1 }, { QStringLiteral("actionId"), 0 } } };
    REQUIRE(QMetaObject::invokeMethod(popup, "closeThen", Q_ARG(QVariant, pick)));
    REQUIRE(closed.isEmpty()); // still in its exit transition
    f.openCentred();
    CHECK(closed.isEmpty());

    // Outside the popup, away from the chip.
    QTest::mouseClick(f.scene->window, Qt::LeftButton, {}, QPoint { 1000, 620 });
    test::waitForPopupClosed(*f.scene->popup);
    QCoreApplication::processEvents(); // the deferred dispatch, were there one
    CHECK(picked.isEmpty());
    CHECK(f.host.switcher() == nullptr);
    CHECK(f.warnings.count(test::isQmlDiagnostic) == 0);
}

TEST_CASE("Switcher: dismissing it by a click outside drops the daemon start under way",
          "[contour][switcher][qml][threading]")
{
    auto f = Fixture {};
    auto start = test::GatedStart {};
    f.world.startJob = [&start] {
        return start.job();
    };
    f.rebuild();
    f.openNewDaemon();
    for (auto const character: std::string_view { "research" })
        QTest::keyClick(f.scene->window, character);
    QTest::keyClick(f.scene->window, Qt::Key_Return);
    REQUIRE(start.awaitStarted());
    REQUIRE(f.switcher->isBusy());

    // Outside the popup, away from the chip.
    QTest::mouseClick(f.scene->window, Qt::LeftButton, {}, QPoint { 1000, 620 });
    test::waitForPopupClosed(*f.scene->popup);
    CHECK_FALSE(f.switcher->isBusy());
    CHECK(f.host.switcher() == nullptr);

    start.release();
    QTest::qWait(100);
    QCoreApplication::processEvents();
    CHECK(f.world.requested.empty());
    CHECK(f.warnings.count(test::isQmlDiagnostic) == 0);
}

TEST_CASE("Switcher: the window it is presented in going away drops the daemon start under way",
          "[contour][switcher][threading]")
{
    auto f = Fixture {};
    auto start = test::GatedStart {};
    f.world.startJob = [&start] {
        return start.job();
    };
    f.rebuild();
    auto other = std::make_unique<test::MockSwitcherHost>();
    f.presenter->open(*other,
                      window::SwitcherOpening { .window = ThisWindow,
                                                .windowDaemon = switcherLabelled("work"),
                                                .mode = SwitchMode::Switch,
                                                .prompt = window::SwitcherPrompt::NewDaemon,
                                                .pick = std::nullopt,
                                                .scope = window::SwitcherScope::Everything,
                                                .windowWorkspaces = workWorkspaces(),
                                                .origin = window::SwitcherOrigin::Centred },
                      std::nullopt);
    f.switcher->submitLabel("research", static_cast<int>(SwitchMode::Switch));
    REQUIRE(start.awaitStarted());

    other.reset();
    CHECK(f.presenter->host() == nullptr);
    CHECK_FALSE(f.switcher->isBusy());
    start.release();
    QTest::qWait(100);
    QCoreApplication::processEvents();
    CHECK(f.world.requested.empty());
}

TEST_CASE("Switcher: a row menu's Rename… runs once the popup has closed, keeping the editor's focus",
          "[contour][switcher][qml]")
{
    auto f = Fixture {};
    f.world.onRan = [&f] {
        f.scene->editor->forceActiveFocus();
    };
    f.openCentred();
    auto* menu = f.scene->popup->findChild<QObject*>(QStringLiteral("switcherRowMenu"));
    REQUIRE(menu != nullptr);
    QTest::keyClick(f.scene->window, Qt::Key_Menu);
    REQUIRE(QTest::qWaitFor([&] { return menu->property("visible").toBool(); }, test::PopupSettleMs));
    auto rename = -1;
    for (auto const& entry: menu->property("entries").toList())
        if (entry.toMap()["title"].toString().startsWith("Rename"))
            rename = entry.toMap()["actionId"].toInt();
    REQUIRE(rename >= 0);

    QMetaObject::invokeMethod(menu, "picked", Q_ARG(int, rename));
    test::waitForPopupClosed(*f.scene->popup);
    REQUIRE(QTest::qWaitFor([&] { return !f.world.ran.empty(); }, test::PopupSettleMs));
    QTest::qWait(50);
    CHECK(f.scene->editor->hasActiveFocus());
    CHECK(f.host.switcher() == nullptr);
    CHECK(f.warnings.count(test::isQmlDiagnostic) == 0);
}

TEST_CASE("Switcher: its rows stay drawn while it fades out", "[contour][switcher][qml]")
{
    auto f = Fixture {};
    f.openCentred();
    auto* list = f.scene->part(QStringLiteral("switcherList"));
    REQUIRE(list != nullptr);
    auto const rows = list->property("count").toInt();
    REQUIRE(rows > 0);

    QTest::keyClick(f.scene->window, Qt::Key_Escape);
    // Every frame of the exit transition still shows every row.
    auto fewest = rows;
    REQUIRE(QTest::qWaitFor(
        [&] {
            if (!f.scene->isOpen())
                return true;
            fewest = std::min(fewest, list->property("count").toInt());
            return false;
        },
        test::PopupSettleMs));
    CHECK(fewest == rows);
    CHECK(f.host.switcher() == nullptr);
}
