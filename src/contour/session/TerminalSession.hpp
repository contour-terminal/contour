// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/command/ContextMenu.hpp>
#include <contour/config/Actions.hpp>
#include <contour/config/Config.hpp>
#include <contour/input/HorizontalWheelGesture.hpp>
#include <contour/input/KeyboardLayout.hpp>
#include <contour/input/MouseMapping.hpp>
#include <contour/platform/Announcer.hpp>
#include <contour/platform/Audio.hpp>
#include <contour/platform/ColorConversion.hpp>
#include <contour/platform/Notifier.hpp>
#include <contour/platform/QtInvoke.hpp>
#include <contour/session/DisplaySurface.hpp>
#include <contour/session/HostedOperation.hpp>
#include <contour/session/HyperlinkTooltip.hpp>
#include <contour/session/SearchStatus.hpp>
#include <contour/session/SessionFactory.hpp>

#include <vtbackend/core/CommandBlockId.hpp>
#include <vtbackend/core/WorkingDirectory.hpp>
#include <vtbackend/screen/GutterSettings.hpp>
#include <vtbackend/screen/Terminal.hpp>
#include <vtbackend/shell/CommandLineText.hpp>
#include <vtbackend/shell/ScrollbarMarks.hpp>

#include <vtpty/Pty.hpp>

#include <vtrasterizer/Renderer.hpp>

#include <crispy/Point.hpp>

#include <QtCore/QAbstractItemModel>
#include <QtCore/QThread>
#include <QtCore/QTimer>
#include <QtCore/QVariant>
#include <QtQml/QJSValue>

#include <algorithm>
#include <array>
#include <atomic>
#include <concepts>
#include <cstdint>
#include <deque>
#include <expected>
#include <format>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <qcolor.h>

#include <vthost/client/ConnectionState.hpp>
#include <vtworkspace/Primitives.hpp>

namespace contour
{

namespace display
{
    class ForcedFontDpiProvider;
} // namespace display

namespace platform
{
    struct FinishNotificationRequest;
} // namespace platform

class ContourGuiApp;

} // namespace contour

namespace vtextension
{
struct Manifest;
class IGrantStore;
class SessionExtensions;
struct ExtensionConsentRequest;
enum class ConsentAnswer : uint8_t;
} // namespace vtextension

namespace contour::session
{

class TerminalSessionManager;

/**
 * A set of user-facing activities that are guarded behind a permission-check wall.
 */
enum class GuardedRole : uint8_t
{
    ChangeFont,
    CaptureBuffer,
    ShowHostWritableStatusLine,
    BigPaste,
    RerunCommand,
};

/**
 * Trivial cache to remember the interactive choice when the user has to be asked
 * and the user decided to permanently decide for the current session.
 */
using PermissionCache = std::map<GuardedRole, bool>;

/// Whether the gutter took an input event, and the grid must therefore not also see it.
///
/// Named rather than left to a bool because the call sites read as a question about routing, not about
/// truth: a bare `true` there says nothing about which of the two paths it selects.
enum class ConsumedByGutter : uint8_t
{
    No = 0,
    Yes,
};

/// Whether the sticky command header took a pointer press, and the grid must therefore not also see it.
///
/// Its own type rather than ConsumedByGutter: the two route independently -- a press beside row 0 can be
/// over the gutter, which the gutter takes, or over the header -- and a shared type would let one
/// handler's answer pass for the other's.
enum class ConsumedByStickyHeader : uint8_t
{
    No = 0,
    Yes,
};

/// Who took a pointer press away from the grid, and is therefore owed its release too: a press the
/// application never saw must not be followed by a release it does see, which would leave it holding a
/// button that was never pressed.
enum class PressOwner : uint8_t
{
    None = 0,     ///< Nobody: the grid's presses and releases go the normal way.
    Gutter,       ///< The gutter took the press.
    StickyHeader, ///< The sticky command header took the press.
    FoldLabel,    ///< A collapsed fold's `⋯ N lines` label took the press, and expanded its fold.
};

/// What a session's program is to the user. Fixed at construction: it decides how the session's end is
/// reported and whether a saved layout keeps the session's command.
enum class SessionRole : uint8_t
{
    /// A shell, or a program configured to run in its place. One that ends within the early-exit
    /// threshold has likely failed to start, so its pane is held open with a notice; a layout saves its
    /// command.
    Shell = 0,
    /// A pager over a command's output (OpenCommandOutput). Ending at once is how it is used, so its pane
    /// closes when it ends; a layout does not save its command, which names a private file that is gone by
    /// the time the layout is restored.
    CommandOutput,
};

/// What became of an action that changes engine state, in a session whose terminal may be hosted elsewhere.
enum class RemoteDispatch : uint8_t
{
    RunHere = 0, ///< The terminal is this session's own: the handler runs the action itself.
    Performed,   ///< The remote end that hosts the terminal performs it.
    Refused,     ///< The remote end cannot, or cannot be reached; the user has been told.
};

/// What an opener was to open when it failed, which names the failure's notice.
enum class OpenFailure : uint8_t
{
    CommandOutput = 0, ///< OpenCommandOutput's output, in a pager.
    File,              ///< A file, with file_opener.
};

/// What the follow gesture did at a cell.
enum class LinkFollow : uint8_t
{
    NothingThere = 0, ///< No link, or one that is not opened (a path printed on another machine).
    Opened,           ///< A link was opened.
    Probing,          ///< A path is being looked for on the probe thread; it opens once found.
};

/// Where a pointer over the gutter is: the row it names and the segment it is over.
struct GutterHit
{
    vtbackend::LineOffset gridLine {};  ///< The grid line beside the pointer.
    vtbackend::LineOffset screenRow {}; ///< The screen row that line is drawn on, a top status line counted.
    vtbackend::GutterSegmentKind segment {}; ///< The segment under the pointer.
};

/**
 * Manages a single terminal session (Client, Terminal, Display)
 *
 * This class is designed to be working in:
 * - graphical displays (OpenGL, software rasterized)
 * - text based displays (think of TMUX client)
 * - headless-mode (think of TMUX server)
 */
class TerminalSession: public QAbstractItemModel, public vtbackend::Terminal::Events
{
    Q_OBJECT
    Q_PROPERTY(int id READ id)
    Q_PROPERTY(int pageLineCount READ pageLineCount NOTIFY lineCountChanged)
    Q_PROPERTY(int pageColumnsCount READ pageColumnsCount NOTIFY columnsCountChanged)
    Q_PROPERTY(bool showResizeIndicator READ showResizeIndicator)
    Q_PROPERTY(int historyLineCount READ historyLineCount NOTIFY historyLineCountChanged)
    Q_PROPERTY(int scrollOffset READ scrollOffset WRITE setScrollOffset NOTIFY scrollOffsetChanged)
    Q_PROPERTY(QString title READ title WRITE setTitle NOTIFY titleChanged)
    Q_PROPERTY(float opacity READ getOpacity NOTIFY opacityChanged)
    Q_PROPERTY(float dimUnfocused READ getDimUnfocused NOTIFY dimUnfocusedChanged)
    Q_PROPERTY(QString pathToBackground READ pathToBackground NOTIFY pathToBackgroundChanged)
    Q_PROPERTY(float opacityBackground READ getOpacityBackground NOTIFY opacityBackgroundChanged)
    Q_PROPERTY(bool isImageBackground READ getIsImageBackground NOTIFY isImageBackgroundChanged)
    Q_PROPERTY(bool isBlurBackground READ getIsBlurBackground NOTIFY isBlurBackgroundChanged)
    Q_PROPERTY(QColor backgroundColor READ getBackgroundColor NOTIFY backgroundColorChanged)
    Q_PROPERTY(bool isScrollbarRight READ getIsScrollbarRight NOTIFY isScrollbarRightChanged)
    Q_PROPERTY(bool isScrollbarVisible READ getIsScrollbarVisible NOTIFY isScrollbarVisibleChanged)
    Q_PROPERTY(int fontSize READ getFontSize)
    Q_PROPERTY(int upTime READ getUptime)
    Q_PROPERTY(QString bellSource READ getBellSource NOTIFY onBell)
    // The OSC 8 hyperlink under the pointer. Empty means no tooltip. Both change together, so one
    // signal serves them: the anchor without its text describes nothing.
    Q_PROPERTY(QString hyperlinkTooltipText READ hyperlinkTooltipText NOTIFY hyperlinkHoverChanged)
    Q_PROPERTY(QRectF hyperlinkTooltipAnchor READ hyperlinkTooltipAnchor NOTIFY hyperlinkHoverChanged)
    // The command block under the pointer in the gutter's block column; an empty anchor means no tooltip.
    // The text is read through commandBlockTooltip() whenever the anchor's signal fires.
    Q_PROPERTY(
        QRectF commandBlockTooltipAnchor READ commandBlockTooltipAnchor NOTIFY commandBlockHoverChanged)

    // The scrollbar's tick marks, top first: { position: 0..1 down the track, kind: a ScrollbarMarkKind as
    // an int, targetLine: what scrollToScrollbarMark() takes }. Empty with nothing to mark, on the
    // alternate screen, and while scrollbar.marks is [] or the scrollbar is hidden. The colours are indexed
    // by kind; both lists are rebuilt together, so one signal serves them.
    Q_PROPERTY(QVariantList scrollbarMarks READ scrollbarMarks NOTIFY scrollbarMarksChanged)
    Q_PROPERTY(QVariantList scrollbarMarkColors READ scrollbarMarkColors NOTIFY scrollbarMarksChanged)

    // The find bar's state. All four change together -- a new pattern re-tallies, which re-words the
    // summary and re-decides whether stepping is possible -- so one signal serves them, as the
    // hyperlink pair above does.
    Q_PROPERTY(QString searchPattern READ searchPattern NOTIFY searchStateChanged)
    Q_PROPERTY(QString searchSummary READ searchSummary NOTIFY searchStateChanged)
    Q_PROPERTY(bool searchHasMatches READ searchHasMatches NOTIFY searchStateChanged)
    Q_PROPERTY(bool searchNavigable READ searchNavigable NOTIFY searchStateChanged)
    // The case affordance as what it SHOWS, not as an enumerator QML would have to interpret. The
    // bar spoke raw ints before and inverted every one of them; a glyph and a lit flag cannot be
    // read backwards. @see contour::session::describeSearchCase.
    Q_PROPERTY(QString searchCaseGlyph READ searchCaseGlyph NOTIFY searchStateChanged)
    Q_PROPERTY(QString searchCaseTooltip READ searchCaseTooltip NOTIFY searchStateChanged)
    Q_PROPERTY(bool searchCasePinned READ searchCasePinned NOTIFY searchStateChanged)
    // Whether the daemon connection this pane mirrors through is being sought again: a QML-facing property,
    // the documented bool exception (AGENT.md), read by TerminalPane.qml's reconnecting overlay.
    Q_PROPERTY(bool isReconnecting READ isReconnecting NOTIFY reconnectingChanged)

    // Q_PROPERTY(QString profileName READ profileName NOTIFY profileNameChanged)

  public:
    // {{{ Model property helper

    int getUptime() const noexcept
    {
        auto const now = std::chrono::steady_clock::now();
        auto const diff = std::chrono::duration_cast<std::chrono::seconds>(now - _startTime);
        return static_cast<int>(diff.count());
    }

    /// What the hyperlink tooltip should say, or empty for "show nothing".
    [[nodiscard]] QString hyperlinkTooltipText() const noexcept { return _hyperlinkTooltipText; }

    // {{{ Find bar
    /// The active search pattern, or empty when nothing is being searched for.
    [[nodiscard]] QString searchPattern() const { return _searchPatternText; }

    /// The label beside the field: "3 of 27", "No results", "" while idle.
    [[nodiscard]] QString searchSummary() const { return _searchSummary; }

    /// Whether the active pattern matches anything. Drives the field's error tint.
    [[nodiscard]] bool searchHasMatches() const noexcept
    {
        return _searchStatus.outcome != SearchOutcome::NoMatch;
    }

    /// Whether previous/next would go anywhere. @see MatchNavigation.
    [[nodiscard]] bool searchNavigable() const noexcept
    {
        return _searchStatus.navigation == MatchNavigation::Available;
    }

    [[nodiscard]] QString searchCaseGlyph() const
    {
        return QString::fromUtf8(_searchCase.glyph.data(), qsizetype(_searchCase.glyph.size()));
    }
    [[nodiscard]] QString searchCaseTooltip() const { return _searchCaseTooltip; }
    [[nodiscard]] bool searchCasePinned() const noexcept { return _searchCase.pinned == CasePinned::Yes; }

    /// Installs @p pattern as the search term and moves to its nearest match, live as the user types.
    Q_INVOKABLE void setSearchPattern(QString const& pattern);

    /// Moves to the next case policy and re-runs the search in place. @see nextSearchCase.
    Q_INVOKABLE void cycleSearchCaseSensitivity();

    /// The find bar opened, or closed. Told rather than inferred: only the bar knows, and a tally is
    /// worth computing only while something displays it.
    Q_INVOKABLE void searchBarOpened();
    Q_INVOKABLE void searchBarClosed();

    /// Steps to the next/previous match, updating the summary's ordinal.
    Q_INVOKABLE void searchNext();
    Q_INVOKABLE void searchPrevious();

