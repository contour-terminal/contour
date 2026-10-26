// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/config/NewDefaultsOffer.hpp>

#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QVariant>
#include <QtGui/QColor>
#include <QtQuick/QQuickItem>

#include <optional>
#include <vector>

namespace contour::test
{

/// A dynamic stand-in for a TerminalSession, exposing the full property + signal surface the QML
/// (TerminalPane
/// + SessionChrome) actually reads and connects to. Unlike a static stub, its properties are settable and
/// carry NOTIFY signals, and it declares every signal the QML `.connect()`s or Connections-targets — so a
/// test can DRIVE the state transitions (resize, opacity change) that real split lifecycle churn produces.
/// This is what makes the connect-leak / null-deref class of split bug reproducible in a headless test.
class MockSession: public QObject
{
    Q_OBJECT
    Q_PROPERTY(int pageLineCount READ pageLineCount NOTIFY lineCountChanged)
    Q_PROPERTY(int pageColumnsCount READ pageColumnsCount NOTIFY columnsCountChanged)
    Q_PROPERTY(int historyLineCount READ historyLineCount NOTIFY historyLineCountChanged)
    Q_PROPERTY(bool showResizeIndicator READ showResizeIndicator CONSTANT)
    Q_PROPERTY(int upTime READ upTime CONSTANT)
    Q_PROPERTY(float opacity READ opacity NOTIFY opacityChanged)
    Q_PROPERTY(float dimUnfocused READ dimUnfocused NOTIFY dimUnfocusedChanged)
    Q_PROPERTY(QColor backgroundColor READ backgroundColor CONSTANT)
    Q_PROPERTY(QString bellSource READ bellSource CONSTANT)
    // Mirrors TerminalSession's profile-derived scrollbar properties: both are NOTIFY-driven (a config
    // reload flips them live), so the mock must be able to drive that transition too.
    Q_PROPERTY(bool isScrollbarRight READ isScrollbarRight NOTIFY isScrollbarRightChanged)
    Q_PROPERTY(bool isScrollbarVisible READ isScrollbarVisible NOTIFY isScrollbarVisibleChanged)
    Q_PROPERTY(bool isImageBackground READ isImageBackground CONSTANT)
    Q_PROPERTY(bool isBlurBackground READ isBlurBackground CONSTANT)
    Q_PROPERTY(float opacityBackground READ opacityBackground CONSTANT)
    Q_PROPERTY(QString pathToBackground READ pathToBackground CONSTANT)
    // Mirrors TerminalSession's hyperlink tooltip pair. Both change together, so one signal serves them.
    Q_PROPERTY(QString hyperlinkTooltipText READ hyperlinkTooltipText NOTIFY hyperlinkHoverChanged)
    Q_PROPERTY(QRectF hyperlinkTooltipAnchor READ hyperlinkTooltipAnchor NOTIFY hyperlinkHoverChanged)
    // Mirrors TerminalSession's block-column tooltip: the anchor, and the invocable its text is read
    // through whenever the anchor moves.
    Q_PROPERTY(
        QRectF commandBlockTooltipAnchor READ commandBlockTooltipAnchor NOTIFY commandBlockHoverChanged)
    // Mirrors TerminalSession's scrollbar marks: ticks as { position, kind, targetLine } and one colour per
    // kind. Both change together, so one signal serves them.
    Q_PROPERTY(QVariantList scrollbarMarks READ scrollbarMarks NOTIFY scrollbarMarksChanged)
    Q_PROPERTY(QVariantList scrollbarMarkColors READ scrollbarMarkColors NOTIFY scrollbarMarksChanged)
    // Mirrors TerminalSession's reconnecting state, which TerminalPane's overlay shows.
    Q_PROPERTY(bool isReconnecting READ isReconnecting NOTIFY reconnectingChanged)
    // {{{ Panes: what the pane header reads, mirroring TerminalSession's properties and their signals.
    Q_PROPERTY(
        QString displayWorkingDirectory READ displayWorkingDirectory NOTIFY displayWorkingDirectoryChanged)
    Q_PROPERTY(QString foregroundProcessName READ foregroundProcessName NOTIFY foregroundProcessNameChanged)
    Q_PROPERTY(QString title READ title NOTIFY titleChanged)
    Q_PROPERTY(int programStatus READ programStatus NOTIFY paneProgramStatusChanged)
    Q_PROPERTY(QString programStatusText READ programStatusText NOTIFY paneProgramStatusChanged)
    // }}}

