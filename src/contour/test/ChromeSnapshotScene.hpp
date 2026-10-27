// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/command/BlockMenu.hpp>
#include <contour/command/CommandCatalog.hpp>
#include <contour/command/CommandHistory.hpp>
#include <contour/command/CommandPaletteTree.hpp>
#include <contour/command/MenuTree.hpp>
#include <contour/command/Shortcut.hpp>
#include <contour/config/Config.hpp>
#include <contour/config/GuiConfigStore.hpp>
#include <contour/config/NewDefaultsOffer.hpp>
#include <contour/config/TabBarMode.hpp>
#include <contour/config/WindowControlStyle.hpp>
#include <contour/platform/ColorConversion.hpp>
#include <contour/session/EndReason.hpp>
#include <contour/session/PermissionPrompt.hpp>
#include <contour/session/SizeIndicator.hpp>
#include <contour/test/ChromeSnapshotPlan.hpp>
#include <contour/test/MockMainController.hpp>
#include <contour/test/PopupWaits.hpp>
#include <contour/test/QmlChromeStyle.hpp>
#include <contour/test/QmlIncubation.hpp>
#include <contour/test/QmlMessageCapture.hpp>
#include <contour/test/QmlMocks.hpp>
#include <contour/window/CommandPaletteModel.hpp>
#include <contour/window/ContextMenuModel.hpp>
#include <contour/window/HostChipState.hpp>
#include <contour/window/NoticeQueue.hpp>
#include <contour/window/PaneHeaderPolicy.hpp>
#include <contour/window/SecureInputPolicy.hpp>
#include <contour/window/SettingsController.hpp>
#include <contour/window/SettingsWindowHost.hpp>
#include <contour/window/SettingsWindowRules.hpp>
#include <contour/window/StickyHeaderChrome.hpp>
#include <contour/window/StickyHeaderPlacement.hpp>
#include <contour/window/TitleBandGeometry.hpp>
#include <contour/window/TitleBandLayout.hpp>
#include <contour/window/TitleBandSurface.hpp>
#include <contour/window/UiStyleProvider.hpp>

#include <vtbackend/shell/ScrollbarMarks.hpp>

#include <core/platform/FileSystem.hpp>
#include <core/platform/NativeFileSystem.hpp>

#include <QtCore/QByteArray>
#include <QtCore/QCoreApplication>
#include <QtCore/QPoint>
#include <QtCore/QPointF>
#include <QtCore/QSize>
#include <QtCore/QStringList>
#include <QtCore/QTemporaryDir>
#include <QtCore/QUrl>
#include <QtCore/QVariant>
#include <QtCore/QVariantList>
#include <QtCore/QVariantMap>
#include <QtGui/QFont>
#include <QtGui/QFontDatabase>
#include <QtGui/QGuiApplication>
#include <QtGui/QImage>
#include <QtGui/QWindow>
#include <QtQml/QQmlComponent>
#include <QtQml/QQmlContext>
#include <QtQml/QQmlEngine>
#include <QtQuick/QQuickItem>
#include <QtQuick/QQuickWindow>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <vector>

#include <QtTest/QTest>

/// The chrome snapshot driver's scene: one SnapshotVariant built as Main.qml over mocks, offscreen.
///
/// The plan (ChromeSnapshotPlan.hpp) decides what each picture shows; this only interprets a row of it,
/// so a new script is a row there rather than a branch here.
namespace contour::test::snapshot
{

/// The size every snapshot window is rendered at, in device-independent pixels.
inline constexpr QSize SnapshotWindowSize { 1024, 640 };

/// Why the bundled chrome fonts could not be registered.
enum class SnapshotFontError : uint8_t
{
    DirectoryMissing = 0, ///< The font directory does not exist, or is not a directory.
    NoFontLoaded,         ///< The directory holds no `*.ttf` that Qt accepted.
};

/// The directory the repository's bundled chrome fonts live in.
/// @return `<CONTOUR_GUI_SOURCE_DIR>/res/fonts`.
[[nodiscard]] inline std::filesystem::path bundledChromeFontDirectory()
{
    return std::filesystem::path(CONTOUR_GUI_SOURCE_DIR) / "res" / "fonts";
}

/// Registers every `*.ttf` in @p directory with Qt, so the chrome renders in the same font everywhere.
///
/// A file that cannot be read, or that Qt does not accept as a font, is skipped.
/// @param files     The file system to read the fonts from.
/// @param directory The directory to read.
/// @return The families Qt loaded, without duplicates; or why none were.
[[nodiscard]] inline std::expected<QStringList, SnapshotFontError> registerBundledChromeFonts(
    core::platform::FileSystem const& files, std::filesystem::path const& directory)
{
    if (!files.isDirectory(directory))
        return std::unexpected { SnapshotFontError::DirectoryMissing };
    auto const entries = files.listDirectory(directory);
    if (!entries)
        return std::unexpected { SnapshotFontError::DirectoryMissing };

    auto const isFontFile = [](core::platform::FileSystem::DirectoryEntry const& entry) {
        return entry.isRegularFile && entry.path.extension() == ".ttf";
    };
    auto families = QStringList {};
    for (auto const& entry: *entries | std::views::filter(isFontFile))
    {
        auto const bytes = files.readFile(entry.path);
        if (!bytes)
            continue;
        auto const id = QFontDatabase::addApplicationFontFromData(QByteArray::fromStdString(*bytes));
        if (id >= 0)
            families << QFontDatabase::applicationFontFamilies(id);
    }
    if (families.isEmpty())
        return std::unexpected { SnapshotFontError::NoFontLoaded };
    families.removeDuplicates();
    return families;
}

/// One snapshot variant, built as the real Main.qml over mocks and ready to show.
///
/// Construction builds everything the variant names and loads Main.qml, failing the enclosing test unless
/// the load is clean. Everything is owned here; the members are ordered so the QML tree dies before the
/// mocks it borrows, and the application palette is given back after everything else.
class ChromeSnapshotScene
{
  public:
    /// The native title bar the macOS row's band covers: AppKit's title bar grown by the empty compact
    /// toolbar, as the Step 1d spike measured it (spike-decisions.md §D2, ruling S2: 40 pt; 32 bare). A
    /// constant for the reason SnapshotTrafficLightInset is one (see there).
    static constexpr qreal SnapshotNativeTitleBarHeight = 40;

    /// AppKit's bare title bar, without the compact toolbar: what a band whose surface grows nothing covers
    /// -- the Terminal row's, and Minimal's bare band (ruling P3, the spike's 32 pt).
    static constexpr qreal SnapshotBareTitleBarHeight = 32;

    /// What the macOS row's band leaves clear for AppKit's traffic lights: the frame adapter's inset as the
    /// Step 1d spike measured it under the compact toolbar (spike-decisions.md §D2: 84 pt; 78 without). A
    /// constant rather than a measurement: the scene's window is a mock's, with no AppKit title bar dressed
    /// to measure, and the pictures must not differ by the machine that renders them.
    static constexpr qreal SnapshotTrafficLightInset = 84;

    /// The traffic lights' inset over a bare title bar (spike-decisions.md §D2: 78 pt; 84 with the toolbar).
    static constexpr qreal SnapshotBareTrafficLightInset = 78;

    /// AppKit's title bar under a band, as the scene stands it in for the controller's measurement.
    struct SnapshotNativeTitleBar
    {
        qreal height = 0;            ///< The title bar's height.
        qreal trafficLightInset = 0; ///< The traffic lights' inset (WindowController::nativeControlsInset).
    };

    /// The title bar a band's surface leaves AppKit with: the surface's growth decides it, as it decides the
    /// toolbar the controller attaches in production.
    /// @param growth The resolved surface's title-bar growth.
    /// @return The grown title bar and its inset under CompactToolbar, the bare ones otherwise.
    [[nodiscard]] static constexpr SnapshotNativeTitleBar nativeTitleBarFor(
        config::TitleBarGrowth growth) noexcept
    {
        switch (growth)
        {
            case config::TitleBarGrowth::CompactToolbar:
                return { .height = SnapshotNativeTitleBarHeight,
                         .trafficLightInset = SnapshotTrafficLightInset };
            case config::TitleBarGrowth::None: break;
        }
        return { .height = SnapshotBareTitleBarHeight, .trafficLightInset = SnapshotBareTrafficLightInset };
    }

