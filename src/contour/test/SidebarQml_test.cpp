// SPDX-License-Identifier: Apache-2.0
//
// Offscreen cases for the workspace sidebar's QML (Sidebar.qml, SidebarRow.qml, the breadcrumb and the
// workspace chip): Main.qml over MockMainController, whose sidebar is a real SidebarModel over a fake window
// (SidebarFakes.hpp), so the rows the view draws are the ones the model builds. Every case runs on each
// chrome row this process's controls style can draw; the four ctest entries of contour_gui_test cover the
// four rows.

#include <contour/command/Shortcut.hpp>
#include <contour/config/AccessibilityPreferences.hpp>
#include <contour/config/TabBarMode.hpp>
#include <contour/config/UiStyle.hpp>
#include <contour/test/ChromeSnapshotPlan.hpp>
#include <contour/test/CoreFixtures.hpp>
#include <contour/test/MockMainController.hpp>
#include <contour/test/QmlChromeStyle.hpp>
#include <contour/test/QmlMessageCapture.hpp>
#include <contour/test/QmlMocks.hpp>
#include <contour/window/CloseDialogTable.hpp>
#include <contour/window/NodeDrag.hpp>
#include <contour/window/SidebarControls.hpp>
#include <contour/window/SidebarModel.hpp>
#include <contour/window/StatusUrgency.hpp>

#include <vtbackend/vt/ProgramStatus.hpp>

#include <QtCore/QCoreApplication>
#include <QtCore/QEvent>
#include <QtCore/QMimeData>
#include <QtCore/QRegularExpression>
#include <QtCore/QStringList>
#include <QtGui/QAccessible>
#include <QtGui/QGuiApplication>
#include <QtGui/QStyleHints>
#include <QtQml/QQmlEngine>
#include <QtQuick/QQuickItem>
#include <QtQuick/QQuickWindow>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <iterator>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <QtQuickControls2/QQuickStyle>
#include <QtTest/QSignalSpy>
#include <QtTest/QTest>
#include <qpa/qplatformdrag.h>
#include <qpa/qwindowsysteminterface.h>

using contour::config::ChromeStyle;
using contour::test::MockMainController;
using namespace std::chrono_literals;

namespace
{

/// The rows this process's controls style draws.
[[nodiscard]] std::vector<ChromeStyle> drawnRows()
{
    auto rows = contour::test::snapshot::rowsRenderableUnder(QQuickStyle::name().toStdString());
    REQUIRE_FALSE(rows.empty());
    return rows;
}

/// The setup @p row is loaded under, with the window controls its host shows.
[[nodiscard]] contour::test::ChromeSetup setupOf(
    ChromeStyle row, contour::config::MotionPreference motion = contour::config::MotionPreference::Full)
{
    auto setup = contour::test::ChromeSetup {
        .row = row,
        .font = QGuiApplication::font(),
        .windowControls = contour::config::resolveWindowControlStyle(
            contour::config::WindowControlStyle::Auto, contour::test::snapshot::rowHost(row).host),
    };
    setup.accessibility.motion = motion;
    return setup;
}

/// A record of @p state that changed state @p age ago.
[[nodiscard]] vtbackend::ProgramStatusRecord recordOf(vtbackend::ProgramState state,
                                                      std::string message = {},
                                                      std::chrono::seconds age = 30s)
{
    auto record = vtbackend::ProgramStatusRecord {};
    record.state = state;
    record.app = "claude";
    record.message = std::move(message);
    record.since = MockMainController::SidebarEpoch - age;
    record.generation = 1;
    return record;
}

/// What billing's first pane asks of the user.
enum class BillingAsks : uint8_t
{
    Nothing = 0, ///< It is idle.
    Permission,  ///< It waits for a permission.
    Done,        ///< It finished, and the user has not looked.
    FailedSeen,  ///< It failed, and the user has looked.
};

/// Whether the window shows a pane tree.
enum class PaneTree : uint8_t
{
    Leaf = 0, ///< One terminal pane.
    None,     ///< None, as a window that is closing has pruned its tree before its controller goes.
};

/// How the window the sidebar is loaded in is set up.
struct SidebarScene
{
    ChromeStyle row = ChromeStyle::MacOS;
    contour::config::SidebarVisibility visibility = contour::config::SidebarVisibility::Shown;
    contour::config::TabPlacement placement = contour::config::TabPlacement::Sidebar;
    contour::config::MotionPreference motion = contour::config::MotionPreference::Full;
    uint64_t shownWorkspace = 1; ///< The workspace the window shows: billing (1) or docs (2).
    BillingAsks billing = BillingAsks::Permission; ///< What billing's first pane asks.
    /// When the strip shows: Never with tabs on top hides a band that is not also the window's controls.
    contour::config::TabBarVisibility tabBar = contour::config::TabBarVisibility::Always;
    PaneTree paneTree = PaneTree::Leaf; ///< Whether the window shows a pane tree.
    /// The sidebar's width in logical pixels; the row's own default when unset.
    std::optional<int> width = std::nullopt;
};

/// Main.qml over a mock whose sidebar lists two workspaces: billing (a pane that asks for a permission and
/// one that works) and docs (one idle pane).
struct SidebarWindow
{
    explicit SidebarWindow(SidebarScene scene):
        ownedController { std::make_unique<MockMainController>(contour::test::MainWindowScene {
            .tabCount = 2,
            .rootPane = scene.paneTree == PaneTree::Leaf ? &pane : nullptr,
            .tabBarVisibility = scene.tabBar,
            .shortcutStyle =
                contour::command::shortcutStyleFor(contour::test::snapshot::rowHost(scene.row).host) }) },
        controller { *ownedController }
    {
        pane.setSession(&session);
        pane.setActive(true);
        controller.mirrorsChromeKeys = true;

        auto claude = contour::test::directoryPaneOf(10, "claude");
        if (scene.billing == BillingAsks::Permission)
        {
            auto blocked = recordOf(vtbackend::ProgramState::Blocked, "Bash: psql $DEV_URL");
            blocked.kind = vtbackend::BlockedKind::Permission;
            claude.status = { blocked };
        }
        if (scene.billing == BillingAsks::Done)
            claude.status = { recordOf(vtbackend::ProgramState::Done, "All tests pass") };
        if (scene.billing == BillingAsks::FailedSeen)
        {
            claude.status = { recordOf(vtbackend::ProgramState::Error, "exit 1") };
            controller.sidebarAttention().adopt(
                contour::window::PaneKey { 10 },
                contour::window::SessionAttention { .seenStatusGeneration = 1 });
        }
        claude.cwd = "/src/billing";
        auto build = contour::test::directoryPaneOf(11, "build");
        build.status = { recordOf(vtbackend::ProgramState::Working) };
        auto& listed = controller.sidebarSource().listed;
        auto billingWorkspace = contour::test::directoryWorkspaceOf(1, "billing", { claude, build });
        billingWorkspace.git = contour::window::DirectoryGit { .branch = "feat/billing", .ahead = 2 };
        listed.workspaces = {
            std::move(billingWorkspace),
            contour::test::directoryWorkspaceOf(2, "docs", { contour::test::directoryPaneOf(20, "notes") }),
        };
        listed.selected = scene.shownWorkspace;
        controller.sidebarHost().displayed =
            contour::window::TabRef { .workspace = scene.shownWorkspace,
                                      .tab = scene.shownWorkspace == 1 ? 10u : 20u };
        controller.sidebarModel().refreshNow();
        controller.setTabPlacement(scene.placement);

        root = contour::test::loadMainWindow(engine, controller, warnings, setupOf(scene.row, scene.motion));
        if (scene.visibility == contour::config::SidebarVisibility::Shown)
            controller.showSidebar(scene.width.value_or(static_cast<int>(token("width"))));
        window = qobject_cast<QQuickWindow*>(root.get());
        REQUIRE(window != nullptr);
        window->resize(1024, 640);
        window->show();
        REQUIRE(QTest::qWaitForWindowExposed(window));
        window->requestActivate();
        REQUIRE(QTest::qWaitForWindowActive(window));
        QCoreApplication::processEvents();
    }

    /// Closes the window as the application does: its controller goes, and the sidebar model with it,
    /// while the window's items are still there (WindowController::closeWindow() deletes the controller
    /// later; the window outlives it). `controller` must not be used afterwards.
    void closeController()
    {
        // The application's session manager outlives the window; the mock is both, so another stands in.
        engine.rootContext()->setContextProperty(QStringLiteral("terminalSessions"), &manager);
        ownedController.reset();
        QCoreApplication::processEvents();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCoreApplication::processEvents();
    }

    /// The `chromeStyle.sidebar` token @p name.
    [[nodiscard]] qreal token(char const* name) const
    {
        auto const* style =
            engine.rootContext()->contextProperty(QStringLiteral("chromeStyle")).value<QObject*>();
        REQUIRE(style != nullptr);
        auto const* sidebar = style->property("sidebar").value<QObject*>();
        REQUIRE(sidebar != nullptr);
        return sidebar->property(name).toReal();
    }

    /// The `chromeStyle` property @p name.
    [[nodiscard]] qreal style(char const* name) const
    {
        auto const* provider =
            engine.rootContext()->contextProperty(QStringLiteral("chromeStyle")).value<QObject*>();
        REQUIRE(provider != nullptr);
        return provider->property(name).toReal();
    }

    /// The first item named @p name in the window's visual tree, or null.
    [[nodiscard]] QQuickItem* item(QString const& name) const
    {
        return findNamed(*window->contentItem(), name);
    }

    /// The first visible item named @p name, or null.
    [[nodiscard]] QQuickItem* visibleItem(QString const& name) const
    {
        auto const found = items(name);
        auto const shown =
            std::ranges::find_if(found, [](QQuickItem const* item) { return item->isVisible(); });
        return shown != found.end() ? *shown : nullptr;
    }

    /// Every item named @p name, in the visual tree's order.
    [[nodiscard]] std::vector<QQuickItem*> items(QString const& name) const
    {
        auto found = std::vector<QQuickItem*> {};
        collect(*window->contentItem(), name, found);
        return found;
    }

