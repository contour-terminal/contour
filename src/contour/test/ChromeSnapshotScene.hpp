// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/command/CommandCatalog.hpp>
#include <contour/command/CommandHistory.hpp>
#include <contour/command/Shortcut.hpp>
#include <contour/config/Config.hpp>
#include <contour/config/GuiConfigStore.hpp>
#include <contour/config/NewDefaultsOffer.hpp>
#include <contour/config/TabBarMode.hpp>
#include <contour/config/WindowControlStyle.hpp>
#include <contour/platform/ColorConversion.hpp>
#include <contour/test/ChromeSnapshotPlan.hpp>
#include <contour/test/MockMainController.hpp>
#include <contour/test/QmlChromeStyle.hpp>
#include <contour/test/QmlIncubation.hpp>
#include <contour/test/QmlMessageCapture.hpp>
#include <contour/test/QmlMocks.hpp>
#include <contour/window/CommandPaletteModel.hpp>
#include <contour/window/SettingsController.hpp>
#include <contour/window/SettingsWindowHost.hpp>
#include <contour/window/SettingsWindowRules.hpp>
#include <contour/window/UiStyleProvider.hpp>

#include <core/platform/FileSystem.hpp>

#include <QtCore/QByteArray>
#include <QtCore/QCoreApplication>
#include <QtCore/QSize>
#include <QtCore/QStringList>
#include <QtCore/QTemporaryDir>
#include <QtCore/QUrl>
#include <QtCore/QVariantMap>
#include <QtGui/QFont>
#include <QtGui/QFontDatabase>
#include <QtGui/QGuiApplication>
#include <QtGui/QImage>
#include <QtQml/QQmlComponent>
#include <QtQml/QQmlContext>
#include <QtQml/QQmlEngine>
#include <QtQuick/QQuickItem>
#include <QtQuick/QQuickWindow>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
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
        _paletteModel { makePaletteModel(
            _history, _actionCommands, _config, command::shortcutStyleFor(rowHost(variant.row).host)) },
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
        } },
        _newDefaults { newDefaultsOfferFor(scriptRow(variant.script).banner) },
        _settingsWindow { makeSettingsWindow() },
        _root { load() }
    {
    }

    ChromeSnapshotScene(ChromeSnapshotScene const&) = delete;
    ChromeSnapshotScene& operator=(ChromeSnapshotScene const&) = delete;
    ChromeSnapshotScene(ChromeSnapshotScene&&) = delete;
    ChromeSnapshotScene& operator=(ChromeSnapshotScene&&) = delete;
    ~ChromeSnapshotScene() = default;

    /// @return The object the script's action created over the window, or null when it created none.
    [[nodiscard]] QObject const* overlay() const noexcept { return _overlay.get(); }

    /// @return The tab colour flyout the script's action opened, or null when it opened none.
    [[nodiscard]] QObject const* openedTabColorFlyout() const noexcept { return _tabColorFlyout; }

    /// Shows the window at SnapshotWindowSize, opens the Settings window over it when the script's surface
    /// is that one, performs the script's action, and lets the scene settle.
    void show()
    {
        window().resize(SnapshotWindowSize);
        window().show();
        if (_settingsWindow != nullptr)
            REQUIRE(_settingsWindow->show(nullptr).has_value());
        switch (scriptRow(_variant.script).action)
        {
            case ScriptAction::None: break;
            case ScriptAction::OpenPalette: {
                auto const* palette = root().findChild<QObject*>(QStringLiteral("commandPalette"));
                REQUIRE(palette != nullptr);
                _controller.openCommandPalette();
                REQUIRE(QTest::qWaitFor([&] { return palette->property("opened").toBool(); }, 2000));
                break;
            }
            case ScriptAction::ShowToast: showToast(); break;
            case ScriptAction::OpenKitDialog: openKitDialog(); break;
            case ScriptAction::OpenTabMenu: openTabMenu(); break;
            case ScriptAction::OpenAbout: openAbout(); break;
            case ScriptAction::OpenTabColorFlyout: openTabColorFlyout(); break;
        }
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

  private:
    /// How many recent commands the palette remembers, as MockPaletteController remembers them.
    static constexpr std::size_t PaletteHistoryCapacity = 5;

    /// The toast's message.
    static inline auto const ToastText = QStringLiteral("Copied 3 lines");

    /// The dialog's texts.
    static inline auto const DialogHeading = QStringLiteral("Close this tab?");
    static inline auto const DialogMessage =
        QStringLiteral("\u201Ccmake\u201D is still running. Closing the tab will end it.");
    static inline auto const DialogAcceptText = QStringLiteral("Close Tab");
    static inline auto const DialogRejectText = QStringLiteral("Cancel");

    /// The tab whose context menu the tab-menu script opens, and whose colour flyout the tab-color-flyout
    /// script opens: the middle of three.
    static constexpr int SnapshotMenuTab = 1;

    /// The title of the tab menu's colour sub-menu.
    static inline auto const TabColorTitle = QStringLiteral("Tab Color");

    /// What halves a length to centre an item.
    static constexpr qreal HalfDivisor = 2;

    /// The leaf that holds focus in every pane shape.
    static constexpr std::size_t ActiveLeaf = 0;

    /// How many panes @p shape divides a tab into.
    [[nodiscard]] static constexpr std::size_t leafCount(PaneShape shape) noexcept
    {
        switch (shape)
        {
            case PaneShape::SingleLeaf: break;
            case PaneShape::SplitThree: return 3;
        }
        return 1;
    }

    /// One session per leaf of @p script's pane shape, in the look's background; every one but the
    /// active leaf's dimmed as unfocused, and all of them reconnecting when the host connection is.
    [[nodiscard]] static std::vector<std::unique_ptr<MockSession>> makeSessions(
        SnapshotScriptRow const& script, SnapshotLook const& look)
    {
        auto sessions = std::vector<std::unique_ptr<MockSession>> {};
        for (auto const leaf: std::views::iota(std::size_t { 0 }, leafCount(script.panes)))
        {
            auto session = std::make_unique<MockSession>(platform::toQColor(look.terminalBackground));
            if (leaf != ActiveLeaf)
                session->setDimUnfocused(static_cast<float>(look.dimUnfocused));
            if (script.connection == HostConnection::Reconnecting)
                session->setReconnecting(true);
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

    /// The command palette's model, wired as MockPaletteController wires it, with the shortcuts spelled
    /// the way the row's host spells them.
    [[nodiscard]] static std::unique_ptr<window::CommandPaletteModel> makePaletteModel(
        command::CommandHistory const& history,
        command::ActionCommandSource const& actions,
        config::Config const& config,
        command::ShortcutStyle shortcuts)
    {
        auto model = std::make_unique<window::CommandPaletteModel>(history);
        model->setSources({ &actions });
        model->setShortcuts(command::shortcutIndex(config, shortcuts));
        model->refresh();
        return model;
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
            case SnapshotBanner::NewDefaults: break;
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
            case SnapshotBanner::Hosting: break;
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
                        std::filesystem::path(_settingsDir.path().toStdU16String())),
                    [] {},
                    nullptr);
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
            window::settingsWindowConventionsFor(rowHost(_variant.row).host),
            [this](window::SettingsWindowHost& host) { return window::loadSettingsWindow(_engine, host); });
    }

    /// Creates the toast over the content, centred horizontally at its top, and shows it.
    void showToast()
    {
        auto component = QQmlComponent(&_engine, QUrl(QStringLiteral("qrc:/qt/qml/Contour/Ui/Toast.qml")));
        INFO("Toast.qml errors: " << component.errorString().toStdString());
        REQUIRE(component.isReady());
        auto overlay = std::unique_ptr<QObject>(component.createWithInitialProperties(
            QVariantMap { { QStringLiteral("text"), ToastText }, { QStringLiteral("shown"), true } }));
        auto* item = qobject_cast<QQuickItem*>(overlay.get());
        REQUIRE(item != nullptr);
        auto* content = window().contentItem();
        item->setParentItem(content);
        item->setX((content->width() - item->implicitWidth()) / HalfDivisor);
        item->setWidth(item->implicitWidth());
        _overlay = std::move(overlay);
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
            { QStringLiteral("heading"), DialogHeading },
            { QStringLiteral("message"), DialogMessage },
            { QStringLiteral("acceptText"), DialogAcceptText },
            { QStringLiteral("rejectText"), DialogRejectText },
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

        auto* colorRow = subMenuRow(*menu, TabColorTitle);
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
            .windowControls = config::resolveWindowControlStyle(config::WindowControlStyle::Auto,
                                                                rowHost(_variant.row).host),
            .accessibility = {},
            .appearance = {},
            .scheme = config::chromeSchemeColorsOf(
                *config::preferredColorPalette(config::defaultColors(_variant.edition), _variant.appearance)),
        };
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
    command::ActionCommandSource _actionCommands;
    std::unique_ptr<window::CommandPaletteModel> _paletteModel;
    QTemporaryDir _settingsDir;
    std::unique_ptr<window::SettingsController> _settings;
    MockMainController _controller;
    MockNewDefaultsNotice _newDefaults;
    QQmlEngine _engine;
    /// The Settings window over *_settings, on _engine; null unless the script's surface is that window.
    std::unique_ptr<window::SettingsWindowHost> _settingsWindow;
    IncubationDriver _incubation;
    std::unique_ptr<QObject> _root;           ///< The QML tree dies before everything it borrows.
    std::unique_ptr<QObject> _overlay;        ///< Last: the toast, parented into _root's tree, dies first.
    QObject const* _tabColorFlyout = nullptr; ///< The flyout openTabColorFlyout() opened; owned by _root.
};

} // namespace contour::test::snapshot