    /// The chip state a script's connection column shows.
    /// @param connection The script's host connection.
    /// @return Its HostChipState; Connected for a local window, which shows no chip.
    [[nodiscard]] static constexpr window::HostChipState hostChipStateFor(HostConnection connection) noexcept
    {
        switch (connection)
        {
            case HostConnection::Local:
            case HostConnection::Connected: break;
            case HostConnection::Reconnecting: return window::HostChipState::Reconnecting;
            case HostConnection::Disconnected: return window::HostChipState::Disconnected;
        }
        return window::HostChipState::Connected;
    }

    /// @param variant The picture to build: its script, chrome row, defaults edition and appearance.
    explicit ChromeSnapshotScene(SnapshotVariant variant):
        _appearance { variant.appearance },
        _variant { variant },
        _config { .defaultsEdition = variant.edition },
        _look { snapshotLook(variant.edition, variant.appearance) },
        _chromeFont { window::resolveChromeFont(_config, variant.row, _config.defaultProfileName.value()) },
        _sessions { makeSessions(scriptRow(variant.script), _look) },
        _rootPane { makePaneTree(scriptRow(variant.script).panes, _sessions) },
        _history { PaletteHistoryCapacity },
        _paletteConfig { makePaletteConfig() },
        _profileCommands { _paletteConfig },
        _layoutCommands { _paletteConfig },
        _paletteModel { makePaletteModel(_history,
                                         { &_profileCommands, &_layoutCommands, &_actionCommands },
                                         _config,
                                         command::shortcutStyleFor(variantHost(variant))) },
        _settings { makeSettings(scriptRow(variant.script).surface) },
        _controller { MainWindowScene {
            .tabCount = scriptRow(variant.script).tabCount,
            .rootPane = _rootPane.get(),
            .commandPalette = _paletteModel.get(),
            .hostLabel = QString::fromUtf8(scriptRow(variant.script).hostLabel),
            .hostingNotice = hostingNoticeFor(scriptRow(variant.script).banner),
            .tabBarVisibility = _look.tabBarVisibility,
            .decor = scriptRow(variant.script).decor,
            .activeTab = scriptRow(variant.script).activeTab,
            .hostChipState = hostChipStateFor(scriptRow(variant.script).connection),
            .tabBarPosition = config::TabBarPosition::Top,
            .drift = scriptRow(variant.script).drift,
            .frame = scriptRow(variant.script).frame,
            // measureBand() gives the macOS host's band its inset, from the row's surface; none elsewhere.
            .nativeControlsInset = 0,
            .secure = scriptRow(variant.script).secure,
        } },
        _newDefaults { newDefaultsOfferFor(scriptRow(variant.script).banner) },
        _settingsWindow { makeSettingsWindow() },
        _root { load() }
    {
        measureBand();
    }

    ChromeSnapshotScene(ChromeSnapshotScene const&) = delete;
    ChromeSnapshotScene& operator=(ChromeSnapshotScene const&) = delete;
    ChromeSnapshotScene(ChromeSnapshotScene&&) = delete;
    ChromeSnapshotScene& operator=(ChromeSnapshotScene&&) = delete;
    /// Leaves full screen before the window goes, so the next scene's window may enter it: macOS refuses
    /// while a window is still on its way out of a full-screen space.
    ~ChromeSnapshotScene()
    {
        if (_root == nullptr || window().visibility() != QWindow::FullScreen)
            return;
        window().showNormal();
        // Best effort: a destructor has nothing to report a window that stays in full screen to.
        [[maybe_unused]] auto const left = QTest::qWaitFor(
            [&] { return window().visibility() != QWindow::FullScreen; }, FullScreenSettleTimeoutMs);
        // The animation out of the space outlasts the state change.
        QTest::qWait(FullScreenLeaveMs);
    }

    /// @return The object the script's action created over the window, or null when it created none.
    [[nodiscard]] QObject const* overlay() const noexcept { return _overlay.get(); }

    /// @return The command palette's model.
    [[nodiscard]] window::CommandPaletteModel const& paletteModel() const noexcept { return *_paletteModel; }

    /// @return How many profiles the palette offers: the default one and PaletteProfiles.
    [[nodiscard]] int paletteProfileCount() const noexcept
    {
        return static_cast<int>(_paletteConfig.profiles.value().size());
    }

    /// @return The tab colour flyout the script's action opened, or null when it opened none.
    [[nodiscard]] QObject const* openedTabColorFlyout() const noexcept { return _tabColorFlyout; }

    /// Shows the window at SnapshotWindowSize, opens the Settings window over it when the script's surface
    /// is that one, performs the script's action, and lets the scene settle.
    void show()
    {
        window().resize(SnapshotWindowSize);
        switch (scriptRow(_variant.script).mode)
        {
            case SnapshotWindowMode::Windowed: window().show(); break;
            case SnapshotWindowMode::FullScreenRevealed: showFullScreenRevealed(); break;
        }
        if (_settingsWindow != nullptr)
            REQUIRE(_settingsWindow->show(nullptr).has_value());
        switch (scriptRow(_variant.script).action)
        {
            case ScriptAction::None: break;
            case ScriptAction::OpenPalette: openPalette(); break;
            case ScriptAction::FilterPalette:
                openPalette();
                filterPalette(QStringLiteral("split"));
                break;
            case ScriptAction::DrillInPalette: drillInPalette(); break;
            case ScriptAction::ShowToast: showToast(); break;
            case ScriptAction::OpenKitDialog: openKitDialog(); break;
            case ScriptAction::OpenTabMenu: openTabMenu(); break;
            case ScriptAction::OpenAbout: openAbout(); break;
            case ScriptAction::OpenTabColorFlyout: openTabColorFlyout(); break;
            case ScriptAction::HoverPaneHeader: hoverPaneHeader(); break;
            // The reveal itself, not the key: which presses reveal is HeldModifierReveal's, tested in
            // TerminalSessionManager_test.
            case ScriptAction::RevealHeaders:
                _controller.setHeaderReveal(static_cast<int>(window::HeaderReveal::HeldModifier));
                break;
            case ScriptAction::OpenFindBar: openFindBar(); break;
            case ScriptAction::OpenPermission: openPermission(scriptRow(_variant.script).permission); break;
            case ScriptAction::OpenSaveLayout: openSaveLayout(scriptRow(_variant.script).layoutName); break;
            case ScriptAction::ShowSizeIndicator: showSizeIndicators(); break;
            case ScriptAction::HoldLinkModifier: holdLinkModifier(); break;
            case ScriptAction::ScrollBack: scrollBack(); break;
            case ScriptAction::FocusTabWell: focusTabWell(); break;
            case ScriptAction::OpenBlockMenu: openBlockMenu(); break;
            case ScriptAction::HoverBlock: hoverBlock(); break;
            case ScriptAction::ShowRestoreSummary: showRestoreSummary(); break;
        }
        // The cards announce where the header lives once the panes have their size, which showSticky() reads.
        _incubation.settle();
        QCoreApplication::processEvents();
        showSticky();
        // The window's notices post their banner as the reload that failed would.
        if (scriptRow(_variant.script).banner == SnapshotBanner::ConfigReload)
            _controller.notices().post(
                window::Notice { .kind = window::NoticeKind::ConfigReloadFailed,
                                 .text = window::configReloadFailedNoticeText(SnapshotConfigReloadError) });
        _incubation.settle();
        QCoreApplication::processEvents();
        QCoreApplication::processEvents();
    }

    /// The window the picture is of, as rendered. Needs a display: the offscreen platform grabs nothing
    /// worth keeping.
    /// @return The grabbed frame of subject().
    [[nodiscard]] QImage grab() const { return subject().grabWindow(); }