    /// Re-reads the tally and re-derives the summary. Called after anything that can change either.
    void refreshSearchStatus();

  private:
    /// Which end of the scrollback a wrapped search restarts from.
    enum class SearchWrapEdge : uint8_t
    {
        Top,    ///< Next-match ran off the bottom; resume at the oldest line.
        Bottom, ///< Previous-match ran off the top; resume at the newest line.
    };

    /// The half of refreshSearchStatus() that reads the terminal, for callers already holding its
    /// lock -- so typing does not take the lock twice and walk the grid twice per keystroke.
    void refreshSearchStatusLocked();

    /// Renders @p status as the label the bar shows, translated.
    ///
    /// Here rather than in SearchStatus.hpp because tr() needs a QObject: that header decides WHICH
    /// thing to say, which is the part with rules worth testing, and this turns it into words.
    [[nodiscard]] QString renderSearchSummary(SearchStatus const& status) const;

    /// The case toggle's tooltip for @p mode, translated. @see describeSearchCase for the glyph.
    [[nodiscard]] QString renderSearchCaseTooltip(vtbackend::SearchCaseSensitivity mode) const;

    /// Arms the debounced tally. @see setSearchPattern for why counting is not done inline.
    void scheduleSearchTally();

    /// Restarts the search from @p edge, which is how the find bar wraps. @see searchNext.
    void wrapSearchTo(SearchWrapEdge edge);

    /// One step of the find bar's navigation, wrapping at @p edge when it cannot go further.
    void stepSearch(SearchWrapEdge edge);

  public:
    // }}}

    /// The cell the pointer entered the hyperlink at, in the display's item-local logical coordinates.
    ///
    /// The ENTRY cell, so the tooltip stays put while the pointer traces the link rather than sliding
    /// along with it.
    [[nodiscard]] QRectF hyperlinkTooltipAnchor() const noexcept { return _hyperlinkTooltipAnchor; }

    /// The gutter cell the hovered command block was entered at, in the display's item-local logical
    /// coordinates; empty while no block is hovered. The ENTRY cell, so the tooltip stays put while the
    /// pointer traces the block's rows.
    [[nodiscard]] QRectF commandBlockTooltipAnchor() const noexcept { return _commandBlockTooltipAnchor; }

    /// What the block-column tooltip says about the hovered command block, or empty for "show nothing"
    /// (@see commandBlockTooltipText). Reads the record under the terminal lock, at the moment it is asked.
    Q_INVOKABLE [[nodiscard]] QString commandBlockTooltip() const;

    /// Withdraws the hyperlink tooltip, whatever the pointer is over.
    ///
    /// Called when the pointer leaves the terminal or the grid, and when the viewport scrolls: the
    /// hovered-link state tracks mouse MOVEMENT, so scrolling moves the link out from under a stationary
    /// pointer without anything noticing. Hiding is the honest answer to that; continuing to show a tooltip
    /// for a link no longer under the pointer is worse than showing none. GUI thread only.
    void clearHyperlinkHover();

  private:
    /// Announces @p message, unless the user switched announcements off.
    void announce(
        QString const& message,
        QAccessible::AnnouncementPoliteness politeness = QAccessible::AnnouncementPoliteness::Polite);

  public:
    /// The pointer left the terminal entirely.
    ///
    /// Withdraws the hyperlink tooltip and puts the mouse cursor back to its default shape. The shape
    /// is reset here rather than left alone because it is only ever changed on a cell CHANGE, so a
    /// pointer that leaves while over a link would otherwise keep the pointing hand it was given.
    void onPointerLeft();

    /// Replaces the announcer this session speaks through.
    ///
    /// Injected rather than constructed here, so the DECISIONS below (what is worth announcing) are
    /// assertable against a recording implementation with no accessibility bridge in sight.
    void setAnnouncer(std::unique_ptr<platform::Announcer> announcer) { _announcer = std::move(announcer); }

  private:
    /// Feeds the hovered-link tracker and publishes any change to QML.
    ///
    /// @param uri  The hyperlink under the pointer, or empty when there is none.
    /// @param cell Where the pointer is, in viewport coordinates.
    void updateHyperlinkHover(std::string_view uri, vtbackend::CellLocation cell);

    /// What the tooltip says for the detected link under the pointer (detectedLinkTooltip()). The caller
    /// holds the terminal lock.
    /// @return The text; empty over no detected link, or one printed as it opens. Valid until the next call.
    [[nodiscard]] std::string const& hoveredDetectedLinkTooltip();

  public:
    QString getBellSource() const noexcept
    {
        if (_profile.bell.value().sound == "default")
        {
            return { "qrc:/contour/bell.wav" };
        }
        if (_profile.bell.value().sound == "off")
        {
            return {};
        }

        return QString::fromStdString(_profile.bell.value().sound);
    }

    int getFontSize() const noexcept { return static_cast<int>(_profile.fonts.value().size.pt); }
    float getOpacity() const noexcept
    {
        return static_cast<float>(_profile.background.value().opacity) / std::numeric_limits<uint8_t>::max();
    }
    /// Blend amount (0.0 = off .. 1.0) applied by TerminalPane.qml while this pane is unfocused
    /// (dim_unfocused profile option).
    float getDimUnfocused() const noexcept { return static_cast<float>(_profile.dimUnfocused.value()); }
    QString pathToBackground() const
    {
        // backgroundImage is a shared_ptr that is null whenever no background image is configured (the
        // common case), so it must be checked before dereferencing ->location — otherwise this is a null
        // member access (undefined behavior, caught by UBSan) even though it happens not to crash in
        // practice.
        auto const& backgroundImage = _terminal.colorPalette().backgroundImage;
        if (!backgroundImage)
            return {};

        if (auto const* p = std::get_if<std::filesystem::path>(&backgroundImage->location))
            return QString("file:") + QString(p->string().c_str());

        return {};
    }
    QColor getBackgroundColor() const noexcept
    {
        auto color = terminal().isModeEnabled(vtbackend::DECMode::ReverseVideo)
                         ? _terminal.colorPalette().defaultForeground
                         : _terminal.colorPalette().defaultBackground;
        auto alpha = static_cast<uint8_t>(_profile.background.value().opacity);
        return platform::toQColor(color, alpha);
    }
    float getOpacityBackground() const noexcept
    {
        if (_terminal.colorPalette().backgroundImage)
        {
            return _terminal.colorPalette().backgroundImage->opacity;
        }
        return 0.0;
    }
    bool getIsImageBackground() const noexcept
    {
        return static_cast<bool>(_terminal.colorPalette().backgroundImage);
    }

    bool getIsBlurBackground() const noexcept
    {
        if (getIsImageBackground())
        {
            return _terminal.colorPalette().backgroundImage->blur;
        }
        return false;
    }

    bool getIsScrollbarRight() const noexcept
    {
        return profile().scrollbar.value().position == config::ScrollBarPosition::Right;
    }

    bool getIsScrollbarVisible() const noexcept
    {
        if (profile().scrollbar.value().position == config::ScrollBarPosition::Hidden)
            return false;

        // The page on display: with DECPCCM reset the cursor may write to another page, under nobody's view.
        if ((_terminal.displayedScreenType() == vtbackend::ScreenType::Alternate)
            && profile().scrollbar.value().hideScrollbarInAltScreen)
            return false;

        return true;
    }

    /// Accumulates a wheel event towards whole-cell scroll steps.
    ///
    /// The horizontal component is dropped unless @ref HorizontalWheelGesture judges it intentional, so
    /// the sideways drift of a long vertical trackpad scroll never becomes a WheelLeft/WheelRight press
    /// in the first place — neither for the tab-switch fallback binding nor for an application that
    /// asked for mouse reporting.
    ///
    /// @param pixelDelta       Pixel-precise delta (trackpads), or {0,0}.
    /// @param angleDelta       Angle delta (wheels), or {0,0}.
    /// @param phase            The gesture phase the windowing system reported.
    /// @param platformInverted Whether the platform already flipped the delta (natural scrolling).
    void addToAccumulatedScroll(crispy::Point pixelDelta,
                                crispy::Point angleDelta,
                                vtbackend::ScrollPhase phase,
                                bool platformInverted) noexcept;

    /// Tells the horizontal-wheel gesture where a gesture begins and ends.
    ///
    /// Called for EVERY wheel event, before any of the paths that may consume one. @see
    /// HorizontalWheelGesture::notePhase for why a gesture boundary must not be missed.
    void noteScrollPhase(vtbackend::ScrollPhase phase) noexcept { _horizontalWheelGesture.notePhase(phase); }
    std::tuple<vtbackend::LineOffset, vtbackend::ColumnOffset> consumeScroll() noexcept;

    QString title() const;
    void setTitle(QString const& value) { terminal().setWindowTitle(value.toStdString()); }

    /// The main page's lines, read without the terminal lock. @see vtbackend::Terminal::mainPageSize()
    int pageLineCount() const noexcept { return unbox(_terminal.mainPageSize().lines); }

    /// The main page's columns, read without the terminal lock. @see vtbackend::Terminal::mainPageSize()
    int pageColumnsCount() const noexcept { return unbox(_terminal.mainPageSize().columns); }

    bool showResizeIndicator() const noexcept { return _config.profile().sizeIndicatorOnResize.value(); }

    /// How far the scrollbar can scroll up, in rows the user can actually reach.
    ///
    /// The scrollable count rather than the raw history depth: collapsed folds take their output out
    /// of the scrollable range, and a scrollbar sized by the raw history would offer a stretch of
    /// travel at the top that scrolls nowhere.
    ///
    /// Reported from what announceScrollableLineCount() last published, and emphatically NOT computed
    /// here -- see there for why this property may neither compute the count nor take the lock that
    /// would make computing it safe. It is therefore also exactly what the NOTIFY signal carried.
    int historyLineCount() const noexcept { return _lastHistoryLineCount; }

    int scrollOffset() const noexcept { return unbox(terminal().viewport().scrollOffset()); }

    /// Scrolls the view to @p value rows above the bottom: the QML scrollbar's write. Under the terminal
    /// lock, as the parser thread moves the view under it too, and through runUnderStateLock(), which runs it
    /// at once where this thread holds the lock already: while hovered, the bar writes back at once the
    /// offset a scroll announces, and a Vi motion or a scroll action announces it under the lock.
    /// @param value The offset, in rows above the bottom.
    void setScrollOffset(int value)
    {
        _terminal.runUnderStateLock([&] {
            _terminal.resetSmoothScroll();
            _terminal.viewport().scrollTo(vtbackend::ScrollOffset::cast_from(value));
        });
    }

    void onScrollOffsetChanged(vtbackend::ScrollOffset value) override;

    /// The ticks the scrollbar draws. @see the scrollbarMarks Q_PROPERTY for the shape of each entry.
    [[nodiscard]] QVariantList scrollbarMarks() const { return _scrollbarMarks; }

    /// One colour per ScrollbarMarkKind, in enumerator order, resolved from the session's colour scheme.
    [[nodiscard]] QVariantList scrollbarMarkColors() const { return _scrollbarMarkColors; }

    /// Tells the session how tall the scrollbar's track is, in logical pixels.
    ///
    /// A setter, deliberately: the track's height is externally-driven geometry -- the QML layout decides
    /// it, and every window resize moves it.
    /// @param pixels The track's height; zero or less means there is no track to mark.
    Q_INVOKABLE void setScrollbarMarkTrackPixels(int pixels);

    /// Scrolls so a tick's target line is the viewport's top row, opening a collapsed fold on the way.
    ///
    /// Does nothing when the target no longer names a row -- evicted, or renamed by a reflow since the ticks
    /// were published; the refresh that change scheduled replaces the tick anyway.
    /// @param targetLine A tick's targetLine.
    Q_INVOKABLE void scrollToScrollbarMark(qint64 targetLine);

    /// Publishes the scrollbar's travel (announceScrollableLineCount()), and rebuilds the ticks if anything
    /// they depend on moved, and announces them.
    ///
    /// GUI thread only, since it takes the terminal lock. Production reaches it from the frame timer that
    /// requestScrollbarMarksRefresh() arms, so it runs at most once per frame however fast output arrives;
    /// public so a test can run it without an event loop, as refreshSearchStatus() is.
    void refreshScrollbarMarks();
    // }}}

    // {{{ QAbstractItemModel overrides
    QModelIndex index(int row, int column, QModelIndex const& parent = QModelIndex()) const override;
    QModelIndex parent(QModelIndex const& child) const override;
    int rowCount(QModelIndex const& parent = QModelIndex()) const override;
    int columnCount(QModelIndex const& parent = QModelIndex()) const override;
    QVariant data(QModelIndex const& index, int role = Qt::DisplayRole) const override;
    bool setData(QModelIndex const& index, QVariant const& value, int role = Qt::EditRole) override;
    // }}}

