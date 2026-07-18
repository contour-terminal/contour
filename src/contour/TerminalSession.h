// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/Actions.h>
#include <contour/Audio.h>
#include <contour/ColorConversion.h>
#include <contour/Config.h>
#include <contour/ContextMenu.h>
#include <contour/HorizontalWheelGesture.h>
#if defined(__linux__)
    #include <contour/FreeDesktopNotifier.h>
#endif
#include <contour/helper.h>

#include <vtbackend/Terminal.h>

#include <vtrasterizer/Renderer.h>

#include <crispy/point.h>

#include <QtCore/QAbstractItemModel>
#include <QtCore/QFileSystemWatcher>
#include <QtCore/QThread>
#include <QtQml/QJSValue>

#include <atomic>
#include <cstdint>
#include <format>
#include <thread>

#include <qcolor.h>

#include <vtmux/Primitives.h>

namespace contour
{

namespace display
{
    class ForcedFontDpiProvider;
    class TerminalDisplay;
} // namespace display

class ContourGuiApp;
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
 * and the user decided to permenently decide for the current session.
 */
using PermissionCache = std::map<GuardedRole, bool>;

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

    // Q_PROPERTY(QString profileName READ profileName NOTIFY profileNameChanged)

  public:
    // {{{ Model property helper

    int getUptime() const noexcept
    {
        auto const now = std::chrono::steady_clock::now();
        auto const diff = std::chrono::duration_cast<std::chrono::seconds>(now - _startTime);
        return static_cast<int>(diff.count());
    }