    /// The tree's delegates, ordered by their row.
    [[nodiscard]] std::vector<QQuickItem*> rows() const
    {
        auto found = items(QStringLiteral("sidebarRow"));
        std::ranges::sort(found, {}, [](QQuickItem const* row) { return row->property("index").toInt(); });
        return found;
    }

    /// The row titled @p title, or null.
    [[nodiscard]] QQuickItem* rowTitled(QString const& title) const
    {
        for (auto* row: rows())
            if (row->property("title").toString() == title)
                return row;
        return nullptr;
    }

    /// Clicks the middle of @p target.
    void click(QQuickItem const& target) const
    {
        auto const centre = target.mapToScene(QPointF { target.width() / 2, target.height() / 2 }).toPoint();
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, centre);
        QCoreApplication::processEvents();
    }

    [[nodiscard]] static QQuickItem* findNamed(QQuickItem const& parent, QString const& name)
    {
        for (auto* child: parent.childItems())
        {
            if (child->objectName() == name)
                return child;
            if (auto* found = findNamed(*child, name))
                return found;
        }
        return nullptr;
    }

    static void collect(QQuickItem const& parent, QString const& name, std::vector<QQuickItem*>& found)
    {
        for (auto* child: parent.childItems())
        {
            if (child->objectName() == name)
                found.push_back(child);
            collect(*child, name, found);
        }
    }

    QQmlEngine engine;
    contour::test::MockSession session;
    contour::test::MockPaneProxy pane { /*leaf=*/true };
    MockMainController manager; ///< The session manager that outlives the window (closeController()).
    std::unique_ptr<MockMainController> ownedController; ///< Null once closeController() has run.
    MockMainController& controller;                      ///< *ownedController.
    contour::test::QmlMessageCapture warnings;
    std::unique_ptr<QObject> root;
    QQuickWindow* window = nullptr;
};

/// Whether @p message is a diagnostic of the sidebar's own QML.
[[nodiscard]] bool isSidebarDiagnostic(QString const& message)
{
    static auto const sidebarFiles = QRegularExpression { QStringLiteral(
        R"((Sidebar|SidebarRow|SidebarHoverCard|SidebarFooter|WorkspaceChip|Breadcrumb)\.qml)") };
    return contour::test::isQmlDiagnostic(message) && sidebarFiles.match(message).hasMatch();
}

/// The `Accessible` attached object of @p item, or null.
[[nodiscard]] QObject* accessibleOf(QObject const& item)
{
    for (auto* child: item.findChildren<QObject*>(Qt::FindDirectChildrenOnly))
        if (child->inherits("QQuickAccessibleAttached"))
            return child;
    return nullptr;
}

/// What macOS keeps at the window's leading edge for its traffic lights, as AppKit reports it.
constexpr auto TrafficLightsInset = qreal { 78 };

/// @return @p item's rectangle in the scene.
[[nodiscard]] QRectF sceneRectOf(QQuickItem const& item)
{
    return item.mapRectToScene(QRectF { 0, 0, item.width(), item.height() });
}

/// Presses the left button at @p from, drags it 80 pixels to the right and down, and releases it.
void dragFrom(QQuickWindow& window, QPoint from)
{
    QTest::mousePress(&window, Qt::LeftButton, Qt::NoModifier, from);
    for (auto const step: std::views::iota(1, 9))
        QTest::mouseMove(&window, from + QPoint { step * 10, step * 10 });
    QTest::mouseRelease(&window, Qt::LeftButton, Qt::NoModifier, from + QPoint { 80, 80 });
    QCoreApplication::processEvents();
}

/// Drags the row @p from onto the row @p to as the user does, and drops at @p fraction of @p to's height.
///
/// The gesture is real mouse events: a press, and moves past the drag threshold, which the row's DragHandler
/// must turn into the node drag (the controller is asked for its payload, and told when the drag is over).
/// The offscreen platform has no OS drag loop, so that drag returns at once; what the OS does with the
/// payload next -- drag-enter, drag-move and drop events to the window under the pointer -- is delivered here
/// through the platform interface, to the DropArea of the row the pointer is over.
void dragRowOnto(SidebarWindow& scene, QQuickItem const& from, QQuickItem const& to, double fraction)
{
    auto const at = [&](QQuickItem const& item, double x, double y) {
        return item.mapToScene(QPointF { item.width() * x, item.height() * y }).toPoint();
    };
    auto const start = at(from, 0.5, 0.5);
    auto const end = at(to, 0.5, fraction);

    QTest::mousePress(scene.window, Qt::LeftButton, Qt::NoModifier, start);
    for (auto const step: std::views::iota(1, 6))
        QTest::mouseMove(scene.window, start + QPoint { 0, step * 4 });
    QTest::mouseRelease(scene.window, Qt::LeftButton, Qt::NoModifier, start + QPoint { 0, 20 });
    QCoreApplication::processEvents();
    REQUIRE(scene.controller.finishedDrags == 1); // the handler started the node drag, and it ended

    auto mime = QMimeData {};
    mime.setData(QString::fromLatin1(contour::window::NodeDragMime),
                 scene.controller.beginSidebarDrag(from.property("index").toInt()).toUtf8());
    constexpr auto Steps = 6;
    for (auto const step: std::views::iota(1, Steps + 1))
        QWindowSystemInterface::handleDrag(scene.window,
                                           &mime,
                                           start + (end - start) * step / Steps,
                                           Qt::MoveAction,
                                           Qt::LeftButton,
                                           Qt::NoModifier);
    QWindowSystemInterface::handleDrop(
        scene.window, &mime, end, Qt::MoveAction, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::processEvents();
}

/// Whether @p item is @p ancestor or lies beneath it.
[[nodiscard]] bool isWithin(QQuickItem const* item, QQuickItem const* ancestor)
{
    return item != nullptr && ancestor != nullptr && (item == ancestor || ancestor->isAncestorOf(item));
}

} // namespace

TEST_CASE("the sidebar shows one row per model row with title, reason and time (offscreen)",
          "[contour][gui][qml][sidebar]")
{
    for (auto const row: drawnRows())
    {
        INFO("row " << static_cast<int>(row));
        auto scene = SidebarWindow { { .row = row } };
        auto const& model = scene.controller.sidebarModel();
        REQUIRE(model.rowCount() == 5); // billing, claude, build, docs, notes
        REQUIRE(QTest::qWaitFor([&] { return std::cmp_equal(scene.rows().size(), model.rowCount()); }, 1000));

        auto* claude = scene.rowTitled(QStringLiteral("claude"));
        REQUIRE(claude != nullptr);
        auto* title = SidebarWindow::findNamed(*claude, QStringLiteral("sidebarRowTitle"));
        auto* reason = SidebarWindow::findNamed(*claude, QStringLiteral("sidebarRowReason"));
        auto* time = SidebarWindow::findNamed(*claude, QStringLiteral("sidebarRowTime"));
        REQUIRE(title != nullptr);
        REQUIRE(reason != nullptr);
        REQUIRE(time != nullptr);
        CHECK(title->property("text").toString() == QStringLiteral("claude"));
        CHECK(reason->isVisible());
        // Only the command is set in the monospace face; its words are the proportional footnote's.
        auto* command = SidebarWindow::findNamed(*claude, QStringLiteral("sidebarRowReasonCommand"));
        REQUIRE(command != nullptr);
        CHECK(reason->property("text").toString() == QStringLiteral("Permission · Bash: "));
        CHECK(command->property("text").toString() == QStringLiteral("psql $DEV_URL"));
        auto const* style =
            scene.engine.rootContext()->contextProperty(QStringLiteral("chromeStyle")).value<QObject*>();
        REQUIRE(style != nullptr);
        auto const* type = style->property("type").value<QObject*>();
        REQUIRE(type != nullptr);
        CHECK(command->property("font").value<QFont>() == type->property("monospaceFootnote").value<QFont>());
        CHECK(reason->property("font").value<QFont>() == type->property("footnote").value<QFont>());
        CHECK(command->isVisible());
        // The two sit on one line, in one row.
        CHECK(reason->property("y").toReal() == command->property("y").toReal());
        CHECK(time->isVisible());
        CHECK(time->property("text").toString() == QStringLiteral("0:30"));
        // A row with a reason is two lines tall, one without it one line.
        CHECK(claude->height() == scene.token("twoLineRowHeight"));
        auto* docs = scene.rowTitled(QStringLiteral("docs"));
        REQUIRE(docs != nullptr);
        CHECK(docs->height() == scene.token("rowHeight"));
        CHECK(scene.warnings.count(contour::test::isQmlDiagnostic) == 0);
    }
}

TEST_CASE("clicking a row calls activate (offscreen)", "[contour][gui][qml][sidebar]")
{
    for (auto const row: drawnRows())
    {
        INFO("row " << static_cast<int>(row));
        auto scene = SidebarWindow { { .row = row } };
        auto* build = scene.rowTitled(QStringLiteral("build"));
        REQUIRE(build != nullptr);
        scene.click(*build);
        // A tab row shows its pane; a workspace row selects the workspace.
        CHECK(scene.controller.sidebarHost().shown == std::vector { contour::window::PaneKey { 11 } });

        auto* docs = scene.rowTitled(QStringLiteral("docs"));
        REQUIRE(docs != nullptr);
        QTest::qWait(QGuiApplication::styleHints()->mouseDoubleClickInterval() + 50);
        scene.click(*docs);
        CHECK(scene.controller.sidebarSource().selected == std::vector<uint64_t> { 2 });
    }
}

