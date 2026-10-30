// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/config/NewDefaultsOffer.hpp>
#include <contour/session/EndReason.hpp>
#include <contour/session/Permission.hpp>
#include <contour/session/SearchStatus.hpp>
#include <contour/window/FindBarPlacement.hpp>
#include <contour/window/PaneState.hpp>
#include <contour/window/ShellIntegrationOffer.hpp>

#include <QtCore/QList>
#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QUrl>
#include <QtCore/QVariant>
#include <QtCore/QVariantList>
#include <QtCore/QVariantMap>
#include <QtGui/QColor>
#include <QtGui/QMouseEvent>
#include <QtQuick/QQuickItem>

#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>
#include <utility>
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
    // Mirrors TerminalSession's size indicator: its text, the request that shows it, and the settle QML
    // calls.
    Q_PROPERTY(QString sizeIndicatorText READ sizeIndicatorText NOTIFY sizeIndicatorRequested)
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
    // Mirrors TerminalSession's link target, for the link pill: its text, its hint and whether it shows
    // change together; the clearance's revision moves on its own.
    Q_PROPERTY(QString linkTargetText READ linkTargetText NOTIFY linkTargetChanged)
    Q_PROPERTY(QString linkTargetHint READ linkTargetHint NOTIFY linkTargetChanged)
    Q_PROPERTY(bool linkTargetShown READ linkTargetShown NOTIFY linkTargetChanged)
    Q_PROPERTY(int overlayClearanceRevision READ overlayClearanceRevision NOTIFY overlayClearanceChanged)
    // Mirrors TerminalSession's command block surface: where the hovered block's ⋯ button goes, which
    // block that is, the block the keyboard focuses (0 for none), and the sticky header the chrome shows.
    Q_PROPERTY(QRectF blockActionsAnchor READ blockActionsAnchor NOTIFY blockActionsAnchorChanged)
    Q_PROPERTY(qulonglong hoveredCommandBlock READ hoveredCommandBlock NOTIFY blockActionsAnchorChanged)
    Q_PROPERTY(qulonglong focusedCommandBlock READ focusedCommandBlock NOTIFY focusedCommandBlockChanged)
    Q_PROPERTY(QVariantMap stickyHeader READ stickyHeader NOTIFY stickyHeaderChanged)
    // Mirrors TerminalSession's scrollbar marks: ticks as { position, kind, style, targetLine } and one
    // colour per kind. Both change together, so one signal serves them.
    Q_PROPERTY(QVariantList scrollbarMarks READ scrollbarMarks NOTIFY scrollbarMarksChanged)
    Q_PROPERTY(QVariantList scrollbarMarkColors READ scrollbarMarkColors NOTIFY scrollbarMarksChanged)
    // Mirrors TerminalSession's pane state (window::paneStateOf over the end and the connection), which the
    // card's reconnecting pill and the ended pane's footer read.
    Q_PROPERTY(int paneState READ paneState NOTIFY paneStateChanged)
    // {{{ Panes: what the pane header reads, mirroring TerminalSession's properties and their signals.
    Q_PROPERTY(
        QString displayWorkingDirectory READ displayWorkingDirectory NOTIFY displayWorkingDirectoryChanged)
    Q_PROPERTY(QString foregroundProcessName READ foregroundProcessName NOTIFY foregroundProcessNameChanged)
    Q_PROPERTY(QString title READ title NOTIFY titleChanged)
    Q_PROPERTY(int programStatus READ programStatus NOTIFY paneProgramStatusChanged)
    // Mirrors TerminalSession's lock: a window::SecureIndicator's value.
    Q_PROPERTY(int secureIndicator READ secureIndicator NOTIFY secureIndicatorChanged)
    Q_PROPERTY(QString programStatusText READ programStatusText NOTIFY paneProgramStatusChanged)
    // }}}
    // {{{ The ended pane: what its footer reads, mirroring TerminalSession's properties and their signal.
    Q_PROPERTY(int sessionEnd READ sessionEnd NOTIFY sessionEndChanged)
    Q_PROPERTY(QString endMessage READ endMessage NOTIFY sessionEndChanged)
    Q_PROPERTY(int paneFooter READ paneFooter NOTIFY paneFooterChanged)
    Q_PROPERTY(QString footerMessage READ footerMessage NOTIFY paneFooterChanged)
    Q_PROPERTY(QString footerActionLabel READ footerActionLabel NOTIFY paneFooterChanged)
    Q_PROPERTY(QString footerDismissLabel READ footerDismissLabel NOTIFY paneFooterChanged)
    Q_PROPERTY(int footerTakesReturn READ footerTakesReturn NOTIFY paneFooterChanged)
    // }}}
    // {{{ The shell-integration card: what its stage says, mirroring TerminalSession's properties.
    Q_PROPERTY(int shellIntegrationCard READ shellIntegrationCard NOTIFY shellIntegrationCardChanged)
    Q_PROPERTY(QString shellIntegrationTitle READ shellIntegrationTitle NOTIFY shellIntegrationCardChanged)
    Q_PROPERTY(QString shellIntegrationBody READ shellIntegrationBody NOTIFY shellIntegrationCardChanged)
    Q_PROPERTY(
        QString shellIntegrationPrimary READ shellIntegrationPrimary NOTIFY shellIntegrationCardChanged)
    Q_PROPERTY(
        QString shellIntegrationSecondary READ shellIntegrationSecondary NOTIFY shellIntegrationCardChanged)
    Q_PROPERTY(QString shellIntegrationError READ shellIntegrationError NOTIFY shellIntegrationCardChanged)
    Q_PROPERTY(QString fontFamily READ fontFamily NOTIFY shellIntegrationCardChanged)
    // }}}
    // {{{ The find bar and the permission question.
    // Mirrors TerminalSession's find bar surface (SearchBar.qml): the pattern, the summary and its flags,
    // the toggles as maps, and the current match's viewport rows.
    Q_PROPERTY(QString searchPattern READ searchPattern NOTIFY searchStateChanged)
    Q_PROPERTY(QString searchSummary READ searchSummary NOTIFY searchStateChanged)
    Q_PROPERTY(bool searchHasMatches READ searchHasMatches NOTIFY searchStateChanged)
    Q_PROPERTY(bool searchNavigable READ searchNavigable NOTIFY searchStateChanged)
    Q_PROPERTY(QVariantList searchToggles READ searchToggles NOTIFY searchStateChanged)
    Q_PROPERTY(QVariantMap searchFocusedRows READ searchFocusedRows NOTIFY searchFocusedRowsChanged)
    Q_PROPERTY(QVariantMap pendingPermission READ pendingPermission NOTIFY pendingPermissionChanged)
    // }}}

  public:
    /// @param background The session background colour; configuration, hence fixed for the lifetime.
    explicit MockSession(QColor background = QColor(Qt::black)): _background(background) {}

    [[nodiscard]] int pageLineCount() const { return _lines; }
    [[nodiscard]] bool isReconnecting() const { return _reconnecting; }

    /// @return window::paneStateOf over the mocked end and connection, as TerminalSession answers it.
    [[nodiscard]] int paneState() const
    {
        using vthost::client::ConnectionState;
        return static_cast<int>(
            window::paneStateOf(static_cast<session::EndReason>(_sessionEnd),
                                _reconnecting ? ConnectionState::Reconnecting : ConnectionState::Connected));
    }

    /// Loses or regains the host connection, as TerminalSession::setHostConnection does.
    void setReconnecting(bool reconnecting)
    {
        _reconnecting = reconnecting;
        emit reconnectingChanged();
        emit paneStateChanged();
    }
    [[nodiscard]] int pageColumnsCount() const { return _columns; }
    [[nodiscard]] int historyLineCount() const { return _historyLineCount; }
    [[nodiscard]] QString sizeIndicatorText() const { return _sizeIndicatorText; }

    /// Raises the size indicator with @p text, as TerminalSession::requestSizeIndicator() does once its gate
    /// lets a request through.
    void requestSizeIndicator(QString text)
    {
        _sizeIndicatorText = std::move(text);
        emit sizeIndicatorRequested();
    }

    /// Records each settle, by whether the pane was active. A bool, as the QML-facing call takes it.
    Q_INVOKABLE void sizeIndicatorSettled(bool paneActive) { settledSizeIndicators.push_back(paneActive); }

    /// Every sizeIndicatorSettled() call, in order.
    std::vector<bool> settledSizeIndicators;

    /// Mirrors TerminalSession::insertDroppedUrls(): what a file drop on the pane hands its session.
    Q_INVOKABLE void insertDroppedUrls(QList<QUrl> const& urls) { droppedUrls.push_back(urls); }
    /// Mirrors TerminalSession::insertDroppedText(): what a text drop on the pane hands its session.
    Q_INVOKABLE void insertDroppedText(QString const& text) { droppedTexts.push_back(text); }
    /// Every insertDroppedUrls() call, in order.
    std::vector<QList<QUrl>> droppedUrls;
    /// Every insertDroppedText() call, in order.
    std::vector<QString> droppedTexts;
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
    [[nodiscard]] QString linkTargetText() const { return _linkTargetText; }
    [[nodiscard]] QString linkTargetHint() const { return _linkTargetHint; }
    [[nodiscard]] bool linkTargetShown() const { return _linkTargetShown; }
    [[nodiscard]] int overlayClearanceRevision() const { return _overlayClearanceRevision; }
    Q_INVOKABLE [[nodiscard]] QVariantMap overlayClearance() const { return _overlayClearance; }

    /// Publishes the link under the pointer, the way TerminalSession does. A bool, as its QML property is.
    void setLinkTarget(QString text, QString hint, bool shown)
    {
        _linkTargetText = std::move(text);
        _linkTargetHint = std::move(hint);
        _linkTargetShown = shown;
        emit linkTargetChanged();
    }

    /// Publishes where an overlay may go, and bumps the revision QML re-reads it on.
    void setOverlayClearance(QVariantMap clearance)
    {
        _overlayClearance = std::move(clearance);
        ++_overlayClearanceRevision;
        emit overlayClearanceChanged();
    }
    [[nodiscard]] QRectF blockActionsAnchor() const { return _blockActionsAnchor; }
    [[nodiscard]] qulonglong hoveredCommandBlock() const { return _hoveredCommandBlock; }
    [[nodiscard]] qulonglong focusedCommandBlock() const { return _focusedCommandBlock; }
    [[nodiscard]] QVariantMap stickyHeader() const { return _stickyHeader; }

    /// Publishes the hovered command block and its button's anchor, the way TerminalSession does when the
    /// pointer reaches one; an empty @p anchor and block 0 are the pointer leaving.
    void setBlockHover(qulonglong block, QRectF anchor)
    {
        _hoveredCommandBlock = block;
        _blockActionsAnchor = anchor;
        emit blockActionsAnchorChanged();
    }

    /// Publishes the block the keyboard focuses, 0 for none.
    void setFocusedCommandBlock(qulonglong block)
    {
        _focusedCommandBlock = block;
        emit focusedCommandBlockChanged();
    }

    /// Publishes the sticky header the chrome shows, as TerminalSession::stickyHeader() binds it; empty for
    /// none.
    void setStickyHeader(QVariantMap header)
    {
        _stickyHeader = std::move(header);
        emit stickyHeaderChanged();
    }

    /// Counts the clicks on the ⋯ button.
    Q_INVOKABLE void openBlockActions() { ++blockActionOpens; }

    /// Counts the clicks on the sticky header's jump link.
    Q_INVOKABLE void jumpToStickyHeader() { ++stickyJumps; }

    /// Records where the card says the sticky header lives, as window::StickyHeaderHost values.
    Q_INVOKABLE void setStickyHeaderHost(int host) { stickyHosts.push_back(host); }

    int blockActionOpens = 0;     ///< How often the ⋯ button asked for the block menu.
    int stickyJumps = 0;          ///< How often the jump link was clicked.
    std::vector<int> stickyHosts; ///< Every host the card announced, in order.

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
    [[nodiscard]] int secureIndicator() const noexcept { return _secureIndicator; }

    /// The program in the pane hid what is typed, or showed it again, as TerminalSession::refreshInputEcho()
    /// tells.
    /// @param indicator A window::SecureIndicator's value.
    void setSecureIndicator(int indicator)
    {
        _secureIndicator = indicator;
        emit secureIndicatorChanged();
    }
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

    // {{{ The ended pane
    [[nodiscard]] int sessionEnd() const noexcept { return _sessionEnd; }
    [[nodiscard]] QString const& endMessage() const noexcept { return _endMessage; }
    [[nodiscard]] int paneFooter() const noexcept { return _paneFooter; }
    [[nodiscard]] QString const& footerMessage() const noexcept { return _footerMessage; }
    [[nodiscard]] QString const& footerActionLabel() const noexcept { return _footerActionLabel; }
    [[nodiscard]] QString const& footerDismissLabel() const noexcept { return _footerDismissLabel; }
    [[nodiscard]] int footerTakesReturn() const noexcept { return _footerTakesReturn; }

    /// The session ended and its pane waits, as TerminalSession::recordEndLocked() tells: the Ended footer.
    /// @param end    A session::EndReason's value; 0 for a running session (no footer).
    /// @param message What the footer says.
    /// @param action A session::FooterAction's value: the primary button, None for only the dismissal.
    void setEnd(int end, QString message, int action)
    {
        _sessionEnd = end;
        _endMessage = message;
        auto const ended = end != static_cast<int>(contour::session::EndReason::None);
        auto const offered = ended && action != static_cast<int>(contour::session::FooterAction::None);
        _paneFooter = static_cast<int>(ended ? contour::window::PaneFooterKind::Ended
                                             : contour::window::PaneFooterKind::None);
        _footerMessage = ended ? std::move(message) : QString {};
        _footerActionLabel = offered ? QString::fromUtf8(contour::session::paneFooterRowOf(
                                                             static_cast<contour::session::EndReason>(end))
                                                             .actionLabel)
                                     : QString {};
        _footerDismissLabel =
            ended
                ? QString::fromUtf8(contour::session::dismissLabelOf(contour::session::FooterDismiss::Close))
                : QString {};
        _footerTakesReturn = ended ? 1 : 0;
        emit sessionEndChanged();
        emit paneFooterChanged();
        emit paneStateChanged();
    }

    /// A restart stopped a command in the shell, as TerminalSession::noteInterruption() tells: the
    /// Interrupted footer.
    /// @param message     What the footer says.
    /// @param actionLabel The primary button's word; empty before the shell shows a prompt.
    void setInterrupted(QString message, QString actionLabel)
    {
        _paneFooter = static_cast<int>(contour::window::PaneFooterKind::Interrupted);
        _footerMessage = std::move(message);
        _footerActionLabel = std::move(actionLabel);
        _footerDismissLabel = QString::fromUtf8(
            contour::session::dismissLabelOf(contour::session::InterruptedFooterRow.dismiss));
        _footerTakesReturn = 0;
        emit paneFooterChanged();
    }

    /// Counts the footer's primary button, as TerminalSession::runFooterAction() would run it.
    Q_INVOKABLE void runFooterAction() { ++footerActions; }
    /// Counts the footer's dismissing button, as TerminalSession::dismissFooter() would answer it.
    Q_INVOKABLE void dismissFooter() { ++footerDismissals; }

    int footerActions = 0;    ///< How often runFooterAction() ran.
    int footerDismissals = 0; ///< How often dismissFooter() ran.
    // }}}

    // {{{ The shell-integration card
    /// The shell and startup file the card's `{shell}` and `{rc}` placeholders name, as the real session
    /// substitutes them through window::cardText().
    static constexpr std::string_view MockCardShell = "zsh";
    static constexpr std::string_view MockCardRc = "~/.zshrc";
    [[nodiscard]] int shellIntegrationCard() const noexcept { return std::to_underlying(_cardStage); }
    [[nodiscard]] QString shellIntegrationTitle() const { return cardWords(&window::CardStageRow::title); }
    [[nodiscard]] QString shellIntegrationBody() const { return cardWords(&window::CardStageRow::body); }
    [[nodiscard]] QString shellIntegrationPrimary() const
    {
        return cardWords(&window::CardStageRow::primary);
    }
    [[nodiscard]] QString shellIntegrationSecondary() const
    {
        return cardWords(&window::CardStageRow::secondary);
    }
    [[nodiscard]] QString const& shellIntegrationError() const noexcept { return _cardError; }
    [[nodiscard]] QString fontFamily() const { return _fontFamily; }

    /// Shows the card on @p stage, as TerminalSession::showShellIntegrationCard does.
    /// @param stage The stage to show; Hidden hides it.
    /// @param line The line the Confirm stage shows as its body.
    /// @param error What the Confirm stage says went wrong; empty for nothing.
    void setShellCard(window::CardStage stage, QString line = {}, QString error = {})
    {
        _cardStage = stage;
        _cardLine = std::move(line);
        _cardError = std::move(error);
        emit shellIntegrationCardChanged();
    }

    /// Counts the card's accent button, as TerminalSession::acceptShellIntegrationCard() would act on it.
    Q_INVOKABLE void acceptShellIntegrationCard() { ++cardAccepts; }
    /// Counts the card's other button, as TerminalSession::declineShellIntegrationCard() would act on it.
    Q_INVOKABLE void declineShellIntegrationCard() { ++cardDeclines; }

    int cardAccepts = 0;  ///< How often acceptShellIntegrationCard() ran.
    int cardDeclines = 0; ///< How often declineShellIntegrationCard() ran.
    // }}}

    // {{{ The find bar
    [[nodiscard]] QString searchPattern() const { return _searchPattern; }
    [[nodiscard]] QString searchSummary() const { return _searchSummary; }
    [[nodiscard]] bool searchHasMatches() const { return _searchHasMatches; }
    [[nodiscard]] bool searchNavigable() const { return _searchNavigable; }
    [[nodiscard]] QVariantMap searchFocusedRows() const { return _searchFocusedRows; }

    /// The toggles as TerminalSession publishes them, from the same table and the same answers.
    [[nodiscard]] QVariantList searchToggles() const
    {
        auto toggles = QVariantList {};
        for (auto const& row: session::SearchToggleRows)
        {
            auto const affordance = session::describeSearchToggle(row.toggle, searchOptions);
            toggles.append(QVariantMap {
                { QStringLiteral("toggle"), static_cast<int>(row.toggle) },
                { QStringLiteral("glyph"), QString::fromUtf8(affordance.glyph) },
                { QStringLiteral("checked"), affordance.state == session::ToggleState::On },
                { QStringLiteral("tooltip"), QString::fromUtf8(row.accessibleName) },
                { QStringLiteral("accessibleName"), QString::fromUtf8(row.accessibleName) },
                { QStringLiteral("objectName"), QString::fromUtf8(row.objectName) },
            });
        }
        return toggles;
    }

    /// Takes @p pattern as the field pushes it, and answers it crudely: empty is idle, "zzz" no match, and
    /// anything else "1 of 3".
    Q_INVOKABLE void setSearchPattern(QString const& pattern)
    {
        ++setPatternCalls;
        _searchPattern = pattern;
        if (pattern.isEmpty())
            setSearchSummary({}, SearchMatchPresence::Some, SearchNavigation::None);
        else if (pattern == QStringLiteral("zzz"))
            setSearchSummary(QStringLiteral("No results"), SearchMatchPresence::None, SearchNavigation::None);
        else
            setSearchSummary(QStringLiteral("1 of 3"), SearchMatchPresence::Some, SearchNavigation::Some);
    }

    /// Records a pressed toggle and moves its option on, as TerminalSession::toggleSearchOption does.
    Q_INVOKABLE void toggleSearchOption(int toggle)
    {
        toggledOptions.push_back(toggle);
        if (toggle < 0 || std::cmp_greater_equal(toggle, session::SearchToggleRows.size()))
            return;
        searchOptions = session::nextSearchOptions(static_cast<session::SearchToggle>(toggle), searchOptions);
        emit searchStateChanged();
    }

    /// Installs @p text as the pattern and asks for the find bar, as the SearchSelection action does.
    void searchSelection(QString const& text)
    {
        setSearchPattern(text);
        emit searchBarRequested();
    }

    Q_INVOKABLE void searchBarOpened() { ++openNotifications; }
    Q_INVOKABLE void searchBarClosed() { ++closeNotifications; }
    Q_INVOKABLE void searchNext() { ++nextCalls; }
    Q_INVOKABLE void searchPrevious() { ++previousCalls; }

    /// Whether the mocked search matched: drives the field's tint.
    enum class SearchMatchPresence : uint8_t
    {
        None = 0,
        Some,
    };
    /// Whether stepping would go anywhere.
    enum class SearchNavigation : uint8_t
    {
        None = 0,
        Some,
    };

    /// Publishes a summary, as a tally or an invalid pattern would.
    void setSearchSummary(QString summary, SearchMatchPresence presence, SearchNavigation navigation)
    {
        _searchSummary = std::move(summary);
        _searchHasMatches = presence == SearchMatchPresence::Some;
        _searchNavigable = navigation == SearchNavigation::Some;
        emit searchStateChanged();
    }

    /// Publishes the current match's viewport rows; an empty map for none.
    void setSearchFocusedRows(QVariantMap rows)
    {
        _searchFocusedRows = std::move(rows);
        emit searchFocusedRowsChanged();
    }

    vtbackend::SearchOptions searchOptions {}; ///< The toggles' state.
    std::vector<int> toggledOptions;           ///< Every toggle pressed, in order.
    int openNotifications = 0;                 ///< How often the bar said it opened.
    int closeNotifications = 0;                ///< How often it said it closed.
    int setPatternCalls = 0;                   ///< Pushes from the field, to tell a re-seed from a keystroke.
    int nextCalls = 0;                         ///< searchNext() calls.
    int previousCalls = 0;                     ///< searchPrevious() calls.
    // }}}

    // {{{ The permission question
    [[nodiscard]] QVariantMap const& pendingPermission() const noexcept { return _pendingPermission; }

    /// The session asks another question, as TerminalSession::publishPendingPermission() tells.
    void setPendingPermission(QVariantMap prompt)
    {
        _pendingPermission = std::move(prompt);
        emit pendingPermissionChanged();
    }

    /// Records the answer, as TerminalSession::answerPermission() would run the question's role.
    Q_INVOKABLE void answerPermission(int questionId,
                                      contour::session::permission::PermissionDecision decision,
                                      contour::session::permission::PermissionMemory memory)
    {
        lastPermissionAnswer = PermissionAnswer { .id = questionId, .decision = decision, .memory = memory };
        ++permissionAnswers;
        // As an extension's consent answered while another waits once did: the question goes, and the next
        // is put up inside the answer.
        if (auto next = std::exchange(nextPermissionOnAnswer, std::nullopt))
        {
            setPendingPermission({});
            setPendingPermission(std::move(*next));
        }
    }

    /// The question the next answer puts up from inside answerPermission(), if any.
    std::optional<QVariantMap> nextPermissionOnAnswer;

    /// An answer the chrome gave: to which question, and what.
    struct PermissionAnswer
    {
        int id = 0;
        contour::session::PermissionDecision decision = contour::session::PermissionDecision::Deny;
        contour::session::PermissionMemory memory = contour::session::PermissionMemory::Once;
    };

    /// The last answer given, if any.
    std::optional<PermissionAnswer> lastPermissionAnswer;
    int permissionAnswers = 0; ///< How often the chrome answered.
                               // }}}

  signals:
    void lineCountChanged();
    void columnsCountChanged();
    void historyLineCountChanged();
    void isScrollbarRightChanged();
    void isScrollbarVisibleChanged();
    void sizeIndicatorRequested();
    void linkTargetChanged();
    void secureIndicatorChanged();
    void overlayClearanceChanged();
    void blockActionsAnchorChanged();
    void focusedCommandBlockChanged();
    void stickyHeaderChanged();
    void scrollbarMarksChanged();
    void opacityChanged();
    void dimUnfocusedChanged();
    void reconnectingChanged();
    void paneStateChanged();
    void displayWorkingDirectoryChanged();
    void foregroundProcessNameChanged();
    void titleChanged();
    void paneProgramStatusChanged();
    void sessionEndChanged();
    void paneFooterChanged();
    void shellIntegrationCardChanged();
    void searchStateChanged();
    void searchFocusedRowsChanged();
    void searchBarRequested(); //!< SessionChrome opens and focuses the find bar on it.
    void onBell();
    void onAlert();
    void onShowNotification(QString const& /*title*/, QString const& /*body*/);
    void onScrollOffsetChanged(int /*value*/);
    void pendingPermissionChanged();

  private:
    QString _searchPattern;
    QString _searchSummary;
    bool _searchHasMatches = true;
    bool _searchNavigable = false;
    QVariantMap _searchFocusedRows;
    int _columns = 80;
    int _lines = 25;
    int _historyLineCount = 0;
    bool _scrollbarRight = true;
    bool _scrollbarVisible = false;
    QString _sizeIndicatorText;
    QString _linkTargetText;
    QString _linkTargetHint;
    bool _linkTargetShown = false;
    int _overlayClearanceRevision = 0;
    QVariantMap _overlayClearance;
    QRectF _blockActionsAnchor;
    qulonglong _hoveredCommandBlock = 0;
    qulonglong _focusedCommandBlock = 0;
    QVariantMap _stickyHeader;
    QVariantList _scrollbarMarks;
    QVariantList _scrollbarMarkColors;
    QColor _background;
    float _opacity = 1.0F;
    float _dimUnfocused = 0.0F;
    bool _reconnecting = false;
    QString _displayWorkingDirectory;
    QString _foregroundProcessName;
    QString _title;
    QVariantMap _pendingPermission;
    int _programStatus = 0;   ///< window::TabBadgeKind::None.
    int _secureIndicator = 0; ///< window::SecureIndicator::None.
    QString _programStatusText;
    int _sessionEnd = 0; ///< session::EndReason::None.
    QString _endMessage;
    int _paneFooter = 0; ///< window::PaneFooterKind::None.
    QString _footerMessage;
    QString _footerActionLabel;
    QString _footerDismissLabel;
    int _footerTakesReturn = 0;
    window::CardStage _cardStage = window::CardStage::Hidden;
    QString _cardLine;
    QString _cardError;
    QString _fontFamily = QStringLiteral("Monospace"); ///< The card's line is set in it.

    /// One column of the current stage's CardStageRows row.
    [[nodiscard]] QString cardWords(std::string_view window::CardStageRow::* column) const
    {
        return QString::fromStdString(
            window::cardText(window::CardStageRows[std::to_underlying(_cardStage)].*column,
                             MockCardShell,
                             MockCardRc,
                             _cardLine.toStdString()));
    }
};

