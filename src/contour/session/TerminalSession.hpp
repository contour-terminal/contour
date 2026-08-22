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
#include <contour/session/DisplaySurface.hpp>
#include <contour/session/HyperlinkTooltip.hpp>
#include <contour/session/SearchStatus.hpp>

#include <vtbackend/core/WorkingDirectory.hpp>
#include <vtbackend/screen/Terminal.hpp>

#include <vtpty/Pty.hpp>

#include <vtrasterizer/Renderer.hpp>

#include <crispy/Point.hpp>

#include <QtCore/QAbstractItemModel>
#include <QtCore/QFileSystemWatcher>
#include <QtCore/QThread>
#include <QtCore/QTimer>
#include <QtQml/QJSValue>

#include <atomic>
#include <concepts>
#include <cstdint>
#include <format>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <thread>

#include <qcolor.h>

#include <vtworkspace/Primitives.hpp>

namespace contour
{

namespace display
{
    class ForcedFontDpiProvider;
} // namespace display

class ContourGuiApp;

} // namespace contour

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
};

/**
 * Trivial cache to remember the interactive choice when the user has to be asked
 * and the user decided to permanently decide for the current session.
 */
using PermissionCache = std::map<GuardedRole, bool>;

/// Whether the fold gutter took an input event, and the grid must therefore not also see it.
///
/// Named rather than left to a bool because the call sites read as a question about routing, not about
/// truth: a bare `true` there says nothing about which of the two paths it selects.
enum class ConsumedByGutter : uint8_t
{
    No = 0,
    Yes,
};

/// Whether a folding action is subject to the folding.enabled setting.
///
/// Named rather than left to a bool because the exception reads as a rule, not as a negation: turning
/// the feature off must still let a user undo what they folded while it was on, rather than stranding
/// the output behind a disabled setting.
enum class FoldingGate : uint8_t
{
    Configured = 0, ///< The action runs only while folding is enabled.
    Always,         ///< The action runs regardless -- it can only ever REVEAL output.
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

    /// Withdraws the hyperlink tooltip, whatever the pointer is over.
    ///
    /// Called when the pointer leaves the terminal, and when the viewport scrolls: the hovered-link
    /// state tracks mouse MOVEMENT, so scrolling moves the link out from under a stationary pointer
    /// without anything noticing. Hiding is the honest answer to that; continuing to show a tooltip for
    /// a link no longer under the pointer is worse than showing none.
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
        if (_terminal.colorPalette().backgroundImage.get())
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

