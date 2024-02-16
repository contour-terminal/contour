// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/Actions.h>
#include <contour/Audio.h>
#include <contour/Config.h>
#include <contour/helper.h>

#include <vtbackend/Terminal.h>

#include <vtrasterizer/Renderer.h>

#include <crispy/point.h>

#include <fmt/format.h>

#include <QtCore/QAbstractItemModel>
#include <QtCore/QFileSystemWatcher>
#include <QtCore/QThread>
#include <QtQml/QJSValue>

#include <cstdint>
#include <thread>

#include <qcolor.h>

namespace contour
{

namespace display
{
    class TerminalDisplay;
}

class ContourGuiApp;

/**
 * A set of user-facing activities that are guarded behind a permission-check wall.
 */
enum class GuardedRole
{
    ChangeFont,
    CaptureBuffer,
    ShowHostWritableStatusLine,
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
    Q_PROPERTY(QString pathToBackground READ pathToBackground NOTIFY pathToBackgroundChanged)
    Q_PROPERTY(float opacityBackground READ getOpacityBackground NOTIFY opacityBackgroundChanged)
    Q_PROPERTY(bool isImageBackground READ getIsImageBackground NOTIFY isImageBackgroundChanged)
    Q_PROPERTY(bool isBlurBackground READ getIsBlurBackground NOTIFY isBlurBackgroundChanged)
    Q_PROPERTY(QColor backgroundColor READ getBackgroundColor NOTIFY backgroundColorChanged)
    Q_PROPERTY(bool isScrollbarRight READ getIsScrollbarRight NOTIFY isScrollbarRightChanged)
    Q_PROPERTY(bool isScrollbarVisible READ getIsScrollbarVisible NOTIFY isScrollbarVisibleChanged)
    Q_PROPERTY(int fontSize READ getFontSize)
    Q_PROPERTY(int upTime READ getUptime)

    // Q_PROPERTY(QString profileName READ profileName NOTIFY profileNameChanged)

  public:
    // {{{ Model property helper

    int getUptime() const noexcept
    {
        auto const now = std::chrono::steady_clock::now();
        auto const diff = std::chrono::duration_cast<std::chrono::seconds>(now - _startTime);
        return static_cast<int>(diff.count());
    }

    int getFontSize() const noexcept { return static_cast<int>(_profile.fonts.value().size.pt); }
    float getOpacity() const noexcept
    {
        fmt::print(" ====== OPACITY background {} ", static_cast<float>(_profile.backgroundOpacity.value()));
        return static_cast<float>(_profile.backgroundOpacity.value()) / std::numeric_limits<uint8_t>::max();
    }
    QString pathToBackground() const
    {
        if (const auto& p =
                std::get_if<std::filesystem::path>(&(_terminal.colorPalette().backgroundImage->location)))
        {
            return QString("file:") + QString(p->string().c_str());
        }
        else
            return QString();
    }
    QColor getBackgroundColor() const noexcept
    {
        auto color = terminal().isModeEnabled(vtbackend::DECMode::ReverseVideo)
                         ? _terminal.colorPalette().defaultForeground
                         : _terminal.colorPalette().defaultBackground;
        auto alpha = static_cast<uint8_t>(_profile.backgroundOpacity.value());
        return QColor(color.red, color.green, color.blue, alpha);
    }
    float getOpacityBackground() const noexcept
    {
        if (_terminal.colorPalette().backgroundImage.get())
        {
            fmt::print(" getOpacityBackground  non zerp \n ");
            return _terminal.colorPalette().backgroundImage->opacity;
        }
        return 0.0;
    }
    bool getIsImageBackground() const noexcept
    {
        if (_terminal.colorPalette().backgroundImage)
        {
            fmt::print(" BACKGROUND IS IMAGE \n");
            return true;
        }
        return false;
    }

    bool getIsBlurBackground() const noexcept
    {
        if (getIsImageBackground())
        {

            fmt::print(" BACKGROUND IS BLUR \n");
            return _terminal.colorPalette().backgroundImage->blur;
        }
        return false;
    }

    bool getIsScrollbarRight() const noexcept
    {
        return profile().scrollbarPosition.value() == config::ScrollBarPosition::Right;
    }

    bool getIsScrollbarVisible() const noexcept
    {
        if (profile().scrollbarPosition.value() == config::ScrollBarPosition::Hidden)
            return false;

        if ((_currentScreenType == vtbackend::ScreenType::Alternate)
            && profile().hideScrollbarInAltScreen.value())
            return false;

        return true;
    }