    /**
     * Constructs a single terminal session.
     *
     * @param manager the owning session manager (may be null for a standalone/headless session).
     * @param pty a PTY object (can be process, networked, mockup, ...).
     * @param app the owning application, providing the shared config and defaults.
     * @param profileName the profile to run this session under. Empty (the default) selects the
     *        application's default profile (@c app.profileName()). Naming a profile explicitly is a
     *        composition-time choice — a session's profile is intrinsic to it — used to spawn a
     *        session under a profile other than the application default (and by tests to exercise
     *        config-driven behaviour). The name must resolve in @c app.config(); an unknown name
     *        falls back to the application default.
     * @param initialPageSize the terminal's initial total page size. Empty (the default) uses the
     *        profile's configured @c terminalSize; a new tab/split passes the live window's running
     *        page size here so the terminal grid — not just the child PTY — is born at the current
     *        window size instead of the profile default (see
     *        TerminalSessionManager::createSessionInBackground).
     * @param launchedCommand the command override this session's PTY was actually launched with, if
     *        any. Empty (the default) means the session launched the profile's configured shell.
     *        Recorded verbatim for later introspection (e.g. by layout tooling); does not affect the
     *        PTY itself, which is already spawned by the caller.
     * @param notifier how desktop notifications are raised. Null (the default) means the notifier
     *        this platform offers, per platform::makeDesktopNotifier(), with the hosting window's
     *        tray where it offers none; a test passes a recording one to assert what was raised and
     *        to drive close/activation events back.
     * @param role what the session's program is to the user (@see SessionRole). The default is a shell.
     */
    TerminalSession(TerminalSessionManager* manager,
                    std::unique_ptr<vtpty::Pty> pty,
                    ContourGuiApp& app,
                    std::string profileName = {},
                    std::optional<vtbackend::PageSize> initialPageSize = std::nullopt,
                    std::optional<vtpty::Process::ExecInfo> launchedCommand = std::nullopt,
                    std::unique_ptr<platform::Notifier> notifier = nullptr,
                    SessionRole role = SessionRole::Shell,
                    SessionHosting hosting = SessionHosting::Local);
    ~TerminalSession() override;

    int id() const noexcept { return _id; }

    /// What the session's program is to the user, as it was constructed.
    [[nodiscard]] SessionRole role() const noexcept { return _role; }

    /// The notifier this session raises desktop notifications through, as chosen at construction:
    /// the one it was handed, or else the platform's own.
    [[nodiscard]] platform::Notifier const& notifier() const noexcept { return *_desktopNotifier; }

    /// The command override this session was created with, if any (nullopt when it launched the
    /// profile's configured shell instead of an explicit command).
    [[nodiscard]] std::optional<vtpty::Process::ExecInfo> const& launchedCommand() const noexcept
    {
        return _launchedCommand;
    }

    /// The name of the profile this session was resolved against. Always a concrete profile: when
    /// the session was created without an explicit profile, this is the application default.
    [[nodiscard]] std::string const& profileName() const noexcept { return _profileName; }

    /// The profile name this session was EXPLICITLY created under, if any (nullopt when it runs
    /// the application's default profile). This is what SaveLayout persists: a saved pane must
    /// keep following the user's default profile unless it genuinely overrode it, so the implicit
    /// default must never be captured as if it were a per-pane choice.
    [[nodiscard]] std::optional<std::string> const& profileOverride() const noexcept
    {
        return _profileOverride;
    }

    /// The id by which the vtworkspace layout model refers to this session. Set when the manager mirrors
    /// the session into the model. Identifies the leaf pane that hosts this session.
    [[nodiscard]] vtworkspace::SessionId modelSessionId() const noexcept { return _modelSessionId; }
    void setModelSessionId(vtworkspace::SessionId id) noexcept { _modelSessionId = id; }

    std::optional<std::string> name() const
    {
        // Resolve under the terminal's _stateMutex: this runs on the GUI thread (via
        // TerminalSessionManager::updateStatusLine(), reached from a posted refreshGuiTabInfoForStatusLine
        // or on tab activation) while the parser thread writes the title strings under that mutex.
        // Reading them across separate unlocked accessor calls would race the writer.
        return terminal().resolvedTabName();
    }

    /// The raw OS-window title, for the GUI tab-label {WindowTitle} placeholder.
    ///
    /// Distinct from name(): name() goes through resolvedTabName(), which honors TabsNamingMode and
    /// returns nullopt unless Title mode is active (the status-line {Tabs} semantics). The GUI tab
    /// label wants the raw title regardless of mode, read thread-safely.
    /// @return The raw window title (empty if none has been set).
    [[nodiscard]] std::string resolvedWindowTitle() const { return terminal().resolvedWindowTitle(); }

    /// Starts the PTY device and, if it came up, the VT background threads.
    ///
    /// Total: a device that cannot be started is REPORTED into this session's screen, never
    /// propagated. Callers reach start() from inside Qt event handlers (see
    /// TerminalDisplay::setSession), where an escaping exception would abandon a
    /// half-constructed display — the crash of issue #1711.
    void start();

    /// Initiates termination of this session, regardless of the underlying terminal state.
    /// Works whether or not a display is attached: with a display the teardown is routed through
    /// closeDisplay(); without one the PTY device is closed directly so the same
    /// onClosed() -> sessionClosed -> TerminalSessionManager::removeSession path still runs (so a
    /// background tab/split-pane session does not leak when closed).
    void terminate();

    /// Records that this session's transport (the daemon connection) died, before the ptys it
    /// fed are closed.
    ///
    /// A daemon-hosted pane's device is a socket-backed pty, so a lost connection arrives at
    /// onClosed() looking exactly like a shell exiting. Only the caller tearing the connection
    /// down knows the difference, and it must say so BEFORE closing the device — that close is
    /// what wakes the exit watcher. Runtime state, not construction-time configuration: a
    /// session is born connected and may lose that at any point.
    void noteTransportLost() noexcept { _transportLost = true; }

    /// Tells a pane that mirrors a remote session where the connection to its host stands, so it can show
    /// that it is reconnecting rather than a grid that merely stopped. Runtime state, not configuration: the
    /// connection comes and goes while the session lives. A local session has no such connection and
    /// ignores it.
    /// @param state Where the connection stands now.
    void setHostConnection(vthost::client::ConnectionState state);

    /// @return Whether this pane's connection to its host is lost and being sought again.
    [[nodiscard]] bool isReconnecting() const noexcept
    {
        return _hostConnection == vthost::client::ConnectionState::Reconnecting;
    }

    config::Config const& config() const noexcept { return _config; }
    config::TerminalProfile const& profile() const noexcept { return _profile; }

    /// Resolves @p button through the BUILT-IN fallback mouse mappings alone and runs what it binds.
    ///
    /// For input that arrives over the window chrome (the tab strip) rather than the grid: there is no
    /// cell under the pointer, so neither the terminal's mouse protocol nor the user's own
    /// `input_mapping:` entries — which may bind cell-relative actions like FollowHyperlink — can
    /// meaningfully apply. Going straight to the fallback table is what keeps the tab-switch binding and
    /// its config gate defined in exactly one place rather than restated for the strip.
    ///
    /// @param button The button to resolve, matched with no modifiers.
    /// @return true when an enabled fallback row matched and its actions ran.
    bool applyFallbackMouseBinding(vtbackend::MouseButton button);

    vtpty::Pty& pty() noexcept { return _terminal.device(); }
    vtbackend::Terminal& terminal() noexcept { return _terminal; }
    vtbackend::Terminal const& terminal() const noexcept { return _terminal; }

    /// The session's current working directory, for inheritance when spawning a new tab, window or
    /// split pane from this one. On non-Windows this reads the local Process's working directory (when
    /// the device is a local process); on Windows it queries the terminal's reported cwd under the
    /// terminal lock. Falls back to "." when no working directory can be determined (e.g. an SSH
    /// session on non-Windows). Centralizing this here keeps every spawn path — new tab, new window and
    /// split pane — using the same logic, including the Windows fallback.
    /// @return The working directory path, or "." if unavailable.
    [[nodiscard]] std::string workingDirectory() const;

    /// This machine's identity, for deciding whether a context describes it.
    ///
    /// Also the vtbackend::Terminal::Events hook the terminal reads at OSC 133;C, on the parser thread:
    /// both strings are fixed at construction, so answering there needs no lock.
    [[nodiscard]] vtbackend::LocalIdentity localIdentity() const noexcept override
    {
        return _localIdentity.view();
    }

    /// The working directory for @p purpose, resolved from the OSC 3008 ancestry and then OSC 7.
    ///
    /// One resolution consulted by every caller, so "where does a new tab start" and "what does the
    /// tab tooltip say" can never disagree about the ORDER even though they deliberately disagree
    /// about the FILTER: a path worth SHOWING need not be one a child could be spawned in. The
    /// terminal's (vtbackend::Terminal::resolveWorkingDirectory), so a pane mirroring a daemon on
    /// another machine holds every directory foreign.
    ///
    /// Takes the terminal lock. A caller that already holds it must use
    /// @ref resolveWorkingDirectoryLocked instead.
    [[nodiscard]] std::optional<vtbackend::ResolvedWorkingDirectory> resolveWorkingDirectory(
        vtbackend::CwdPurpose purpose) const;

    /// As @ref resolveWorkingDirectory, for a caller that ALREADY holds the terminal lock.
    ///
    /// Two named functions rather than a `locked` flag, per the project's own rule about boolean
    /// parameters -- and the distinction is load-bearing rather than stylistic: the terminal's mutex is
    /// NOT recursive, so taking it a second time deadlocks the GUI thread outright. contextMenuState()
    /// is exactly such a caller, its whole body being one locked snapshot.
    [[nodiscard]] std::optional<vtbackend::ResolvedWorkingDirectory> resolveWorkingDirectoryLocked(
        vtbackend::CwdPurpose purpose) const;

    /// The working directory to SHOW the user, as opposed to the one to spawn a child in.
    ///
    /// Prefers what the shell reported over OSC 7, which is the only source that tracks a `cd` inside a
    /// full-screen application and the only one that is right for a remote (SSH) session. Falls back to
    /// the local process's directory — the one the session was started in — when the shell has reported
    /// nothing yet.
    ///
    /// Distinct from workingDirectory() on purpose: that one answers "where should a new tab start",
    /// must name a directory that exists on THIS machine, and says "." when it cannot tell. Neither is
    /// something to put in front of a user.
    ///
    /// Made safe to show (vtbackend::SanitizePurpose::Display): a control or bidi override a directory name
    /// holds is replaced, not shown raw.
    ///
    /// @return The directory, or an empty string when none can be determined.
    [[nodiscard]] std::string displayWorkingDirectory() const;

    /// The view this session renders through (@ref DisplaySurface), or nullptr while it has none —
    /// a background tab that was never shown, or a headless test.
    [[nodiscard]] DisplaySurface* display() noexcept { return _display; }
    [[nodiscard]] DisplaySurface const* display() const noexcept { return _display; }

    /// @return The shape the application last requested via `OSC 22`, or nullopt while it has
    ///         requested none. Survives a display hand-off; see _applicationPointerShape.
    [[nodiscard]] std::optional<input::MouseCursorShape> applicationPointerShape() const noexcept
    {
        return _applicationPointerShape.load();
    }

    void attachDisplay(DisplaySurface& display);
    void detachDisplay(DisplaySurface& display);

    /// Tells the terminal's status-line extensions the font the attached display now draws in: its point
    /// size and its cell size in device pixels. The display calls it wherever a font or pixel-density change
    /// is applied -- the reconfig recompute (TerminalDisplay::recomputeGeometryAfterFontReconfig) and the
    /// setup-time materialization (TerminalDisplay::createRenderer) -- and attachDisplay() once for the
    /// display's current font. Without a display, or without extensions, it returns after one lock-free
    /// test; extensions a reload installs later are told when they are (statusLineExtensionsReplaced).
    void updateStatusLineFontMetrics();

    TerminalSessionManager* getTerminalManager() const noexcept { return _manager; }

    /// The app-wide forced-font-DPI provider (see display/ContentScale.h), for injection into the
    /// display. Routed through the session so the display layer needs no ContourGuiApp dependency.
    /// @return The provider, or nullptr when no Qt application exists (tests).
    [[nodiscard]] display::ForcedFontDpiProvider* forcedFontDpiProvider() noexcept;

    /// The app-wide keyboard layout (see KeyboardLayout.h), for the display's key path. Routed
    /// through the session for the same reason as the DPI provider above.
    /// @return The layout; never nullptr.
    [[nodiscard]] input::KeyboardLayout const& keyboardLayout() const noexcept;

    Q_INVOKABLE void applyPendingFontChange(bool allow, bool remember);
    Q_INVOKABLE void applyPendingPaste(bool allow, bool remember);
    Q_INVOKABLE void executePendingBufferCapture(bool allow, bool remember);
    Q_INVOKABLE void executeShowHostWritableStatusLine(bool allow, bool remember);

    /// Runs the command RerunCommand asked about, if @p allow: typed at the prompt and entered -- unless
    /// the prompt went away while the dialog was up, or the line can no longer be typed as it is.
    /// The two bools are the dialog's answer as every permission wall here gives it to QML.
    /// @param allow Whether the user allowed it.
    /// @param remember Whether the answer stands for the rest of the session.
    Q_INVOKABLE void applyPendingRerun(bool allow, bool remember);

    /// The command RerunCommand asks to run, made safe to show; empty while none is waiting.
    /// @return The command line, for the dialog that asks.
    [[nodiscard]] Q_INVOKABLE QString pendingRerunCommandLine() const;
    Q_INVOKABLE void resizeTerminalToDisplaySize();

