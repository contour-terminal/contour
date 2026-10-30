// SPDX-License-Identifier: Apache-2.0
//
// QML component tests, continued: the permission dialog, dialogs on the display, secure input, the
// Settings kit controls and the shell integration card.

#include <contour/config/ChromeTokenGroups.hpp>
#include <contour/config/UiStyle.hpp>
#include <contour/platform/ColorConversion.hpp>
#include <contour/platform/SecureInput.hpp>
#include <contour/session/PermissionPrompt.hpp>
#include <contour/test/ChromeSnapshotPlan.hpp>
#include <contour/test/PopupWaits.hpp>
#include <contour/test/QmlChromeStyle.hpp>
#include <contour/test/QmlComponentsTestSupport.hpp>
#include <contour/test/QmlMessageCapture.hpp>
#include <contour/test/QmlMocks.hpp>
#include <contour/window/PaneHeaderPolicy.hpp>
#include <contour/window/SecureInputPolicy.hpp>
#include <contour/window/UiStyleProvider.hpp>

#include <vtbackend/shell/ScrollbarMarks.hpp>

#include <QtCore/QAbstractListModel>
#include <QtCore/QCoreApplication>
#include <QtCore/QDirIterator>
#include <QtCore/QEvent>
#include <QtCore/QFile>
#include <QtCore/QHash>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QMimeData>
#include <QtCore/QObject>
#include <QtCore/QPointF>
#include <QtCore/QRectF>
#include <QtCore/QRegularExpression>
#include <QtCore/QStringList>
#include <QtCore/QTextStream>
#include <QtGui/QAccessible>
#include <QtGui/QColor>
#include <QtGui/QDragEnterEvent>
#include <QtGui/QFont>
#include <QtGui/QGuiApplication>
#include <QtGui/QMouseEvent>
#include <QtGui/QPalette>
#include <QtQml/QJSValue>
#include <QtQml/QQmlComponent>
#include <QtQml/QQmlContext>
#include <QtQml/QQmlEngine>
#include <QtQml/QQmlExpression>
#include <QtQml/QQmlProperty>
#include <QtQuick/QQuickItem>
#include <QtQuick/QQuickWindow>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include <QtTest/QSignalSpy>
#include <QtTest/QTest>

using namespace contour::test::qmlSupport;