        if ((_currentScreenType == vtbackend::ScreenType::Alternate)
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

    int pageLineCount() const noexcept { return unbox(_terminal.pageSize().lines); }

    int pageColumnsCount() const noexcept { return unbox(_terminal.pageSize().columns); }

    bool showResizeIndicator() const noexcept { return _config.profile().sizeIndicatorOnResize.value(); }

    /// How far the scrollbar can scroll up, in rows the user can actually reach.
    ///
    /// The scrollable count rather than the raw history depth: collapsed folds take their output out
    /// of the scrollable range, and a scrollbar sized by the raw history would offer a stretch of
    /// travel at the top that scrolls nowhere.
    ///
    /// Reported from what announceScrollableLineCount() last published, and emphatically NOT computed
    /// here -- see there for why the GUI thread may neither compute this count nor take the lock that
    /// would make computing it safe. It is therefore also exactly what the NOTIFY signal carried.
    int historyLineCount() const noexcept { return _lastHistoryLineCount.load(std::memory_order_relaxed); }

    int scrollOffset() const noexcept { return unbox(terminal().viewport().scrollOffset()); }
    void setScrollOffset(int value)
    {
        terminal().resetSmoothScroll();
        terminal().viewport().scrollTo(vtbackend::ScrollOffset::cast_from(value));
    }

    void onScrollOffsetChanged(vtbackend::ScrollOffset value) override;
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
     *        this platform offers, per platform::makeDesktopNotifier(); a test passes a recording
     *        one to assert what OSC 99 asked for and to drive close/activation events back.
     */
    TerminalSession(TerminalSessionManager* manager,
                    std::unique_ptr<vtpty::Pty> pty,
                    ContourGuiApp& app,
                    std::string profileName = {},
                    std::optional<vtbackend::PageSize> initialPageSize = std::nullopt,
                    std::optional<vtpty::Process::ExecInfo> launchedCommand = std::nullopt,
                    std::unique_ptr<platform::Notifier> notifier = nullptr);
    ~TerminalSession() override;

    int id() const noexcept { return _id; }

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
    vtbackend::ScreenType currentScreenType() const noexcept { return _currentScreenType; }

    /// The session's current working directory, for inheritance when spawning a new tab, window or
    /// split pane from this one. On non-Windows this reads the local Process's working directory (when
    /// the device is a local process); on Windows it queries the terminal's reported cwd under the
    /// terminal lock. Falls back to "." when no working directory can be determined (e.g. an SSH
    /// session on non-Windows). Centralizing this here keeps every spawn path — new tab, new window and
    /// split pane — using the same logic, including the Windows fallback.
    /// @return The working directory path, or "." if unavailable.
    [[nodiscard]] std::string workingDirectory() const;

    /// This machine's identity, for deciding whether a context describes it.
    [[nodiscard]] vtbackend::LocalIdentity localIdentity() const noexcept
    {
        return vtbackend::LocalIdentity { .machineId = _localMachineId, .hostname = _localHostName };
    }

    /// The working directory for @p purpose, resolved from the OSC 3008 ancestry and then OSC 7.
    ///
    /// One resolution consulted by every caller, so "where does a new tab start" and "what does the
    /// tab tooltip say" can never disagree about the ORDER even though they deliberately disagree
    /// about the FILTER: a path worth SHOWING need not be one a child could be spawned in.
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
    Q_INVOKABLE void resizeTerminalToDisplaySize();

    void updateColorPreference(vtbackend::ColorPreference preference);

    // vtbackend::Events
    //
    void requestCaptureBuffer(vtbackend::LineCount lines, bool logical) override;
    void bell() override;
    void bufferChanged(vtbackend::ScreenType) override;
    void renderBufferUpdated() override;
    void screenUpdated() override;
    vtbackend::FontDef getFontDef() override;
    void setFontDef(vtbackend::FontDef const& fontDef) override;
    void copyToClipboard(std::string_view data) override;
    void setPointerShape(std::string_view cssName) override;
    void openDocument(std::string_view /*fileOrUrl*/) override;
    void inspect() override;
    void notify(std::string_view title, std::string_view content) override;
    void showDesktopNotification(vtbackend::DesktopNotification const& notification) override;
    void discardDesktopNotification(std::string_view identifier) override;
    void focusTerminalWindow() override;
    void onClosed() override;

    void pasteFromClipboard(unsigned count, bool strip) override;
    void onSelectionCompleted() override;
    void requestWindowResize(vtbackend::LineCount, vtbackend::ColumnCount) override;
    void requestWindowResize(vtbackend::Width, vtbackend::Height) override;
    void setWindowTitle(std::string_view title) override;
    void setTabName(std::string_view name) override;
    void setWindowFrameColor(vtbackend::RGBColor color) override;
    void resetWindowFrameColor() override;
    void progressChanged(vtbackend::Progress progress) override;
    void setTerminalProfile(std::string const& configProfileName) override;
    void discardImage(vtbackend::Image const&) override;
    void inputModeChanged(vtbackend::ViMode mode) override;
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
                             vtbackend::PixelCoordinate pixelPosition);
    void sendMouseMoveEvent(vtbackend::Modifiers modifiers,
                            vtbackend::CellLocation pos,
                            vtbackend::PixelCoordinate pixelPosition);

    /// Reports where the pointer is over the fold gutter, or that it is not over it at all.
    ///
    /// Consulted BEFORE the ordinary move path and, when it claims the event, instead of it: the
    /// gutter is not part of the grid, so a move there must not reach the mouse protocol, extend a
    /// selection, or hover a hyperlink at the column-0 cell it would otherwise be clamped onto.
    ///
    /// @param gridLine The grid line under the pointer, or nullopt when it is not over the gutter.
    /// @return Whether the gutter consumed the event.
    ConsumedByGutter sendGutterHoverEvent(std::optional<vtbackend::LineOffset> gridLine);

    /// Toggles the fold at @p gridLine when the press landed on the fold column.
    ///
    /// The press half of the gutter's click handshake, and the only place that arms it: a press the
    /// child never saw must be followed by a release it never sees either, and leaving that to the
    /// caller means a third event path re-deriving the rule (@see sendGutterReleaseEvent).
    ///
    /// @param gridLine The grid line under the pointer, or nullopt when it is not over the gutter.
    /// @param button The button that went down.
    /// @return Whether the gutter consumed the event.
    ConsumedByGutter sendGutterPressEvent(std::optional<vtbackend::LineOffset> gridLine,
                                          vtbackend::MouseButton button);

    /// Swallows the release matching a press the fold column consumed.
    /// @return Whether the gutter consumed the event.
    ConsumedByGutter sendGutterReleaseEvent();
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

    // Actions
    bool operator()(actions::CancelSelection);
    bool operator()(actions::ChangeProfile const&);
    bool operator()(actions::ClearHistoryAndReset);
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
    bool operator()(actions::OpenContextMenu);
    bool operator()(actions::SelectAll);
    bool operator()(actions::SoftReset);
    bool operator()(actions::CopyLastCommandPrompt);
    bool operator()(actions::CopyLastCommandOutput);
    bool operator()(actions::CopyLastCommandBlock);
    bool operator()(actions::CopyHyperlink const& action);

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
    ///         the cursor returns false, which is what lets a key binding fall through to the terminal.
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