    /// The window the picture is of: the Settings window for SnapshotSurface::SettingsWindow, else window().
    /// @return That window.
    /// @pre show() ran.
    [[nodiscard]] QQuickWindow& subject() const
    {
        if (_settingsWindow == nullptr)
            return window();
        REQUIRE(_settingsWindow->window() != nullptr); // show() builds it
        return *_settingsWindow->window();
    }

    /// @return Main.qml's ApplicationWindow.
    [[nodiscard]] QQuickWindow& window() const { return *qobject_cast<QQuickWindow*>(_root.get()); }

    /// @return Main.qml's root object, for findChild().
    [[nodiscard]] QObject& root() const { return *_root; }

    /// @return The QML diagnostics captured since construction (see isQmlDiagnostic).
    [[nodiscard]] QStringList diagnostics() const
    {
        auto diagnostics = QStringList {};
        for (auto const& message: _warnings.messages() | std::views::filter(isQmlDiagnostic))
            diagnostics << message;
        return diagnostics;
    }

    /// @return The font the chrome was configured with, as resolved for the variant's row.
    [[nodiscard]] QFont const& chromeFont() const noexcept { return _chromeFont; }

    /// @return The look the scene was built with, from its edition and appearance.
    [[nodiscard]] SnapshotLook const& look() const noexcept { return _look; }

    /// @return The AppKit title bar the band was measured over (see measureBand), or std::nullopt when the
    ///         variant has none to measure: off a macOS host, under the native frame, or in full screen.
    [[nodiscard]] std::optional<SnapshotNativeTitleBar> const& measuredTitleBar() const noexcept
    {
        return _measuredTitleBar;
    }

    /// The session of the active leaf, whose card announced where its sticky header lives.
    [[nodiscard]] MockSession& activeSession() { return *_sessions[ActiveLeaf]; }

  private:
    /// How many recent commands the palette remembers, as MockPaletteController remembers them.
    static constexpr std::size_t PaletteHistoryCapacity = 5;

    /// The toast's message.
    [[nodiscard]] static QString toastText() { return QStringLiteral("Copied 3 lines"); }
    /// The windows and tabs the restore-summary script's daemon brought back.
    static constexpr std::size_t RestoredWindows = 2;
    static constexpr std::size_t RestoredTabs = 7;
    /// The size of the paste the permission-paste script asks about, and the lines its preview leaves out.
    static constexpr std::size_t SnapshotPasteBytes = 2'400'000;
    static constexpr std::size_t SnapshotPasteMoreLines = 48'211;

    /// The dialog's texts.
    [[nodiscard]] static QString dialogHeading() { return QStringLiteral("Close this tab?"); }
    [[nodiscard]] static QString dialogMessage()
    {
        return QStringLiteral("\u201Ccmake\u201D is still running. Closing the tab will end it.");
    }
    [[nodiscard]] static QString dialogAcceptText() { return QStringLiteral("Close Tab"); }
    [[nodiscard]] static QString dialogRejectText() { return QStringLiteral("Cancel"); }

    /// The tab whose context menu the tab-menu script opens, and whose colour flyout the tab-color-flyout
    /// script opens: the middle of three.
    static constexpr int SnapshotMenuTab = 1;

    /// The title of the tab menu's colour sub-menu.
    [[nodiscard]] static QString tabColorTitle() { return QStringLiteral("Tab Color"); }

    /// How often, and for how long at most, the full-screen size is polled until it holds still.
    static constexpr int FullScreenSettlePollMs = 200;
    static constexpr int FullScreenSettleTimeoutMs = 5000;
    /// How long macOS's animation out of a full-screen space is given.
    static constexpr int FullScreenLeaveMs = 1500;

    /// What halves a length to centre an item.
    static constexpr qreal HalfDivisor = 2;

    /// The leaf that holds focus in every pane shape.
    static constexpr std::size_t ActiveLeaf = 0;

    /// The leaf whose session a script with an ended pane ends: the last, an inactive one beside the active.
    [[nodiscard]] static constexpr std::size_t endedLeaf(PaneShape shape) noexcept
    {
        return paneCount(shape) - 1;
    }

    /// Gives @p session the search @p find names, as the find bar would find it.
    static void searchIn(MockSession& session, SnapshotFind find)
    {
        switch (find)
        {
            case SnapshotFind::None: break;
            case SnapshotFind::Matches:
                session.searchOptions.syntax = vtbackend::SearchSyntax::Regex;
                session.setSearchPattern(QStringLiteral("config"));
                session.setSearchSummary(QStringLiteral("2 of 5"),
                                         MockSession::SearchMatchPresence::Some,
                                         MockSession::SearchNavigation::Some);
                break;
            case SnapshotFind::Invalid:
                session.searchOptions.syntax = vtbackend::SearchSyntax::Regex;
                session.setSearchPattern(QStringLiteral("a("));
                session.setSearchSummary(QStringLiteral("Invalid pattern"),
                                         MockSession::SearchMatchPresence::None,
                                         MockSession::SearchNavigation::None);
                break;
        }
    }

    /// A cell of the stubbed terminal: StubContourTerminal's rows, and the width the link pill's clearance
    /// measures columns in.
    static constexpr qreal SnapshotCellWidth = 8;
    static constexpr qreal SnapshotCellHeight = 20;
    /// The row of the page the sample block's head is on, for the ⋯ button's anchor.
    static constexpr qreal SnapshotBlockHoverRow = 3;

    /// The terminal item of each leaf's session, in leaf order: the pane whose background is drawn for it.
    /// Waits for the panes to be laid out: the window was only just shown.
    [[nodiscard]] std::vector<QQuickItem*> terminalsOfLeaves()
    {
        _incubation.settle();
        auto const backgrounds = itemsNamed(*window().contentItem(), QStringLiteral("paneBackground"));
        auto terminals = std::vector<QQuickItem*> {};
        for (auto const& session: _sessions)
        {
            auto const found = std::ranges::find_if(backgrounds, [&](QQuickItem const* background) {
                return background->parentItem()->property("session").value<QObject*>() == session.get();
            });
            REQUIRE(found != backgrounds.end());
            terminals.push_back((*found)->parentItem());
        }
        REQUIRE(QTest::qWaitFor(
            [&] {
                return std::ranges::all_of(terminals, [](QQuickItem const* terminal) {
                    return terminal->width() >= SnapshotCellWidth && terminal->height() >= SnapshotCellHeight;
                });
            },
            2000));
        return terminals;
    }

    /// The page @p terminal holds in cells of SnapshotCellWidth × SnapshotCellHeight.
    [[nodiscard]] static vtbackend::PageSize pageOf(QQuickItem const& terminal)
    {
        return vtbackend::PageSize { .lines = vtbackend::LineCount::cast_from(
                                         static_cast<int>(terminal.height() / SnapshotCellHeight)),
                                     .columns = vtbackend::ColumnCount::cast_from(
                                         static_cast<int>(terminal.width() / SnapshotCellWidth)) };
    }

    /// Gives every leaf's session the page its pane holds, and raises its size indicator with that size, as
    /// the resize that settled on it does.
    ///
    /// Each pill is then held: its pane's hold timer (`sizeIndicatorHold`) is stopped, so the pill stays
    /// until the scene goes, and the picture is taken of a pill fully shown however long the grab takes --
    /// never of one fading out after its 600 ms hold.
    void showSizeIndicators()
    {
        auto pills = std::vector<QQuickItem*> {};
        for (auto const [session, terminal]: std::views::zip(_sessions, terminalsOfLeaves()))
        {
            auto const page = pageOf(*terminal);
            session->setPageSize(unbox(page.columns), unbox(page.lines));
            session->requestSizeIndicator(QString::fromStdString(
                session::sizeIndicatorText(session::SizeIndicatorKind::PageSize, page, 0.0)));
            auto* hold = terminal->findChild<QObject*>(QStringLiteral("sizeIndicatorHold"));
            REQUIRE(hold != nullptr);
            QMetaObject::invokeMethod(hold, "stop");
            auto* pill = terminal->findChild<QQuickItem*>(QStringLiteral("sizeIndicator"));
            REQUIRE(pill != nullptr);
            pills.push_back(pill);
        }
        waitFullyShown(pills);
    }

