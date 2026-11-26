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
#include <contour/window/LocationChip.hpp>
#include <contour/window/NodeDrag.hpp>
#include <contour/window/SidebarControls.hpp>
#include <contour/window/SidebarModel.hpp>
#include <contour/window/SidebarTabColor.hpp>
#include <contour/window/StatusUrgency.hpp>

#include <vtbackend/core/Color.hpp>
#include <vtbackend/core/ColorPalette.hpp>
#include <vtbackend/vt/ProgramStatus.hpp>

#include <QtCore/QCoreApplication>
#include <QtCore/QEvent>
#include <QtCore/QMimeData>
#include <QtCore/QRegularExpression>
#include <QtCore/QStringList>
#include <QtGui/QAccessible>
#include <QtGui/QFont>
#include <QtGui/QFontDatabase>
#include <QtGui/QFontMetricsF>
#include <QtGui/QGuiApplication>
#include <QtGui/QStyleHints>
#include <QtQml/QQmlEngine>
#include <QtQuick/QQuickItem>
#include <QtQuick/QQuickWindow>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
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

/// Skips the running test where the platform's UI font is not installed: the sidebar's default and minimum
/// widths are sized against it, and a substitute's metrics decide other fits. Windows ships Segoe UI, but a
/// Windows build run elsewhere (wine) may lack it; every other platform's application font is its own.
void skipWithoutPlatformUiFont()
{
#ifdef _WIN32
    if (!QFontDatabase::hasFamily(QStringLiteral("Segoe UI")))
        SKIP("Segoe UI is not installed: the sidebar's widths are sized against it");
#endif
}

/// The font a scene's chrome is set in.
enum class SceneFont : uint8_t
{
    Application = 0, ///< The application's font, on every row.
    /// The row's own as a running window sets it: on the Terminal row the profile's terminal font, which is
    /// monospaced -- in a proportional face its cells would hold words a running window's do not.
    RowOwn,
    /// The application's font set a quarter wider, as a wide UI face (DejaVu Sans, many a Linux desktop's)
    /// sets the chrome's words: whatever the machine running the test has installed.
    Wide,
};

/// How much wider SceneFont::Wide sets each letter, in percent of the face's own spacing: wider than DejaVu
/// Sans sets "Workspaces" against Noto Sans or Cantarell.
constexpr auto WideLetterSpacing = 125.0;

/// @return The font a scene set in @p font chromes @p row with.
[[nodiscard]] QFont sceneFontOf(ChromeStyle row, SceneFont font)
{
    switch (font)
    {
        case SceneFont::Application: break;
        case SceneFont::RowOwn:
            if (row == ChromeStyle::Terminal)
                return QFontDatabase::systemFont(QFontDatabase::FixedFont);
            break;
        case SceneFont::Wide: {
            auto wide = QGuiApplication::font();
            wide.setLetterSpacing(QFont::PercentageSpacing, WideLetterSpacing);
            return wide;
        }
    }
    return QGuiApplication::font();
}