    void requestPermission(config::Permission allowedByConfig, GuardedRole role);
    void executeRole(GuardedRole role, bool allow, bool remember);

    /// Derives the terminal's image canvas ceiling from the monitor the display currently sits on.
    ///
    /// The single writer of that ceiling; call it whenever the display attaches or changes monitor.
    /// The cap is the monitor rather than the window, so an ordinary resize needs no update. No-op
    /// while no window/screen is available — the caller re-runs once one is.
    void updateImageCanvasCeiling();

  signals:
    void hyperlinkHoverChanged();
    /// The find bar should open and take focus. Raised by searchPromptRequested(), the
    /// vtbackend::Terminal::Events hook -- which cannot itself be the signal, being an override.
    void searchBarRequested();
    /// The pattern, its tally, or the case policy changed.
    void searchStateChanged();
    void sessionClosed(TerminalSession&);
    void profileNameChanged(QString newValue);
    void lineCountChanged(int newValue);
    void columnsCountChanged(int newValue);
    void historyLineCountChanged(int newValue);
    void scrollOffsetChanged(int newValue);
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
    void showNotification(QString const& title, QString const& content);
    void fontSizeChanged();

    // Tab handling signals
    void createNewTab();
    void closeTab();
    void switchToPreviousTab();
    void switchToTabLeft();
    void switchToTabRight();
    void switchToTab(int position);

  public slots:
    void onConfigReload();
    void onHighlightUpdate();
    void configureDisplay();

  private:
    // helpers

    /// Runs @p action against the terminal under the lock, and republishes the scrollable count.
    ///
    /// The one gate every folding action passes, so that adding one is a handler rather than a handler
    /// plus a remembered check -- a forgotten check leaves an action live behind a disabled setting,
    /// and nothing diagnoses it. Republishing is here for the same reason: an action that moved rows
    /// into or out of the scrollable range and did not say so leaves the scrollbar sized for the range
    /// before it.
    ///
    /// @param action What to do with the terminal; its result is the action's result.
    /// @param gate   Whether @p action is subject to the folding.enabled setting.
    /// @return What @p action returned, or false when folding is disabled and @p gate honours it.
    bool withFolding(std::invocable<vtbackend::Terminal&> auto&& action,
                     FoldingGate gate = FoldingGate::Configured);

    bool reloadConfig(config::Config newConfig, std::string const& profileName);
    int executeAllActions(std::vector<actions::Action> const& actions);
    void spawnNewTerminal(std::string const& profileName);

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

    /// Publishes @p scrollable as the scrollbar's travel, announcing it when it moved.
    ///
    /// The historyLineCount property reads what this stored rather than computing a count of its
    /// own, because computing one goes through the fold projection -- a lazily built cache the
    /// render pass clears and refills under the terminal lock, so a GUI-thread rebuild races it.
    /// Taking that lock in the property instead is what cannot be done: a scroll performed under it
    /// emits scrollOffsetChanged, whose QML handler reads this very property on the same thread, and
    /// the lock is not recursive. So every caller computes the count itself and hands it here, and
    /// this is called OUTSIDE any lock -- the binding it wakes reaches back into the session.
    ///
    /// @param scrollable The scrollable line count, computed by the caller.
    void announceScrollableLineCount(vtbackend::LineCount scrollable);

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
    bool reloadConfigWithProfile(std::string const& profileName);
    bool resetConfig();
    void followHyperlink(vtbackend::HyperlinkInfo const& hyperlink);

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

    /// Copies @p part of the most recently finished shell command into the clipboard.
    /// @param part Which part of the command block to copy.
    /// @return false when the scrollback holds no finished command (no OSC 133 shell integration).
    bool copyLastCommandBlock(vtbackend::CommandBlockPart part);

    void setFontSize(text::FontSize size);

    /// Posts a refresh of the indicator status-line tab info (tab names) to the GUI thread.
    ///
    /// Called when the window title or tab name changes at runtime. Must not refresh inline: these
    /// notifications arrive on the parser thread while the terminal state mutex is held, and the
    /// refresh re-enters that non-recursive mutex (see scheduleRedraw()), so it is deferred to the GUI
    /// thread via the display's event loop.
    void refreshGuiTabInfoForStatusLine();

    void setDefaultCursor();
    void configureTerminal();
    void configureCursor(config::CursorConfig const& cursorConfig);

    /// Scrolls up by @p lineCount lines, using smooth pixel scrolling if enabled, otherwise line-based.
    void smoothScrollUp(vtbackend::LineCount lineCount);