    /// Waits until every one of @p overlays has faded fully in: a state, not a duration.
    static void waitFullyShown(std::span<QQuickItem* const> overlays)
    {
        REQUIRE(QTest::qWaitFor(
            [&] {
                return std::ranges::all_of(overlays,
                                           [](QQuickItem const* item) { return item->opacity() == 1.0; });
            },
            2000));
    }

    /// Holds the follow modifier over a link in the active pane: the session names SnapshotLinkTarget, how
    /// the row's host opens it, and a page whose every cell is blank.
    void holdLinkModifier()
    {
        auto* terminal = terminalsOfLeaves()[ActiveLeaf];
        auto const page = pageOf(*terminal);
        auto& session = *_sessions[ActiveLeaf];
        session.setOverlayClearance(QVariantMap {
            { QStringLiteral("marginLeft"), 0.0 },
            { QStringLiteral("marginTop"), 0.0 },
            { QStringLiteral("cellWidth"), SnapshotCellWidth },
            { QStringLiteral("cellHeight"), SnapshotCellHeight },
            { QStringLiteral("columns"), unbox(page.columns) },
            { QStringLiteral("trailingBlank"),
              QVariantList(unbox(page.lines), QVariant(unbox(page.columns))) },
            { QStringLiteral("avoidRows"), QVariantList {} },
        });
        auto const hint = command::linkOpenHint(config::followHyperlinkModifiers(_paletteConfig),
                                                vtbackend::Modifiers {},
                                                command::shortcutStyleFor(variantHost(_variant)));
        session.setLinkTarget(QString::fromUtf8(SnapshotLinkTarget), QString::fromStdString(hint), true);
        auto* pill = terminal->findChild<QQuickItem*>(QStringLiteral("linkPill"));
        REQUIRE(pill != nullptr);
        waitFullyShown(std::array { pill });
    }

    /// Puts keyboard focus in the tab well, on tab SnapshotFocusedTab, as Ctrl+F6 does, and waits for the
    /// strip to have moved active focus there. The panes are incubated first: a pane taking focus as it
    /// appears is the terminal's, and ends the chrome's.
    void focusTabWell()
    {
        _incubation.settle();
        QCoreApplication::processEvents();
        window().requestActivate();
        // The strip's list creates its delegates lazily, and focusTab() does nothing for a row that has none
        // yet: a strip narrower than the window (the Fluent and Terminal rows) has not made this one.
        REQUIRE(QTest::qWaitFor(
            [&] {
                auto const tabs = itemsNamed(*window().contentItem(), QStringLiteral("tabBackground"));
                return std::ranges::any_of(tabs, [](QQuickItem const* background) {
                    return background->parentItem()->property("tabIndex").toInt() == SnapshotFocusedTab;
                });
            },
            2000));
        _controller.focusTabWell(SnapshotFocusedTab);
        REQUIRE(QTest::qWaitFor(
            [&] {
                auto const* tab = window().activeFocusItem();
                return tab != nullptr && tab->property("tabIndex").toInt() == SnapshotFocusedTab;
            },
            2000));
    }

    /// Opens the sample block's menu as the ⋯ button and the focus chords open it: the terminal's menu, its
    /// model the block menu's four header rows, a separator and five commands.
    void openBlockMenu()
    {
        auto const state = command::BlockMenuState {
            .id = vtbackend::CommandBlockId { static_cast<uint32_t>(SnapshotBlockId) },
            .header =
                std::vector<std::string> { SnapshotBlockMenuHeader.begin(), SnapshotBlockMenuHeader.end() },
            .output = command::OutputPresence::Present,
            .commandLine = command::CommandLinePresence::Present,
            .livePrompt = command::PromptPresence::Present,
            .fold = command::FoldPresence::Expanded,
        };
        auto const tree = command::buildMenuTree(
            command::buildBlockMenu(state), {}, command::shortcutStyleFor(variantHost(_variant)));
        _controller.setContextMenuModel(window::toContextMenuModel(tree.nodes));
        // Anchored at the block's first row, as the ⋯ button and the chords anchor it (the strip's foot).
        _controller.setContextMenuAnchor(QPointF { 0, SnapshotBlockHoverRow * SnapshotCellHeight });
        auto* menu = root().findChild<QObject*>(QStringLiteral("terminalContextMenu"));
        REQUIRE(menu != nullptr);
        _controller.openContextMenu();
        REQUIRE(QTest::qWaitFor([&] { return menu->property("opened").toBool(); }, 2000));
    }

    /// Puts the pointer on the sample block's strip: the active pane's session publishes the block and where
    /// its ⋯ button goes, a few rows down the strip the rail lives in (a cell wide on the Terminal row).
    void hoverBlock()
    {
        auto const* provider = qobject_cast<window::UiStyleProvider const*>(
            _engine.rootContext()->contextProperty(QStringLiteral("chromeStyle")).value<QObject*>());
        REQUIRE(provider != nullptr);
        auto const stripWidth =
            std::max(provider->property("space").value<QObject*>()->property("blockGutter").toReal(),
                     provider->property("size").value<QObject*>()->property("blockActions").toReal());
        _sessions[ActiveLeaf]->setBlockHover(
            SnapshotBlockId,
            QRectF { 0, SnapshotBlockHoverRow * SnapshotCellHeight, stripWidth, SnapshotCellHeight });
        auto const buttons = root().findChildren<QQuickItem*>(QStringLiteral("blockActionsButton"));
        REQUIRE(QTest::qWaitFor(
            [&] {
                return std::ranges::any_of(buttons,
                                           [](QQuickItem const* button) { return button->isVisible(); });
            },
            2000));
    }

    /// Shows the sample block's sticky header where the script says it lives: the active pane's header, or
    /// the title band. Nothing for a script without a sample block, or whose action shows the block
    /// elsewhere.
    void showSticky()
    {
        auto const publish = [&](SnapshotSticky const& sticky, window::StickyHeaderHost host) {
            auto const& glyph = window::StickyHeaderGlyphRows.at(static_cast<std::size_t>(sticky.outcome));
            return QVariantMap {
                { QStringLiteral("glyph"), QString::fromUtf8(sticky.glyph) },
                { QStringLiteral("command"), QString::fromUtf8(sticky.command) },
                { QStringLiteral("detail"), QString::fromUtf8(sticky.detail) },
                { QStringLiteral("outcome"), static_cast<int>(sticky.outcome) },
                { QStringLiteral("statusRole"), static_cast<int>(glyph.role) },
                { QStringLiteral("host"), static_cast<int>(host) },
                { QStringLiteral("jumpLabel"), QStringLiteral("Jump to Command ↑") },
            };
        };
        // Where the active card says the header lives, as the real session would be told: the grid draws it
        // where neither a pane header nor the band's title can, so the chrome then carries nothing.
        auto const hostOf = [&] {
            auto const& hosts = _sessions[ActiveLeaf]->stickyHosts;
            REQUIRE_FALSE(hosts.empty());
            return static_cast<window::StickyHeaderHost>(hosts.back());
        };
        auto const publishFor = [&](SnapshotSticky const& sticky) {
            auto const host = hostOf();
            if (host == window::StickyHeaderHost::Grid)
                return;
            _sessions[ActiveLeaf]->setStickyHeader(publish(sticky, host));
            if (host == window::StickyHeaderHost::TitleBand)
                _controller.setActiveStickyHeader(publish(sticky, host));
        };
        switch (_variant.script)
        {
            case SnapshotScript::StickyPaneHeader: publishFor(SnapshotStickyFailed); break;
            case SnapshotScript::StickyTitleBand: publishFor(SnapshotStickySucceeded); break;
            default: break;
        }
    }