TEST_CASE("F2 opens the rename field and Enter commits (offscreen)", "[contour][gui][qml][sidebar]")
{
    for (auto const row: drawnRows())
    {
        INFO("row " << static_cast<int>(row));
        auto scene = SidebarWindow { { .row = row } };
        auto* tree = scene.item(QStringLiteral("sidebarTree"));
        REQUIRE(tree != nullptr);

        // The keyboard comes in on the row of the tab the window shows; Up reaches its workspace.
        scene.controller.focusSidebar();
        REQUIRE(QTest::qWaitFor([&] { return tree->hasActiveFocus(); }, 1000));
        CHECK(tree->property("currentIndex").toInt() == 1);
        QTest::keyClick(scene.window, Qt::Key_Up);
        CHECK(tree->property("currentIndex").toInt() == 0);

        QTest::keyClick(scene.window, Qt::Key_F2);
        QQuickItem* field = nullptr;
        REQUIRE(QTest::qWaitFor(
            [&] { return (field = scene.item(QStringLiteral("sidebarRenameField"))) != nullptr; }, 1000));
        REQUIRE(QTest::qWaitFor([&] { return field->hasActiveFocus(); }, 1000));
        CHECK(field->property("text").toString() == QStringLiteral("billing"));

        // Select-all is on, so the typed name replaces the old one.
        for (auto const key: QStringLiteral("payments"))
            QTest::keyClick(scene.window, key.toLatin1());
        QTest::keyClick(scene.window, Qt::Key_Return);
        QCoreApplication::processEvents();
        CHECK(scene.controller.sidebarHost().renamed
              == std::vector { std::pair { uint64_t { 1 }, std::string { "payments" } } });
        CHECK(QTest::qWaitFor([&] { return scene.item(QStringLiteral("sidebarRenameField")) == nullptr; },
                              1000));
        // The tree has the keyboard back.
        CHECK(QTest::qWaitFor([&] { return tree->hasActiveFocus(); }, 1000));
    }
}

TEST_CASE("Escape returns focus to the terminal (offscreen)", "[contour][gui][qml][sidebar]")
{
    for (auto const row: drawnRows())
    {
        INFO("row " << static_cast<int>(row));
        auto scene = SidebarWindow { { .row = row } };
        auto* tree = scene.item(QStringLiteral("sidebarTree"));
        auto* content = scene.item(QStringLiteral("content"));
        REQUIRE(tree != nullptr);
        REQUIRE(content != nullptr);

        scene.controller.focusSidebar();
        REQUIRE(QTest::qWaitFor([&] { return tree->hasActiveFocus(); }, 1000));
        QTest::keyClick(scene.window, Qt::Key_Escape);
        QCoreApplication::processEvents();
        CHECK_FALSE(scene.controller.sidebarHasFocus());
        CHECK(QTest::qWaitFor([&] { return isWithin(scene.window->activeFocusItem(), content); }, 1000));
    }
}

TEST_CASE("Escape in the tree clears a typed filter, and the next one leaves (offscreen)",
          "[contour][gui][qml][sidebar]")
{
    for (auto const row: drawnRows())
    {
        INFO("row " << static_cast<int>(row));
        auto scene = SidebarWindow { { .row = row } };
        auto* tree = scene.item(QStringLiteral("sidebarTree"));
        auto* field = scene.item(QStringLiteral("sidebarFilterField"));
        REQUIRE(tree != nullptr);
        REQUIRE(field != nullptr);
        auto const& model = scene.controller.sidebarModel();
        auto const unfiltered = model.rowCount();

        scene.controller.focusSidebar();
        REQUIRE(QTest::qWaitFor([&] { return tree->hasActiveFocus(); }, 1000));
        QTest::keyClick(scene.window, 'b');
        REQUIRE(QTest::qWaitFor([&] { return field->hasActiveFocus(); }, 1000));
        REQUIRE(model.isFiltered());
        // Back into the tree with the filter still typed.
        QTest::keyClick(scene.window, Qt::Key_Down);
        REQUIRE(QTest::qWaitFor([&] { return tree->hasActiveFocus(); }, 1000));

        QTest::keyClick(scene.window, Qt::Key_Escape);
        QCoreApplication::processEvents();
        CHECK_FALSE(model.isFiltered());
        CHECK(model.rowCount() == unfiltered);
        CHECK(field->property("text").toString().isEmpty());
        CHECK_FALSE(field->isVisible());
        CHECK(scene.controller.sidebarHasFocus());
        CHECK(tree->hasActiveFocus());

        QTest::keyClick(scene.window, Qt::Key_Escape);
        QCoreApplication::processEvents();
        CHECK_FALSE(scene.controller.sidebarHasFocus());
    }
}

TEST_CASE("the sidebar is absent when hidden and content fills the width (offscreen)",
          "[contour][gui][qml][sidebar]")
{
    for (auto const row: drawnRows())
    {
        INFO("row " << static_cast<int>(row));
        {
            auto scene =
                SidebarWindow { { .row = row, .visibility = contour::config::SidebarVisibility::Hidden } };
            auto* content = scene.item(QStringLiteral("content"));
            REQUIRE(content != nullptr);
            CHECK(scene.item(QStringLiteral("sidebar")) == nullptr);
            CHECK(content->x() == 0);
            CHECK(content->width() == scene.window->width());
        }
        {
            auto scene = SidebarWindow { { .row = row } };
            auto* content = scene.item(QStringLiteral("content"));
            auto* sidebar = scene.item(QStringLiteral("sidebar"));
            auto* divider = scene.item(QStringLiteral("sidebarDivider"));
            REQUIRE(content != nullptr);
            REQUIRE(sidebar != nullptr);
            REQUIRE(divider != nullptr);
            // The controller's whole pixels, as the row's own units: whole cells on the Terminal row.
            CHECK(std::abs(sidebar->width() - scene.controller.sidebarWidth()) < 1);
            CHECK(content->x() == sidebar->width());
            CHECK(content->width() == scene.window->width() - sidebar->width());
            // The divider is the sidebar's last column: one cell on the Terminal row, a hairline elsewhere.
            CHECK(divider->x() + divider->width() == sidebar->width());
            if (row == ChromeStyle::Terminal)
                CHECK(divider->width() == scene.style("cellWidth"));
        }
    }
}

TEST_CASE("breadcrumb shows workspace › tab (offscreen)", "[contour][gui][qml][sidebar]")
{
    for (auto const row: drawnRows())
    {
        INFO("row " << static_cast<int>(row));
        auto scene = SidebarWindow { { .row = row } };
        auto* breadcrumb = scene.item(QStringLiteral("breadcrumb"));
        REQUIRE(breadcrumb != nullptr);
        CHECK(breadcrumb->isVisible());
        auto* workspace = scene.item(QStringLiteral("breadcrumbWorkspace"));
        auto* separator = scene.item(QStringLiteral("breadcrumbSeparator"));
        auto* tab = scene.item(QStringLiteral("breadcrumbTab"));
        REQUIRE(workspace != nullptr);
        REQUIRE(separator != nullptr);
        REQUIRE(tab != nullptr);
        CHECK(workspace->property("text").toString() == QStringLiteral("billing"));
        CHECK(separator->property("text").toString() == QStringLiteral("›"));
        CHECK(tab->property("text").toString() == QStringLiteral("Tab 1"));
        // The sidebar holds the tabs: the band shows no strip.
        CHECK(scene.item(QStringLiteral("tabStrip")) == nullptr);
    }
}

TEST_CASE("the breadcrumb's parts are spaced on every row (offscreen)", "[contour][gui][qml][sidebar]")
{
    for (auto const row: drawnRows())
    {
        INFO("row " << static_cast<int>(row));
        auto scene = SidebarWindow { { .row = row } };
        auto* workspace = scene.item(QStringLiteral("breadcrumbWorkspace"));
        auto* separator = scene.item(QStringLiteral("breadcrumbSeparator"));
        auto* tab = scene.item(QStringLiteral("breadcrumbTab"));
        REQUIRE(workspace != nullptr);
        REQUIRE(separator != nullptr);
        REQUIRE(tab != nullptr);
        // The separator is parted from both neighbours, by at least a cell on the Terminal row.
        auto const least = row == ChromeStyle::Terminal ? scene.style("cellWidth") : 1.0;
        CHECK(separator->x() - (workspace->x() + workspace->width()) >= least - 0.01);
        CHECK(tab->x() - (separator->x() + separator->width()) >= least - 0.01);
    }
}

namespace
{
/// Waits until a row's trail has placed its roll-up marker after the git text at their current widths. A
/// Row lays its items out on the window's next polish, not when a width changes, and the title's right edge
/// follows the trail's left one -- so until then the title measures against the trail's stale width, which
/// a slow runner keeps for longer than any fixed wait.
/// @param git The row's git text, the trail's first item.
/// @param rollUp The row's roll-up marker, placed after the git text.
/// @return Whether the trail laid out within the timeout.
[[nodiscard]] bool waitForTrailLayout(QQuickItem const& git, QQuickItem const& rollUp)
{
    auto const spacing = git.parentItem()->property("spacing").toReal();
    return QTest::qWaitFor(
        [&] {
            auto const expected = git.isVisible() ? git.x() + git.width() + spacing : 0.0;
            return std::abs(rollUp.x() - expected) < 0.01;
        },
        2000);
}
} // namespace

TEST_CASE("a narrow workspace row keeps its name and elides the git text first (offscreen)",
          "[contour][gui][qml][sidebar]")
{
    for (auto const row: drawnRows())
    {
        INFO("row " << static_cast<int>(row));
        auto scene = SidebarWindow { { .row = row } };
        auto* billing = scene.rowTitled(QStringLiteral("billing"));
        REQUIRE(billing != nullptr);
        auto* title = SidebarWindow::findNamed(*billing, QStringLiteral("sidebarRowTitle"));
        auto* git = SidebarWindow::findNamed(*billing, QStringLiteral("sidebarRowGit"));
        auto* rollUp = SidebarWindow::findNamed(*billing, QStringLiteral("sidebarRowRollUp"));
        REQUIRE(title != nullptr);
        REQUIRE(git != nullptr);
        REQUIRE(rollUp != nullptr);
        auto const titleWidth = title->property("implicitWidth").toReal();
        auto const gitWidth = git->property("implicitWidth").toReal();
        REQUIRE(gitWidth > 0);

        // Room for the whole name and about half of the git text: the git text elides, the name does not.
        auto const cell = scene.style("cellWidth");
        billing->setWidth(std::ceil(title->x() + titleWidth + rollUp->width() + (gitWidth / 2) + (3 * cell)));
        CHECK(waitForTrailLayout(*git, *rollUp));
        INFO("row width " << billing->width() << ", title " << title->width() << "/" << titleWidth << ", git "
                          << git->width() << "/" << gitWidth);
        CHECK(title->width() >= titleWidth);
        CHECK(git->isVisible());
        CHECK(git->width() < gitWidth);

        // Less room than the name needs: the git text is gone and only now does the name elide.
        billing->setWidth(title->x() + (titleWidth / 2));
        CHECK(waitForTrailLayout(*git, *rollUp));
        CHECK(!git->isVisible());
        CHECK(title->width() < titleWidth);
    }
}

