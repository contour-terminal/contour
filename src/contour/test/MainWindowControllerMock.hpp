// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/command/MenuTree.hpp>
#include <contour/command/Shortcut.hpp>
#include <contour/command/TabMenus.hpp>
#include <contour/platform/ColorConversion.hpp>
#include <contour/window/ContextMenuModel.hpp>
#include <contour/window/RemoteChrome.hpp>
#include <contour/window/TabColorScheme.hpp>

#include <vtbackend/core/Color.hpp>

#include <QtCore/QAbstractListModel>
#include <QtCore/QPointF>
#include <QtCore/QString>
#include <QtCore/QVariantList>
#include <QtGui/QColor>

#include <array>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

namespace contour::test
{

/// The colours a mock offers in its tab menu's "Tab Color" sub-menu, in order.
inline constexpr auto MockTabPalette = std::array {
    vtbackend::RGBColor { 0xEE6666u }, vtbackend::RGBColor { 0xF0A050u }, vtbackend::RGBColor { 0xE8D05Au },
    vtbackend::RGBColor { 0x7CC87Cu }, vtbackend::RGBColor { 0x5AB4D8u }, vtbackend::RGBColor { 0x6A8CE8u },
    vtbackend::RGBColor { 0xB07CE0u }, vtbackend::RGBColor { 0xA0A0A0u },
};

/// The profiles a mock offers in its new-tab menu, in order.
inline constexpr auto MockProfileNames =
    std::array { std::string_view { "dark" }, std::string_view { "light" } };

/// The tab menu a mock publishes, built by the real table: the right-clicked tab wears the palette's
/// first colour, so its swatch is the checked one.
/// @param index The right-clicked tab.
/// @param count The tabs in the window.
/// @return The menu as QML sees it.
[[nodiscard]] inline QVariantList mockTabContextMenuModel(int index, int count)
{
    auto const state = command::TabContextMenuState {
        .tabIndex = index,
        .tabCount = count,
        .palette = { MockTabPalette.begin(), MockTabPalette.end() },
        .color = MockTabPalette.front(),
        .userColor = command::UserColorPresence::Present,
    };
    return window::toContextMenuModel(
        command::buildMenuTree(command::buildTabContextMenu(state), {}, command::ShortcutStyle::Text).nodes);
}

/// The new-tab menu a mock publishes, built by the real table over @ref MockProfileNames.
/// @return The menu as QML sees it.
[[nodiscard]] inline QVariantList mockNewTabMenuModel()
{
    return window::toContextMenuModel(
        command::buildMenuTree(
            command::buildNewTabMenu(
                { .profileNames = std::ranges::to<std::vector<std::string>>(MockProfileNames) }),
            {},
            command::ShortcutStyle::Text)
            .nodes);
}

/// What WindowController::tabBackgroundColor() answers, for a mock to answer the same: the WT-style fill
/// of a coloured tab on the native Flat strip. The flags mirror the invokable QML calls.
/// @return The fill, from the same TabColorScheme math as the real one.
[[nodiscard]] inline QColor mockTabBackgroundColor(
    QColor const& tabColor, QColor const& rowBackground, bool active, bool hovered, bool windowActive)
{
    auto const state = [&] {
        if (active)
            return window::TabVisualState::Active;
        if (hovered)
            return window::TabVisualState::Hover;
        return windowActive ? window::TabVisualState::Inactive
                            : window::TabVisualState::InactiveWindowUnfocused;
    }();
    return platform::toQColor(window::tabBackgroundColor(
        platform::toRGBColor(tabColor), platform::toRGBColor(rowBackground), state));
}

/// What WindowController::tabTextColor() answers: the label that reads on a coloured fill.
[[nodiscard]] inline QColor mockTabTextColor(QColor const& tabBackground)
{
    return platform::toQColor(window::contrastingTextColor(platform::toRGBColor(tabBackground)));
}

/// The window-controller surface Main.qml's own Connections bind, shared by every offscreen mock that
/// loads Main.qml in place of the real WindowController.
///
/// A signal a Connections handler names but the target lacks is a QML warning, which the run-wide gate
/// turns into a failure of the whole suite. Declaring the signals once keeps each mock in step with
/// Main.qml: a handler added there is a line here, not a line in every mock.
class MainWindowControllerMock: public QAbstractListModel
{
    Q_OBJECT
    // Mirrors WindowController's remote chrome, which Main.qml's title and the tab strip's badge read.
    Q_PROPERTY(QString hostLabel READ hostLabel CONSTANT)
    // Mirrors WindowController's host chip state (a window::HostChipState); a test sets it to drive the chip.
    Q_PROPERTY(int hostChipState READ hostChipState WRITE setHostChipState NOTIFY hostChipStateChanged)
    // Mirrors WindowController's version string, which the About dialog shows.
    Q_PROPERTY(QString applicationVersion READ applicationVersion CONSTANT)
    // Mirrors WindowController's tab and new-tab menu surfaces, which TabStrip.qml binds and
    // Connections-targets.
    Q_PROPERTY(QVariantList tabContextMenuModel READ tabContextMenuModel NOTIFY tabContextMenuModelChanged)
    Q_PROPERTY(QVariantList newTabMenuModel READ newTabMenuModel NOTIFY newTabMenuModelChanged)