    /// Scrolls the active pane SnapshotScrollOffset lines back into SnapshotHistoryLines of history, with a
    /// failed command's tick above the page and a running one's below it, in the variant's scheme colours.
    void scrollBack()
    {
        auto* session = qobject_cast<ScrollingMockSession*>(_sessions[ActiveLeaf].get());
        REQUIRE(session != nullptr); // makeSessions() gives a scrolling script's active leaf one
        // The colour config owns the palettes preferredColorPalette points into, so it must outlive them.
        auto const colorConfig = config::defaultColors(_variant.edition);
        auto const* const palette = config::preferredColorPalette(colorConfig, _variant.appearance);
        auto colors = QVariantList {};
        for (auto const kind: vtbackend::ScrollbarMarkKindList)
            colors.append(
                QVariant::fromValue(platform::toQColor(vtbackend::scrollbarMarkColor(*palette, kind))));
        auto const tick = [](double position, vtbackend::ScrollbarMarkKind kind, qint64 targetLine) {
            return QVariantMap { { QStringLiteral("position"), position },
                                 { QStringLiteral("kind"), static_cast<int>(kind) },
                                 { QStringLiteral("style"),
                                   static_cast<int>(vtbackend::scrollbarMarkKindRow(kind).style) },
                                 { QStringLiteral("targetLine"), targetLine } };
        };
        session->setHistoryLineCount(SnapshotHistoryLines);
        session->setScrollbarMarks(QVariantList { tick(0.62, vtbackend::ScrollbarMarkKind::Failure, 620),
                                                  tick(0.99, vtbackend::ScrollbarMarkKind::Running, 990) },
                                   colors);
        session->setScrollbarVisible(true);
        session->setScrollOffset(SnapshotScrollOffset);
        auto* bar =
            terminalsOfLeaves()[ActiveLeaf]->findChild<QQuickItem*>(QStringLiteral("verticalScrollBar"));
        REQUIRE(bar != nullptr);
        waitFullyShown(std::array { bar });
    }

    /// Opens the active pane's find bar, as the SearchReverse action does, and waits for it to stand open.
    void openFindBar()
    {
        auto* active = _sessions[ActiveLeaf].get();
        auto* bar = static_cast<QObject*>(nullptr);
        for (auto* candidate: root().findChildren<QObject*>(QStringLiteral("searchBar")))
            if (candidate->property("session").value<QObject*>() == active)
                bar = candidate;
        REQUIRE(bar != nullptr);
        QMetaObject::invokeMethod(bar, "open");
        REQUIRE(QTest::qWaitFor([&] { return bar->property("opened").toBool(); }, 2000));
    }

    /// One session per leaf of @p script's pane shape, in the look's background; every one but the
    /// active leaf's dimmed as unfocused, each running what SnapshotPaneLabels names, all of them
    /// reconnecting when the host connection is, and the last one ended -- its program exited, Restart
    /// offered -- when the script ends a pane.
    [[nodiscard]] static std::vector<std::unique_ptr<MockSession>> makeSessions(
        SnapshotScriptRow const& script, SnapshotLook const& look)
    {
        auto sessions = std::vector<std::unique_ptr<MockSession>> {};
        for (auto const leaf: std::views::iota(std::size_t { 0 }, paneCount(script.panes)))
        {
            // A scrolled-back script's active leaf scrolls; every other session has no viewport of its own.
            auto const background = platform::toQColor(look.terminalBackground);
            auto session =
                script.action == ScriptAction::ScrollBack && leaf == ActiveLeaf
                    ? std::unique_ptr<MockSession> { std::make_unique<ScrollingMockSession>(background) }
                    : std::make_unique<MockSession>(background);
            if (leaf != ActiveLeaf)
                session->setDimUnfocused(static_cast<float>(look.dimUnfocused));
            auto const& label = SnapshotPaneLabels[leaf];
            session->setForegroundProcessName(QString::fromUtf8(label.process));
            session->setDisplayWorkingDirectory(QString::fromUtf8(label.workingDirectory));
            if (script.connection == HostConnection::Reconnecting)
                session->setReconnecting(true);
            if (leaf == ActiveLeaf)
                searchIn(*session, script.find);
            // The active pane waits for a password, as the scene's tabs say (MockMainController::isLocked).
            if (leaf == ActiveLeaf && script.secure == SecureDecor::Locked)
                session->setSecureIndicator(static_cast<int>(window::SecureIndicator::Locked));
            switch (script.paneEnd)
            {
                case SnapshotPaneEnd::None: break;
                case SnapshotPaneEnd::Exited:
                    if (leaf == endedLeaf(script.panes))
                        session->setEnd(static_cast<int>(session::EndReason::Exited),
                                        QString::fromUtf8(SnapshotExitMessage),
                                        static_cast<int>(session::FooterAction::Restart));
                    break;
                case SnapshotPaneEnd::Interrupted:
                    if (leaf == endedLeaf(script.panes))
                        session->setInterrupted(
                            QString::fromStdString(window::interruptedFooterMessage(
                                vtbackend::CommandLineText { std::string { SnapshotInterruptedCommand } })),
                            QString::fromUtf8(session::InterruptedFooterRow.actionLabel));
                    break;
            }
            sessions.push_back(std::move(session));
        }
        return sessions;
    }

    /// The pane tree @p shape describes over @p sessions, one leaf per session in order.
    [[nodiscard]] static std::unique_ptr<MockPaneProxy> makePaneTree(
        PaneShape shape, std::span<std::unique_ptr<MockSession> const> sessions)
    {
        auto const leaf = [&](std::size_t index) {
            auto pane = std::make_unique<MockPaneProxy>(/*leaf=*/true);
            pane->setSession(sessions[index].get());
            pane->setActive(index == ActiveLeaf);
            // The active pane has focus, by a click: its header reads primary, the others secondary.
            pane->setFocus(static_cast<int>(index == ActiveLeaf ? window::PaneFocus::Focused
                                                                : window::PaneFocus::Unfocused));
            return pane;
        };
        switch (shape)
        {
            case PaneShape::SingleLeaf: break;
            case PaneShape::SplitThree: {
                // A split node takes QObject ownership of its children as it is built.
                auto first = leaf(0);
                auto second = leaf(1);
                auto third = leaf(2);
                auto inner =
                    std::make_unique<MockPaneProxy>(/*leaf=*/false, second.release(), third.release());
                return std::make_unique<MockPaneProxy>(/*leaf=*/false, first.release(), inner.release());
            }
        }
        return leaf(ActiveLeaf);
    }

    /// The profiles the palette's scene adds beside the default one, and the layouts it saves (R-m).
    static constexpr auto PaletteProfiles = std::array<std::string_view, 2> { "work", "presentation" };
    static constexpr auto PaletteLayouts = std::array<std::string_view, 2> { "split-dev", "logs" };

    /// The configuration the palette's profile and layout rows come from: the default profile, plus
    /// PaletteProfiles and PaletteLayouts. Apart from the scene's own, so the Settings window keeps
    /// showing the defaults.
    [[nodiscard]] static config::Config makePaletteConfig()
    {
        auto config = config::Config {};
        for (auto const name: PaletteProfiles)
            config.profiles.value().insert_or_assign(std::string(name), config::TerminalProfile {});
        for (auto const name: PaletteLayouts)
            config.layouts.value().insert_or_assign(std::string(name), config::Layout {});
        return config;
    }

    /// The command palette's model over @p sources (the scene's profile, layout and action rows), with the
    /// shortcuts spelled the way the row's host spells them.
    [[nodiscard]] static std::unique_ptr<window::CommandPaletteModel> makePaletteModel(
        command::CommandHistory const& history,
        std::vector<command::CommandSource const*> sources,
        config::Config const& config,
        command::ShortcutStyle shortcuts)
    {
        auto model = std::make_unique<window::CommandPaletteModel>(history, std::move(sources), shortcuts);
        model->refresh(command::shortcutIndex(config, shortcuts));
        return model;
    }

    /// Opens the palette, as the OpenCommandPalette action does, waits until it has finished opening, and
    /// lets the list finish building its rows.
    void openPalette()
    {
        auto const* palette = root().findChild<QObject*>(QStringLiteral("commandPalette"));
        REQUIRE(palette != nullptr);
        _controller.openCommandPalette();
        waitForPopupOpened(*palette);
        // Let the list finish building the rows it shows, so a filter or a level right after does not
        // cancel a row still incubating ("DelegateModel::cancel: index out range").
        _incubation.settle();
        QCoreApplication::processEvents();
    }