    /// What the prompt asking requestPermissionForExtension() says: which extension asks, from which module,
    /// and every command its manifest declares, made safe to show. Empty while nothing is asked, and always
    /// in a build without extensions.
    /// @return The prompt's text.
    [[nodiscard]] Q_INVOKABLE QString pendingExtensionPermissionText() const;

    /// Answers the extension prompt: Allow is remembered (in the grants file) for that extension's manifest.
    Q_INVOKABLE void allowPendingExtension();

    /// Answers the extension prompt: Deny is remembered too.
    Q_INVOKABLE void denyPendingExtension();

    /// Closes the extension prompt unanswered: nothing is remembered, and a new pane asks again.
    Q_INVOKABLE void dismissPendingExtension();

    // The extension members below are declared, and the class laid out, the same with and without
    // CONTOUR_EXTENSIONS: translation units that see no such definition (ContourTerminalDisplay's) include
    // this header too. Only a build with extensions defines and calls them.

    /// Asks the user whether an extension may run its declared commands. Callable from any thread -- it is
    /// what an extension's lane calls -- and never waits: the question is posted to the GUI thread, where
    /// requestPermissionForExtension() is emitted once it is the one to show.
    /// @param hostId Which installed extension asks.
    /// @param manifest What it declares.
    void requestExtensionPermission(std::string_view hostId, vtextension::Manifest const& manifest);

    /// Asks the user whether an extension a daemon runs for this pane may run its declared commands: through
    /// the prompt this pane's own extensions ask through, its answer recorded in @p answers -- which sends it
    /// to the daemon. GUI thread.
    /// @param request The question, as the daemon asked it.
    /// @param answers Where its answer goes; it must outlive the session.
    void askForHostedExtension(vtextension::ExtensionConsentRequest request,
                               vtextension::IGrantStore& answers);

    /// @return The pane's status-line extensions, empty while the profile declares none; null only in a build
    ///         without extensions.
    [[nodiscard]] vtextension::SessionExtensions const* statusLineExtensions() const noexcept;

    /// @return Whether any status-line extension runs in the pane; false in a build without extensions.
    [[nodiscard]] bool runsStatusLineExtensions() const noexcept;

    /// @return Whether the pane polls which program is in its foreground: only while one of its status-line
    ///         extensions follows it. GUI thread.
    [[nodiscard]] bool pollsForegroundProcess() const noexcept;

    void updateColorPreference(vtbackend::ColorPreference preference);

    /// Applies the OS motion preference to the running terminal: under Reduce Motion the cursor glide and
    /// the screen transition take no time, otherwise they take what the profile configures. The
    /// configuration itself is left as it is.
    /// @param motion The OS motion preference.
    void applyMotionPreference(config::MotionPreference motion);

    // vtbackend::Events
    //
    void requestCaptureBuffer(vtbackend::LineCount lines, bool logical) override;
    void bell() override;
    void bufferChanged(vtbackend::ScreenType) override;
    void renderBufferUpdated() override;
    void extensionFramePublished() override;
    void screenUpdated() override;
    void pageResized() override;

    /// New output brings a scrolled-back view down to it, except where the user parked the view on a search
    /// match: with the find bar open, or after stepping to one. Answered from atomics, as the terminal asks
    /// under its lock on the parser thread.
    [[nodiscard]] vtbackend::OutputFollowing outputFollowing() const noexcept override;

    vtbackend::FontDef getFontDef() override;
    void setFontDef(vtbackend::FontDef const& fontDef) override;
    void copyToClipboard(std::string_view data) override;
    void setPointerShape(std::string_view cssName) override;
    void openDocument(std::string_view /*fileOrUrl*/) override;
    /// Opens a resolved link: a URL or a template's expansion as a hyperlink, a path through openLocalPath().
    /// Ctrl+click, the context menu's pinned link and hint mode's Open all end here.
    /// @param link The link; a path only once confirmed.
    void openLink(vtbackend::LinkTarget const& link) override;
    /// Opens what the follow gesture finds at @p position (vtbackend::Terminal::lookUpLinkAt()), for a caller
    /// with a cell of its own: the vi cursor's (`gx`). A path the probe has not answered is looked for on the
    /// probe thread, and opened back on the GUI thread once found; one printed on another machine is
    /// refused, and the refusal logged.
    /// @param position A cell of the page on display, in grid coordinates.
    void followLinkAt(vtbackend::CellLocation position) override;
    void inspect() override;
    void notify(std::string_view title, std::string_view content) override;
    void showDesktopNotification(vtbackend::DesktopNotification const& notification) override;
    void discardDesktopNotification(std::string_view identifier) override;
    void focusTerminalWindow() override;
    void onClosed() override;

    /// The PTY device closed. Unless this session asked for it (terminate()), the process exited by
    /// itself, so its working and blocked program-status records go (Terminal::markProcessExited()).
    /// Called by the exit watcher on its own thread, strictly before it posts onClosed().
    void deviceClosed();

    void pasteFromClipboard(unsigned count, bool strip) override;
    void onSelectionCompleted() override;
    void requestWindowResize(vtbackend::LineCount, vtbackend::ColumnCount) override;
    void requestWindowResize(vtbackend::Width, vtbackend::Height) override;
    void setWindowTitle(std::string_view title) override;
    void setTabName(std::string_view name) override;
    void setWindowFrameColor(vtbackend::RGBColor color) override;
    void resetWindowFrameColor() override;
    /// Runs on whichever thread holds the terminal lock (the parser thread, or the GUI thread for a
    /// re-seed); posts the change to the manager on the GUI thread either way.
    void defaultBackgroundColorChanged(vtbackend::RGBColor color) override;

    /// The terminal's current default background, OSC 11 included. Takes the terminal lock, so it
    /// must not be called while holding it.
    /// @return The live default background.
    [[nodiscard]] vtbackend::RGBColor liveBackground() const;
    void progressChanged(vtbackend::Progress progress) override;
    /// OSC 7501: posted to the GUI thread as progressChanged() is, where the tab strip reads the records
    /// back.
    void programStatusChanged(vtbackend::ProgramStatusChange const& change) override;

    /// A shell command block finished (OSC 133;D, or an OSC 3008 end while OSC 3008 owns the marks).
    ///
    /// Raised on the parser thread with Terminal::_stateMutex held, exactly like progressChanged():
    /// this only copies the summary into the block events the GUI thread is to handle, reading no
    /// terminal state.
    /// @see handlePendingBlockEvents
    /// @param summary The finished block, as a value copy.
    void commandBlockFinished(vtbackend::CommandBlockSummary const& summary) override;

    /// Has the block tooltip re-read the block whose record changed, should it be the hovered one: the
    /// tooltip reads a block as the pointer enters it, and would go on saying what the record said then --
    /// "Running" after the command finished, or no signal after OSC 3008 named one -- under a pointer that
    /// stays. A command starting is such a change too, and the one a `clear_on: next` finish notification
    /// waits for.
    /// Gathered, as commandBlockFinished() is, for the GUI thread to handle.
    /// @see handlePendingBlockEvents
    /// @see withdrawFinishNoticeIfNextCommandStarted
    /// @param change The block whose record changed, and the newest block that ran a command.
    void commandBlockChanged(vtbackend::CommandBlockChange const& change) override;
    void setTerminalProfile(std::string const& configProfileName) override;
    void discardImage(vtbackend::Image const&) override;
    void inputModeChanged(vtbackend::ViMode mode) override;
    /// Asks the session's host to place or take off the user mark the user toggled on this mirror's row
    /// (SetUserMark, ClearUserMark), on the GUI thread: the request is raised under the terminal lock, and
    /// asking reaches the remote end, which takes the mirror's lock to name the row. Where the session turns
    /// out to run here after all, the mark is placed on this terminal.
    /// @param request The row, by this terminal's stable id, and what to do to its line.
    void userMarkRequested(vtbackend::UserMarkRequest const& request) override;
    void searchPromptRequested() override;
    void searchCleared() override;
    void updateHighlights() override;
    void playSound(vtbackend::Sequence::Parameters const& params) override;
    void requestShowHostWritableStatusLine() override;
    void cursorPositionChanged() override;

    bool isClosed() const noexcept { return _onClosedHandled; }

    // Input Events
    using Timestamp = std::chrono::steady_clock::time_point;
    void sendKeyEvent(vtbackend::Key key,
                      vtbackend::KeyboardModifiers modifiers,
                      vtbackend::KeyboardEventType eventType,
                      Timestamp now);
    void sendCharEvent(char32_t value,
                       vtbackend::KeyIdentity keyIdentity,
                       vtbackend::KeyboardModifiers modifiers,
                       vtbackend::KeyboardEventType eventType,
                       Timestamp now);

    void sendMousePressEvent(vtbackend::Modifiers modifiers,
                             vtbackend::MouseButton button,
                             vtbackend::CellLocation pos,
                             vtbackend::PixelCoordinate pixelPosition);
    void sendMouseMoveEvent(vtbackend::Modifiers modifiers,
                            vtbackend::CellLocation pos,
                            vtbackend::PixelCoordinate pixelPosition);

    /// Reports where the pointer is over the gutter, or that it is not over it at all.
    ///
    /// Consulted BEFORE the ordinary move path and, when it claims the event, instead of it: the
    /// gutter is not part of the grid, so a hover there must not reach the mouse protocol or hover a
    /// hyperlink at the column-0 cell it would otherwise be clamped onto. Only the block column lights
    /// a fold; beside the timestamps or the line numbers the pointer is the gutter's all the same.
    ///
    /// Not consulted for a drag the grid's own press began (@see pressOwner): that one stays the grid's
    /// over the gutter, as it does past the page's right edge, so a selection keeps extending.
    ///
    /// @param hit Where over the gutter the pointer is, or nullopt when it is not over the gutter.
    /// @return Whether the gutter consumed the event.
    ConsumedByGutter sendGutterHoverEvent(std::optional<GutterHit> hit);

    /// Acts on the press that landed on the gutter: a left one does what the block column offers beside
    /// that row (vtbackend::Terminal::blockColumnPressAt) -- toggles its fold, or opens the menu of the block
    /// whose status mark it is -- and any other button over a block runs its binding for that block, a right
    /// press opening that block's context menu.
    ///
    /// The press half of the gutter's click handshake, and the only place that arms it: a press the
    /// child never saw must be followed by a release it never sees either, and leaving that to the
    /// caller means a third event path re-deriving the rule (@see sendOwedRelease).
    ///
    /// Every left press on the gutter is consumed, one with nothing to act on included; another button is
    /// the application's anywhere but over a block on the block column.
    ///
    /// @param hit Where over the gutter the pointer is, or nullopt when it is not over the gutter.
    /// @param button The button that went down.
    /// @param modifiers The modifiers held, which pick another button's binding.
    /// @return Whether the gutter consumed the event.
    ConsumedByGutter sendGutterPressEvent(std::optional<GutterHit> hit,
                                          vtbackend::MouseButton button,
                                          vtbackend::Modifiers modifiers);

    /// Acts on the press that landed on the sticky header, for the header's command block.
    ///
    /// Consulted after the gutter and before the grid, as the gutter's press is: the header covers row 0,
    /// so a press there must not start a selection or reach the mouse protocol at a row the user cannot
    /// see. Every button is consumed while a header shows. The left one jumps to the command; a press on the
    /// evicted placeholder jumps nowhere, the command's row being gone. Any other button does what its
    /// binding does on the block's own rows -- a middle press pastes, a right press opens the block's context
    /// menu -- even while an application tracks the mouse, which never hears of the press.
    ///
    /// @param mainPageRow The main-page row under the pointer, measured WITHOUT the smooth-scroll shift
    ///                    (the header is pinned, not scrolled), or nullopt when off the page.
    /// @param button The button that went down.
    /// @param modifiers The modifiers held, which pick the button's binding.
    /// @return Whether the header consumed the event.
    [[nodiscard]] ConsumedByStickyHeader sendStickyHeaderPressEvent(
        std::optional<vtbackend::LineOffset> mainPageRow,
        vtbackend::MouseButton button,
        vtbackend::Modifiers modifiers);

    /// Swallows a release owed to whoever took its press -- the gutter or the sticky header.
    ///
    /// Owed for the button that was pressed only: another button's release, half of a chord, goes the
    /// normal way.
    /// @param button The button that went up.
    /// @return Who the release was owed to; PressOwner::None when it is the grid's.
    [[nodiscard]] PressOwner sendOwedRelease(vtbackend::MouseButton button);

    /// Who took the press whose release is still owed, if anyone did.
    /// @return PressOwner::None while no button is down, or while the one that is belongs to the grid.
    [[nodiscard]] PressOwner pressOwner() const noexcept { return _owedRelease.owner; }

    /// Forgets a release still owed: the pointer grab ended without one -- a popup, such as the context menu
    /// a right press opens, took the button's release -- so none is coming.
    void dropOwedRelease();