  public:
    /// @param background The session background colour; configuration, hence fixed for the lifetime.
    explicit MockSession(QColor background = QColor(Qt::black)): _background(background) {}

    [[nodiscard]] int pageLineCount() const { return _lines; }
    [[nodiscard]] bool isReconnecting() const { return _reconnecting; }

    /// Loses or regains the host connection, as TerminalSession::setHostConnection does.
    void setReconnecting(bool reconnecting)
    {
        _reconnecting = reconnecting;
        emit reconnectingChanged();
    }
    [[nodiscard]] int pageColumnsCount() const { return _columns; }
    [[nodiscard]] int historyLineCount() const { return _historyLineCount; }
    [[nodiscard]] bool showResizeIndicator() const { return true; }
    [[nodiscard]] int upTime() const { return 5; } // > 1.0 so the resize popup path is exercised
    [[nodiscard]] float opacity() const { return _opacity; }
    [[nodiscard]] float dimUnfocused() const { return _dimUnfocused; }
    [[nodiscard]] bool isImageBackground() const { return false; }
    [[nodiscard]] bool isBlurBackground() const { return false; }
    [[nodiscard]] float opacityBackground() const { return 1.0F; }
    [[nodiscard]] QString pathToBackground() const { return {}; }
    [[nodiscard]] QColor backgroundColor() const { return _background; }
    [[nodiscard]] QString bellSource() const { return {}; }
    [[nodiscard]] bool isScrollbarRight() const { return _scrollbarRight; }
    [[nodiscard]] bool isScrollbarVisible() const { return _scrollbarVisible; }
    [[nodiscard]] QString hyperlinkTooltipText() const { return _hyperlinkTooltipText; }
    [[nodiscard]] QRectF hyperlinkTooltipAnchor() const { return _hyperlinkTooltipAnchor; }
    [[nodiscard]] QRectF commandBlockTooltipAnchor() const { return _commandBlockTooltipAnchor; }
    Q_INVOKABLE [[nodiscard]] QString commandBlockTooltip() const { return _commandBlockTooltipText; }

    /// Publishes a hovered hyperlink, the way TerminalSession does on a real cell transition.
    void setHyperlinkHover(QString text, QRectF anchor)
    {
        _hyperlinkTooltipText = std::move(text);
        _hyperlinkTooltipAnchor = anchor;
        emit hyperlinkHoverChanged();
    }

    /// Publishes a hovered command block, the way TerminalSession does when the pointer reaches one.
    void setCommandBlockHover(QString text, QRectF anchor)
    {
        _commandBlockTooltipText = std::move(text);
        _commandBlockTooltipAnchor = anchor;
        emit commandBlockHoverChanged();
    }

    [[nodiscard]] QVariantList scrollbarMarks() const { return _scrollbarMarks; }
    [[nodiscard]] QVariantList scrollbarMarkColors() const { return _scrollbarMarkColors; }

    /// Publishes ticks, as TerminalSession::refreshScrollbarMarks() does.
    void setScrollbarMarks(QVariantList marks, QVariantList colors)
    {
        _scrollbarMarks = std::move(marks);
        _scrollbarMarkColors = std::move(colors);
        emit scrollbarMarksChanged();
    }

    /// Records the track height the chrome reports, so a test can assert the session was told.
    Q_INVOKABLE void setScrollbarMarkTrackPixels(int pixels) { trackPixels = pixels; }

    /// Records each jump the chrome asks for, so a click on a tick is observable.
    Q_INVOKABLE void scrollToScrollbarMark(qint64 targetLine) { jumpedTo.push_back(targetLine); }

    int trackPixels = 0;          ///< The track height the chrome last reported.
    std::vector<qint64> jumpedTo; ///< Every tick target the chrome asked to jump to, in order.