    /// Types @p text into the open palette's filter field.
    void filterPalette(QString const& text) const
    {
        auto* filter = root().findChild<QObject*>(QStringLiteral("commandPaletteFilter"));
        REQUIRE(filter != nullptr);
        filter->setProperty("text", text);
        QCoreApplication::processEvents();
    }

    /// Opens the palette inside Change Profile's level: filtered by "profile", the parent found by its id
    /// (not by its rank), entered, and the level shown.
    void drillInPalette()
    {
        openPalette();
        filterPalette(QStringLiteral("profile"));
        auto& model = *_paletteModel;
        auto const rows = std::views::iota(0, model.rowCount());
        auto const parent = std::ranges::find_if(rows, [&](int row) {
            return model
                       .data(model.index(row, 0),
                             static_cast<int>(window::CommandPaletteModel::Roles::IdRole))
                       .toString()
                   == QStringLiteral("ChangeProfile");
        });
        REQUIRE(parent != rows.end());
        model.enter(*parent); // synchronous: the level is listed on return
        REQUIRE(model.depth() == static_cast<int>(command::PaletteDepth::Level));
    }

    /// Gives the mock the band geometry and the traffic lights' inset the controller measures in production
    /// on the row's host: the title bar the row's resolved surface leaves AppKit with (nativeTitleBarFor --
    /// the grown one under the compact toolbar, the bare one for the Terminal row and Minimal's bare band),
    /// and window::resolveTitleBandGeometry over the chrome's tokens and that height. Only on a macOS host
    /// with the client band: elsewhere there is no native title bar under the band (a native frame draws its
    /// own above it). In full screen the title bar leaves the window: the band keeps its token and a revealed
    /// one rests at the window's top (the scene has no AppKit strip to rest below); the inset is published as
    /// the controller publishes it there, and Main.qml applies none.
    void measureBand()
    {
        if (variantHost(_variant) != config::HostPlatform::MacOS
            || scriptRow(_variant.script).frame != SnapshotFrame::ClientBand)
            return;
        auto const mode = scriptRow(_variant.script).mode == SnapshotWindowMode::FullScreenRevealed
                              ? window::WindowMode::FullScreen
                              : window::WindowMode::Windowed;
        auto const* provider = qobject_cast<window::UiStyleProvider const*>(
            _engine.rootContext()->contextProperty(QStringLiteral("chromeStyle")).value<QObject*>());
        REQUIRE(provider != nullptr);
        // The band Main.qml decided: under Minimal an empty one is the bare band, over the bare title bar.
        auto const named = window::barCentreNamed(
            window().property("band").toMap().value(QStringLiteral("centre")).toString().toStdString());
        REQUIRE(named.has_value());
        auto const centre = *named;
        auto const tokens = provider->titleBandTokens(centre);
        auto const surface = window::resolveTitleBandSurface(window::TitleBandSurfaceInputs {
            .style = tokens.style,
            .material = tokens.material,
            .frame = window::WindowFrame::ClientBand,
            .mode = mode,
            .centre = centre,
            .density = tokens.density,
        });
        auto const native = nativeTitleBarFor(surface.growth);
        _controller.setNativeControlsInset(native.trafficLightInset);
        if (mode == window::WindowMode::FullScreen)
            return;
        _measuredTitleBar = native;
        auto const geometry = window::resolveTitleBandGeometry(window::TitleBandGeometryInputs {
            .tokens = tokens,
            .mode = window::WindowMode::Windowed,
            .devicePixelRatio = window().devicePixelRatio(),
            .nativeHeight = native.height,
            .fullScreenTop = 0,
        });
        _controller.setTitleBandHeight(geometry.height);
        _controller.setTitleBandRevealTop(geometry.revealTop);
    }

    /// Shows the window in full screen and rests the pointer on the band's reveal edge, waiting until the
    /// band is revealed.
    void showFullScreenRevealed() const
    {
        // Shown first: macOS takes a window into full screen from its frame, not from nothing.
        window().show();
        REQUIRE(QTest::qWaitForWindowExposed(&window(), FullScreenSettleTimeoutMs));
        window().showFullScreen();
        REQUIRE(QTest::qWaitFor([&] { return window().visibility() == QWindow::FullScreen; }, 3000));
        // macOS animates into full screen after the state changes: settled once the size holds still.
        auto sizes = std::vector<QSize> { window().size() };
        for ([[maybe_unused]] auto const poll:
             std::views::iota(0, FullScreenSettleTimeoutMs / FullScreenSettlePollMs))
        {
            QTest::qWait(FullScreenSettlePollMs);
            sizes.push_back(window().size());
            if (sizes.back() == sizes[sizes.size() - 2] && sizes.back() != SnapshotWindowSize)
                break;
        }
        INFO("full-screen sizes seen: " << sizes.size() << ", last " << sizes.back().width() << "x"
                                        << sizes.back().height());
        REQUIRE(sizes.back() == sizes[sizes.size() - 2]);
        auto const* edge = root().findChild<QQuickItem*>(QStringLiteral("bandRevealEdge"));
        auto const* titleBar = root().findChild<QQuickItem*>(QStringLiteral("titleBar"));
        REQUIRE(edge != nullptr);
        REQUIRE(titleBar != nullptr);
        REQUIRE(QTest::qWaitFor([&] { return edge->isVisible(); }, 2000));
        auto const onEdge =
            edge->mapToScene(QPointF(edge->width() / HalfDivisor, edge->height() / HalfDivisor)).toPoint();
        // Two moves, because a move to where the pointer already is sends nothing.
        QTest::mouseMove(&window(), onEdge + QPoint(0, static_cast<int>(window().height() / HalfDivisor)));
        QTest::mouseMove(&window(), onEdge);
        REQUIRE(QTest::qWaitFor([&] { return titleBar->property("revealed").toBool(); }, 2000));
        // The slide (or, under Reduce Motion, the fade) runs out.
        REQUIRE(QTest::qWaitFor([&] { return titleBar->y() == 0 && titleBar->opacity() == 1; }, 2000));
    }

    /// The hosting notice a script's banner column asks for.
    /// @param banner The script's banner.
    /// @return The notice text, or an empty string when the script shows none.
    [[nodiscard]] static QString hostingNoticeFor(SnapshotBanner banner)
    {
        switch (banner)
        {
            case SnapshotBanner::None: break;
            case SnapshotBanner::Hosting: return QString::fromUtf8(SnapshotHostingNotice);
            case SnapshotBanner::NewDefaults:
            case SnapshotBanner::ConfigReload: break;
        }
        return {};
    }

    /// The offer the new-defaults banner shows for a script's banner column.
    /// @param banner The script's banner.
    /// @return NewDefaultsOffer::ExistingConfig for SnapshotBanner::NewDefaults, None otherwise.
    [[nodiscard]] static config::NewDefaultsOffer newDefaultsOfferFor(SnapshotBanner banner) noexcept
    {
        switch (banner)
        {
            case SnapshotBanner::None:
            case SnapshotBanner::Hosting:
            case SnapshotBanner::ConfigReload: break;
            case SnapshotBanner::NewDefaults: return config::NewDefaultsOffer::ExistingConfig;
        }
        return config::NewDefaultsOffer::None;
    }

    /// The settings page's controller over a store in a temporary directory, or null unless @p surface
    /// is the Settings window.
    [[nodiscard]] std::unique_ptr<window::SettingsController> makeSettings(SnapshotSurface surface) const
    {
        switch (surface)
        {
            case SnapshotSurface::MainWindow: break;
            case SnapshotSurface::SettingsWindow:
                REQUIRE(_settingsDir.isValid());
                return std::make_unique<window::SettingsController>(
                    [this]() -> config::Config const& { return _config; },
                    std::make_shared<config::FileGuiConfigStore>(
                        _settingsFiles, std::filesystem::path(_settingsDir.path().toStdU16String())),
                    [] {},
                    nullptr,
                    vtbackend::ColorPreference::Dark);
        }
        return nullptr;
    }