    /// Takes a pointer motion over the sticky header, while one shows.
    ///
    /// Consulted after the gutter and before the grid, as the press is. The header is drawn over row 0, so a
    /// motion there must not hover a link on the hidden row, show the grid's I-beam, or reach an application
    /// tracking the mouse: the pointer is off the grid, and shows the arrow. Off the header's band the motion
    /// is the grid's again.
    ///
    /// @param mainPageRow The main-page row under the pointer, measured WITHOUT the smooth-scroll shift
    ///                    (the header is pinned, not scrolled), or nullopt when off the page.
    /// @return Whether the header consumed the event.
    [[nodiscard]] ConsumedByStickyHeader sendStickyHeaderHoverEvent(
        std::optional<vtbackend::LineOffset> mainPageRow);
    void sendMouseReleaseEvent(vtbackend::Modifiers modifiers,
                               vtbackend::MouseButton button,
                               vtbackend::PixelCoordinate pixelPosition);

    /// Scrolls the viewport and extends the active selection during auto-scroll.
    ///
    /// @param direction  Negative = scroll up (into history), positive = scroll down.
    /// @param lineCount  Number of lines to scroll per tick.
    void performAutoScroll(int direction, vtbackend::LineCount lineCount);

    void sendFocusInEvent();
    void sendFocusOutEvent();

    /// The user is looking at this pane: it holds focus, in a window that is shown. A `clear_on: focus`
    /// finish notification has then done its job. Said on a focus-in, and by the session manager when
    /// the window comes back to the user without focus moving at all.
    void paneSeen();

    // Actions
    bool operator()(actions::CancelSelection);
    bool operator()(actions::ChangeProfile const&);
    bool operator()(actions::ClearHistoryAndReset);
    bool operator()(actions::ClearToPrompt);
    bool operator()(actions::CopyPreviousMarkRange);
    bool operator()(actions::CopySelection);
    bool operator()(actions::CreateDebugDump);
    bool operator()(actions::CreateSelection const&);
    bool operator()(actions::DecreaseFontSize);
    bool operator()(actions::DecreaseOpacity);
    bool operator()(actions::FollowHyperlink const& action);
    bool operator()(actions::HintMode const&);
    bool operator()(actions::FocusNextSearchMatch);
    bool operator()(actions::FocusPreviousSearchMatch);
    bool operator()(actions::IncreaseFontSize);
    bool operator()(actions::IncreaseOpacity);
    bool operator()(actions::NewTerminal const&);
    bool operator()(actions::NoSearchHighlight);
    bool operator()(actions::OpenCommandPalette);
    bool operator()(actions::OpenConfiguration);
    bool operator()(actions::OpenFileManager);
    bool operator()(actions::OpenSelection);
    bool operator()(actions::PasteClipboard);
    bool operator()(actions::PasteSelection);
    bool operator()(actions::Quit);
    bool operator()(actions::ReloadConfig const&);
    bool operator()(actions::ResetConfig);
    bool operator()(actions::ResetFontSize);
    bool operator()(actions::ScreenshotVT);
    bool operator()(actions::CopyScreenshot);
    bool operator()(actions::SaveScreenshot);
    bool operator()(actions::CollapseAllFolds);
    bool operator()(actions::CollapseLastFold);
    bool operator()(actions::ExpandAllFolds);
    bool operator()(actions::ScrollDown);
    bool operator()(actions::ScrollMarkDown);
    bool operator()(actions::ScrollMarkUp);
    bool operator()(actions::ScrollOneDown);
    bool operator()(actions::ScrollOneUp);
    bool operator()(actions::ScrollPageDown);
    bool operator()(actions::ScrollPageUp);
    bool operator()(actions::ScrollToBottom);
    bool operator()(actions::ScrollToTop);
    bool operator()(actions::ToggleFold);
    bool operator()(actions::ToggleFoldAt);
    bool operator()(actions::ToggleLastFold);
    bool operator()(actions::ScrollUp);
    bool operator()(actions::SearchReverse);
    bool operator()(actions::SendChars const& event);
    bool operator()(actions::ToggleAllKeyMaps);
    bool operator()(actions::ToggleFullscreen);
    bool operator()(actions::ToggleInputMethodHandling);
    bool operator()(actions::ToggleInputProtection);
    bool operator()(actions::ToggleStatusLine);
    bool operator()(actions::ToggleTitleBar);
    bool operator()(actions::TraceBreakAtEmptyQueue);
    bool operator()(actions::TraceEnter);
    bool operator()(actions::TraceLeave);
    bool operator()(actions::TraceStep);
    bool operator()(actions::ViNormalMode);
    bool operator()(actions::WriteScreen const& event);
    bool operator()(actions::CreateNewTab const&);
    bool operator()(actions::CloseTab);
    bool operator()(actions::CloseAllTabs);
    bool operator()(actions::CloseOtherTabs const& action);
    bool operator()(actions::CloseTabsToRight const& action);
    bool operator()(actions::MoveTabToNewWindow const& action);
    bool operator()(actions::ShowAbout);
    bool operator()(actions::SpeakSelection);
    bool operator()(actions::StopSpeaking);
    bool operator()(actions::SetTabBarVisibility);
    bool operator()(actions::SetTabBarPosition);
    bool operator()(actions::MoveTabTo);
    bool operator()(actions::MoveTabToLeft);
    bool operator()(actions::MoveTabToRight);
    bool operator()(actions::SwitchToTab const& event);
    bool operator()(actions::SwitchToPreviousTab);
    bool operator()(actions::SwitchToTabLeft);
    bool operator()(actions::SwitchToTabRight);
    bool operator()(actions::SetTabTitle);
    bool operator()(actions::SetTabColor const& action);
    bool operator()(actions::ResetTabColor);
    bool operator()(actions::SplitVertical);
    bool operator()(actions::SplitHorizontal);
    bool operator()(actions::ClosePane);
    bool operator()(actions::FocusPaneLeft);
    bool operator()(actions::FocusPaneRight);
    bool operator()(actions::FocusPaneUp);
    bool operator()(actions::FocusPaneDown);
    bool operator()(actions::SwapPaneLeft);
    bool operator()(actions::SwapPaneRight);
    bool operator()(actions::SwapPaneUp);
    bool operator()(actions::SwapPaneDown);
    bool operator()(actions::MovePaneLeft);
    bool operator()(actions::MovePaneRight);
    bool operator()(actions::MovePaneUp);
    bool operator()(actions::MovePaneDown);
    bool operator()(actions::ToggleSplitOrientation);
    bool operator()(actions::TogglePaneZoom);
    bool operator()(actions::ResizePane const& action);
    bool operator()(actions::LaunchLayout const& event);
    bool operator()(actions::SaveLayout const& event);
    bool operator()(actions::AttachToHost const& event);
    bool operator()(actions::OpenContextMenu);
    bool operator()(actions::SelectAll);
    bool operator()(actions::SoftReset);
    bool operator()(actions::CopyLastCommandPrompt);
    bool operator()(actions::CopyLastCommandOutput);
    bool operator()(actions::CopyLastCommandBlock);
    bool operator()(actions::CopyHyperlink const& action);
    bool operator()(actions::SelectCommandBlock const& action);
    bool operator()(actions::CopyCommandBlock const& action);
    bool operator()(actions::CopyCommandLine const& action);
    bool operator()(actions::RerunCommand const& action);
    bool operator()(actions::OpenCommandOutput const& action);

    /// The world as this pane's context menu needs to see it, snapshotted under a single terminal lock.
    ///
    /// The ONLY place that touches the ambient clipboard and the live terminal on the menu's behalf.
    /// Everything downstream (buildContextMenu) is a pure function of the plain struct it returns, which
    /// is what lets every row and every enable/hide rule be decided headlessly in ContextMenu_test.
    ///
    /// @return The snapshot, with `hasSplits` left for the window to fill in (a session does not know
    ///         whether its tab holds siblings).
    [[nodiscard]] command::ContextMenuState contextMenuState();

    /// Runs @p action against this session — the same dispatch a key binding takes.
    ///
    /// Public because a key chord is no longer the only way an action is asked for: the command
    /// palette runs one the user PICKED. This widens the surface only nominally — the per-action
    /// `operator()` overloads above have always been public, so any caller could already reach every
    /// one of them; this only spares them re-implementing the visit.
    ///
    /// @param action The action to run.
    /// @return Whether the action applied. An action may decline — FollowHyperlink with no link under
    ///         the cursor returns false, which is what lets a key binding fall through to the terminal. A
    ///         path still being looked for counts as followed, as it opens once found: its key stays
    ///         claimed even should the probe then find no file.
    bool executeAction(actions::Action const& action);

    void scheduleRedraw();

    ContourGuiApp& app() noexcept { return _app; }

    std::chrono::steady_clock::time_point startTime() const noexcept { return _startTime; }

    float uptime() const noexcept
    {
        using namespace std::chrono;
        auto const now = steady_clock::now();
        auto const uptimeMsecs = duration_cast<milliseconds>(now - _startTime).count();
        auto const uptimeSecs = static_cast<float>(uptimeMsecs) / 1000.0f;
        return uptimeSecs;
    }

    /// The permission the active profile configures for @p role.
    ///
    /// The single place that pairs a role with its configuration field, so a role's setting is named
    /// once rather than at each call site. @return Ask for a role the configuration has no field for.
    [[nodiscard]] config::Permission configuredPermissionFor(GuardedRole role) const;

    /// Hands @p role's permission decision to the GUI thread, where every guarded executor belongs.
    /// Does nothing for a display-less session, which has no queue to hand it to.
    void postPermissionRequest(GuardedRole role);

    void requestPermission(config::Permission allowedByConfig, GuardedRole role);
    void executeRole(GuardedRole role, bool allow, bool remember);

    /// Sends @p text to the application as one paste, repeated @p count times.
    void sendPasteRepeatedly(std::string const& text, unsigned count);

    /// Derives the terminal's image canvas ceiling from the monitor the display currently sits on.
    ///
    /// The single writer of that ceiling; call it whenever the display attaches or changes monitor.
    /// The cap is the monitor rather than the window, so an ordinary resize needs no update. No-op
    /// while no window/screen is available — the caller re-runs once one is.
    void updateImageCanvasCeiling();

  signals:
    void hyperlinkHoverChanged();
    /// The hovered command block changed, or the pointer left it: re-read commandBlockTooltip().
    void commandBlockHoverChanged();
    /// The find bar should open and take focus. Raised by searchPromptRequested(), the
    /// vtbackend::Terminal::Events hook -- which cannot itself be the signal, being an override.
    void searchBarRequested();
    /// The pattern, its tally, or the case policy changed.
    void searchStateChanged();
    /// isReconnecting() changed.
    void reconnectingChanged();
    void sessionClosed(TerminalSession&);
    void profileNameChanged(QString newValue);
    void lineCountChanged(int newValue);
    void columnsCountChanged(int newValue);
    void historyLineCountChanged(int newValue);
    void scrollOffsetChanged(int newValue);
    /// The scrollbarMarks and scrollbarMarkColors properties changed.
    void scrollbarMarksChanged();
    void titleChanged(QString const& value);
    void pathToBackgroundChanged();
    void opacityBackgroundChanged();
    void isImageBackgroundChanged();
    void isBlurBackgroundChanged();
    void backgroundColorChanged();
    void isScrollbarRightChanged();
    void isScrollbarVisibleChanged();
    void opacityChanged();
    void dimUnfocusedChanged();
    void onBell(float volume);
    void onAlert();
    void requestPermissionForFontChange();
    void requestPermissionForPasteLargeFile();
    void requestPermissionForBufferCapture();
    void requestPermissionForShowHostWritableStatusLine();
    void requestPermissionForRerunCommand();
    /// A status-line extension asks to run its commands; pendingExtensionPermissionText() says what to show.
    void requestPermissionForExtension();
    void showNotification(QString const& title, QString const& content);
    void fontSizeChanged();

    // Tab handling signals
    void createNewTab();
    void closeTab();
    void switchToPreviousTab();
    void switchToTabLeft();
    void switchToTabRight();
    void switchToTab(int position);

  public:
    /// Applies a configuration that was just reloaded -- read once, by
    /// TerminalSessionManager::reloadAllSessions, for every session -- under @p profileName.
    /// A configuration that no longer has the profile is refused and logged; the session keeps
    /// the configuration it has.
    /// @param config The reloaded configuration.
    /// @param profileName The profile to run under: the session's own, unless it is switching.
    void applyReloadedConfig(config::Config const& config, std::string const& profileName);

  public slots:
    void onHighlightUpdate();
    void configureDisplay();

  private:
    // helpers

    /// Runs @p action against the terminal under the lock, republishes the scrollable count, and asks for the
    /// scrollbar's ticks to be rebuilt.
    ///
    /// The one path every action takes that moves rows into or out of the scrollable range -- the folding
    /// actions, ClearToPrompt, and the jumps that open a fold -- so that adding one is a handler rather than
    /// a handler plus a remembered step: an action that moved rows and did not say so leaves the scrollbar
    /// sized, and its ticks spread, for the range before it.
    /// Whether an action may fold at all with folding.enabled off is the terminal's to decide
    /// (vtbackend::FoldingAvailability), as it is for the vi `z` keys.
    ///
    /// @param action What to do with the terminal; its result is the action's result.
    /// @return What @p action returned.
    bool withScrollableChange(std::invocable<vtbackend::Terminal&> auto&& action);

