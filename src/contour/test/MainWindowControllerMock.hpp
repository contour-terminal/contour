// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/askpass/AskpassPromptRouter.hpp>
#include <contour/command/MenuTree.hpp>
#include <contour/command/Shortcut.hpp>
#include <contour/command/TabMenus.hpp>
#include <contour/config/ChromeTokenGroups.hpp>
#include <contour/config/TabBarMode.hpp>
#include <contour/config/WindowControlStyle.hpp>
#include <contour/platform/CaptionHitTest.hpp>
#include <contour/platform/ColorConversion.hpp>
#include <contour/test/SidebarFakes.hpp>
#include <contour/window/CloseDialogTable.hpp>
#include <contour/window/ContextMenuModel.hpp>
#include <contour/window/LocationChip.hpp>
#include <contour/window/LocationChipModel.hpp>
#include <contour/window/LocationChipSteps.hpp>
#include <contour/window/NodeDrag.hpp>
#include <contour/window/NoticeModel.hpp>
#include <contour/window/PromptHost.hpp>
#include <contour/window/RemoteChrome.hpp>
#include <contour/window/SidebarModel.hpp>
#include <contour/window/StatusUrgency.hpp>
#include <contour/window/TabAttention.hpp>
#include <contour/window/TabBarDropPolicy.hpp>
#include <contour/window/TabBarHitValue.hpp>
#include <contour/window/TabColorScheme.hpp>
#include <contour/window/TitleBandGeometry.hpp>
#include <contour/window/ToastPlacement.hpp>

#include <vtbackend/core/Color.hpp>

#include <core/platform/Clock.hpp>