/// A MockSession whose viewport scrolls, as TerminalSession's does: the chrome writes the offset when the bar
/// moves, and is told whenever it moved.
class ScrollingMockSession final: public MockSession
{
    Q_OBJECT
    Q_PROPERTY(int scrollOffset READ scrollOffset WRITE setScrollOffset NOTIFY scrollOffsetChanged)

  public:
    using MockSession::MockSession;

    [[nodiscard]] int scrollOffset() const { return _scrollOffset; }

    /// Moves the viewport, announcing it only when it moved, as the terminal's viewport does.
    void setScrollOffset(int value)
    {
        if (value == _scrollOffset)
            return;
        _scrollOffset = value;
        emit scrollOffsetChanged();
    }

  signals:
    void scrollOffsetChanged();

  private:
    int _scrollOffset = 0;
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
    Q_INVOKABLE void activate()
    {
        ++activateCalls;
        if (onActivate)
            onActivate();
    }
    int activateCalls = 0;
    /// What activate() does beyond counting, as the model would (make this leaf the active one); nothing if
    /// unset.
    std::function<void()> onActivate;
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
/// the test engine, which does not link the real display type. Behaves as a plain Item, save for the pointer:
/// it takes every button's press and the hover as TerminalDisplay does, so a click on a pane meets the grab
/// the real grid takes.
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
    Q_PROPERTY(int cellGeometryRevision READ cellGeometryRevision NOTIFY cellGeometryChanged)
  public:
    StubContourTerminal()
    {
        setAcceptedMouseButtons(Qt::AllButtons);
        setAcceptHoverEvents(true);
    }

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