    /// The Settings window's host over the settings controller, under the variant's row's conventions;
    /// null when the script has no controller, because its surface is the terminal window.
    [[nodiscard]] std::unique_ptr<window::SettingsWindowHost> makeSettingsWindow()
    {
        if (_settings == nullptr)
            return nullptr;
        return std::make_unique<window::SettingsWindowHost>(
            *_settings,
            window::settingsWindowConventionsFor(variantHost(_variant)),
            [this](window::SettingsWindowHost& host) { return window::loadSettingsWindow(_engine, host); });
    }

    /// Posts a copy's toast through the window's notices, as a copy does: Main.qml places and shows it. Waits
    /// for its fade-in to run out, or the picture would catch it invisible.
    void showToast()
    {
        _controller.notices().post(
            window::Notice { .kind = window::NoticeKind::Copied, .text = toastText().toStdString() });
        auto const* toast = _root->findChild<QQuickItem*>(QStringLiteral("noticeToast"));
        REQUIRE(toast != nullptr);
        REQUIRE(QTest::qWaitFor([&] { return toast->opacity() == 1.0; }, PopupSettleMs));
    }

    /// Posts the summary the window the daemon restored is told, through its notices as the manager does
    /// (TerminalSessionManager::deliverRestoreSummaryTo()): Main.qml places and shows it, in the band or the
    /// Terminal row's bar. Waits for its fade-in to run out.
    void showRestoreSummary()
    {
        _controller.notices().post(
            window::Notice { .kind = window::NoticeKind::RestoreSummary,
                             .text = window::restoreSummaryText(RestoredWindows, RestoredTabs) });
        auto const* toast = _root->findChild<QQuickItem*>(QStringLiteral("noticeToast"));
        REQUIRE(toast != nullptr);
        REQUIRE(QTest::qWaitFor([&] { return toast->opacity() == 1.0; }, PopupSettleMs));
    }

    /// The question a script's session asks: what the session publishes for it (permissionPromptMap()).
    [[nodiscard]] static QVariantMap permissionQuestion(SnapshotPermission permission)
    {
        switch (permission)
        {
            case SnapshotPermission::None: break;
            case SnapshotPermission::Paste:
                return session::permissionPromptMap(
                    session::bigPastePrompt(
                        "zsh",
                        SnapshotPasteBytes,
                        session::PastePreview { .lines = { "git log --oneline --graph --decorate --all",
                                                           "cmake --build out/release -j 8" },
                                                .moreLines = SnapshotPasteMoreLines }),
                    1);
            case SnapshotPermission::Consent: {
                // Re-spells vtextension::consentQuestion() rather than calling it: vtextension exists only
                // with CONTOUR_EXTENSIONS, and the snapshots must build without it.
                auto lines = std::vector<session::PromptLine> {
                    session::consentLine("Module: git-stats@/usr/share/contour/extensions/git-stats.wasm",
                                         vtcontrol::ConsentWeight::Observes),
                };
                for (auto const control: { vtcontrol::Permission::Read,
                                           vtcontrol::Permission::Screen,
                                           vtcontrol::Permission::Input })
                {
                    auto const weight = vtcontrol::consentWeightOf(control);
                    lines.push_back(session::consentLine(
                        std::format("May {}{}.",
                                    vtcontrol::consentPhraseOf(control),
                                    weight == vtcontrol::ConsentWeight::CanRunCommands ? " (can run commands)"
                                                                                       : ""),
                        weight));
                }
                return session::permissionPromptMap(
                    session::consentPrompt("“git-stats” wants to control Contour", std::move(lines)), 1);
            }
        }
        return {};
    }

    /// Has the active pane's session ask @p permission, and waits for its dialog to open.
    void openPermission(SnapshotPermission permission)
    {
        REQUIRE_FALSE(_sessions.empty());
        _sessions.front()->setPendingPermission(permissionQuestion(permission));
        REQUIRE(QTest::qWaitFor(
            [&] {
                auto const* dialog = _root->findChild<QObject*>(QStringLiteral("permissionDialog"));
                return dialog != nullptr && dialog->property("opened").toBool();
            },
            PopupSettleMs));
    }

    /// Opens Save Layout as a nameless SaveLayout action does, over this script's tabs, and types the name
    /// into it -- one a layout already has, for SnapshotLayoutName::Taken.
    void openSaveLayout(SnapshotLayoutName name)
    {
        _controller.layoutTabs = scriptRow(_variant.script).tabCount;
        _controller.layoutPanes = scriptRow(_variant.script).tabCount
                                  * static_cast<int>(paneCount(scriptRow(_variant.script).panes));
        _controller.existingLayouts.clear();
        if (name == SnapshotLayoutName::Taken)
            _controller.existingLayouts.emplace_back(SnapshotLayoutTyped);
        _controller.beginSaveLayoutPrompt();
        auto* dialog = _root->findChild<QObject*>(QStringLiteral("saveLayoutDialog"));
        REQUIRE(dialog != nullptr);
        REQUIRE(QTest::qWaitFor([&] { return dialog->property("opened").toBool(); }, PopupSettleMs));
        auto const* content = dialog->property("contentItem").value<QQuickItem*>();
        REQUIRE(content != nullptr);
        auto const fields = itemsNamed(*content, QStringLiteral("saveLayoutNameField"));
        REQUIRE(fields.size() == 1);
        fields.front()->setProperty("text", QString::fromUtf8(SnapshotLayoutTyped));
    }

    /// Creates the dialog over the window's overlay, with a destructive accept, and waits for it to open.
    void openKitDialog()
    {
        auto component =
            QQmlComponent(&_engine, QUrl(QStringLiteral("qrc:/qt/qml/Contour/Ui/KitDialog.qml")));
        INFO("KitDialog.qml errors: " << component.errorString().toStdString());
        REQUIRE(component.isReady());
        auto* style = _engine.rootContext()->contextProperty(QStringLiteral("chromeStyle")).value<QObject*>();
        REQUIRE(style != nullptr);
        auto dialog = std::unique_ptr<QObject>(component.createWithInitialProperties(QVariantMap {
            { QStringLiteral("parent"), QVariant::fromValue(window().contentItem()) },
            { QStringLiteral("heading"), dialogHeading() },
            { QStringLiteral("message"), dialogMessage() },
            { QStringLiteral("acceptText"), dialogAcceptText() },
            { QStringLiteral("rejectText"), dialogRejectText() },
            { QStringLiteral("acceptRole"), style->property("statusDanger") },
        }));
        REQUIRE(dialog != nullptr);
        REQUIRE(QMetaObject::invokeMethod(dialog.get(), "open"));
        auto const* opened = dialog.get();
        REQUIRE(QTest::qWaitFor([&] { return opened->property("opened").toBool(); }, 2000));
        _overlay = std::move(dialog);
    }

    /// Raises the About dialog the way the ShowAbout action does, and waits for it to open.
    void openAbout()
    {
        auto const* dialog = root().findChild<QObject*>(QStringLiteral("aboutDialog"));
        REQUIRE(dialog != nullptr);
        _controller.showAbout();
        REQUIRE(QTest::qWaitFor([&] { return dialog->property("opened").toBool(); }, 2000));
    }

    /// Opens the second tab's context menu below its tab, then its "Tab Color" sub-menu, and waits for both.
    void openTabMenu()
    {
        auto* menu = root().findChild<QObject*>(QStringLiteral("tabContextMenu"));
        REQUIRE(menu != nullptr);
        _controller.openTabContextMenu(SnapshotMenuTab, {});
        REQUIRE(QTest::qWaitFor([&] { return menu->property("opened").toBool(); }, 2000));

        auto* colorRow = subMenuRow(*menu, tabColorTitle());
        REQUIRE(colorRow != nullptr);
        auto const* colors = colorRow->property("subMenu").value<QObject*>();
        // Triggering a sub-menu's row is what a click on it does: the menu opens the sub-menu beside it.
        REQUIRE(QMetaObject::invokeMethod(colorRow, "triggered"));
        REQUIRE(QTest::qWaitFor([&] { return colors->property("opened").toBool(); }, 2000));
    }