#include <QtCore/QAbstractListModel>
#include <QtCore/QList>
#include <QtCore/QPointF>
#include <QtCore/QPointer>
#include <QtCore/QRectF>
#include <QtCore/QString>
#include <QtCore/QVariant>
#include <QtCore/QVariantList>
#include <QtGui/QColor>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
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
    Q_PROPERTY(QString hostLabel READ hostLabel NOTIFY hostLabelChanged)
    /// Mirrors WindowController's switcher hosting: the switcher presented, and whether it hangs from the
    /// chip.
    Q_PROPERTY(QObject* switcher READ switcher NOTIFY switcherChanged)
    Q_PROPERTY(bool isWorkspaceDropdownOpen READ isWorkspaceDropdownOpen NOTIFY switcherChanged)
    // Mirrors WindowController's host chip state (a window::HostChipState); a test sets it to drive the chip.
    Q_PROPERTY(int hostChipState READ hostChipState WRITE setHostChipState NOTIFY hostChipStateChanged)
    // Mirrors WindowController's location chip: the model the real window owns, built from this mock's
    // inputs. A test sets daemonName (empty: an in-process window), the host chip state and the sidebar to
    // drive it.
    Q_PROPERTY(contour::window::LocationChipModel* chip READ chip CONSTANT)
    // Mirrors WindowController's version string, which the About dialog shows.
    Q_PROPERTY(QString applicationVersion READ applicationVersion CONSTANT)
    // Mirrors WindowController's host for ssh's login prompts, which Main.qml's AskpassDialog shows.
    Q_PROPERTY(QObject* promptHost READ promptHost CONSTANT)
    // Mirrors WindowController's tab and new-tab menu surfaces, which TabStrip.qml binds and
    // Connections-targets.
    Q_PROPERTY(QVariantList tabContextMenuModel READ tabContextMenuModel NOTIFY tabContextMenuModelChanged)
    // The sidebar rows' menu surface: Main.qml binds an ActionContextMenu to sidebarContextMenuModel and pops
    // it on sidebarContextMenuRequested, so every controller Main.qml loads against carries both.
    Q_PROPERTY(QVariantList sidebarContextMenuModel READ sidebarContextMenuModel NOTIFY
                   sidebarContextMenuModelChanged)
    Q_PROPERTY(QVariantList newTabMenuModel READ newTabMenuModel NOTIFY newTabMenuModelChanged)
    // {{{ Panes
    // Mirrors WindowController's visible pane count; a test sets it to drive the pane headers.
    Q_PROPERTY(
        int visiblePaneCount READ visiblePaneCount WRITE setVisiblePaneCount NOTIFY visiblePaneCountChanged)
    // Mirrors WindowController's held-modifier reveal (a window::HeaderReveal); a test sets it to show the
    // headers Minimal hides.
    Q_PROPERTY(int headerReveal READ headerReveal WRITE setHeaderReveal NOTIFY headerRevealChanged)
    // }}}
    // Mirrors WindowController's measured title band: 0 (the band keeps its token height, at the window's
    // top) unless a test sets them, as the material's measurement would.
    Q_PROPERTY(qreal titleBandHeight READ titleBandHeight WRITE setTitleBandHeight NOTIFY titleBandChanged)
    Q_PROPERTY(
        qreal titleBandRevealTop READ titleBandRevealTop WRITE setTitleBandRevealTop NOTIFY titleBandChanged)
    // Mirrors WindowController's titleBandFill: undefined in QML (Main.qml takes the row's own fill) unless a
    // test sets it, as a window whose material failed would.
    Q_PROPERTY(QVariant titleBandFill READ titleBandFill NOTIFY titleBandChanged)
    // Mirrors WindowController's notices: a real NoticeModel, on a clock the test moves, saying nothing.
    Q_PROPERTY(contour::window::NoticeModel* notices READ noticesForQml CONSTANT)
    // Mirrors WindowController's maximise button as the OS drives it; a test sets it with
    // setNativeMaximizeVisual().
    Q_PROPERTY(bool nativeMaximizeHovered READ nativeMaximizeHovered NOTIFY nativeMaximizeVisualChanged)
    Q_PROPERTY(bool nativeMaximizePressed READ nativeMaximizePressed NOTIFY nativeMaximizeVisualChanged)
    // {{{ The workspace sidebar
    // Mirrors WindowController's: a real SidebarModel over a fake window (sidebarSource(), sidebarHost()),
    // hidden until a test shows it (showSidebar()).
    Q_PROPERTY(QObject* sidebar READ sidebarObject CONSTANT)
    Q_PROPERTY(int sidebarVisibility READ sidebarVisibility NOTIFY sidebarVisibilityChanged)
    Q_PROPERTY(int sidebarWidth READ sidebarWidth NOTIFY sidebarWidthChanged)
    Q_PROPERTY(int tabPlacement READ tabPlacement NOTIFY tabPlacementChanged)
    Q_PROPERTY(int otherWorkspacesUrgency READ otherWorkspacesUrgency NOTIFY otherWorkspacesUrgencyChanged)
    Q_PROPERTY(QVariantMap sidebarCommands READ sidebarCommands CONSTANT)
    // }}}

  public:
    /// @param parent The QObject parent.
    explicit MainWindowControllerMock(QObject* parent = nullptr): QAbstractListModel(parent)
    {
        // The chip follows what the window's own controller feeds it from.
        for (auto const signal: { &MainWindowControllerMock::hostLabelChanged,
                                  &MainWindowControllerMock::hostChipStateChanged,
                                  &MainWindowControllerMock::switcherChanged,
                                  &MainWindowControllerMock::sidebarVisibilityChanged,
                                  &MainWindowControllerMock::sidebarWidthChanged,
                                  &MainWindowControllerMock::tabPlacementChanged })
            connect(this, signal, this, &MainWindowControllerMock::refreshChip);
        connect(
            &_sidebar, &window::SidebarModel::summaryChanged, this, &MainWindowControllerMock::refreshChip);
    }

    /// Mirrors WindowController::hostLabel(): the attached host, set by a test before Main.qml loads.
    [[nodiscard]] QString const& hostLabel() const noexcept { return attachedHost; }

    /// Mirrors WindowController::hostChipState().
    [[nodiscard]] int hostChipState() const noexcept { return _hostChipState; }

    /// A daemon window's daemon, as the chip names it; empty falls back to the attached host. Says nothing of
    /// what serves the window: that is `daemonHosting`.
    QString daemonName {};
    /// What serves the window, as TerminalSessionManager::hostingOf() answers it: set by a test beside the
    /// name (setDaemon(), rebindHost() or setHosting()), never worked out from it.
    window::DaemonHosting daemonHosting = window::DaemonHosting::InProcess;
    /// Whether the daemon runs on this machine; a window attached to a host is remote whatever this says.
    window::DaemonLocality daemonLocality = window::DaemonLocality::Local;

    /// The one rule the mocks derive hosting by: a name or a host makes a daemon's window, neither an
    /// in-process one. A window that is bound to a host that is no daemon is set by setHosting().
    /// @param host   The host label.
    /// @param daemon The daemon name.
    /// @return Daemon where either is non-empty, else InProcess.
    [[nodiscard]] static window::DaemonHosting hostingNamed(QString const& host, QString const& daemon)
    {
        return host.isEmpty() && daemon.isEmpty() ? window::DaemonHosting::InProcess
                                                  : window::DaemonHosting::Daemon;
    }

    /// @param name   The daemon the chip names from now on; empty for an in-process window.
    /// @param locality Where it runs.
    void setDaemon(QString name, window::DaemonLocality locality = window::DaemonLocality::Local)
    {
        daemonHosting = hostingNamed({}, name);
        daemonName = std::move(name);
        daemonLocality = locality;
        refreshChip();
    }

    /// Serves the window as @p hosting says, whatever the name: a window attached to a host that is no daemon
    /// (a tmux attach) has a host label and no daemon.
    /// @param hosting What serves the window from now on.
    void setHosting(window::DaemonHosting hosting)
    {
        daemonHosting = hosting;
        refreshChip();
    }

    /// Mirrors WindowController::chip().
    [[nodiscard]] window::LocationChipModel* chip() noexcept { return &_chip; }

    /// The chrome row's side of the band decision, which the chip's place follows: a test that loads Main.qml
    /// under a row sets the row's (the loaders do, from the engine's chromeStyle).
    window::TitleBandRow bandRow {};

    /// Moves the chip to this mock's state, as WindowController::refreshChip() does. A derived mock calls it
    /// when one of the band's inputs it owns changes.
    void refreshChip() { _chip.setInputs(chipInputs()); }

    /// Mirrors WindowController::setBandWidths(): keeps the minimum the parts sum to and where the band sits.
    Q_INVOKABLE void setBandWidths(QVariantMap const& parts, int placement)
    {
        _bandMinimumWidth = window::bandMinimumWidthOfParts(parts);
        _bandPlacement = window::bandPlacementFromInt(placement).value_or(window::BandPlacement::FullWidth);
    }

    /// @param hintMinimum The window's minimum width from its page, as WindowController has one.
    /// @return The minimum width the window would get (window::minimumWidthWith()), no sidebar beside it.
    [[nodiscard]] int minimumWidthWith(int hintMinimum) const noexcept
    {
        return window::minimumWidthWith(hintMinimum, _bandMinimumWidth, 0, _bandPlacement);
    }

    /// @return What the last setBandWidths() summed to; 0 before any.
    [[nodiscard]] double bandMinimumWidth() const noexcept { return _bandMinimumWidth; }

    /// What the connection reports, as WindowController::notifyHostConnectionChanged() would tell it.
    /// @param state A window::HostChipState as its underlying value.
    void setHostChipState(int state)
    {
        if (_hostChipState == state)
            return;
        _hostChipState = state;
        emit hostChipStateChanged();
    }

    /// @return The window's prompt host, as WindowController has one; asks arrive through promptRouter().
    [[nodiscard]] QObject* promptHost() noexcept { return &_promptHost; }
    /// @return The router that sends prompts to this window's host.
    [[nodiscard]] std::shared_ptr<contour::askpass::AskpassPromptRouter> const& promptRouter() const noexcept
    {
        return _promptRouter;
    }

    /// Mirrors WindowController::applicationVersion() with a fixed test value.
    [[nodiscard]] QString applicationVersion() const { return QStringLiteral("0.0.0-test"); }

    /// How the chords shortcutText() names are spelled: the row's host spelling, as the palette's are.
    command::ShortcutStyle shortcutStyle = command::ShortcutStyle::Text;

    /// The chords a configuration binds, by command, where a test hands one (command::chordIndex()); none
    /// keeps the fixed answers below.
    std::optional<command::ChordIndex> chords;

    /// Mirrors WindowController::shortcutText(): with chords, the chord they bind @p commandName to, spelled
    /// in shortcutStyle, or nothing; without, a ClosePane chord (the desktop's Ctrl+Shift+X), CreateNewTab's,
    /// Ctrl+Shift+T, JumpToNextAttention's, Ctrl+Shift+J, and nothing for any other command (an ended pane's
    /// footer names Close's).
    [[nodiscard]] Q_INVOKABLE QString shortcutText(QString const& commandName) const
    {
        if (chords)
        {
            auto const chord = chords->find(commandName.toStdString());
            return chord != chords->end()
                       ? QString::fromStdString(command::shortcutText(chord->second, shortcutStyle))
                       : QString {};
        }
        if (commandName == QStringLiteral("CreateNewTab"))
            return QStringLiteral("Ctrl+Shift+T");
        // The sidebar footer's jump chord, as native2026 binds it on the desktop.
        if (commandName == QStringLiteral("JumpToNextAttention"))
            return QString::fromStdString(command::shortcutText(
                vtbackend::Modifiers { vtbackend::Modifier::Control, vtbackend::Modifier::Shift },
                command::ShortcutInput { U'J' },
                shortcutStyle));
        return commandName == QStringLiteral("ClosePane") ? QStringLiteral("Ctrl+Shift+X") : QString {};
    }

    /// Mirrors WindowController::showAbout(): asks the window for its About dialog.
    Q_INVOKABLE void showAbout() { emit aboutRequested(); }
    /// Mirrors WindowController::dropPayloadOf(): nothing here is a node drag.
    Q_INVOKABLE [[nodiscard]] int dropPayloadOf(QString const&) const { return 0; }
    /// Mirrors WindowController::dropZoneAt(): nothing is a drop target here.
    Q_INVOKABLE [[nodiscard]] int dropZoneAt(QObject*, int, qreal, qreal, qreal, qreal, QString const&) const
    {
        return 0;
    }
    /// Mirrors WindowController::dropNodeOnPane(): nothing is dropped on a pane here.
    Q_INVOKABLE void dropNodeOnPane(QObject*, int, QString const&) {}
    /// Mirrors WindowController::dropGhostLabel(): no zone has a ghost here.
    Q_INVOKABLE [[nodiscard]] QString dropGhostLabel(int, int, QObject*) const { return {}; }
    /// Mirrors WindowController::dropGhostRect(): no zone has a ghost here.
    Q_INVOKABLE [[nodiscard]] QRectF dropGhostRect(int, qreal, qreal, qreal) const { return {}; }
    /// Mirrors WindowController::refusalText(): nothing refuses a drag here.
    Q_INVOKABLE [[nodiscard]] QString refusalText(QString const&) const { return {}; }
    /// Mirrors WindowController::dragTitle(): a drag here names nothing.
    Q_INVOKABLE [[nodiscard]] QString dragTitle(QString const&) const { return {}; }
    /// Mirrors WindowController::beginPaneDrag(): a pane here cannot be dragged.
    Q_INVOKABLE [[nodiscard]] QString beginPaneDrag(QObject*) const { return {}; }
    /// Mirrors WindowController::beginTabDrag(): counted, and the tab's drag (tabDragPayload()).
    Q_INVOKABLE [[nodiscard]] QString beginTabDrag(int tabIndex)
    {
        ++beganDrags;
        return tabDragPayload(tabIndex);
    }
    /// Mirrors WindowController::finishNodeDrag(): counted.
    Q_INVOKABLE void finishNodeDrag(int) { ++finishedDrags; }
    /// How often a node drag began.
    int beganDrags = 0;
    /// How often a node drag ended.
    int finishedDrags = 0;

    /// Mirrors WindowController::tabDragPayload(): the node drag of the tab at @p tabIndex, a tab of two
    /// panes named after its row.
    [[nodiscard]] QString tabDragPayload(int tabIndex) const
    {
        return contour::window::writeNodeDrag(
            contour::window::NodeDrag { .payload = contour::window::DropPayload::Tab,
                                        .host = { .kind = contour::window::HostKind::Process, .id = "test" },
                                        .window = vtworkspace::WorkspaceId { 1 },
                                        .tabIndex = tabIndex,
                                        .sessions = { 100 + (static_cast<uint64_t>(tabIndex) * 2),
                                                      101 + (static_cast<uint64_t>(tabIndex) * 2) } });
    }
    /// Mirrors WindowController::dropNodeOnTab(): recorded.
    Q_INVOKABLE void dropNodeOnTab(int tabIndex, QString const& nodePayload)
    {
        tabDrops.emplace_back(tabIndex, nodePayload);
    }
    /// Mirrors WindowController::dropNodeInGap(): recorded.
    Q_INVOKABLE void dropNodeInGap(int index, QString const& nodePayload)
    {
        gapDrops.emplace_back(index, nodePayload);
    }
    /// Mirrors WindowController::springOpens(): any tab opens.
    Q_INVOKABLE [[nodiscard]] bool springOpens(int, QString const&) const { return true; }
    /// Mirrors WindowController::setTabSpans(): kept for tabBarHit().
    Q_INVOKABLE void setTabSpans(QList<QRectF> const& tabs)
    {
        // The call form: clang with libstdc++ 14 cannot pipe a view into ranges::to.
        tabSpans = std::ranges::to<std::vector<contour::window::TabSpan>>(
            tabs | std::views::transform([](QRectF const& tab) {
                return contour::window::TabSpan { .left = tab.left(), .right = tab.right() };
            }));
    }
    /// Mirrors WindowController::tabBarHit() through the same pure decision (resolveTabBarHit).
    Q_INVOKABLE [[nodiscard]] contour::window::TabBarHitValue tabBarHit(qreal x) const
    {
        return contour::window::TabBarHitValue { contour::window::resolveTabBarHit(tabSpans, x) };
    }
    std::vector<std::pair<int, QString>> tabDrops;  ///< Every dropNodeOnTab(), in order.
    std::vector<std::pair<int, QString>> gapDrops;  ///< Every dropNodeInGap(), in order.
    std::vector<contour::window::TabSpan> tabSpans; ///< What setTabSpans() last reported.

    /// Mirrors WindowController::windowTitle(), through the same remoteWindowTitle().
    Q_INVOKABLE [[nodiscard]] QString windowTitle(QString const& sessionTitle) const
    {
        return QString::fromStdString(
            contour::window::remoteWindowTitle(attachedHost.toStdString(), sessionTitle.toStdString()));
    }

    QString attachedHost; ///< The host hostLabel() reports; empty for a local window.

    /// What WindowController::notifyHostBindingChanged() tells: the window was bound to another link after it
    /// loaded, as the boot window is from Main.qml's onCompleted.
    /// @param host The host hostLabel() reports from now on.
    void rebindHost(QString host)
    {
        daemonHosting = hostingNamed(host, daemonName);
        attachedHost = std::move(host);
        emit hostLabelChanged();
    }

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

    /// @return The sidebar row menu last published by openSidebarContextMenu().
    [[nodiscard]] QVariantList const& sidebarContextMenuModel() const noexcept
    {
        return _sidebarContextMenuModel;
    }
    /// Mirrors WindowController::openSidebarContextMenu(): publishes a row's menu, then asks for it at @p at.
    void openSidebarContextMenu(QVariantList model, int row, QPointF at)
    {
        _sidebarContextMenuModel = std::move(model);
        emit sidebarContextMenuModelChanged();
        emit sidebarContextMenuRequested(row, at);
    }
    /// Mirrors WindowController::triggerSidebarContextMenuAction(), recording the pick.
    Q_INVOKABLE void triggerSidebarContextMenuAction(int actionId) { sidebarMenuPicks.push_back(actionId); }

    /// Mirrors WindowController::openNewTabMenu(): publishes the menu, then asks for it, counting the calls.
    Q_INVOKABLE void openNewTabMenu(QPointF /*windowPosition*/)
    {
        ++newTabMenuOpenings;
        _newTabMenuModel = mockNewTabMenuModel();
        emit newTabMenuModelChanged();
        emit newTabMenuRequested();
    }

    /// Mirrors WindowController::triggerNewTabMenuAction(), recording the pick.
    Q_INVOKABLE void triggerNewTabMenuAction(int actionId) { newTabMenuPicks.push_back(actionId); }

    // {{{ Panes
    /// Mirrors WindowController::visiblePaneCount().
    [[nodiscard]] int visiblePaneCount() const noexcept { return _visiblePaneCount; }

    /// Splits or zooms, as WindowController recounts the active tab's panes.
    /// @param count The panes shown.
    void setVisiblePaneCount(int count)
    {
        if (_visiblePaneCount == count)
            return;
        _visiblePaneCount = count;
        emit visiblePaneCountChanged();
    }

    /// Mirrors WindowController::headerReveal().
    [[nodiscard]] int headerReveal() const noexcept { return _headerReveal; }

    /// Holds or lets go of the reveal modifier, as WindowController's event filter decides.
    /// @param reveal A window::HeaderReveal as its underlying value.
    void setHeaderReveal(int reveal)
    {
        if (_headerReveal == reveal)
            return;
        _headerReveal = reveal;
        emit headerRevealChanged();
    }
    // }}}

    /// Mirrors WindowController::titleBandHeight(): the band's measured height, 0 for none.
    [[nodiscard]] qreal titleBandHeight() const noexcept { return _titleBandHeight; }

    /// Measures the band, as WindowController::refreshTitleBand() would.
    /// @param height The band's height in logical pixels; 0 leaves the band to its token.
    void setTitleBandHeight(qreal height)
    {
        if (_titleBandHeight == height)
            return;
        _titleBandHeight = height;
        emit titleBandChanged();
    }

    /// Mirrors WindowController::titleBandFill(): what this window's band paints, a config::BandFill.
    /// @return The fill a test set, or an invalid variant for none.
    [[nodiscard]] QVariant titleBandFill() const { return _titleBandFill; }

    /// Decides the band's fill, as WindowController::refreshTitleBand() would.
    /// @param fill What the band paints.
    void setTitleBandFill(config::BandFill fill)
    {
        _titleBandFill = static_cast<int>(fill);
        emit titleBandChanged();
    }

    /// Mirrors WindowController::titleBandRevealTop(): where a revealed full-screen band rests.
    [[nodiscard]] qreal titleBandRevealTop() const noexcept { return _titleBandRevealTop; }

    /// Measures the full-screen reveal top, as WindowController::refreshTitleBand() would.
    /// @param top The band's resting top in full screen, in window coordinates.
    void setTitleBandRevealTop(qreal top)
    {
        if (_titleBandRevealTop == top)
            return;
        _titleBandRevealTop = top;
        emit titleBandChanged();
    }

    // {{{ Notices
    /// This window's notices, as WindowController::notices() gives them.
    [[nodiscard]] window::NoticeModel& notices() noexcept { return _notices; }
    /// @return The notices, as the `notices` property hands them to QML.
    [[nodiscard]] window::NoticeModel* noticesForQml() noexcept { return &_notices; }

    /// Mirrors WindowController::toastHost(), with the active pane's cursor row a test sets.
    [[nodiscard]] Q_INVOKABLE QString toastHost(QString const& band,
                                                QString const& edge,
                                                int bandHeight,
                                                int toastHeight) const
    {
        return QString::fromUtf8(window::placeToastNamed(band.toStdString(),
                                                         edge.toStdString(),
                                                         bandHeight,
                                                         toastHeight,
                                                         cursorRow,
                                                         window::PaneFooter::Hidden));
    }

    core::platform::ManualClock noticeClock {}; ///< What the notices' toasts are timed by.
    int cursorRow = window::ToastCoveredRows;   ///< The active pane's cursor row toastHost() reads.
    // }}}

    // {{{ The caption buttons the OS drives
    /// Mirrors WindowController::setCaptionButtonRect(): records the rectangle and its owner; out-of-range
    /// kinds ignored.
    Q_INVOKABLE void setCaptionButtonRect(
        QObject* owner, int kind, qreal x, qreal y, qreal width, qreal height)
    {
        if (kind < 0 || std::cmp_greater_equal(kind, captionRects.size()))
            return;
        captionRects[static_cast<size_t>(kind)] =
            platform::LogicalRect { .x = x, .y = y, .width = width, .height = height };
        captionOwners[static_cast<size_t>(kind)] = owner;
    }

    /// Mirrors WindowController::clearCaptionButtonRect(): records the call, and forgets the rectangle when
    /// @p owner reported it last.
    Q_INVOKABLE void clearCaptionButtonRect(QObject* owner, int kind)
    {
        clearedCaptionKinds.push_back(kind);
        if (kind < 0 || std::cmp_greater_equal(kind, captionRects.size()))
            return;
        if (captionOwners[static_cast<size_t>(kind)] != owner)
            return;
        captionRects[static_cast<size_t>(kind)].reset();
        captionOwners[static_cast<size_t>(kind)] = nullptr;
    }

    /// Mirrors WindowController::nativeMaximizeHovered().
    [[nodiscard]] bool nativeMaximizeHovered() const noexcept
    {
        return _nativeMaximizeVisual != platform::CaptionButtonVisual::Rest;
    }

    /// Mirrors WindowController::nativeMaximizePressed().
    [[nodiscard]] bool nativeMaximizePressed() const noexcept
    {
        return _nativeMaximizeVisual == platform::CaptionButtonVisual::Pressed;
    }

    /// What the caption adapter reports, as WindowController's binding would hear it.
    /// @param visual How the maximise button looks now.
    void setNativeMaximizeVisual(platform::CaptionButtonVisual visual)
    {
        if (_nativeMaximizeVisual == visual)
            return;
        _nativeMaximizeVisual = visual;
        emit nativeMaximizeVisualChanged();
    }

    /// The rectangles WindowControls.qml last reported, by config::WindowControlKind.
    std::array<std::optional<platform::LogicalRect>, config::WindowControlCount> captionRects {};
    /// The control that reported each rectangle last, by config::WindowControlKind.
    std::array<QObject*, config::WindowControlCount> captionOwners {};
    std::vector<int> clearedCaptionKinds {}; ///< Every clearCaptionButtonRect() kind, in order.
    // }}}

    // {{{ The workspace sidebar
    /// @return The sidebar model, as QML sees it.
    [[nodiscard]] QObject* sidebarObject() noexcept { return &_sidebar; }
    /// @return The sidebar model.
    [[nodiscard]] window::SidebarModel& sidebarModel() noexcept { return _sidebar; }
    /// @return What the sidebar lists; a test sets its workspaces, then calls sidebarModel().refreshNow().
    [[nodiscard]] FakeDirectorySource& sidebarSource() noexcept { return _sidebarSource; }
    /// @return The fake window the sidebar asks things of.
    [[nodiscard]] FakeSidebarHost& sidebarHost() noexcept { return _sidebarHost; }
    /// When the sidebar's clock stands.
    static constexpr auto SidebarEpoch =
        std::chrono::system_clock::time_point { std::chrono::seconds { 1'000'000 } };
    /// @return The window's seen-state, which the sidebar's statuses read.
    [[nodiscard]] window::TabAttention& sidebarAttention() noexcept { return _sidebarAttention; }

    /// Mirrors WindowController::sidebarVisibility(): a config::SidebarVisibility's underlying value.
    [[nodiscard]] int sidebarVisibility() const noexcept { return static_cast<int>(_sidebarVisibility); }
    /// Mirrors WindowController::sidebarWidth(): the width it takes, 0 while hidden.
    [[nodiscard]] int sidebarWidth() const noexcept
    {
        return _sidebarVisibility == config::SidebarVisibility::Shown ? _sidebarUnits : 0;
    }
    /// Mirrors WindowController::tabPlacement(): a config::TabPlacement's underlying value.
    [[nodiscard]] int tabPlacement() const noexcept { return static_cast<int>(_tabPlacement); }
    /// Mirrors WindowController::otherWorkspacesUrgency().
    [[nodiscard]] int otherWorkspacesUrgency() const noexcept { return _sidebar.otherWorkspacesUrgency(); }
    /// Mirrors WindowController::sidebarCommands().
    [[nodiscard]] static QVariantMap sidebarCommands() { return window::sidebarCommandMap(); }

    /// Shows the sidebar @p width pixels wide, as a user's toggle would.
    void showSidebar(int width)
    {
        _sidebarUnits = width;
        _sidebarVisibility = config::SidebarVisibility::Shown;
        emit sidebarVisibilityChanged();
        emit sidebarWidthChanged();
    }
    /// Where the tabs live, as the configuration says.
    void setTabPlacement(config::TabPlacement placement)
    {
        _tabPlacement = placement;
        emit tabPlacementChanged();
    }
    /// Mirrors WindowController::setSidebarWidth(): records the width the drag ended at, and takes it.
    Q_INVOKABLE void setSidebarWidth(int width)
    {
        savedSidebarWidths.push_back(width);
        _sidebarUnits = width;
        emit sidebarWidthChanged();
    }
    /// Mirrors WindowController::toggleSidebar().
    Q_INVOKABLE void toggleSidebar()
    {
        ++sidebarToggles;
        _sidebarVisibility = _sidebarVisibility == config::SidebarVisibility::Shown
                                 ? config::SidebarVisibility::Hidden
                                 : config::SidebarVisibility::Shown;
        emit sidebarVisibilityChanged();
        emit sidebarWidthChanged();
    }
    /// Mirrors WindowController::confirmCloseWorkspace(): records the answer.
    Q_INVOKABLE void confirmCloseWorkspace(quint64 workspace)
    {
        confirmedWorkspaceCloses.push_back(workspace);
    }
    /// Mirrors WindowController::closeConfirmation(): the table's wording, with the counts the mock's scene
    /// knows nothing of left at zero.
    Q_INVOKABLE [[nodiscard]] QVariantMap closeConfirmation(
        int kind, quint64 /*workspace*/, quint64 /*tab*/, QString const& label, QString const& program) const
    {
        auto const text = contour::window::closeDialogText(
            static_cast<contour::window::CloseKind>(kind),
            { .label = label.toStdString(), .tabs = 0, .panes = 0, .program = program.toStdString() });
        return { { QStringLiteral("heading"), QString::fromStdString(text.heading) },
                 { QStringLiteral("message"), QString::fromStdString(text.message) },
                 { QStringLiteral("acceptText"), QString::fromStdString(text.acceptText) } };
    }
    /// Mirrors WindowController::confirmClose(): records the answer.
    Q_INVOKABLE void confirmClose(int kind, quint64 workspace, quint64 tab, quint64 pane)
    {
        confirmedCloses.push_back({ kind, workspace, tab, pane });
    }
    /// Mirrors WindowController::newWorkspace(): counts the request.
    Q_INVOKABLE void newWorkspace() { ++newWorkspaces; }
    /// Mirrors WindowController::showWorkspaceDropdown(): records where the chip asked it to drop down.
    Q_INVOKABLE void showWorkspaceDropdown(QRectF const& chip) { workspaceDropdownAnchor = chip; }
    /// Mirrors WindowController::presentSwitcher().
    void presentSwitcher(QObject* switcher, std::optional<QRectF> anchor)
    {
        _switcher = switcher;
        _switcherAnchor = anchor;
        emit switcherChanged();
        emit switcherRequested(anchor ? QVariant { *anchor } : QVariant {});
    }
    /// Mirrors WindowController::closeSwitcher(): a popup that closed while still presented was dismissed.
    Q_INVOKABLE void closeSwitcher()
    {
        auto* const dismissed = _switcher.data();
        if (dismissed == nullptr)
            return;
        _switcher = nullptr;
        _switcherAnchor.reset();
        emit switcherChanged();
        QMetaObject::invokeMethod(dismissed, "cancel");
    }
    [[nodiscard]] QObject* switcher() const { return _switcher.data(); }
    [[nodiscard]] bool isWorkspaceDropdownOpen() const { return _switcher && _switcherAnchor.has_value(); }
    /// Mirrors WindowController::openSidebarFilter().
    Q_INVOKABLE void openSidebarFilter() { emit sidebarFilterRequested(); }
    /// Mirrors WindowController::beginSidebarDrag(): the payload a workspace, tab or pane row carries, as the
    /// sidebar's own host would name it (a tab by its panes' keys, a pane by its own); empty out of range.
    Q_INVOKABLE [[nodiscard]] QString beginSidebarDrag(int row) const
    {
        auto const& rows = _sidebar.rows();
        if (row < 0 || static_cast<std::size_t>(row) >= rows.size())
            return {};
        auto const& key = rows[static_cast<std::size_t>(row)].key;
        auto drag = window::NodeDrag {};
        drag.host = _sidebarHost.identity;
        switch (key.kind)
        {
            case window::SidebarRowKind::Workspace:
                drag.payload = window::DropPayload::Workspace;
                drag.workspace = key.workspace;
                return window::writeNodeDrag(drag);
            case window::SidebarRowKind::Tab:
                for (auto const& workspace: _sidebarSource.listed.workspaces)
                    for (auto const& tab: workspace.tabs)
                        if (workspace.id == key.workspace && tab.id == key.tab)
                            for (auto const& pane: tab.panes)
                                drag.sessions.push_back(pane.key.value);
                if (drag.sessions.empty())
                    return {};
                drag.payload =
                    drag.sessions.size() > 1 ? window::DropPayload::Tab : window::DropPayload::Pane;
                drag.tabIndex = static_cast<int>(rows[static_cast<std::size_t>(row)].position);
                return window::writeNodeDrag(drag);
            case window::SidebarRowKind::Pane:
                drag.payload = window::DropPayload::Pane;
                drag.sessions = { key.pane.value };
                return window::writeNodeDrag(drag);
        }
        return {};
    }

    /// Mirrors WindowController::toggleMaximized(): counts the request, as a title-bar double-click makes it.
    Q_INVOKABLE void toggleMaximized() { ++maximizeToggles; }

    std::vector<int> savedSidebarWidths {};           ///< Every setSidebarWidth() width, in order.
    int sidebarToggles = 0;                           ///< How often toggleSidebar() ran.
    int maximizeToggles = 0;                          ///< How often toggleMaximized() ran.
    int newWorkspaces = 0;                            ///< How often newWorkspace() ran.
    std::optional<QRectF> workspaceDropdownAnchor;    ///< What showWorkspaceDropdown() was last asked.
    std::vector<quint64> confirmedWorkspaceCloses {}; ///< Every confirmCloseWorkspace() workspace, in order.
    /// One confirmClose() answer: the kind, the workspace, the tab and the pane.
    struct ConfirmedClose
    {
        int kind = 0;
        quint64 workspace = 0;
        quint64 tab = 0;
        quint64 pane = 0;
    };
    std::vector<ConfirmedClose> confirmedCloses {}; ///< Every confirmClose() answer, in order.
    // }}}

    std::vector<int> tabMenuPicks {};     ///< Every tab menu pick, in order.
    std::vector<int> sidebarMenuPicks {}; ///< Every sidebar row menu pick, in order.
    std::vector<int> newTabMenuPicks {};  ///< Every new-tab menu pick, in order.
    int newTabMenuOpenings = 0;           ///< How often the new-tab menu was asked for.

  signals:
    void switcherChanged();
    void switcherRequested(QVariant anchor);
    void switcherWithdrawn();
    void hostChipStateChanged();
    void hostLabelChanged();
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
    void sidebarContextMenuModelChanged();
    void sidebarContextMenuRequested(int row, QPointF windowPosition);
    void newTabMenuModelChanged();
    void newTabMenuRequested();
    void aboutRequested();
    void visiblePaneCountChanged();
    void headerRevealChanged();
    void titleBandChanged();
    void nativeMaximizeVisualChanged();
    void sidebarVisibilityChanged();
    void sidebarWidthChanged();
    void tabPlacementChanged();
    void otherWorkspacesUrgencyChanged();
    void sidebarCommandRequested(int command, QString const& text);
    /// Mirrors WindowController::sidebarFilterRequested().
    void sidebarFilterRequested();
    void workspaceRenameRequested();
    void sidebarCloseConfirmationRequested(
        int kind, quint64 workspace, quint64 tab, quint64 pane, QString const& label, QString const& program);
    void closeWorkspaceConfirmationRequested(quint64 workspace,
                                             QString const& workspaceLabel,
                                             QString const& program);

  protected:
    /// @return What this mock's window says of the band decision: the frame, the tab-bar gate and the edge
    ///         default to a shown strip on a client band, which a derived mock overrides with its own state.
    [[nodiscard]] virtual window::TitleBandInputs titleBandInputs() const
    {
        return window::TitleBandInputs {
            .notice = window::hostNoticeOf(window::hostChipPresence(attachedHost.toStdString()), chipState()),
            .placement = _tabPlacement,
            .sidebar = _sidebarVisibility,
        };
    }

  private:
    /// @return The chip's inputs as this mock's window stands: what WindowController::chipInputs() reads from
    ///         the manager, the sidebar and the title band, read from the mock's own fields.
    [[nodiscard]] window::LocationChipInputs chipInputs() const
    {
        auto const band = window::resolveTitleBand(bandRow, titleBandInputs());
        auto const& summary = _sidebar.summary();
        return window::LocationChipInputs {
            .daemon = (daemonName.isEmpty() ? attachedHost : daemonName).toStdString(),
            .hosting = daemonHosting,
            .locality = !attachedHost.isEmpty() ? window::DaemonLocality::Remote : daemonLocality,
            .state = chipState(),
            .workspace = summary.currentWorkspace,
            .otherWorkspaces = summary.otherWorkspaces,
            .disclosure = isWorkspaceDropdownOpen() ? window::Disclosure::Expanded
                                                    : window::Disclosure::Collapsed,
            .band = band.band,
            .centre = band.centre,
            .sidebar = sidebarWidth() > 0 ? window::SidebarShown::Yes : window::SidebarShown::No,
        };
    }

    QPointer<QObject> _switcher;           ///< The switcher presented; null for none.
    std::optional<QRectF> _switcherAnchor; ///< What it hangs from; none while centred.
    std::shared_ptr<contour::askpass::AskpassPromptRouter> _promptRouter =
        std::make_shared<contour::askpass::AskpassPromptRouter>();
    contour::window::PromptHost _promptHost { _promptRouter, 1 };
    QVariantList _tabContextMenuModel {};
    QVariantList _sidebarContextMenuModel {};
    QVariantList _newTabMenuModel {};
    double _bandMinimumWidth = 0;                                            ///< @see setBandWidths.
    window::BandPlacement _bandPlacement = window::BandPlacement::FullWidth; ///< @see setBandWidths.
    int _hostChipState = 0; ///< @see hostChipState; 0 = HostChipState::Connected.

    /// @return The connection's state as the chip's pure decisions take it.
    [[nodiscard]] window::HostChipState chipState() const
    {
        return static_cast<window::HostChipState>(_hostChipState);
    }

    int _visiblePaneCount = 1;     ///< @see visiblePaneCount; one pane, unsplit.
    int _headerReveal = 0;         ///< @see headerReveal; 0 = HeaderReveal::None.
    qreal _titleBandHeight = 0;    ///< @see titleBandHeight; 0 = not measured.
    QVariant _titleBandFill {};    ///< @see titleBandFill; invalid = the row's own.
    qreal _titleBandRevealTop = 0; ///< @see titleBandRevealTop; 0 = the window's top.
    /// @see setNativeMaximizeVisual.
    platform::CaptionButtonVisual _nativeMaximizeVisual = platform::CaptionButtonVisual::Rest;
    platform::NullAnnouncer _noticeAnnouncer;
    window::NoticeModel _notices { noticeClock, _noticeAnnouncer, [](window::NoticeAction) {} };
    // The sidebar's window, in the order the model needs them built.
    FakeDirectorySource _sidebarSource;
    FakeSidebarHost _sidebarHost { &_sidebarSource };
    window::TabAttention _sidebarAttention;
    core::platform::ManualWallClock _sidebarClock { SidebarEpoch };
    platform::NullAnnouncer _sidebarAnnouncer;
    window::SidebarModel _sidebar { _sidebarHost, _sidebarAttention, _sidebarClock, _sidebarAnnouncer };
    config::SidebarVisibility _sidebarVisibility = config::SidebarVisibility::Hidden;
    int _sidebarUnits = 0;
    config::TabPlacement _tabPlacement = config::TabPlacement::Top;
    /// Declared last: it is built from everything above. A derived mock refreshes it once its own state is
    /// set.
    window::LocationChipModel _chip { chipInputs() };
};

} // namespace contour::test