    // Drivers: mutate state and fire the NOTIFY the QML binds to, reproducing a live resize / opacity change.
    void setPageSize(int columns, int lines)
    {
        _columns = columns;
        _lines = lines;
        emit columnsCountChanged();
        emit lineCountChanged();
    }
    /// Give the session scrollback, so SessionChrome's `hasScrollback` (thumb size < 1.0) turns true and
    /// the scrollbar is actually shown.
    void setHistoryLineCount(int lines)
    {
        _historyLineCount = lines;
        emit historyLineCountChanged();
    }
    /// Move the bar to the other edge, as a profile reload / switch does (TerminalSession::activateProfile
    /// emits isScrollbarRightChanged for exactly this).
    void setScrollbarRight(bool right)
    {
        _scrollbarRight = right;
        emit isScrollbarRightChanged();
    }
    /// Show or hide the bar, as scrollbar.position: Hidden — or hide_in_alt_screen on the alt screen — does.
    void setScrollbarVisible(bool visible)
    {
        _scrollbarVisible = visible;
        emit isScrollbarVisibleChanged();
    }
    void setOpacity(float o)
    {
        _opacity = o;
        emit opacityChanged();
    }
    void setDimUnfocused(float dim)
    {
        _dimUnfocused = dim;
        emit dimUnfocusedChanged();
    }

    // {{{ Panes
    [[nodiscard]] QString const& displayWorkingDirectory() const noexcept { return _displayWorkingDirectory; }
    [[nodiscard]] QString const& foregroundProcessName() const noexcept { return _foregroundProcessName; }
    [[nodiscard]] QString const& title() const noexcept { return _title; }
    [[nodiscard]] int programStatus() const noexcept { return _programStatus; }
    [[nodiscard]] QString const& programStatusText() const noexcept { return _programStatusText; }

    /// The shell reported another directory, as TerminalSession::refreshDisplayWorkingDirectory() tells.
    void setDisplayWorkingDirectory(QString directory)
    {
        _displayWorkingDirectory = std::move(directory);
        emit displayWorkingDirectoryChanged();
    }
    /// Another program came to the front, as TerminalSession::pollForegroundProcess() tells.
    void setForegroundProcessName(QString name)
    {
        _foregroundProcessName = std::move(name);
        emit foregroundProcessNameChanged();
    }
    /// The application set another title.
    void setTitle(QString title)
    {
        _title = std::move(title);
        emit titleChanged();
    }
    /// The pane's program status changed, as TerminalSession::setPaneProgramStatus() tells.
    /// @param kind A window::TabBadgeKind's value.
    /// @param text What it says.
    void setProgramStatus(int kind, QString text)
    {
        _programStatus = kind;
        _programStatusText = std::move(text);
        emit paneProgramStatusChanged();
    }
    // }}}

    // Permission dialog stubs (SessionChrome connects these signals to dialog .open slots).
    Q_INVOKABLE void applyPendingFontChange(bool, bool) {}
    Q_INVOKABLE void applyPendingPaste(bool, bool) {}
    Q_INVOKABLE void executePendingBufferCapture(bool, bool) {}
    Q_INVOKABLE void executeShowHostWritableStatusLine(bool, bool) {}

    // The extension prompt: what it shows, and each answer, counted.
    [[nodiscard]] Q_INVOKABLE QString pendingExtensionPermissionText() const { return extensionQuestion; }
    Q_INVOKABLE void allowPendingExtension() { ++extensionAllowed; }
    Q_INVOKABLE void denyPendingExtension() { ++extensionDenied; }
    Q_INVOKABLE void dismissPendingExtension() { ++extensionDismissed; }
    QString extensionQuestion;  ///< What pendingExtensionPermissionText() answers.
    int extensionAllowed = 0;   ///< How often the chrome allowed.
    int extensionDenied = 0;    ///< How often it denied.
    int extensionDismissed = 0; ///< How often it closed the prompt unanswered.

  signals:
    void lineCountChanged();
    void columnsCountChanged();
    void historyLineCountChanged();
    void isScrollbarRightChanged();
    void isScrollbarVisibleChanged();
    void hyperlinkHoverChanged();
    void commandBlockHoverChanged();
    void scrollbarMarksChanged();
    void opacityChanged();
    void dimUnfocusedChanged();
    void reconnectingChanged();
    void displayWorkingDirectoryChanged();
    void foregroundProcessNameChanged();
    void titleChanged();
    void paneProgramStatusChanged();
    void onBell();
    void onAlert();
    void onShowNotification(QString const& /*title*/, QString const& /*body*/);
    void onScrollOffsetChanged(int /*value*/);
    void requestPermissionForFontChange();
    void requestPermissionForBufferCapture();
    void requestPermissionForShowHostWritableStatusLine();
    void requestPermissionForPasteLargeFile();
    void requestPermissionForExtension();