    QString getBellSource() const noexcept
    {
        if (_profile.bell.value().sound == "default")
        {
            return QString("qrc:/contour/bell.wav");
        }
        if (_profile.bell.value().sound == "off")
        {
            return QString();
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
            return QString();

        if (const auto* p = std::get_if<std::filesystem::path>(&backgroundImage->location))
            return QString("file:") + QString(p->string().c_str());

        return QString();
    }
    QColor getBackgroundColor() const noexcept
    {
        auto color = terminal().isModeEnabled(vtbackend::DECMode::ReverseVideo)
                         ? _terminal.colorPalette().defaultForeground
                         : _terminal.colorPalette().defaultBackground;
        auto alpha = static_cast<uint8_t>(_profile.background.value().opacity);
        return toQColor(color, alpha);
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
        if (_terminal.colorPalette().backgroundImage)
        {
            return true;
        }
        return false;
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
    /// @param pixelDelta Pixel-precise delta (trackpads), or {0,0}.
    /// @param angleDelta Angle delta (wheels), or {0,0}.
    /// @param phase      The gesture phase the windowing system reported.
    void addToAccumulatedScroll(crispy::point pixelDelta,
                                crispy::point angleDelta,
                                vtbackend::ScrollPhase phase) noexcept;

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

    int historyLineCount() const noexcept { return unbox(_terminal.currentScreen().historyLineCount()); }

    int scrollOffset() const noexcept { return unbox(terminal().viewport().scrollOffset()); }
    void setScrollOffset(int value)
    {
        terminal().resetSmoothScroll();
        terminal().viewport().scrollTo(vtbackend::ScrollOffset::cast_from(value));
    }

    void onScrollOffsetChanged(vtbackend::ScrollOffset value) override;
    // }}}

    // {{{ QAbstractItemModel overrides
    QModelIndex index(int row, int column, const QModelIndex& parent = QModelIndex()) const override;
    QModelIndex parent(const QModelIndex& child) const override;
    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    bool setData(const QModelIndex& index, const QVariant& value, int role = Qt::EditRole) override;
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
     */
    TerminalSession(TerminalSessionManager* manager,
                    std::unique_ptr<vtpty::Pty> pty,
                    ContourGuiApp& app,
                    std::string profileName = {},
                    std::optional<vtbackend::PageSize> initialPageSize = std::nullopt,
                    std::optional<vtpty::Process::ExecInfo> launchedCommand = std::nullopt);
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

    /// The id by which the vtmux layout model refers to this session. Set when the manager mirrors
    /// the session into the model. Identifies the leaf pane that hosts this session.
    [[nodiscard]] vtmux::SessionId modelSessionId() const noexcept { return _modelSessionId; }
    void setModelSessionId(vtmux::SessionId id) noexcept { _modelSessionId = id; }

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

    /// Starts the VT background thread.
    void start();

    /// Initiates termination of this session, regardless of the underlying terminal state.
    /// Works whether or not a display is attached: with a display the teardown is routed through
    /// closeDisplay(); without one the PTY device is closed directly so the same
    /// onClosed() -> sessionClosed -> TerminalSessionManager::removeSession path still runs (so a
    /// background tab/split-pane session does not leak when closed).
    void terminate();

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

    display::TerminalDisplay* display() noexcept { return _display; }
    display::TerminalDisplay const* display() const noexcept { return _display; }

    void attachDisplay(display::TerminalDisplay& display);
    void detachDisplay(display::TerminalDisplay& display);

    TerminalSessionManager* getTerminalManager() const noexcept { return _manager; }

    /// The app-wide forced-font-DPI provider (see display/ContentScale.h), for injection into the
    /// display. Routed through the session so the display layer needs no ContourGuiApp dependency.
    /// @return The provider, or nullptr when no Qt application exists (tests).
    [[nodiscard]] display::ForcedFontDpiProvider* forcedFontDpiProvider() noexcept;

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
    void setTerminalProfile(std::string const& configProfileName) override;
    void discardImage(vtbackend::Image const&) override;
    void inputModeChanged(vtbackend::ViMode mode) override;
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
                       uint32_t physicalKey,
                       vtbackend::KeyboardModifiers modifiers,
                       vtbackend::KeyboardEventType eventType,
                       Timestamp now);

    void sendMousePressEvent(vtbackend::Modifiers modifiers,
                             vtbackend::MouseButton button,
                             vtbackend::PixelCoordinate pixelPosition);
    void sendMouseMoveEvent(vtbackend::Modifiers modifiers,
                            vtbackend::CellLocation pos,
                            vtbackend::PixelCoordinate pixelPosition);
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
    bool operator()(actions::ScrollDown);
    bool operator()(actions::ScrollMarkDown);
    bool operator()(actions::ScrollMarkUp);
    bool operator()(actions::ScrollOneDown);
    bool operator()(actions::ScrollOneUp);
    bool operator()(actions::ScrollPageDown);
    bool operator()(actions::ScrollPageUp);
    bool operator()(actions::ScrollToBottom);
    bool operator()(actions::ScrollToTop);
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
    bool operator()(actions::CreateNewTab);
    bool operator()(actions::CloseTab);
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
    [[nodiscard]] ContextMenuState contextMenuState();

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
    bool reloadConfig(config::Config newConfig, std::string const& profileName);
    int executeAllActions(std::vector<actions::Action> const& actions);
    void spawnNewTerminal(std::string const& profileName);

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

    /// Copies @p part of the most recently finished shell command into the clipboard.
    /// @param part Which part of the command block to copy.
    /// @return false when the scrollback holds no finished command (no OSC 133 shell integration).
    bool copyLastCommandBlock(vtbackend::CommandBlockPart part);

    void setFontSize(text::font_size size);

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
    vtmux::SessionId _modelSessionId {};
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

    crispy::point _accumulatedPixelScroll;
    crispy::point _accumulatedAngleScroll;
    HorizontalWheelGesture _horizontalWheelGesture;

    vtbackend::Terminal _terminal;
    bool _terminatedAndWaitingForKeyPress = false;
    display::TerminalDisplay* _display = nullptr;

    std::unique_ptr<QFileSystemWatcher> _configFileChangeWatcher;

    std::atomic<bool> _terminating { false };

    /// Guards against piling up caret-report posts. Set on the terminal thread when one is queued and
    /// cleared on the GUI thread when it runs, so a cursor that moves faster than the GUI thread drains
    /// its queue coalesces into one report rather than a backlog.
    std::atomic_flag _caretUpdatePending = ATOMIC_FLAG_INIT;
    std::thread::id _mainLoopThreadID {};
    std::unique_ptr<std::thread> _screenUpdateThread;

    // state vars
    //
    vtbackend::ScreenType _currentScreenType = vtbackend::ScreenType::Primary;
    vtbackend::CellLocation _currentMousePosition = vtbackend::CellLocation {};
    bool _allowKeyMappings = true;
    std::unique_ptr<Audio> _audio;
    std::vector<int> _musicalNotesBuffer;

    vtbackend::LineCount _lastHistoryLineCount;

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

#if defined(__linux__)
    FreeDesktopNotifier _desktopNotifier;
#endif
};

} // namespace contour

Q_DECLARE_INTERFACE(contour::TerminalSession, "org.contour.TerminalSession")

template <>
struct std::formatter<contour::GuardedRole>: std::formatter<std::string_view>
{
    template <typename FormatContext>
    auto format(contour::GuardedRole value, FormatContext& ctx) const
    {
        std::string_view output;
        // clang-format off
        switch (value)
        {
            case contour::GuardedRole::ChangeFont: output = "Change Font"; break;
            case contour::GuardedRole::CaptureBuffer: output = "Capture Buffer"; break;
            case contour::GuardedRole::ShowHostWritableStatusLine:  output = "show Host Writable Statusline"; break;
            case contour::GuardedRole::BigPaste:  output = "paste large number of characters"; break;
        }
        // clang-format on
        return formatter<string_view>::format(output, ctx);
    }
};