namespace
{

/// Runs @p check under every bundled scheme, chrome row (on its own host) and Qt appearance.
template <typename Check>
void forEverySchemeRowAndAppearance(Check const& check)
{
    for (auto const appearance: { vtbackend::ColorPreference::Light, vtbackend::ColorPreference::Dark })
    {
        contour::test::ScopedChromeAppearance const forced { appearance };
        for (auto const& rowHost: contour::test::snapshot::SnapshotRowHosts)
            for (auto const schemeName: vtbackend::defaultColorPaletteNames())
            {
                INFO("Qt palette: " << appearanceName(appearance));
                INFO("row: " << contour::config::chromeStyleName(rowHost.row));
                INFO("scheme: " << schemeName);
                auto palette = vtbackend::ColorPalette {};
                REQUIRE(vtbackend::defaultColorPalettes(schemeName, palette));
                auto setup = contour::test::defaultChromeSetup();
                setup.row = rowHost.row;
                setup.windowControls = contour::config::resolveWindowControlStyle(
                    contour::config::WindowControlStyle::Auto, rowHost.host);
                setup.scheme = contour::config::chromeSchemeColorsOf(palette);
                check(setup);
            }
    }
}

/// The questions the snapshot scripts ask -- a paste and an extension's consent -- and one whose verbatim
/// line and checkbox are wider than the dialog's own width in any chrome font.
[[nodiscard]] std::vector<QVariantMap> displayedQuestions()
{
    using contour::session::consentLine;
    using vtcontrol::ConsentWeight;
    auto const preview =
        contour::session::PastePreview { .lines = { "git log --oneline --graph --decorate --all",
                                                    "cmake --build out/release -j 8" },
                                         .moreLines = 48'211 };
    auto const wide =
        contour::session::PastePreview { .lines = { std::string(120, 'x') + " --and-a-long-flag", "y" },
                                         .moreLines = 3 };
    auto wideQuestion =
        contour::session::permissionPromptMap(contour::session::bigPastePrompt("zsh", 2'400'000, wide), 3);
    wideQuestion.insert(QStringLiteral("rememberText"),
                        QStringLiteral("Don't ask again for any paste into this session or any other one"));
    return {
        contour::session::permissionPromptMap(contour::session::bigPastePrompt("zsh", 2'400'000, preview), 1),
        wideQuestion,
        contour::session::permissionPromptMap(
            contour::session::consentPrompt(
                "“git-stats” wants to control Contour",
                { consentLine("Module: git-stats@/usr/share/contour/extensions/git-stats.wasm",
                              ConsentWeight::Observes),
                  consentLine("May see the windows, tabs and panes.", ConsentWeight::Observes),
                  consentLine("May read what panes show, and their command history.",
                              ConsentWeight::Observes),
                  consentLine("May type into panes (can run commands).", ConsentWeight::CanRunCommands) }),
            2),
    };
}

/// @return Whether @p item is a scroll bar that shows.
[[nodiscard]] bool isShownScrollBar(QQuickItem const& item)
{
    return item.inherits("QQuickScrollBar") && item.isVisible() && item.opacity() > 0.0;
}

/// Appends @p root and every item beneath it to @p items.
void collectItemsBeneath(QQuickItem* root, QList<QQuickItem*>& items)
{
    items.append(root);
    for (auto* child: root->childItems())
        collectItemsBeneath(child, items);
}

/// The gap between @p dialog's heading and the top of its part named @p next.
[[nodiscard]] qreal headingGap(QObject const& dialog, QString const& next)
{
    auto const* content = dialog.property("contentItem").value<QQuickItem*>();
    REQUIRE(content != nullptr);
    auto const headings = itemsNamed(*content, QStringLiteral("kitDialogHeading"));
    auto const below = itemsNamed(*content, next);
    REQUIRE(headings.size() == 1);
    REQUIRE(below.size() == 1);
    auto const* heading = headings.front();
    return below.front()->y() - (heading->y() + heading->height());
}

/// The kit's own gap under its heading: a dialog with a message.
[[nodiscard]] qreal kitHeadingGap(contour::test::ChromeSetup const& setup)
{
    QQmlEngine engine;
    contour::test::installChromeStyle(engine, setup);
    DialogHost const host;
    auto dialog = openKitDialog(engine,
                                host.window.contentItem(),
                                QVariantMap { { "heading", QStringLiteral("Close this tab?") },
                                              { "message", QStringLiteral("“cmake” is still running.") } });
    QCoreApplication::processEvents();
    return headingGap(*dialog, QStringLiteral("kitDialogMessage"));
}

/// Panes side by side in an offscreen window, each on its own MockSession: the first @p activeIndex is
/// active, and each is as wide as @p widths says.
struct SizeIndicatorScene
{
    QQmlEngine engine;
    MockTabController controller;
    std::vector<std::unique_ptr<MockSession>> sessions;
    std::unique_ptr<QObject> window;
    std::vector<QQuickItem*> panes;

    SizeIndicatorScene(std::vector<int> const& widths,
                       size_t activeIndex,
                       contour::test::ChromeSetup const& setup = macRowSetup())
    {
        qmlRegisterType<StubContourTerminal>("Contour.Terminal", 1, 0, "ContourTerminal");
        engine.rootContext()->setContextProperty("terminalSessions", &controller);
        contour::test::installChromeStyle(engine, setup);
        auto source = QByteArray("import QtQuick\nimport QtQuick.Window\nimport Contour.Ui\n"
                                 "Window {\n  width: 900; height: 300; visible: true\n  Row {\n");
        for (auto const index: std::views::iota(size_t { 0 }, widths.size()))
        {
            sessions.push_back(std::make_unique<MockSession>());
            auto const name = QStringLiteral("session%1").arg(index);
            engine.rootContext()->setContextProperty(name, sessions.back().get());
            source += QStringLiteral(
                          "    TerminalPane { objectName: \"pane%1\"; width: %2; height: 300; session: %3;"
                          " paneActive: %4 }\n")
                          .arg(index)
                          .arg(widths[index])
                          .arg(name)
                          .arg(index == activeIndex ? QStringLiteral("true") : QStringLiteral("false"))
                          .toUtf8();
        }
        source += "  }\n}\n";
        QQmlComponent component(&engine);
        component.setData(source, QUrl(QStringLiteral("qrc:/qt/qml/Contour/Ui/SizeIndicatorScene.qml")));
        while (component.status() == QQmlComponent::Loading)
            QCoreApplication::processEvents();
        INFO("component errors: " << component.errorString().toStdString());
        REQUIRE(component.isReady());
        window.reset(component.create());
        REQUIRE(window != nullptr);
        for (auto const index: std::views::iota(size_t { 0 }, widths.size()))
        {
            auto* pane = window->findChild<QQuickItem*>(QStringLiteral("pane%1").arg(index));
            REQUIRE(pane != nullptr);
            panes.push_back(pane);
        }
        QCoreApplication::processEvents();
    }

    [[nodiscard]] static contour::test::ChromeSetup macRowSetup()
    {
        auto setup = contour::test::defaultChromeSetup();
        setup.row = contour::config::ChromeStyle::MacOS;
        return setup;
    }

    /// Pane @p index's pill.
    [[nodiscard]] QQuickItem* pill(size_t index) const
    {
        auto* found = panes.at(index)->findChild<QQuickItem*>(QStringLiteral("sizeIndicator"));
        REQUIRE(found != nullptr);
        return found;
    }

    /// The hold every pill keeps after the last request, in milliseconds.
    [[nodiscard]] int linger() { return static_cast<int>(chromeToken(engine, "motion", "linger")); }
};

/// The durations of the number animations under @p item: the Behaviors it fades and slides by.
[[nodiscard]] std::vector<int> numberAnimationDurations(QObject const& item)
{
    auto durations = std::vector<int> {};
    for (auto const* child: item.findChildren<QObject*>())
        if (child->inherits("QQuickNumberAnimation"))
            durations.push_back(child->property("duration").toInt());
    return durations;
}

/// The rows on which one tab of one pane under `tab_bar: multiple` has no header, tab well or band: the lock
/// goes in the pane (window::SecureIndicatorRules).
constexpr auto InPaneLockRows = std::array { contour::config::ChromeStyle::Fluent,
                                             contour::config::ChromeStyle::ContourLinux,
                                             contour::config::ChromeStyle::Terminal };

/// An 800 × 400 SessionChrome on @p row whose pane is locked and whose rule puts the lock in the pane.
struct SecureLockScene
{
    QQmlEngine engine;
    MockTabController controller;
    std::unique_ptr<MockSession> session = std::make_unique<MockSession>();
    ChromeHost host;
    QQuickItem* lock {};

    explicit SecureLockScene(contour::config::ChromeStyle row)
    {
        engine.rootContext()->setContextProperty("terminalSessions", &controller);
        auto setup = contour::test::defaultChromeSetup();
        setup.row = row;
        setup.secureInput = contour::platform::SecureInputSupport::None;
        contour::test::installChromeStyle(engine, setup);
        host = createChromeInWindow(engine, QSize { 800, 400 });
        session->setOverlayClearance(linkClearance(100, 20, 100));
        host.chrome->setProperty("session", QVariant::fromValue(static_cast<QObject*>(session.get())));
        host.chrome->setProperty("lockInPane", true);
        QCoreApplication::processEvents();
        lock = host.chrome->findChild<QQuickItem*>(QStringLiteral("secureInputPill"));
        REQUIRE(lock != nullptr);
    }

    void setLocked(
        contour::window::SecureIndicator indicator = contour::window::SecureIndicator::Locked) const
    {
        session->setSecureIndicator(static_cast<int>(indicator));
        QCoreApplication::processEvents();
    }

    [[nodiscard]] QString anchor() const
    {
        return lock->property("placement").toMap().value(QStringLiteral("anchor")).toString();
    }
};

/// A kit control under test: its QML item, parented into a shown window, with the chrome installed.
struct KitControl
{
    QQuickWindow window;
    std::unique_ptr<QObject> root;

    /// Loads the kit control @p type from the module with @p initialProperties and shows it.
    KitControl(QQmlEngine& engine, char const* type, QVariantMap const& initialProperties)
    {
        QQmlComponent component(
            &engine, QUrl(QStringLiteral("qrc:/qt/qml/Contour/Ui/%1.qml").arg(QString::fromLatin1(type))));
        while (component.status() == QQmlComponent::Loading)
            QCoreApplication::processEvents();
        INFO("component errors: " << component.errorString().toStdString());
        REQUIRE(component.isReady());
        root.reset(component.createWithInitialProperties(initialProperties));
        REQUIRE(root != nullptr);
        window.resize(KitWindowEdge, KitWindowEdge);
        window.show();
        item()->setParentItem(window.contentItem());
        item()->forceActiveFocus();
        QCoreApplication::processEvents();
    }

    /// The control as an item.
    [[nodiscard]] QQuickItem* item() const { return qobject_cast<QQuickItem*>(root.get()); }

    /// Presses and releases @p key in the window.
    void key(Qt::Key key)
    {
        QTest::keyClick(&window, key);
        QCoreApplication::processEvents();
    }

    /// Every item beneath the control named @p name, in the visual tree, left to right and top to bottom.
    [[nodiscard]] std::vector<QQuickItem*> parts(char const* name) const
    {
        auto found = std::vector<QQuickItem*> {};
        auto pending = std::vector<QQuickItem*> { item() };
        while (!pending.empty())
        {
            auto* current = pending.back();
            pending.pop_back();
            for (auto* child: current->childItems())
            {
                if (child->objectName() == QLatin1String(name))
                    found.push_back(child);
                pending.push_back(child);
            }
        }
        std::ranges::sort(found, [](QQuickItem const* a, QQuickItem const* b) {
            auto const left = a->mapToScene({ 0, 0 });
            auto const right = b->mapToScene({ 0, 0 });
            return std::tuple { left.y(), left.x() } < std::tuple { right.y(), right.x() };
        });
        return found;
    }

    /// The one item named @p name.
    [[nodiscard]] QQuickItem* part(char const* name) const
    {
        auto const found = parts(name);
        INFO("part: " << name);
        REQUIRE(found.size() == 1);
        return found.front();
    }

    static constexpr int KitWindowEdge = 400;
};

/// The rows the contrast test visits: the three native rows, then every bundled colour scheme on the Terminal
/// row.
[[nodiscard]] std::vector<std::string_view> kitRowNames()
{
    auto names = std::vector<std::string_view> { "macos", "fluent", "linux" };
    for (auto const name: vtbackend::defaultColorPaletteNames())
        names.push_back(name);
    return names;
}

/// The rows the kit controls are drawn on: the shape row the brief names, and the cell row.
[[nodiscard]] contour::test::ChromeSetup kitRowSetup(contour::config::ChromeStyle row)
{
    auto setup = contour::test::defaultChromeSetup();
    setup.row = row;
    return setup;
}

/// @p top painted over @p bottom.
[[nodiscard]] QColor kitOver(QColor top, QColor bottom)
{
    auto const alpha = top.alphaF();
    return QColor::fromRgbF((top.redF() * alpha) + (bottom.redF() * (1 - alpha)),
                            (top.greenF() * alpha) + (bottom.greenF() * (1 - alpha)),
                            (top.blueF() * alpha) + (bottom.blueF() * (1 - alpha)));
}

/// The contrast ratio of @p ink over @p ground.
[[nodiscard]] double kitContrast(QColor ink, QColor ground)
{
    return vtbackend::contrastRatio(contour::platform::toRGBColor(kitOver(ink, ground)),
                                    contour::platform::toRGBColor(ground));
}

/// What a chrome token colour @p name is, as the application palette paints it now.
[[nodiscard]] QColor kitToken(QQmlEngine& engine, char const* name)
{
    auto* style = engine.rootContext()->contextProperty(QStringLiteral("chromeStyle")).value<QObject*>();
    REQUIRE(style != nullptr);
    auto* colors = style->property("colors").value<QObject*>();
    REQUIRE(colors != nullptr);
    return colors->property(name).value<QColor>();
}

/// The line the Confirm stage shows in these cases.
constexpr auto ShellCardLine = std::string_view { "source <(contour generate integration shell zsh)" };

/// Puts the card on @p stage; Confirm shows ShellCardLine and, when @p error is not empty, the failure.
void showCard(contour::test::MockSession& session, contour::window::CardStage stage, QString error = {})
{
    session.setShellCard(stage, QString::fromUtf8(ShellCardLine), std::move(error));
}

} // namespace

TEST_CASE("PermissionDialog: keyboard-only answering (offscreen)", "[contour][gui][qml][permission]")
{
    MockSession session;
    auto host = PermissionChrome { session };
    auto const answered = [&](PermissionDecision decision, PermissionMemory memory) {
        REQUIRE(host.waitOpened(false));
        REQUIRE(session.lastPermissionAnswer.has_value());
        CHECK(session.lastPermissionAnswer->decision == decision);
        CHECK(session.lastPermissionAnswer->memory == memory);
        CHECK(QTest::qWaitFor([&] { return host.displayItem()->hasActiveFocus(); }, 2000));
    };

    SECTION("Return takes Paste, the paste prompt's default")
    {
        session.setPendingPermission(pasteQuestion(1));
        REQUIRE(host.waitOpened(true));
        QTest::keyClick(&host.window, Qt::Key_Return);
        answered(PermissionDecision::Allow, PermissionMemory::Once);
    }

    SECTION("Return takes Deny, the capture prompt's default")
    {
        session.setPendingPermission(captureQuestion(1));
        REQUIRE(host.waitOpened(true));
        QTest::keyClick(&host.window, Qt::Key_Return);
        answered(PermissionDecision::Deny, PermissionMemory::Once);
    }

    SECTION("Tab reaches the box, Space ticks it, and Deny remembers")
    {
        session.setPendingPermission(captureQuestion(1));
        REQUIRE(host.waitOpened(true));
        auto* const remember = host.part("permissionRemember");
        for ([[maybe_unused]] auto const tab: std::views::iota(0, 4))
        {
            if (remember->hasActiveFocus())
                break;
            QTest::keyClick(&host.window, Qt::Key_Tab);
        }
        REQUIRE(remember->hasActiveFocus());
        QTest::keyClick(&host.window, Qt::Key_Space);
        CHECK(remember->property("checked").toBool());
        host.part("kitDialogReject")->forceActiveFocus();
        QTest::keyClick(&host.window, Qt::Key_Space);
        answered(PermissionDecision::Deny, PermissionMemory::Session);
    }

    SECTION("Escape is Deny this once, even with the box ticked")
    {
        session.setPendingPermission(captureQuestion(1));
        REQUIRE(host.waitOpened(true));
        host.part("permissionRemember")->setProperty("checked", true);
        QTest::keyClick(&host.window, Qt::Key_Escape);
        answered(PermissionDecision::Deny, PermissionMemory::Once);
    }
}

TEST_CASE("PermissionDialog: a question replaced while shown is shown anew (offscreen)",
          "[contour][gui][qml][permission]")
{
    MockSession session;
    auto host = PermissionChrome { session };
    session.setPendingPermission(captureQuestion(1));
    REQUIRE(host.waitOpened(true));
    auto shows = QSignalSpy { host.dialog, SIGNAL(aboutToShow()) };
    auto hides = QSignalSpy { host.dialog, SIGNAL(aboutToHide()) };

    // The same question, asked again with more to it, under a new id while the dialog is open.
    session.setPendingPermission(captureQuestion(2, "zsh (larger)"));
    QCoreApplication::processEvents();
    REQUIRE(host.waitOpened(true));
    CHECK(hides.count() == 1); // closed...
    CHECK(shows.count() == 1); // ...and opened again with it
    CHECK(host.part("kitDialogHeading")->property("text").toString().contains("zsh (larger)"));

    // An answer now names the question shown now: the old one can never be answered from this dialog.
    REQUIRE(QMetaObject::invokeMethod(host.part("kitDialogAccept"), "clicked"));
    REQUIRE(session.lastPermissionAnswer.has_value());
    CHECK(session.lastPermissionAnswer->id == 2);
    CHECK(session.permissionAnswers == 1);
}

TEST_CASE("PermissionDialog: the dialog reopens for the next question (offscreen)",
          "[contour][gui][qml][permission]")
{
    MockSession session;
    auto host = PermissionChrome { session };
    session.setPendingPermission(captureQuestion(1));
    REQUIRE(host.waitOpened(true));

    REQUIRE(QMetaObject::invokeMethod(host.part("kitDialogAccept"), "clicked"));
    // The session publishes the next question once the answer is in.
    session.setPendingPermission(pasteQuestion(2));
    QCoreApplication::processEvents();
    REQUIRE(host.waitOpened(true));
    CHECK(host.part("kitDialogHeading")->property("text").toString() == "Paste 2.4 MB into “zsh”?");
    REQUIRE(QMetaObject::invokeMethod(host.part("kitDialogReject"), "clicked"));
    CHECK(session.permissionAnswers == 2);
    CHECK(session.lastPermissionAnswer->id == 2);
}

TEST_CASE("PermissionDialog: a rebound session shows the question it owes (offscreen)",
          "[contour][gui][qml][permission]")
{
    MockSession session;
    constexpr auto QuestionId = 3;
    session.setPendingPermission(captureQuestion(QuestionId));
    auto host = PermissionChrome { session };
    REQUIRE(host.waitOpened(true));

    // Rebound to a session that asks something else, the dialog answers that session's question.
    MockSession other;
    constexpr auto OtherId = 11;
    other.setPendingPermission(pasteQuestion(OtherId));
    host.chrome->setProperty("session", QVariant::fromValue(static_cast<QObject*>(&other)));
    QCoreApplication::processEvents();
    REQUIRE(host.waitOpened(true));
    CHECK(host.part("kitDialogHeading")->property("text").toString() == "Paste 2.4 MB into “zsh”?");
    REQUIRE(QMetaObject::invokeMethod(host.part("kitDialogReject"), "clicked"));
    REQUIRE(other.lastPermissionAnswer.has_value());
    CHECK(other.lastPermissionAnswer->id == OtherId);
    CHECK(session.permissionAnswers == 0);
}

TEST_CASE("PermissionDialog: the dialog closes when the question goes (offscreen)",
          "[contour][gui][qml][permission]")
{
    MockSession session;
    auto host = PermissionChrome { session };
    session.setPendingPermission(captureQuestion(5));
    REQUIRE(host.waitOpened(true));

    // Settled elsewhere (a reload, another answer): the dialog closes, answering nothing.
    session.setPendingPermission({});
    REQUIRE(host.waitOpened(false));
    CHECK(session.permissionAnswers == 0);
}

TEST_CASE("PermissionDialog: a program-chosen title renders as text (offscreen)",
          "[contour][gui][qml][permission]")
{
    MockSession session;
    auto host = PermissionChrome { session };
    session.setPendingPermission(captureQuestion(1, "<img src=x>"));
    REQUIRE(host.waitOpened(true));
    auto const* heading = host.part("kitDialogHeading");
    CHECK(heading->property("text").toString() == "Allow “<img src=x>” to read the screen?");
    CHECK(heading->property("textFormat").toInt() == Qt::PlainText);
}

TEST_CASE("PermissionDialog: an extension's consent sets apart what can run commands (offscreen)",
          "[contour][gui][qml][extensions]")
{
    using contour::session::consentLine;
    using contour::session::consentPrompt;
    MockSession session;
    auto host = PermissionChrome { session };
    constexpr auto QuestionId = 4;
    session.setPendingPermission(contour::session::permissionPromptMap(
        consentPrompt(
            "“git-stats” wants to control Contour",
            { consentLine("Module: git-stats@/x.wasm", vtcontrol::ConsentWeight::Observes),
              consentLine("May see the windows, tabs and panes.", vtcontrol::ConsentWeight::Observes),
              consentLine("May type into panes (can run commands).",
                          vtcontrol::ConsentWeight::CanRunCommands) }),
        QuestionId));
    REQUIRE(host.waitOpened(true));

    auto const lines = permissionLines(*host.dialog);
    REQUIRE(lines.size() == 3);
    CHECK_FALSE(lines[1]->property("font").value<QFont>().bold());
    CHECK(lines[2]->property("font").value<QFont>().bold());
    auto* const remember = host.part("permissionRemember");
    CHECK(remember->property("checked").toBool());
    CHECK(remember->property("text").toString() == "Remember for this extension");

    auto* const accept = host.part("kitDialogAccept");
    CHECK(accept->property("enabled").toBool());

    SECTION("Allow, remembered")
    {
        REQUIRE(QMetaObject::invokeMethod(accept, "clicked"));
        REQUIRE(session.lastPermissionAnswer.has_value());
        CHECK(session.lastPermissionAnswer->id == QuestionId);
        CHECK(session.lastPermissionAnswer->decision == PermissionDecision::Allow);
        CHECK(session.lastPermissionAnswer->memory == PermissionMemory::Persistent);
    }

    SECTION("unticked, Allow is not offered, and Deny is this once")
    {
        remember->setProperty("checked", false);
        CHECK_FALSE(accept->property("enabled").toBool());
        // Return takes Deny, the default; it would not take a disabled Allow either.
        REQUIRE(QMetaObject::invokeMethod(host.part("kitDialogReject"), "clicked"));
        REQUIRE(session.lastPermissionAnswer.has_value());
        CHECK(session.lastPermissionAnswer->decision == PermissionDecision::Deny);
        CHECK(session.lastPermissionAnswer->memory == PermissionMemory::Once);
    }
    CHECK(session.permissionAnswers == 1);
}

TEST_CASE("PermissionDialog: its lines and checkbox read on its surface everywhere (offscreen)",
          "[contour][gui][qml][permission]")
{
    forEverySchemeRowAndAppearance([](contour::test::ChromeSetup const& setup) {
        MockSession session;
        auto host = PermissionChrome { session, setup };
        session.setPendingPermission(pasteQuestion(1));
        REQUIRE(host.waitOpened(true));
        auto const* surface = host.dialog->property("background").value<QQuickItem*>();
        REQUIRE(surface != nullptr);
        auto const fill = fillOf(surface);
        auto const lines = permissionLines(*host.dialog);
        REQUIRE_FALSE(lines.isEmpty());
        for (auto const* line: lines)
            CHECK(contrastOn(line, fill) >= 4.5);
        auto const* remember = host.part("permissionRemember")->property("contentItem").value<QQuickItem*>();
        REQUIRE(remember != nullptr);
        CHECK(contrastOn(remember, fill) >= 4.5);
    });
}

TEST_CASE(
    "PermissionDialog: Allow is disabled while the session holds it after a re-presentation (offscreen)",
    "[contour][gui][qml][permission]")
{
    MockSession session;
    auto host = PermissionChrome { session };
    auto question = pasteQuestion(1);
    session.setPendingPermission(question);
    REQUIRE(host.waitOpened(true));
    CHECK(host.part("kitDialogAccept")->property("enabled").toBool());

    // Re-presented: another id, Allow held; Deny stays live.
    question.insert(QStringLiteral("id"), 2);
    question.insert(QString::fromUtf8(contour::session::AllowHeldKey), true);
    session.setPendingPermission(question);
    REQUIRE(QTest::qWaitFor([&] { return host.opened() && host.dialog->property("questionId").toInt() == 2; },
                            2000));
    CHECK_FALSE(host.part("kitDialogAccept")->property("enabled").toBool());
    CHECK(host.part("kitDialogReject")->property("enabled").toBool());

    // Given back: the same question, Allow live again.
    question.remove(QString::fromUtf8(contour::session::AllowHeldKey));
    session.setPendingPermission(question);
    QCoreApplication::processEvents();
    CHECK(host.part("kitDialogAccept")->property("enabled").toBool());
    CHECK(host.dialog->property("questionId").toInt() == 2);
}

TEST_CASE("PermissionDialog: a question put up from inside the answer to the last is shown and answered",
          "[contour][gui][qml][permission]")
{
    // The session's answer may publish the next question before the dialog that answered has closed: the
    // next one must still end up shown, bound to its session and id, and take its own answer.
    MockSession session;
    auto host = PermissionChrome { session };
    session.setPendingPermission(pasteQuestion(1));
    REQUIRE(host.waitOpened(true));
    session.nextPermissionOnAnswer = captureQuestion(2);

    REQUIRE(QMetaObject::invokeMethod(host.part("kitDialogAccept"), "clicked"));
    REQUIRE(session.lastPermissionAnswer.has_value());
    CHECK(session.lastPermissionAnswer->id == 1);

    REQUIRE(QTest::qWaitFor(
        [&] {
            return host.opened() && host.dialog->property("questionId").toInt() == 2
                   && host.dialog->property("prompt").toMap().value("id").toInt() == 2;
        },
        2000));
    QTest::qWait(200); // nothing closes it behind the answer's back
    REQUIRE(host.opened());
    CHECK(host.dialog->property("askingSession").value<QObject*>() == &session);

    REQUIRE(QMetaObject::invokeMethod(host.part("kitDialogReject"), "clicked"));
    REQUIRE(session.lastPermissionAnswer.has_value());
    CHECK(session.lastPermissionAnswer->id == 2);
    CHECK(session.permissionAnswers == 2);
}

TEST_CASE("PermissionDialog: long consent text scrolls, and the buttons stay in view (offscreen)",
          "[contour][gui][qml][permission][extensions]")
{
    MockSession session;
    auto host = PermissionChrome { session };
    auto lines = std::vector<contour::session::PromptLine> {};
    constexpr auto ManyCommands = 80;
    for (auto const index: std::views::iota(0, ManyCommands))
        lines.push_back(contour::session::consentLine(
            std::format("command{}: run something {}", index, index), vtcontrol::ConsentWeight::Observes));
    session.setPendingPermission(contour::session::permissionPromptMap(
        contour::session::consentPrompt("“many” wants to run commands", std::move(lines)), 1));
    REQUIRE(host.waitOpened(true));

    auto const windowHeight = host.window.height();
    for (auto const* name: { "kitDialogAccept", "kitDialogReject", "permissionRemember" })
    {
        INFO(name);
        auto const* part = host.part(QString::fromLatin1(name));
        auto const bottom = part->mapToScene(QPointF { 0, part->height() }).y();
        CHECK(bottom <= windowHeight);
    }
    // The lines scroll, inside half the window, rather than spill past the dialog's edge.
    auto const* scroller = host.part("permissionLines");
    REQUIRE(QTest::qWaitFor([&] { return scroller->height() > 0; }, 2000));
    CHECK(scroller->height() <= (windowHeight / 2.0) + 1);
    CHECK(scroller->property("contentHeight").toReal() > scroller->height());
    auto const* dialogItem = host.dialog->property("contentItem").value<QQuickItem*>();
    REQUIRE(dialogItem != nullptr);
    CHECK(scroller->mapToScene(QPointF { 0, scroller->height() }).y()
          <= dialogItem->mapToScene(QPointF { 0, dialogItem->height() }).y());
}

// }}}

// {{{ Notices read on every row
TEST_CASE("notices: toast and banner text read on every scheme, row and appearance (offscreen)",
          "[contour][gui][qml][kit][notices]")
{
    // Text at 4.5:1 (WCAG 1.4.3), the "!" mark at 3:1 (1.4.11), on what each paints behind it.
    forEverySchemeRowAndAppearance([](contour::test::ChromeSetup const& setup) {
        QQmlEngine engine;
        contour::test::installChromeStyle(engine, setup);

        auto toast = createToast(
            engine,
            QVariantMap { { "text", QStringLiteral("Not pasted") }, { "glyph", QStringLiteral("!") } });
        auto const* toastFill = toast->property("background").value<QQuickItem*>();
        REQUIRE(toastFill != nullptr);
        auto const* toastText = toast->findChild<QObject*>(QStringLiteral("toastText"));
        auto const* toastGlyph = toast->findChild<QObject*>(QStringLiteral("toastGlyph"));
        REQUIRE(toastText != nullptr);
        REQUIRE(toastGlyph != nullptr);
        CHECK(contrastOn(toastText, fillOf(toastFill)) >= 4.5);
        CHECK(contrastOn(toastGlyph, fillOf(toastFill)) >= 3.0);

        auto banner = createBanner(engine,
                                   QVariantMap { { "text", QStringLiteral("Contour could not reload") },
                                                 { "glyph", QStringLiteral("!") },
                                                 { "actionText", QStringLiteral("Show in Editor") } });
        auto const* bannerFill = banner->property("background").value<QQuickItem*>();
        REQUIRE(bannerFill != nullptr);
        auto const* bannerText = banner->findChild<QObject*>(QStringLiteral("bannerText"));
        auto const* bannerGlyph = banner->findChild<QObject*>(QStringLiteral("bannerGlyph"));
        REQUIRE(bannerText != nullptr);
        REQUIRE(bannerGlyph != nullptr);
        CHECK(contrastOn(bannerText, fillOf(bannerFill)) >= 4.5);
        CHECK(contrastOn(bannerGlyph, fillOf(bannerFill)) >= 3.0);
    });
}

// }}}

// {{{ Save Layout as a kit dialog
TEST_CASE("Save Layout: Save is offered for a new name, and Replace only once confirmed (offscreen)",
          "[contour][gui][qml][layout]")
{
    contour::test::QmlMessageCapture const warnings;
    QQmlEngine engine;
    MockSaveLayoutController controller;
    controller.existing = { "dev", "ops" };
    auto host = makeSaveLayoutHost(engine, controller);
    REQUIRE(host.dialog != nullptr);
    openSaveLayoutPrompt(controller);
    REQUIRE(host.opened());
    auto* const field = host.nameField();
    auto* const accept = host.part(QStringLiteral("kitDialogAccept"));
    auto* const taken = host.part(QStringLiteral("saveLayoutTaken"));
    auto* const replace = host.part(QStringLiteral("saveLayoutReplace"));
    REQUIRE(field != nullptr);
    REQUIRE(accept != nullptr);
    REQUIRE(taken != nullptr);
    REQUIRE(replace != nullptr);
    auto const typed = [&](QString const& text) {
        field->setProperty("text", text);
        QCoreApplication::processEvents();
    };
    auto const* provider = engine.rootContext()
                               ->contextProperty(QStringLiteral("chromeStyle"))
                               .value<contour::window::UiStyleProvider*>();
    REQUIRE(provider != nullptr);

    CHECK(host.part(QStringLiteral("saveLayoutSummary"))->property("text").toString()
          == "3 tabs · 5 panes · working directories and commands");
    CHECK_FALSE(accept->property("enabled").toBool()); // empty
    typed(QStringLiteral("   "));
    CHECK_FALSE(accept->property("enabled").toBool()); // white space only
    typed(QStringLiteral("web"));
    CHECK(accept->property("enabled").toBool());
    CHECK(accept->property("text").toString() == "Save");
    CHECK_FALSE(taken->isVisible());

    typed(QStringLiteral("dev"));
    CHECK(taken->isVisible());
    CHECK_FALSE(accept->property("enabled").toBool());

    // Return on a disabled Save leaves the dialog open, saving nothing.
    field->forceActiveFocus();
    QTest::keyClick(qobject_cast<QWindow*>(host.window.get()), Qt::Key_Return);
    QCoreApplication::processEvents();
    CHECK(host.opened());
    CHECK(controller.saved.isEmpty());

    REQUIRE(QMetaObject::invokeMethod(replace, "clicked"));
    QCoreApplication::processEvents();
    CHECK(accept->property("enabled").toBool());
    CHECK(accept->property("text").toString() == "Replace");
    CHECK(host.dialog->property("acceptRole").toInt() == contour::window::UiStyleProvider::statusDanger());
    CHECK_FALSE(taken->isVisible());

    // Typing withdraws the confirmation.
    typed(QStringLiteral("devx"));
    typed(QStringLiteral("dev"));
    CHECK(accept->property("text").toString() == "Save");
    CHECK_FALSE(accept->property("enabled").toBool());

    REQUIRE(QMetaObject::invokeMethod(replace, "clicked"));
    QCoreApplication::processEvents();
    REQUIRE(QMetaObject::invokeMethod(host.dialog, "pressDefault"));
    QCoreApplication::processEvents();
    CHECK(controller.replaced == QStringList { QStringLiteral("dev") });
    CHECK(controller.saved.isEmpty());
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("Save Layout: what it saves is read as it opens (offscreen)", "[contour][gui][qml][layout]")
{
    QQmlEngine engine;
    MockSaveLayoutController controller;
    auto host = makeSaveLayoutHost(engine, controller);
    REQUIRE(host.dialog != nullptr);
    controller.tabs = 1;
    controller.panes = 1;
    openSaveLayoutPrompt(controller);
    REQUIRE(host.opened());
    CHECK(host.part(QStringLiteral("saveLayoutSummary"))->property("text").toString()
          == "1 tab · 1 pane · working directories and commands");
    REQUIRE(QMetaObject::invokeMethod(host.dialog, "close"));
    REQUIRE(QTest::qWaitFor([&] { return !host.opened(); }, 2000));

    // Tabs added between two openings: the second says so.
    controller.tabs = 3;
    controller.panes = 4;
    openSaveLayoutPrompt(controller);
    REQUIRE(QTest::qWaitFor([&] { return host.opened(); }, 2000));
    CHECK(host.part(QStringLiteral("saveLayoutSummary"))->property("text").toString()
          == "3 tabs · 4 panes · working directories and commands");
}

TEST_CASE("Save Layout: its labels read on its surface everywhere (offscreen)", "[contour][gui][qml][layout]")
{
    forEverySchemeRowAndAppearance([](contour::test::ChromeSetup const& setup) {
        QQmlEngine engine;
        MockSaveLayoutController controller;
        controller.existing = { "dev" };
        auto host = makeSaveLayoutHost(engine, controller, setup);
        REQUIRE(host.dialog != nullptr);
        openSaveLayoutPrompt(controller);
        REQUIRE(QTest::qWaitFor([&] { return host.opened(); }, 2000));
        host.nameField()->setProperty("text", QStringLiteral("dev"));
        QCoreApplication::processEvents();
        auto const* surface = host.dialog->property("background").value<QQuickItem*>();
        REQUIRE(surface != nullptr);
        auto const fill = fillOf(surface);
        CHECK(contrastOn(host.part(QStringLiteral("saveLayoutNameLabel")), fill) >= 4.5);
        CHECK(contrastOn(host.part(QStringLiteral("saveLayoutSummary")), fill) >= 4.5);
        auto const* replace = host.part(QStringLiteral("saveLayoutReplace"));
        REQUIRE(replace != nullptr);
        auto const* replaceLabel = replace->property("contentItem").value<QQuickItem*>();
        REQUIRE(replaceLabel != nullptr);
        CHECK(contrastOn(replaceLabel, fill) >= 4.5);
    });
}

TEST_CASE("Save Layout: a sheet on the macOS row (offscreen)", "[contour][gui][qml][layout]")
{
    QQmlEngine engine;
    MockSaveLayoutController controller;
    auto setup = contour::test::defaultChromeSetup();
    setup.row = contour::config::ChromeStyle::MacOS;
    auto host = makeSaveLayoutHost(engine, controller, setup);
    REQUIRE(host.dialog != nullptr);
    openSaveLayoutPrompt(controller);
    REQUIRE(QTest::qWaitFor([&] { return host.opened(); }, 2000));
    CHECK(host.dialog->property("y").toReal() == 0);
}

TEST_CASE("Save Layout: the Terminal row's Replace reads on its fill (offscreen)",
          "[contour][gui][qml][layout]")
{
    auto const schemeName = GENERATE(as<std::string_view> {}, "contour-dark", "contour-light");
    INFO("scheme: " << schemeName);
    QQmlEngine engine;
    MockSaveLayoutController controller;
    controller.existing = { "dev" };
    auto host = makeSaveLayoutHost(engine, controller, terminalRowSetup(schemeName));
    REQUIRE(host.dialog != nullptr);
    openSaveLayoutPrompt(controller);
    REQUIRE(QTest::qWaitFor([&] { return host.opened(); }, 2000));
    host.nameField()->setProperty("text", QStringLiteral("dev"));
    QCoreApplication::processEvents();
    REQUIRE(QMetaObject::invokeMethod(host.part(QStringLiteral("saveLayoutReplace")), "clicked"));
    QCoreApplication::processEvents();

    auto const* provider = engine.rootContext()
                               ->contextProperty(QStringLiteral("chromeStyle"))
                               .value<contour::window::UiStyleProvider*>();
    REQUIRE(provider != nullptr);
    auto const danger = contour::window::UiStyleProvider::statusDanger();
    CHECK(vtbackend::contrastRatio(contour::platform::toRGBColor(provider->onStatusColor(danger)),
                                   contour::platform::toRGBColor(provider->statusColor(danger)))
          >= 4.5);
    // And the button paints exactly that pair.
    auto* const accept = host.part(QStringLiteral("kitDialogAccept"));
    REQUIRE(accept != nullptr);
    auto const* label = accept->property("contentItem").value<QQuickItem*>();
    auto const* fill = accept->property("background").value<QQuickItem*>();
    REQUIRE(label != nullptr);
    REQUIRE(fill != nullptr);
    CHECK(contrastOn(label, fillOf(fill)) >= 4.5);
}

TEST_CASE("PopupSurface: on the Terminal row it casts the e2 hard copy the width of the box (offscreen)",
          "[contour][gui][qml][kit]")
{
    // A menu, a flyout or a dialog: none names an elevation, and on a row without blur the popup shadow's
    // nine-patch drew a slab wider than the box.
    QQmlEngine engine;
    contour::test::installChromeStyle(engine, terminalRowSetup("contour-dark"));
    contour::test::QmlMessageCapture const warnings;

    QQmlComponent component(&engine);
    component.setData("import QtQuick\n"
                      "import Contour.Ui\n"
                      "PopupSurface { width: 200; height: 100 }\n",
                      QUrl());
    INFO("component errors: " << component.errorString().toStdString());
    REQUIRE(component.isReady());
    auto surface = std::unique_ptr<QObject>(component.create());
    auto* item = qobject_cast<QQuickItem*>(surface.get());
    REQUIRE(item != nullptr);
    QCoreApplication::processEvents();

    auto under = QList<QQuickItem*> {};
    collectItemsBeneath(item, under);
    auto const hard = std::ranges::find_if(
        under, [](QQuickItem const* part) { return part->objectName() == "elevationHardShadow"; });
    REQUIRE(hard != under.end());
    CHECK((*hard)->isVisible());
    CHECK((*hard)->width() == Catch::Approx(item->width()));
    auto const* style =
        engine.rootContext()->contextProperty(QStringLiteral("chromeStyle")).value<QObject*>();
    REQUIRE(style != nullptr);
    auto const offset =
        style->property("elevation").value<QObject*>()->property("e2").value<QObject*>()->property("offsetX");
    CHECK((*hard)->x() == Catch::Approx(offset.toReal()));
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("PermissionDialog: on the Terminal row nothing runs past its border (offscreen)",
          "[contour][gui][qml][permission]")
{
    for (auto const& question: displayedQuestions())
    {
        INFO("question: " << question.value("title").toString().toStdString());
        MockSession session;
        auto host = PermissionChrome { session, terminalRowSetup("contour-dark") };
        session.setPendingPermission(question);
        REQUIRE(host.waitOpened(true));
        auto* const scroll = host.part("permissionLines");
        REQUIRE(QTest::qWaitFor([&] { return scroll->height() > 0.0; }, 2000));
        QCoreApplication::processEvents();

        auto const* content = host.dialog->property("contentItem").value<QQuickItem*>();
        REQUIRE(content != nullptr);
        auto const inner = host.dialog->property("availableWidth").toReal();
        auto const cell = host.engine.rootContext()
                              ->contextProperty(QStringLiteral("chromeStyle"))
                              .value<QObject*>()
                              ->property("cellWidth")
                              .toReal();
        REQUIRE(cell > 0.0);

        // Sized in whole cells, and its content no wider than inside its border.
        auto const width = host.dialog->property("width").toReal();
        CHECK(std::abs((width / cell) - std::round(width / cell)) < 0.01);
        CHECK(content->implicitWidth() <= inner + 0.5);

        // The checkbox and both buttons sit inside the border.
        for (auto const* name: { "permissionRemember", "kitDialogReject", "kitDialogAccept" })
        {
            INFO("part: " << name);
            auto* const part = host.part(QString::fromLatin1(name));
            auto const box = part->mapRectToItem(content, QRectF(0, 0, part->width(), part->height()));
            CHECK(box.left() >= -0.5);
            CHECK(box.right() <= inner + 0.5);
        }

        // The e2 elevation: a hard copy of the box, one cell over and one down, no wider than the box.
        auto* const surface = host.dialog->property("background").value<QQuickItem*>();
        REQUIRE(surface != nullptr);
        auto under = QList<QQuickItem*> {};
        collectItemsBeneath(surface, under);
        auto const hard = std::ranges::find_if(
            under, [](QQuickItem const* item) { return item->objectName() == "elevationHardShadow"; });
        REQUIRE(hard != under.end());
        CHECK((*hard)->isVisible());
        CHECK((*hard)->width() == Catch::Approx(surface->width()));
        CHECK((*hard)->x() == Catch::Approx(cell));

        // No scroll bar chrome, and every line whole: wrapped, never cut, the last one in view.
        auto beneath = QList<QQuickItem*> {};
        collectItemsBeneath(scroll, beneath);
        auto const shownBars =
            std::ranges::count_if(beneath, [](QQuickItem const* item) { return isShownScrollBar(*item); });
        CHECK(shownBars == 0);
        auto const lines = permissionLines(*host.dialog);
        REQUIRE_FALSE(lines.isEmpty());
        for (auto const* line: lines)
        {
            INFO("line: " << line->property("text").toString().toStdString());
            CHECK_FALSE(line->property("truncated").toBool());
            CHECK(line->width() <= scroll->width() + 0.5);
        }
        auto const* last = lines.back();
        CHECK(last->mapRectToItem(scroll, QRectF(0, 0, last->width(), last->height())).bottom()
              <= scroll->height() + 0.5);
    }
}

TEST_CASE("PermissionDialog: its body starts under the heading as the kit's message does (offscreen)",
          "[contour][gui][qml][permission]")
{
    auto const setup = contour::test::defaultChromeSetup();
    auto const expected = kitHeadingGap(setup);
    MockSession session;
    auto host = PermissionChrome { session, setup };
    session.setPendingPermission(pasteQuestion(1));
    REQUIRE(host.waitOpened(true));
    QCoreApplication::processEvents();
    CHECK(headingGap(*host.dialog, QStringLiteral("kitDialogBody")) == Catch::Approx(expected).margin(0.5));
}

TEST_CASE("Save Layout: its body starts under the heading as the kit's message does (offscreen)",
          "[contour][gui][qml][layout]")
{
    auto const setup = contour::test::defaultChromeSetup();
    auto const expected = kitHeadingGap(setup);
    QQmlEngine engine;
    MockSaveLayoutController controller;
    auto host = makeSaveLayoutHost(engine, controller, setup);
    REQUIRE(host.dialog != nullptr);
    openSaveLayoutPrompt(controller);
    REQUIRE(QTest::qWaitFor([&] { return host.opened(); }, 2000));
    QCoreApplication::processEvents();
    CHECK(headingGap(*host.dialog, QStringLiteral("kitDialogBody")) == Catch::Approx(expected).margin(0.5));
}

TEST_CASE("Save Layout: on the Terminal row a taken name's message wraps rather than being cut (offscreen)",
          "[contour][gui][qml][layout]")
{
    QQmlEngine engine;
    MockSaveLayoutController controller;
    controller.existing = { "dev" };
    auto host = makeSaveLayoutHost(engine, controller, terminalRowSetup("contour-dark"));
    REQUIRE(host.dialog != nullptr);
    openSaveLayoutPrompt(controller);
    REQUIRE(QTest::qWaitFor([&] { return host.opened(); }, 2000));
    host.nameField()->setProperty("text", QStringLiteral("dev"));
    QCoreApplication::processEvents();

    auto* const message = host.part(QStringLiteral("saveLayoutTakenMessage"));
    REQUIRE(message != nullptr);
    REQUIRE(QTest::qWaitFor([&] { return message->width() > 0.0; }, 2000));
    CHECK(message->property("text").toString() == QStringLiteral("“dev” already exists."));
    CHECK_FALSE(message->property("truncated").toBool());
}

TEST_CASE("size indicator: a pane rebound to another session drops the held pill unsettled (offscreen)",
          "[contour][gui][qml][sizeindicator]")
{
    contour::test::QmlMessageCapture const warnings;
    auto scene = SizeIndicatorScene { { 600 }, 0 };
    auto& leaving = *scene.sessions.front();
    auto* pill = scene.pill(0);
    auto* pane = scene.panes.front();
    auto* hold = pane->findChild<QObject*>(QStringLiteral("sizeIndicatorHold"));
    REQUIRE(hold != nullptr);
    leaving.requestSizeIndicator(QStringLiteral("100 × 30"));
    REQUIRE(QTest::qWaitFor([&] { return pill->property("shown").toBool(); }));
    REQUIRE(hold->property("running").toBool());

    auto coming = MockSession {};
    pane->setProperty("session", QVariant::fromValue(static_cast<QObject*>(&coming)));
    CHECK_FALSE(pane->property("sizeIndicatorHeld").toBool());
    CHECK_FALSE(hold->property("running").toBool());
    CHECK_FALSE(pill->property("shown").toBool());
    // Neither session is told the pill settled: the leaving one's burst ended with it, the coming one had
    // none.
    QTest::qWait(scene.linger() * 2);
    CHECK(leaving.settledSizeIndicators.empty());
    CHECK(coming.settledSizeIndicators.empty());
    pane->setProperty("session", QVariant::fromValue(static_cast<QObject*>(nullptr)));
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE(
    "size indicator: a resize shows the pill centred on its pane and settles after the hold (offscreen)",
    "[contour][gui][qml][sizeindicator]")
{
    contour::test::QmlMessageCapture const warnings;
    auto scene = SizeIndicatorScene { { 600 }, 0 };
    auto& session = *scene.sessions.front();
    auto* pill = scene.pill(0);
    auto* pane = scene.panes.front();
    CHECK_FALSE(pill->property("shown").toBool());

    session.requestSizeIndicator(QStringLiteral("100 × 30"));
    // Shown once its row has laid out, on the window's next polish: before that it knows no width to fit.
    REQUIRE(QTest::qWaitFor([&] { return pill->property("shown").toBool(); }));
    CHECK(pill->property("text").toString() == QStringLiteral("100 × 30"));
    CHECK(pill->x() + (pill->width() / 2) == Catch::Approx(pane->width() / 2).margin(1));
    CHECK(pill->height() == chromeToken(scene.engine, "size", "toast"));
    auto* background = pill->property("background").value<QQuickItem*>();
    REQUIRE(background != nullptr);
    CHECK(background->property("radius").toReal() == chromeToken(scene.engine, "radii", "toast"));
    auto const font = pill->property("textFont").value<QFont>();
    CHECK(font.weight() == QFont::DemiBold);
    CHECK(font.featureValue(QFont::Tag("tnum")) == 1);
    // Above the unfocused dim.
    // In the pane's overlay layer, which sits above the dim.
    REQUIRE(pill->parentItem() == pane->findChild<QQuickItem*>(QStringLiteral("paneOverlay")));
    CHECK(pill->parentItem()->z() > pane->findChild<QQuickItem*>(QStringLiteral("dimOverlay"))->z());
    // The session announces the settled size itself: the pill is no accessible object of its own.
    auto const attached = std::ranges::find_if(
        pill->children(), [](QObject const* child) { return child->inherits("QQuickAccessibleAttached"); });
    REQUIRE(attached != pill->children().end());
    CHECK((*attached)->property("ignored").toBool());

    // A second request within the hold restarts it: two thirds of a hold after it, a hold and a third after
    // the first, the pill still shows; one settle, after the last.
    QTest::qWait(scene.linger() * 2 / 3);
    session.requestSizeIndicator(QStringLiteral("101 × 30"));
    QTest::qWait(scene.linger() * 2 / 3);
    CHECK(pill->property("shown").toBool());
    CHECK(session.settledSizeIndicators.empty());

    CHECK(QTest::qWaitFor([&] { return !pill->property("shown").toBool(); }, scene.linger() * 4));
    CHECK(session.settledSizeIndicators == std::vector { true });
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("size indicator: every pane of a split shows its own (offscreen)",
          "[contour][gui][qml][sizeindicator]")
{
    contour::test::QmlMessageCapture const warnings;
    auto scene = SizeIndicatorScene { { 300, 280, 260, 60 }, 1 };
    auto const texts = std::array { QStringLiteral("37 × 30"),
                                    QStringLiteral("34 × 30"),
                                    QStringLiteral("32 × 30"),
                                    QStringLiteral("6 × 30") };
    for (auto const index: std::views::iota(size_t { 0 }, texts.size()))
        scene.sessions[index]->requestSizeIndicator(texts[index]);
    // The pills' rows lay out on the window's next polish, and only then know how wide they are.
    auto const padded = [](QQuickItem const* pill) {
        return pill->implicitWidth()
               > pill->property("leftPadding").toReal() + pill->property("rightPadding").toReal();
    };
    REQUIRE(QTest::qWaitFor([&] {
        return std::ranges::all_of(scene.panes, [&](auto const* pane) {
            return padded(pane->template findChild<QQuickItem*>(QStringLiteral("sizeIndicator")));
        });
    }));

    for (auto const index: std::views::iota(size_t { 0 }, size_t { 3 }))
    {
        INFO("pane " << index);
        auto* pill = scene.pill(index);
        CHECK(pill->property("shown").toBool());
        CHECK(pill->property("text").toString() == texts[index]);
        // centerIn snaps to whole pixels, so an odd width sits half a pixel off.
        CHECK(pill->x() + (pill->width() / 2) == Catch::Approx(scene.panes[index]->width() / 2).margin(1));
    }
    // Too narrow for the pill: none.
    CHECK_FALSE(scene.pill(3)->property("shown").toBool());

    CHECK(QTest::qWaitFor(
        [&] {
            return std::ranges::all_of(scene.sessions,
                                       [](auto const& s) { return !s->settledSizeIndicators.empty(); });
        },
        scene.linger() * 4));
    for (auto const index: std::views::iota(size_t { 0 }, scene.sessions.size()))
    {
        INFO("pane " << index);
        CHECK(scene.sessions[index]->settledSizeIndicators == std::vector { index == 1 });
    }
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("size indicator: the Terminal row draws it as one inverse row (offscreen)",
          "[contour][gui][qml][sizeindicator]")
{
    contour::test::QmlMessageCapture const warnings;
    auto setup = contour::test::defaultChromeSetup();
    setup.row = contour::config::ChromeStyle::Terminal;
    auto scene = SizeIndicatorScene { { 600 }, 0, setup };
    scene.sessions.front()->requestSizeIndicator(QStringLiteral("100 × 30"));
    QCoreApplication::processEvents();

    auto* pill = scene.pill(0);
    auto* style =
        scene.engine.rootContext()->contextProperty(QStringLiteral("chromeStyle")).value<QObject*>();
    REQUIRE(style != nullptr);
    CHECK(pill->height() == style->property("cellHeight").toReal());
    auto* background = pill->property("background").value<QQuickItem*>();
    REQUIRE(background != nullptr);
    CHECK(background->property("radius").toReal() == 0.0);
    // Inverse video: the scheme's foreground as the fill.
    auto const& scheme = setup.scheme.bySlot;
    CHECK(
        background->property("color").value<QColor>()
        == contour::platform::toQColor(scheme[static_cast<size_t>(contour::config::SchemeSlot::Foreground)]));
    // An in-pane pill casts no hard offset copy on this row: that is a real popover's.
    auto* hard = pill->findChild<QQuickItem*>(QStringLiteral("elevationHardShadow"));
    REQUIRE(hard != nullptr);
    CHECK_FALSE(hard->isVisible());
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("size indicator: an inactive Terminal-row card shows it at full strength (offscreen)",
          "[contour][gui][qml][sizeindicator][panecard]")
{
    // The Terminal row fades an inactive card's content (tui.inactivePaneOpacity); the size indicator is
    // drawn outside that fade, so a resized inactive pane reads its size as the active one does.
    contour::test::QmlMessageCapture const warnings;
    auto setup = paneCardSetup();
    setup.row = contour::config::ChromeStyle::Terminal;
    auto host = PaneTreeHost { setup, QSize { 800, 400 } };
    auto panes = TwoPanes {};
    host.show(panes.split, /*visiblePanes*/ 2);

    auto const& inactive = host.cardOf(panes.right());
    REQUIRE(PaneTreeHost::partOf(inactive, "paneCardContent").opacity() < 1.0);
    panes.rightSession.requestSizeIndicator(QStringLiteral("40 \u00D7 24"));
    auto* pill = inactive.findChild<QQuickItem*>(QStringLiteral("sizeIndicator"));
    REQUIRE(pill != nullptr);
    REQUIRE(QTest::qWaitFor([&] { return pill->property("shown").toBool(); }));

    // Nothing above the pill fades it.
    auto opacity = 1.0;
    auto const* item = pill->parentItem();
    while (item != nullptr)
    {
        opacity *= item->opacity();
        item = item->parentItem();
    }
    CHECK(opacity == 1.0);
    // And it is still centred on its own grid.
    auto const& grid = *pill->parentItem();
    CHECK(pill->x() + (pill->width() / 2) == Catch::Approx(grid.width() / 2).margin(1));
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("overlay motion: the overlay bar fades out over the standard duration (offscreen)",
          "[contour][gui][qml][motion]")
{
    QQmlEngine engine;
    MockTabController controller;
    engine.rootContext()->setContextProperty("terminalSessions", &controller);
    contour::test::installChromeStyle(engine);
    contour::test::QmlMessageCapture const warnings;

    auto host = createChromeInWindow(engine);
    auto session = createScrollableSession();
    host.chrome->setProperty("session", QVariant::fromValue(static_cast<QObject*>(session.get())));
    REQUIRE(QTest::qWaitFor([&] { return host.bar->opacity() == 1.0; }));

    auto const standard = static_cast<int>(chromeToken(engine, "motion", "standard"));
    CHECK(std::ranges::contains(numberAnimationDurations(*host.bar), standard));

    session->setScrollbarVisible(false);
    QCoreApplication::processEvents();
    // Still fading: visible, and partly transparent on the way out.
    CHECK(host.bar->isVisible());
    CHECK(QTest::qWaitFor([&] { return host.bar->opacity() < 1.0; }));
    // Gone once it reaches 0, so a hidden bar takes no press.
    CHECK(QTest::qWaitFor([&] { return !host.bar->isVisible(); }, standard * 10));
    CHECK(host.bar->opacity() == 0.0);
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("overlay motion: Reduce Motion cross-fades and keeps the hold (offscreen)",
          "[contour][gui][qml][motion]")
{
    contour::test::QmlMessageCapture const warnings;
    auto setup = contour::test::defaultChromeSetup();
    setup.row = contour::config::ChromeStyle::MacOS;
    setup.accessibility.motion = contour::config::MotionPreference::Reduced;

    SECTION("the size pill and its hold")
    {
        auto scene = SizeIndicatorScene { { 600 }, 0, setup };
        auto* pill = scene.pill(0);
        CHECK(pill->property("enterOffset").toReal() == 0.0);
        auto* hold = scene.panes.front()->findChild<QObject*>(QStringLiteral("sizeIndicatorHold"));
        REQUIRE(hold != nullptr);
        // A hold is no animation: as long as without Reduce Motion.
        CHECK(hold->property("interval").toInt() == 600);
        CHECK(hold->property("interval").toInt() == scene.linger());
    }

    SECTION("the overlay bar's fade")
    {
        QQmlEngine engine;
        MockTabController controller;
        engine.rootContext()->setContextProperty("terminalSessions", &controller);
        contour::test::installChromeStyle(engine, setup);
        auto host = createChromeInWindow(engine);
        auto session = createScrollableSession();
        host.chrome->setProperty("session", QVariant::fromValue(static_cast<QObject*>(session.get())));
        QCoreApplication::processEvents();
        auto const durations = numberAnimationDurations(*host.bar);
        REQUIRE_FALSE(durations.empty());
        CHECK(std::ranges::all_of(durations,
                                  [](int ms) { return ms == contour::config::ReducedMotionCrossFadeMs; }));
    }
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

// }}}

// {{{ Secure input: the lock

TEST_CASE("SecureInputLock: the icon on the native rows, `[secure]` on the Terminal row (offscreen)",
          "[contour][gui][qml][secureinput]")
{
    for (auto const row: { contour::config::ChromeStyle::MacOS, contour::config::ChromeStyle::Terminal })
    {
        INFO("row: " << contour::config::chromeStyleName(row));
        contour::test::QmlMessageCapture const warnings;
        QQmlEngine engine;
        auto setup = contour::test::defaultChromeSetup();
        setup.row = row;
        setup.secureInput = contour::platform::SecureInputSupport::Native;
        contour::test::installChromeStyle(engine, setup);

        QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/qt/qml/Contour/Ui/SecureInputLock.qml")));
        INFO("errors: " << component.errorString().toStdString());
        REQUIRE(component.isReady());
        auto lock = std::unique_ptr<QObject>(component.create());
        REQUIRE(lock != nullptr);
        QCoreApplication::processEvents();

        // A lock whose pane is not the one the OS secures does not claim it, on any row (review I3) ...
        CHECK(accessibleNameOf(*lock) == QStringLiteral("Input hidden"));
        // ... and one whose pane is, named for what it means: the OS turns secure keyboard entry on here.
        lock->setProperty("indicator", static_cast<int>(contour::window::SecureIndicator::LockedSecured));
        QCoreApplication::processEvents();
        CHECK(accessibleNameOf(*lock) == QStringLiteral("Secure input on"));
        auto* icon = lock->findChild<QQuickItem*>(QStringLiteral("secureInputIcon"));
        auto* text = lock->findChild<QQuickItem*>(QStringLiteral("secureInputText"));
        REQUIRE(icon != nullptr);
        REQUIRE(text != nullptr);
        if (row == contour::config::ChromeStyle::Terminal)
        {
            CHECK_FALSE(icon->isVisible());
            CHECK(text->isVisible());
            CHECK(text->property("text").toString() == QStringLiteral("[secure]"));
        }
        else
        {
            CHECK(icon->isVisible());
            CHECK_FALSE(text->isVisible());
            auto const edge = chromeToken(engine, "size", "secureInputGlyph");
            CHECK(edge > 0);
            CHECK(qobject_cast<QQuickItem*>(lock.get())->width() == edge);
            CHECK(qobject_cast<QQuickItem*>(lock.get())->height() == edge);
        }
        CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
    }
}

TEST_CASE("SecureInputLock: \"Input hidden\" where the OS has no secure keyboard entry (offscreen)",
          "[contour][gui][qml][secureinput]")
{
    QQmlEngine engine;
    auto setup = contour::test::defaultChromeSetup();
    setup.secureInput = contour::platform::SecureInputSupport::None;
    contour::test::installChromeStyle(engine, setup);
    QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/qt/qml/Contour/Ui/SecureInputLock.qml")));
    REQUIRE(component.isReady());
    auto lock = std::unique_ptr<QObject>(component.create());
    REQUIRE(lock != nullptr);
    CHECK(accessibleNameOf(*lock) == QStringLiteral("Input hidden"));
}

TEST_CASE(
    "secure lock pill: with no header, well or band a locked pane's pill sits top right over blank cells "
    "(offscreen)",
    "[contour][gui][qml][secureinput][securelock]")
{
    for (auto const row: InPaneLockRows)
    {
        INFO("row: " << contour::config::chromeStyleName(row));
        contour::test::QmlMessageCapture const warnings;
        auto scene = SecureLockScene { row };
        CHECK_FALSE(scene.lock->isVisible());

        scene.setLocked();
        REQUIRE(scene.lock->isVisible());
        CHECK(scene.anchor() == QStringLiteral("topRight"));
        // Placed like the link pill: its insets, its height.
        CHECK(scene.lock->y() == chromeToken(scene.engine, "space", "s4"));
        CHECK(scene.lock->x() + scene.lock->width()
              == Catch::Approx(scene.host.chrome->width() - chromeToken(scene.engine, "space", "s5")));
        CHECK(scene.lock->height() == chromeToken(scene.engine, "size", "linkPill"));
        CHECK(accessibleNameOf(*scene.lock) == QStringLiteral("Input hidden"));

        auto* icon = scene.lock->findChild<QQuickItem*>(QStringLiteral("secureInputIcon"));
        auto* cells = scene.lock->findChild<QQuickItem*>(QStringLiteral("secureInputText"));
        auto* words = scene.lock->findChild<QQuickItem*>(QStringLiteral("secureInputPillText"));
        REQUIRE(icon != nullptr);
        REQUIRE(cells != nullptr);
        REQUIRE(words != nullptr);
        if (row == contour::config::ChromeStyle::Terminal)
        {
            // An inverse cell run: `[secure]`, no icon and no words beside it.
            CHECK(cells->isVisible());
            CHECK(cells->property("text").toString() == QStringLiteral("[secure]"));
            CHECK_FALSE(icon->isVisible());
            CHECK_FALSE(words->isVisible());
        }
        else
        {
            CHECK(icon->isVisible());
            CHECK(words->isVisible());
            CHECK(words->property("text").toString() == QStringLiteral("Input hidden"));
            CHECK_FALSE(cells->isVisible());
        }

        scene.setLocked(contour::window::SecureIndicator::None);
        CHECK_FALSE(scene.lock->isVisible());
        CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
    }
}

TEST_CASE("secure lock pill: shown only where the rule puts the lock in the pane (offscreen)",
          "[contour][gui][qml][secureinput][securelock]")
{
    for (auto const row: InPaneLockRows)
    {
        INFO("row: " << contour::config::chromeStyleName(row));
        auto scene = SecureLockScene { row };
        scene.setLocked();
        REQUIRE(scene.lock->isVisible());
        // A header, a tab or a band holds the lock instead.
        scene.host.chrome->setProperty("lockInPane", false);
        QCoreApplication::processEvents();
        CHECK_FALSE(scene.lock->isVisible());
    }
}

TEST_CASE("secure lock pill: it yields the top to the link pill and goes to the bottom (offscreen)",
          "[contour][gui][qml][secureinput][securelock]")
{
    for (auto const row: InPaneLockRows)
    {
        INFO("row: " << contour::config::chromeStyleName(row));
        auto scene = SecureLockScene { row };
        scene.setLocked();
        REQUIRE(scene.anchor() == QStringLiteral("topRight"));

        auto* link = scene.host.chrome->findChild<QQuickItem*>(QStringLiteral("linkPill"));
        REQUIRE(link != nullptr);
        scene.session->setLinkTarget(
            QStringLiteral("github.com › contour-terminal"), QStringLiteral("Ctrl+Click to open"), true);
        REQUIRE(QTest::qWaitFor([&] { return link->property("shown").toBool() && link->isVisible(); }));
        CHECK(scene.anchor() == QStringLiteral("bottomRight"));
        CHECK(scene.lock->y() > scene.host.chrome->height() / 2);
        CHECK_FALSE(sceneRectOf(*scene.lock).intersects(sceneRectOf(*link)));

        // The link pill gone (faded out), the lock returns to the top.
        scene.session->setLinkTarget({}, {}, false);
        REQUIRE(QTest::qWaitFor([&] { return !link->isVisible(); }));
        CHECK(scene.anchor() == QStringLiteral("topRight"));
    }
}

TEST_CASE("secure lock pill: it yields to the find bar (offscreen)",
          "[contour][gui][qml][secureinput][securelock]")
{
    for (auto const row: InPaneLockRows)
    {
        INFO("row: " << contour::config::chromeStyleName(row));
        auto scene = SecureLockScene { row };
        scene.setLocked();
        REQUIRE(scene.anchor() == QStringLiteral("topRight"));

        auto* bar = scene.host.chrome->findChild<QObject*>(QStringLiteral("searchBar"));
        REQUIRE(bar != nullptr);
        QMetaObject::invokeMethod(bar, "open");
        QCoreApplication::processEvents();
        REQUIRE(bar->property("visible").toBool());
        // The bar holds the top right: the lock goes below it, whatever the bar's own anchor name.
        CHECK(scene.anchor() == QStringLiteral("bottomRight"));
        CHECK(scene.lock->y() > scene.host.chrome->height() / 2);
    }
}

TEST_CASE("secure lock pill: over a full-screen program it covers the top right, never vanishing (offscreen)",
          "[contour][gui][qml][secureinput][securelock]")
{
    for (auto const row: InPaneLockRows)
    {
        INFO("row: " << contour::config::chromeStyleName(row));
        auto scene = SecureLockScene { row };
        scene.setLocked();
        REQUIRE(scene.lock->isVisible());
        // Every cell painted: no blank place. Secure Keyboard Entry is on, so the lock must show: it covers
        // a few cells at the top right (review I1).
        scene.session->setOverlayClearance(linkClearance(100, 20, 0));
        QCoreApplication::processEvents();
        CHECK(scene.anchor() == QStringLiteral("topRightOver"));
        CHECK(scene.lock->isVisible());
    }
}

TEST_CASE("pane card: the in-pane lock follows the pane's own header, shown or not (offscreen)",
          "[contour][gui][qml][secureinput][securelock][panecard]")
{
    // Review I1: header visibility is decided per pane. Under Minimal density a split's headers show only
    // while the reveal modifier is held; with them hidden, nothing else on screen need hold the lock, so the
    // pane draws it itself -- and drops it again once its header is on screen to carry it.
    auto setup = paneCardSetup();
    setup.density = contour::config::ChromeDensity::Minimal;
    auto host = PaneTreeHost { setup, QSize { 800, 400 } };
    auto panes = TwoPanes {};
    host.show(panes.split, /*visiblePanes*/ 2);
    host.root().setProperty("lockInPane", true); // the window level: no tabs and no band on screen
    panes.leftSession.setOverlayClearance(linkClearance(100, 20, 100));
    panes.leftSession.setSecureIndicator(static_cast<int>(contour::window::SecureIndicator::Locked));
    QCoreApplication::processEvents();

    auto const& card = host.cardOf(panes.left());
    REQUIRE_FALSE(card.property("headerShown").toBool());
    CHECK(PaneTreeHost::partOf(card, "secureInputPill").isVisible());
    CHECK_FALSE(PaneTreeHost::partOf(card, "paneHeaderLock").isVisible());

    host.root().setProperty("headerReveal", static_cast<int>(contour::window::HeaderReveal::HeldModifier));
    QCoreApplication::processEvents();
    REQUIRE(card.property("headerShown").toBool());
    CHECK_FALSE(PaneTreeHost::partOf(card, "secureInputPill").isVisible());
    CHECK(PaneTreeHost::partOf(card, "paneHeaderLock").isVisible());
}

TEST_CASE("pane card: a pane at a password prompt shows the lock and says so in its header (offscreen)",
          "[contour][gui][qml][secureinput][panecard]")
{
    contour::test::QmlMessageCapture const warnings;
    auto setup = paneCardSetup();
    setup.secureInput = contour::platform::SecureInputSupport::Native;
    auto host = PaneTreeHost { setup, QSize { 800, 400 } };
    auto panes = TwoPanes {};
    panes.leftSession.setForegroundProcessName(QStringLiteral("sudo"));
    host.show(panes.split, /*visiblePanes*/ 2);

    auto const& card = host.cardOf(panes.left());
    auto const& lock = PaneTreeHost::partOf(card, "paneHeaderLock");
    auto const& name = PaneTreeHost::partOf(card, "paneHeaderName");
    CHECK_FALSE(lock.isVisible());
    CHECK(name.property("text").toString() == QStringLiteral("sudo"));

    // Locked but not the pane the OS secures (an unfocused split): it hides the typing, and says no more.
    panes.leftSession.setSecureIndicator(static_cast<int>(contour::window::SecureIndicator::Locked));
    QCoreApplication::processEvents();
    CHECK(lock.isVisible());
    CHECK(name.property("text").toString() == QStringLiteral("sudo · Input hidden"));
    CHECK(accessibleNameOf(lock) == QStringLiteral("Input hidden"));

    panes.leftSession.setSecureIndicator(static_cast<int>(contour::window::SecureIndicator::LockedSecured));
    QCoreApplication::processEvents();
    CHECK(lock.isVisible());
    CHECK(name.property("text").toString() == QStringLiteral("sudo · Secure input on"));
    CHECK(accessibleNameOf(lock) == QStringLiteral("Secure input on"));
    // After the lock, by the header's gap: the lock never covers the name.
    CHECK(name.x() >= lock.x() + lock.width());
    // The other pane is not locked.
    CHECK_FALSE(PaneTreeHost::partOf(host.cardOf(panes.right()), "paneHeaderLock").isVisible());

    panes.leftSession.setSecureIndicator(static_cast<int>(contour::window::SecureIndicator::None));
    QCoreApplication::processEvents();
    CHECK_FALSE(lock.isVisible());
    CHECK(name.property("text").toString() == QStringLiteral("sudo"));
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("KitSwitch: the style's extent, Space and Return toggle, a non-editable one does not (offscreen)",
          "[contour][gui][qml][kit][switch]")
{
    using contour::config::ChromeStyle;
    auto const row = GENERATE(ChromeStyle::MacOS, ChromeStyle::Terminal);
    QQmlEngine engine;
    contour::test::QmlMessageCapture const warnings;
    contour::test::installChromeStyle(engine, kitRowSetup(row));

    auto kit = KitControl(engine, "KitSwitch", { { "name", QStringLiteral("Reflow") } });
    auto* item = kit.item();
    CHECK(item->width() == chromeToken(engine, "settings", "switchWidth"));
    CHECK(item->height() == chromeToken(engine, "settings", "switchHeight"));
    if (row == ChromeStyle::MacOS)
    {
        CHECK(item->width() == 32);
        CHECK(item->height() == 19);
    }

    QSignalSpy const toggled(item, SIGNAL(toggled(bool)));
    kit.key(Qt::Key_Space);
    REQUIRE(toggled.count() == 1);
    CHECK(toggled.at(0).at(0).toBool());
    kit.key(Qt::Key_Return);
    REQUIRE(toggled.count() == 2);
    CHECK(toggled.at(1).at(0).toBool()); // `checked` is the caller's: still off, so the ask is still "on"

    item->setProperty("checked", true);
    kit.key(Qt::Key_Space);
    REQUIRE(toggled.count() == 3);
    CHECK_FALSE(toggled.at(2).at(0).toBool());

    SECTION("not editable: disabled, dimmed, deaf")
    {
        item->setProperty("editable", false);
        item->forceActiveFocus();
        kit.key(Qt::Key_Space);
        CHECK(toggled.count() == 3);
        CHECK_FALSE(item->isEnabled());
        CHECK(item->opacity() == Catch::Approx(chromeToken(engine, "settings", "disabledOpacity")));
    }

    SECTION("the Terminal row draws the glyph of the state")
    {
        auto const glyph = kit.parts("switchGlyph");
        if (row == ChromeStyle::Terminal)
        {
            REQUIRE(glyph.size() == 1);
            CHECK(glyph.front()->property("text").toString() == QStringLiteral("[x]"));
            item->setProperty("checked", false);
            CHECK(glyph.front()->property("text").toString() == QStringLiteral("[ ]"));
        }
        else
        {
            CHECK_FALSE(glyph.front()->isVisible());
        }
    }
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("KitSwitch: the knob does not animate under Reduce Motion (offscreen)",
          "[contour][gui][qml][kit][switch]")
{
    using contour::config::MotionPreference;
    auto const motion = GENERATE(MotionPreference::Full, MotionPreference::Reduced);
    QQmlEngine engine;
    contour::test::QmlMessageCapture const warnings;
    auto setup = kitRowSetup(contour::config::ChromeStyle::MacOS);
    setup.accessibility.motion = motion;
    contour::test::installChromeStyle(engine, setup);

    auto kit = KitControl(engine, "KitSwitch", { { "name", QStringLiteral("Reflow") } });
    auto* knob = kit.part("switchKnob");
    auto const off = knob->x();
    kit.item()->setProperty("checked", true);
    auto const target = kit.item()->width() - knob->width() - off;
    if (motion == MotionPreference::Reduced)
    {
        // The knob is where it ends at once.
        CHECK(knob->x() == target);
    }
    else
    {
        // The knob is on its way: still at the start, and then there.
        CHECK(knob->x() != target);
        CHECK(QTest::qWaitFor([&] { return knob->x() == target; }, contour::test::PopupSettleMs));
    }
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE(
    "KitSegmented: the arrows pick the neighbour, the Terminal row marks the chosen segment (offscreen)",
    "[contour][gui][qml][kit][segmented]")
{
    using contour::config::ChromeStyle;
    auto const row = GENERATE(ChromeStyle::MacOS, ChromeStyle::Terminal);
    QQmlEngine engine;
    contour::test::QmlMessageCapture const warnings;
    contour::test::installChromeStyle(engine, kitRowSetup(row));

    auto kit = KitControl(engine,
                          "KitSegmented",
                          { { "options", QStringList { "Off", "Left", "Right", "Both" } },
                            { "labels", QStringList { "Off", "Left", "Right", "Both" } },
                            { "value", QStringLiteral("Off") },
                            { "name", QStringLiteral("Use Option as Meta") } });
    auto* item = kit.item();
    QSignalSpy const picked(item, SIGNAL(picked(QString)));

    kit.key(Qt::Key_Right);
    REQUIRE(picked.count() == 1);
    CHECK(picked.at(0).at(0).toString() == QStringLiteral("Left"));
    kit.key(Qt::Key_Left); // at the start: nothing before "Off"
    CHECK(picked.count() == 1);

    item->setProperty("value", QStringLiteral("Both"));
    kit.key(Qt::Key_Right); // at the end
    CHECK(picked.count() == 1);
    kit.key(Qt::Key_Left);
    REQUIRE(picked.count() == 2);
    CHECK(picked.at(1).at(0).toString() == QStringLiteral("Right"));

    auto const labels = kit.parts("segmentLabel");
    REQUIRE(labels.size() == 4);
    item->setProperty("value", QStringLiteral("Left"));
    if (row == ChromeStyle::Terminal)
    {
        CHECK(labels[1]->property("text").toString().startsWith(QStringLiteral("(•)")));
        CHECK(labels[0]->property("text").toString().startsWith(QStringLiteral("( )")));
        CHECK(labels[3]->property("text").toString().startsWith(QStringLiteral("( )")));
    }
    else
    {
        CHECK(labels[1]->property("text").toString() == QStringLiteral("Left"));
    }

    SECTION("not editable: deaf")
    {
        item->setProperty("editable", false);
        kit.key(Qt::Key_Right);
        CHECK(picked.count() == 2);
    }
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("KitStepper: the field holds the widest value a setting shows in every row (offscreen)",
          "[contour][gui][qml][kit][stepper]")
{
    using contour::config::ChromeStyle;
    auto const row =
        GENERATE(ChromeStyle::MacOS, ChromeStyle::Fluent, ChromeStyle::ContourLinux, ChromeStyle::Terminal);
    CAPTURE(static_cast<int>(row));
    QQmlEngine engine;
    contour::test::QmlMessageCapture const warnings;
    contour::test::installChromeStyle(engine, kitRowSetup(row));

    // The PTY read buffer size's default: the widest whole number the Settings window ships a stepper for.
    auto kit = KitControl(engine,
                          "KitStepper",
                          { { "value", 65536.0 },
                            { "from", 0.0 },
                            { "to", 1048576.0 },
                            { "name", QStringLiteral("PTY read buffer size") } });
    auto* field = kit.part("stepperField");
    REQUIRE(field->property("text").toString() == QStringLiteral("65536"));
    auto const room =
        field->width() - field->property("leftPadding").toReal() - field->property("rightPadding").toReal();
    // The Terminal row's chrome font is the terminal's: one cell per digit, which the test's own fallback
    // font does not give, so the digits' width is the text's cells there.
    auto const digits = static_cast<qreal>(field->property("text").toString().size());
    auto const cellWidth = engine.rootContext()
                               ->contextProperty(QStringLiteral("chromeStyle"))
                               .value<QObject*>()
                               ->property("cellWidth")
                               .toReal();
    auto const needed =
        row == ChromeStyle::Terminal ? digits * cellWidth : field->property("contentWidth").toReal();
    CHECK(needed <= room);
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("KitStepper: the arrows and the keys step, the range holds, typing commits (offscreen)",
          "[contour][gui][qml][kit][stepper]")
{
    using contour::config::ChromeStyle;
    auto const row = GENERATE(ChromeStyle::MacOS, ChromeStyle::Terminal);
    QQmlEngine engine;
    contour::test::QmlMessageCapture const warnings;
    contour::test::installChromeStyle(engine, kitRowSetup(row));

    auto kit = KitControl(engine,
                          "KitStepper",
                          { { "value", 12.0 },
                            { "from", 6.0 },
                            { "to", 13.0 },
                            { "stepSize", 0.5 },
                            { "decimals", 1 },
                            { "unitText", QStringLiteral("pt") },
                            { "name", QStringLiteral("Font size") } });
    auto* item = kit.item();
    QSignalSpy const committed(item, SIGNAL(committed(qreal)));
    auto* field = kit.part("stepperField");
    CHECK(field->property("text").toString() == QStringLiteral("12.0"));

    kit.key(Qt::Key_Up);
    REQUIRE(committed.count() == 1);
    CHECK(committed.at(0).at(0).toReal() == 12.5);

    item->setProperty("value", 13.0);
    kit.key(Qt::Key_Up); // at `to`
    CHECK(committed.count() == 1);
    kit.key(Qt::Key_Down);
    REQUIRE(committed.count() == 2);
    CHECK(committed.at(1).at(0).toReal() == 12.5);

    SECTION("the arrows press like the keys")
    {
        item->setProperty("value", 12.0);
        auto* up = kit.part("stepperUp");
        QTest::mouseClick(&kit.window,
                          Qt::LeftButton,
                          Qt::NoModifier,
                          up->mapToScene({ up->width() / 2, up->height() / 2 }).toPoint());
        QCoreApplication::processEvents();
        REQUIRE(committed.count() == 3);
        CHECK(committed.at(2).at(0).toReal() == 12.5);
    }

    SECTION("a typed number commits on Return, held to the range")
    {
        item->setProperty("value", 12.0);
        field->setProperty("text", QStringLiteral("99"));
        field->forceActiveFocus();
        kit.key(Qt::Key_Return);
        REQUIRE(committed.count() == 3);
        CHECK(committed.at(2).at(0).toReal() == 13.0);
        CHECK(field->property("text").toString() == QStringLiteral("12.0")); // the caller's value again
    }

    SECTION("text that is no number puts the value back")
    {
        field->setProperty("text", QStringLiteral("twelve"));
        field->forceActiveFocus();
        kit.key(Qt::Key_Return);
        CHECK(committed.count() == 2);
        CHECK(field->property("text").toString() == QStringLiteral("13.0"));
    }

    SECTION("not editable: deaf")
    {
        item->setProperty("editable", false);
        kit.key(Qt::Key_Up);
        CHECK(committed.count() == 2);
        CHECK_FALSE(item->isEnabled());
    }

    if (row == ChromeStyle::Terminal)
    {
        auto const up = kit.parts("stepperUp");
        REQUIRE(up.size() == 1);
        auto* glyph = up.front()->childItems().front();
        CHECK(glyph->property("text").toString() == QStringLiteral("▴"));
    }
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("KitSlider: it shows the formatted value, and a key step commits at once (offscreen)",
          "[contour][gui][qml][kit][slider]")
{
    using contour::config::ChromeStyle;
    auto const row = GENERATE(ChromeStyle::MacOS, ChromeStyle::Terminal);
    QQmlEngine engine;
    contour::test::QmlMessageCapture const warnings;
    contour::test::installChromeStyle(engine, kitRowSetup(row));

    auto kit = KitControl(engine,
                          "KitSlider",
                          { { "value", 0.15 },
                            { "from", 0.0 },
                            { "to", 1.0 },
                            { "stepSize", 0.01 },
                            { "displayFactor", 100.0 },
                            { "unitText", QStringLiteral("%") },
                            { "name", QStringLiteral("Dim inactive panes") } });
    auto* item = kit.item();
    CHECK(kit.part("sliderValue")->property("text").toString() == QStringLiteral("15 %"));

    QSignalSpy const committed(item, SIGNAL(committed(qreal)));
    kit.key(Qt::Key_Right);
    REQUIRE(committed.count() == 1);
    CHECK(committed.at(0).at(0).toReal() == Catch::Approx(0.16));
    kit.key(Qt::Key_Down);
    REQUIRE(committed.count() == 2);
    CHECK(committed.at(1).at(0).toReal() == Catch::Approx(0.14));

    item->setProperty("value", 1.0);
    kit.key(Qt::Key_Up); // at `to`
    CHECK(committed.count() == 2);

    SECTION("the Terminal row draws the track in cells")
    {
        item->setProperty("value", 0.4);
        auto* glyphs = kit.part("sliderGlyphs");
        if (row == ChromeStyle::Terminal)
        {
            CHECK(glyphs->property("text").toString() == QStringLiteral("━━━━●──────"));
        }
        else
        {
            CHECK_FALSE(glyphs->isVisible());
        }
    }

    SECTION("a release asks once, for where the knob was let go")
    {
        auto* track = kit.part("sliderTrack");
        item->setProperty("value", 0.0);
        auto const at = [&](qreal share) {
            return track->mapToScene({ track->width() * share, track->height() / 2 }).toPoint();
        };
        QTest::mousePress(&kit.window, Qt::LeftButton, Qt::NoModifier, at(0.5));
        QTest::mouseMove(&kit.window, at(0.75));
        CHECK(committed.count() == 2);
        QTest::mouseRelease(&kit.window, Qt::LeftButton, Qt::NoModifier, at(0.75));
        REQUIRE(committed.count() == 3);
        CHECK(committed.at(2).at(0).toReal() == Catch::Approx(0.75).margin(0.02));
    }

    SECTION("not editable: deaf")
    {
        item->setProperty("value", 0.5);
        item->setProperty("editable", false);
        kit.key(Qt::Key_Right);
        CHECK(committed.count() == 2);
    }
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("KitShortcutField: shows none or the chord, and a screen reader hears it with the name (offscreen)",
          "[contour][gui][qml][kit][shortcut]")
{
    using contour::config::ChromeStyle;
    auto const row = GENERATE(ChromeStyle::MacOS, ChromeStyle::Terminal);
    QQmlEngine engine;
    contour::test::QmlMessageCapture const warnings;
    contour::test::installChromeStyle(engine, kitRowSetup(row));

    auto kit = KitControl(engine, "KitShortcutField", { { "name", QStringLiteral("Hotkey") } });
    auto* item = kit.item();
    CHECK(kit.part("shortcutCaption")->property("text").toString() == QStringLiteral("None"));
    CHECK(accessibleNameOf(*item) == QStringLiteral("Hotkey, None"));

    item->setProperty("chord", QStringLiteral("Ctrl+`"));
    CHECK(kit.part("shortcutCaption")->property("text").toString() == QStringLiteral("Ctrl+`"));
    CHECK(accessibleNameOf(*item) == QStringLiteral("Hotkey, Ctrl+`"));
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("KitShortcutField: records the next chord, Escape cancels, Delete clears (offscreen)",
          "[contour][gui][qml][kit][shortcut]")
{
    QQmlEngine engine;
    contour::test::QmlMessageCapture const warnings;
    contour::test::installChromeStyle(engine, kitRowSetup(contour::config::ChromeStyle::MacOS));

    auto kit = KitControl(engine,
                          "KitShortcutField",
                          { { "chord", QStringLiteral("Ctrl+`") }, { "name", QStringLiteral("Hotkey") } });
    auto* item = kit.item();
    QSignalSpy const recorded(item, SIGNAL(recorded(int, int)));
    QSignalSpy const cleared(item, SIGNAL(cleared()));
    auto const recording = [&] {
        return item->property("recording").toBool();
    };

    // Nothing is recorded until asked: a chord pressed on the idle field is no one's.
    kit.key(Qt::Key_A);
    CHECK(recorded.count() == 0);
    CHECK_FALSE(recording());

    kit.key(Qt::Key_Return);
    CHECK(recording());
    CHECK(kit.part("shortcutCaption")->property("text").toString() != QStringLiteral("Ctrl+`"));

    // A modifier alone starts a chord, it is not one.
    kit.key(Qt::Key_Control);
    CHECK(recording());
    CHECK(recorded.count() == 0);

    QTest::keyClick(&kit.window, Qt::Key_QuoteLeft, Qt::ControlModifier | Qt::AltModifier);
    QCoreApplication::processEvents();
    REQUIRE(recorded.count() == 1);
    CHECK(recorded.at(0).at(0).toInt() == static_cast<int>(Qt::Key_QuoteLeft));
    CHECK(recorded.at(0).at(1).toInt() == static_cast<int>(Qt::ControlModifier | Qt::AltModifier));
    CHECK_FALSE(recording());

    kit.key(Qt::Key_Return);
    kit.key(Qt::Key_Escape);
    CHECK_FALSE(recording());
    CHECK(recorded.count() == 1);
    CHECK(cleared.count() == 0);

    kit.key(Qt::Key_Return);
    kit.key(Qt::Key_Delete);
    CHECK_FALSE(recording());
    CHECK(cleared.count() == 1);
    CHECK(recorded.count() == 1);

    SECTION("a chord with Escape or Delete in it is a chord")
    {
        kit.key(Qt::Key_Return);
        QTest::keyClick(&kit.window, Qt::Key_Escape, Qt::ControlModifier);
        QCoreApplication::processEvents();
        CHECK(recorded.count() == 2);
        CHECK(cleared.count() == 1);
    }

    SECTION("not editable: deaf")
    {
        item->setProperty("editable", false);
        CHECK_FALSE(item->isEnabled());
        kit.key(Qt::Key_Return);
        CHECK_FALSE(recording());
    }
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("KitColorWell: a screen reader hears the readout, verdict included",
          "[contour][gui][qml][kit][colorwell]")
{
    QQmlEngine engine;
    contour::test::QmlMessageCapture const warnings;
    contour::test::installChromeStyle(engine, kitRowSetup(contour::config::ChromeStyle::MacOS));

    auto kit = KitControl(engine,
                          "KitColorWell",
                          { { "hex", QStringLiteral("#777777") },
                            { "readout", QStringLiteral("4.47 : 1, fails") },
                            { "readoutFails", true },
                            { "description", QStringLiteral("Background") },
                            { "name", QStringLiteral("Tab colour") } });
    auto* readout = kit.part("colorReadout");
    CHECK(accessibleNameOf(*readout) == QStringLiteral("4.47 : 1, fails"));
    auto const* attached = accessibleOf(kit.item());
    REQUIRE(attached != nullptr);
    CHECK(attached->property("description").toString().contains(QStringLiteral("fails")));
    CHECK(attached->property("description").toString().contains(QStringLiteral("Background")));
}

TEST_CASE("KitColorWell: only #rgb and #rrggbb commit, anything else puts the old hex back (offscreen)",
          "[contour][gui][qml][kit][colorwell]")
{
    using contour::config::ChromeStyle;
    auto const row = GENERATE(ChromeStyle::MacOS, ChromeStyle::Terminal);
    QQmlEngine engine;
    contour::test::QmlMessageCapture const warnings;
    contour::test::installChromeStyle(engine, kitRowSetup(row));

    auto kit = KitControl(engine,
                          "KitColorWell",
                          { { "hex", QStringLiteral("#112233") },
                            { "readout", QStringLiteral("7.1 : 1") },
                            { "name", QStringLiteral("Tab colour") } });
    auto* item = kit.item();
    QSignalSpy const committed(item, SIGNAL(committed(QString)));
    auto* field = kit.part("colorHexField");
    CHECK(field->property("text").toString() == QStringLiteral("#112233"));
    CHECK(kit.part("colorReadout")->property("text").toString() == QStringLiteral("7.1 : 1"));

    auto const type = [&](QString const& text) {
        field->setProperty("text", text);
        field->forceActiveFocus();
        kit.key(Qt::Key_Return);
    };

    type(QStringLiteral("#12345"));
    CHECK(committed.count() == 0);
    CHECK(field->property("text").toString() == QStringLiteral("#112233"));

    type(QStringLiteral("123456"));
    CHECK(committed.count() == 0);

    type(QStringLiteral("#123456"));
    REQUIRE(committed.count() == 1);
    CHECK(committed.at(0).at(0).toString() == QStringLiteral("#123456"));

    type(QStringLiteral("#ABC"));
    REQUIRE(committed.count() == 2);
    CHECK(committed.at(1).at(0).toString() == QStringLiteral("#aabbcc"));

    type(QStringLiteral("#112233")); // what it already is
    CHECK(committed.count() == 2);

    SECTION("the swatch is the colour")
    {
        if (row == ChromeStyle::Terminal)
            CHECK(kit.part("colorWellGlyph")->property("color").value<QColor>() == QColor(0x11, 0x22, 0x33));
        else
            CHECK(kit.part("colorWellSwatch")->property("color").value<QColor>() == QColor(0x11, 0x22, 0x33));
    }

    SECTION("not editable: deaf")
    {
        item->setProperty("editable", false);
        CHECK_FALSE(item->isEnabled());
        CHECK_FALSE(field->isEnabled());
    }
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("KitColorWell: Return commits the colour shown only where asked to, leaving the field never does",
          "[contour][gui][qml][kit][colorwell]")
{
    QQmlEngine engine;
    contour::test::QmlMessageCapture const warnings;
    contour::test::installChromeStyle(engine, kitRowSetup(contour::config::ChromeStyle::MacOS));

    auto const commitsFor = [&](bool commitsUnchanged, auto const& leave) {
        auto kit = KitControl(engine,
                              "KitColorWell",
                              { { "hex", QStringLiteral("#112233") },
                                { "commitsUnchanged", commitsUnchanged },
                                { "name", QStringLiteral("Derived") } });
        QSignalSpy const committed(kit.item(), SIGNAL(committed(QString)));
        kit.part("colorHexField")->forceActiveFocus();
        leave(kit);
        return committed.count();
    };
    auto const pressReturn = [](KitControl& kit) {
        kit.key(Qt::Key_Return);
    };
    auto const dropFocus = [](KitControl& kit) {
        kit.part("colorHexField")->setFocus(false);
    };

    CHECK(commitsFor(true, pressReturn) == 1);
    CHECK(commitsFor(false, pressReturn) == 0);
    CHECK(commitsFor(true, dropFocus) == 0);
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("KitFontPicker: the search narrows the families, Escape closes the list (offscreen)",
          "[contour][gui][qml][kit][fontpicker]")
{
    using contour::config::ChromeStyle;
    auto const row = GENERATE(ChromeStyle::MacOS, ChromeStyle::Terminal);
    QQmlEngine engine;
    contour::test::QmlMessageCapture const warnings;
    contour::test::installChromeStyle(engine, kitRowSetup(row));

    auto kit = KitControl(engine,
                          "KitFontPicker",
                          { { "family", QStringLiteral("Iosevka") },
                            { "families", QStringList { "JetBrains Mono", "Iosevka", "Fira Code" } },
                            { "name", QStringLiteral("Font family") } });
    auto* item = kit.item();
    QSignalSpy const picked(item, SIGNAL(picked(QString)));
    CHECK(kit.part("popUpCaption")->property("text").toString() == QStringLiteral("Iosevka"));

    auto* popup = item->findChild<QObject*>(QStringLiteral("popUpPopup"));
    REQUIRE(popup != nullptr);
    CHECK_FALSE(popup->property("visible").toBool());
    REQUIRE(QMetaObject::invokeMethod(item, "open"));
    contour::test::waitForPopupOpened(*popup);

    auto* search = popup->findChild<QObject*>(QStringLiteral("popUpSearch"));
    auto* list = popup->findChild<QQuickItem*>(QStringLiteral("popUpList"));
    REQUIRE(search != nullptr);
    REQUIRE(list != nullptr);
    CHECK(list->property("count").toInt() == 3);

    search->setProperty("text", QStringLiteral("jet"));
    QCoreApplication::processEvents();
    REQUIRE(list->property("count").toInt() == 1);
    auto const labels = [&] {
        auto found = QStringList {};
        for (auto* child: list->property("contentItem").value<QQuickItem*>()->childItems())
            if (child->objectName() == QLatin1String("popUpRow"))
                found << child->findChild<QObject*>(QStringLiteral("popUpRowLabel"))
                             ->property("text")
                             .toString();
        return found;
    }();
    CHECK(labels == QStringList { "JetBrains Mono" });

    SECTION("Return chooses the row the search left")
    {
        search->setProperty("focus", true);
        kit.key(Qt::Key_Return);
        REQUIRE(picked.count() == 1);
        CHECK(picked.at(0).at(0).toString() == QStringLiteral("JetBrains Mono"));
        contour::test::waitForPopupClosed(*popup);
    }

    SECTION("Escape closes it without choosing")
    {
        kit.key(Qt::Key_Escape);
        contour::test::waitForPopupClosed(*popup);
        CHECK(picked.count() == 0);
    }
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("KitPopUp: it shows the chosen label, the labels stand for the values, a pick asks (offscreen)",
          "[contour][gui][qml][kit][popup]")
{
    QQmlEngine engine;
    contour::test::QmlMessageCapture const warnings;
    contour::test::installChromeStyle(engine, kitRowSetup(contour::config::ChromeStyle::MacOS));

    auto kit = KitControl(engine,
                          "KitPopUp",
                          { { "options", QStringList { "auto", "vulkan", "metal" } },
                            { "labels", QStringList { "Automatic", "Vulkan", "Metal" } },
                            { "value", QStringLiteral("vulkan") },
                            { "name", QStringLiteral("Renderer") } });
    auto* item = kit.item();
    QSignalSpy const picked(item, SIGNAL(picked(QString)));
    CHECK(kit.part("popUpCaption")->property("text").toString() == QStringLiteral("Vulkan"));

    auto* popup = item->findChild<QObject*>(QStringLiteral("popUpPopup"));
    REQUIRE(popup != nullptr);
    kit.key(Qt::Key_Down); // opens
    contour::test::waitForPopupOpened(*popup);
    auto* list = popup->findChild<QQuickItem*>(QStringLiteral("popUpList"));
    REQUIRE(list != nullptr);
    CHECK(list->property("count").toInt() == 3);
    CHECK(list->property("currentIndex").toInt() == 1); // the chosen row
    kit.key(Qt::Key_Down);
    kit.key(Qt::Key_Return);
    REQUIRE(picked.count() == 1);
    CHECK(picked.at(0).at(0).toString() == QStringLiteral("metal"));
    contour::test::waitForPopupClosed(*popup);
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("KitFontPicker: the list fits the longest family, or scrolls to it, never elides (offscreen)",
          "[contour][gui][qml][kit][fontpicker]")
{
    using contour::config::ChromeStyle;
    auto const row =
        GENERATE(ChromeStyle::MacOS, ChromeStyle::Fluent, ChromeStyle::ContourLinux, ChromeStyle::Terminal);
    QQmlEngine engine;
    contour::test::QmlMessageCapture const warnings;
    contour::test::installChromeStyle(engine, kitRowSetup(row));

    auto families = QStringList {};
    for (auto const index: std::views::iota(0, 300))
        families << QStringLiteral("Family %1").arg(index);
    auto const longName = QStringLiteral("Noto Sans CJK Extra Condensed Display Semi-Bold Italic Variable");
    auto const wayTooLong = QString(300, QLatin1Char('W'));
    families << longName << wayTooLong;

    auto kit = KitControl(
        engine,
        "KitFontPicker",
        { { "family", longName }, { "families", families }, { "name", QStringLiteral("Font family") } });
    auto* item = kit.item();
    auto* caption = kit.part("popUpCaption");
    REQUIRE(caption != nullptr);
    CHECK(caption->property("implicitWidth").toReal() <= caption->property("width").toReal());

    auto* popup = item->findChild<QObject*>(QStringLiteral("popUpPopup"));
    REQUIRE(popup != nullptr);
    REQUIRE(QMetaObject::invokeMethod(item, "open"));
    contour::test::waitForPopupOpened(*popup);
    auto* search = popup->findChild<QObject*>(QStringLiteral("popUpSearch"));
    auto* list = popup->findChild<QQuickItem*>(QStringLiteral("popUpList"));
    REQUIRE(search != nullptr);
    REQUIRE(list != nullptr);

    // The longest family that still fits the window sets the list's width...
    search->setProperty("text", QStringLiteral("Noto"));
    QCoreApplication::processEvents();
    auto const findLabel = [&](QString const& text) -> QQuickItem* {
        auto* content = list->property("contentItem").value<QQuickItem*>();
        for (auto* child: content->childItems())
            if (child->objectName() == QLatin1String("popUpRow"))
                if (auto* label = child->findChild<QQuickItem*>(QStringLiteral("popUpRowLabel"));
                    label->property("text").toString() == text)
                    return label;
        return nullptr;
    };
    REQUIRE(QTest::qWaitFor([&] { return findLabel(longName) != nullptr; }, 2000));
    auto const* label = findLabel(longName);
    CHECK(label->property("implicitWidth").toReal() <= label->width());
    CHECK(popup->property("width").toReal() <= kit.window.width());

    // ...and one wider than the window scrolls to it: the row is as wide as the text, and nothing elides.
    search->setProperty("text", QStringLiteral("WWWW"));
    QCoreApplication::processEvents();
    REQUIRE(QTest::qWaitFor([&] { return findLabel(wayTooLong) != nullptr; }, 2000));
    label = findLabel(wayTooLong);
    CHECK(label->property("implicitWidth").toReal() <= label->width());
    CHECK(label->property("elide").toInt() == static_cast<int>(Qt::ElideNone));
    CHECK(popup->property("width").toReal() <= kit.window.width());
    CHECK(list->property("contentWidth").toReal() >= label->property("implicitWidth").toReal());
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("KitPopUp: the button and its list fit every option, not just the chosen one (offscreen)",
          "[contour][gui][qml][kit][popup]")
{
    using contour::config::ChromeStyle;
    auto const row =
        GENERATE(ChromeStyle::MacOS, ChromeStyle::Fluent, ChromeStyle::ContourLinux, ChromeStyle::Terminal);
    QQmlEngine engine;
    contour::test::QmlMessageCapture const warnings;
    contour::test::installChromeStyle(engine, kitRowSetup(row));

    // The shortest option is the chosen one: sizing to it alone cuts the others off ("Alw").
    auto kit =
        KitControl(engine,
                   "KitPopUp",
                   { { "options", QStringList { "never", "always", "underline", "dotted-wavy" } },
                     { "labels", QStringList { "No", "Always", "Underlined text", "Dotted wavy line" } },
                     { "value", QStringLiteral("never") },
                     { "name", QStringLiteral("Mode") } });
    auto* item = kit.item();
    auto* caption = kit.part("popUpCaption");
    REQUIRE(caption != nullptr);
    CHECK(caption->property("text").toString() == QStringLiteral("No"));

    auto const widest = [&](QQuickItem const& labelItem) {
        auto const font = labelItem.property("font").value<QFont>();
        auto const metrics = QFontMetricsF(font);
        auto result = 0.0;
        for (auto const& label: { "No", "Always", "Underlined text", "Dotted wavy line" })
            result = std::max(result, metrics.horizontalAdvance(QString::fromLatin1(label)));
        return result;
    };
    CHECK(caption->property("width").toReal() >= widest(*qobject_cast<QQuickItem*>(caption)));

    // Choosing the longest one changes nothing about the width: the button never jumps.
    auto const before = item->width();
    item->setProperty("value", QStringLiteral("dotted-wavy"));
    QCoreApplication::processEvents();
    CHECK(item->width() == before);
    CHECK(caption->property("width").toReal() >= caption->property("implicitWidth").toReal());

    auto* popup = item->findChild<QObject*>(QStringLiteral("popUpPopup"));
    REQUIRE(popup != nullptr);
    REQUIRE(QMetaObject::invokeMethod(item, "open"));
    contour::test::waitForPopupOpened(*popup);
    auto* list = popup->findChild<QQuickItem*>(QStringLiteral("popUpList"));
    REQUIRE(list != nullptr);
    auto const collectRows = [&] {
        auto found = std::vector<QQuickItem*> {};
        for (auto* child: list->property("contentItem").value<QQuickItem*>()->childItems())
            if (child->objectName() == QLatin1String("popUpRow"))
                found.push_back(child);
        return found;
    };
    REQUIRE(QTest::qWaitFor([&] { return collectRows().size() == 4; }, 2000));
    auto const rows = collectRows();
    CHECK(popup->property("width").toReal() >= item->width());
    for (auto* rowItem: rows)
    {
        auto* label = rowItem->findChild<QQuickItem*>(QStringLiteral("popUpRowLabel"));
        REQUIRE(label != nullptr);
        INFO("row label: " << label->property("text").toString().toStdString());
        CHECK(label->property("implicitWidth").toReal() <= label->width());
    }
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("kit controls: every ink reads on what it sits on, on every row, scheme and appearance (offscreen)",
          "[contour][gui][qml][kit][contrast]")
{
    using contour::config::ChromeStyle;
    auto const appearance = GENERATE(vtbackend::ColorPreference::Light, vtbackend::ColorPreference::Dark);
    // The three native rows once; the Terminal row once for every bundled scheme.
    auto const rowName = GENERATE(from_range(kitRowNames()));
    INFO("appearance: " << appearanceName(appearance) << ", row: " << rowName);
    contour::test::ScopedChromeAppearance const forced { appearance };
    QQmlEngine engine;
    contour::test::QmlMessageCapture const warnings;
    auto setup = contour::test::defaultChromeSetup();
    if (rowName == "macos")
        setup.row = ChromeStyle::MacOS;
    else if (rowName == "fluent")
        setup.row = ChromeStyle::Fluent;
    else if (rowName == "linux")
        setup.row = ChromeStyle::ContourLinux;
    else
        setup = terminalRowSetup(rowName);
    contour::test::installChromeStyle(engine, setup);
    auto const isTerminal = setup.row == ChromeStyle::Terminal;

    // What the controls are laid on: the group's fill over the page's over the window.
    auto const window = QGuiApplication::palette().color(QPalette::Window);
    auto const ground =
        kitOver(kitToken(engine, "settingsGroupFill"), kitOver(kitToken(engine, "settingsPageFill"), window));
    auto const controlFill = kitOver(kitToken(engine, "controlFill"), ground);
    auto const colorOf = [](QObject const* object) {
        return object->property("color").value<QColor>();
    };
    auto const borderOf = [](QObject const* object) {
        return object->property("border").value<QObject*>()->property("color").value<QColor>();
    };
    constexpr auto TextContrast = 4.5;
    constexpr auto MarkContrast = 3.0;

    SECTION("switch")
    {
        for (auto const checked: { false, true })
        {
            auto kit =
                KitControl(engine, "KitSwitch", { { "checked", checked }, { "name", QStringLiteral("x") } });
            if (isTerminal)
            {
                CHECK(kitContrast(colorOf(kit.part("switchGlyph")), ground) >= TextContrast);
            }
            else
            {
                auto const track = kitOver(colorOf(kit.part("switchTrack")), ground);
                CHECK(kitContrast(track, ground) >= MarkContrast);
                CHECK(kitContrast(colorOf(kit.part("switchKnob")), track) >= MarkContrast);
                CHECK(kitContrast(borderOf(kit.part("switchFocusRing")), ground) >= MarkContrast);
            }
        }
    }

    SECTION("segmented")
    {
        auto kit = KitControl(engine,
                              "KitSegmented",
                              { { "options", QStringList { "a", "b" } },
                                { "value", QStringLiteral("a") },
                                { "name", QStringLiteral("x") } });
        auto const segments = kit.parts("segment");
        auto const labels = kit.parts("segmentLabel");
        REQUIRE(segments.size() == 2);
        REQUIRE(labels.size() == 2);
        // The chosen segment sits on the selection (the shape rows), the other on the control's fill. On the
        // native rows the selection is the OS's own highlight pair, whose contrast is the OS's (ruling of
        // Task 8), so only the Terminal row's, which the scheme decides, is held to 4.5: its chosen label
        // is lit only while focused, and then reads like the selection does.
        if (isTerminal)
            CHECK(kitContrast(colorOf(labels[0]), controlFill) >= TextContrast);
        CHECK(kitContrast(colorOf(labels[1]), controlFill) >= TextContrast);
        // The ring around the whole control, on the group.
        CHECK(kitContrast(borderOf(kit.item()->property("background").value<QObject*>()), ground)
              >= MarkContrast);
    }

    SECTION("stepper, colour well and pop-up fields")
    {
        auto stepper = KitControl(
            engine, "KitStepper", { { "name", QStringLiteral("x") }, { "unitText", QStringLiteral("pt") } });
        CHECK(kitContrast(colorOf(stepper.part("stepperField")), controlFill) >= TextContrast);
        CHECK(kitContrast(colorOf(stepper.part("stepperUnit")), ground) >= TextContrast);
        for (auto* arrow: stepper.parts("stepperUp"))
        {
            auto const inlineArrows = stepper.item()->property("_inline").toBool();
            CHECK(kitContrast(colorOf(arrow->childItems().front()), inlineArrows ? controlFill : ground)
                  >= TextContrast);
        }

        auto well = KitControl(engine,
                               "KitColorWell",
                               { { "hex", QStringLiteral("#336699") },
                                 { "readout", QStringLiteral("5 : 1") },
                                 { "name", QStringLiteral("x") } });
        CHECK(kitContrast(colorOf(well.part("colorHexField")), controlFill) >= TextContrast);
        CHECK(kitContrast(colorOf(well.part("colorReadout")), ground) >= TextContrast);
        // A readout under the rule is in the danger ink, which reads as well (on a Terminal row it may be the
        // scheme's own help ink, where that reads).
        auto failing = KitControl(engine,
                                  "KitColorWell",
                                  { { "hex", QStringLiteral("#336699") },
                                    { "readout", QStringLiteral("2.1 : 1") },
                                    { "readoutFails", true },
                                    { "name", QStringLiteral("x") } });
        CHECK(kitContrast(colorOf(failing.part("colorReadout")), ground) >= TextContrast);

        auto shortcut =
            KitControl(engine,
                       "KitShortcutField",
                       { { "chord", QStringLiteral("Ctrl+`") }, { "name", QStringLiteral("x") } });
        CHECK(kitContrast(colorOf(shortcut.part("shortcutCaption")), controlFill) >= TextContrast);
        // The Record button's label, as the style draws it: the colour of its content item.
        auto const* recordContent =
            shortcut.part("shortcutRecordButton")->property("contentItem").value<QObject*>();
        REQUIRE(recordContent != nullptr);
        CHECK(kitContrast(colorOf(recordContent), controlFill) >= TextContrast);

        auto popUp = KitControl(engine,
                                "KitPopUp",
                                { { "options", QStringList { "a" } },
                                  { "value", QStringLiteral("a") },
                                  { "name", QStringLiteral("x") } });
        CHECK(kitContrast(colorOf(popUp.part("popUpCaption")), controlFill) >= TextContrast);
    }

    SECTION("slider")
    {
        auto kit = KitControl(engine, "KitSlider", { { "value", 0.5 }, { "name", QStringLiteral("x") } });
        CHECK(kitContrast(colorOf(kit.part("sliderValue")), ground) >= TextContrast);
        if (isTerminal)
        {
            CHECK(kitContrast(colorOf(kit.part("sliderGlyphs")), ground) >= TextContrast);
        }
        else
        {
            auto const rail = kitOver(colorOf(kit.part("sliderRail")), ground);
            auto const fill = kitOver(colorOf(kit.part("sliderFill")), ground);
            CHECK(kitContrast(rail, ground) >= MarkContrast);
            CHECK(kitContrast(fill, ground) >= MarkContrast);
            CHECK(kitContrast(colorOf(kit.part("sliderKnob")), fill) >= MarkContrast);
            CHECK(kitContrast(colorOf(kit.part("sliderKnob")), rail) >= MarkContrast);
        }
    }

    SECTION("pop-up rows")
    {
        auto kit = KitControl(engine,
                              "KitPopUp",
                              { { "options", QStringList { "a", "b" } },
                                { "value", QStringLiteral("a") },
                                { "name", QStringLiteral("x") } });
        auto* popup = kit.item()->findChild<QObject*>(QStringLiteral("popUpPopup"));
        REQUIRE(popup != nullptr);
        REQUIRE(QMetaObject::invokeMethod(kit.item(), "open"));
        contour::test::waitForPopupOpened(*popup);
        auto* background = popup->property("background").value<QQuickItem*>();
        REQUIRE(background != nullptr);
        auto const surface = colorOf(background);
        // The popup lives in the window's overlay, which is not under the control's own item.
        auto const* list = popup->findChild<QQuickItem*>(QStringLiteral("popUpList"));
        REQUIRE(list != nullptr);
        auto const rowsOf = [&] {
            auto rows = std::vector<QQuickItem*> {};
            for (auto* row: list->property("contentItem").value<QQuickItem*>()->childItems())
                if (row->objectName() == QLatin1String("popUpRow"))
                    rows.push_back(row);
            return rows;
        };
        // The list builds its rows when it is polished, which is after it opens.
        REQUIRE(QTest::qWaitFor([&] { return rowsOf().size() == 2; }, contour::test::PopupSettleMs));
        for (auto* row: rowsOf())
        {
            auto* label = row->findChild<QObject*>(QStringLiteral("popUpRowLabel"));
            REQUIRE(label != nullptr);
            auto const rowFill = row->property("color").value<QColor>();
            auto const under = rowFill.alpha() == 0 ? surface : kitOver(rowFill, surface);
            // The current row is the selection: the OS's highlight pair on the native rows (see above).
            if (isTerminal || !row->property("isCurrent").toBool())
                CHECK(kitContrast(colorOf(label), under) >= TextContrast);
        }
    }
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("shell integration card: the offer stands below the cells and answers both buttons (offscreen)",
          "[contour][gui][qml][shellintegration]")
{
    using contour::window::CardStage;
    auto host = PaneTreeHost { paneCardSetup(), QSize { 800, 400 } };
    MockSession session;
    MockPaneProxy leaf { /*leaf*/ true };
    leaf.setSession(&session);
    leaf.setActive(true);
    host.show(leaf, /*visiblePanes*/ 1);
    auto const& pane = host.cardOf(leaf);
    auto const& shellCard = PaneTreeHost::partOf(pane, "shellIntegrationCard");
    auto const& band = PaneTreeHost::partOf(pane, "shellIntegrationBand");
    auto const& terminal = PaneTreeHost::terminalOf(pane);
    CHECK_FALSE(shellCard.isVisible());
    auto const heightWithoutCard = terminal.height();

    showCard(session, CardStage::Offer);
    host.settle();
    REQUIRE(shellCard.isVisible());
    CHECK(shellCard.width() == 330);
    CHECK(PaneTreeHost::partOf(shellCard, "shellIntegrationFill").property("radius").toReal() == 12);
    CHECK(PaneTreeHost::partOf(shellCard, "shellIntegrationTitle").property("text").toString()
          == session.shellIntegrationTitle());
    // The cells end where the card's band starts: the card is never over them.
    CHECK(terminal.height() < heightWithoutCard);
    CHECK(terminal.mapToItem(&pane, QPointF { 0, terminal.height() }).y() == band.y());
    CHECK(band.mapToItem(&pane, QPointF { 0, band.height() }).y() == pane.height());
    // Centred in its band.
    CHECK(shellCard.x() == Catch::Approx((band.width() - shellCard.width()) / 2));

    clickOn(host, PaneTreeHost::partOf(shellCard, "shellIntegrationPrimary"));
    CHECK(session.cardAccepts == 1);
    CHECK(session.cardDeclines == 0);
    clickOn(host, PaneTreeHost::partOf(shellCard, "shellIntegrationSecondary"));
    CHECK(session.cardDeclines == 1);
    CHECK(session.cardAccepts == 1);

    // Escape on the card is the other button's answer.
    QMetaObject::invokeMethod(const_cast<QQuickItem*>(&shellCard), "forceActiveFocus");
    QTest::keyClick(&host.window, Qt::Key_Escape);
    CHECK(session.cardDeclines == 2);

    showCard(session, CardStage::Hidden);
    host.settle();
    CHECK_FALSE(shellCard.isVisible());
    CHECK(terminal.height() == heightWithoutCard);
}

TEST_CASE("shell integration card: the line is selectable, in the terminal font, with the failure beneath "
          "(offscreen)",
          "[contour][gui][qml][shellintegration]")
{
    using contour::window::CardStage;
    auto host = PaneTreeHost { paneCardSetup(), QSize { 800, 400 } };
    MockSession session;
    MockPaneProxy leaf { /*leaf*/ true };
    leaf.setSession(&session);
    leaf.setActive(true);
    host.show(leaf, /*visiblePanes*/ 1);
    auto const& shellCard = PaneTreeHost::partOf(host.cardOf(leaf), "shellIntegrationCard");

    showCard(session, CardStage::Confirm);
    host.settle();
    auto const& body = PaneTreeHost::partOf(shellCard, "shellIntegrationBody");
    CHECK(body.property("text").toString() == QString::fromUtf8(ShellCardLine));
    CHECK(body.property("readOnly").toBool());
    CHECK(body.property("selectByMouse").toBool());
    CHECK(body.property("font").value<QFont>().family() == session.fontFamily());
    CHECK_FALSE(PaneTreeHost::partOf(shellCard, "shellIntegrationError").isVisible());

    showCard(session, CardStage::Confirm, QStringLiteral("~/.zshrc was not changed: it is read-only."));
    host.settle();
    auto const& error = PaneTreeHost::partOf(shellCard, "shellIntegrationError");
    CHECK(error.isVisible());
    CHECK(error.property("text").toString() == QStringLiteral("~/.zshrc was not changed: it is read-only."));
    // The error row appears with the text, and the column lays it out on a later polish: on a slow runner
    // settle() can return before it has moved below the line.
    CHECK(QTest::qWaitFor([&] { return error.y() > body.y(); }));

    // The offer's sentence is set in the chrome's font, not the terminal's.
    showCard(session, CardStage::Offer);
    host.settle();
    CHECK(body.property("font").value<QFont>().family() != session.fontFamily());
}

TEST_CASE("shell integration card: an ended pane shows its footer and no card (offscreen)",
          "[contour][gui][qml][shellintegration][paneend]")
{
    using contour::window::CardStage;
    auto host = PaneTreeHost { paneCardSetup(), QSize { 800, 400 } };
    MockSession session;
    MockPaneProxy leaf { /*leaf*/ true };
    leaf.setSession(&session);
    leaf.setActive(true);
    host.show(leaf, /*visiblePanes*/ 1);
    auto const& pane = host.cardOf(leaf);
    showCard(session, CardStage::Offer);
    host.settle();
    REQUIRE(PaneTreeHost::partOf(pane, "shellIntegrationCard").isVisible());

    endWithExitCode(session);
    host.settle();
    CHECK_FALSE(PaneTreeHost::partOf(pane, "shellIntegrationCard").isVisible());
    CHECK_FALSE(PaneTreeHost::partOf(pane, "shellIntegrationBand").isVisible());
    auto const& footer = PaneTreeHost::partOf(pane, "exitedFooter");
    REQUIRE(footer.isVisible());
    CHECK(PaneTreeHost::terminalOf(pane)
              .mapToItem(&pane, QPointF { 0, PaneTreeHost::terminalOf(pane).height() })
              .y()
          == footer.y());
}

TEST_CASE("shell integration card: the Terminal row's card is three cell rows with bracketed buttons "
          "(offscreen)",
          "[contour][gui][qml][shellintegration]")
{
    using contour::window::CardStage;
    auto setup = paneCardSetup();
    setup.row = contour::config::ChromeStyle::Terminal;
    auto host = PaneTreeHost { setup, QSize { 800, 400 } };
    MockSession session;
    MockPaneProxy leaf { /*leaf*/ true };
    leaf.setSession(&session);
    leaf.setActive(true);
    host.show(leaf, /*visiblePanes*/ 1);
    auto const& pane = host.cardOf(leaf);
    showCard(session, CardStage::Offer);
    host.settle();

    auto const& shellCard = PaneTreeHost::partOf(pane, "shellIntegrationCard");
    REQUIRE(shellCard.isVisible());
    auto const cell = host.style().cellHeight();
    CHECK(shellCard.height() == 3 * cell);
    CHECK(shellCard.width() == pane.width() - (2 * host.style().cellWidth()));
    // No room around it: the band is the card.
    CHECK(PaneTreeHost::partOf(pane, "shellIntegrationBand").height() == shellCard.height());
    auto const label = [&](char const* button) {
        return PaneTreeHost::partOf(shellCard, button)
            .findChild<QQuickItem*>(QString::fromLatin1(button) + QStringLiteral("Label"));
    };
    REQUIRE(label("shellIntegrationPrimary") != nullptr);
    CHECK(label("shellIntegrationPrimary")->property("text").toString()
          == host.style().chipOpenGlyph() + session.shellIntegrationPrimary()
                 + host.style().chipCloseGlyph());
    CHECK(label("shellIntegrationSecondary")->property("text").toString()
          == host.style().chipOpenGlyph() + session.shellIntegrationSecondary()
                 + host.style().chipCloseGlyph());
}

TEST_CASE("shell integration card: every ink reads at 4.5:1 on every row, scheme and appearance (offscreen)",
          "[contour][gui][qml][shellintegration]")
{
    using contour::window::CardStage;
    auto const appearance = GENERATE(vtbackend::ColorPreference::Light, vtbackend::ColorPreference::Dark);
    auto const row = GENERATE(contour::config::ChromeStyle::MacOS,
                              contour::config::ChromeStyle::Fluent,
                              contour::config::ChromeStyle::ContourLinux,
                              contour::config::ChromeStyle::Terminal);
    auto const schemeName = GENERATE(from_range(vtbackend::defaultColorPaletteNames().begin(),
                                                vtbackend::defaultColorPaletteNames().end()));
    INFO("Qt palette: " << appearanceName(appearance));
    INFO("scheme: " << schemeName);
    CAPTURE(static_cast<int>(row));
    contour::test::ScopedChromeAppearance const forced { appearance };
    auto palette = vtbackend::ColorPalette {};
    REQUIRE(vtbackend::defaultColorPalettes(schemeName, palette));
    auto setup = terminalRowSetup(schemeName);
    setup.row = row;
    auto const background = contour::platform::toQColor(palette.defaultBackground);

    auto host = PaneTreeHost { setup, QSize { 800, 400 } };
    MockSession session { background };
    MockPaneProxy leaf { /*leaf*/ true };
    leaf.setSession(&session);
    leaf.setActive(true);
    host.show(leaf, /*visiblePanes*/ 1);
    // The Confirm stage shows every ink at once: title, line, failure and both buttons.
    showCard(session, CardStage::Confirm, QStringLiteral("~/.zshrc was not changed: it is read-only."));
    host.settle();

    auto const& shellCard = PaneTreeHost::partOf(host.cardOf(leaf), "shellIntegrationCard");
    auto const fill = over(colorOf(PaneTreeHost::partOf(shellCard, "shellIntegrationFill")), background);
    for (auto const* name: { "shellIntegrationTitle", "shellIntegrationBody", "shellIntegrationError" })
    {
        INFO("text: " << name);
        CHECK(contrastOf(colorOf(PaneTreeHost::partOf(shellCard, name)), fill) >= 4.5);
    }
    for (auto const* name: { "shellIntegrationPrimary", "shellIntegrationSecondary" })
    {
        INFO("button: " << name);
        auto const& button = PaneTreeHost::partOf(shellCard, name);
        auto const buttonFill = over(colorOf(button), fill);
        auto const& label = PaneTreeHost::partOf(button, (std::string { name } + "Label").c_str());
        CHECK(contrastOf(colorOf(label), buttonFill) >= 4.5);
        // Hovered and pressed lay the same wash: the label must stay at the mark floor over it.
        auto const wash = button.property("hoverWash").value<QColor>();
        CHECK(contrastOf(colorOf(label), over(wash, buttonFill)) >= 3.0);
    }
}