TEST_CASE("the chip shows the label and a marker when another workspace needs you (offscreen)",
          "[contour][gui][qml][sidebar]")
{
    for (auto const row: drawnRows())
    {
        INFO("row " << static_cast<int>(row));
        for (auto const asks: { BillingAsks::Permission, BillingAsks::Nothing })
        {
            auto const needsYou = asks == BillingAsks::Permission;
            INFO("billing needs you: " << needsYou);
            auto scene = SidebarWindow { { .row = row,
                                           .visibility = contour::config::SidebarVisibility::Hidden,
                                           .shownWorkspace = 2,
                                           .billing = asks } };
            auto* chip = scene.item(QStringLiteral("workspaceChip"));
            REQUIRE(chip != nullptr);
            CHECK(chip->isVisible());
            auto* label = scene.item(QStringLiteral("workspaceChipLabel"));
            REQUIRE(label != nullptr);
            CHECK(label->property("text").toString() == QStringLiteral("docs"));
            auto* caret = scene.item(QStringLiteral("workspaceChipCaret"));
            REQUIRE(caret != nullptr);
            CHECK(caret->property("text").toString() == QStringLiteral("▾"));
            // A step between the name and the caret, so it never reads `docs▾`.
            CHECK(caret->property("leftPadding").toReal() > 0);
            // The strip follows the chip.
            CHECK(scene.item(QStringLiteral("tabStrip")) != nullptr);

            // billing works either way, which marks nothing; its permission prompt marks the chip.
            CHECK(scene.controller.sidebarModel().workspaceChipMarked() == needsYou);
            CHECK(scene.controller.otherWorkspacesUrgency()
                  == static_cast<int>(needsYou ? contour::window::Urgency::NeedsYou
                                               : contour::window::Urgency::Working));
            auto* marker = scene.item(QStringLiteral("workspaceChipMarker"));
            REQUIRE(marker != nullptr);
            CHECK(marker->isVisible() == needsYou);

            // A pop-up button: its workspaces drop down below it, not the centred switcher.
            scene.click(*chip);
            CHECK(scene.controller.workspaceDropdownAnchor.has_value());
            CHECK(scene.controller.switcherRequests == 0);
        }
    }
}

TEST_CASE("＋ triggers NewWorkspace (offscreen)", "[contour][gui][qml][sidebar]")
{
    for (auto const row: drawnRows())
    {
        INFO("row " << static_cast<int>(row));
        auto scene = SidebarWindow { { .row = row } };
        // The header bar's part above the sidebar holds it, or the sidebar's footer (a source list's, a
        // navigation pane's, the cell tree's), or its own title row.
        auto const inHeader = scene.token("titleInHeaderBar") != 0;
        auto const inFooter = scene.token("addInFooter") != 0;
        auto const name = [&] {
            if (inHeader)
                return QStringLiteral("headerNewWorkspaceButton");
            if (inFooter)
                return QStringLiteral("sidebarFooterNewWorkspaceButton");
            return QStringLiteral("sidebarNewWorkspaceButton");
        }();
        auto* plus = scene.item(name);
        REQUIRE(plus != nullptr);
        CHECK(plus->isVisible());
        scene.click(*plus);
        CHECK(scene.controller.newWorkspaces == 1);
    }
}

TEST_CASE("the tree and its rows are announced as a tree (offscreen)",
          "[contour][gui][qml][sidebar][accessibility]")
{
    for (auto const row: drawnRows())
    {
        INFO("row " << static_cast<int>(row));
        auto scene = SidebarWindow { { .row = row } };
        auto* tree = scene.item(QStringLiteral("sidebarTree"));
        REQUIRE(tree != nullptr);
        auto const* treeAccessible = accessibleOf(*tree);
        REQUIRE(treeAccessible != nullptr);
        CHECK(treeAccessible->property("role").toInt() == QAccessible::Tree);

        auto const& model = scene.controller.sidebarModel();
        auto const rows = scene.rows();
        REQUIRE(std::cmp_equal(rows.size(), model.rowCount()));
        for (auto const* item: rows)
        {
            auto const index = item->property("index").toInt();
            auto const* attached = accessibleOf(*item);
            REQUIRE(attached != nullptr);
            CHECK(attached->property("role").toInt() == QAccessible::TreeItem);
            CHECK(attached->property("name").toString()
                  == model.index(index).data(contour::window::SidebarModel::AccessibleNameRole).toString());
            CHECK(attached->property("description").toString()
                  == model.index(index)
                         .data(contour::window::SidebarModel::AccessibleDescriptionRole)
                         .toString());
            CHECK(attached->property("selected").toBool() == (index == model.currentIndex()));
        }
        // The name is the title; the kind, the fold and the status are the description.
        auto const* billing = accessibleOf(*rows.front());
        REQUIRE(billing != nullptr);
        CHECK(billing->property("name").toString() == QStringLiteral("billing"));
        CHECK(billing->property("description").toString()
              == QStringLiteral("Workspace, expanded, 1 pane needs you"));
    }
}

TEST_CASE("no spinner rotation under Reduce Motion (offscreen)", "[contour][gui][qml][sidebar][motion]")
{
    for (auto const row: drawnRows())
    {
        for (auto const motion:
             { contour::config::MotionPreference::Full, contour::config::MotionPreference::Reduced })
        {
            INFO("row " << static_cast<int>(row)
                        << ", reduced: " << (motion == contour::config::MotionPreference::Reduced));
            auto scene = SidebarWindow { { .row = row, .motion = motion } };
            auto* build = scene.rowTitled(QStringLiteral("build"));
            REQUIRE(build != nullptr);
            auto* glyph = SidebarWindow::findNamed(*build, QStringLiteral("sidebarRowGlyph"));
            REQUIRE(glyph != nullptr);
            auto* spinner = glyph->findChild<QObject*>(QStringLiteral("tabBadgeSpinner"));
            REQUIRE(spinner != nullptr);
            CHECK(spinner->property("running").toBool()
                  == (motion == contour::config::MotionPreference::Full));
            // Nor do the rows animate as they fold.
            auto* tree = scene.item(QStringLiteral("sidebarTree"));
            REQUIRE(tree != nullptr);
            auto* add = tree->property("add").value<QObject*>();
            REQUIRE(add != nullptr);
            CHECK(add->property("enabled").toBool() == (motion == contour::config::MotionPreference::Full));
            auto* displaced = tree->property("displaced").value<QObject*>();
            REQUIRE(displaced != nullptr);
            CHECK(displaced->property("enabled").toBool()
                  == (motion == contour::config::MotionPreference::Full));
        }
    }
}

TEST_CASE("F2 on a tab row renames the tab (offscreen)", "[contour][gui][qml][sidebar]")
{
    for (auto const row: drawnRows())
    {
        INFO("row " << static_cast<int>(row));
        auto scene = SidebarWindow { { .row = row } };
        auto* tree = scene.item(QStringLiteral("sidebarTree"));
        REQUIRE(tree != nullptr);
        scene.controller.focusSidebar();
        REQUIRE(QTest::qWaitFor([&] { return tree->hasActiveFocus(); }, 1000));
        REQUIRE(tree->property("currentIndex").toInt() == 1); // the claude tab

        QTest::keyClick(scene.window, Qt::Key_F2);
        QQuickItem* field = nullptr;
        REQUIRE(QTest::qWaitFor(
            [&] { return (field = scene.item(QStringLiteral("sidebarRenameField"))) != nullptr; }, 1000));
        CHECK(field->property("text").toString() == QStringLiteral("claude"));
        for (auto const key: QStringLiteral("agent"))
            QTest::keyClick(scene.window, key.toLatin1());
        QTest::keyClick(scene.window, Qt::Key_Return);
        QCoreApplication::processEvents();
        auto const& renamed = scene.controller.sidebarHost().renamedTabs;
        REQUIRE(renamed.size() == 1);
        CHECK(renamed[0].first.workspace == 1);
        CHECK(renamed[0].first.tab == 10);
        CHECK(renamed[0].second == "agent");
    }
}

TEST_CASE("RenameWorkspace shows a hidden sidebar and opens the editor (offscreen)",
          "[contour][gui][qml][sidebar]")
{
    for (auto const row: drawnRows())
    {
        INFO("row " << static_cast<int>(row));
        auto scene = SidebarWindow { { .row = row } };
        scene.controller.toggleSidebar();
        REQUIRE(QTest::qWaitFor([&] { return scene.item(QStringLiteral("sidebar")) == nullptr; }, 1000));

        scene.controller.renameCurrentWorkspace();
        QQuickItem* field = nullptr;
        REQUIRE(QTest::qWaitFor(
            [&] { return (field = scene.item(QStringLiteral("sidebarRenameField"))) != nullptr; }, 1000));
        CHECK(field->property("text").toString() == QStringLiteral("billing"));
        CHECK(QTest::qWaitFor([&] { return field->hasActiveFocus(); }, 1000));
    }
}

TEST_CASE("workspace and tab rows drag, pane rows do not (offscreen)", "[contour][gui][qml][sidebar]")
{
    auto scene = SidebarWindow { { .row = drawnRows().front() } };
    for (auto const& title: { QStringLiteral("billing"), QStringLiteral("claude") })
    {
        auto* item = scene.rowTitled(title);
        REQUIRE(item != nullptr);
        auto* drag = item->findChild<QObject*>(QStringLiteral("sidebarRowDragHandler"));
        REQUIRE(drag != nullptr);
        CHECK(drag->property("enabled").toBool());
    }
}