  private:
    int _columns = 80;
    int _lines = 25;
    int _historyLineCount = 0;
    bool _scrollbarRight = true;
    bool _scrollbarVisible = false;
    QString _hyperlinkTooltipText;
    QRectF _hyperlinkTooltipAnchor;
    QString _commandBlockTooltipText;
    QRectF _commandBlockTooltipAnchor;
    QVariantList _scrollbarMarks;
    QVariantList _scrollbarMarkColors;
    QColor _background;
    float _opacity = 1.0F;
    float _dimUnfocused = 0.0F;
    bool _reconnecting = false;
    QString _displayWorkingDirectory;
    QString _foregroundProcessName;
    QString _title;
    int _programStatus = 0; ///< window::TabBadgeKind::None.
    QString _programStatusText;
};

/// A DYNAMIC stand-in for a PaneProxy (one node of the split tree). Unlike the old static mock, session /
/// active / isLeaf / first / second are settable and carry NOTIFY signals, so a test can drive the exact
/// transitions a real split/collapse produces: a leaf becoming a split node, a session rebinding
/// null->non-null->null, a pane's active flag flipping. This is what exercises PaneNode's Loader
/// re-activation and TerminalPane's onSessionChanged/paneActive handlers — where the split bugs actually
/// live.
class MockPaneProxy: public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool isLeaf READ isLeaf NOTIFY changed)
    Q_PROPERTY(int orientation READ orientation NOTIFY changed)
    Q_PROPERTY(double ratio READ ratio NOTIFY changed)
    Q_PROPERTY(MockPaneProxy* first READ first NOTIFY changed)
    Q_PROPERTY(MockPaneProxy* second READ second NOTIFY changed)
    Q_PROPERTY(QObject* session READ session NOTIFY changed)
    Q_PROPERTY(bool active READ active NOTIFY changed)
    // A window::PaneFocus's value, as PaneProxy's focus gives it.
    Q_PROPERTY(int focus READ focus NOTIFY changed)

  public:
    explicit MockPaneProxy(bool leaf, MockPaneProxy* a = nullptr, MockPaneProxy* b = nullptr):
        _leaf { leaf }, _first { a }, _second { b }
    {
        if (a)
            a->setParent(this);
        if (b)
            b->setParent(this);
    }

    [[nodiscard]] bool isLeaf() const { return _leaf; }
    [[nodiscard]] int orientation() const { return _orientation; }
    [[nodiscard]] double ratio() const { return _ratio; }
    [[nodiscard]] MockPaneProxy* first() const { return _first; }
    [[nodiscard]] MockPaneProxy* second() const { return _second; }
    [[nodiscard]] QObject* session() const { return _session; }
    [[nodiscard]] bool active() const { return _active; }
    [[nodiscard]] int focus() const { return _focus; }

    // setRatio (from the divider drag) records what it was handed, so a drag is observable; the mock's
    // own ratio stays put, as the model only re-publishes it on its next rebuild.
    // activate() (from a pane tap) is call-tracked so click->activate routing is observable.
    Q_INVOKABLE void setRatio(double ratio) { lastRatio = ratio; }
    std::optional<double> lastRatio {}; ///< What the last divider drag wrote back; none before one.
    Q_INVOKABLE void activate() { ++activateCalls; }
    int activateCalls = 0;
    /// Records each header button's action (PaneProxy::runHeaderAction), in order.
    Q_INVOKABLE void runHeaderAction(int action) { headerActions.push_back(action); }
    std::vector<int> headerActions; ///< Every window::PaneHeaderAction value a header button ran, in order.

    // Drivers used by the behavioral split tests to reproduce lifecycle transitions.
    void setSession(QObject* s)
    {
        if (_session == s)
            return;
        _session = s;
        emit changed();
    }
    /// How the pane is focused now. @param focus A window::PaneFocus's value.
    void setFocus(int focus)
    {
        if (_focus == focus)
            return;
        _focus = focus;
        emit changed();
    }
    void setActive(bool a)
    {
        if (_active == a)
            return;
        _active = a;
        emit changed();
    }
    /// Turn this leaf into a split node with two leaf children (as vtworkspace Pane::split does), then
    /// notify. The split axis defaults to Vertical (2, matching vtworkspace::SplitState); use
    /// setOrientation() to change it.
    void becomeSplit(MockPaneProxy* a, MockPaneProxy* b)
    {
        _leaf = false;
        _first = a;
        _second = b;
        a->setParent(this);
        b->setParent(this);
        emit changed();
    }
    void setOrientation(int o)
    {
        _orientation = o;
        emit changed();
    }
    /// Collapse this split node back to a leaf (as Tab::closePane absorbs the sibling), adopting @p
    /// survivor's session, then notify.
    void collapseToLeaf(QObject* survivorSession)
    {
        _leaf = true;
        _first = nullptr;
        _second = nullptr;
        _session = survivorSession;
        emit changed();
    }

  signals:
    void changed();

  private:
    bool _leaf;
    MockPaneProxy* _first;
    MockPaneProxy* _second;
    QObject* _session = nullptr;
    bool _active = false;
    int _focus = 0;       // window::PaneFocus::Unfocused
    int _orientation = 2; // vtworkspace::SplitState::Vertical for a split node; ignored for a leaf
    double _ratio = 0.5;
};