    /// Scrolls down by @p lineCount lines, using smooth pixel scrolling if enabled, otherwise line-based.
    void smoothScrollDown(vtbackend::LineCount lineCount);
    uint8_t matchModeFlags() const;
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
    config::TerminalProfile _profile;
    ContourGuiApp& _app;
    vtbackend::ColorPreference _currentColorPreference;
    /// This machine's host name, asked once because one of the file:// URLs measured against it is
    /// resolved on the GUI thread under the terminal lock. @see vtbackend::isLocalHost
    std::string _localHostName;

    /// This machine's /etc/machine-id, read once, empty where there is none.
    ///
    /// The STRONG half of the locality test an OSC 3008 working directory needs. Nothing emits a
    /// `remote` context for ssh today, so a remote host's own sequences look entirely local and its
    /// paths may well exist here too; comparing machine ids is the only thing that catches it, and
    /// systemd's shim emits one for free. @see vtbackend::LocalIdentity.
    std::string _localMachineId;

    crispy::Point _accumulatedPixelScroll;
    crispy::Point _accumulatedAngleScroll;
    input::HorizontalWheelGesture _horizontalWheelGesture;
    HyperlinkHoverTracker _hyperlinkHover;
    /// Never null: a NullAnnouncer stands in wherever there is nothing to announce through, so the call
    /// sites never have to ask whether announcing is possible.
    std::unique_ptr<platform::Announcer> _announcer = std::make_unique<platform::NullAnnouncer>();
    QString _hyperlinkTooltipText;
    QRectF _hyperlinkTooltipAnchor;

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
    /// means the user is reading something they went looking for. @see screenUpdated.
    /// Atomic: written on the GUI thread, read by screenUpdated() on the parser thread.
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

    vtbackend::Terminal _terminal;
    bool _terminatedAndWaitingForKeyPress = false;
    DisplaySurface* _display = nullptr;

    std::unique_ptr<QFileSystemWatcher> _configFileChangeWatcher;

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
    vtbackend::ScreenType _currentScreenType = vtbackend::ScreenType::Primary;
    vtbackend::CellLocation _currentMousePosition = vtbackend::CellLocation {};

    /// Whether the pointer was last seen over the fold gutter rather than over the grid.
    ///
    /// Kept here so the common case -- pointer over the grid -- can be answered without taking
    /// the terminal lock on every single motion event, while still clearing a hover that was set.
    bool _gutterHovered = false;

    /// Whether this pane's last press was consumed by the fold column, so its release must be too.
    ///
    /// A press the child never saw must not be followed by a release it does see: that leaves the
    /// application holding a button down that was never pressed. Per SESSION rather than per process:
    /// every pane and every window shares the GUI thread, so a press in one pane's gutter followed by
    /// a release delivered to another would otherwise swallow the second pane's release.
    bool _gutterClickPending = false;

    /// Whether the mouse cursor shape must be re-decided on the next motion, even over the same cell.
    ///
    /// The move path changes the shape only when the pointer changes grid CELL, that being the only
    /// thing that can change its answer -- but the fold gutter sets the shape without going through
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
    bool _allowKeyMappings = true;
    std::unique_ptr<platform::Audio> _audio;
    std::vector<int> _musicalNotesBuffer;

    /// The scrollable line count the historyLineCount property reports and its NOTIFY signal last
    /// carried. Atomic because the parser thread publishes it while the GUI thread reads it; see
    /// announceScrollableLineCount() for why the GUI thread cannot simply compute it.
    std::atomic<int> _lastHistoryLineCount = 0;

    struct CaptureBufferRequest
    {
        vtbackend::LineCount lines;
        bool logical;
    };
    std::optional<CaptureBufferRequest> _pendingBufferCapture;
    std::optional<vtbackend::FontDef> _pendingFontChange;
    std::optional<QClipboard*> _pendingBigPaste;
    PermissionCache _rememberedPermissions;
    std::unique_ptr<QThread> _exitWatcherThread;

    std::atomic<bool> _onClosedHandled = false;
    std::mutex _onClosedMutex;

    /// Set by terminate() before it closes the PTY: tells onClosed() (exit-watcher thread) that this
    /// close was deliberate (tab/pane/window close, at-exit dump) and must NOT be routed through the
    /// early-exit "shell terminated too quickly" notice, which exists only for shells dying on their
    /// own shortly after startup.
    std::atomic<bool> _terminationRequested = false;

    /// @see noteTransportLost.
    std::atomic<bool> _transportLost = false;

    /// How desktop notifications are raised. Never null: platform::makeDesktopNotifier() answers
    /// with a NullNotifier where there is nothing to raise them through, so no #ifdef reaches this
    /// declaration and no call site has to check.
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
        }
        // clang-format on
        return formatter<string_view>::format(output, ctx);
    }
};