    int getScrollX() const noexcept { return _accumulatedScrollX; }
    void addScrollX(int v) noexcept { _accumulatedScrollX += v; }
    void resetScrollX() noexcept { _accumulatedScrollX = 0; }

    int getScrollY() const noexcept { return _accumulatedScrollY; }
    void addScrollY(int v) noexcept { _accumulatedScrollY += v; }
    void resetScrollY() noexcept { _accumulatedScrollY = 0; }

    QString title() const;
    void setTitle(QString const& value) { terminal().setWindowTitle(value.toStdString()); }

    int pageLineCount() const noexcept { return unbox(_terminal.pageSize().lines); }

    int pageColumnsCount() const noexcept { return unbox(_terminal.pageSize().columns); }

    bool showResizeIndicator() const noexcept { return _config.profile().sizeIndicatorOnResize.value(); }

    int historyLineCount() const noexcept { return unbox(_terminal.currentScreen().historyLineCount()); }

    int scrollOffset() const noexcept { return unbox(terminal().viewport().scrollOffset()); }
    void setScrollOffset(int value)
    {
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
     * @param pty a PTY object (can be process, networked, mockup, ...)
     * @param display fronend display to render the terminal.
     */
    TerminalSession(std::unique_ptr<vtpty::Pty> pty, ContourGuiApp& app);
    ~TerminalSession() override;

    int id() const noexcept { return _id; }

    /// Starts the VT background thread.
    void start();

    /// Initiates termination of this session, regardless of the underlying terminal state.
    void terminate();

    config::Config const& config() const noexcept { return _config; }
    config::TerminalProfile const& profile() const noexcept { return _profile; }

    double contentScale() const noexcept { return _contentScale; }
    void setContentScale(double value) noexcept { _contentScale = value; }

    vtpty::Pty& pty() noexcept { return _terminal.device(); }
    vtbackend::Terminal& terminal() noexcept { return _terminal; }
    vtbackend::Terminal const& terminal() const noexcept { return _terminal; }
    vtbackend::ScreenType currentScreenType() const noexcept { return _currentScreenType; }

    display::TerminalDisplay* display() noexcept { return _display; }
    display::TerminalDisplay const* display() const noexcept { return _display; }

    void attachDisplay(display::TerminalDisplay& display);
    void detachDisplay(display::TerminalDisplay& display);

    Q_INVOKABLE void applyPendingFontChange(bool answer, bool remember);
    Q_INVOKABLE void executePendingBufferCapture(bool answer, bool remember);
    Q_INVOKABLE void executeShowHostWritableStatusLine(bool answer, bool remember);
    Q_INVOKABLE void adaptToWidgetSize();

    void updateColorPreference(vtbackend::ColorPreference preference);

    // vtbackend::Events
    //
    void requestCaptureBuffer(vtbackend::LineCount lineCount, bool logical) override;
    void bell() override;
    void bufferChanged(vtbackend::ScreenType) override;
    void renderBufferUpdated() override;
    void screenUpdated() override;
    vtbackend::FontDef getFontDef() override;
    void setFontDef(vtbackend::FontDef const& fontSpec) override;
    void copyToClipboard(std::string_view data) override;
    void openDocument(std::string_view /*fileOrUrl*/) override;
    void inspect() override;
    void notify(std::string_view title, std::string_view body) override;
    void onClosed() override;
    void pasteFromClipboard(unsigned count, bool strip) override;
    void onSelectionCompleted() override;
    void requestWindowResize(vtbackend::LineCount, vtbackend::ColumnCount) override;
    void requestWindowResize(vtbackend::Width, vtbackend::Height) override;
    void setWindowTitle(std::string_view title) override;
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
                      vtbackend::Modifiers modifiers,
                      vtbackend::KeyboardEventType eventType,
                      Timestamp now);
    void sendCharEvent(char32_t value,
                       uint32_t physicalKey,
                       vtbackend::Modifiers modifiers,
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

    void sendFocusInEvent();
    void sendFocusOutEvent();

    // Actions
    bool operator()(actions::CancelSelection);
    bool operator()(actions::ChangeProfile const&);
    bool operator()(actions::ClearHistoryAndReset);
    bool operator()(actions::CopyPreviousMarkRange);
    bool operator()(actions::CopySelection);
    bool operator()(actions::CreateDebugDump);
    bool operator()(actions::DecreaseFontSize);
    bool operator()(actions::DecreaseOpacity);
    bool operator()(actions::FollowHyperlink);
    bool operator()(actions::FocusNextSearchMatch);
    bool operator()(actions::FocusPreviousSearchMatch);
    bool operator()(actions::IncreaseFontSize);
    bool operator()(actions::IncreaseOpacity);
    bool operator()(actions::NewTerminal const&);
    bool operator()(actions::NoSearchHighlight);
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
    bool operator()(actions::ToggleInputProtection);
    bool operator()(actions::ToggleStatusLine);
    bool operator()(actions::ToggleTitleBar);
    bool operator()(actions::TraceBreakAtEmptyQueue);
    bool operator()(actions::TraceEnter);
    bool operator()(actions::TraceLeave);
    bool operator()(actions::TraceStep);
    bool operator()(actions::ViNormalMode);
    bool operator()(actions::WriteScreen const& event);

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
    void onBell(float volume);
    void onAlert();
    void requestPermissionForFontChange();
    void requestPermissionForBufferCapture();
    void requestPermissionForShowHostWritableStatusLine();
    void showNotification(QString const& title, QString const& content);
    void fontSizeChanged();

  public slots:
    void onConfigReload();
    void onHighlightUpdate();
    void configureDisplay();

  private:
    // helpers
    bool reloadConfig(config::Config newConfig, std::string const& profileName);
    int executeAllActions(std::vector<actions::Action> const& actions);
    bool executeAction(actions::Action const& action);
    void spawnNewTerminal(std::string const& profileName);
    void activateProfile(std::string const& newProfileName);
    bool reloadConfigWithProfile(std::string const& profileName);
    bool resetConfig();
    void followHyperlink(vtbackend::HyperlinkInfo const& hyperlink);
    void setFontSize(text::font_size size);
    void setDefaultCursor();
    void configureTerminal();
    void configureCursor(config::CursorConfig const& cursorConfig);
    uint8_t matchModeFlags() const;
    void flushInput();
    void mainLoop();

    // private data
    //
    int _id;
    std::chrono::steady_clock::time_point _startTime;
    config::Config _config;
    std::string _profileName;
    config::TerminalProfile _profile;
    double _contentScale = 1.0;
    ContourGuiApp& _app;
    vtbackend::ColorPreference _currentColorPreference;

    int _accumulatedScrollX;
    int _accumulatedScrollY;

    vtbackend::Terminal _terminal;
    bool _terminatedAndWaitingForKeyPress = false;
    display::TerminalDisplay* _display = nullptr;

    std::unique_ptr<QFileSystemWatcher> _configFileChangeWatcher;

    bool _terminating = false;
    std::thread::id _mainLoopThreadID {};
    std::unique_ptr<std::thread> _screenUpdateThread;

    // state vars
    //
    vtbackend::ScreenType _currentScreenType = vtbackend::ScreenType::Primary;
    vtbackend::CellLocation _currentMousePosition = vtbackend::CellLocation {};
    bool _allowKeyMappings = true;
    Audio _audio;
    std::vector<int> _musicalNotesBuffer;

    vtbackend::LineCount _lastHistoryLineCount;

    struct CaptureBufferRequest
    {
        vtbackend::LineCount lines;
        bool logical;
    };
    std::optional<CaptureBufferRequest> _pendingBufferCapture;
    std::optional<vtbackend::FontDef> _pendingFontChange;
    PermissionCache _rememberedPermissions;
    std::unique_ptr<QThread> _exitWatcherThread;

    std::atomic<bool> _onClosedHandled = false;
    std::mutex _onClosedMutex;
};

} // namespace contour

Q_DECLARE_INTERFACE(contour::TerminalSession, "org.contour.TerminalSession")

namespace fmt
{

template <>
struct formatter<contour::GuardedRole>
{
    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }

    template <typename FormatContext>
    auto format(contour::GuardedRole value, FormatContext& ctx)
    {
        switch (value)
        {
                // clang-format off
            case contour::GuardedRole::ChangeFont: return fmt::format_to(ctx.out(), "Change Font");
            case contour::GuardedRole::CaptureBuffer: return fmt::format_to(ctx.out(), "Capture Buffer");
            case contour::GuardedRole::ShowHostWritableStatusLine:  return fmt::format_to(ctx.out(), "show Host Writable Statusline");
                // clang-format on
        }
        crispy::unreachable();
    }
};

} // namespace fmt