    /// Hands @p action to the session's remote end when the terminal it changes is hosted there.
    ///
    /// The first step of every handler of an action that changes engine state (the table in
    /// HostedOperation.hpp): a pane attached to a daemon mirrors a terminal the daemon runs, so changing
    /// the local one would be undone by the next update. A refusal is told to the user.
    /// @param action The action, one the table names.
    /// @return RunHere when the handler runs the action itself; otherwise what became of it.
    template <typename Action>
    [[nodiscard]] RemoteDispatch dispatchRemotely(Action const& action);

    /// Asks where @p request runs, which sends it to the session's remote end when it runs there, and tells
    /// the user when it runs nowhere.
    /// @param request The operation.
    /// @return Where it runs: RunHere leaves it to the caller, RunOnHost has sent it, and Refused and
    ///         Unreachable did nothing but say so.
    [[nodiscard]] HostedOperationRoute routeHostedOperation(HostedOperationRequest const& request);

    /// Queues @p change to run on the manager on the GUI thread, from a callback the parser thread raises
    /// under the terminal lock.
    ///
    /// Queued on the MANAGER, not on this session, and handed nothing of the session's: a session is not
    /// deleted with its manager, so a call queued on the session could still run once the manager was gone.
    /// Qt drops a call queued on a destroyed manager, and the manager's own lookups by session id answer a
    /// session closed in the meantime.
    /// @param change Invoked with the manager; it must capture what it needs by value.
    template <typename Change>
    void postToManager(Change change);

    bool reloadConfig(config::Config newConfig, std::string const& profileName);
    int executeAllActions(std::vector<actions::Action> const& actions);
    /// Opens a new window: a new process or an in-process window, or -- attached -- one the daemon hosts.
    /// @param requestedProfile The profile the action named; std::nullopt runs this session's own locally,
    ///        and the one this client named in its hello on a daemon.
    void spawnNewTerminal(std::optional<std::string> const& requestedProfile);

    /// Writes @p lines into this session's own screen, highlighted the way onClosed()'s early-exit
    /// notice is, so the user reads them where the shell's output would have been.
    void writeNotice(std::span<std::string const> lines);

    /// Reports a PTY device that failed to start, and leaves the pane in the state the early-exit
    /// notice already defines: the reason on screen, the device closed, and the pane pruned by the
    /// next key press (see sendKeyEvent) or by closing the tab (see terminate).
    void reportDeviceStartFailure(vtpty::StartFailure const& failure);

    /// Ends a gutter hover, if one is in effect.
    ///
    /// The one place _gutterHovered is cleared, so the flag and the terminal's own hover line cannot
    /// disagree -- clearing one without the other left a hover the next motion had to undo.
    void clearGutterHover();

    /// Withdraws everything the pointer's position decided -- the hyperlink tooltip, the gutter's hover and
    /// the block tooltip -- for a pointer that no longer stands where it did: it left, or the content moved
    /// under it. The next move decides them afresh. GUI thread only: QML reads that state without a lock.
    void clearHovers();

    /// The pointer stands on no grid cell -- over the gutter, over the sticky header, or outside the terminal
    /// -- so nothing there is hovered: the hyperlink's tooltip goes, the terminal stops drawing the link as
    /// hovered, and the next move onto the grid decides the pointer's shape afresh, on the cell it left from
    /// too. The one place that says so, for every way off the grid.
    void leaveGrid();

    /// Points the block-column tooltip at @p block, entered at screen row @p screenRow, or withdraws it.
    /// Announces only a change of BLOCK: tracing one block's rows keeps the tooltip where it was entered.
    /// @param block The block under the pointer, or nullopt when there is none to describe.
    /// @param screenRow The screen row the pointer entered @p block at, a top status line counted.
    void updateCommandBlockHover(std::optional<vtbackend::CommandBlockId> block,
                                 vtbackend::LineOffset screenRow);

    /// Publishes @p scrollable as the scrollbar's travel, announcing it when it moved. GUI thread only.
    ///
    /// The historyLineCount property reads what this stored rather than computing a count of its
    /// own, because computing one goes through the fold projection -- a lazily built cache the
    /// render pass clears and refills under the terminal lock, so a rebuild without that lock, on any
    /// thread, races it. Taking that lock in the property instead is what cannot be done: a scroll
    /// performed under it emits scrollOffsetChanged, whose QML handler reads this very property on the
    /// same thread, and the lock is not recursive. So every caller computes the count under the lock
    /// and hands it here after releasing it -- the binding this wakes reaches back into the session.
    /// Output on the parser thread is no caller: it asks refreshScrollbarMarks() to publish instead.
    ///
    /// @param scrollable The scrollable line count, computed by the caller under the terminal lock.
    void announceScrollableLineCount(vtbackend::LineCount scrollable);

    /// Arranges for refreshScrollbarMarks() to run on the GUI thread within a frame.
    ///
    /// Callable from either thread, the terminal lock held or not: it neither locks nor computes, only
    /// posts -- and at most one post is in flight, so a burst of output costs one atomic per PTY read.
    void requestScrollbarMarksRefresh();

    /// Re-announces every Q_PROPERTY whose value is derived from the profile, so the QML bindings that
    /// read them re-evaluate against the profile that was just swapped in. Call after every assignment
    /// to _profile; without it a property keeps reporting the OLD profile's value until the app is
    /// restarted (the signal is declared, simply never emitted), which is what made a reload of
    /// scrollbar.position — and still, background_image and friends — a no-op.
    void emitProfileDerivedPropertiesChanged();

    /// Whether activating a profile should also resize the window to the profile's configured
    /// terminal_size. Data-driven so the resize is tied to the *intent* of the activation, not to
    /// the activation itself: an explicit profile switch (a keybinding / OSC request) should fit the
    /// new profile's grid, but a passive config-file reload must preserve the user's live window
    /// size. See activateProfile().
    enum class ProfileWindowSizePolicy : uint8_t
    {
        Preserve, ///< Keep the current window size (config-file reload).
        Apply,    ///< Resize the window to the profile's terminal_size (explicit profile switch).
    };

    /// Activates the named profile: applies it to the terminal and, per @p windowSizePolicy,
    /// optionally resizes the window to the profile's configured terminal_size.
    /// @param newProfileName The profile to activate (tolerant of a removed profile, keeps default).
    /// @param windowSizePolicy Whether to resize the window to the profile's terminal_size.
    void activateProfile(std::string const& newProfileName,
                         ProfileWindowSizePolicy windowSizePolicy = ProfileWindowSizePolicy::Preserve);
    /// Reloads the configuration file for every session (TerminalSessionManager::reloadAllSessions),
    /// and makes sure this one runs under @p profileName afterwards.
    /// @return Why the file could not be read; nothing changed then.
    [[nodiscard]] std::expected<void, std::string> reloadAllConfigs(std::string const& profileName);

    /// After the manager reloaded every session it hosts, makes sure this one runs under @p profileName
    /// in the reloaded configuration: it is not reached when the manager does not host it, or when it
    /// is to switch profile.
    /// @param profileName The profile to run under from now on.
    void followReload(std::string const& profileName);
    bool resetConfig();
    void followHyperlink(vtbackend::HyperlinkInfo const& hyperlink);

    /// What the follow gesture finds at the pointer's cell (vtbackend::Terminal::lookUpLinkAt()), read in the
    /// same hold of the terminal lock as the cell, and acted on as followLinkAt() acts.
    /// @return What became of it; NothingThere off the grid.
    [[nodiscard]] LinkFollow followLinkUnderPointer();

    /// What a follow gesture found at a cell; defined in the .cpp.
    struct FoundLink;

    /// What lies at @p position, and what a path there would be probed with. The caller holds the terminal
    /// lock.
    /// @param position A cell of the page on display, in grid coordinates.
    /// @return What was found.
    [[nodiscard]] FoundLink findLinkAt(vtbackend::CellLocation position);

    /// Acts on what a follow gesture found, the terminal lock released: opens it, probes a path, or logs a
    /// refusal.
    /// @param found What findLinkAt() found.
    /// @return What became of it.
    [[nodiscard]] LinkFollow follow(FoundLink found);

    /// Opens a confirmed local path (§7.3): a directory in the file manager; a file with the profile's
    /// file_opener at its location -- detached, or in a split or a tab whose working directory is the one the
    /// path resolved against -- or, without one, with the application it is associated with.
    /// @param link A path link, its target the absolute place.
    void openLocalPath(vtbackend::LinkTarget const& link);

    /// Hands @p url to the desktop's handler, logging why nothing happened when it will not go.
    ///
    /// One place rather than one per action: five actions open something, they all report the same
    /// way, and openUrl() only DISPATCHES -- so what is logged here is a request that was rejected
    /// outright, never a handler that failed later. @see platform::ExternalLauncher::openUrl.
    ///
    /// @param url     The resource to open.
    /// @param what    What it is, for the log line ("document", "folder", "hyperlink", ...).
    /// @param subject How to name it -- the user's own spelling, which is not always @p url's.
    void openExternally(QUrl const& url, std::string_view what, std::string_view subject);

    /// Copies one part of a command block as plain text; refuses (and leaves the clipboard alone) when
    /// there is nothing to copy.
    /// @param target Which block.
    /// @param part Which part of it.
    /// @param block A pinned block (zero: resolve @p target).
    /// @return Nothing once copied; otherwise why nothing was -- no such block, or an empty part.
    [[nodiscard]] std::expected<void, vtbackend::CommandBlockActionError> copyCommandBlock(
        vtbackend::CommandBlockTarget target,
        vtbackend::CommandBlockPart part,
        vtbackend::CommandBlockId block);

    /// Copies text taken from a command block into the clipboard, unless there is none.
    /// @param text The text, or why there is none.
    /// @return Nothing once copied; otherwise why nothing was. Empty text is an empty part: copying it
    ///         would only destroy what the clipboard held.
    [[nodiscard]] std::expected<void, vtbackend::CommandBlockActionError> copyBlockText(
        std::expected<std::string, vtbackend::CommandBlockActionError> text);

    /// Logs why an opener showed nothing and tells the user through showNotification().
    /// @param what What it was to open.
    /// @param reason What went wrong, lower-case and without a trailing period.
    void reportOpenFailure(OpenFailure what, std::string_view reason);

    void setFontSize(text::FontSize size);

    /// Posts a refresh of the indicator status-line tab info (tab names) to the GUI thread.
    ///
    /// Called when the window title or tab name changes at runtime. Must not refresh inline: these
    /// notifications arrive on the parser thread while the terminal state mutex is held, and the
    /// refresh re-enters that non-recursive mutex (see scheduleRedraw()), so it is deferred to the GUI
    /// thread via the display's event loop.
    void refreshGuiTabInfoForStatusLine();

    /// Shows an application's notification (OSC 777, OSC 9 or OSC 99) if its occasion wants it from where
    /// this session is now, and wires the reports it asked for. GUI thread.
    /// @param notification The notification, with the occasion it is decided by.
    void raiseDesktopNotification(vtbackend::DesktopNotification const& notification);

    /// Decides on, and delivers, the finish notification for @p summary. GUI thread.
    /// @param summary The finished block.
    void handleCommandBlockFinished(vtbackend::CommandBlockSummary const& summary);

    /// Handles every block event gathered since the last call, in the order they were raised: each finish,
    /// then what the changes ask -- the hovered block's tooltip re-read, a `clear_on: next` notice
    /// withdrawn. GUI thread; takes no lock but the one guarding the gathered events.
    void handlePendingBlockEvents();

    /// Delivers @p request about @p block, remembering a raised notification for withdrawal on
    /// @p clearOn. It is raised through the session's notifier: the desktop's on Linux, elsewhere one
    /// that hands it to the hosting window's tray, as the bell always goes (a background tab has no
    /// SessionChrome to relay it).
    /// @param block The block the notification reports.
    /// @param request What to say, and how.
    /// @param clearOn What withdraws the notification.
    void deliverFinishNotice(vtbackend::CommandBlockId block,
                             platform::FinishNotificationRequest const& request,
                             config::NotifyClearOn clearOn);

    /// Withdraws this session's finish notification from the desktop, if it has one.
    void withdrawFinishNotice();

    /// Forgets this session's finish notification without withdrawing it: the desktop retired it.
    void forgetFinishNotice() noexcept;

    /// Whether this session has a finish notification out that @p trigger withdraws.
    /// @param trigger The `clear_on` event that just happened.
    /// @return True when a notification is outstanding and its `clear_on` is @p trigger.
    [[nodiscard]] bool finishNoticeClearsOn(config::NotifyClearOn trigger) const noexcept;

    /// Connects the notifier's click and close events for this session's finish notification once --
    /// every finish notification of a session carries the same identifier -- and the session's own close.
    void connectFinishNoticeEvents();

    /// Withdraws a `clear_on: next` finish notification once its block is no longer the newest that ran a
    /// command: a later command has started, or a reset forgot the block. GUI thread; takes no lock.
    /// @param lastCommand The newest record that ran a command, as a record change left the store; zero
    ///                    when none had.
    void withdrawFinishNoticeIfNextCommandStarted(vtbackend::CommandBlockId lastCommand);

    /// Hands the terminal the chord now held and, if that changes how links are presented, re-decides the
    /// pointer. GUI thread; takes the terminal lock.
    /// @param held The chord now held.
    void onHeldModifiersChanged(vtbackend::Modifiers held);

    /// Re-decides the pointer shape where the pointer rests: a held or released modifier changes it without a
    /// move. Does nothing while the pointer is off the grid. GUI thread; takes the terminal lock.
    void refreshPointerShape();