TEST_CASE("Left and Right collapse and expand the tree (offscreen)", "[contour][gui][qml][sidebar]")
{
    for (auto const row: drawnRows())
    {
        INFO("row " << static_cast<int>(row));
        auto scene = SidebarWindow { { .row = row } };
        auto* tree = scene.item(QStringLiteral("sidebarTree"));
        REQUIRE(tree != nullptr);
        auto const& model = scene.controller.sidebarModel();
        scene.controller.focusSidebar();
        REQUIRE(QTest::qWaitFor([&] { return tree->hasActiveFocus(); }, 1000));
        REQUIRE(tree->property("currentIndex").toInt() == 1);

        // A tab with nothing to fold goes to its workspace; there Left folds it and Right unfolds it, and a
        // second Right goes into it.
        QTest::keyClick(scene.window, Qt::Key_Left);
        CHECK(tree->property("currentIndex").toInt() == 0);
        QTest::keyClick(scene.window, Qt::Key_Left);
        CHECK(model.rowCount() == 3);
        QTest::keyClick(scene.window, Qt::Key_Right);
        CHECK(model.rowCount() == 5);
        CHECK(tree->property("currentIndex").toInt() == 0);
        QTest::keyClick(scene.window, Qt::Key_Right);
        CHECK(tree->property("currentIndex").toInt() == 1);
    }
}

TEST_CASE("the chip names what the other workspaces ask (offscreen)",
          "[contour][gui][qml][sidebar][accessibility]")
{
    struct Case
    {
        BillingAsks billing;
        bool marked;
        char const* name;
    };
    for (auto const row: drawnRows())
    {
        for (auto const& [billing, marked, name]:
             { Case { BillingAsks::Permission, true, "Workspace docs, another workspace needs you" },
               Case { BillingAsks::Done, true, "Workspace docs, another workspace done" },
               Case { BillingAsks::FailedSeen, false, "Workspace docs" } })
        {
            INFO("row " << static_cast<int>(row) << ", " << name);
            auto scene = SidebarWindow { { .row = row,
                                           .visibility = contour::config::SidebarVisibility::Hidden,
                                           .shownWorkspace = 2,
                                           .billing = billing } };
            auto* chip = scene.item(QStringLiteral("workspaceChip"));
            REQUIRE(chip != nullptr);
            auto const* attached = accessibleOf(*chip);
            REQUIRE(attached != nullptr);
            CHECK(attached->property("name").toString() == QString::fromLatin1(name));
            auto* marker = scene.item(QStringLiteral("workspaceChipMarker"));
            REQUIRE(marker != nullptr);
            CHECK(marker->isVisible() == marked);
        }
    }
}

TEST_CASE("the + is in reach whether the header bar draws the title row or not (offscreen)",
          "[contour][gui][qml][sidebar]")
{
    for (auto const row: drawnRows())
    {
        for (auto const tabBar:
             { contour::config::TabBarVisibility::Always, contour::config::TabBarVisibility::Never })
        {
            INFO("row " << static_cast<int>(row) << ", strip " << static_cast<int>(tabBar));
            auto scene = SidebarWindow {
                { .row = row, .placement = contour::config::TabPlacement::Top, .tabBar = tabBar }
            };
            auto const shown = [&](QString const& name) {
                auto* item = scene.item(name);
                return item != nullptr && item->isVisible() ? item : nullptr;
            };
            auto const found = std::array { shown(QStringLiteral("headerNewWorkspaceButton")),
                                            shown(QStringLiteral("sidebarNewWorkspaceButton")),
                                            shown(QStringLiteral("sidebarFooterNewWorkspaceButton")) };
            // Exactly one of them, wherever the row keeps it.
            CHECK(std::ranges::count(found, nullptr) == 2);
            auto const at = std::ranges::find_if(found, [](QQuickItem* item) { return item != nullptr; });
            REQUIRE(at != found.end());
            auto* plus = *at;
            REQUIRE(plus != nullptr);
            scene.click(*plus);
            CHECK(scene.controller.newWorkspaces == 1);
        }
    }
}

TEST_CASE("dragging the divider saves the width once, on release (offscreen)", "[contour][gui][qml][sidebar]")
{
    for (auto const row: drawnRows())
    {
        INFO("row " << static_cast<int>(row));
        auto scene = SidebarWindow { { .row = row } };
        auto* sidebar = scene.item(QStringLiteral("sidebar"));
        auto* handle = scene.item(QStringLiteral("sidebarResizeHandle"));
        REQUIRE(sidebar != nullptr);
        REQUIRE(handle != nullptr);
        auto const start = sidebar->width();
        auto const from = handle->mapToScene(QPointF { handle->width() / 2, handle->height() / 2 }).toPoint();
        QTest::mousePress(scene.window, Qt::LeftButton, Qt::NoModifier, from);
        for (auto const step: std::views::iota(1, 9))
            QTest::mouseMove(scene.window, from + QPoint { step * 10, 0 });
        CHECK(scene.controller.savedSidebarWidths.empty());
        CHECK(sidebar->width() > start);
        QTest::mouseRelease(scene.window, Qt::LeftButton, Qt::NoModifier, from + QPoint { 80, 0 });
        QCoreApplication::processEvents();
        REQUIRE(scene.controller.savedSidebarWidths.size() == 1);
        CHECK(std::abs(scene.controller.savedSidebarWidths[0] - sidebar->width()) <= 1);
    }
}

TEST_CASE("the hover card closes when the pointer leaves it (offscreen)", "[contour][gui][qml][sidebar]")
{
    auto scene = SidebarWindow { { .row = drawnRows().front() } };
    auto* claude = scene.rowTitled(QStringLiteral("claude"));
    REQUIRE(claude != nullptr);
    auto* card = scene.root->findChild<QObject*>(QStringLiteral("sidebarHoverCard"));
    REQUIRE(card != nullptr);
    auto const over = claude->mapToScene(QPointF { claude->width() / 2, claude->height() / 2 }).toPoint();
    QTest::mouseMove(scene.window, over - QPoint { 0, 1 });
    QTest::mouseMove(scene.window, over);
    REQUIRE(QTest::qWaitFor([&] { return card->property("opened").toBool(); }, 3000));
    auto const* detail =
        SidebarWindow::findNamed(*scene.window->contentItem(), QStringLiteral("sidebarHoverCardDetail"));
    if (detail != nullptr)
        CHECK(detail->property("text").toString() == QStringLiteral("claude · /src/billing · 0:30"));

    // Onto the card, then off it into the terminal: it closes.
    auto* surface = qobject_cast<QQuickItem*>(card->property("contentItem").value<QObject*>());
    REQUIRE(surface != nullptr);
    auto const onCard =
        surface->mapToScene(QPointF { surface->width() / 2, surface->height() / 2 }).toPoint();
    QTest::mouseMove(scene.window, onCard);
    QTest::mouseMove(scene.window, QPoint { scene.window->width() - 10, scene.window->height() - 10 });
    CHECK(QTest::qWaitFor([&] { return !card->property("opened").toBool(); }, 3000));
}

TEST_CASE("the Terminal row keeps the sidebar's text on the cell grid (offscreen)",
          "[contour][gui][qml][sidebar]")
{
    auto const rows = drawnRows();
    if (!std::ranges::contains(rows, ChromeStyle::Terminal))
        return; // the ContourTui pass covers it
    auto scene = SidebarWindow { { .row = ChromeStyle::Terminal } };
    auto* sidebar = scene.item(QStringLiteral("sidebar"));
    REQUIRE(sidebar != nullptr);
    auto const cell = scene.style("cellWidth");
    REQUIRE(cell > 0);
    auto const onGrid = [&](QQuickItem const& item) {
        auto const x = item.mapToItem(sidebar, QPointF { 0, 0 }).x();
        return std::abs(std::remainder(x, cell)) < 0.01;
    };
    CHECK(std::abs(std::remainder(sidebar->width(), cell)) < 0.01);
    for (auto* row: scene.rows())
    {
        INFO("row " << row->property("title").toString().toStdString());
        for (auto const& name: { QStringLiteral("sidebarRowTitle"),
                                 QStringLiteral("sidebarRowTree"),
                                 QStringLiteral("sidebarRowTime") })
            if (auto* part = SidebarWindow::findNamed(*row, name); part != nullptr && part->isVisible())
            {
                INFO(name.toStdString()
                     << " at " << part->mapToItem(sidebar, QPointF { 0, 0 }).x() << ", cell " << cell);
                CHECK(onGrid(*part));
            }
    }
}

TEST_CASE("rows that outlive their model while the window closes read nothing from it (offscreen)",
          "[contour][gui][qml][sidebar]")
{
    // The application's window outlives its controller, which takes the sidebar model with it: rows that
    // still exist then must not read the model in their bindings.
    for (auto const row: drawnRows())
    {
        INFO("row " << static_cast<int>(row));
        auto scene = SidebarWindow { { .row = row, .paneTree = PaneTree::None } };
        auto& listed = scene.controller.sidebarSource().listed;
        auto const rowsShown = [&] {
            return QTest::qWaitFor(
                [&] {
                    return std::cmp_equal(scene.rows().size(), scene.controller.sidebarModel().rowCount());
                },
                1000);
        };

        // A workspace opens with a split tab (a tab row and two pane rows), and a tab of it closes.
        auto ops = contour::test::directoryWorkspaceOf(
            3,
            "ops",
            { contour::test::directoryPaneOf(30, "deploy"), contour::test::directoryPaneOf(32, "logs") });
        ops.tabs.front().panes.push_back(contour::test::directoryPaneOf(31, "tail"));
        listed.workspaces.push_back(std::move(ops));
        scene.controller.sidebarModel().refreshNow();
        REQUIRE(rowsShown());
        REQUIRE(scene.rowTitled(QStringLiteral("tail")) != nullptr);
        listed.workspaces.back().tabs.pop_back();
        scene.controller.sidebarModel().refreshNow();
        REQUIRE(rowsShown());
        CHECK(scene.rowTitled(QStringLiteral("logs")) == nullptr);

        // A workspace closes.
        listed.workspaces.erase(listed.workspaces.begin());
        scene.controller.sidebarModel().refreshNow();
        REQUIRE(rowsShown());
        CHECK(scene.rowTitled(QStringLiteral("billing")) == nullptr);
        REQUIRE_FALSE(scene.rows().empty());

        scene.closeController();

        auto sidebarDiagnostics = QStringList {};
        std::ranges::copy_if(
            scene.warnings.messages(), std::back_inserter(sidebarDiagnostics), isSidebarDiagnostic);
        INFO("QML warnings: " << sidebarDiagnostics.join(QStringLiteral("\n")).toStdString());
        CHECK(sidebarDiagnostics.isEmpty());
    }
}