    /// Opens the second tab's colour flyout the way the SetTabColor action does for its tab, and waits
    /// for it.
    void openTabColorFlyout()
    {
        // A delegate has no QObject parent, so the flyout is reached through the list that made it.
        auto* list = root().findChild<QQuickItem*>(QStringLiteral("tabList"));
        REQUIRE(list != nullptr);
        // The window was only just shown: the strip is laid out over the next frames.
        QQuickItem* tab = nullptr;
        auto const realized = [&] {
            QMetaObject::invokeMethod(list, "forceLayout");
            QMetaObject::invokeMethod(
                list, "itemAtIndex", Q_RETURN_ARG(QQuickItem*, tab), Q_ARG(int, SnapshotMenuTab));
            return tab != nullptr;
        };
        REQUIRE(QTest::qWaitFor(realized, 2000));
        auto* flyout = tab->findChild<QObject*>(QStringLiteral("tabColorFlyout"));
        REQUIRE(flyout != nullptr);
        emit _controller.tabColorPickRequested(SnapshotMenuTab);
        REQUIRE(QTest::qWaitFor([&] { return flyout->property("opened").toBool(); }, 2000));
        _tabColorFlyout = flyout;
    }

    /// Rests the pointer on @p point of the window. Two moves, because a move to where the pointer already
    /// is sends nothing.
    void pointAt(QPoint point) const
    {
        QTest::mouseMove(&window(), point + QPoint(0, 1));
        QTest::mouseMove(&window(), point);
    }

    /// Rests the pointer on the active pane's header, waiting for its actions, and then on the gap beside the
    /// pane, at the header's height, waiting for the split handle to engage and its grabber to fade in.
    ///
    /// The pane is first focused by keyboard, as a FocusPane* action reaches it: on the native rows the gap's
    /// hit area reaches into the pane, and the handle takes the hover from it there, so a header shown by the
    /// pointer alone would fade as the pointer reaches the gap. Keyboard focus keeps the actions shown.
    void hoverPaneHeader()
    {
        auto const* content = window().contentItem();
        auto const cards = itemsNamed(*content, QStringLiteral("paneCard"));
        auto const active = std::ranges::find_if(
            cards, [](QQuickItem const* card) { return card->property("isActive").toBool(); });
        REQUIRE(active != cards.end());
        auto const* card = *active;
        auto* node = qobject_cast<MockPaneProxy*>(card->property("node").value<QObject*>());
        REQUIRE(node != nullptr);
        node->setFocus(static_cast<int>(window::PaneFocus::KeyboardFocused));
        auto const* header = card->findChild<QQuickItem*>(QStringLiteral("paneHeader"));
        REQUIRE(header != nullptr);
        // The window was only just shown: the panes are laid out, and the header decided, over the next
        // frames.
        _incubation.settle();
        REQUIRE(QTest::qWaitFor([&] { return header->isVisible(); }, 2000));
        auto const* actions = header->findChild<QQuickItem*>(QStringLiteral("paneHeaderActions"));
        REQUIRE(actions != nullptr);
        auto const headerCentre =
            header->mapToScene(QPointF(header->width() / HalfDivisor, header->height() / HalfDivisor));
        pointAt(headerCentre.toPoint());
        REQUIRE(QTest::qWaitFor([&] { return actions->opacity() == 1; }, 2000));

        // The gap at the card's trailing edge.
        auto const cardRight = card->mapToScene(QPointF(card->width(), 0)).x();
        auto const handles = itemsNamed(*content, QStringLiteral("splitHandle"));
        // The handle whose scene rectangle holds the point just past the card's edge, level with its header.
        auto const target = QPointF(cardRight + 1, headerCentre.y());
        auto const owner = std::ranges::find_if(handles, [&](QQuickItem const* handle) {
            return handle->mapRectToScene(handle->boundingRect()).contains(target);
        });
        REQUIRE(owner != handles.end());
        auto const* handle = *owner;
        auto const* grabber = handle->findChild<QQuickItem*>(QStringLiteral("splitGrabber"));
        REQUIRE(grabber != nullptr);
        pointAt(QPointF(handle->mapToScene(QPointF(handle->width() / HalfDivisor, 0)).x(), headerCentre.y())
                    .toPoint());
        REQUIRE(QTest::qWaitFor([&] { return handle->property("engaged").toBool(); }, 2000));
        REQUIRE(QTest::qWaitFor([&] { return grabber->opacity() == 1 && actions->opacity() == 1; }, 2000));
    }

    /// The row of @p menu that opens the sub-menu titled @p title.
    /// @return The row, or null when @p menu has no such sub-menu.
    [[nodiscard]] static QQuickItem* subMenuRow(QObject& menu, QString const& title)
    {
        for (auto const index: std::views::iota(0, menu.property("count").toInt()))
        {
            QQuickItem* item = nullptr;
            QMetaObject::invokeMethod(&menu, "itemAt", Q_RETURN_ARG(QQuickItem*, item), Q_ARG(int, index));
            auto const* subMenu = item != nullptr ? item->property("subMenu").value<QObject*>() : nullptr;
            if (subMenu != nullptr && subMenu->property("title").toString() == title)
                return item;
        }
        return nullptr;
    }

    /// Loads Main.qml against the controller under the variant's chrome row.
    [[nodiscard]] std::unique_ptr<QObject> load()
    {
        _engine.setIncubationController(&_incubation);
        auto const chrome = ChromeSetup {
            .row = _variant.row,
            .font = _chromeFont,
            .windowControls =
                config::resolveWindowControlStyle(config::WindowControlStyle::Auto, variantHost(_variant)),
            .accessibility = { .transparency = scriptRow(_variant.script).transparency },
            .appearance = {},
            .scheme = config::chromeSchemeColorsOf(
                *config::preferredColorPalette(config::defaultColors(_variant.edition), _variant.appearance)),
            .density = scriptRow(_variant.script).density,
        };
        // The panes the active tab shows, as WindowController counts them: two or more make cards.
        _controller.setVisiblePaneCount(static_cast<int>(paneCount(scriptRow(_variant.script).panes)));
        _engine.rootContext()->setContextProperty(QStringLiteral("newDefaults"), &_newDefaults);
        auto root = loadMainWindow(_engine, _controller, _warnings, chrome);
        REQUIRE(qobject_cast<QQuickWindow*>(root.get()) != nullptr);
        _incubation.settle();
        return root;
    }

    ScopedChromeAppearance _appearance; ///< First: the palette is given back after everything else.
    SnapshotVariant _variant;
    config::Config _config;
    SnapshotLook _look;
    QFont _chromeFont;
    QmlMessageCapture _warnings;
    std::vector<std::unique_ptr<MockSession>> _sessions;
    std::unique_ptr<MockPaneProxy> _rootPane;
    command::CommandHistory _history;
    config::Config _paletteConfig; ///< The palette's profiles and layouts (makePaletteConfig()).
    command::ProfileCommandSource _profileCommands;
    command::LayoutCommandSource _layoutCommands;
    command::ActionCommandSource _actionCommands;
    std::unique_ptr<window::CommandPaletteModel> _paletteModel;
    core::platform::NativeFileSystem _settingsFiles; ///< What the Settings controller's store writes through.
    QTemporaryDir _settingsDir;
    std::unique_ptr<window::SettingsController> _settings;
    MockMainController _controller;
    MockNewDefaultsNotice _newDefaults;
    QQmlEngine _engine;
    /// The Settings window over *_settings, on _engine; null unless the script's surface is that window.
    std::unique_ptr<window::SettingsWindowHost> _settingsWindow;
    IncubationDriver _incubation;
    std::unique_ptr<QObject> _root; ///< The QML tree dies before everything it borrows.
    /// Last: openKitDialog()'s dialog, parented into _root's tree, dies first.
    std::unique_ptr<QObject> _overlay;
    QObject const* _tabColorFlyout = nullptr; ///< The flyout openTabColorFlyout() opened; owned by _root.
    std::optional<SnapshotNativeTitleBar>
        _measuredTitleBar; ///< What measureBand() measured over, if anything.
};

} // namespace contour::test::snapshot