/// The setup @p row is loaded under, with the window controls its host shows.
[[nodiscard]] contour::test::ChromeSetup setupOf(
    ChromeStyle row,
    contour::config::MotionPreference motion = contour::config::MotionPreference::Full,
    SceneFont font = SceneFont::Application)
{
    auto setup = contour::test::ChromeSetup {
        .row = row,
        .font = sceneFontOf(row, font),
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

/// Whether billing's first tab, claude's, is split.
enum class ClaudeTab : uint8_t
{
    OnePane = 0, ///< claude alone.
    Split,       ///< claude and a second pane, review (key 12).
};

/// Whether billing's tabs are coloured.
enum class TabColours : uint8_t
{
    None = 0,   ///< No tab has a colour.
    Coloured,   ///< claude's tab is #4C5FD5 and build's #E8853A; docs' notes stays uncoloured.
    ClaudeOnly, ///< claude's tab is #4C5FD5; build and docs' notes stay uncoloured.
};

/// claude's tab colour under TabColours::Coloured.
constexpr auto ClaudeTabColour = vtbackend::RGBColor { 0x4C, 0x5F, 0xD5 };
/// build's tab colour under TabColours::Coloured.
constexpr auto BuildTabColour = vtbackend::RGBColor { 0xE8, 0x85, 0x3A };

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
    PaneTree paneTree = PaneTree::Leaf;      ///< Whether the window shows a pane tree.
    SceneFont font = SceneFont::Application; ///< The font the chrome is set in.
    /// The sidebar's width in logical pixels; the row's own default when unset.
    std::optional<int> width = std::nullopt;
    ClaudeTab claudeTab = ClaudeTab::OnePane; ///< Whether claude's tab is split.
    std::string billingTitle = "billing";     ///< The first workspace's name, which the location chip shows.
    int tabCount = 2;                         ///< How many tabs the window has.
    TabColours tabColours = TabColours::None; ///< Whether billing's tabs are coloured.
    /// claude's tab colour in place of ClaudeTabColour, under TabColours::Coloured.
    std::optional<vtbackend::RGBColor> claudeTabColour = std::nullopt;
    /// How a coloured tab shows on its rows (`sidebar.tab_color`).
    contour::config::SidebarTabColorStyle tabColorStyle = contour::config::SidebarTabColorStyle::Tint;
    /// The first workspace's colour; docs stays uncoloured.
    std::optional<vtbackend::RGBColor> billingColour = std::nullopt;
    /// How a coloured workspace shows in the sidebar (`sidebar.workspace_color`).
    contour::config::SidebarWorkspaceColorStyle workspaceColorStyle =
        contour::config::SidebarWorkspaceColorStyle::Tint;
    /// How much room a tab's or a pane's row takes (`sidebar.row_spacing`).
    contour::config::SidebarRowSpacing rowSpacing = contour::config::SidebarRowSpacing::Comfortable;
};

/// Main.qml over a mock whose sidebar lists two workspaces: billing (a pane that asks for a permission and
/// one that works) and docs (one idle pane).
struct SidebarWindow
{
    explicit SidebarWindow(SidebarScene scene):
        ownedController { std::make_unique<MockMainController>(contour::test::MainWindowScene {
            .tabCount = scene.tabCount,
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
        auto billingWorkspace = contour::test::directoryWorkspaceOf(1, scene.billingTitle, { claude, build });
        billingWorkspace.git = contour::window::DirectoryGit { .branch = "feat/billing", .ahead = 2 };
        if (scene.claudeTab == ClaudeTab::Split)
            billingWorkspace.tabs.front().panes.push_back(contour::test::directoryPaneOf(12, "review"));
        if (scene.tabColours != TabColours::None)
            billingWorkspace.tabs.at(0).color = scene.claudeTabColour.value_or(ClaudeTabColour);
        if (scene.tabColours == TabColours::Coloured)
            billingWorkspace.tabs.at(1).color = BuildTabColour;
        billingWorkspace.color = scene.billingColour;
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
        controller.setSidebarTabColorStyle(scene.tabColorStyle);
        controller.setSidebarWorkspaceColorStyle(scene.workspaceColorStyle);
        controller.setSidebarRowSpacing(scene.rowSpacing);

        root = contour::test::loadMainWindow(
            engine, controller, warnings, setupOf(scene.row, scene.motion, scene.font));
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

    /// The `chromeStyle.sidebar` token @p name that depends on `sidebar.row_spacing`, at the window's
    /// spacing.
    /// @param name `twoLineRowHeight` or `lineGap`.
    [[nodiscard]] qreal spacedToken(char const* name) const
    {
        return contour::test::sidebarSpacedToken(
            *engine.rootContext(),
            name,
            static_cast<contour::config::SidebarRowSpacing>(controller.sidebarRowSpacing()));
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

    /// Moves the pointer onto the middle of @p target, from elsewhere first: Qt sends no hover for a move to
    /// where the pointer already rests, and what an earlier test left it on is as good as a click there.
    void hover(QQuickItem const& target) const
    {
        QTest::mouseMove(window, QPoint { window->width() / 2, window->height() - 5 });
        auto const centre = target.mapToScene(QPointF { target.width() / 2, target.height() / 2 }).toPoint();
        QTest::mouseMove(window, centre - QPoint { 2, 0 });
        QTest::mouseMove(window, centre);
        QCoreApplication::processEvents();
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

    /// Lays every positioner and list view of the window out now: one does so at its next polish, which the
    /// offscreen platform runs with the next frame, so a control that has just appeared would still overlap
    /// its neighbour under the pointer.
    void layOut() const
    {
        QCoreApplication::processEvents();
        std::function<void(QQuickItem&)> visit = [&](QQuickItem& item) {
            for (auto* child: item.childItems())
            {
                // A list view, too, lays out its delegates for a new model row at its next polish.
                if (child->inherits("QQuickBasePositioner") || child->inherits("QQuickItemView"))
                    QMetaObject::invokeMethod(child, "forceLayout");
                visit(*child);
            }
        };
        visit(*window->contentItem());
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
    static auto const sidebarFiles = QRegularExpression {
        QStringLiteral(
            R"((Sidebar|SidebarRow|SidebarHoverCard|SidebarFooter|SidebarControlButton|SidebarNeedsMeButton|WorkspaceChip|Breadcrumb)\.qml)")
    };
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

/// Drags the row @p from to @p end, a point of the window, as the user does, and drops there.
///
/// The gesture is real mouse events: a press, and moves past the drag threshold, which the row's DragHandler
/// must turn into the node drag (the controller is asked for its payload, and told when the drag is over).
/// The offscreen platform has no OS drag loop, so that drag returns at once; what the OS does with the
/// payload next -- drag-enter, drag-move and drop events to the window under the pointer -- is delivered here
/// through the platform interface, to the DropArea of the row the pointer is over.
void dragRowTo(SidebarWindow& scene, QQuickItem const& from, QPoint end)
{
    auto const start = from.mapToScene(QPointF { from.width() / 2, from.height() / 2 }).toPoint();

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

/// Drags the row @p from onto the row @p to (dragRowTo()), and drops at @p fraction of @p to's height.
void dragRowOnto(SidebarWindow& scene, QQuickItem const& from, QQuickItem const& to, double fraction)
{
    dragRowTo(scene, from, to.mapToScene(QPointF { to.width() / 2, to.height() * fraction }).toPoint());
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
        // A tab row is two lines tall, with a reason or without one; a workspace row is one line.
        CHECK(claude->height() == scene.spacedToken("twoLineRowHeight"));
        auto* notes = scene.rowTitled(QStringLiteral("notes"));
        REQUIRE(notes != nullptr);
        CHECK(notes->height() == scene.spacedToken("twoLineRowHeight"));
        auto* docs = scene.rowTitled(QStringLiteral("docs"));
        REQUIRE(docs != nullptr);
        CHECK(docs->height() == scene.token("rowHeight"));
        CHECK(scene.warnings.count(contour::test::isQmlDiagnostic) == 0);
    }
}

namespace
{
/// Both values of `sidebar.row_spacing`.
constexpr auto RowSpacings = std::array { contour::config::SidebarRowSpacing::Comfortable,
                                          contour::config::SidebarRowSpacing::Compact };

/// Every row this process draws, at each `sidebar.row_spacing`.
[[nodiscard]] std::vector<std::pair<ChromeStyle, contour::config::SidebarRowSpacing>> drawnRowsAtEachSpacing()
{
    auto cases = std::vector<std::pair<ChromeStyle, contour::config::SidebarRowSpacing>> {};
    for (auto const row: drawnRows())
        for (auto const spacing: RowSpacings)
            cases.emplace_back(row, spacing);
    return cases;
}
} // namespace

TEST_CASE("a two-line row is as tall as sidebar.row_spacing says, or as its lines need (offscreen)",
          "[contour][gui][qml][sidebar]")
{
    for (auto const row: drawnRows())
    {
        INFO("row " << static_cast<int>(row));
        auto heights = std::vector<qreal> {};
        for (auto const spacing: RowSpacings)
        {
            INFO("spacing " << contour::config::configEnumToken(spacing));
            auto scene = SidebarWindow { { .row = row, .rowSpacing = spacing } };
            auto const twoLines = scene.spacedToken("twoLineRowHeight");
            auto height = qreal { 0 };
            for (auto const* title: { "claude", "build", "notes" })
            {
                INFO(title);
                auto* rowItem = scene.rowTitled(QString::fromUtf8(title));
                REQUIRE(rowItem != nullptr);
                // The spacing's height, unless the face is too tall for it: then what its lines take.
                auto const fitted = row == ChromeStyle::Terminal
                                        ? twoLines
                                        : std::max(twoLines, rowItem->property("fittedHeight").toReal());
                CHECK(rowItem->height() == fitted);
                height = rowItem->height();
            }
            // A workspace's row is one line at either spacing.
            auto* docs = scene.rowTitled(QStringLiteral("docs"));
            REQUIRE(docs != nullptr);
            CHECK(docs->height() == scene.token("rowHeight"));
            CHECK(scene.warnings.count(contour::test::isQmlDiagnostic) == 0);
            heights.push_back(height);
        }
        // Comfortable only adds room, which a face too tall for compact may already take; the Terminal look's
        // rows are two cells under either.
        REQUIRE(heights.size() == 2);
        if (row == ChromeStyle::Terminal)
            CHECK(heights.front() == heights.back());
        else
            CHECK(heights.front() >= heights.back());
    }
}

TEST_CASE("a tab row keeps its height when its reason comes and goes (offscreen)",
          "[contour][gui][qml][sidebar]")
{
    for (auto const row: drawnRows())
    {
        INFO("row " << static_cast<int>(row));
        auto scene = SidebarWindow { { .row = row } };
        auto* claude = scene.rowTitled(QStringLiteral("claude"));
        auto* build = scene.rowTitled(QStringLiteral("build"));
        REQUIRE(claude != nullptr);
        REQUIRE(build != nullptr);
        auto* reasonLine = SidebarWindow::findNamed(*claude, QStringLiteral("sidebarRowReasonLine"));
        REQUIRE(reasonLine != nullptr);
        REQUIRE(reasonLine->isVisible());
        auto const height = claude->height();
        auto const below = build->y();

        // The reason goes; the list lays out again on the next frame.
        auto const frames = QSignalSpy { scene.window, &QQuickWindow::frameSwapped };
        scene.controller.sidebarSource().listed.workspaces[0].tabs[0].panes[0].status = {};
        scene.controller.sidebarModel().refreshNow();
        REQUIRE(QTest::qWaitFor([&] { return !reasonLine->isVisible() && frames.count() > 0; }, 1000));

        CHECK(claude->height() == height);
        CHECK(build->y() == below);
        CHECK(scene.warnings.count(contour::test::isQmlDiagnostic) == 0);
    }
}

TEST_CASE("an idle row shows its folder on the second line (offscreen)", "[contour][gui][qml][sidebar]")
{
    for (auto const row: drawnRows())
    {
        INFO("row " << static_cast<int>(row));
        auto scene = SidebarWindow { { .row = row } };
        scene.controller.sidebarSource().listed.workspaces[1].tabs[0].panes[0].cwd = "/src/docs";
        scene.controller.sidebarModel().refreshNow();

        auto* notes = scene.rowTitled(QStringLiteral("notes"));
        REQUIRE(notes != nullptr);
        auto* idle = SidebarWindow::findNamed(*notes, QStringLiteral("sidebarRowIdle"));
        auto* reasonLine = SidebarWindow::findNamed(*notes, QStringLiteral("sidebarRowReasonLine"));
        REQUIRE(idle != nullptr);
        REQUIRE(reasonLine != nullptr);
        CHECK(idle->isVisible());
        CHECK(idle->property("text").toString() == QStringLiteral("/src/docs"));
        CHECK_FALSE(reasonLine->isVisible());

        // A row with a reason shows it instead.
        auto* claude = scene.rowTitled(QStringLiteral("claude"));
        REQUIRE(claude != nullptr);
        auto* claudeIdle = SidebarWindow::findNamed(*claude, QStringLiteral("sidebarRowIdle"));
        auto* claudeReason = SidebarWindow::findNamed(*claude, QStringLiteral("sidebarRowReasonLine"));
        REQUIRE(claudeIdle != nullptr);
        REQUIRE(claudeReason != nullptr);
        CHECK_FALSE(claudeIdle->isVisible());
        CHECK(claudeReason->isVisible());
        CHECK(scene.warnings.count(contour::test::isQmlDiagnostic) == 0);
    }
}

TEST_CASE("two-line rows stand further apart than the lines within one (offscreen)",
          "[contour][gui][qml][sidebar]")
{
    for (auto const& [row, spacing]: drawnRowsAtEachSpacing())
    {
        INFO("row " << static_cast<int>(row) << ", spacing " << contour::config::configEnumToken(spacing));
        auto scene = SidebarWindow { { .row = row, .rowSpacing = spacing } };
        if (scene.token("look") == scene.token("cellTree"))
            continue; // whole cell rows: each line is a row of the grid, with no space to share out
        // claude asks, and build under it works in a folder: two rows that both have a second line.
        scene.controller.sidebarSource().listed.workspaces[0].tabs[1].panes[0].cwd = "/src/billing";
        scene.controller.sidebarModel().refreshNow();
        auto* upper = scene.rowTitled(QStringLiteral("claude"));
        auto* lower = scene.rowTitled(QStringLiteral("build"));
        REQUIRE(upper != nullptr);
        REQUIRE(lower != nullptr);
        REQUIRE(lower->y() > upper->y());

        /// @return The scene box of the visible part named @p name of @p rowItem.
        auto const boxOf = [](QQuickItem& rowItem, QString const& name) {
            auto* part = SidebarWindow::findNamed(rowItem, name);
            REQUIRE(part != nullptr);
            REQUIRE(part->isVisible());
            return sceneRectOf(*part);
        };
        auto const secondOf = [&](QQuickItem& rowItem) {
            auto* reason = SidebarWindow::findNamed(rowItem, QStringLiteral("sidebarRowReasonLine"));
            REQUIRE(reason != nullptr);
            return boxOf(rowItem,
                         reason->isVisible() ? QStringLiteral("sidebarRowReasonLine")
                                             : QStringLiteral("sidebarRowIdle"));
        };

        // From the line boxes, so it holds for any face: a row's two lines read as one block only when the
        // space between two blocks is larger than the space within one.
        auto const within =
            secondOf(*upper).top() - boxOf(*upper, QStringLiteral("sidebarRowTitle")).bottom();
        auto const between =
            boxOf(*lower, QStringLiteral("sidebarRowTitle")).top() - secondOf(*upper).bottom();
        INFO("within " << within << ", between " << between);
        CHECK(between > within);
        CHECK(scene.warnings.count(contour::test::isQmlDiagnostic) == 0);
    }
}

TEST_CASE("a two-line row keeps its lines inside it, and centres a name with nothing under it (offscreen)",
          "[contour][gui][qml][sidebar]")
{
    // Compared with the row's own geometry, never with a font's metrics, so it holds for every platform's
    // faces; a fraction of a pixel is the rounding of a box placed by its own size.
    constexpr auto Tolerance = qreal { 0.51 };
    // The name is centred in its line by an anchor, which Qt aligns to whole pixels (alignWhenCentered):
    // the line's half height and the name's are each rounded, by at most half a pixel apiece, so with a
    // face whose heights are fractional the name lands less than a pixel from the true centre.
    constexpr auto CentringRounding = qreal { 1 };
    for (auto const& [row, spacing]: drawnRowsAtEachSpacing())
    {
        INFO("row " << static_cast<int>(row) << ", spacing " << contour::config::configEnumToken(spacing));
        auto scene = SidebarWindow { { .row = row, .rowSpacing = spacing } };
        // claude asks (a reason), docs/notes is idle in a folder, and build stops working and knows nothing
        // of itself: the three second lines a row can have.
        scene.controller.sidebarSource().listed.workspaces[1].tabs[0].panes[0].cwd = "/src/docs";
        scene.controller.sidebarSource().listed.workspaces[0].tabs[1].panes[0].status = {};
        scene.controller.sidebarModel().refreshNow();
        auto const twoLines = scene.spacedToken("twoLineRowHeight");
        auto const cellTree = scene.token("look") == scene.token("cellTree");

        /// @return @p name's box within the row @p rowItem.
        auto const boxOf = [](QQuickItem& rowItem, QString const& name) {
            auto* part = SidebarWindow::findNamed(rowItem, name);
            REQUIRE(part != nullptr);
            REQUIRE(part->isVisible());
            return part->mapRectToItem(&rowItem, QRectF { 0, 0, part->width(), part->height() });
        };

        auto* blank = scene.rowTitled(QStringLiteral("build"));
        auto* asking = scene.rowTitled(QStringLiteral("claude"));
        auto* idle = scene.rowTitled(QStringLiteral("notes"));
        REQUIRE(blank != nullptr);
        REQUIRE(asking != nullptr);
        REQUIRE(idle != nullptr);
        REQUIRE(QTest::qWaitFor(
            [&] {
                auto* glyph = SidebarWindow::findNamed(*blank, QStringLiteral("sidebarRowGlyph"));
                return glyph != nullptr && !glyph->isVisible();
            },
            1000));

        // The spacing's height, or more where the face is too tall for it (SidebarRow's fittedHeight): the
        // same for every row, whatever its second line says.
        if (cellTree)
            CHECK(blank->height() == twoLines);
        CHECK(blank->height() >= twoLines);
        for (auto* rowItem: { blank, asking, idle })
        {
            INFO("title " << rowItem->property("title").toString().toStdString());
            CHECK(rowItem->height() == blank->height());
            auto const title = boxOf(*rowItem, QStringLiteral("sidebarRowTitle"));
            CHECK(title.top() > -CentringRounding);
            CHECK(title.bottom() < rowItem->height() + CentringRounding);
        }

        // The second lines stay inside the row, under the name.
        for (auto const& [rowItem, line]: { std::pair { asking, QStringLiteral("sidebarRowReasonLine") },
                                            std::pair { idle, QStringLiteral("sidebarRowIdle") } })
        {
            INFO(line.toStdString());
            auto const title = boxOf(*rowItem, QStringLiteral("sidebarRowTitle"));
            auto const second = boxOf(*rowItem, line);
            if (cellTree)
            {
                // Each line is a whole cell row: the second starts where the first cell row ends.
                CHECK(std::abs(second.top() - scene.token("rowHeight")) <= Tolerance);
                continue;
            }
            CHECK(second.top() > title.bottom() - CentringRounding);
            CHECK(second.bottom() <= rowItem->height() + Tolerance);
            // A name with a line under it sits above the row's middle, further than the rounding could ever
            // move a centred one: the two cases below never overlap.
            CHECK(title.center().y() < (rowItem->height() / 2) - CentringRounding);
        }

        // The row with nothing for its second line shows none, and centres its name in its two lines; the
        // Terminal row keeps it in its first cell row, as half a cell is no place on the grid.
        auto* blankIdle = SidebarWindow::findNamed(*blank, QStringLiteral("sidebarRowIdle"));
        REQUIRE(blankIdle != nullptr);
        CHECK_FALSE(blankIdle->isVisible());
        auto const blankTitle = boxOf(*blank, QStringLiteral("sidebarRowTitle"));
        if (cellTree)
            CHECK(blankTitle.center().y() < scene.token("rowHeight") + Tolerance);
        else
            CHECK(std::abs(blankTitle.center().y() - (blank->height() / 2)) < CentringRounding);
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

TEST_CASE("Enter activates the tree's row and hands the keyboard to the terminal (offscreen)",
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

        QTest::keyClick(scene.window, Qt::Key_Return);
        QCoreApplication::processEvents();
        CHECK_FALSE(scene.controller.sidebarHost().shown.empty());
        CHECK(scene.controller.chromeFocusArea() == static_cast<int>(contour::window::ChromeArea::Terminal));
    }
}

TEST_CASE("Enter on a tree with no row keeps the keyboard in the sidebar (offscreen)",
          "[contour][gui][qml][sidebar]")
{
    auto scene = SidebarWindow { { .row = drawnRows().front() } };
    auto* tree = scene.item(QStringLiteral("sidebarTree"));
    REQUIRE(tree != nullptr);
    scene.controller.focusSidebar();
    REQUIRE(QTest::qWaitFor([&] { return tree->hasActiveFocus(); }, 1000));
    // A filter nothing matches leaves the tree empty.
    scene.controller.sidebarModel().setFilter(QStringLiteral("no row is called this"));
    QCoreApplication::processEvents();
    REQUIRE(scene.controller.sidebarModel().rowCount() == 0);

    QTest::keyClick(scene.window, Qt::Key_Return);
    QCoreApplication::processEvents();
    CHECK(scene.controller.sidebarHost().shown.empty());
    CHECK(scene.controller.chromeFocusArea() == static_cast<int>(contour::window::ChromeArea::Sidebar));
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

TEST_CASE("workspace, tab and pane rows drag (offscreen)", "[contour][gui][qml][sidebar]")
{
    auto scene = SidebarWindow { { .row = drawnRows().front(), .claudeTab = ClaudeTab::Split } };
    for (auto const& title: { QStringLiteral("billing"), QStringLiteral("claude"), QStringLiteral("review") })
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
        char const* description;
    };
    for (auto const row: drawnRows())
    {
        // The name is what the chip shows, whatever the others ask; what they ask is the description's.
        for (auto const& [billing, marked, description]:
             { Case { BillingAsks::Permission, true, "Another workspace needs you, collapsed" },
               Case { BillingAsks::Done, true, "Another workspace is done, collapsed" },
               Case { BillingAsks::FailedSeen, false, "collapsed" } })
        {
            INFO("row " << static_cast<int>(row) << ", " << description);
            auto scene = SidebarWindow { { .row = row,
                                           .visibility = contour::config::SidebarVisibility::Hidden,
                                           .shownWorkspace = 2,
                                           .billing = billing } };
            auto* chip = scene.item(QStringLiteral("workspaceChip"));
            REQUIRE(chip != nullptr);
            auto const* attached = accessibleOf(*chip);
            REQUIRE(attached != nullptr);
            CHECK(attached->property("name").toString() == QStringLiteral("Workspace docs"));
            CHECK(attached->property("description").toString() == QString::fromLatin1(description));
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

namespace
{
/// Gives claude's pane @p record and hovers its row until the hover card opens.
/// @return The card.
QObject& openClaudeCard(SidebarWindow& scene, vtbackend::ProgramStatusRecord record)
{
    scene.controller.sidebarSource().listed.workspaces[0].tabs[0].panes[0].status = { std::move(record) };
    scene.controller.sidebarModel().refreshNow();
    QCoreApplication::processEvents();
    auto* claude = scene.rowTitled(QStringLiteral("claude"));
    REQUIRE(claude != nullptr);
    auto* card = scene.root->findChild<QObject*>(QStringLiteral("sidebarHoverCard"));
    REQUIRE(card != nullptr);
    auto const over = claude->mapToScene(QPointF { claude->width() / 2, claude->height() / 2 }).toPoint();
    QTest::mouseMove(scene.window, over - QPoint { 0, 1 });
    QTest::mouseMove(scene.window, over);
    REQUIRE(QTest::qWaitFor([&] { return card->property("opened").toBool(); }, 3000));
    return *card;
}

/// Checks that the card's reason words, @p label long, wrap within the card and show every character.
void checkReasonWraps(std::string const& label)
{
    for (auto const row: drawnRows())
    {
        INFO("row " << static_cast<int>(row));
        auto scene = SidebarWindow { { .row = row } };
        auto const& card = openClaudeCard(scene, recordOf(vtbackend::ProgramState::Working, label));
        auto const* reason =
            SidebarWindow::findNamed(*scene.window->contentItem(), QStringLiteral("sidebarHoverCardReason"));
        REQUIRE(reason != nullptr);
        CHECK(reason->property("text").toString() == QString::fromStdString(label));
        CHECK(reason->width() <= card.property("availableWidth").toReal());
        CHECK(reason->property("lineCount").toInt() > 1);
        CHECK_FALSE(reason->property("truncated").toBool());
        CHECK(scene.warnings.count(contour::test::isQmlDiagnostic) == 0);
    }
}
} // namespace

TEST_CASE("the hover card wraps a long status with no command (offscreen)", "[contour][gui][qml][sidebar]")
{
    auto label = std::string {};
    for (auto const word: std::views::iota(1, 41))
        label += std::format("{}word{:02}", label.empty() ? "" : " ", word);
    checkReasonWraps(label);
}

TEST_CASE("the hover card wraps a status with no spaces (offscreen)", "[contour][gui][qml][sidebar]")
{
    checkReasonWraps(std::string(500, 'x'));
}

TEST_CASE("the hover card keeps its text inside a width of no whole pixels (offscreen)",
          "[contour][gui][qml][sidebar]")
{
    // A layout rounds the width it gives an item to a whole pixel, up as well as down; a card whose width is
    // whole cells of a fractional cell width (the Terminal row) must still not have its text overflow it.
    for (auto const row: drawnRows())
    {
        INFO("row " << static_cast<int>(row));
        auto scene = SidebarWindow { { .row = row } };
        scene.controller.sidebarSource().listed.workspaces[0].tabs[0].panes[0].cwd =
            "/" + std::string(280, 'p') + "/the-last-directory";
        // Before it opens, as the card's own width is: to a fraction above the half, which a layout rounds
        // up.
        auto* closed = scene.root->findChild<QObject*>(QStringLiteral("sidebarHoverCard"));
        REQUIRE(closed != nullptr);
        auto const available = closed->property("availableWidth").toReal();
        auto const fraction = 0.71875;
        closed->setProperty(
            "width", closed->property("width").toReal() - (available - std::floor(available)) + fraction);
        auto const& card =
            openClaudeCard(scene, recordOf(vtbackend::ProgramState::Working, std::string(500, 'x')));
        auto const bound = card.property("availableWidth").toReal();
        REQUIRE(bound - std::floor(bound) == Catch::Approx(fraction));
        for (auto const* name: { "sidebarHoverCardReason", "sidebarHoverCardDetail" })
        {
            INFO(name);
            auto const* text =
                SidebarWindow::findNamed(*scene.window->contentItem(), QString::fromLatin1(name));
            REQUIRE(text != nullptr);
            CHECK(text->width() <= bound);
            CHECK(text->property("lineCount").toInt() > 1);
        }
        CHECK(scene.warnings.count(contour::test::isQmlDiagnostic) == 0);
    }
}

TEST_CASE("the hover card shows a long folder to its end (offscreen)", "[contour][gui][qml][sidebar]")
{
    auto const folder = "/" + std::string(280, 'p') + "/the-last-directory";
    REQUIRE(folder.size() == 300);
    for (auto const row: drawnRows())
    {
        INFO("row " << static_cast<int>(row));
        auto scene = SidebarWindow { { .row = row } };
        scene.controller.sidebarSource().listed.workspaces[0].tabs[0].panes[0].cwd = folder;
        openClaudeCard(scene, recordOf(vtbackend::ProgramState::Working, "Thinking"));
        auto const* detail =
            SidebarWindow::findNamed(*scene.window->contentItem(), QStringLiteral("sidebarHoverCardDetail"));
        REQUIRE(detail != nullptr);
        auto const text = detail->property("text").toString();
        INFO("detail " << text.toStdString());
        // The time follows the folder.
        CHECK(text.endsWith(QString::fromStdString(folder) + QStringLiteral(" · 0:30")));
        CHECK(detail->property("lineCount").toInt() > 1);
        CHECK_FALSE(detail->property("truncated").toBool());
        CHECK(scene.warnings.count(contour::test::isQmlDiagnostic) == 0);
    }
}

TEST_CASE("a tab row shows the strip's icon, and its hover card what runs there (offscreen)",
          "[contour][gui][qml][sidebar]")
{
    auto scene = SidebarWindow { { .row = drawnRows().front() } };
    auto& host = scene.controller.sidebarHost();
    host.icons[10] = contour::window::TabIcon { .kind = contour::window::TabIconKind::Monogram,
                                                .initial = "C",
                                                .image = {} };
    host.lines[contour::window::SidebarRowKey {
        .kind = contour::window::SidebarRowKind::Tab, .workspace = 1, .tab = 10, .pane = {} }] = {
        "Running: claude", "Last: make · exit 0 · 2s"
    };
    scene.controller.sidebarModel().noteChanged();
    QCoreApplication::processEvents();

    auto* claude = scene.rowTitled(QStringLiteral("claude"));
    REQUIRE(claude != nullptr);
    auto const* icon = SidebarWindow::findNamed(*claude, QStringLiteral("sidebarRowIcon"));
    REQUIRE(icon != nullptr);
    CHECK(icon->isVisible());
    auto const* initial = SidebarWindow::findNamed(*icon, QStringLiteral("tabIconInitial"));
    REQUIRE(initial != nullptr);
    CHECK(initial->property("text").toString() == QStringLiteral("C"));
    // build's tab is one the window does not hold an icon for: its row shows none.
    auto* build = scene.rowTitled(QStringLiteral("build"));
    REQUIRE(build != nullptr);
    CHECK_FALSE(SidebarWindow::findNamed(*build, QStringLiteral("sidebarRowIcon"))->isVisible());

    auto* card = scene.root->findChild<QObject*>(QStringLiteral("sidebarHoverCard"));
    REQUIRE(card != nullptr);
    auto const over = claude->mapToScene(QPointF { claude->width() / 2, claude->height() / 2 }).toPoint();
    QTest::mouseMove(scene.window, over - QPoint { 0, 1 });
    QTest::mouseMove(scene.window, over);
    REQUIRE(QTest::qWaitFor([&] { return card->property("opened").toBool(); }, 3000));
    // The card's items are its content item's, in the window's overlay.
    auto* surface = qobject_cast<QQuickItem*>(card->property("contentItem").value<QObject*>());
    REQUIRE(surface != nullptr);
    auto lines = QStringList {};
    auto const collect = [&](this auto const& self, QQuickItem const& item) -> void {
        for (auto const* child: item.childItems())
        {
            if (child->objectName() == QStringLiteral("sidebarHoverCardLine"))
                lines.append(child->property("text").toString());
            self(*child);
        }
    };
    collect(*surface);
    CHECK(lines
          == QStringList { QStringLiteral("Running: claude"), QStringLiteral("Last: make · exit 0 · 2s") });
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
        // A daemon's window: the row carries the switcher and the Needs-me control beside the filter.
        scene.controller.setDaemon(QStringLiteral("local"));
        scene.layOut();

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
               inHeader ? QStringLiteral("headerSidebarSwitcherButton")
                        : QStringLiteral("sidebarSwitcherButton"),
               inHeader ? QStringLiteral("headerSidebarNeedsMeButton")
                        : QStringLiteral("sidebarNeedsMeButton"),
               QStringLiteral("sidebarToggle") })
            if (auto* button = SidebarWindow::findNamed(*header, name);
                button != nullptr && button->isVisible())
                buttons.push_back(button);
        // The filter and switcher buttons stand in the title row on every row at its default width, and the
        // Needs-me one where the look keeps it there rather than beside the filter field; the "+" and the
        // toggle only on some.
        auto const needsMeInTitle = scene.token("needsMeInFilterRow") == 0;
        REQUIRE(buttons.size() >= (needsMeInTitle ? 3u : 2u));
        CHECK(std::ranges::any_of(buttons, [&](QQuickItem const* button) {
            return button->objectName().contains(QStringLiteral("Switcher"));
        }));
        CHECK(std::ranges::any_of(buttons,
                                  [&](QQuickItem const* button) {
                                      return button->objectName().contains(QStringLiteral("NeedsMe"));
                                  })
              == needsMeInTitle);
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
        scene.hover(*docs);
        CHECK(QTest::qWaitFor([&] { return feedback->isVisible() == (scene.token("rowFeedback") != 0); },
                              1000));
    }
}

TEST_CASE("a row shows hover where the pointer rested after a click in the window before (offscreen)",
          "[contour][gui][qml][sidebar]")
{
    for (auto const row: drawnRows())
    {
        INFO("row " << static_cast<int>(row));
        {
            // What the test before leaves behind: the pointer on the row's middle, as a click there ends.
            auto before = SidebarWindow { { .row = row } };
            auto* docs = before.rowTitled(QStringLiteral("docs"));
            REQUIRE(docs != nullptr);
            before.click(*docs);
        }
        auto scene = SidebarWindow { { .row = row } };
        auto* docs = scene.rowTitled(QStringLiteral("docs"));
        REQUIRE(docs != nullptr);
        auto const* feedback = SidebarWindow::findNamed(*docs, QStringLiteral("sidebarRowFeedback"));
        REQUIRE(feedback != nullptr);
        scene.hover(*docs);
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
        // The chip itself: the dropdown hangs below it, by the gap popupBelow puts between them.
        CHECK(anchor.left() == Catch::Approx(chipRect.left()));
        CHECK(anchor.top() == Catch::Approx(chipRect.top()));
        CHECK(anchor.width() == Catch::Approx(chipRect.width()));
        CHECK(anchor.bottom() == Catch::Approx(chipRect.bottom()));
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
            // The wide sidebar shows the chord. The narrowest drops it where the counts would not keep their
            // room beside it -- with the default fonts of every platform CI measures; with narrower ones it
            // may still fit, and then it stands clear of the counts, which keep their full wording.
            if (chordShown)
                CHECK(chord->isVisible());
            else if (chord->isVisible())
            {
                CHECK(text == QStringLiteral("1 needs you · 1 working"));
                CHECK(counts->x() + counts->property("implicitWidth").toReal() <= chord->x() + 0.5);
            }
        }
    }
}

TEST_CASE("at the narrowest sidebar in the row's own font the footer's counts are whole, and named in full "
          "(offscreen)",
          "[contour][gui][qml][sidebar]")
{
    // The Terminal row's footer is set in the profile's monospaced font, where even the shorter words take
    // more cells than its narrowest sidebar has: its status glyphs say the counts there.
    for (auto const row: drawnRows())
    {
        INFO("row " << static_cast<int>(row));
        auto scene = SidebarWindow { { .row = row, .font = SceneFont::RowOwn } };
        scene.controller.showSidebar(static_cast<int>(scene.token("minWidth")));
        QTest::qWait(50);
        auto const* counts = scene.item(QStringLiteral("sidebarFooterCounts"));
        REQUIRE(counts != nullptr);
        INFO("counts " << counts->property("text").toString().toStdString() << ", width " << counts->width()
                       << ", implicit " << counts->property("implicitWidth").toReal());
        CHECK(counts->isVisible());
        CHECK(!counts->property("truncated").toBool());
        auto const* attached = accessibleOf(*counts);
        REQUIRE(attached != nullptr);
        CHECK(attached->property("name").toString() == QStringLiteral("1 needs you · 1 working"));
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

        REQUIRE(scene.controller.sidebarHost().movedNodes.size() == 1);
        auto const& [node, target] = scene.controller.sidebarHost().movedNodes.front();
        CHECK(node == contour::window::SidebarNode { contour::window::TabRef { .workspace = 1, .tab = 10 } });
        REQUIRE(std::holds_alternative<contour::window::MoveTab>(target));
        CHECK(std::get<contour::window::MoveTab>(target).toWorkspace == 2);
        CHECK(scene.controller.finishedDrags == 1);
        CHECK(scene.warnings.count(contour::test::isQmlDiagnostic) == 0);
    }
}

TEST_CASE("dragging a pane row with the mouse joins a tab, or becomes a tab or a workspace (offscreen)",
          "[contour][gui][qml][sidebar]")
{
    using contour::window::SidebarNode;
    struct Case
    {
        QString target;  ///< The row it is dropped on; empty for the tree's empty area.
        double fraction; ///< Where on that row's height.
        std::function<bool(contour::window::NodeDestination const&)> lands; ///< Where it must land.
    };
    auto const cases = std::vector<Case> {
        // The middle of the build tab: review joins it as a split.
        { QStringLiteral("build"),
          0.5,
          [](auto const& to) {
              auto const* join = std::get_if<contour::window::JoinTab>(&to);
              return join != nullptr && join->tab == contour::window::TabRef { .workspace = 1, .tab = 11 };
          } },
        // The middle of the docs workspace: review becomes its last tab.
        { QStringLiteral("docs"),
          0.5,
          [](auto const& to) {
              auto const* move = std::get_if<contour::window::MoveTab>(&to);
              return move != nullptr && move->toWorkspace == 2
                     && move->toPosition == contour::window::AppendTab;
          } },
        // Below the rows: a workspace of its own.
        { QString {},
          0.5,
          [](auto const& to) { return std::holds_alternative<contour::window::IntoNewWorkspace>(to); } },
    };
    for (auto const row: drawnRows())
        for (auto const& [target, fraction, lands]: cases)
        {
            INFO("row " << static_cast<int>(row) << " onto " << target.toStdString());
            auto scene = SidebarWindow { { .row = row, .claudeTab = ClaudeTab::Split } };
            auto* review = scene.rowTitled(QStringLiteral("review"));
            REQUIRE(review != nullptr);
            if (target.isEmpty())
            {
                auto* empty = scene.item(QStringLiteral("sidebarTreeEmptyDrop"));
                REQUIRE(empty != nullptr);
                REQUIRE(empty->height() > 0);
                dragRowTo(scene, *review, sceneRectOf(*empty).center().toPoint());
            }
            else
            {
                auto* onto = scene.rowTitled(target);
                REQUIRE(onto != nullptr);
                dragRowOnto(scene, *review, *onto, fraction);
            }
            REQUIRE(scene.controller.sidebarHost().movedNodes.size() == 1);
            auto const& [node, to] = scene.controller.sidebarHost().movedNodes.front();
            CHECK(node == SidebarNode { contour::window::PaneKey { 12 } });
            CHECK(lands(to));
            CHECK(scene.controller.finishedDrags == 1);
            CHECK(scene.warnings.count(contour::test::isQmlDiagnostic) == 0);
        }
}

TEST_CASE("a pane dragged over its own tab is refused, and over another shows where it lands (offscreen)",
          "[contour][gui][qml][sidebar]")
{
    auto scene = SidebarWindow { { .row = drawnRows().front(), .claudeTab = ClaudeTab::Split } };
    auto* review = scene.rowTitled(QStringLiteral("review"));
    auto* claude = scene.rowTitled(QStringLiteral("claude"));
    auto* build = scene.rowTitled(QStringLiteral("build"));
    REQUIRE(review != nullptr);
    REQUIRE(claude != nullptr);
    REQUIRE(build != nullptr);
    auto mime = QMimeData {};
    mime.setData(QString::fromLatin1(contour::window::NodeDragMime),
                 scene.controller.beginSidebarDrag(review->property("index").toInt()).toUtf8());
    auto const hover = [&](QQuickItem const& row, double fraction) {
        auto const at = row.mapToScene(QPointF { row.width() / 2, row.height() * fraction }).toPoint();
        return QWindowSystemInterface::handleDrag(
            scene.window, &mime, at, Qt::MoveAction, Qt::LeftButton, Qt::NoModifier);
    };
    auto const shown = [](QQuickItem const& row, char const* name) {
        auto* item = SidebarWindow::findNamed(row, QString::fromLatin1(name));
        return item != nullptr && item->isVisible();
    };

    // Its own tab's middle: the row refuses it (the window beneath takes it as a no-op), and shows nothing.
    std::ignore = hover(*claude, 0.5);
    QCoreApplication::processEvents();
    auto* ownDrop = SidebarWindow::findNamed(*claude, QStringLiteral("sidebarRowDrop"));
    REQUIRE(ownDrop != nullptr);
    CHECK_FALSE(ownDrop->property("containsDrag").toBool());
    CHECK_FALSE(shown(*claude, "sidebarRowDropHighlight"));
    // Another tab's middle: that row is highlighted; its upper third: a line above it.
    auto const into = hover(*build, 0.5);
    QCoreApplication::processEvents();
    CHECK(into.isAccepted());
    CHECK(SidebarWindow::findNamed(*build, QStringLiteral("sidebarRowDrop"))
              ->property("containsDrag")
              .toBool());
    CHECK(shown(*build, "sidebarRowDropHighlight"));
    std::ignore = hover(*build, 0.1);
    QCoreApplication::processEvents();
    CHECK(shown(*build, "sidebarRowDropLine"));
    CHECK_FALSE(shown(*build, "sidebarRowDropHighlight"));
    QWindowSystemInterface::handleDrag(
        scene.window, nullptr, QPoint { -1, -1 }, Qt::IgnoreAction, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::processEvents();
    CHECK(scene.controller.sidebarHost().movedNodes.empty());
}

namespace
{
/// Every sidebar control on screen in @p scene (SidebarControlButton, wherever its look stands it).
[[nodiscard]] std::vector<QQuickItem*> shownSidebarControls(QQuickItem const& parent)
{
    auto found = std::vector<QQuickItem*> {};
    for (auto* child: parent.childItems())
    {
        if (child->isVisible() && child->width() > 0 && child->property("control").isValid()
            && child->property("toolTipCommand").isValid())
            found.push_back(child);
        std::ranges::copy(shownSidebarControls(*child), std::back_inserter(found));
    }
    return found;
}

/// The configuration of @p row's host with @p inputMapping as its `input_mapping` section, of the native2026
/// defaults edition.
[[nodiscard]] contour::config::Config bindingsOf(ChromeStyle row, std::string_view inputMapping)
{
    auto const kind = row == ChromeStyle::MacOS ? contour::config::KeyboardPlatformKind::Mac
                                                : contour::config::KeyboardPlatformKind::Xkb;
    return contour::test::loadConfigFromYaml(
        "defaults_edition: native2026\n"
            + contour::test::profileConfigYaml("main", std::format("input_mapping:\n{}", inputMapping)),
        kind);
}

/// Hovers @p control until its tooltip is open, and returns it.
[[nodiscard]] QObject* openToolTipOf(QQuickWindow& window, QQuickItem const& control)
{
    REQUIRE(QMetaObject::invokeMethod(window.property("tooltipClock").value<QObject*>(), "touch"));
    auto const centre = control.mapToScene(QPointF { control.width() / 2, control.height() / 2 }).toPoint();
    // From elsewhere first: a move to where the pointer already is sends no hover.
    QTest::mouseMove(&window, QPoint { window.width() / 2, window.height() - 40 });
    QTest::mouseMove(&window, centre - QPoint { 2, 0 });
    QTest::mouseMove(&window, centre);
    auto const* host = control.findChild<QQuickItem*>(QStringLiteral("kitToolTipHost"));
    REQUIRE(host != nullptr);
    REQUIRE(QTest::qWaitFor(
        [&] {
            auto const* tip = host->property("toolTip").value<QObject*>();
            return tip != nullptr && tip->property("opened").toBool();
        },
        5000));
    return host->property("toolTip").value<QObject*>();
}
} // namespace

TEST_CASE("every sidebar control's tooltip names the chord its action is bound to now, or none (offscreen)",
          "[contour][gui][qml][sidebar][tooltip]")
{
    // A configuration written before the sidebar existed: it binds New Tab only, and the sidebar's chords
    // come from the defaults' fallbacks. Then one that binds all three controls' actions itself, with the
    // host's Primary chord, which a Mac prefers over a fallback's as the menus do (ChordPreference).
    constexpr auto Predating =
        std::string_view { "  - { mods: [Primary], key: 'T', action: CreateNewTab }\n" };
    constexpr auto Rebound =
        std::string_view { "  - { mods: [Primary, Alt], key: 'B', action: ToggleSidebar }\n"
                           "  - { mods: [Primary, Alt], key: 'N', action: NewWorkspace }\n"
                           "  - { mods: [Primary, Alt], key: 'F', action: FilterWorkspaces }\n"
                           "  - { mods: [Primary, Alt], key: 'S', action: ShowSwitcher }\n"
                           "  - { mods: [Primary, Alt], key: 'M', action: ToggleSidebarNeedsMe }\n" };
    using contour::window::SidebarControl;
    using vtbackend::Modifier;
    // In SidebarControl order, as the expected chords below are.
    constexpr auto Controls = std::array { SidebarControl::Toggle,
                                           SidebarControl::NewWorkspace,
                                           SidebarControl::Filter,
                                           SidebarControl::Switcher,
                                           SidebarControl::NeedsMe };
    static_assert(Controls.size() == contour::window::SidebarControlCount);

    for (auto const row: drawnRows())
        for (auto const visibility:
             { contour::config::SidebarVisibility::Shown, contour::config::SidebarVisibility::Hidden })
        {
            INFO("row " << static_cast<int>(row) << ", sidebar " << static_cast<int>(visibility));
            auto scene = SidebarWindow { { .row = row, .visibility = visibility } };
            // A daemon's window, so its title row carries the switcher too.
            scene.controller.setDaemon(QStringLiteral("local"));
            scene.layOut();
            auto const style = scene.controller.shortcutStyle;
            auto const spelled = [&](vtbackend::Modifiers modifiers, char32_t key) {
                return QString::fromStdString(contour::command::shortcutText(
                    modifiers, contour::command::ShortcutInput { key }, style));
            };
            // The toggle's default: Ctrl+Shift+B, and Command+\ on a Mac.
            auto const toggleDefault = row == ChromeStyle::MacOS
                                           ? spelled(vtbackend::Modifiers { Modifier::Super }, U'\\')
                                           : QStringLiteral("Ctrl+Shift+B");
            // New Workspace's default: Shift+Command+N on a Mac, Ctrl+Shift+Alt+N elsewhere.
            auto const newWorkspaceDefault =
                row == ChromeStyle::MacOS
                    ? spelled(vtbackend::Modifiers { Modifier::Super, Modifier::Shift }, U'N')
                    : spelled(vtbackend::Modifiers { Modifier::Control, Modifier::Shift, Modifier::Alt },
                              U'N');
            // Primary+Alt: Command+Option on a Mac, Ctrl+Shift+Alt elsewhere.
            auto const primaryAlt =
                row == ChromeStyle::MacOS
                    ? vtbackend::Modifiers { Modifier::Super, Modifier::Alt }
                    : vtbackend::Modifiers { Modifier::Control, Modifier::Shift, Modifier::Alt };

            auto const controls = shownSidebarControls(*scene.window->contentItem());
            REQUIRE_FALSE(controls.empty());
            // The toggle stands on every row, shown or hidden.
            CHECK(std::ranges::any_of(controls, [](QQuickItem const* control) {
                return control->property("control").toString() == QStringLiteral("toggle");
            }));

            for (auto const& [mapping, toggle, newWorkspace, filter, switcher, needsMe]:
                 { std::tuple { Predating,
                                toggleDefault,
                                newWorkspaceDefault,
                                QString {},
                                // ShowSwitcher's chord is written into the defaults, not a fallback: a
                                // configuration with its own input_mapping has none.
                                QString {},
                                QString {} },
                   std::tuple { Rebound,
                                spelled(primaryAlt, U'B'),
                                spelled(primaryAlt, U'N'),
                                spelled(primaryAlt, U'F'),
                                spelled(primaryAlt, U'S'),
                                spelled(primaryAlt, U'M') } })
            {
                scene.controller.chords = contour::command::chordIndex(bindingsOf(row, mapping));
                for (auto* control: controls)
                {
                    auto const name = control->property("control").toString();
                    INFO("control " << name.toStdString() << " (" << control->objectName().toStdString()
                                    << ")");
                    // The command comes from the table, never from the placement.
                    auto const which = std::ranges::find_if(Controls, [&](SidebarControl each) {
                        return name.toStdString() == contour::window::sidebarControlName(each);
                    });
                    REQUIRE(which != Controls.end());
                    CHECK(control->property("toolTipCommand").toString().toStdString()
                          == contour::window::sidebarControlCommand(*which));
                    auto const expected = std::array {
                        toggle, newWorkspace, filter, switcher, needsMe
                    }[static_cast<std::size_t>(*which)];
                    auto const* tip = openToolTipOf(*scene.window, *control);
                    CHECK_FALSE(tip->property("text").toString().isEmpty());
                    CHECK(tip->property("shownShortcut").toString().toStdString() == expected.toStdString());
                    // Unbound: the plain label, with no slot (and so no gap) for a chord.
                    CHECK(tip->property("shortcutShown").toBool() == !expected.isEmpty());
                }
            }
            CHECK(scene.warnings.count(isSidebarDiagnostic) == 0);
        }
}

// {{{ The Needs-me scope

namespace
{
/// The Needs-me control a shown sidebar carries: its own title row's, the header bar's part above it, or on
/// a look that keeps it beside the filter field, that one's, the filter opened for it.
[[nodiscard]] QQuickItem* needsMeControlOf(SidebarWindow const& scene)
{
    if (auto* own = scene.visibleItem(QStringLiteral("sidebarNeedsMeButton")); own != nullptr)
        return own;
    if (auto* header = scene.visibleItem(QStringLiteral("headerSidebarNeedsMeButton")); header != nullptr)
        return header;
    if (scene.token("needsMeInFilterRow") == 0)
        return nullptr;
    auto* sidebar = scene.item(QStringLiteral("sidebar"));
    REQUIRE(sidebar != nullptr);
    QMetaObject::invokeMethod(sidebar, "openFilter");
    scene.layOut();
    return scene.visibleItem(QStringLiteral("sidebarFilterNeedsMeButton"));
}
} // namespace

TEST_CASE("the Needs me control flips Accessible.checked and its name (offscreen)",
          "[contour][gui][qml][sidebar][needsme]")
{
    for (auto const row: drawnRows())
    {
        INFO("row " << static_cast<int>(row));
        auto scene =
            SidebarWindow { { .row = row, .visibility = contour::config::SidebarVisibility::Shown } };
        auto* control = needsMeControlOf(scene);
        REQUIRE(control != nullptr);
        auto const* attached = accessibleOf(*control);
        REQUIRE(attached != nullptr);
        auto const allRows = scene.controller.sidebarModel().rows();
        CHECK(attached->property("checkable").toBool());
        CHECK_FALSE(attached->property("checked").toBool());
        CHECK(attached->property("name").toString() == QStringLiteral("Show only what needs me"));

        // The first press narrows the tree to billing's waiting pane and the shown tab's path.
        scene.click(*control);
        CHECK(scene.controller.needsMeToggles == 1);
        CHECK(scene.controller.sidebarModel().scope() == contour::config::SidebarScope::NeedsMe);
        CHECK(attached->property("checked").toBool());
        CHECK(attached->property("name").toString() == QStringLiteral("Show all"));
        CHECK(scene.controller.sidebarModel().rows().size() < allRows.size());
        CHECK(std::ranges::none_of(scene.controller.sidebarModel().rows(),
                                   [](auto const& listed) { return listed.title == "docs"; }));

        // The second, at the same spot, brings every row back.
        CHECK(needsMeControlOf(scene) == control);
        scene.click(*control);
        CHECK(scene.controller.sidebarModel().scope() == contour::config::SidebarScope::All);
        CHECK_FALSE(attached->property("checked").toBool());
        CHECK(attached->property("name").toString() == QStringLiteral("Show only what needs me"));
        CHECK(scene.controller.sidebarModel().rows() == allRows);
        CHECK(scene.warnings.count(isSidebarDiagnostic) == 0);
    }
}

TEST_CASE("beside the filter field, the Needs me control stays while it narrows the tree (offscreen)",
          "[contour][gui][qml][sidebar][needsme]")
{
    for (auto const row: drawnRows())
    {
        INFO("row " << static_cast<int>(row));
        auto scene =
            SidebarWindow { { .row = row, .visibility = contour::config::SidebarVisibility::Shown } };
        if (scene.token("needsMeInFilterRow") == 0)
            continue;
        // Its action, with the filter closed: the row the control stands in opens with it.
        CHECK(scene.visibleItem(QStringLiteral("sidebarFilterNeedsMeButton")) == nullptr);
        scene.controller.toggleSidebarNeedsMe();
        scene.layOut();
        auto* control = scene.visibleItem(QStringLiteral("sidebarFilterNeedsMeButton"));
        REQUIRE(control != nullptr);
        CHECK(control->property("checked").toBool());
        auto const* field = scene.visibleItem(QStringLiteral("sidebarFilterField"));
        REQUIRE(field != nullptr);
        // Beside the field, not over it.
        CHECK(sceneRectOf(*field).right() <= sceneRectOf(*control).left());

        // Its second press, at the same spot, lists everything and lets the row go.
        scene.click(*control);
        CHECK(scene.controller.sidebarModel().scope() == contour::config::SidebarScope::All);
        CHECK(scene.visibleItem(QStringLiteral("sidebarFilterNeedsMeButton")) == nullptr);
        CHECK(scene.warnings.count(isSidebarDiagnostic) == 0);
    }
}

TEST_CASE("its tooltip shows the chord when bound and the plain label when not (offscreen)",
          "[contour][gui][qml][sidebar][needsme][tooltip]")
{
    constexpr auto Bound =
        std::string_view { "  - { mods: [Primary, Alt], key: 'M', action: ToggleSidebarNeedsMe }\n" };
    for (auto const row: drawnRows())
    {
        INFO("row " << static_cast<int>(row));
        auto scene =
            SidebarWindow { { .row = row, .visibility = contour::config::SidebarVisibility::Shown } };
        auto* control = needsMeControlOf(scene);
        REQUIRE(control != nullptr);
        CHECK(control->property("toolTipCommand").toString() == QStringLiteral("ToggleSidebarNeedsMe"));

        scene.controller.chords = contour::command::chordIndex(bindingsOf(row, Bound));
        auto const chord = scene.controller.shortcutText(QStringLiteral("ToggleSidebarNeedsMe"));
        REQUIRE_FALSE(chord.isEmpty());
        auto const* tip = openToolTipOf(*scene.window, *control);
        CHECK(tip->property("text").toString() == QStringLiteral("Show only what needs me"));
        CHECK(tip->property("shownShortcut").toString() == chord);
        CHECK(tip->property("shortcutShown").toBool());

        // Unbound, as by default: the label alone, no stray separator and no invented chord.
        QTest::mouseMove(scene.window, QPoint { scene.window->width() - 5, scene.window->height() - 5 });
        QCoreApplication::processEvents();
        scene.controller.chords = contour::command::ChordIndex {};
        auto const* unbound = openToolTipOf(*scene.window, *control);
        CHECK(unbound->property("shownShortcut").toString().isEmpty());
        CHECK_FALSE(unbound->property("shortcutShown").toBool());
        CHECK(unbound->property("text").toString() == QStringLiteral("Show only what needs me"));
    }
}

TEST_CASE("the empty state's Show all restores the scope (offscreen)",
          "[contour][gui][qml][sidebar][needsme]")
{
    for (auto const row: drawnRows())
    {
        INFO("row " << static_cast<int>(row));
        // Billing's first pane asks nothing; its second only works.
        auto scene = SidebarWindow { { .row = row,
                                       .visibility = contour::config::SidebarVisibility::Shown,
                                       .billing = BillingAsks::Nothing } };
        CHECK(scene.visibleItem(QStringLiteral("sidebarEmptyState")) == nullptr);
        scene.controller.toggleSidebarNeedsMe();
        QCoreApplication::processEvents();
        CHECK(scene.controller.sidebarModel().emptyState()
              == contour::window::SidebarEmptyState::NothingNeedsYou);
        auto* empty = scene.visibleItem(QStringLiteral("sidebarEmptyState"));
        REQUIRE(empty != nullptr);
        auto const* text = SidebarWindow::findNamed(*empty, QStringLiteral("sidebarEmptyStateText"));
        REQUIRE(text != nullptr);
        CHECK(text->property("text").toString() == QStringLiteral("Nothing needs you"));
        auto* showAll = SidebarWindow::findNamed(*empty, QStringLiteral("sidebarEmptyStateShowAll"));
        REQUIRE(showAll != nullptr);
        auto const* attached = accessibleOf(*showAll);
        REQUIRE(attached != nullptr);
        CHECK(attached->property("role").toInt() == QAccessible::Button);
        CHECK(attached->property("name").toString() == QStringLiteral("Show all"));

        scene.click(*showAll);
        CHECK(scene.controller.sidebarModel().scope() == contour::config::SidebarScope::All);
        CHECK(scene.visibleItem(QStringLiteral("sidebarEmptyState")) == nullptr);
        CHECK(scene.rowTitled(QStringLiteral("docs")) != nullptr);
        CHECK(scene.warnings.count(isSidebarDiagnostic) == 0);
    }
}
// }}}

// {{{ The location chip: daemon, dot, state and workspace in one control

namespace
{
/// What the chip says, left to right, the way the Terminal row prints it: its visible parts joined by a
/// space, the brackets hugging what they enclose (`[local › billing ▾]`).
[[nodiscard]] QString chipSpelling(QQuickItem const& chip)
{
    constexpr auto Parts =
        std::array { "workspaceChipOpen",   "workspaceChipDotGlyph",  "workspaceChipDaemon",
                     "workspaceChipStatus", "workspaceChipSeparator", "workspaceChipLabel",
                     "workspaceChipCaret",  "workspaceChipClose" };
    auto spoken = QStringList {};
    for (auto const* name: Parts)
    {
        auto const* part = chip.findChild<QQuickItem*>(QString::fromLatin1(name));
        if (part != nullptr && part->isVisible() && !part->property("text").toString().isEmpty())
            spoken << part->property("text").toString();
    }
    return spoken.join(QLatin1Char(' '))
        .replace(QStringLiteral("[ "), QStringLiteral("["))
        .replace(QStringLiteral(" ]"), QStringLiteral("]"));
}

/// The chip's child named @p name.
[[nodiscard]] QQuickItem* chipPart(QQuickItem const& chip, char const* name)
{
    return chip.findChild<QQuickItem*>(QString::fromLatin1(name));
}
} // namespace

TEST_CASE("the location chip names the daemon, a remote's dot and its state on every row (offscreen)",
          "[contour][gui][qml][sidebar][chip]")
{
    using contour::window::DaemonLocality;
    using contour::window::HostChipState;
    auto const row =
        GENERATE(ChromeStyle::MacOS, ChromeStyle::Fluent, ChromeStyle::ContourLinux, ChromeStyle::Terminal);
    INFO("row " << static_cast<int>(row));
    auto scene = SidebarWindow { { .row = row, .visibility = contour::config::SidebarVisibility::Hidden } };
    auto* chip = scene.item(QStringLiteral("workspaceChip"));
    REQUIRE(chip != nullptr);
    auto const bracketed = row == ChromeStyle::Terminal;

    // The default daemon, up: its name and nothing else before the workspace.
    scene.controller.setDaemon(QStringLiteral("local"));
    QCoreApplication::processEvents();
    CHECK(chipPart(*chip, "workspaceChipDaemon")->property("text").toString() == QStringLiteral("local"));
    CHECK_FALSE(chipPart(*chip, "workspaceChipDot")->isVisible());
    CHECK_FALSE(chipPart(*chip, "workspaceChipDotGlyph")->isVisible());
    CHECK_FALSE(chipPart(*chip, "workspaceChipStatus")->isVisible());
    CHECK(chipPart(*chip, "workspaceChipLabel")->property("text").toString() == QStringLiteral("billing"));
    if (bracketed)
        CHECK(chipSpelling(*chip) == QStringLiteral("[local › billing ▾]"));

    // A remote daemon that is reconnecting: the dot (a glyph on the Terminal row), the host and the
    // words.
    scene.controller.setDaemon(QStringLiteral("starfive"), DaemonLocality::Remote);
    scene.controller.setHostChipState(static_cast<int>(HostChipState::Reconnecting));
    QCoreApplication::processEvents();
    CHECK(chipPart(*chip, "workspaceChipDot")->isVisible() == !bracketed);
    CHECK(chipPart(*chip, "workspaceChipDotGlyph")->isVisible() == bracketed);
    CHECK(chipPart(*chip, "workspaceChipStatus")->property("text").toString()
          == QStringLiteral("Reconnecting…"));
    if (bracketed)
        CHECK(chipSpelling(*chip) == QStringLiteral("[↻ starfive Reconnecting… › billing ▾]"));

    // Each state has its own glyph, so colour is never the only cue.
    if (bracketed)
        for (auto const state: { HostChipState::Connected, HostChipState::Disconnected })
        {
            scene.controller.setHostChipState(static_cast<int>(state));
            QCoreApplication::processEvents();
            CHECK(chipPart(*chip, "workspaceChipDotGlyph")->property("text").toString()
                  == QString::fromUtf8(contour::window::daemonStateGlyph(state)));
        }
    CHECK(scene.warnings.count(isSidebarDiagnostic) == 0);
}

TEST_CASE("the location chip gives way in steps, and the workspace keeps six characters (offscreen)",
          "[contour][gui][qml][sidebar][chip]")
{
    using contour::window::DaemonLocality;
    using contour::window::HostChipState;
    auto const row =
        GENERATE(ChromeStyle::MacOS, ChromeStyle::Fluent, ChromeStyle::ContourLinux, ChromeStyle::Terminal);
    INFO("row " << static_cast<int>(row));
    auto scene = SidebarWindow { { .row = row,
                                   .visibility = contour::config::SidebarVisibility::Hidden,
                                   .billingTitle = "billing-and-invoices" } };
    scene.controller.setDaemon(QStringLiteral("starfive-build-server-0123456789-and-more"),
                               DaemonLocality::Remote);
    scene.controller.setHostChipState(static_cast<int>(HostChipState::Reconnecting));
    QCoreApplication::processEvents();
    auto* chip = scene.item(QStringLiteral("workspaceChip"));
    auto* well = scene.item(QStringLiteral("tabWell"));
    auto* bar = scene.item(QStringLiteral("titleBar"));
    REQUIRE(bar != nullptr);
    REQUIRE(chip != nullptr);
    REQUIRE(well != nullptr);
    auto* const style =
        scene.engine.rootContext()->contextProperty(QStringLiteral("chromeStyle")).value<QObject*>();
    REQUIRE(style != nullptr);
    auto minimum = 0.0;
    REQUIRE(QMetaObject::invokeMethod(style, "tabWellMinimum", Q_RETURN_ARG(qreal, minimum), Q_ARG(int, 2)));

    // The window cannot be narrower than the band's parts need (WindowController::setBandWidths): the
    // window manager holds it there, so the steps below stop at that width.
    auto const windowMinimum = static_cast<int>(std::ceil(scene.controller.bandMinimumWidth()));
    REQUIRE(windowMinimum > 0);

    // Narrowing the window steps the chip down in order, never skipping back.
    auto previous = 0;
    auto lastStepWidth = -1.0;
    for (auto const requested: { 1400, 900, 700, 560, 480, 400, 340, 300 })
    {
        auto const width = std::max(requested, windowMinimum);
        INFO("window width " << width);
        scene.window->resize(width, 640);
        QCoreApplication::processEvents();
        QTest::qWait(30);
        auto const fit = chip->property("fit").toInt();
        INFO("fit " << fit);
        CHECK(fit >= previous);
        previous = fit;
        // The workspace is whole until the last step, which keeps exactly the floor (6 characters) and `…`;
        // and the well keeps at least its minimum, at every width the window can have.
        auto const workspaceFits = fit < static_cast<int>(contour::window::LocationChipFit::WorkspaceElided);
        CHECK(chipPart(*chip, "workspaceChipLabel")->property("text").toString()
              == (workspaceFits ? QStringLiteral("billing-and-invoices") : QStringLiteral("billin…")));
        // The chip fits the room that leaves the tabs their labels, except at its last step, which it cannot
        // go below: there it is one width however narrow the window gets (the window's minimum keeps the room
        // for exactly that, and one icon-only tab).
        if (fit == static_cast<int>(contour::window::LocationChipFit::WorkspaceElided))
        {
            if (lastStepWidth < 0)
                lastStepWidth = chip->width();
            CHECK(chip->width() == lastStepWidth);
        }
        else
            CHECK(chip->width() <= bar->property("chipRoom").toReal() + 0.5);
        CHECK(well->width() >= minimum - 0.5);
        // The state's words go first, the daemon's name next.
        CHECK(chipPart(*chip, "workspaceChipStatus")->isVisible() == (fit == 0));
        CHECK(chipPart(*chip, "workspaceChipDaemon")->isVisible() == (fit <= 2));
        // The dot stays through every step.
        CHECK((chipPart(*chip, "workspaceChipDot")->isVisible()
               || chipPart(*chip, "workspaceChipDotGlyph")->isVisible()));
        if (fit == 2)
            CHECK(chipPart(*chip, "workspaceChipDaemon")->property("text").toString().size() <= 10);
    }
    // The narrowest window reached the end of the steps.
    CHECK(previous >= 3);
    CHECK(scene.warnings.count(isSidebarDiagnostic) == 0);
}

TEST_CASE("the location chip yields before the tabs go icon-only (offscreen)",
          "[contour][gui][qml][sidebar][chip]")
{
    using contour::window::DaemonLocality;
    using contour::window::HostChipState;
    using contour::window::LocationChipFit;
    auto const row =
        GENERATE(ChromeStyle::MacOS, ChromeStyle::Fluent, ChromeStyle::ContourLinux, ChromeStyle::Terminal);
    INFO("row " << static_cast<int>(row));
    // Three tabs and a long remote daemon that is reconnecting: the chip's widest case.
    auto scene = SidebarWindow { { .row = row,
                                   .visibility = contour::config::SidebarVisibility::Hidden,
                                   .billingTitle = "billing-and-invoices",
                                   .tabCount = 3 } };
    scene.controller.setDaemon(
        QStringLiteral("starfive-build-server-0123456789-and-more-of-its-very-long-name"),
        DaemonLocality::Remote);
    scene.controller.setHostChipState(static_cast<int>(HostChipState::Reconnecting));
    QCoreApplication::processEvents();
    auto* chip = scene.item(QStringLiteral("workspaceChip"));
    auto* strip = scene.item(QStringLiteral("tabStrip"));
    auto* well = scene.item(QStringLiteral("tabWell"));
    auto* bar = scene.item(QStringLiteral("titleBar"));
    REQUIRE(chip != nullptr);
    REQUIRE(strip != nullptr);
    REQUIRE(well != nullptr);
    REQUIRE(bar != nullptr);
    REQUIRE(strip->property("tabCount").toInt() == 3);
    auto const pills = strip->property("pill").toBool();

    // Whatever the window's width, no tab goes icon-only while the chip could still give way: each tab keeps
    // at least its minimum (its label), and the chip sits wherever its room leaves it.
    auto const windowMinimum = static_cast<int>(std::ceil(scene.controller.bandMinimumWidth()));
    auto sawElidedChip = false;
    for (auto const requested: { 1400, 1000, 800, 700 })
    {
        auto const width = std::max(requested, windowMinimum);
        INFO("window width " << width);
        scene.window->resize(width, 640);
        QCoreApplication::processEvents();
        QTest::qWait(30);
        auto const fit = chip->property("fit").toInt();
        INFO("chip fit " << fit);
        if (pills)
        {
            auto const layout = strip->property("wellLayout").toMap();
            CHECK(layout.value(QStringLiteral("label")).toString() == QStringLiteral("full"));
            CHECK(layout.value(QStringLiteral("tabWidth")).toReal() >= scene.style("minTabWidth") - 0.5);
        }
        sawElidedChip = sawElidedChip || fit > static_cast<int>(LocationChipFit::Full);

        // The chip takes the first step, in elision order, that fits the room the band left it -- the last
        // step when none does. Read against the chip's own measured widths, so it holds whatever widths the
        // platform's fonts give the text.
        auto const widths = chip->property("fitWidths").toMap();
        auto const room = bar->property("chipRoom").toReal();
        auto const widthOf = [&](LocationChipFit step) {
            auto const key = contour::window::LocationChipSteps.at(static_cast<size_t>(step)).key;
            return widths.value(QString::fromUtf8(key.data(), static_cast<qsizetype>(key.size()))).toReal();
        };
        auto const taken = static_cast<LocationChipFit>(fit);
        INFO("chip room " << room << ", step width " << widthOf(taken));
        if (taken != contour::window::LocationChipFits.back())
            CHECK(widthOf(taken) <= room + 0.5);
        if (taken != LocationChipFit::Full)
            CHECK(widthOf(static_cast<LocationChipFit>(fit - 1)) > room - 0.5);
        if (row == ChromeStyle::Terminal)
        {
            // The Terminal row counts in cells: its well is a whole number of them, three at least.
            auto const cells = well->width() / scene.style("cellWidth");
            CHECK(std::abs(cells - std::round(cells)) < 0.01);
            CHECK(cells >= 3);
        }
        CHECK(well->width() > 0);
    }
    CHECK(sawElidedChip);
    CHECK(scene.warnings.count(isSidebarDiagnostic) == 0);
}

TEST_CASE("the window's minimum width keeps the tab well and the chip's last step (offscreen)",
          "[contour][gui][qml][sidebar][chip]")
{
    using contour::window::DaemonLocality;
    auto const row =
        GENERATE(ChromeStyle::MacOS, ChromeStyle::Fluent, ChromeStyle::ContourLinux, ChromeStyle::Terminal);
    INFO("row " << static_cast<int>(row));
    auto scene = SidebarWindow { { .row = row,
                                   .visibility = contour::config::SidebarVisibility::Hidden,
                                   .billingTitle = "billing-and-invoices" } };
    scene.controller.setDaemon(QStringLiteral("starfive-build-server-0123456789-and-more"),
                               DaemonLocality::Remote);
    scene.controller.setHostChipState(static_cast<int>(contour::window::HostChipState::Reconnecting));
    QCoreApplication::processEvents();
    auto* chip = scene.item(QStringLiteral("workspaceChip"));
    auto* well = scene.item(QStringLiteral("tabWell"));
    REQUIRE(chip != nullptr);
    REQUIRE(well != nullptr);
    auto* const style =
        scene.engine.rootContext()->contextProperty(QStringLiteral("chromeStyle")).value<QObject*>();
    REQUIRE(style != nullptr);
    auto minimum = 0.0;
    REQUIRE(QMetaObject::invokeMethod(style, "tabWellMinimum", Q_RETURN_ARG(qreal, minimum), Q_ARG(int, 2)));

    auto const windowMinimum = static_cast<int>(std::ceil(scene.controller.bandMinimumWidth()));
    scene.window->resize(windowMinimum, 640);
    QCoreApplication::processEvents();
    QTest::qWait(30);

    // At the narrowest the window can be, the chip is at its last step and the well still has its minimum.
    CHECK(chip->property("fit").toInt()
          == static_cast<int>(contour::window::LocationChipFit::WorkspaceElided));
    CHECK(well->width() >= minimum - 0.5);
    // The workspace keeps exactly the floor and the ellipsis, while the daemon reconnects.
    CHECK(chipPart(*chip, "workspaceChipLabel")->property("text").toString() == QStringLiteral("billin…"));
    CHECK(scene.warnings.count(isSidebarDiagnostic) == 0);
}

TEST_CASE("the window's minimum width does not move with the connection or a marker (offscreen)",
          "[contour][gui][qml][sidebar][chip]")
{
    using contour::window::HostChipState;
    auto const row =
        GENERATE(ChromeStyle::MacOS, ChromeStyle::Fluent, ChromeStyle::ContourLinux, ChromeStyle::Terminal);
    INFO("row " << static_cast<int>(row));
    auto const minimumOf = [&](BillingAsks asks, HostChipState state, int& windowMinimum) {
        auto scene = SidebarWindow { { .row = row,
                                       .visibility = contour::config::SidebarVisibility::Hidden,
                                       .shownWorkspace = 2,
                                       .billing = asks } };
        scene.controller.setDaemon(QStringLiteral("local"));
        scene.controller.setHostChipState(static_cast<int>(HostChipState::Connected));
        QCoreApplication::processEvents();
        auto* well = scene.item(QStringLiteral("tabWell"));
        REQUIRE(well != nullptr);
        auto* const style =
            scene.engine.rootContext()->contextProperty(QStringLiteral("chromeStyle")).value<QObject*>();
        REQUIRE(style != nullptr);
        auto wellMinimum = 0.0;
        REQUIRE(QMetaObject::invokeMethod(
            style, "tabWellMinimum", Q_RETURN_ARG(qreal, wellMinimum), Q_ARG(int, 2)));
        auto const connected = scene.controller.bandMinimumWidth();
        auto* marker = scene.item(QStringLiteral("workspaceChipMarker"));
        REQUIRE(marker != nullptr);
        CHECK(marker->isVisible() == (asks == BillingAsks::Permission));

        // The window sits at its narrowest, then the link goes down: nothing may ask for more room.
        windowMinimum = static_cast<int>(std::ceil(connected));
        scene.window->resize(windowMinimum, 640);
        scene.controller.setHostChipState(static_cast<int>(state));
        QCoreApplication::processEvents();
        QTest::qWait(30);
        CHECK(scene.controller.bandMinimumWidth() == connected);
        CHECK(well->width() >= wellMinimum - 0.5);
        return connected;
    };
    auto unusedMinimum = 0;
    auto const reference = minimumOf(BillingAsks::Nothing, HostChipState::Connected, unusedMinimum);
    CHECK(reference > 0);
    CHECK(minimumOf(BillingAsks::Nothing, HostChipState::Reconnecting, unusedMinimum) == reference);
    CHECK(minimumOf(BillingAsks::Nothing, HostChipState::Disconnected, unusedMinimum) == reference);
    // Another workspace asks for you: the marker shows, and the minimum is the same.
    CHECK(minimumOf(BillingAsks::Permission, HostChipState::Connected, unusedMinimum) == reference);
    CHECK(minimumOf(BillingAsks::Permission, HostChipState::Reconnecting, unusedMinimum) == reference);
}

TEST_CASE("another workspace's marker does not move the chip's fit or the tab layout (offscreen)",
          "[contour][gui][qml][sidebar][chip]")
{
    auto const row =
        GENERATE(ChromeStyle::MacOS, ChromeStyle::Fluent, ChromeStyle::ContourLinux, ChromeStyle::Terminal);
    INFO("row " << static_cast<int>(row));
    // At a width where the chip is stepping down, so that a few pixels of marker would change the answer.
    struct Layout
    {
        int fit = -1;
        QVariantMap widths;
        QVariantMap tabs;
        bool markerShown = false;
    };
    auto const layoutWith = [&](BillingAsks asks, int width) {
        auto scene = SidebarWindow { { .row = row,
                                       .visibility = contour::config::SidebarVisibility::Hidden,
                                       .shownWorkspace = 2,
                                       .billing = asks,
                                       .billingTitle = "billing-and-invoices",
                                       .tabCount = 3 } };
        scene.controller.setDaemon(QStringLiteral("local"));
        QCoreApplication::processEvents();
        auto* chip = scene.item(QStringLiteral("workspaceChip"));
        auto* bar = scene.item(QStringLiteral("titleBar"));
        auto* marker = scene.item(QStringLiteral("workspaceChipMarker"));
        REQUIRE(chip != nullptr);
        REQUIRE(bar != nullptr);
        REQUIRE(marker != nullptr);
        scene.window->resize(
            std::max(width, static_cast<int>(std::ceil(scene.controller.bandMinimumWidth()))), 640);
        QCoreApplication::processEvents();
        QTest::qWait(30);
        return Layout { .fit = chip->property("fit").toInt(),
                        .widths = chip->property("fitWidths").toMap(),
                        .tabs = bar->property("bandAllocation").toMap(),
                        .markerShown = marker->isVisible() };
    };
    for (auto const width: { 1000, 800, 700, 600, 500 })
    {
        INFO("window width " << width);
        auto const without = layoutWith(BillingAsks::Nothing, width);
        auto const with = layoutWith(BillingAsks::Permission, width);
        CHECK_FALSE(without.markerShown);
        CHECK(with.markerShown);
        // Every step is measured as if the marker were there, so it never re-fits the band: the allocation of
        // the room between the chip and the tabs is the same (the marker's own pixels still sit in the chip).
        CHECK(with.widths == without.widths);
        CHECK(with.fit == without.fit);
        CHECK(with.tabs == without.tabs);
    }
}

TEST_CASE("the location chip's tooltip names the switcher and the chord bound now, or none (offscreen)",
          "[contour][gui][qml][sidebar][chip][tooltip]")
{
    constexpr auto Bound =
        std::string_view { "  - { mods: [Primary, Alt], key: 'R', action: ShowSwitcher }\n" };
    auto const row =
        GENERATE(ChromeStyle::MacOS, ChromeStyle::Fluent, ChromeStyle::ContourLinux, ChromeStyle::Terminal);
    INFO("row " << static_cast<int>(row));
    auto scene = SidebarWindow { { .row = row, .visibility = contour::config::SidebarVisibility::Hidden } };
    scene.controller.setDaemon(QStringLiteral("local"));
    QCoreApplication::processEvents();
    auto* chip = scene.item(QStringLiteral("workspaceChip"));
    REQUIRE(chip != nullptr);

    auto const* host = chip->findChild<QQuickItem*>(QStringLiteral("kitToolTipHost"));
    REQUIRE(host != nullptr);
    CHECK(host->property("text").toString().startsWith(QStringLiteral("Switch workspace or daemon")));
    CHECK(host->property("shortcutCommand").toString() == QStringLiteral("ShowSwitcher"));

    // Bound: the chord is read from the binding as the tooltip is about to show.
    scene.controller.chords = contour::command::chordIndex(bindingsOf(row, Bound));
    auto const chord = scene.controller.shortcutText(QStringLiteral("ShowSwitcher"));
    REQUIRE_FALSE(chord.isEmpty());
    auto const* tip = openToolTipOf(*scene.window, *chip);
    CHECK(tip->property("shownShortcut").toString() == chord);
    CHECK(tip->property("shortcutShown").toBool());

    // Unbound (rule 5.5): the text alone, no stray separator and no invented chord.
    QTest::mouseMove(scene.window, QPoint { 5, scene.window->height() - 5 });
    QCoreApplication::processEvents();
    scene.controller.chords = contour::command::ChordIndex {};
    auto const* unbound = openToolTipOf(*scene.window, *chip);
    CHECK(unbound->property("shownShortcut").toString().isEmpty());
    CHECK_FALSE(unbound->property("shortcutShown").toBool());
    CHECK(unbound->property("text").toString().startsWith(QStringLiteral("Switch workspace or daemon")));
}
// }}}

// {{{ The sidebar's head: no daemon chip on a daemon's window, the switcher and the host chip of a tmux one

namespace
{
/// The host control a shown sidebar carries (a tmux window's): its own title row's, or on a row whose header
/// bar draws that row, the header bar's part above the sidebar.
[[nodiscard]] QQuickItem* sidebarDaemonChipOf(SidebarWindow const& scene)
{
    if (auto* own = scene.visibleItem(QStringLiteral("sidebarDaemonChip")); own != nullptr)
        return own;
    return scene.visibleItem(QStringLiteral("headerSidebarDaemonChip"));
}

/// The switcher control a shown sidebar carries: its own title row's, or the header bar's part's.
[[nodiscard]] QQuickItem* switcherControlOf(SidebarWindow const& scene)
{
    if (auto* own = scene.visibleItem(QStringLiteral("sidebarSwitcherButton")); own != nullptr)
        return own;
    return scene.visibleItem(QStringLiteral("headerSidebarSwitcherButton"));
}

/// Makes @p scene's window a daemon's: the chip's name, and the tree's own section headed by @p name, with
/// @p others listed beside it.
void onDaemon(SidebarWindow& scene,
              QString const& name,
              contour::window::DaemonLinkState state = contour::window::DaemonLinkState::Connected,
              std::vector<contour::window::DaemonSection> others = {})
{
    scene.controller.setDaemon(name);
    scene.controller.sidebarHost().own = contour::window::OwnDaemon {
        .daemon = contour::window::localDaemonRef("default"),
        .instance = contour::window::DaemonInstance { 1 },
        .name = name.toStdString(),
        .locality = contour::window::DaemonLocality::Local,
        .state = state,
    };
    scene.controller.sidebarSections().listed = std::move(others);
    scene.controller.sidebarModel().refreshNow();
    scene.layOut();
}

/// starfive, connected, with one workspace of one tab.
[[nodiscard]] contour::window::DaemonSection starfiveSection()
{
    auto section = contour::window::DaemonSection {};
    section.daemon = contour::window::hostDaemonRef("starfive");
    section.instance = contour::window::DaemonInstance { 9 };
    section.name = "starfive";
    section.state = contour::window::DaemonLinkState::Connected;
    section.directory.workspaces = { contour::test::directoryWorkspaceOf(
        5, "ci", { contour::test::directoryPaneOf(50, "build") }) };
    section.feed = contour::window::SectionFeed::Observer;
    return section;
}

/// Makes @p scene's window a tmux attach's: a host with no daemon behind it.
void onTmuxHost(SidebarWindow& scene)
{
    scene.controller.attachedHost = QStringLiteral("tmuxhost");
    scene.controller.setHosting(contour::window::DaemonHosting::OtherHost);
    QCoreApplication::processEvents();
}
} // namespace

TEST_CASE("no daemon chip in the sidebar or header bar of a daemon window (offscreen)",
          "[contour][gui][qml][sidebar][chip]")
{
    for (auto const placement: { contour::config::TabPlacement::Sidebar, contour::config::TabPlacement::Top })
        for (auto const row: drawnRows())
        {
            INFO("row " << static_cast<int>(row) << ", placement " << static_cast<int>(placement));
            // Tabs on top with the strip off: some rows draw no band at all, which used to make the sidebar's
            // chip the one place that named the daemon. The tree's heading row is that place now.
            auto scene = SidebarWindow { { .row = row,
                                           .visibility = contour::config::SidebarVisibility::Shown,
                                           .placement = placement,
                                           .tabBar = placement == contour::config::TabPlacement::Top
                                                         ? contour::config::TabBarVisibility::Never
                                                         : contour::config::TabBarVisibility::Always } };
            onDaemon(scene, QStringLiteral("local"));
            CHECK(scene.controller.chip()->place() == QStringLiteral("none"));
            CHECK(sidebarDaemonChipOf(scene) == nullptr);
            CHECK(scene.visibleItem(QStringLiteral("workspaceChip")) == nullptr);
            // The daemon is named once, by its heading row.
            auto const& rows = scene.controller.sidebarModel().rows();
            REQUIRE_FALSE(rows.empty());
            CHECK(rows.front().key.kind == contour::window::SidebarRowKind::Daemon);
            CHECK(rows.front().title == "local");
            CHECK(switcherControlOf(scene) != nullptr);
            CHECK(scene.warnings.count(isSidebarDiagnostic) == 0);
        }
}

TEST_CASE("the chip returns to the band while the own link is down (offscreen)",
          "[contour][gui][qml][sidebar][chip]")
{
    using contour::window::HostChipState;
    for (auto const row: drawnRows())
    {
        INFO("row " << static_cast<int>(row));
        auto scene =
            SidebarWindow { { .row = row, .visibility = contour::config::SidebarVisibility::Shown } };
        onDaemon(scene, QStringLiteral("starfive"), contour::window::DaemonLinkState::Reconnecting);
        scene.controller.daemonLocality = contour::window::DaemonLocality::Remote;
        scene.controller.setHostChipState(static_cast<int>(HostChipState::Reconnecting));
        QCoreApplication::processEvents();
        CHECK(scene.controller.chip()->place() == QStringLiteral("band"));
        CHECK(sidebarDaemonChipOf(scene) == nullptr);
        auto* chip = scene.visibleItem(QStringLiteral("workspaceChip"));
        REQUIRE(chip != nullptr);
        CHECK(chipPart(*chip, "workspaceChipDaemon")
                  ->property("text")
                  .toString()
                  .startsWith(QStringLiteral("starfive")));
        // The connection's dot says why the chip is back.
        auto const bracketed = row == ChromeStyle::Terminal;
        CHECK(chipPart(*chip, "workspaceChipDot")->isVisible() == !bracketed);
        CHECK(chipPart(*chip, "workspaceChipDotGlyph")->isVisible() == bracketed);

        // Back up, the band lets it go again.
        scene.controller.setHostChipState(static_cast<int>(HostChipState::Connected));
        QCoreApplication::processEvents();
        CHECK(scene.controller.chip()->place() == QStringLiteral("none"));
        CHECK(scene.visibleItem(QStringLiteral("workspaceChip")) == nullptr);
        CHECK(scene.warnings.count(isSidebarDiagnostic) == 0);
    }
}

TEST_CASE("the switcher control opens the switcher and shows the bound ShowSwitcher chord (offscreen)",
          "[contour][gui][qml][sidebar][chip][tooltip]")
{
    constexpr auto Bound =
        std::string_view { "  - { mods: [Primary, Alt], key: 'R', action: ShowSwitcher }\n" };
    for (auto const row: drawnRows())
    {
        INFO("row " << static_cast<int>(row));
        auto scene =
            SidebarWindow { { .row = row, .visibility = contour::config::SidebarVisibility::Shown } };
        // In process: no daemon to switch, so no control.
        CHECK(switcherControlOf(scene) == nullptr);
        onDaemon(scene, QStringLiteral("local"));
        auto* control = switcherControlOf(scene);
        REQUIRE(control != nullptr);
        CHECK(control->property("toolTipCommand").toString() == QStringLiteral("ShowSwitcher"));
        auto const* attached = accessibleOf(*control);
        REQUIRE(attached != nullptr);
        CHECK(attached->property("role").toInt() == QAccessible::Button);
        CHECK(attached->property("name").toString() == QStringLiteral("Switch workspace or daemon"));

        // A click drops the switcher down below the control (ui-ux 4.2).
        scene.click(*control);
        REQUIRE(scene.controller.workspaceDropdownAnchor.has_value());
        auto const controlRect = sceneRectOf(*control);
        auto const& anchor = *scene.controller.workspaceDropdownAnchor;
        CHECK(anchor.left() == Catch::Approx(controlRect.left()));
        CHECK(anchor.top() == Catch::Approx(controlRect.top()));
        CHECK(anchor.bottom() == Catch::Approx(controlRect.bottom()));

        // Its tooltip names the chord bound now, or none.
        scene.controller.chords = contour::command::chordIndex(bindingsOf(row, Bound));
        auto const chord = scene.controller.shortcutText(QStringLiteral("ShowSwitcher"));
        REQUIRE_FALSE(chord.isEmpty());
        auto const* tip = openToolTipOf(*scene.window, *control);
        CHECK(tip->property("text").toString() == QStringLiteral("Switch workspace or daemon"));
        CHECK(tip->property("shownShortcut").toString() == chord);
        QTest::mouseMove(scene.window, QPoint { scene.window->width() - 5, scene.window->height() - 5 });
        QCoreApplication::processEvents();
        scene.controller.chords = contour::command::ChordIndex {};
        auto const* unbound = openToolTipOf(*scene.window, *control);
        CHECK(unbound->property("shownShortcut").toString().isEmpty());
        CHECK_FALSE(unbound->property("shortcutShown").toBool());
        CHECK(scene.warnings.count(isSidebarDiagnostic) == 0);
    }
}

TEST_CASE("Ctrl+F6 reaches both controls (offscreen)", "[contour][gui][qml][sidebar][chip]")
{
    for (auto const row: drawnRows())
    {
        INFO("row " << static_cast<int>(row));
        auto scene =
            SidebarWindow { { .row = row, .visibility = contour::config::SidebarVisibility::Shown } };
        onDaemon(scene, QStringLiteral("local"));
        // A look that keeps the Needs-me control beside the filter field has no toolbar item for it.
        auto const needsMeInTitle = scene.token("needsMeInFilterRow") == 0;
        auto* switcher = switcherControlOf(scene);
        REQUIRE(switcher != nullptr);

        // The walk counts both, the switcher at the window's leading end first.
        CHECK(scene.controller.reportedToolbarItems >= (needsMeInTitle ? 2 : 1));
        scene.controller.focusToolbarItem(0);
        QCoreApplication::processEvents();
        CHECK(switcher->hasActiveFocus());
        CHECK(switcher->property("focusRingShown").toBool());
        QTest::keyClick(scene.window, Qt::Key_Return);
        QCoreApplication::processEvents();
        CHECK(scene.controller.workspaceDropdownAnchor.has_value());
        CHECK(scene.warnings.count(isSidebarDiagnostic) == 0);
        if (!needsMeInTitle)
            continue;

        auto* needsMe = needsMeControlOf(scene);
        REQUIRE(needsMe != nullptr);
        scene.controller.focusToolbarItem(1);
        QCoreApplication::processEvents();
        CHECK(needsMe->hasActiveFocus());
        CHECK(needsMe->property("focusRingShown").toBool());
        QTest::keyClick(scene.window, Qt::Key_Space);
        QCoreApplication::processEvents();
        CHECK(scene.controller.sidebarModel().scope() == contour::config::SidebarScope::NeedsMe);
        CHECK(scene.warnings.count(isSidebarDiagnostic) == 0);
    }
}

TEST_CASE("a tmux window keeps its host chip (offscreen)", "[contour][gui][qml][sidebar][chip]")
{
    for (auto const row: drawnRows())
    {
        INFO("row " << static_cast<int>(row));
        auto scene =
            SidebarWindow { { .row = row, .visibility = contour::config::SidebarVisibility::Shown } };
        onTmuxHost(scene);
        CHECK(scene.controller.chip()->place() == QStringLiteral("sidebar"));
        auto* chip = sidebarDaemonChipOf(scene);
        REQUIRE(chip != nullptr);
        CHECK(chipPart(*chip, "workspaceChipDaemon")
                  ->property("text")
                  .toString()
                  .startsWith(QStringLiteral("tmuxhost")));
        CHECK(chipPart(*chip, "workspaceChipCaret")->isVisible());
        CHECK_FALSE(chipPart(*chip, "workspaceChipLabel")->isVisible());
        CHECK(scene.visibleItem(QStringLiteral("workspaceChip")) == nullptr);
        // The chip opens the switcher, so no switcher control stands beside it.
        CHECK(switcherControlOf(scene) == nullptr);
        auto const* host = chip->findChild<QQuickItem*>(QStringLiteral("kitToolTipHost"));
        REQUIRE(host != nullptr);
        CHECK(host->property("shortcutCommand").toString() == QStringLiteral("ShowSwitcher"));

        // A click drops the switcher down below it.
        scene.click(*chip);
        REQUIRE(scene.controller.workspaceDropdownAnchor.has_value());
        auto const chipRect = sceneRectOf(*chip);
        CHECK(scene.controller.workspaceDropdownAnchor->left() == Catch::Approx(chipRect.left()));
        CHECK(scene.controller.workspaceDropdownAnchor->bottom() == Catch::Approx(chipRect.bottom()));
        CHECK(accessibleOf(*chip)->property("role").toInt() == QAccessible::ButtonMenu);

        // It leads the chrome's keyboard walk; Return and Space open the switcher.
        scene.controller.focusToolbarItem(0);
        QCoreApplication::processEvents();
        CHECK(chip->hasActiveFocus());
        CHECK(chip->property("focusRingShown").toBool());
        for (auto const key: { Qt::Key_Return, Qt::Key_Space })
        {
            INFO("key " << static_cast<int>(key));
            scene.controller.workspaceDropdownAnchor.reset();
            QTest::keyClick(scene.window, key);
            QCoreApplication::processEvents();
            CHECK(scene.controller.workspaceDropdownAnchor.has_value());
        }

        // No tree row names a tmux host, so its chip stays whatever its connection says.
        scene.controller.setHostChipState(static_cast<int>(contour::window::HostChipState::Reconnecting));
        QCoreApplication::processEvents();
        CHECK(scene.controller.chip()->place() == QStringLiteral("sidebar"));
        CHECK(sidebarDaemonChipOf(scene) != nullptr);
        CHECK(scene.warnings.count(isSidebarDiagnostic) == 0);
    }
}

TEST_CASE("a tmux window with the sidebar hidden names its host alone in the band (offscreen)",
          "[contour][gui][qml][sidebar][chip]")
{
    for (auto const row: drawnRows())
    {
        INFO("row " << static_cast<int>(row));
        auto scene = SidebarWindow { { .row = row,
                                       .visibility = contour::config::SidebarVisibility::Hidden,
                                       .placement = contour::config::TabPlacement::Top } };
        onTmuxHost(scene);
        auto* chip = scene.visibleItem(QStringLiteral("workspaceChip"));
        REQUIRE(chip != nullptr);
        CHECK(chip->property("namesDaemonOnly").toBool());
        CHECK(chipPart(*chip, "workspaceChipDaemon")->property("text").toString()
              == QStringLiteral("tmuxhost"));
        CHECK_FALSE(chipPart(*chip, "workspaceChipLabel")->isVisible());
    }
}

TEST_CASE("a tmux window with no band names its host at the sidebar's head (offscreen)",
          "[contour][gui][qml][sidebar][chip]")
{
    auto hiddenBands = 0;
    for (auto const row: drawnRows())
    {
        INFO("row " << static_cast<int>(row));
        auto scene = SidebarWindow { { .row = row,
                                       .visibility = contour::config::SidebarVisibility::Shown,
                                       .placement = contour::config::TabPlacement::Top,
                                       .tabBar = contour::config::TabBarVisibility::Never } };
        onTmuxHost(scene);
        auto const* bar = scene.item(QStringLiteral("titleBar"));
        REQUIRE(bar != nullptr);
        if (bar->property("presence").toString() != QStringLiteral("hidden"))
            continue;
        ++hiddenBands;
        auto* chip = sidebarDaemonChipOf(scene);
        REQUIRE(chip != nullptr);
        CHECK(chip->objectName() == QStringLiteral("sidebarDaemonChip"));
        CHECK(scene.visibleItem(QStringLiteral("workspaceChip")) == nullptr);
    }
    if (hiddenBands == 0)
        SKIP("no row of this style hides its band here");
}

TEST_CASE("the breadcrumb names the daemon with two daemons listed (offscreen)",
          "[contour][gui][qml][sidebar]")
{
    for (auto const row: drawnRows())
    {
        INFO("row " << static_cast<int>(row));
        auto scene =
            SidebarWindow { { .row = row, .visibility = contour::config::SidebarVisibility::Shown } };
        auto* bar = scene.item(QStringLiteral("titleBar"));
        REQUIRE(bar != nullptr);
        if (bar->property("centre").toString() != QStringLiteral("breadcrumb"))
            SKIP("this row's band does not show a breadcrumb here");
        auto* daemon = scene.item(QStringLiteral("breadcrumbDaemon"));
        REQUIRE(daemon != nullptr);

        // One daemon goes without saying.
        onDaemon(scene, QStringLiteral("local"));
        CHECK(scene.controller.sidebarModel().sectionCount() == 1);
        CHECK_FALSE(daemon->isVisible());

        // Beside another, the breadcrumb leads with it: `local › billing › claude`.
        onDaemon(scene,
                 QStringLiteral("local"),
                 contour::window::DaemonLinkState::Connected,
                 { starfiveSection() });
        CHECK(scene.controller.sidebarModel().sectionCount() == 2);
        CHECK(daemon->isVisible());
        CHECK(daemon->property("text").toString() == QStringLiteral("local"));
        CHECK(scene.item(QStringLiteral("breadcrumbDaemonSeparator"))->isVisible());
        CHECK(scene.item(QStringLiteral("breadcrumbWorkspace"))->property("text").toString()
              == QStringLiteral("billing"));
        auto const* crumb = scene.item(QStringLiteral("breadcrumb"));
        REQUIRE(crumb != nullptr);
        CHECK(accessibleOf(*crumb)->property("name").toString().startsWith(QStringLiteral("local, billing")));
        CHECK(scene.warnings.count(isSidebarDiagnostic) == 0);
    }
}

TEST_CASE("with the sidebar hidden, a breadcrumb band holds the daemon's chip (offscreen)",
          "[contour][gui][qml][sidebar][chip]")
{
    auto const row =
        GENERATE(ChromeStyle::MacOS, ChromeStyle::Fluent, ChromeStyle::ContourLinux, ChromeStyle::Terminal);
    INFO("row " << static_cast<int>(row));
    auto scene = SidebarWindow { { .row = row, .visibility = contour::config::SidebarVisibility::Hidden } };
    scene.controller.setDaemon(QStringLiteral("local"));
    QCoreApplication::processEvents();
    auto const* bar = scene.item(QStringLiteral("titleBar"));
    REQUIRE(bar != nullptr);
    // The sidebar holds the tabs, so the band's middle is the breadcrumb; with the sidebar off screen no
    // control heads it, so the band names the daemon.
    if (bar->property("centre").toString() != QStringLiteral("breadcrumb"))
        SKIP("this row's band does not show a breadcrumb here");
    CHECK(bar->property("chipPlace").toString() == QStringLiteral("band"));
    auto* chip = scene.visibleItem(QStringLiteral("workspaceChip"));
    REQUIRE(chip != nullptr);
    CHECK(chipPart(*chip, "workspaceChipDaemon")
              ->property("text")
              .toString()
              .startsWith(QStringLiteral("local")));
}

TEST_CASE("in process a shown sidebar keeps its heading and carries no daemon control (offscreen)",
          "[contour][gui][qml][sidebar][chip]")
{
    auto const row =
        GENERATE(ChromeStyle::MacOS, ChromeStyle::Fluent, ChromeStyle::ContourLinux, ChromeStyle::Terminal);
    INFO("row " << static_cast<int>(row));
    auto scene = SidebarWindow { { .row = row, .visibility = contour::config::SidebarVisibility::Shown } };
    CHECK(sidebarDaemonChipOf(scene) == nullptr);
    auto* title = scene.visibleItem(QStringLiteral("sidebarTitle"));
    if (title == nullptr)
        title = scene.visibleItem(QStringLiteral("headerSidebarTitle"));
    // macOS leaves the row unnamed (no heading to replace); the others read "Workspaces".
    if (row == ChromeStyle::MacOS)
        CHECK(title == nullptr);
    else
    {
        REQUIRE(title != nullptr);
        CHECK(title->property("text").toString() == QStringLiteral("Workspaces"));
    }
}

TEST_CASE("no look elides the sidebar's heading at its default width (offscreen)",
          "[contour][gui][qml][sidebar][chip]")
{
    skipWithoutPlatformUiFont();
    auto const row =
        GENERATE(ChromeStyle::MacOS, ChromeStyle::Fluent, ChromeStyle::ContourLinux, ChromeStyle::Terminal);
    // A daemon's window stands its switcher beside the heading too.
    auto const daemon = GENERATE(false, true);
    // The default width is sized against the face the heading is set in: a wide one widens it.
    auto const font = GENERATE(SceneFont::Application, SceneFont::Wide);
    INFO("row " << static_cast<int>(row) << ", daemon " << daemon << ", font " << static_cast<int>(font));
    auto scene = SidebarWindow {
        { .row = row, .visibility = contour::config::SidebarVisibility::Shown, .font = font }
    };
    if (daemon)
        onDaemon(scene, QStringLiteral("local"));
    scene.layOut();
    auto* title = scene.visibleItem(QStringLiteral("sidebarTitle"));
    if (title == nullptr)
        title = scene.visibleItem(QStringLiteral("headerSidebarTitle"));
    if (row == ChromeStyle::MacOS)
        CHECK(title == nullptr);
    else
    {
        REQUIRE(title != nullptr);
        CHECK(title->width() > 0);
        CHECK_FALSE(title->property("truncated").toBool());
    }
}
// }}}

// {{{ Tab colour
namespace
{

/// @return @p value's colour as one comparable number, whatever spec it was made in.
[[nodiscard]] QRgb rgbaOf(QVariant const& value)
{
    return value.value<QColor>().rgba();
}

/// What `chromeStyle.sidebarTabColor` paints on @p row as it stands: under the pointer as it is, in its
/// window as active as the window the test drives really is.
[[nodiscard]] QVariantMap tabPaintOf(SidebarWindow const& scene, QQuickItem const& row)
{
    auto* style =
        scene.engine.rootContext()->contextProperty(QStringLiteral("chromeStyle")).value<QObject*>();
    REQUIRE(style != nullptr);
    auto const* colors = style->property("colors").value<QObject*>();
    REQUIRE(colors != nullptr);
    auto paint = QVariantMap {};
    REQUIRE(QMetaObject::invokeMethod(style,
                                      "sidebarTabColor",
                                      Q_RETURN_ARG(QVariantMap, paint),
                                      Q_ARG(int, scene.controller.sidebarTabColorStyle()),
                                      Q_ARG(QColor, row.property("tabColor").value<QColor>()),
                                      Q_ARG(QColor, colors->property("sidebarFill").value<QColor>()),
                                      Q_ARG(int, row.property("selection").toInt()),
                                      Q_ARG(int, row.property("pointer").toInt()),
                                      Q_ARG(bool, scene.window->isActive())));
    return paint;
}

/// The part of @p row named @p name.
[[nodiscard]] QQuickItem& rowPart(QQuickItem const& row, char const* name)
{
    auto* found = SidebarWindow::findNamed(row, QString::fromLatin1(name));
    REQUIRE(found != nullptr);
    return *found;
}

/// @return The vertical centre of @p part, in @p row's coordinates.
[[nodiscard]] qreal centreYIn(QQuickItem const& part, QQuickItem const& row)
{
    return part.mapToItem(&row, QPointF { 0, part.height() / 2 }).y();
}

/// Checks the Terminal row's tab-colour bar on @p row, a tab row whose tab has a colour: one glyph cell on
/// each of the row's two lines, each centred on the line of the tree's connectors it stands beside.
void checkCellBar(QQuickItem const& row)
{
    auto const cells = contour::test::itemsNamed(row, QStringLiteral("sidebarRowTabBarCell"));
    REQUIRE(cells.size() == 2);
    CHECK(centreYIn(*cells[0], row)
          == Catch::Approx(centreYIn(rowPart(row, "sidebarRowTree"), row)).margin(0.5));
    CHECK(centreYIn(*cells[1], row)
          == Catch::Approx(centreYIn(rowPart(row, "sidebarRowTreeContinued"), row)).margin(0.5));
}

/// Checks that @p row wears no tab colour: no tint, no bar, and its name in the row's own ink.
void checkWearsNoTabColour(QQuickItem const& row)
{
    CHECK_FALSE(rowPart(row, "sidebarRowTabFill").isVisible());
    CHECK_FALSE(rowPart(row, "sidebarRowTabBar").isVisible());
    CHECK(rgbaOf(rowPart(row, "sidebarRowTitle").property("color")) == rgbaOf(row.property("ink")));
}

} // namespace

TEST_CASE("a coloured tab's rows show its colour as sidebar.tab_color says (offscreen)",
          "[contour][gui][qml][sidebar]")
{
    using contour::config::SidebarTabColorStyle;
    using contour::window::RowSelection;
    using contour::window::SidebarRowKind;
    auto const orange = QColor { QStringLiteral("#E8853A") };
    auto const blue = QColor { QStringLiteral("#4C5FD5") };

    for (auto const row: drawnRows())
        for (auto const style:
             { SidebarTabColorStyle::Tint, SidebarTabColorStyle::Bar, SidebarTabColorStyle::Name })
        {
            INFO("row " << static_cast<int>(row) << ", style " << static_cast<int>(style));
            auto scene = SidebarWindow { { .row = row,
                                           .claudeTab = ClaudeTab::Split,
                                           .tabColours = TabColours::Coloured,
                                           .tabColorStyle = style } };
            // The pointer off the sidebar, so no row is hovered.
            QTest::mouseMove(scene.window, QPoint { scene.window->width() - 5, scene.window->height() - 5 });
            QCoreApplication::processEvents();

            auto colouredRows = 0;
            for (auto const* item: scene.rows())
            {
                INFO("title " << item->property("title").toString().toStdString());
                auto const kind = static_cast<SidebarRowKind>(item->property("kind").toInt());
                auto const tabColour = item->property("tabColor").value<QColor>();
                if (tabColour.alpha() == 0)
                {
                    checkWearsNoTabColour(*item);
                    continue;
                }
                REQUIRE(kind != SidebarRowKind::Workspace);
                ++colouredRows;
                // build's tab row is orange; claude's tab row and both its pane rows are blue.
                CHECK(tabColour.rgba()
                      == (item->property("title") == QStringLiteral("build") ? orange : blue).rgba());

                auto const& fill = rowPart(*item, "sidebarRowTabFill");
                auto const& bar = rowPart(*item, "sidebarRowTabBar");
                auto const& selection = rowPart(*item, "sidebarRowSelection");
                auto const titleInk = rgbaOf(rowPart(*item, "sidebarRowTitle").property("color"));
                auto const marked =
                    item->property("selection").toInt() != static_cast<int>(RowSelection::None);
                auto const paint = tabPaintOf(scene, *item);
                switch (style)
                {
                    case SidebarTabColorStyle::Tint:
                        CHECK(fill.isVisible());
                        CHECK(rgbaOf(fill.property("color")) == rgbaOf(paint.value(QStringLiteral("fill"))));
                        CHECK(titleInk == rgbaOf(paint.value(QStringLiteral("ink"))));
                        CHECK_FALSE(selection.isVisible());
                        CHECK_FALSE(bar.isVisible());
                        break;
                    case SidebarTabColorStyle::Bar:
                        CHECK(bar.isVisible());
                        CHECK(bar.width() > 0);
                        CHECK(bar.width() == scene.token("tabColorBarWidth"));
                        CHECK(bar.height() == item->height());
                        CHECK(rgbaOf(bar.property("color")) == tabColour.rgba());
                        if (row == ChromeStyle::Terminal)
                            checkCellBar(*item);
                        CHECK_FALSE(fill.isVisible());
                        CHECK(selection.isVisible() == marked);
                        CHECK(titleInk == rgbaOf(item->property("ink")));
                        break;
                    case SidebarTabColorStyle::Name:
                        CHECK(titleInk == tabColour.rgba());
                        CHECK_FALSE(fill.isVisible());
                        CHECK_FALSE(bar.isVisible());
                        CHECK(selection.isVisible() == marked);
                        break;
                }
            }
            CHECK(colouredRows == 4);

            // Folded, billing stands in for its selected, coloured tab, and still wears no tab colour.
            auto* billing = scene.rowTitled(QStringLiteral("billing"));
            REQUIRE(billing != nullptr);
            scene.controller.sidebarModel().collapse(billing->property("index").toInt());
            QCoreApplication::processEvents();
            billing = scene.rowTitled(QStringLiteral("billing"));
            REQUIRE(billing != nullptr);
            REQUIRE(billing->property("selection").toInt()
                    == static_cast<int>(RowSelection::ContainsSelected));
            checkWearsNoTabColour(*billing);
        }
}
TEST_CASE("a tinted row shows a press apart from a hover (offscreen)", "[contour][gui][qml][sidebar]")
{
    for (auto const row: drawnRows())
    {
        INFO("row " << static_cast<int>(row));
        auto scene = SidebarWindow { { .row = row,
                                       .claudeTab = ClaudeTab::Split,
                                       .tabColours = TabColours::Coloured,
                                       .tabColorStyle = contour::config::SidebarTabColorStyle::Tint } };
        // build: coloured, and not the shown tab, so the pointer is what fades its tint.
        auto* build = scene.rowTitled(QStringLiteral("build"));
        REQUIRE(build != nullptr);
        REQUIRE(build->property("selection").toInt()
                == static_cast<int>(contour::window::RowSelection::None));
        auto const& fill = rowPart(*build, "sidebarRowTabFill");
        auto const fillColour = [&] {
            return rgbaOf(fill.property("color"));
        };

        scene.hover(*build);
        REQUIRE(build->property("pointer").toInt()
                == static_cast<int>(contour::window::RowPointer::Hovering));
        auto const hovered = fillColour();
        CHECK(hovered == rgbaOf(tabPaintOf(scene, *build).value(QStringLiteral("fill"))));

        auto const centre = build->mapToScene(QPointF { build->width() / 2, build->height() / 2 }).toPoint();
        QTest::mousePress(scene.window, Qt::LeftButton, Qt::NoModifier, centre);
        QCoreApplication::processEvents();
        CHECK(build->property("pointer").toInt() == static_cast<int>(contour::window::RowPointer::Pressing));
        CHECK(fill.isVisible());
        CHECK(fillColour() != hovered);
        CHECK(fillColour() == rgbaOf(tabPaintOf(scene, *build).value(QStringLiteral("fill"))));
        QTest::mouseRelease(scene.window, Qt::LeftButton, Qt::NoModifier, centre);
        QCoreApplication::processEvents();
    }
}

TEST_CASE("a status mark stays legible on its row's tint, even in the tab's own colour (offscreen)",
          "[contour][gui][qml][sidebar]")
{
    for (auto const row: drawnRows())
    {
        INFO("row " << static_cast<int>(row));
        // claude's permission mark, as it is drawn on an uncoloured row.
        auto const markColour = [&] {
            auto plain = SidebarWindow { { .row = row } };
            auto* claude = plain.rowTitled(QStringLiteral("claude"));
            REQUIRE(claude != nullptr);
            return rowPart(*claude, "sidebarRowGlyph").property("ink").value<QColor>();
        }();
        REQUIRE(markColour.alpha() > 0);

        // claude's tab coloured exactly as its mark: the selected row's tint is the mark's own colour.
        auto scene = SidebarWindow { { .row = row,
                                       .tabColours = TabColours::Coloured,
                                       .claudeTabColour =
                                           vtbackend::RGBColor { static_cast<uint8_t>(markColour.red()),
                                                                 static_cast<uint8_t>(markColour.green()),
                                                                 static_cast<uint8_t>(markColour.blue()) },
                                       .tabColorStyle = contour::config::SidebarTabColorStyle::Tint } };
        auto* claude = scene.rowTitled(QStringLiteral("claude"));
        REQUIRE(claude != nullptr);
        REQUIRE(claude->property("selection").toInt()
                == static_cast<int>(contour::window::RowSelection::Selected));
        REQUIRE(claude->property("tabColor").value<QColor>().rgb() == markColour.rgb());
        auto const& fill = rowPart(*claude, "sidebarRowTabFill");
        REQUIRE(fill.isVisible());
        auto const fillColour = fill.property("color").value<QColor>();
        auto const drawn = rowPart(*claude, "sidebarRowGlyph").property("ink").value<QColor>();
        CHECK(drawn.rgb() != fillColour.rgb());
        auto const rgbOf = [](QColor const& colour) {
            return vtbackend::RGBColor { static_cast<uint8_t>(colour.red()),
                                         static_cast<uint8_t>(colour.green()),
                                         static_cast<uint8_t>(colour.blue()) };
        };
        CHECK(vtbackend::contrastRatio(rgbOf(drawn), rgbOf(fillColour)) >= vtbackend::MinimumMarkContrast);
    }
}
// }}}

// {{{ Workspace colour
namespace
{

/// contour's colour in the workspace-colour cases.
constexpr auto ContourColour = vtbackend::RGBColor { 0xC2, 0x54, 0x8F };

/// The scene the workspace-colour cases share: contour coloured #C2548F, its claude tab coloured (and split)
/// and its build tab not, and docs uncoloured.
[[nodiscard]] SidebarScene workspaceColourScene(
    ChromeStyle row,
    contour::config::SidebarWorkspaceColorStyle style,
    contour::config::SidebarTabColorStyle tabStyle = contour::config::SidebarTabColorStyle::Tint)
{
    return SidebarScene { .row = row,
                          .claudeTab = ClaudeTab::Split,
                          .billingTitle = "contour",
                          .tabColours = TabColours::ClaudeOnly,
                          .tabColorStyle = tabStyle,
                          .billingColour = ContourColour,
                          .workspaceColorStyle = style };
}

/// What `chromeStyle.sidebarWorkspaceColor` paints on @p row as it stands: under the pointer as it is, in its
/// window as active as the window the test drives really is.
[[nodiscard]] QVariantMap workspacePaintOf(SidebarWindow const& scene, QQuickItem const& row)
{
    auto* style =
        scene.engine.rootContext()->contextProperty(QStringLiteral("chromeStyle")).value<QObject*>();
    REQUIRE(style != nullptr);
    auto const* colors = style->property("colors").value<QObject*>();
    REQUIRE(colors != nullptr);
    auto paint = QVariantMap {};
    REQUIRE(QMetaObject::invokeMethod(style,
                                      "sidebarWorkspaceColor",
                                      Q_RETURN_ARG(QVariantMap, paint),
                                      Q_ARG(int, scene.controller.sidebarWorkspaceColorStyle()),
                                      Q_ARG(int, row.property("kind").toInt()),
                                      Q_ARG(QColor, row.property("workspaceColor").value<QColor>()),
                                      Q_ARG(QColor, colors->property("sidebarFill").value<QColor>()),
                                      Q_ARG(int, row.property("selection").toInt()),
                                      Q_ARG(int, row.property("pointer").toInt()),
                                      Q_ARG(bool, scene.window->isActive())));
    return paint;
}

/// The four things a workspace's colour draws on a row.
constexpr auto WorkspacePaintParts = std::array { "sidebarRowWorkspaceFill",
                                                  "sidebarRowWorkspaceRail",
                                                  "sidebarRowWorkspaceWash",
                                                  "sidebarRowWorkspaceSwatch" };

/// Checks that @p row wears no workspace colour.
void checkWearsNoWorkspaceColour(QQuickItem const& row)
{
    for (auto const* part: WorkspacePaintParts)
    {
        INFO(part);
        CHECK_FALSE(rowPart(row, part).isVisible());
    }
}

/// Checks that @p row, contour's, wears its colour as @p style draws it there, folded or not.
void checkWorkspaceRowPaint(QQuickItem const& row, contour::config::SidebarWorkspaceColorStyle style)
{
    using contour::config::SidebarWorkspaceColorStyle;
    CHECK(rowPart(row, "sidebarRowWorkspaceFill").isVisible() == (style == SidebarWorkspaceColorStyle::Tint));
    CHECK(rowPart(row, "sidebarRowWorkspaceRail").isVisible()
          == (style == SidebarWorkspaceColorStyle::Group));
    CHECK(rowPart(row, "sidebarRowWorkspaceWash").isVisible()
          == (style == SidebarWorkspaceColorStyle::Group));
    CHECK(rowPart(row, "sidebarRowWorkspaceSwatch").isVisible()
          == (style == SidebarWorkspaceColorStyle::Swatch));
}

/// Checks the Terminal row's group rail on @p row, a row of contour's: the cells the rail draws, on each of
/// the row's lines, are the model's, never blank, inked in @p colour, and stand in the tree's own cells, on
/// the same line as the connectors they continue. A workspace row's cell is its disclosure where it folds.
void checkCellRail(QQuickItem const& row, QQuickItem const& rail, QColor const& colour)
{
    using contour::window::RowExpansion;
    auto const& cell = rowPart(row, "sidebarRowWorkspaceRailCell");
    auto const& continued = rowPart(row, "sidebarRowWorkspaceRailCellContinued");
    CHECK(cell.property("text").toString() == row.property("railText").toString());
    if (static_cast<contour::window::SidebarRowKind>(row.property("kind").toInt())
        == contour::window::SidebarRowKind::Workspace)
    {
        CHECK_FALSE(continued.isVisible());
        auto const folds = row.property("expansion").toInt() != static_cast<int>(RowExpansion::None);
        auto const& disclosure = rowPart(row, "sidebarRowDisclosureGlyph");
        auto const& drawn = folds ? disclosure : cell;
        INFO("the workspace's cell: " << drawn.property("text").toString().toStdString());
        CHECK_FALSE(drawn.property("text").toString().trimmed().isEmpty());
        CHECK(rgbaOf(drawn.property("color")) == colour.rgba());
        CHECK(drawn.mapToItem(&row, QPointF {}).x() == Catch::Approx(rail.x()).margin(0.5));
        return;
    }
    // Two lines, and on each a connector in the colour where the tree has one, or a blank.
    REQUIRE(cell.isVisible());
    REQUIRE(continued.isVisible());
    CHECK(continued.property("text").toString() == row.property("railTextContinued").toString());
    for (auto const* drawn: { &cell, &continued })
    {
        INFO("line " << (drawn == &cell ? 1 : 2) << ": '" << drawn->property("text").toString().toStdString()
                     << "'");
        CHECK_FALSE(drawn->property("text").toString().trimmed().isEmpty());
        CHECK(rgbaOf(drawn->property("color")) == colour.rgba());
    }
    // In the tree's own lines: the rail's cell on the connectors' line, the rest of them past its cell, and
    // the second line's connectors under the first line's, with no gap after the rail.
    auto const& tree = rowPart(row, "sidebarRowTree");
    auto const& treeContinued = rowPart(row, "sidebarRowTreeContinued");
    CHECK(centreYIn(cell, row) == Catch::Approx(centreYIn(tree, row)).margin(0.5));
    CHECK(centreYIn(continued, row) == Catch::Approx(centreYIn(treeContinued, row)).margin(0.5));
    CHECK(tree.mapToItem(&row, QPointF {}).x() >= rail.x() + rail.width() - 0.5);
    CHECK(treeContinued.mapToItem(&row, QPointF {}).x()
          == Catch::Approx(tree.mapToItem(&row, QPointF {}).x()).margin(0.5));
}

/// @return @p colour as a vtbackend colour, its alpha dropped.
[[nodiscard]] vtbackend::RGBColor rgbOf(QColor const& colour)
{
    return vtbackend::RGBColor { static_cast<uint8_t>(colour.red()),
                                 static_cast<uint8_t>(colour.green()),
                                 static_cast<uint8_t>(colour.blue()) };
}

} // namespace

TEST_CASE("a coloured workspace's rows show its colour as sidebar.workspace_color says (offscreen)",
          "[contour][gui][qml][sidebar]")
{
    using contour::config::SidebarWorkspaceColorStyle;
    using contour::window::SidebarRowKind;
    auto const magenta = QColor { QStringLiteral("#C2548F") };

    for (auto const row: drawnRows())
        for (auto const style: { SidebarWorkspaceColorStyle::Tint,
                                 SidebarWorkspaceColorStyle::Group,
                                 SidebarWorkspaceColorStyle::Swatch })
        {
            INFO("row " << static_cast<int>(row) << ", style " << static_cast<int>(style));
            auto scene = SidebarWindow { workspaceColourScene(row, style) };
            QTest::mouseMove(scene.window, QPoint { scene.window->width() - 5, scene.window->height() - 5 });
            QCoreApplication::processEvents();

            auto colouredRows = 0;
            auto washes = std::vector<QRectF> {};
            for (auto const* item: scene.rows())
            {
                INFO("title " << item->property("title").toString().toStdString());
                auto const kind = static_cast<SidebarRowKind>(item->property("kind").toInt());
                auto const workspaceColour = item->property("workspaceColor").value<QColor>();
                if (workspaceColour.alpha() == 0)
                {
                    // docs and its tab: none of the four, in any style.
                    checkWearsNoWorkspaceColour(*item);
                    continue;
                }
                ++colouredRows;
                CHECK(workspaceColour.rgba() == magenta.rgba());
                auto const isWorkspace = kind == SidebarRowKind::Workspace;
                auto const& fill = rowPart(*item, "sidebarRowWorkspaceFill");
                auto const& rail = rowPart(*item, "sidebarRowWorkspaceRail");
                auto const& wash = rowPart(*item, "sidebarRowWorkspaceWash");
                auto const& swatch = rowPart(*item, "sidebarRowWorkspaceSwatch");
                auto const paint = workspacePaintOf(scene, *item);
                switch (style)
                {
                    case SidebarWorkspaceColorStyle::Tint:
                        CHECK(fill.isVisible() == isWorkspace);
                        if (isWorkspace)
                        {
                            CHECK(rgbaOf(fill.property("color"))
                                  == rgbaOf(paint.value(QStringLiteral("fill"))));
                            CHECK_FALSE(rowPart(*item, "sidebarRowSelection").isVisible());
                            CHECK_FALSE(rowPart(*item, "sidebarRowFeedback").isVisible());
                            CHECK(rgbaOf(rowPart(*item, "sidebarRowTitle").property("color"))
                                  == rgbaOf(paint.value(QStringLiteral("ink"))));
                        }
                        CHECK_FALSE(rail.isVisible());
                        CHECK_FALSE(wash.isVisible());
                        CHECK_FALSE(swatch.isVisible());
                        break;
                    case SidebarWorkspaceColorStyle::Group:
                        CHECK(rail.isVisible());
                        CHECK(wash.isVisible());
                        CHECK(rail.width() > 0);
                        CHECK(rail.width() == scene.token("workspaceRailWidth"));
                        CHECK(rail.x() == 0);
                        CHECK(rgbaOf(rail.property("color")) == magenta.rgba());
                        CHECK(rgbaOf(wash.property("color")) == rgbaOf(paint.value(QStringLiteral("wash"))));
                        // The wash is the row's whole box, square: one band down the workspace's rows.
                        CHECK(wash.mapToItem(item, QPointF {}).x() == 0);
                        CHECK(wash.mapToItem(item, QPointF {}).y() == 0);
                        CHECK(wash.width() == item->width());
                        CHECK(wash.height() == item->height());
                        CHECK(wash.property("radius").toReal() == 0);
                        washes.push_back(wash.mapRectToScene(wash.boundingRect()));
                        if (row != ChromeStyle::Terminal)
                            CHECK(rail.height() == item->height());
                        else
                            checkCellRail(*item, rail, magenta);
                        // A tab's own tint is drawn over the wash.
                        if (item->property("title") == QStringLiteral("claude") && !isWorkspace)
                            CHECK(rowPart(*item, "sidebarRowTabFill").isVisible());
                        CHECK_FALSE(fill.isVisible());
                        CHECK_FALSE(swatch.isVisible());
                        break;
                    case SidebarWorkspaceColorStyle::Swatch:
                        CHECK(swatch.isVisible() == isWorkspace);
                        if (isWorkspace)
                        {
                            CHECK(swatch.width() > 0);
                            CHECK(rgbaOf(swatch.property("color")) == magenta.rgba());
                            // On the Terminal row the glyph takes whole cells, at least the one the token
                            // names and as many as the face draws it across, centred in them, and so never
                            // spills into the name's.
                            if (row == ChromeStyle::Terminal)
                            {
                                auto const& glyph = rowPart(*item, "sidebarRowWorkspaceSwatchGlyph");
                                auto const cell = scene.style("cellWidth");
                                auto const drawn = cell * std::ceil(glyph.implicitWidth() / cell);
                                CHECK(swatch.width()
                                      == Catch::Approx(std::max(scene.token("workspaceSwatchSize"), drawn)));
                                CHECK(glyph.width() == Catch::Approx(swatch.width()));
                                CHECK(glyph.implicitWidth() <= swatch.width());
                                CHECK(glyph.property("horizontalAlignment").toInt() == Qt::AlignHCenter);
                            }
                            else
                                CHECK(swatch.width() == scene.token("workspaceSwatchSize"));
                            // Before the name.
                            CHECK(swatch.mapToItem(item, QPointF {}).x()
                                  < rowPart(*item, "sidebarRowTitle").mapToItem(item, QPointF {}).x());
                        }
                        CHECK_FALSE(fill.isVisible());
                        CHECK_FALSE(rail.isVisible());
                        CHECK_FALSE(wash.isVisible());
                        break;
                }
            }
            // contour, claude, its two panes, and build.
            CHECK(colouredRows == 5);
            // No gap between one coloured row's wash and the next's.
            std::ranges::sort(washes, {}, &QRectF::top);
            for (auto const& [above, below]: std::views::zip(washes, washes | std::views::drop(1)))
                CHECK(below.top() == Catch::Approx(above.bottom()));

            // Folded, contour keeps its paint.
            auto* contourRow = scene.rowTitled(QStringLiteral("contour"));
            REQUIRE(contourRow != nullptr);
            scene.controller.sidebarModel().collapse(contourRow->property("index").toInt());
            QCoreApplication::processEvents();
            contourRow = scene.rowTitled(QStringLiteral("contour"));
            REQUIRE(contourRow != nullptr);
            checkWorkspaceRowPaint(*contourRow, style);
        }
}

TEST_CASE("a tinted workspace row shows a press apart from a hover (offscreen)",
          "[contour][gui][qml][sidebar]")
{
    for (auto const row: drawnRows())
    {
        INFO("row " << static_cast<int>(row));
        auto scene =
            SidebarWindow { workspaceColourScene(row, contour::config::SidebarWorkspaceColorStyle::Tint) };
        auto* contourRow = scene.rowTitled(QStringLiteral("contour"));
        REQUIRE(contourRow != nullptr);
        REQUIRE(contourRow->property("selection").toInt()
                == static_cast<int>(contour::window::RowSelection::None));
        auto const& fill = rowPart(*contourRow, "sidebarRowWorkspaceFill");
        auto const fillColour = [&] {
            return rgbaOf(fill.property("color"));
        };

        scene.hover(*contourRow);
        REQUIRE(contourRow->property("pointer").toInt()
                == static_cast<int>(contour::window::RowPointer::Hovering));
        auto const hovered = fillColour();
        CHECK(hovered == rgbaOf(workspacePaintOf(scene, *contourRow).value(QStringLiteral("fill"))));

        // Off the disclosure, which takes the press itself.
        auto const at =
            contourRow->mapToScene(QPointF { contourRow->width() * 3 / 4, contourRow->height() / 2 })
                .toPoint();
        QTest::mouseMove(scene.window, at);
        QTest::mousePress(scene.window, Qt::LeftButton, Qt::NoModifier, at);
        QCoreApplication::processEvents();
        CHECK(contourRow->property("pointer").toInt()
              == static_cast<int>(contour::window::RowPointer::Pressing));
        CHECK(fill.isVisible());
        CHECK(fillColour() != hovered);
        CHECK(fillColour() == rgbaOf(workspacePaintOf(scene, *contourRow).value(QStringLiteral("fill"))));
        QTest::mouseRelease(scene.window, Qt::LeftButton, Qt::NoModifier, at);
        QCoreApplication::processEvents();
    }
}

TEST_CASE("a workspace's rail and a tab's bar stand side by side (offscreen)", "[contour][gui][qml][sidebar]")
{
    for (auto const row: drawnRows())
    {
        INFO("row " << static_cast<int>(row));
        auto scene = SidebarWindow { workspaceColourScene(row,
                                                          contour::config::SidebarWorkspaceColorStyle::Group,
                                                          contour::config::SidebarTabColorStyle::Bar) };
        auto* claude = scene.rowTitled(QStringLiteral("claude"));
        REQUIRE(claude != nullptr);
        auto const& rail = rowPart(*claude, "sidebarRowWorkspaceRail");
        auto const& tabBar = rowPart(*claude, "sidebarRowTabBar");
        CHECK(rail.isVisible());
        CHECK(tabBar.isVisible());
        INFO("bar at " << tabBar.x() << ", rail at " << rail.x() << " + " << rail.width());
        CHECK(tabBar.x() >= rail.x() + rail.width());
        if (row == ChromeStyle::Terminal)
            checkCellBar(*claude);
    }
}

TEST_CASE("a status mark stays legible on a tinted workspace row (offscreen)", "[contour][gui][qml][sidebar]")
{
    for (auto const row: drawnRows())
    {
        INFO("row " << static_cast<int>(row));
        // The workspace's roll-up mark, as it is drawn on an uncoloured row.
        auto const markColour = [&] {
            auto plain = SidebarWindow { { .row = row } };
            auto* billing = plain.rowTitled(QStringLiteral("billing"));
            REQUIRE(billing != nullptr);
            return rowPart(*billing, "sidebarRowRollUp").property("ink").value<QColor>();
        }();
        REQUIRE(markColour.alpha() > 0);

        // The workspace coloured exactly as its mark, and folded so its row stands for the selected tab.
        auto setup = workspaceColourScene(row, contour::config::SidebarWorkspaceColorStyle::Tint);
        setup.billingColour = rgbOf(markColour);
        auto scene = SidebarWindow { setup };
        auto* contourRow = scene.rowTitled(QStringLiteral("contour"));
        REQUIRE(contourRow != nullptr);
        scene.controller.sidebarModel().collapse(contourRow->property("index").toInt());
        QCoreApplication::processEvents();
        contourRow = scene.rowTitled(QStringLiteral("contour"));
        REQUIRE(contourRow != nullptr);
        auto const& fill = rowPart(*contourRow, "sidebarRowWorkspaceFill");
        REQUIRE(fill.isVisible());
        auto const fillColour = fill.property("color").value<QColor>();
        auto const drawn = rowPart(*contourRow, "sidebarRowRollUp").property("ink").value<QColor>();
        CHECK(vtbackend::contrastRatio(rgbOf(drawn), rgbOf(fillColour)) >= vtbackend::MinimumMarkContrast);
    }
}

TEST_CASE("the sidebar's colour picker opens at the workspace's row and colours it by id (offscreen)",
          "[contour][gui][qml][sidebar]")
{
    for (auto const row: drawnRows())
    {
        INFO("row " << static_cast<int>(row));
        auto scene =
            SidebarWindow { workspaceColourScene(row, contour::config::SidebarWorkspaceColorStyle::Tint) };
        auto* flyout = scene.window->findChild<QObject*>(QStringLiteral("sidebarWorkspaceColorFlyout"));
        REQUIRE(flyout != nullptr);
        REQUIRE_FALSE(flyout->property("opened").toBool());

        // docs, the workspace the window does not show.
        emit scene.controller.workspaceColorPickRequested(2);
        REQUIRE(QTest::qWaitFor([&] { return flyout->property("opened").toBool(); }, 2000));
        auto* docs = scene.rowTitled(QStringLiteral("docs"));
        REQUIRE(docs != nullptr);
        CHECK(flyout->property("parent").value<QQuickItem*>() == docs);

        auto const picked = QColor { 0x12, 0x34, 0x56 };
        QMetaObject::invokeMethod(flyout, "colorPicked", Q_ARG(QColor, picked));
        REQUIRE(scene.controller.workspaceColorCalls.size() == 1);
        CHECK(scene.controller.workspaceColorCalls[0].workspace == 2);
        CHECK(scene.controller.workspaceColorCalls[0].color == picked);

        QMetaObject::invokeMethod(flyout, "resetRequested");
        REQUIRE(scene.controller.workspaceColorCalls.size() == 2);
        CHECK(scene.controller.workspaceColorCalls[1].workspace == 2);
        CHECK_FALSE(scene.controller.workspaceColorCalls[1].color.has_value());
    }
}

TEST_CASE("the sidebar's tab colour picker opens at the tab's row and colours it by id (offscreen)",
          "[contour][gui][qml][sidebar][tabcolor]")
{
    for (auto const row: drawnRows())
    {
        INFO("row " << static_cast<int>(row));
        auto scene = SidebarWindow { { .row = row, .tabColours = TabColours::Coloured } };
        auto* flyout = scene.window->findChild<QObject*>(QStringLiteral("sidebarTabColorFlyout"));
        REQUIRE(flyout != nullptr);
        REQUIRE_FALSE(flyout->property("opened").toBool());

        // build, a tab that is not the current one.
        auto* build = scene.rowTitled(QStringLiteral("build"));
        REQUIRE(build != nullptr);
        constexpr auto BuildTab = quint64 { 11 };
        emit scene.controller.sidebarTabColorPickRequested(build->property("index").toInt(), BuildTab);
        REQUIRE(QTest::qWaitFor([&] { return flyout->property("opened").toBool(); }, 2000));
        build = scene.rowTitled(QStringLiteral("build"));
        CHECK(flyout->property("parent").value<QQuickItem*>() == build);
        // Its keyboard cursor starts on the colour the tab has.
        CHECK(flyout->property("currentColor").value<QColor>()
              == QColor { BuildTabColour.red, BuildTabColour.green, BuildTabColour.blue });

        auto const picked = QColor { 0x12, 0x34, 0x56 };
        QMetaObject::invokeMethod(flyout, "colorPicked", Q_ARG(QColor, picked));
        REQUIRE(scene.controller.tabColorCalls.size() == 1);
        CHECK(scene.controller.tabColorCalls[0].tab == BuildTab);
        CHECK(scene.controller.tabColorCalls[0].color == picked);

        QMetaObject::invokeMethod(flyout, "resetRequested");
        REQUIRE(scene.controller.tabColorCalls.size() == 2);
        CHECK(scene.controller.tabColorCalls[1].tab == BuildTab);
        CHECK_FALSE(scene.controller.tabColorCalls[1].color.has_value());
        CHECK(scene.controller.workspaceColorCalls.empty());
    }
}
// }}}

// {{{ Daemon rows

namespace
{
/// The daemon row titled @p title's heading, or null.
[[nodiscard]] QQuickItem* daemonHeadingOf(SidebarWindow const& scene, QString const& title)
{
    auto* row = scene.rowTitled(title);
    return row != nullptr ? SidebarWindow::findNamed(*row, QStringLiteral("sidebarDaemonHeading")) : nullptr;
}

/// The part named @p name of @p heading.
[[nodiscard]] QQuickItem& headingPart(QQuickItem const& heading, char const* name)
{
    auto* found = SidebarWindow::findNamed(heading, QString::fromLatin1(name));
    REQUIRE(found != nullptr);
    return *found;
}

/// The buttons @p heading shows, leading first.
[[nodiscard]] std::vector<QQuickItem*> daemonButtonsOf(QQuickItem const& heading)
{
    auto buttons = contour::test::itemsNamed(heading, QStringLiteral("sidebarDaemonButton"));
    std::erase_if(buttons, [](QQuickItem const* button) { return !button->isVisible(); });
    std::ranges::sort(buttons, {}, [](QQuickItem const* button) { return sceneRectOf(*button).left(); });
    return buttons;
}

/// starfive in @p state, its one workspace listed as last known.
[[nodiscard]] contour::window::DaemonSection starfiveIn(contour::window::DaemonLinkState state,
                                                        std::string name = "starfive")
{
    auto section = starfiveSection();
    section.state = state;
    section.name = std::move(name);
    return section;
}

/// A daemon of this machine named @p name in @p state, with one workspace: Start and Remove are its verbs.
[[nodiscard]] contour::window::DaemonSection workIn(contour::window::DaemonLinkState state,
                                                    std::string name = "work")
{
    auto section = contour::window::DaemonSection {};
    section.daemon = contour::window::localDaemonRef(name);
    section.instance = contour::window::DaemonInstance { 3 };
    section.name = std::move(name);
    section.locality = contour::window::DaemonLocality::Local;
    section.state = state;
    section.directory.workspaces = { contour::test::directoryWorkspaceOf(
        7, "api", { contour::test::directoryPaneOf(70, "server") }) };
    section.feed = contour::window::SectionFeed::Observer;
    return section;
}

/// The row index of the daemon row titled @p title in @p scene's model.
[[nodiscard]] int daemonRowIndex(SidebarWindow const& scene, std::string_view title)
{
    auto const& rows = scene.controller.sidebarModel().rows();
    auto const at = std::ranges::find_if(rows, [&](contour::window::SidebarRow const& row) {
        return row.key.kind == contour::window::SidebarRowKind::Daemon && row.title == title;
    });
    REQUIRE(at != rows.end());
    return static_cast<int>(std::distance(rows.begin(), at));
}

/// Whether @p part lies inside @p row, horizontally.
[[nodiscard]] bool insideRow(QQuickItem const& part, QQuickItem const& row)
{
    auto const rect = sceneRectOf(part);
    auto const bounds = sceneRectOf(row);
    return rect.left() >= bounds.left() - 0.5 && rect.right() <= bounds.right() + 0.5;
}

/// Types @p text into @p window, a key at a time: QTest::keyClicks() takes a widget only.
void typeInto(QQuickWindow& window, std::string_view text)
{
    for (auto const letter: text)
    {
        QTest::keyClick(&window, letter);
        QCoreApplication::processEvents();
    }
}

/// The look @p row draws its sidebar in.
[[nodiscard]] contour::config::SidebarLook lookOf(ChromeStyle row)
{
    return contour::config::resolveChromeTokens(row, {}).layout.sidebar;
}
} // namespace

TEST_CASE("a daemon row shows its name and link state (offscreen)",
          "[contour][gui][qml][sidebar][daemon-row]")
{
    using contour::window::DaemonLinkState;
    for (auto const row: drawnRows())
    {
        INFO("row " << static_cast<int>(row));
        auto scene = SidebarWindow { { .row = row } };
        // At its widest, so Remove, which a narrower row lets go after the locality icon, stands beside
        // Start.
        scene.controller.showSidebar(static_cast<int>(scene.token("maxWidth")));
        onDaemon(scene,
                 QStringLiteral("local"),
                 DaemonLinkState::Connected,
                 { workIn(DaemonLinkState::NotRunning), starfiveIn(DaemonLinkState::NotRunning) });

        auto* own = daemonHeadingOf(scene, QStringLiteral("local"));
        auto* other = daemonHeadingOf(scene, QStringLiteral("work"));
        auto* host = daemonHeadingOf(scene, QStringLiteral("starfive"));
        REQUIRE(own != nullptr);
        REQUIRE(other != nullptr);
        REQUIRE(host != nullptr);
        CHECK(headingPart(*other, "sidebarDaemonName").property("text").toString() == QStringLiteral("work"));
        // A host that is not running offers nothing to press: nothing here starts it.
        CHECK(daemonButtonsOf(*host).empty());

        // Not running: its mark from the link table, its words, and Start beside Remove.
        auto const look = contour::window::linkLookOf(DaemonLinkState::NotRunning, lookOf(row));
        auto& mark = headingPart(*other, "sidebarDaemonLinkMark");
        CHECK(mark.isVisible());
        CHECK(mark.property("text").toString() == QString::fromUtf8(look.glyph));
        auto& words = headingPart(*other, "sidebarDaemonLinkWords");
        CHECK(words.property("text").toString() == QStringLiteral("Not running"));
        auto const buttons = daemonButtonsOf(*other);
        REQUIRE(buttons.size() == 2);
        CHECK(buttons[0]->property("accessibleName").toString() == QStringLiteral("Start"));
        CHECK(buttons[1]->property("accessibleName").toString() == QStringLiteral("Remove"));

        // The window's own daemon is connected: no link mark, no button.
        CHECK_FALSE(headingPart(*own, "sidebarDaemonLinkMark").isVisible());
        CHECK(daemonButtonsOf(*own).empty());

        auto& name = headingPart(*own, "sidebarDaemonName");
        auto const* ownRow = scene.rowTitled(QStringLiteral("local"));
        REQUIRE(ownRow != nullptr);
        // The name in the look's section-heading type.
        auto* style =
            scene.engine.rootContext()->contextProperty(QStringLiteral("chromeStyle")).value<QObject*>();
        auto const sectionFont =
            style->property("type").value<QObject*>()->property("sidebarSection").value<QFont>();
        CHECK(name.property("font").value<QFont>() == sectionFont);
        auto const* workspace = scene.rowTitled(QStringLiteral("billing"));
        REQUIRE(workspace != nullptr);
        auto const& workspaceTitle = rowPart(*workspace, "sidebarRowTitle");
        if (row != ChromeStyle::Terminal)
        {
            // A source list's section header: the footnote's size, semibold, smaller than the workspaces it
            // heads, in the secondary ink; it ranks by being a header, not by its weight.
            auto const footnote =
                style->property("type").value<QObject*>()->property("footnote").value<QFont>();
            auto const drawn = name.property("font").value<QFont>();
            CHECK(drawn.pointSizeF() == Catch::Approx(footnote.pointSizeF()));
            CHECK(drawn.pixelSize() == footnote.pixelSize());
            CHECK(drawn.weight() == QFont::DemiBold);
            auto const workspaceFont = workspaceTitle.property("font").value<QFont>();
            CHECK(QFontMetricsF(drawn).height() < QFontMetricsF(workspaceFont).height());
        }
        if (row == ChromeStyle::Terminal)
        {
            // A section heading: bold at column 0, its disclosure after the name, no locality mark.
            CHECK_FALSE(headingPart(*other, "sidebarDaemonDisclosure").isVisible());
            CHECK_FALSE(headingPart(*other, "sidebarDaemonLocality").isVisible());
            CHECK(sceneRectOf(name).left() == Catch::Approx(sceneRectOf(*ownRow).left()).margin(0.5));
            CHECK(name.property("font").value<QFont>().bold());
            auto const& fold = headingPart(*own, "sidebarDaemonTrailingDisclosure");
            CHECK(fold.isVisible());
            CHECK(sceneRectOf(fold).left() >= sceneRectOf(name).right());
        }
        else
        {
            // A chevron as the workspaces', the name where their names stand, and the locality's icon after
            // it: the heading never reads as nested deeper than its workspaces.
            CHECK(headingPart(*other, "sidebarDaemonDisclosure").isVisible());
            auto const& locality = headingPart(*own, "sidebarDaemonLocality");
            CHECK(locality.isVisible());
            CHECK(sceneRectOf(locality).left() >= sceneRectOf(name).right());
            CHECK(sceneRectOf(name).left() <= sceneRectOf(workspaceTitle).left() + 0.5);
        }
        // A blank line, or a gap, sets the second section apart; the first starts at the top.
        auto const* first = scene.rows().front();
        auto const* second =
            daemonRowIndex(scene, "local") == 0 ? scene.rowTitled(QStringLiteral("starfive")) : ownRow;
        REQUIRE(first->property("kind").toInt() == static_cast<int>(contour::window::SidebarRowKind::Daemon));
        CHECK(second->height() > first->height());
        CHECK(scene.warnings.count(isSidebarDiagnostic) == 0);
    }
}

TEST_CASE("Connect, Start, Cancel and Remove call the model (offscreen)",
          "[contour][gui][qml][sidebar][daemon-row]")
{
    using contour::test::DaemonSectionsCall;
    using contour::window::DaemonLinkState;
    for (auto const row: drawnRows())
    {
        INFO("row " << static_cast<int>(row));
        auto scene = SidebarWindow { { .row = row } };
        // At its widest, so every offered button is on the row to press.
        scene.controller.showSidebar(static_cast<int>(scene.token("maxWidth")));
        auto const pressAllOf = [&](contour::window::DaemonSection const& section) {
            onDaemon(scene, QStringLiteral("local"), DaemonLinkState::Connected, { section });
            auto* heading = daemonHeadingOf(scene, QString::fromStdString(section.name));
            REQUIRE(heading != nullptr);
            for (auto* button: daemonButtonsOf(*heading))
                scene.click(*button);
        };
        auto const pressAll = [&](DaemonLinkState state) {
            pressAllOf(starfiveIn(state));
        };
        auto& calls = scene.controller.sidebarSections().calls;
        auto const starfive = contour::window::hostDaemonRef("starfive");
        auto const work = contour::window::localDaemonRef("work");

        pressAllOf(workIn(DaemonLinkState::NotRunning));
        REQUIRE(calls.size() == 2);
        CHECK(calls[0].verb == decltype(calls[0].verb) { DaemonSectionsCall::Start { .daemon = work } });
        CHECK(calls[1].verb == decltype(calls[1].verb) { DaemonSectionsCall::Remove { .daemon = work } });

        calls.clear();
        pressAll(DaemonLinkState::NotConnected);
        REQUIRE(calls.size() == 1);
        CHECK(std::holds_alternative<DaemonSectionsCall::Connect>(calls[0].verb));

        // Cancel on a connect that is under way ends it; on a pending jump into the section, the jump.
        calls.clear();
        pressAll(DaemonLinkState::Connecting);
        REQUIRE(calls.size() == 1);
        CHECK(calls[0].verb
              == decltype(calls[0].verb) { DaemonSectionsCall::Disconnect { .daemon = starfive } });

        calls.clear();
        scene.controller.sidebarHost().pending =
            contour::window::SidebarTarget { .daemon = starfive,
                                             .instance = contour::window::DaemonInstance { 9 },
                                             .workspace = 5,
                                             .tab = std::nullopt,
                                             .pane = {} };
        pressAll(DaemonLinkState::Connected);
        CHECK(calls.empty());
        CHECK(scene.controller.sidebarHost().cancels == 1);
        CHECK(scene.warnings.count(isSidebarDiagnostic) == 0);
    }
}

TEST_CASE("the daemon row fits at 160 px and 18 cells in the documented order (offscreen)",
          "[contour][gui][qml][sidebar][daemon-row]")
{
    skipWithoutPlatformUiFont();
    using contour::window::DaemonLinkState;
    using contour::window::sidebarrows::DaemonRowFit;
    for (auto const row: drawnRows())
    {
        INFO("row " << static_cast<int>(row));
        auto scene = SidebarWindow { { .row = row } };
        // A long name, the link's words and two buttons: everything that can give way.
        onDaemon(scene,
                 QStringLiteral("local"),
                 DaemonLinkState::Connected,
                 { workIn(DaemonLinkState::NotRunning, "build-farm-eu-west") });
        auto const fitNow = [&] {
            // Twice: the row's new width changes its fit, and the buttons' new widths lay out the next time.
            scene.layOut();
            scene.layOut();
            auto* heading = daemonHeadingOf(scene, QStringLiteral("build-farm-eu-west"));
            REQUIRE(heading != nullptr);
            auto const* rowItem = scene.rowTitled(QStringLiteral("build-farm-eu-west"));
            // Whatever gave way, every part stays inside the row and the name is not covered.
            for (auto* button: daemonButtonsOf(*heading))
            {
                CHECK(insideRow(*button, *rowItem));
                CHECK(sceneRectOf(headingPart(*heading, "sidebarDaemonName")).right()
                      <= sceneRectOf(*button).left() + 0.5);
            }
            CHECK(insideRow(headingPart(*heading, "sidebarDaemonLinkMark"), *rowItem));
            // What the fit gives the name is drawn whole: never cut further by the room left.
            auto const& name = headingPart(*heading, "sidebarDaemonName");
            CHECK(name.width() > 0);
            auto parts = std::format("heading {} name x {} w {} implicit {} fit {}",
                                     heading->width(),
                                     sceneRectOf(name).left(),
                                     name.width(),
                                     name.implicitWidth(),
                                     heading->property("fit").toInt());
            for (auto const* part: { "sidebarDaemonLocality", "sidebarDaemonLinkMark" })
                parts += std::format(" | {} x {} w {}",
                                     part,
                                     sceneRectOf(headingPart(*heading, part)).left(),
                                     headingPart(*heading, part).width());
            for (auto* button: daemonButtonsOf(*heading))
                parts += std::format(" | button x {} w {}", sceneRectOf(*button).left(), button->width());
            INFO(parts);
            CHECK_FALSE(name.property("truncated").toBool());
            auto const fit = heading->property("fit").toInt();
            // The buttons shrink first: the words stay beside the mark until they have.
            CHECK(headingPart(*heading, "sidebarDaemonLinkWords").isVisible()
                  == (fit < static_cast<int>(DaemonRowFit::NoWords)));
            // Once the words are gone they are the mark's tooltip.
            auto const* tip =
                SidebarWindow::findNamed(*heading, QStringLiteral("sidebarDaemonReasonToolTipHost"));
            REQUIRE(tip != nullptr);
            CHECK(tip->property("text").toString().contains(QStringLiteral("Not running"))
                  == (fit >= static_cast<int>(DaemonRowFit::NoWords)));
            // Remove is words wherever it stands.
            for (auto* button: daemonButtonsOf(*heading))
                if (button->property("accessibleName").toString() == QStringLiteral("Remove"))
                    CHECK(button->property("glyph").toString().contains(QStringLiteral("Remove")));
            return fit;
        };
        auto const roomy = fitNow();
        scene.controller.showSidebar(static_cast<int>(scene.token("minWidth")));
        auto const narrowest = fitNow();
        // At the narrowest the row gives way at least as far as at its default, and something has given.
        CHECK(narrowest >= roomy);
        CHECK(narrowest > static_cast<int>(DaemonRowFit::Full));
        CHECK(scene.warnings.count(isSidebarDiagnostic) == 0);
    }
}

TEST_CASE("at its default width a daemon row says its state in words (offscreen)",
          "[contour][gui][qml][sidebar][daemon-row]")
{
    skipWithoutPlatformUiFont();
    // Spec §2 and §5.1: "Not running · Start", "Connecting… · Cancel", "Needs you · Connect", and the longest
    // words, "Not connected" and "Reconnecting…", on the Terminal row's 30 cells too. The locality
    // icon and the secondary button give way, and the primary button shrinks to its icon, before the words
    // leave, so a first-time user reads what the mark means -- under an ordinary name, eight letters long.
    using contour::window::DaemonLinkState;
    using contour::window::sidebarrows::DaemonRowFit;
    for (auto const row: drawnRows())
        for (auto const state: { DaemonLinkState::NotConnected,
                                 DaemonLinkState::NotRunning,
                                 DaemonLinkState::Connecting,
                                 DaemonLinkState::Reconnecting,
                                 DaemonLinkState::NeedsInput,
                                 DaemonLinkState::Failed })
            // A daemon of this machine, so a NotRunning one offers Start and Remove; and a saved host, whose
            // row also stands its fold mark and locality beside the name.
            for (auto const& section: { workIn(state, "starfive"), starfiveIn(state) })
            {
                INFO("row " << static_cast<int>(row) << ", state " << static_cast<int>(state) << ", locality "
                            << static_cast<int>(section.locality));
                auto scene = SidebarWindow { { .row = row, .font = SceneFont::RowOwn } };
                onDaemon(scene, QStringLiteral("local"), DaemonLinkState::Connected, { section });
                scene.layOut();
                scene.layOut();
                auto* heading = daemonHeadingOf(scene, QStringLiteral("starfive"));
                REQUIRE(heading != nullptr);
                // The locality icon, Remove and the buttons' words give way first: the words stand beside the
                // mark on every look.
                CHECK(headingPart(*heading, "sidebarDaemonLinkWords").isVisible());
                CHECK(heading->property("fit").toInt() < static_cast<int>(DaemonRowFit::NoWords));
                CHECK_FALSE(headingPart(*heading, "sidebarDaemonName").property("truncated").toBool());
                // On the Terminal row the link's mark takes one cell, however wide its face draws it, a
                // button in words is bracketed, so two never read as one phrase, and one in its glyph form
                // takes one cell.
                if (row == ChromeStyle::Terminal)
                    CHECK(headingPart(*heading, "sidebarDaemonLinkMark").width() == scene.style("cellWidth"));
                if (row == ChromeStyle::Terminal)
                    for (auto* button: daemonButtonsOf(*heading))
                    {
                        if (!button->property("iconOnly").toBool())
                            CHECK(button->property("glyph").toString().startsWith(QLatin1Char('[')));
                        else
                            CHECK(button->width() == scene.style("cellWidth"));
                    }
                CHECK(scene.warnings.count(isSidebarDiagnostic) == 0);
            }
}

TEST_CASE("at its narrowest a daemon row keeps six letters of its name, in every state (offscreen)",
          "[contour][gui][qml][sidebar][daemon-row]")
{
    // At the 160 px / 18-cell minimum the primary button stays (as its icon, or its one-cell glyph on the
    // Terminal row), and the name is drawn whole at its shortest: at least six of its letters.
    using contour::window::DaemonLinkState;
    using contour::window::RowPending;
    for (auto const daemonName:
         { std::string_view { "build-farm-eu-west" }, std::string_view { "starfive" } })
        for (auto const row: drawnRows())
            for (auto const& [state, pending]: { std::pair { DaemonLinkState::NotRunning, RowPending::No },
                                                 std::pair { DaemonLinkState::Connecting, RowPending::No },
                                                 std::pair { DaemonLinkState::NeedsInput, RowPending::No },
                                                 std::pair { DaemonLinkState::Connecting, RowPending::Yes } })
            {
                INFO("name " << daemonName << ", row " << static_cast<int>(row) << ", state "
                             << static_cast<int>(state) << ", pending " << static_cast<int>(pending));
                auto scene = SidebarWindow { { .row = row } };
                auto const section = workIn(state, std::string(daemonName));
                if (pending == RowPending::Yes)
                    scene.controller.sidebarHost().pending =
                        contour::window::SidebarTarget { .daemon = section.daemon,
                                                         .instance = section.instance,
                                                         .workspace = 7,
                                                         .tab = std::nullopt,
                                                         .pane = {} };
                onDaemon(scene, QStringLiteral("local"), DaemonLinkState::Connected, { section });
                scene.controller.showSidebar(static_cast<int>(scene.token("minWidth")));
                scene.layOut();
                scene.layOut();
                auto* heading =
                    daemonHeadingOf(scene, QString::fromUtf8(daemonName.data(), daemonName.size()));
                REQUIRE(heading != nullptr);
                auto const& name = headingPart(*heading, "sidebarDaemonName");
                auto const text = name.property("text").toString();
                INFO("name " << text.toStdString() << ", fit " << heading->property("fit").toInt());
                CHECK_FALSE(name.property("truncated").toBool());
                CHECK(QString(text).remove(QStringLiteral("…")).size() >= 6);
                // The primary button is never dropped.
                auto const buttons = daemonButtonsOf(*heading);
                REQUIRE_FALSE(buttons.empty());
                for (auto* button: buttons)
                {
                    CHECK(sceneRectOf(name).right() <= sceneRectOf(*button).left() + 0.5);
                    // On the Terminal row the button in its glyph form takes one cell.
                    if (row == ChromeStyle::Terminal && button->property("iconOnly").toBool())
                        CHECK(button->width() == scene.style("cellWidth"));
                }
                CHECK(scene.warnings.count(isSidebarDiagnostic) == 0);
            }
}

TEST_CASE("Cancel's tooltip names CancelSwitch's chord only while a jump is under way (offscreen)",
          "[contour][gui][qml][sidebar][daemon-row]")
{
    // CancelSwitch ends a pending jump, not a connect: a Cancel that ends a connect names no chord.
    using contour::window::DaemonLinkState;
    for (auto const row: drawnRows())
    {
        INFO("row " << static_cast<int>(row));
        auto scene = SidebarWindow { { .row = row } };
        auto const cancelCommand = [&] {
            onDaemon(scene,
                     QStringLiteral("local"),
                     DaemonLinkState::Connected,
                     { starfiveIn(DaemonLinkState::Connecting) });
            auto* heading = daemonHeadingOf(scene, QStringLiteral("starfive"));
            REQUIRE(heading != nullptr);
            auto const buttons = daemonButtonsOf(*heading);
            REQUIRE(buttons.size() == 1);
            CHECK(buttons[0]->property("accessibleName").toString() == QStringLiteral("Cancel"));
            return buttons[0]->property("toolTipCommand").toString();
        };
        CHECK(cancelCommand().isEmpty());

        scene.controller.sidebarHost().pending =
            contour::window::SidebarTarget { .daemon = contour::window::hostDaemonRef("starfive"),
                                             .instance = contour::window::DaemonInstance { 9 },
                                             .workspace = 5,
                                             .tab = std::nullopt,
                                             .pane = {} };
        CHECK(cancelCommand() == QStringLiteral("CancelSwitch"));
        CHECK(scene.warnings.count(isSidebarDiagnostic) == 0);
    }
}

TEST_CASE("hovering a daemon row opens no tab hover card (offscreen)",
          "[contour][gui][qml][sidebar][daemon-row]")
{
    // The card is a tab's and a pane's; a daemon row's reason is its own tooltip, which the card would cover.
    auto scene = SidebarWindow { { .row = drawnRows().front() } };
    onDaemon(scene,
             QStringLiteral("local"),
             contour::window::DaemonLinkState::Connected,
             { starfiveIn(contour::window::DaemonLinkState::Failed) });
    auto* daemon = scene.rowTitled(QStringLiteral("starfive"));
    REQUIRE(daemon != nullptr);
    auto* card = scene.root->findChild<QObject*>(QStringLiteral("sidebarHoverCard"));
    REQUIRE(card != nullptr);
    auto const over = daemon->mapToScene(QPointF { daemon->width() / 4, daemon->height() / 2 }).toPoint();
    QTest::mouseMove(scene.window, over - QPoint { 0, 1 });
    QTest::mouseMove(scene.window, over);
    CHECK_FALSE(QTest::qWaitFor([&] { return card->property("opened").toBool(); }, 1500));
}

TEST_CASE("long daemon names elide in the middle (offscreen)", "[contour][gui][qml][sidebar][daemon-row]")
{
    using contour::window::DaemonLinkState;
    for (auto const row: drawnRows())
    {
        INFO("row " << static_cast<int>(row));
        auto scene = SidebarWindow { { .row = row } };
        onDaemon(scene,
                 QStringLiteral("local"),
                 DaemonLinkState::Connected,
                 { workIn(DaemonLinkState::NotRunning, "build-farm-eu-west-2-staging") });
        // At the narrowest, where no row has room for it whole: the name gives way last of all.
        scene.controller.showSidebar(static_cast<int>(scene.token("minWidth")));
        scene.layOut();
        scene.layOut();
        auto* heading = daemonHeadingOf(scene, QStringLiteral("build-farm-eu-west-2-staging"));
        REQUIRE(heading != nullptr);
        auto const& name = headingPart(*heading, "sidebarDaemonName");
        CHECK(heading->property("fit").toInt()
              >= static_cast<int>(contour::window::sidebarrows::DaemonRowFit::NameElided));
        auto const text = name.property("text").toString();
        INFO(text.toStdString());
        // Its ends kept, the middle given up.
        CHECK(text.startsWith(QLatin1Char('b')));
        CHECK(text.endsWith(QLatin1Char('g')));
        CHECK(text.contains(QStringLiteral("…")));
        CHECK(scene.warnings.count(isSidebarDiagnostic) == 0);
    }
}

TEST_CASE("the progress mark is static under reduced motion (offscreen)",
          "[contour][gui][qml][sidebar][daemon-row][motion]")
{
    using contour::config::MotionPreference;
    using contour::window::DaemonLinkState;
    for (auto const row: drawnRows())
        for (auto const motion: { MotionPreference::Full, MotionPreference::Reduced })
        {
            INFO("row " << static_cast<int>(row) << ", reduced " << (motion == MotionPreference::Reduced));
            auto scene = SidebarWindow { { .row = row, .motion = motion } };
            scene.controller.sidebarHost().pending =
                contour::window::SidebarTarget { .daemon = contour::window::hostDaemonRef("starfive"),
                                                 .instance = contour::window::DaemonInstance { 9 },
                                                 .workspace = 5,
                                                 .tab = std::nullopt,
                                                 .pane = {} };
            onDaemon(scene,
                     QStringLiteral("local"),
                     DaemonLinkState::Connected,
                     { starfiveIn(DaemonLinkState::Connecting) });
            auto* heading = daemonHeadingOf(scene, QStringLiteral("starfive"));
            REQUIRE(heading != nullptr);
            auto& mark = headingPart(*heading, "sidebarPendingMark");
            CHECK(mark.isVisible());
            CHECK(mark.property("text").toString()
                  == QString::fromUtf8(contour::window::daemonRowFace(lookOf(row)).pendingMark));
            CHECK(mark.property("pulsing").toBool() == (motion == MotionPreference::Full));
            CHECK(scene.warnings.count(isSidebarDiagnostic) == 0);
        }
}

TEST_CASE("filter Return jumps to the first match, Shift+Return opens it in a new window (offscreen)",
          "[contour][gui][qml][sidebar][daemon-row]")
{
    using contour::window::DaemonLinkState;
    using contour::window::JumpMode;
    for (auto const row: drawnRows())
    {
        INFO("row " << static_cast<int>(row));
        auto scene = SidebarWindow { { .row = row } };
        onDaemon(scene,
                 QStringLiteral("local"),
                 DaemonLinkState::Connected,
                 { starfiveIn(DaemonLinkState::Connected) });
        auto* field = scene.item(QStringLiteral("sidebarFilterField"));
        auto* content = scene.item(QStringLiteral("content"));
        REQUIRE(field != nullptr);
        REQUIRE(content != nullptr);
        auto const& jumps = scene.controller.sidebarHost().jumps;

        // Another daemon's workspace: Return jumps to it, and the keyboard stays where it was.
        scene.controller.focusSidebar();
        REQUIRE(QTest::qWaitFor([&] { return scene.item(QStringLiteral("sidebarTree"))->hasActiveFocus(); },
                                1000));
        typeInto(*scene.window, "ci");
        REQUIRE(QTest::qWaitFor([&] { return field->hasActiveFocus(); }, 1000));
        QTest::keyClick(scene.window, Qt::Key_Return);
        QCoreApplication::processEvents();
        REQUIRE(jumps.size() == 1);
        CHECK(jumps.back().first.daemon == contour::window::hostDaemonRef("starfive"));
        CHECK(jumps.back().second == JumpMode::Here);
        CHECK(field->hasActiveFocus());

        QTest::keyClick(scene.window, Qt::Key_Return, Qt::ShiftModifier);
        QCoreApplication::processEvents();
        REQUIRE(jumps.size() == 2);
        CHECK(jumps.back().second == JumpMode::NewWindow);

        // A row of the window's own is shown here, and the terminal takes the keyboard.
        QTest::keyClick(scene.window, 'a', Qt::ControlModifier);
        typeInto(*scene.window, "claude");
        QCoreApplication::processEvents();
        auto const shownBefore = scene.controller.sidebarHost().shown.size();
        QTest::keyClick(scene.window, Qt::Key_Return);
        QCoreApplication::processEvents();
        CHECK(scene.controller.sidebarHost().shown.size() == shownBefore + 1);
        CHECK(QTest::qWaitFor([&] { return isWithin(scene.window->activeFocusItem(), content); }, 1000));
        CHECK(scene.warnings.count(isSidebarDiagnostic) == 0);
    }
}

TEST_CASE("Esc in the sidebar cancels a pending jump, then clears the filter, then leaves (offscreen)",
          "[contour][gui][qml][sidebar][daemon-row]")
{
    for (auto const row: drawnRows())
    {
        INFO("row " << static_cast<int>(row));
        auto scene = SidebarWindow { { .row = row } };
        onDaemon(scene, QStringLiteral("local"));
        auto* tree = scene.item(QStringLiteral("sidebarTree"));
        auto* field = scene.item(QStringLiteral("sidebarFilterField"));
        REQUIRE(tree != nullptr);
        REQUIRE(field != nullptr);
        auto& host = scene.controller.sidebarHost();
        auto const& model = scene.controller.sidebarModel();
        auto const intoStarfive =
            contour::window::SidebarTarget { .daemon = contour::window::hostDaemonRef("starfive"),
                                             .instance = {},
                                             .workspace = 5,
                                             .tab = std::nullopt,
                                             .pane = {} };

        scene.controller.focusSidebar();
        REQUIRE(QTest::qWaitFor([&] { return tree->hasActiveFocus(); }, 1000));
        QTest::keyClick(scene.window, 'b');
        REQUIRE(QTest::qWaitFor([&] { return field->hasActiveFocus(); }, 1000));

        // In the filter field: the jump first, the filter after.
        host.pending = intoStarfive;
        QTest::keyClick(scene.window, Qt::Key_Escape);
        QCoreApplication::processEvents();
        CHECK(host.cancels == 1);
        CHECK(model.isFiltered());
        CHECK(field->hasActiveFocus());
        host.pending.reset();

        // In the tree: the same order, then the keyboard leaves.
        QTest::keyClick(scene.window, Qt::Key_Down);
        REQUIRE(QTest::qWaitFor([&] { return tree->hasActiveFocus(); }, 1000));
        host.pending = intoStarfive;
        QTest::keyClick(scene.window, Qt::Key_Escape);
        QCoreApplication::processEvents();
        CHECK(host.cancels == 2);
        CHECK(model.isFiltered());
        host.pending.reset();
        QTest::keyClick(scene.window, Qt::Key_Escape);
        QCoreApplication::processEvents();
        CHECK_FALSE(model.isFiltered());
        CHECK(scene.controller.sidebarHasFocus());
        QTest::keyClick(scene.window, Qt::Key_Escape);
        QCoreApplication::processEvents();
        CHECK_FALSE(scene.controller.sidebarHasFocus());
        CHECK(host.cancels == 2);
    }
}

TEST_CASE(
    "Left and Right fold a daemon row, Left from a workspace reaches its daemon row, Home and End cross "
    "daemons (offscreen)",
    "[contour][gui][qml][sidebar][daemon-row]")
{
    using contour::window::RowExpansion;
    for (auto const row: drawnRows())
    {
        INFO("row " << static_cast<int>(row));
        auto scene = SidebarWindow { { .row = row } };
        onDaemon(scene,
                 QStringLiteral("local"),
                 contour::window::DaemonLinkState::Connected,
                 { starfiveSection() });
        auto* tree = scene.item(QStringLiteral("sidebarTree"));
        REQUIRE(tree != nullptr);
        auto const& model = scene.controller.sidebarModel();
        auto const current = [&] {
            return tree->property("currentIndex").toInt();
        };
        auto const expansionOf = [&](int index) {
            return model.rows().at(static_cast<std::size_t>(index)).expansion;
        };

        scene.controller.focusSidebar();
        REQUIRE(QTest::qWaitFor([&] { return tree->hasActiveFocus(); }, 1000));
        auto const starfive = daemonRowIndex(scene, "starfive");
        tree->setProperty("currentIndex", starfive);
        REQUIRE(expansionOf(starfive) == RowExpansion::Expanded);

        QTest::keyClick(scene.window, Qt::Key_Left);
        QCoreApplication::processEvents();
        CHECK(expansionOf(daemonRowIndex(scene, "starfive")) == RowExpansion::Collapsed);
        QTest::keyClick(scene.window, Qt::Key_Right);
        QCoreApplication::processEvents();
        CHECK(expansionOf(daemonRowIndex(scene, "starfive")) == RowExpansion::Expanded);

        // Its workspace, folded by default: Left moves up to the daemon row.
        auto const ci = model.rowIndexOf(
            contour::window::SidebarRowKey { .kind = contour::window::SidebarRowKind::Workspace,
                                             .daemon = contour::window::hostDaemonRef("starfive"),
                                             .instance = contour::window::DaemonInstance { 9 },
                                             .workspace = 5,
                                             .tab = 0,
                                             .pane = {} });
        REQUIRE(ci >= 0);
        tree->setProperty("currentIndex", ci);
        QTest::keyClick(scene.window, Qt::Key_Left);
        QCoreApplication::processEvents();
        CHECK(current() == daemonRowIndex(scene, "starfive"));

        // Home and End reach the first and the last daemon's rows.
        QTest::keyClick(scene.window, Qt::Key_Home);
        QCoreApplication::processEvents();
        CHECK(current() == 0);
        CHECK(model.rows().front().key.kind == contour::window::SidebarRowKind::Daemon);
        QTest::keyClick(scene.window, Qt::Key_End);
        QCoreApplication::processEvents();
        CHECK(current() == model.rowCount() - 1);
        CHECK(current() > std::max(daemonRowIndex(scene, "starfive"), daemonRowIndex(scene, "local")));
        CHECK(scene.warnings.count(isSidebarDiagnostic) == 0);
    }
}

TEST_CASE("the Menu key and Shift+F10 open a daemon row's menu (offscreen)",
          "[contour][gui][qml][sidebar][daemon-row]")
{
    for (auto const row: drawnRows())
    {
        INFO("row " << static_cast<int>(row));
        auto scene = SidebarWindow { { .row = row } };
        onDaemon(scene, QStringLiteral("local"));
        auto* tree = scene.item(QStringLiteral("sidebarTree"));
        REQUIRE(tree != nullptr);
        scene.controller.focusSidebar();
        REQUIRE(QTest::qWaitFor([&] { return tree->hasActiveFocus(); }, 1000));
        tree->setProperty("currentIndex", daemonRowIndex(scene, "local"));
        auto& menus = scene.controller.sidebarHost().menus;
        menus.clear();

        QTest::keyClick(scene.window, Qt::Key_Menu);
        QCoreApplication::processEvents();
        QTest::keyClick(scene.window, Qt::Key_F10, Qt::ShiftModifier);
        QCoreApplication::processEvents();
        REQUIRE(menus.size() == 2);
        for (auto const& key: menus)
            CHECK(key.kind == contour::window::SidebarRowKind::Daemon);
    }
}

TEST_CASE("after a jump the selected row is scrolled into view (offscreen)",
          "[contour][gui][qml][sidebar][daemon-row]")
{
    auto scene = SidebarWindow { { .row = drawnRows().front() } };
    // The presence lists the window's own daemon first; starfive below it lists more workspaces than the
    // tree has room for.
    auto own = contour::window::DaemonSection {};
    own.daemon = contour::window::localDaemonRef("default");
    own.name = "local";
    own.locality = contour::window::DaemonLocality::Local;
    own.state = contour::window::DaemonLinkState::Connected;
    auto starfive = starfiveSection();
    for (auto const id: std::views::iota(uint64_t { 100 }, uint64_t { 140 }))
        starfive.directory.workspaces.push_back(contour::test::directoryWorkspaceOf(
            id, std::format("ws{}", id), { contour::test::directoryPaneOf(id * 10, "sh") }));
    onDaemon(scene, QStringLiteral("local"), contour::window::DaemonLinkState::Connected, { own, starfive });
    REQUIRE(daemonRowIndex(scene, "local") == 0);
    auto* tree = scene.item(QStringLiteral("sidebarTree"));
    REQUIRE(tree != nullptr);
    QMetaObject::invokeMethod(tree, "positionViewAtEnd");
    QCoreApplication::processEvents();
    REQUIRE(tree->property("contentY").toReal() > 0);

    // The window lands on build, at the top: it is brought into view.
    scene.controller.sidebarHost().displayed = contour::window::TabRef { .workspace = 1, .tab = 11 };
    scene.controller.sidebarModel().refreshNow();
    QCoreApplication::processEvents();
    auto const index = scene.controller.sidebarModel().currentIndex();
    REQUIRE(index >= 0);
    QQuickItem* item = nullptr;
    QMetaObject::invokeMethod(tree, "itemAtIndex", Q_RETURN_ARG(QQuickItem*, item), Q_ARG(int, index));
    REQUIRE(item != nullptr);
    auto const top = tree->property("contentY").toReal();
    CHECK(item->y() >= top - 0.5);
    CHECK(item->y() + item->height() <= top + tree->height() + 0.5);
}

TEST_CASE("a last-known row is dimmed and keeps 3:1 (offscreen)", "[contour][gui][qml][sidebar][daemon-row]")
{
    using contour::window::DaemonLinkState;
    // The live row's contrast over the last-known row's, both on the sidebar's fill: a step the eye sees, on
    // every look and appearance (the live row's ink is also its secondary text's on the native rows).
    constexpr auto MinimumDimGap = 1.6;
    for (auto const appearance: { vtbackend::ColorPreference::Light, vtbackend::ColorPreference::Dark })
        for (auto const row: drawnRows())
            for (auto const down:
                 { DaemonLinkState::Reconnecting, DaemonLinkState::NotRunning, DaemonLinkState::Failed })
            {
                INFO("appearance " << static_cast<int>(appearance) << ", row " << static_cast<int>(row)
                                   << ", state " << static_cast<int>(down));
                auto const forced = contour::test::ScopedChromeAppearance { appearance };
                auto scene = SidebarWindow { { .row = row } };
                // ci's pane works: what was last known of it draws no running mark.
                auto starfive = starfiveIn(down);
                starfive.directory.workspaces[0].tabs[0].panes[0].status = { recordOf(
                    vtbackend::ProgramState::Working) };
                onDaemon(scene, QStringLiteral("local"), DaemonLinkState::Connected, { starfive });
                scene.controller.sidebarModel().expand(scene.controller.sidebarModel().rowIndexOf(
                    contour::window::SidebarRowKey { .kind = contour::window::SidebarRowKind::Workspace,
                                                     .daemon = contour::window::hostDaemonRef("starfive"),
                                                     .instance = contour::window::DaemonInstance { 9 },
                                                     .workspace = 5,
                                                     .tab = 0,
                                                     .pane = {} }));
                scene.layOut();
                auto* lastKnown = scene.rowTitled(QStringLiteral("ci"));
                auto* live = scene.rowTitled(QStringLiteral("docs"));
                auto* lastKnownTab = scene.rowTitled(QStringLiteral("build"));
                REQUIRE(lastKnown != nullptr);
                REQUIRE(live != nullptr);
                REQUIRE(lastKnownTab != nullptr);
                auto* style = scene.engine.rootContext()
                                  ->contextProperty(QStringLiteral("chromeStyle"))
                                  .value<QObject*>();
                auto const* colors = style->property("colors").value<QObject*>();
                auto const fill = colors->property("sidebarFill").value<QColor>();
                auto const drawn = rowPart(*lastKnown, "sidebarRowTitle").property("color").value<QColor>();
                auto const liveInk = rowPart(*live, "sidebarRowTitle").property("color").value<QColor>();
                CHECK(drawn.rgba() == colors->property("sidebarLastKnownInk").value<QColor>().rgba());
                // A live row keeps the row's own ink.
                CHECK(liveInk.rgba() == colors->property("tabText").value<QColor>().rgba());
                // Over what shows through a translucent fill, as the window's surface is.
                auto const ground = fill.alpha() == 255 ? fill : scene.window->color();
                auto const over = [&](QColor const& ink) {
                    return vtbackend::mixColor(rgbOf(ground), rgbOf(ink), ink.alphaF());
                };
                auto const dimContrast = vtbackend::contrastRatio(over(drawn), rgbOf(ground));
                CHECK(dimContrast >= vtbackend::MinimumMarkContrast);
                CHECK(vtbackend::contrastRatio(over(liveInk), rgbOf(ground)) / dimContrast >= MinimumDimGap);
                // Its status mark takes the same ink, and no running ring.
                auto const& glyph = rowPart(*lastKnownTab, "sidebarRowGlyph");
                CHECK_FALSE(glyph.isVisible());
                CHECK(scene.warnings.count(isSidebarDiagnostic) == 0);
            }
}
// }}}