TEST_CASE("the sidebar toggle stays at one spot while it hides and shows the sidebar (offscreen)",
          "[contour][gui][qml][sidebar]")
{
    for (auto const row: drawnRows())
    {
        INFO("row " << static_cast<int>(row));
        // Shown once and hidden by the toggle, so the sidebar has a width to come back at.
        auto scene = SidebarWindow { { .row = row } };
        // macOS draws its traffic lights over the band's leading end, and over a sidebar beside the band.
        if (row == ChromeStyle::MacOS)
            scene.controller.setNativeControlsInset(TrafficLightsInset);
        scene.controller.toggleSidebar();
        QCoreApplication::processEvents();
        REQUIRE(scene.item(QStringLiteral("sidebar")) == nullptr);

        // Hidden, the sidebar takes its toggle with it nowhere: the band holds it.
        auto* hidden = scene.visibleItem(QStringLiteral("sidebarToggle"));
        REQUIRE(hidden != nullptr);
        auto const* hiddenAccessible = accessibleOf(*hidden);
        REQUIRE(hiddenAccessible != nullptr);
        CHECK(hiddenAccessible->property("name").toString() == QStringLiteral("Show sidebar"));
        CHECK(hiddenAccessible->property("checkable").toBool());
        CHECK_FALSE(hiddenAccessible->property("checked").toBool());
        CHECK(hidden->property("toolTipText").toString() == QStringLiteral("Show sidebar"));
        CHECK(hidden->property("toolTipCommand").toString() == QStringLiteral("ToggleSidebar"));
        auto const spot = sceneRectOf(*hidden);
        CHECK(spot.width() > 0);
        CHECK(spot.height() > 0);
        INFO("hidden toggle in " << hidden->parentItem()->objectName().toStdString() << " at x " << spot.x());
        if (row == ChromeStyle::MacOS)
            CHECK(spot.left() >= TrafficLightsInset);

        scene.click(*hidden);
        REQUIRE(scene.controller.sidebarVisibility()
                == static_cast<int>(contour::config::SidebarVisibility::Shown));
        REQUIRE(scene.item(QStringLiteral("sidebar")) != nullptr);

        // Shown, the toggle is where the pointer left it, and says what a second click does.
        auto* shown = scene.visibleItem(QStringLiteral("sidebarToggle"));
        REQUIRE(shown != nullptr);
        auto const* shownAccessible = accessibleOf(*shown);
        REQUIRE(shownAccessible != nullptr);
        CHECK(shownAccessible->property("name").toString() == QStringLiteral("Hide sidebar"));
        // A toggle says whether what it toggles is on.
        CHECK(shownAccessible->property("checkable").toBool());
        CHECK(shownAccessible->property("checked").toBool());
        auto const shownSpot = sceneRectOf(*shown);
        INFO("hidden at " << spot.x() << "," << spot.y() << " " << spot.width() << "x" << spot.height()
                          << ", shown at " << shownSpot.x() << "," << shownSpot.y() << " "
                          << shownSpot.width() << "x" << shownSpot.height());
        CHECK(std::abs(shownSpot.x() - spot.x()) < 0.5);
        CHECK(std::abs(shownSpot.y() - spot.y()) < 0.5);
        CHECK(std::abs(shownSpot.width() - spot.width()) < 0.5);
        CHECK(std::abs(shownSpot.height() - spot.height()) < 0.5);

        // The same spot, clicked again, hides the sidebar.
        QTest::mouseClick(scene.window, Qt::LeftButton, Qt::NoModifier, spot.center().toPoint());
        QCoreApplication::processEvents();
        CHECK(scene.controller.sidebarVisibility()
              == static_cast<int>(contour::config::SidebarVisibility::Hidden));
        CHECK(scene.controller.sidebarToggles == 3);
        CHECK(scene.visibleItem(QStringLiteral("sidebarToggle")) != nullptr);
    }
}

TEST_CASE("the Terminal row's sidebar toggle is whole cells wide (offscreen)", "[contour][gui][qml][sidebar]")
{
    if (!std::ranges::contains(drawnRows(), ChromeStyle::Terminal))
        return; // the ContourTui pass covers it
    auto scene = SidebarWindow { { .row = ChromeStyle::Terminal,
                                   .visibility = contour::config::SidebarVisibility::Hidden } };
    auto const cell = scene.style("cellWidth");
    REQUIRE(cell > 0);
    auto const* toggle = scene.visibleItem(QStringLiteral("sidebarToggle"));
    REQUIRE(toggle != nullptr);
    CHECK(std::abs(std::remainder(toggle->width(), cell)) < 0.01);
    CHECK(std::abs(std::remainder(sceneRectOf(*toggle).x(), cell)) < 0.01);
}

TEST_CASE("the sidebar's header is the title bar's, its buttons excepted (offscreen)",
          "[contour][gui][qml][sidebar]")
{
    for (auto const row: drawnRows())
    {
        INFO("row " << static_cast<int>(row));
        auto scene = SidebarWindow { { .row = row } };
        if (row == ChromeStyle::MacOS)
            scene.controller.setNativeControlsInset(TrafficLightsInset);
        QCoreApplication::processEvents();

        // The header bar's part above the sidebar, or the sidebar's own title row.
        auto const inHeader = scene.token("titleInHeaderBar") != 0;
        auto* header =
            scene.item(inHeader ? QStringLiteral("headerSidebarPart") : QStringLiteral("sidebarTitleRow"));
        REQUIRE(header != nullptr);
        REQUIRE(header->isVisible());
        auto* gestures = SidebarWindow::findNamed(*header,
                                                  inHeader ? QStringLiteral("headerSidebarGestures")
                                                           : QStringLiteral("sidebarTitleGestures"));
        REQUIRE(gestures != nullptr);
        auto* drag = gestures->findChild<QObject*>(QStringLiteral("titleBarGesturesDrag"));
        REQUIRE(drag != nullptr);
        CHECK(drag->property("enabled").toBool());
        QSignalSpy const activations(drag, SIGNAL(activeChanged()));
        QSignalSpy const windowMenus(&scene.controller, SIGNAL(titleBarContextMenuRequested()));

        // Its empty places, the title among them, move the window, maximise it on a double-click and open the
        // window's menu on a right-click (below).
        auto const* title = SidebarWindow::findNamed(
            *header, inHeader ? QStringLiteral("headerSidebarTitle") : QStringLiteral("sidebarTitle"));
        REQUIRE(title != nullptr);
        auto const onTitle = sceneRectOf(*title).center().toPoint();
        REQUIRE(sceneRectOf(*header).contains(onTitle));
        dragFrom(*scene.window, onTitle);
        CHECK_FALSE(activations.isEmpty());
        QTest::mouseDClick(scene.window, Qt::LeftButton, Qt::NoModifier, onTitle);
        QCoreApplication::processEvents();
        CHECK(scene.controller.maximizeToggles == 1);

        // Beside the band, the strip behind the traffic lights drags too.
        if (row == ChromeStyle::MacOS && scene.token("besideBand") != 0)
        {
            auto const activationsBefore = activations.count();
            auto const behindLights = QPoint { static_cast<int>(TrafficLightsInset / 2),
                                               static_cast<int>(sceneRectOf(*header).center().y()) };
            dragFrom(*scene.window, behindLights);
            CHECK(activations.count() > activationsBefore);
        }

        // Its buttons keep their clicks: a drag that starts on one moves no window.
        auto const activationsBefore = activations.count();
        auto buttons = std::vector<QQuickItem*> {};
        for (auto const& name:
             { inHeader ? QStringLiteral("headerNewWorkspaceButton")
                        : QStringLiteral("sidebarNewWorkspaceButton"),
               inHeader ? QStringLiteral("headerSidebarFilterButton") : QStringLiteral("sidebarFilterButton"),
               QStringLiteral("sidebarToggle") })
            if (auto* button = SidebarWindow::findNamed(*header, name);
                button != nullptr && button->isVisible())
                buttons.push_back(button);
        // The filter button stands in the title row on every row; the "+" and the toggle only on some.
        REQUIRE_FALSE(buttons.empty());
        for (auto* button: buttons)
        {
            INFO(button->objectName().toStdString());
            dragFrom(*scene.window, sceneRectOf(*button).center().toPoint());
            CHECK(activations.count() == activationsBefore);
        }
        scene.click(*buttons.front());
        CHECK((scene.controller.newWorkspaces == 1
               || scene.item(QStringLiteral("sidebarFilterField"))->isVisible()));

        // Last, since the menu it opens takes the pointer.
        QTest::mouseClick(scene.window, Qt::RightButton, Qt::NoModifier, onTitle);
        QCoreApplication::processEvents();
        CHECK(windowMenus.count() == 1);
    }
}