    void setDefaultCursor();
    void configureTerminal();

    /// @return What the terminal shows status-line extensions through; none without extensions.
    [[nodiscard]] std::optional<vtbackend::StatusLineExtensionServices> terminalExtensionServices() noexcept;

    /// What a pane with status-line extensions keeps for them. Defined, with the extensions, in
    /// TerminalSession.cpp.
    struct ExtensionState;

    /// Owns an ExtensionState through the deleter chosen where it is made, so that this header needs it
    /// complete nowhere: the class is the same in a translation unit without CONTOUR_EXTENSIONS.
    using ExtensionStatePtr = std::unique_ptr<ExtensionState, void (*)(ExtensionState*)>;

    /// @return What the session keeps for status-line extensions, with none running yet; null without
    ///         CONTOUR_EXTENSIONS.
    [[nodiscard]] ExtensionStatePtr createExtensionState();

    /// Rebuilds the extensions when the profile now declares others (construction, a reload, a profile
    /// switch). Returns at once: they are loaded off this thread.
    void reloadExtensions();

    /// Tells the terminal that other extensions run now. On the GUI thread, holding no lock.
    void statusLineExtensionsReplaced();

    /// @return The font the attached display draws in, for the status-line extensions. Requires a display.
    [[nodiscard]] vtbackend::StatusLineFontMetrics displayFontMetrics();

    /// Queues an extension's question, and asks it if it is the one to show. GUI thread.
    /// @param request The question.
    /// @param answers Where its answer goes.
    void offerExtensionConsent(vtextension::ExtensionConsentRequest request,
                               vtextension::IGrantStore& answers);

    /// Records the answer to the extension prompt, and asks the next question, if any.
    void answerExtensionConsent(vtextension::ConsentAnswer answer);

    /// Tells the extensions when the program in the foreground changed. Ticks only while one follows it.
    void pollForegroundProcess();

    /// Starts the foreground poll if an extension follows the foreground process, and stops it if none does.
    /// GUI thread.
    void armForegroundProcessPoll();

    /// Has the engines and modules that runners retired released a moment from now, once their lanes ran
    /// their last jobs.
    void scheduleRetiredRelease();

    void configureCursor(config::CursorConfig const& cursorConfig);

    /// Scrolls up by @p lineCount lines, using smooth pixel scrolling if enabled, otherwise line-based.
    void smoothScrollUp(vtbackend::LineCount lineCount);

    /// Scrolls down by @p lineCount lines, using smooth pixel scrolling if enabled, otherwise line-based.
    void smoothScrollDown(vtbackend::LineCount lineCount);

    /// The actions a `clicks:` mouse binding has for this press: the user's mappings first, then the
    /// built-in fallbacks. Only looks: it runs under the terminal lock, inside the claimant.
    /// @param button The pressed button.
    /// @param modifiers The chord, with the bypass modifier already removed.
    /// @param clickCount The press's place in its multi-click sequence.
    /// @return The bound actions, or nullptr -- also for a binding executeAllActions() would refuse.
    [[nodiscard]] config::ActionList const* clickCountBinding(vtbackend::MouseButton button,
                                                              vtbackend::Modifiers modifiers,
                                                              uint8_t clickCount) const;

    /// The actions of the follow gesture for this press: the ordinary binding of the left @p button, when it
    /// follows links and the cell under the pointer holds one that opens at once -- an OSC 8 link or a target
    /// known now (vtbackend::Terminal::lookUpLinkAt), not a path still to be probed. Such a press is claimed,
    /// so it starts no selection: following the link takes the focus, and with it the press's release. Only
    /// looks: it runs under the terminal lock, inside the claimant.
    /// @param button The pressed button.
    /// @param modifiers The chord, with the bypass modifier already removed (@see bindingModifiers).
    /// @return The bound actions, or nullptr -- also for a binding executeAllActions() would refuse.
    [[nodiscard]] config::ActionList const* linkFollowBinding(vtbackend::MouseButton button,
                                                              vtbackend::Modifiers modifiers) const;

    /// Whether executeAllActions() would run @p actions: key mappings are on, or the binding turns them back
    /// on, the one binding still run while they are toggled off.
    /// @param actions A binding's actions.
    /// @return True when they would run.
    [[nodiscard]] bool willRun(config::ActionList const& actions) const;

    /// The actions an ordinary press of @p button is bound to: the user's mappings first, then the built-in
    /// fallbacks, as for a press the grid declined.
    /// @param button The pressed button.
    /// @param modifiers The chord, with the bypass modifier already removed (@see bindingModifiers).
    /// @return The bound actions, or nullptr.
    [[nodiscard]] config::ActionList const* mouseBinding(vtbackend::MouseButton button,
                                                         vtbackend::Modifiers modifiers) const;

    /// @p modifiers as a mouse binding matches them: without the mouse-protocol bypass modifiers, when it
    /// holds them, since those only say the press is not the application's.
    /// @param modifiers The chord held.
    /// @return The chord to match.
    [[nodiscard]] vtbackend::Modifiers bindingModifiers(vtbackend::Modifiers modifiers) const;
    void flushInput();
    void mainLoop();

    // private data
    //
    TerminalSessionManager* _manager;
    int _id;
    vtworkspace::SessionId _modelSessionId {};
    std::chrono::steady_clock::time_point _startTime;
    config::Config _config;
    std::string _profileName;
    // The ctor's explicit profile choice, verbatim (nullopt when the session runs the app
    // default). Kept separate from _profileName, which always resolves to a concrete profile.
    std::optional<std::string> _profileOverride;
    std::optional<vtpty::Process::ExecInfo> _launchedCommand;
    SessionRole _role;
    /// Where the session's program runs: here, or on a daemon or tmux server the pane mirrors. A mirrored
    /// pane runs no status-line extension of its own -- they run where its terminal does, on the remote end.
    SessionHosting _hosting;
    /// Where the connection to the host stands, for a mirrored pane. @see setHostConnection
    vthost::client::ConnectionState _hostConnection = vthost::client::ConnectionState::Connected;
    config::TerminalProfile _profile;
    ContourGuiApp& _app;
    vtbackend::ColorPreference _currentColorPreference;
    /// This machine's identity, as the app read it once (ContourGuiApp::hostIdentity): neither half changes
    /// while the process runs, and both are asked on the parser thread and on the GUI thread under the
    /// terminal lock. A copy rather than a reference into the app, which a session may outlive at exit.
    ///
    /// The machine id is the STRONG half of the locality test an OSC 3008 working directory needs.
    /// Nothing emits a `remote` context for ssh today, so a remote host's own sequences look entirely
    /// local and its paths may well exist here too; comparing machine ids is the only thing that catches
    /// it, and systemd's shim emits one for free. The host name is what every file:// URL is measured
    /// against. @see vtbackend::LocalIdentity, vtbackend::isLocalHost
    vtbackend::HostIdentity _localIdentity;

    crispy::Point _accumulatedPixelScroll;
    crispy::Point _accumulatedAngleScroll;
    input::HorizontalWheelGesture _horizontalWheelGesture;
    HyperlinkHoverTracker _hyperlinkHover;
    /// The tooltip last worked out for a detected link, by the link's id and the index holding it, so that
    /// tracing one link along its cells reads nothing from the index (hoveredDetectedLinkTooltip()). GUI
    /// thread only. Also reset when a configuration reload may have replaced the detector, whose ids start
    /// over: a new index may reuse the old one's address.
    struct DetectedLinkTooltip
    {
        vtbackend::LinkIndex const* index = nullptr; ///< The index it was read from; none at first.
        vtbackend::LinkId link {};                   ///< The link it was worked out for; none at first.
        /// detectedLinkTooltip() of that link, elided as the tooltip shows it; empty for none.
        std::string text;
    };
    DetectedLinkTooltip _detectedLinkTooltip {};
    /// Never null: a NullAnnouncer stands in wherever there is nothing to announce through, so the call
    /// sites never have to ask whether announcing is possible.
    std::unique_ptr<platform::Announcer> _announcer = std::make_unique<platform::NullAnnouncer>();
    QString _hyperlinkTooltipText;
    QRectF _hyperlinkTooltipAnchor;

    /// The command block whose tooltip is showing, if any (@see commandBlockTooltip).
    std::optional<vtbackend::CommandBlockId> _hoveredBlock {};
    QRectF _commandBlockTooltipAnchor {};

    /// What the find bar currently shows, re-derived by refreshSearchStatus().
    SearchStatus _searchStatus;
    SearchCaseAffordance _searchCase = describeSearchCase(vtbackend::SearchCaseSensitivity::Smart);
    QString _searchPatternText;
    QString _searchSummary;
    QString _searchCaseTooltip;
    /// Coalesces re-tallying while output keeps arriving: a tally walks the whole scrollback, so it
    /// must never run once per frame. Armed by screenUpdated(), and only while the bar is open.
    QTimer _searchTallyTimer;
    /// Whether the find bar is open. Two things read it: whether a tally is worth computing at all,
    /// and whether new output may auto-scroll the viewport back to the bottom -- the bar being open
    /// means the user is reading something they went looking for. @see outputFollowing.
    /// Atomic: written on the GUI thread, read on the parser thread.
    std::atomic<bool> _isSearchBarOpen { false };
    /// Whether the viewport was last moved to show a search match rather than by new output.
    ///
    /// The bar-closed half of the same rule @c _isSearchBarOpen carries: F3/Shift+F3 are bound to the
    /// Search match mode, which is "a pattern is set" and therefore outlives the bar, so a match can be
    /// stepped to with nothing on screen to say the user is searching. Set by the two
    /// Focus*SearchMatch actions before they step, and cleared by screenUpdated() the moment the
    /// viewport is back at the bottom -- so following the output again is all it takes to un-park it.
    /// Atomic for the same reason as above.
    std::atomic<bool> _searchMatchRevealed { false };
    /// Collapses repeat re-arm posts to at most one in flight, as _cursorMovedPostPending does.
    std::atomic_flag _searchTallyPostPending = ATOMIC_FLAG_INIT;

    /// Repoints renderBufferUpdated() at @p target (null: drop notifications) under _renderNotifyMutex.
    /// @param target The surface to forward to, or null.
    void setRenderNotifyTarget(DisplaySurface* target);

    /// Guards _renderNotifyTarget, the copy of _display that renderBufferUpdated() forwards to. That
    /// callback can run on a pool thread (the status-line command cache), so it cannot read _display
    /// itself. Lock order: the cache's mutex, then this one. Held only for the forward and the stores in
    /// attachDisplay()/detachDisplay()/~TerminalSession(); a surface's renderBufferUpdated() only posts to
    /// the GUI thread and takes no terminal or cache lock, so it cannot call back into the cache.
    /// A plain pointer under a mutex, rather than an atomic, because the lock is what keeps the surface
    /// alive for the duration of the forwarded call: a detach waits for an in-flight forward.
    /// Declared above _terminal so it outlives the cache's last callback.
    std::mutex _renderNotifyMutex;
    DisplaySurface* _renderNotifyTarget = nullptr; ///< Guarded by _renderNotifyMutex.

    /// Where a job on another thread (a follow gesture's path probe) hands its result back to this session,
    /// shared with the job: ~TerminalSession() withdraws it first, so a job that answers later posts nothing.
    std::shared_ptr<platform::PostTarget> _postTarget = std::make_shared<platform::PostTarget>(*this);

    /// The pane's status-line extensions, and what it keeps for them; null only without CONTOUR_EXTENSIONS.
    /// Made eagerly, with the session (R47), because the forwarder and the sink inside it must exist when the
    /// terminal is built; what costs is lazy: the runtime, its build thread, the engine, the workers and the
    /// foreground-process timer come only once the profile declares extensions. Until then it forwards to
    /// nothing, and every event hook of the terminal stays one untaken branch. A reload that declares some
    /// starts them here.
    ///
    /// Declared after _postTarget, which its callbacks post through, and before _terminal, which borrows its
    /// forwarder and sink: ~Terminal revokes the sink (no delivery reaches a dying terminal), then this
    /// destroys the runners -- none waits for its extension -- and the sink. ~TerminalSession holds no lock
    /// while the terminal goes (it releases _renderNotifyMutex in its body), so the revoke cannot deadlock on
    /// a delivery that takes it.
    ExtensionStatePtr _extensionState;

    vtbackend::Terminal _terminal;
    /// A notice is on screen and the pane is alive only to be dismissed: the shell exited too early,
    /// the device never started, or the daemon connection went away. It is also what makes
    /// onClosed() idempotent for those exits, which arrive from two callers (see onClosed()), so it
    /// is written and read under _onClosedMutex wherever more than the GUI thread can see it.
    bool _terminatedAndWaitingForKeyPress = false;
    DisplaySurface* _display = nullptr;

    std::atomic<bool> _terminating { false };

    /// Guards against piling up cursor-moved posts (IME rectangle update + accessible caret report).
    /// Set on the terminal thread when one is queued and cleared on the GUI thread when it runs, so a
    /// cursor that moves faster than the GUI thread drains its queue coalesces into one report rather
    /// than a backlog.
    std::atomic_flag _cursorMovedPostPending = ATOMIC_FLAG_INIT;
    std::thread::id _mainLoopThreadID {};
    std::unique_ptr<std::thread> _screenUpdateThread;