  public:
    using QAbstractListModel::QAbstractListModel;

    /// Mirrors WindowController::hostLabel(): the attached host, set by a test before Main.qml loads.
    [[nodiscard]] QString const& hostLabel() const noexcept { return attachedHost; }

    /// Mirrors WindowController::hostChipState().
    [[nodiscard]] int hostChipState() const noexcept { return _hostChipState; }

    /// What the connection reports, as WindowController::notifyHostConnectionChanged() would tell it.
    /// @param state A window::HostChipState as its underlying value.
    void setHostChipState(int state)
    {
        if (_hostChipState == state)
            return;
        _hostChipState = state;
        emit hostChipStateChanged();
    }

    /// Mirrors WindowController::applicationVersion() with a fixed test value.
    [[nodiscard]] QString applicationVersion() const { return QStringLiteral("0.0.0-test"); }

    /// Mirrors WindowController::showAbout(): asks the window for its About dialog.
    Q_INVOKABLE void showAbout() { emit aboutRequested(); }

    /// Mirrors WindowController::windowTitle(), through the same remoteWindowTitle().
    Q_INVOKABLE [[nodiscard]] QString windowTitle(QString const& sessionTitle) const
    {
        return QString::fromStdString(
            contour::window::remoteWindowTitle(attachedHost.toStdString(), sessionTitle.toStdString()));
    }

    QString attachedHost; ///< The host hostLabel() reports; empty for a local window.

    /// Mirrors WindowController::trayMessageClicked(), counting the calls.
    Q_INVOKABLE void trayMessageClicked() { ++trayClickCount; }

    /// Mirrors WindowController::requestNotification(): shows the message as the real one does.
    Q_INVOKABLE void requestNotification(QString const& title, QString const& body)
    {
        emit notificationRequested(title, body);
    }

    int trayClickCount = 0; ///< How many tray message clicks Main.qml passed on to the window.

    /// @return The tab menu last published by openTabContextMenu().
    [[nodiscard]] QVariantList const& tabContextMenuModel() const noexcept { return _tabContextMenuModel; }
    /// @return The new-tab menu last published by openNewTabMenu().
    [[nodiscard]] QVariantList const& newTabMenuModel() const noexcept { return _newTabMenuModel; }

    /// Mirrors WindowController::openTabContextMenu(): publishes the tab's menu, then asks for it.
    Q_INVOKABLE void openTabContextMenu(int index, QPointF /*windowPosition*/)
    {
        _tabContextMenuModel = mockTabContextMenuModel(index, rowCount());
        emit tabContextMenuModelChanged();
        emit tabContextMenuRequested(index);
    }

    /// Mirrors WindowController::triggerTabContextMenuAction(), recording the pick.
    Q_INVOKABLE void triggerTabContextMenuAction(int actionId) { tabMenuPicks.push_back(actionId); }

    /// Mirrors WindowController::openNewTabMenu(): publishes the menu, then asks for it.
    Q_INVOKABLE void openNewTabMenu(QPointF /*windowPosition*/)
    {
        _newTabMenuModel = mockNewTabMenuModel();
        emit newTabMenuModelChanged();
        emit newTabMenuRequested();
    }

    /// Mirrors WindowController::triggerNewTabMenuAction(), recording the pick.
    Q_INVOKABLE void triggerNewTabMenuAction(int actionId) { newTabMenuPicks.push_back(actionId); }

    std::vector<int> tabMenuPicks {};    ///< Every tab menu pick, in order.
    std::vector<int> newTabMenuPicks {}; ///< Every new-tab menu pick, in order.

  signals:
    void hostChipStateChanged();
    void commandPaletteRequested();
    void contextMenuRequested();
    void titleBarContextMenuRequested();
    void saveLayoutRequested();
    // The window's attention on behalf of a session that may have no pane on screen.
    void notificationRequested(QString const& title, QString const& body);
    void bellRequested(QString const& source, double volume);
    void alertRequested();
    void tabContextMenuModelChanged();
    void tabContextMenuRequested(int index);
    void newTabMenuModelChanged();
    void newTabMenuRequested();
    void aboutRequested();

  private:
    QVariantList _tabContextMenuModel {};
    QVariantList _newTabMenuModel {};
    int _hostChipState = 0; ///< @see hostChipState; 0 = HostChipState::Connected.
};

} // namespace contour::test