TEST_CASE("the Menu key and Shift+F10 open the focused row's menu (offscreen)",
          "[contour][gui][qml][sidebar]")
{
    for (auto const row: drawnRows())
    {
        INFO("row " << static_cast<int>(row));
        auto scene = SidebarWindow { { .row = row } };
        auto* tree = scene.item(QStringLiteral("sidebarTree"));
        REQUIRE(tree != nullptr);
        scene.controller.focusSidebar();
        REQUIRE(QTest::qWaitFor([&] { return tree->hasActiveFocus(); }, 1000));
        // Home goes to the first row, the billing workspace; End to the last, docs' tab.
        QTest::keyClick(scene.window, Qt::Key_End);
        CHECK(tree->property("currentIndex").toInt() == scene.controller.sidebarModel().rowCount() - 1);
        QTest::keyClick(scene.window, Qt::Key_Home);
        CHECK(tree->property("currentIndex").toInt() == 0);

        QTest::keyClick(scene.window, Qt::Key_Menu);
        QTest::keyClick(scene.window, Qt::Key_F10, Qt::ShiftModifier);
        QCoreApplication::processEvents();
        auto const& menus = scene.controller.sidebarHost().menus;
        REQUIRE(menus.size() == 2);
        CHECK(menus[0].kind == contour::window::SidebarRowKind::Workspace);
        CHECK(menus[0].workspace == 1);
        CHECK(menus[1] == menus[0]);
        // Under the focused row, inset from its leading edge, wherever the pointer is.
        auto* const current = tree->property("currentItem").value<QQuickItem*>();
        REQUIRE(current != nullptr);
        auto const bottomLeft = current->mapToItem(nullptr, QPointF { 0, current->height() });
        auto const& positions = scene.controller.sidebarHost().menuPositions;
        REQUIRE(positions.size() == 2);
        CHECK(positions[0].y() == bottomLeft.y());
        CHECK(positions[0].x() > bottomLeft.x());
        CHECK(positions[0].x() < bottomLeft.x() + current->width());
        CHECK(positions[1] == positions[0]);
    }
}

TEST_CASE("the keyboard is back on its row once the row's menu closes (offscreen)",
          "[contour][gui][qml][sidebar]")
{
    for (auto const row: drawnRows())
    {
        INFO("row " << static_cast<int>(row));
        auto scene = SidebarWindow { { .row = row } };
        auto* tree = scene.item(QStringLiteral("sidebarTree"));
        auto* menu = scene.root->findChild<QObject*>(QStringLiteral("sidebarContextMenu"));
        REQUIRE(tree != nullptr);
        REQUIRE(menu != nullptr);
        scene.controller.focusSidebar();
        REQUIRE(QTest::qWaitFor([&] { return tree->hasActiveFocus(); }, 1000));
        QTest::keyClick(scene.window, Qt::Key_End);
        auto const focusedRow = tree->property("currentIndex").toInt();

        // What the controller asks once the Menu key named the row (WindowController.openSidebarContextMenu).
        scene.controller.openSidebarContextMenu(
            QVariantList { QVariantMap { { QStringLiteral("kind"), "command" },
                                         { QStringLiteral("title"), "Mark Seen" },
                                         { QStringLiteral("enabled"), true },
                                         { QStringLiteral("checkable"), false },
                                         { QStringLiteral("checked"), false },
                                         { QStringLiteral("actionId"), 1 } } },
            focusedRow,
            QPointF { 40, 120 });
        REQUIRE(QTest::qWaitFor([&] { return menu->property("opened").toBool(); }, 2000));
        CHECK_FALSE(tree->hasActiveFocus());

        QTest::keyClick(scene.window, Qt::Key_Escape);
        REQUIRE(QTest::qWaitFor([&] { return !menu->property("opened").toBool(); }, 2000));
        CHECK(QTest::qWaitFor([&] { return tree->hasActiveFocus(); }, 1000));
        CHECK(tree->property("currentIndex").toInt() == focusedRow);
    }
}

TEST_CASE("the filter button opens the filter, its hint drawn past the caret (offscreen)",
          "[contour][gui][qml][sidebar]")
{
    for (auto const row: drawnRows())
    {
        INFO("row " << static_cast<int>(row));
        auto scene = SidebarWindow { { .row = row } };
        auto const inHeader = scene.token("titleInHeaderBar") != 0;
        auto* button = scene.visibleItem(inHeader ? QStringLiteral("headerSidebarFilterButton")
                                                  : QStringLiteral("sidebarFilterButton"));
        REQUIRE(button != nullptr);
        scene.click(*button);
        auto* field = scene.item(QStringLiteral("sidebarFilterField"));
        REQUIRE(field != nullptr);
        CHECK(QTest::qWaitFor([&] { return field->isVisible() && field->hasActiveFocus(); }, 1000));
        // The style's placeholder would put the caret on the hint's first letter; the sidebar draws its own.
        CHECK(field->property("placeholderText").toString().isEmpty());
        auto* hint = scene.item(QStringLiteral("sidebarFilterHint"));
        REQUIRE(hint != nullptr);
        CHECK(hint->isVisible());
        QTest::keyClick(scene.window, 'b');
        CHECK_FALSE(hint->isVisible());
        CHECK(scene.controller.sidebarModel().isFiltered());
    }
}

TEST_CASE("the sidebar's buttons are icons in an icon bar, and the + is not the new tab's (offscreen)",
          "[contour][gui][qml][sidebar]")
{
    for (auto const row: drawnRows())
    {
        INFO("row " << static_cast<int>(row));
        auto scene = SidebarWindow { { .row = row } };
        auto const* toggle = scene.visibleItem(QStringLiteral("sidebarToggle"));
        REQUIRE(toggle != nullptr);
        auto const iconOf = [](QQuickItem const& button) {
            return button.property("iconSource").toString();
        };
        switch (row)
        {
            case ChromeStyle::ContourLinux: {
                // The header bar's icons, themed, as its own items are.
                CHECK(toggle->property("iconName").toString() == QStringLiteral("sidebar-show-symbolic"));
                auto const* plus = scene.visibleItem(QStringLiteral("headerNewWorkspaceButton"));
                REQUIRE(plus != nullptr);
                CHECK(plus->property("iconName").toString() == QStringLiteral("folder-new-symbolic"));
                CHECK_FALSE(iconOf(*plus).isEmpty());
                break;
            }
            case ChromeStyle::MacOS:
            case ChromeStyle::Fluent: CHECK_FALSE(iconOf(*toggle).isEmpty()); break;
            case ChromeStyle::Terminal: CHECK(iconOf(*toggle).isEmpty()); break;
        }
    }
}

TEST_CASE("a selection follows its row's convention for keyboard focus (offscreen)",
          "[contour][gui][qml][sidebar]")
{
    for (auto const row: drawnRows())
    {
        INFO("row " << static_cast<int>(row));
        auto scene = SidebarWindow { { .row = row } };
        auto const selectedRow = [&] {
            auto const rows = scene.rows();
            auto const at = std::ranges::find_if(
                rows, [](QQuickItem const* item) { return item->property("selected").toBool(); });
            REQUIRE(at != rows.end());
            return *at;
        };
        auto const fillOf = [&] {
            auto* selection = SidebarWindow::findNamed(*selectedRow(), QStringLiteral("sidebarRowSelection"));
            REQUIRE(selection != nullptr);
            CHECK(selection->isVisible());
            return selection->property("color").value<QColor>();
        };
        auto const* colors = [&] {
            auto const* style =
                scene.engine.rootContext()->contextProperty(QStringLiteral("chromeStyle")).value<QObject*>();
            REQUIRE(style != nullptr);
            return style->property("colors").value<QObject*>();
        }();
        REQUIRE(colors != nullptr);
        // The terminal has the keyboard: the quiet fill.
        CHECK(fillOf() == colors->property("sidebarSelectionFillInactive").value<QColor>());
        scene.controller.focusSidebar();
        auto* tree = scene.item(QStringLiteral("sidebarTree"));
        REQUIRE(QTest::qWaitFor([&] { return tree->hasActiveFocus(); }, 1000));
        // The accent fill follows the keyboard on macOS and the Terminal row; Linux keeps a neutral fill and
        // Fluent a grey one with its pill, focused or not.
        auto const followsKeyboard = row == ChromeStyle::MacOS || row == ChromeStyle::Terminal;
        CHECK(fillOf()
              == colors->property(followsKeyboard ? "sidebarSelectionFill" : "sidebarSelectionFillInactive")
                     .value<QColor>());
        auto* pill = SidebarWindow::findNamed(*selectedRow(), QStringLiteral("sidebarRowAccentPill"));
        REQUIRE(pill != nullptr);
        CHECK(pill->isVisible() == (row == ChromeStyle::Fluent));
    }
}

TEST_CASE("a row shows hover where its platform does (offscreen)", "[contour][gui][qml][sidebar]")
{
    for (auto const row: drawnRows())
    {
        INFO("row " << static_cast<int>(row));
        auto scene = SidebarWindow { { .row = row } };
        auto* docs = scene.rowTitled(QStringLiteral("docs"));
        REQUIRE(docs != nullptr);
        auto const* feedback = SidebarWindow::findNamed(*docs, QStringLiteral("sidebarRowFeedback"));
        REQUIRE(feedback != nullptr);
        CHECK_FALSE(feedback->isVisible());
        auto const centre = docs->mapToScene(QPointF { docs->width() / 2, docs->height() / 2 }).toPoint();
        QTest::mouseMove(scene.window, centre);
        QCoreApplication::processEvents();
        CHECK(QTest::qWaitFor([&] { return feedback->isVisible() == (scene.token("rowFeedback") != 0); },
                              1000));
    }
}

TEST_CASE("the footer leaves out counts that are zero (offscreen)", "[contour][gui][qml][sidebar]")
{
    for (auto const row: drawnRows())
    {
        INFO("row " << static_cast<int>(row));
        {
            auto scene = SidebarWindow { { .row = row } };
            auto const* counts = scene.item(QStringLiteral("sidebarFooterCounts"));
            REQUIRE(counts != nullptr);
            CHECK(counts->property("text").toString() == QStringLiteral("1 needs you · 1 working"));
        }
        {
            auto scene = SidebarWindow { { .row = row, .billing = BillingAsks::Nothing } };
            auto const* counts = scene.item(QStringLiteral("sidebarFooterCounts"));
            REQUIRE(counts != nullptr);
            CHECK(counts->property("text").toString() == QStringLiteral("1 working"));
        }
    }
}