    /// The band rows @p first to @p last take, as TerminalDisplay::rowBand answers: no margin, rows of
    /// @ref setRowHeight's height (20 pixels until it is called).
    Q_INVOKABLE [[nodiscard]] QVariantMap rowBand(int first, int last) const
    {
        auto const band = window::rowBandOf(first, last, 0.0, _rowHeight);
        return { { QStringLiteral("top"), band.top }, { QStringLiteral("bottom"), band.bottom } };
    }

    /// @return How many times @ref setRowHeight changed the rows, as TerminalDisplay counts its cell
    /// geometry.
    [[nodiscard]] int cellGeometryRevision() const noexcept { return _cellGeometryRevision; }

    /// Changes the rows' height as a zoom does, and says so as TerminalDisplay does.
    /// @param height The new row height, in item pixels.
    void setRowHeight(double height)
    {
        _rowHeight = height;
        ++_cellGeometryRevision;
        emit cellGeometryChanged();
    }

  protected:
    /// Takes the press, as TerminalDisplay::mousePressEvent does: the stub becomes the exclusive grabber.
    void mousePressEvent(QMouseEvent* event) override { event->accept(); }

  signals:
    void sessionChanged(QObject* /*session*/);
    void showNotification(QString const& /*title*/, QString const& /*body*/);
    void terminated();          //!< TerminalPane's onTerminated handler binds to this.
    void cellGeometryChanged(); //!< The rows moved under an unmoved item.

  private:
    QObject* _session = nullptr;
    double _rowHeight = 20.0;
    int _cellGeometryRevision = 0;
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

/// Every item named @p name in the visual tree beneath @p root, in depth-first order.
///
/// The visual tree rather than findChildren(): Loader- and Repeater-made items and a SplitView's handles
/// have a parent item but no QObject parent.
/// @param root Where the walk starts.
/// @param name The object name to find.
/// @return The items, possibly none.
[[nodiscard]] inline std::vector<QQuickItem*> itemsNamed(QQuickItem const& root, QString const& name)
{
    auto found = std::vector<QQuickItem*> {};
    for (auto* child: root.childItems())
    {
        if (child->objectName() == name)
            found.push_back(child);
        auto const below = itemsNamed(*child, name);
        found.insert(found.end(), below.begin(), below.end());
    }
    return found;
}

} // namespace contour::test