/// A trivial stub so PaneNode.qml's leaf branch (TerminalPane → ContourTerminal) can instantiate in
/// the test engine, which does not link the real display type. Behaves as a plain Item.
///
/// `session` is exposed with a NOTIFY signal (mirroring the real ContourTerminal::sessionChanged) and
/// a `showNotification` signal so TerminalPane.qml's onSessionChanged handler and its
/// `vt.onShowNotification.connect(pane.showNotification)` wiring resolve against the stub. The stub
/// session stays null in these tests, and TerminalPane's onSessionChanged early-returns on a null
/// session, so none of the per-session signal connections are dereferenced.
class StubContourTerminal: public QQuickItem
{
    Q_OBJECT
    Q_PROPERTY(QObject* session READ session WRITE setSession NOTIFY sessionChanged)
    // Terminal.qml / TerminalPane read `fontSize` and connect a window-level `opacityChanged`; expose both
    // so the pane instantiates against the stub.
    Q_PROPERTY(double fontSize READ fontSize CONSTANT)
  public:
    [[nodiscard]] QObject* session() const { return _session; }
    void setSession(QObject* s)
    {
        if (_session != s)
        {
            _session = s;
            emit sessionChanged(s);
        }
    }
    [[nodiscard]] double fontSize() const { return 12.0; }
  signals:
    void sessionChanged(QObject* /*session*/);
    void showNotification(QString const& /*title*/, QString const& /*body*/);
    void terminated(); //!< TerminalPane's onTerminated handler binds to this.

  private:
    QObject* _session = nullptr;
};

/// A stand-in for window::NewDefaultsNotice, the `newDefaults` context property Main.qml's banner reads.
/// It records the answers instead of acting on them.
class MockNewDefaultsNotice: public QObject
{
    Q_OBJECT
    /// A plain `bool`, as the real notice's: a QML-facing Q_PROPERTY read as a predicate.
    Q_PROPERTY(bool offered READ offered WRITE setOffered NOTIFY offeredChanged)

  public:
    /// @param offer The offer the banner shows; NewDefaultsOffer::None shows none.
    explicit MockNewDefaultsNotice(config::NewDefaultsOffer offer):
        _isOffered { offer != config::NewDefaultsOffer::None }
    {
    }

    /// @return Whether the banner is shown.
    [[nodiscard]] bool offered() const noexcept { return _isOffered; }

    /// The "parameter is the property" setter carve-out: it only assigns the predicate QML reads.
    /// @param offered Whether the banner is shown.
    void setOffered(bool offered)
    {
        if (_isOffered == offered)
            return;
        _isOffered = offered;
        emit offeredChanged();
    }

    /// Records the banner's action.
    Q_INVOKABLE void useNew() { calls << QStringLiteral("useNew"); }
    /// Records the banner's dismissal.
    Q_INVOKABLE void keepMine() { calls << QStringLiteral("keepMine"); }

    QStringList calls; ///< The answers given, in order.

  signals:
    void offeredChanged();

  private:
    bool _isOffered = false;
};

} // namespace contour::test