    // state vars
    //

    /// The cell the mouse cursor's shape was last decided for, in main-page rows.
    ///
    /// Not the pointer's position, which is the terminal's (Terminal::currentMouseGridPosition()): a press or
    /// a wheel notch moves that onto a new cell without deciding the shape, so the next move onto that cell
    /// must still decide it. Read by sendMouseMoveEvent()'s shape decision alone; @ref _isPointerShapeStale
    /// says when the shape no longer holds even for this cell.
    vtbackend::CellLocation _pointerShapeCell {};

    /// Whether the pointer was last seen over the gutter rather than over the grid.
    ///
    /// Kept here so the common case -- pointer over the grid -- can be answered without taking
    /// the terminal lock on every single motion event, while still clearing a hover that was set.
    bool _gutterHovered = false;

    /// A press taken away from the grid, whose release is owed to the same taker (@see PressOwner).
    struct OwedRelease
    {
        PressOwner owner = PressOwner::None;                          ///< Who took the press.
        vtbackend::MouseButton button = vtbackend::MouseButton::Left; ///< The button pressed.
    };

    /// This pane's press that the gutter or the sticky header took, if any. Per SESSION rather than per
    /// process: every pane and every window shares the GUI thread, so a press in one pane's gutter
    /// followed by a release delivered to another would otherwise swallow the second pane's release.
    OwedRelease _owedRelease {};

    /// The block a context menu opened over the sticky header or the gutter describes, while the press that
    /// opens it runs; zero otherwise, when the menu describes the cell under the pointer.
    vtbackend::CommandBlockId _contextMenuBlock {};

    /// Runs @p button's binding with @p modifiers for @p block, as over the block's own rows: a context menu
    /// it opens describes @p block. The sticky header's and the gutter's buttons, other than the left one.
    /// @param button The button pressed.
    /// @param modifiers The modifiers held, which pick its binding.
    /// @param block The block the press stands for.
    void runBindingForBlock(vtbackend::MouseButton button,
                            vtbackend::Modifiers modifiers,
                            vtbackend::CommandBlockId block);

    /// Opens the context menu of @p block: what a click on its status mark in the gutter does.
    /// @param block The block.
    void openBlockMenu(vtbackend::CommandBlockId block);

    /// Runs @p action with @p block as the block a context menu it opens describes (_contextMenuBlock), and
    /// forgets it afterwards.
    /// @param block The block the sticky header's or the gutter's press stands for.
    /// @param action What the press runs.
    void withContextMenuBlock(vtbackend::CommandBlockId block, std::invocable auto&& action);

    /// Whether the mouse cursor shape must be re-decided on the next motion, even over the same cell.
    ///
    /// The move path changes the shape only when the pointer changes grid CELL, that being the only
    /// thing that can change its answer -- but the gutter sets the shape without going through
    /// it, and leaving the gutter lands the pointer back on the cell it came from. @see
    /// clearGutterHover(), which raises this.
    bool _isPointerShapeStale = false;

    /// The shape the application last asked for via `OSC 22`, or nullopt while it has asked for
    /// none. Recorded whether or not a display is attached: a session between displays -- a split
    /// hand-off, a tab whose display was released -- still has to hand the shape to whichever display
    /// attaches next, and writing it only from the lambda posted to the display dropped it entirely
    /// for the rest of the session.
    ///
    /// Atomic because it is now written on the parser thread (where `OSC 22` arrives) and read on the
    /// GUI thread in setDefaultCursor(). Caching it here rather than reading the pointer-shape stack
    /// back is still the point: that stack belongs to the parser thread.
    std::atomic<std::optional<input::MouseCursorShape>> _applicationPointerShape;
    /// The type of status display the configuration named when it was last applied; nothing before the first
    /// application. A reload or profile switch selects the type only where it changes this: a Set-Up change
    /// of the type (DEC STD 070 ch. 14 §3), which an unchanged field is not.
    std::optional<vtbackend::StatusDisplayType> _configuredStatusDisplayType;
    bool _allowKeyMappings = true;
    std::unique_ptr<platform::Audio> _audio;
    std::vector<int> _musicalNotesBuffer;

    /// The scrollable line count the historyLineCount property reports and its NOTIFY signal last
    /// carried. GUI thread only; see announceScrollableLineCount() for why the property cannot simply
    /// compute it.
    int _lastHistoryLineCount = 0;

    /// Everything the scrollbar's ticks are a function of -- the marks, as the terminal's revision of them
    /// says, and the rows they are spread over -- plus the track and what selects and colours them.
    /// Compared under the terminal lock, so a refresh that finds none of it moved neither copies the marks
    /// out nor wakes QML.
    struct ScrollbarMarksKey
    {
        uint64_t marksRevision = 0;                 ///< Terminal::scrollbarMarksRevision()
        int64_t visibleRowCount = 0;                ///< The rows the scrollbar spans
        int trackPixels = 0;                        ///< The track's height
        vtbackend::ScrollbarMarkSources sources {}; ///< The kinds drawn
        std::array<vtbackend::RGBColor, vtbackend::ScrollbarMarkKindList.size()> colors {}; ///< By kind

        [[nodiscard]] bool operator==(ScrollbarMarksKey const&) const noexcept = default;
    };

    /// What the scrollbarMarks / scrollbarMarkColors properties report. GUI thread only.
    QVariantList _scrollbarMarks;
    QVariantList _scrollbarMarkColors;

    /// The ticks _scrollbarMarks was built from, so a refresh that buckets the same ones wakes no binding.
    /// GUI thread only.
    std::vector<vtbackend::ScrollbarTick> _scrollbarTicks;

    /// The track's height in logical pixels, as the QML last reported it. GUI thread only.
    int _scrollbarMarkTrackPixels = 0;

    /// The key the published ticks were built from; nullopt before the first refresh. GUI thread only.
    std::optional<ScrollbarMarksKey> _scrollbarMarksKey;

    /// The Grid::stableIdGeneration() the published ticks' target ids were read in: what a click on one hands
    /// Terminal::revealStableLineAtTop(), which refuses an id from another. GUI thread only.
    uint64_t _scrollbarTicksGeneration = 0;

    /// Fires refreshScrollbarMarks() a frame after the first request, so a burst of output costs one
    /// rebuild per frame rather than one per PTY read.
    QTimer _scrollbarMarksTimer;

    /// Collapses repeated requests to at most one post in flight, as _searchTallyPostPending does.
    std::atomic_flag _scrollbarMarksPostPending = ATOMIC_FLAG_INIT;

    struct CaptureBufferRequest
    {
        vtbackend::LineCount lines;
        bool logical;
    };

    /// Capture requests accepted but not yet answered, oldest first.
    ///
    /// A queue rather than one slot, and mutex-guarded, because the two ends sit on different threads:
    /// requestCaptureBuffer() is a Terminal::Events hook and appends from the terminal thread, while
    /// the permission gate drains it on the GUI thread. A single slot silently dropped whichever
    /// request arrived while another was outstanding, and every dropped request is a client blocked
    /// forever -- the protocol has no way to say "no reply is coming".
    /// @see docs/vt-extensions/buffer-capture.md
    std::vector<CaptureBufferRequest> _pendingBufferCaptures;
    std::mutex _pendingBufferCaptureMutex;

    std::optional<vtbackend::FontDef> _pendingFontChange;

    /// A paste that exceeded the soft limit and is waiting for the user's verdict.
    ///
    /// The text is stored already normalized and stripped, exactly as the immediate path would have
    /// sent it, rather than a @c QClipboard* re-read on approval: the clipboard may hold something
    /// else entirely by the time the dialog is answered, so re-reading would paste content the user
    /// never approved and never measured against the hard limit. It also spares a second synchronous
    /// round-trip to the clipboard's owning process. @see pasteFromClipboard.
    struct BigPasteRequest
    {
        std::string text;
        unsigned count;
    };
    std::optional<BigPasteRequest> _pendingBigPaste;

    /// The command line RerunCommand asked to run, while its dialog is up. Kept, not read again when the
    /// answer comes: the block's record may have gone by then, and what runs is what was shown.
    std::optional<vtbackend::CommandLineText> _pendingRerun;
    PermissionCache _rememberedPermissions;
    std::unique_ptr<QThread> _exitWatcherThread;

    std::atomic<bool> _onClosedHandled = false;
    std::mutex _onClosedMutex;

    /// Set by terminate() before it closes the PTY: tells onClosed() (exit-watcher thread) that this
    /// close was deliberate (tab/pane/window close, at-exit dump) and must NOT be routed through the
    /// early-exit "shell terminated too quickly" notice, which exists only for shells dying on their
    /// own shortly after startup.
    std::atomic<bool> _terminationRequested = false;
    /// Whether a tab-strip refresh for this session's program status is queued on the GUI thread, so that
    /// a program reporting in a loop queues one. Set on the parser thread, cleared on the GUI thread.
    std::atomic<bool> _isProgramStatusRefreshPending = false;

    /// @see noteTransportLost.
    std::atomic<bool> _transportLost = false;

    /// The finish notification this session has on the desktop: which block it reports and what
    /// withdraws it. At most one -- a newer finish replaces the older under the same identifier.
    /// Recorded on every platform, so withdrawal is uniform (a tray message's is a no-op).
    struct FinishNotice
    {
        vtbackend::CommandBlockId block {};                           ///< The block it reports.
        config::NotifyClearOn clearOn = config::NotifyClearOn::Focus; ///< What withdraws it.
    };
    std::optional<FinishNotice> _finishNotice;

    /// Whether gathering a block event must queue the call that hands the gathered events on.
    enum class BlockEventsCall : uint8_t
    {
        Queued = 0, ///< One is queued already, and takes this event along.
        Needed,     ///< None is: this event is the first since the last call.
    };

    /// Which records changed, as far as PendingBlockEvents keeps count.
    enum class ChangedRecords : uint8_t
    {
        None = 0, ///< No record changed.
        Listed,   ///< The records PendingBlockEvents::changed lists did.
        Unlisted, ///< More did than the list keeps: any record may have.
    };

    /// The command-block events the parser thread raised that the GUI thread has yet to handle. Gathered
    /// rather than queued one call apiece, so a flood of them -- a `cat` of a log replaying its OSC 133 --
    /// queues one call, and holds no more of them than the store holds records.
    struct PendingBlockEvents
    {
        std::deque<vtbackend::CommandBlockSummary> finished;  ///< Oldest first; the oldest go past the limit.
        std::vector<vtbackend::CommandBlockId> changed;       ///< Ascending, while ChangedRecords::Listed.
        ChangedRecords changedRecords = ChangedRecords::None; ///< How far @ref changed tells.
        vtbackend::CommandBlockId lastCommand {};             ///< As the newest change left the store.

        /// Gathers a finish.
        /// @param summary The finished block.
        /// @param limit The most finishes kept.
        /// @return Whether the call that hands the events on must be queued.
        [[nodiscard]] BlockEventsCall noteFinished(vtbackend::CommandBlockSummary const& summary,
                                                   size_t limit);

        /// Gathers a change.
        /// @param change The record that changed, and the newest command.
        /// @param limit The most changed records listed.
        /// @return Whether the call that hands the events on must be queued.
        [[nodiscard]] BlockEventsCall noteChanged(vtbackend::CommandBlockChange const& change, size_t limit);

        /// @return Whether nothing is gathered.
        [[nodiscard]] bool empty() const noexcept
        {
            return finished.empty() && changedRecords == ChangedRecords::None;
        }
    };

    /// Guards _pendingBlockEvents: the parser thread gathers, the GUI thread takes.
    std::mutex _pendingBlockEventsMutex;

    /// @see handlePendingBlockEvents
    PendingBlockEvents _pendingBlockEvents;

    /// The most finishes, and changed records, _pendingBlockEvents keeps: as many records as the terminal's
    /// store keeps, which is fixed with the terminal.
    size_t _pendingBlockEventsLimit =
        std::max(_terminal.settings().commandBlockLimits.maxRecords, size_t { 1 });

    /// How desktop notifications are raised. Never null: platform::makeDesktopNotifier() answers
    /// with a tray where there is no desktop to raise them on, so no #ifdef reaches this declaration and
    /// no call site has to check.
    std::unique_ptr<platform::Notifier> _desktopNotifier;
};

} // namespace contour::session

Q_DECLARE_INTERFACE(contour::session::TerminalSession, "org.contour.TerminalSession")

template <>
struct std::formatter<contour::session::GuardedRole>: std::formatter<std::string_view>
{
    template <typename FormatContext>
    auto format(contour::session::GuardedRole value, FormatContext& ctx) const
    {
        std::string_view output;
        // clang-format off
        switch (value)
        {
            case contour::session::GuardedRole::ChangeFont: output = "Change Font"; break;
            case contour::session::GuardedRole::CaptureBuffer: output = "Capture Buffer"; break;
            case contour::session::GuardedRole::ShowHostWritableStatusLine:  output = "show Host Writable Statusline"; break;
            case contour::session::GuardedRole::BigPaste:  output = "paste large number of characters"; break;
            case contour::session::GuardedRole::RerunCommand: output = "run a command again"; break;
        }
        // clang-format on
        return formatter<string_view>::format(output, ctx);
    }
};