TEST_CASE("the chip drops its workspaces down below itself (offscreen)", "[contour][gui][qml][sidebar]")
{
    for (auto const row: drawnRows())
    {
        INFO("row " << static_cast<int>(row));
        auto scene =
            SidebarWindow { { .row = row, .visibility = contour::config::SidebarVisibility::Hidden } };
        auto* chip = scene.visibleItem(QStringLiteral("workspaceChip"));
        REQUIRE(chip != nullptr);
        scene.click(*chip);
        REQUIRE(scene.controller.workspaceDropdownAnchor.has_value());
        auto const chipRect = sceneRectOf(*chip);
        auto const& anchor = *scene.controller.workspaceDropdownAnchor;
        // The chip itself, and the gap the dropdown hangs below it by: it opens under the chip, not centred.
        CHECK(anchor.left() == Catch::Approx(chipRect.left()));
        CHECK(anchor.top() == Catch::Approx(chipRect.top()));
        CHECK(anchor.width() == Catch::Approx(chipRect.width()));
        CHECK(anchor.bottom() >= chipRect.bottom());
        auto const* attached = accessibleOf(*chip);
        REQUIRE(attached != nullptr);
        CHECK(attached->property("role").toInt() == QAccessible::ButtonMenu);
    }
}

TEST_CASE("the selection is inset alike on both sides and keeps the row's trailing padding inside it "
          "(offscreen)",
          "[contour][gui][qml][sidebar]")
{
    for (auto const row: drawnRows())
    {
        INFO("row " << static_cast<int>(row));
        auto scene = SidebarWindow { { .row = row } };
        auto* claude = scene.rowTitled(QStringLiteral("claude"));
        REQUIRE(claude != nullptr);
        auto* selection = SidebarWindow::findNamed(*claude, QStringLiteral("sidebarRowSelection"));
        REQUIRE(selection != nullptr);
        REQUIRE(selection->isVisible());
        auto const leftInset = selection->x();
        auto const rightInset = claude->width() - (selection->x() + selection->width());
        CHECK(rightInset == Catch::Approx(leftInset).margin(0.01));

        auto const trailPadding = claude->property("trailPadding").toReal();
        CHECK(trailPadding > 0);
        auto const selectionRight = selection->x() + selection->width();
        auto const rightEdgeOf = [&](char const* name) {
            auto* part = SidebarWindow::findNamed(*claude, QString::fromLatin1(name));
            REQUIRE(part != nullptr);
            REQUIRE(part->isVisible());
            return part->mapToItem(claude, QPointF { part->width(), 0 }).x();
        };
        // The time, and the reason's line, end their padding short of the selection's edge.
        CHECK(selectionRight - rightEdgeOf("sidebarRowTime") >= trailPadding - 0.01);
        CHECK(selectionRight - rightEdgeOf("sidebarRowReason") >= trailPadding - 0.01);
    }
}

TEST_CASE("the footer names its chord in the row's own notation (offscreen)", "[contour][gui][qml][sidebar]")
{
    for (auto const row: drawnRows())
    {
        INFO("row " << static_cast<int>(row));
        // Wide enough for the counts and the chord both.
        auto scene = SidebarWindow { { .row = row, .width = 480 } };
        auto const* chord = scene.item(QStringLiteral("sidebarFooterChord"));
        REQUIRE(chord != nullptr);
        auto const style = contour::command::shortcutStyleFor(contour::test::snapshot::rowHost(row).host);
        auto const expected = QString::fromStdString(contour::command::shortcutText(
            vtbackend::Modifiers { vtbackend::Modifier::Control, vtbackend::Modifier::Shift },
            contour::command::ShortcutInput { U'J' },
            style));
        // The macOS row spells it in glyphs, the others in words.
        CHECK(expected
              == (row == ChromeStyle::MacOS ? QStringLiteral("⌃⇧J") : QStringLiteral("Ctrl+Shift+J")));
        CHECK(chord->property("text").toString() == expected);
        CHECK(chord->isVisible());
    }
}

TEST_CASE("at the narrowest sidebar the footer keeps its counts whole, in fewer letters, and drops the chord "
          "(offscreen)",
          "[contour][gui][qml][sidebar]")
{
    for (auto const row: drawnRows())
    {
        INFO("row " << static_cast<int>(row));
        auto scene = SidebarWindow { { .row = row } };
        auto const narrowest = static_cast<int>(scene.token("minWidth"));
        auto const widest = static_cast<int>(scene.token("width"));
        INFO("min width " << narrowest << ", default " << widest);
        for (auto const [width, chordShown]:
             { std::pair { narrowest, false }, std::pair { std::max(widest, 320), true } })
        {
            INFO("sidebar width " << width);
            scene.controller.showSidebar(width);
            QTest::qWait(50);
            auto const* counts = scene.item(QStringLiteral("sidebarFooterCounts"));
            auto const* chord = scene.item(QStringLiteral("sidebarFooterChord"));
            REQUIRE(counts != nullptr);
            REQUIRE(chord != nullptr);
            // The narrowest sidebar has the same counts in fewer letters; none is ever elided.
            auto const text = counts->property("text").toString();
            if (chordShown)
                CHECK(text == QStringLiteral("1 needs you · 1 working"));
            else
                CHECK((text == QStringLiteral("1 needs you · 1 working")
                       || text == QStringLiteral("1 waiting · 1 busy")));
            INFO("counts width " << counts->width() << ", implicit "
                                 << counts->property("implicitWidth").toReal() << ", x " << counts->x());
            CHECK(!counts->property("truncated").toBool());
            CHECK(chord->isVisible() == chordShown);
        }
    }
}

TEST_CASE(
    "each close from the sidebar's menu opens one confirmation, and cancelling closes nothing (offscreen)",
    "[contour][gui][qml][sidebar]")
{
    using contour::window::CloseKind;
    struct Case
    {
        CloseKind kind;
        char const* heading;
        char const* accept;
    };
    for (auto const row: drawnRows())
    {
        auto scene = SidebarWindow { { .row = row } };
        for (auto const& [kind, heading, accept]: {
                 Case { CloseKind::Workspace, "Close the workspace “billing”?", "Close Workspace" },
                 Case { CloseKind::Tab, "Close the tab “billing”?", "Close Tab" },
                 Case { CloseKind::Pane, "Close the pane “billing”?", "Close Pane" },
             })
        {
            INFO("row " << static_cast<int>(row) << ", kind " << static_cast<int>(kind));
            auto const asked = [&]() -> QObject* {
                emit scene.controller.sidebarCloseConfirmationRequested(
                    static_cast<int>(kind), 1, 10, 11, QStringLiteral("billing"), QString {});
                auto* dialog = scene.root->findChild<QObject*>(QStringLiteral("closeConfirmation"));
                REQUIRE(dialog != nullptr);
                REQUIRE(QTest::qWaitFor([&] { return dialog->property("opened").toBool(); }, 1000));
                return dialog;
            };

            // Cancel closes nothing (Escape is the popup's own CloseOnEscape policy, KitDialog's).
            auto* dialog = asked();
            CHECK(dialog->property("heading").toString() == QString::fromUtf8(heading));
            CHECK(dialog->property("acceptText").toString() == QString::fromUtf8(accept));
            REQUIRE(QMetaObject::invokeMethod(dialog, "reject"));
            REQUIRE(QTest::qWaitFor([&] { return !dialog->property("opened").toBool(); }, 1000));
            CHECK(scene.controller.confirmedCloses.empty());

            // The destructive button closes exactly the node the menu was opened on.
            dialog = asked();
            REQUIRE(QMetaObject::invokeMethod(dialog, "accept"));
            REQUIRE(QTest::qWaitFor([&] { return !scene.controller.confirmedCloses.empty(); }, 1000));
            REQUIRE(scene.controller.confirmedCloses.size() == 1);
            auto const& closed = scene.controller.confirmedCloses.front();
            CHECK(closed.kind == static_cast<int>(kind));
            CHECK(closed.workspace == 1);
            CHECK(closed.tab == 10);
            CHECK(closed.pane == 11);
            scene.controller.confirmedCloses.clear();
        }
        CHECK(scene.warnings.count(contour::test::isQmlDiagnostic) == 0);
    }
}

TEST_CASE("dragging a workspace row with the mouse reorders the workspaces (offscreen)",
          "[contour][gui][qml][sidebar]")
{
    for (auto const row: drawnRows())
    {
        INFO("row " << static_cast<int>(row));
        auto scene = SidebarWindow { { .row = row } };
        auto* billing = scene.rowTitled(QStringLiteral("billing"));
        auto* docs = scene.rowTitled(QStringLiteral("docs"));
        REQUIRE(billing != nullptr);
        REQUIRE(docs != nullptr);

        // Dropped on the lower half of docs: billing goes after it.
        dragRowOnto(scene, *billing, *docs, 0.8);

        REQUIRE(scene.controller.sidebarHost().movedWorkspaces.size() == 1);
        CHECK(scene.controller.sidebarHost().movedWorkspaces.front().first == 1);
        CHECK(scene.controller.sidebarHost().movedWorkspaces.front().second == 2);
        CHECK(scene.controller.finishedDrags == 1);
        CHECK(scene.warnings.count(contour::test::isQmlDiagnostic) == 0);
    }
}

TEST_CASE("dragging a tab row onto another workspace with the mouse moves the tab (offscreen)",
          "[contour][gui][qml][sidebar]")
{
    for (auto const row: drawnRows())
    {
        INFO("row " << static_cast<int>(row));
        auto scene = SidebarWindow { { .row = row } };
        auto* claude = scene.rowTitled(QStringLiteral("claude"));
        auto* docs = scene.rowTitled(QStringLiteral("docs"));
        REQUIRE(claude != nullptr);
        REQUIRE(docs != nullptr);

        // Dropped on the middle of the docs row: the tab goes to the end of that workspace.
        dragRowOnto(scene, *claude, *docs, 0.5);

        REQUIRE(scene.controller.sidebarHost().movedTabs.size() == 1);
        auto const& [tab, target] = scene.controller.sidebarHost().movedTabs.front();
        CHECK(tab.workspace == 1);
        CHECK(tab.tab == 10);
        CHECK(target.first == 2);
        CHECK(scene.controller.finishedDrags == 1);
        CHECK(scene.warnings.count(contour::test::isQmlDiagnostic) == 0);
    }
}
