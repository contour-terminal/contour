// SPDX-License-Identifier: Apache-2.0
#include <contour/Actions.h>
#include <contour/BlurBehind.h>
#include <contour/ContourGuiApp.h>
#include <contour/display/OpenGLRenderer.h>
#include <contour/display/TerminalDisplay.h>
#include <contour/display/TerminalRenderNode.h>
#include <contour/helper.h>

#include <vtbackend/Color.h>
#include <vtbackend/Metrics.h>

#include <vtpty/Pty.h>

#include <crispy/App.h>
#include <crispy/logstore.h>
#include <crispy/utils.h>

#include <QtCore/QDebug>
#include <QtCore/QFileInfo>
#include <QtCore/QFileSystemWatcher>
#include <QtCore/QMetaEnum>
#include <QtCore/QMetaObject>
#include <QtCore/QProcess>
#include <QtCore/QRunnable>
#include <QtCore/QStandardPaths>
#include <QtCore/QTimer>
#include <QtGui/QClipboard>
#include <QtGui/QDesktopServices>
#include <QtGui/QGuiApplication>
#include <QtGui/QKeyEvent>
#include <QtGui/QScreen>
#include <QtNetwork/QHostInfo>
#include <QtQml/QQmlContext>
#include <QtQuick/QQuickWindow>

#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <ranges>
#include <string_view>
#include <tuple>
#include <variant>
#include <vector>

namespace fs = std::filesystem;

// Temporarily disabled (I think it was macOS that didn't like glDebugMessageCallback).
// #define CONTOUR_DEBUG_OPENGL 1

#if defined(_MSC_VER)
    #define __PRETTY_FUNCTION__ __FUNCDNAME__
#endif

// Must be in global namespace
// NB: must be publicly visible, and due to -Wmissing-declarations, we better tell the compiler.
void initializeResourcesForContourFrontendOpenGL();

void initializeResourcesForContourFrontendOpenGL()
{
    Q_INIT_RESOURCE(DisplayResources);
}

namespace contour::display
{

using vtbackend::Height;
using vtbackend::ImageSize;
using vtbackend::Width;

using vtbackend::ColumnCount;
using vtbackend::LineCount;
using vtbackend::PageSize;
using vtbackend::RGBAColor;

using text::DPI;

using namespace std::string_literals;
using namespace std::string_view_literals;
using namespace std;

using std::chrono::steady_clock;

using actions::Action;

// {{{ helper
namespace
{
#if !defined(NDEBUG) && defined(GL_DEBUG_OUTPUT) && defined(CONTOUR_DEBUG_OPENGL)
    void glMessageCallback(GLenum _source,
                           GLenum _type,
                           [[maybe_unused]] GLuint _id,
                           GLenum _severity,
                           [[maybe_unused]] GLsizei _length,
                           GLchar const* _message,
                           [[maybe_unused]] void const* _userParam)
    {
        string const sourceName = [&]() {
            switch (_source)
            {
    #if defined(GL_DEBUG_SOURCE_API_ARB)
                case GL_DEBUG_SOURCE_API_ARB: return "API"s;
    #endif
    #if defined(GL_DEBUG_SOURCE_WINDOW_SYSTEM_ARB)
                case GL_DEBUG_SOURCE_WINDOW_SYSTEM_ARB: return "window system"s;
    #endif
    #if defined(GL_DEBUG_SOURCE_SHADER_COMPILER_ARB)
                case GL_DEBUG_SOURCE_SHADER_COMPILER_ARB: return "shader compiler"s;
    #endif
    #if defined(GL_DEBUG_SOURCE_THIRD_PARTY_ARB)
                case GL_DEBUG_SOURCE_THIRD_PARTY_ARB: return "third party"s;
    #endif
    #if defined(GL_DEBUG_SOURCE_APPLICATION_ARB)
                case GL_DEBUG_SOURCE_APPLICATION_ARB: return "application"s;
    #endif
    #if defined(GL_DEBUG_SOURCE_OTHER_ARB)
                case GL_DEBUG_SOURCE_OTHER_ARB: return "other"s;
    #endif
                default: return std::format("{}", _severity);
            }
        }();
        string const typeName = [&]() {
            switch (_type)
            {
    #if defined(GL_DEBUG_TYPE_ERROR)
                case GL_DEBUG_TYPE_ERROR: return "error"s;
    #endif
    #if defined(GL_DEBUG_TYPE_DEPRECATED_BEHAVIOR)
                case GL_DEBUG_TYPE_DEPRECATED_BEHAVIOR: return "deprecated"s;
    #endif
    #if defined(GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR)
                case GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR: return "undefined"s;
    #endif
    #if defined(GL_DEBUG_TYPE_PORTABILITY)
                case GL_DEBUG_TYPE_PORTABILITY: return "portability"s;
    #endif
    #if defined(GL_DEBUG_TYPE_PERFORMANCE)
                case GL_DEBUG_TYPE_PERFORMANCE: return "performance"s;
    #endif
    #if defined(GL_DEBUG_TYPE_OTHER)
                case GL_DEBUG_TYPE_OTHER: return "other"s;
    #endif
                default: return std::format("{}", _severity);
            }
        }();
        string const debugSeverity = [&]() {
            switch (_severity)
            {
    #if defined(GL_DEBUG_SEVERITY_LOW)
                case GL_DEBUG_SEVERITY_LOW: return "low"s;
    #endif
    #if defined(GL_DEBUG_SEVERITY_MEDIUM)
                case GL_DEBUG_SEVERITY_MEDIUM: return "medium"s;
    #endif
    #if defined(GL_DEBUG_SEVERITY_HIGH)
                case GL_DEBUG_SEVERITY_HIGH: return "high"s;
    #endif
    #if defined(GL_DEBUG_SEVERITY_NOTIFICATION)
                case GL_DEBUG_SEVERITY_NOTIFICATION: return "notification"s;
    #endif
                default: return std::format("{}", _severity);
            }
        }();
        auto const tag = []([[maybe_unused]] GLint _type) {
            switch (_type)
            {
    #ifdef GL_DEBUG_TYPE_DEPRECATED_BEHAVIOR
                case GL_DEBUG_TYPE_DEPRECATED_BEHAVIOR: return "DEPRECATED";
    #endif
    #ifdef GL_DEBUG_TYPE_MARKER
                case GL_DEBUG_TYPE_MARKER: return "MARKER";
    #endif
    #ifdef GL_DEBUG_TYPE_OTHER
                case GL_DEBUG_TYPE_OTHER: return "OTHER";
    #endif
    #ifdef GL_DEBUG_TYPE_PORTABILITY
                case GL_DEBUG_TYPE_PORTABILITY: return "PORTABILITY";
    #endif
    #ifdef GL_DEBUG_TYPE_PERFORMANCE
                case GL_DEBUG_TYPE_PERFORMANCE: return "PERFORMANCE";
    #endif
    #ifdef GL_DEBUG_TYPE_ERROR
                case GL_DEBUG_TYPE_ERROR: return "ERROR";
    #endif
                default: return "UNKNOWN";
            }
        }(_type);

        displayLog()("[OpenGL/{}]: type:{}, source:{}, severity:{}; {}",
                     tag,
                     typeName,
                     sourceName,
                     debugSeverity,
                     _message);
    }
#endif

    std::string unhandledExceptionMessage(std::string_view const& where, exception const& e)
    {
        return std::format("{}: Unhandled exception caught ({}). {}", where, typeid(e).name(), e.what());
    }

    void reportUnhandledException(std::string_view const& where, exception const& e)
    {
        displayLog()("{}", unhandledExceptionMessage(where, e));
        cerr << unhandledExceptionMessage(where, e) << '\n';
    }

    // Returns the config file containing the user-configured DPI setting for KDE desktops.
    [[maybe_unused]] std::optional<fs::path> kcmFontsFilePath()
    {
#if !defined(__APPLE__) && !defined(_WIN32)
        auto const xdgConfigHome = config::configHome("");
        auto const kcmFontsFile = xdgConfigHome / "kcmfonts";
        if (fs::exists(kcmFontsFile))
            return { kcmFontsFile };
#endif

        return nullopt;
    }

    QScreen* findScreenWithBiggestWidth(QScreen* startScreen)
    {
        auto* screenToUse = startScreen;
        for (auto* screen: startScreen->virtualSiblings())
        {
            if (screen->size().width() > screenToUse->size().width())
            {
                screenToUse = screen;
            }
        }
        return screenToUse;
    }

} // namespace
// }}}

// {{{ Display creation and QQuickItem overides
TerminalDisplay::TerminalDisplay(QQuickItem* parent):
    QQuickItem(parent),
    _startTime { steady_clock::time_point::min() },
    _lastFontDPI { fontDPI() },
    _updateTimer(this),
    _autoScrollTimer(this),
    _filesystemWatcher(this)
{
    startupLog()("TerminalDisplay constructed (QML component instantiation reached)");
    auto const timer = ScopedTimer(startupLog, "TerminalDisplay constructor");
    initializeResourcesForContourFrontendOpenGL();

    setFlag(Flag::ItemIsFocusScope);
    setFlag(Flag::ItemHasContents);
    setAcceptedMouseButtons(Qt::MouseButton::AllButtons);
    setAcceptHoverEvents(true);

#if QT_CONFIG(im)
    setFlag(Flag::ItemAcceptsInputMethod);
    // updateInputMethod(Qt::ImEnabled | Qt::ImCursorRectangle | Qt::ImFont | Qt::ImAnchorRectangle |
    // Qt::ImHints);
    updateInputMethod(Qt::ImQueryAll);
#endif

    connect(this, &QQuickItem::windowChanged, this, &TerminalDisplay::handleWindowChanged);

    // setMouseTracking(true);
    // setFormat(createSurfaceFormat());
    //
    // TODO: setAttribute(Qt::WA_InputMethodEnabled, true);

    _updateTimer.setSingleShot(true);
    connect(&_updateTimer, &QTimer::timeout, this, &TerminalDisplay::scheduleRedraw, Qt::QueuedConnection);

    _autoScrollTimer.setInterval(50);
    connect(
        &_autoScrollTimer, &QTimer::timeout, this, &TerminalDisplay::onAutoScrollTick, Qt::QueuedConnection);
}

TerminalDisplay::~TerminalDisplay()
{
    displayLog()("Destroying terminal widget.");
    // Evict this display from the manager's per-display bookkeeping before it is freed, so no
    // dangling TerminalDisplay* key (or its dangling currentSession) survives in _displayStates. Use
    // the cached manager rather than _session->getTerminalManager(): a closed split pane is destroyed
    // after its session was already detached (_session == nullptr), so the session route would miss it.
    if (_manager != nullptr)
        _manager->detachDisplay(this);
    if (_session)
        _session->detachDisplay(*this);
}

void TerminalDisplay::setSession(TerminalSession* newSession)
{
    displayLog()("TerminalDisplay::setSession: {} -> {}\n", (void*) _session, (void*) newSession);
    if (_session == newSession)
        return;

    // This will print the same pointer address for `this` but a new one for newSession (model data).
    displayLog()("Assigning session to display({} <- {}): shell={}, terminalSize={}, fontSize={}, "
                 "contentScale={}",
                 (void const*) this,
                 (void const*) newSession,
                 newSession->profile().ssh.value().hostname.empty()
                     ? std::format("program={}", newSession->profile().shell.value().program)
                     : std::format("{}@{}:{}",
                                   newSession->profile().ssh.value().username,
                                   newSession->profile().ssh.value().hostname,
                                   newSession->profile().ssh.value().port),
                 newSession->profile().terminalSize.value(),
                 newSession->profile().fonts.value().size,
                 contentScale());

    if (_session)
    {
        QObject::disconnect(_session, &TerminalSession::titleChanged, this, &TerminalDisplay::titleChanged);
        _session->detachDisplay(*this);
    }

    _session = newSession;

    // Cache the manager so ~TerminalDisplay can self-evict from _displayStates even if this pane is
    // closed before it ever receives focus (focus-in is the other place the cache is set).
    if (auto* manager = newSession->getTerminalManager(); manager != nullptr)
        _manager = manager;

    QObject::connect(newSession, &TerminalSession::titleChanged, this, &TerminalDisplay::titleChanged);

    auto const imeEnabled = profile().inputMethodEditor.value();
    setFlag(Flag::ItemAcceptsInputMethod, imeEnabled);
    displayLog()("IME enabled: {}", imeEnabled);

    {
        auto const timer = ScopedTimer(startupLog, "Session start");
        _session->start();
    }

    // NB: The window frame is owned by QML now. main.qml makes the window frameless on the platforms
    // that use the custom client-side TitleBar (the tab strip + window controls). The profile's
    // show_title_bar setting controls the *custom* bar's visibility (not the native frame, which
    // would otherwise double-decorate); main.qml binds the TitleBar's visibility to titleBarVisible.
    setTitleBarVisible(profile().showTitleBar.value());

    if (!_renderer)
    {
        auto const timer = ScopedTimer(startupLog, "Renderer construction");
        _renderer = make_unique<vtrasterizer::Renderer>(
            _session->profile().terminalSize.value(),
            sanitizeFontDescription(profile().fonts.value(), fontDPI()),
            _session->terminal().colorPalette(),
            _session->config().renderer.value().textureAtlasHashtableSlots,
            _session->config().renderer.value().textureAtlasTileCount,
            _session->config().renderer.value().textureAtlasDirectMapping,
            _session->profile().hyperlinkDecoration.value().normal,
            _session->profile().hyperlinkDecoration.value().hover
            // TODO: , WindowMargin(windowMargin_.left, windowMargin_.bottom);
        );

        // setup once with the renderer creation
        applyFontDPI();
        updateImplicitSize();
        updateSizeConstraints();
    }

    _session->attachDisplay(*this); // NB: Requires Renderer to be instanciated to retrieve grid metrics.

    _session->terminal().setImageDecoder(
        [](vtbackend::ImageFormat format,
           std::span<uint8_t const> data,
           vtbackend::ImageSize& size) -> std::optional<vtbackend::Image::Data> {
            if (format != vtbackend::ImageFormat::PNG)
                return std::nullopt;

            QImage image;
            image.loadFromData(static_cast<uchar const*>(data.data()), static_cast<int>(data.size()));
            if (image.isNull())
                return std::nullopt;

            image = image.convertToFormat(QImage::Format_RGBA8888);

            size = vtbackend::ImageSize { vtbackend::Width::cast_from(image.width()),
                                          vtbackend::Height::cast_from(image.height()) };

            auto const rowBytes = static_cast<size_t>(image.width()) * 4;
            vtbackend::Image::Data pixels;
            pixels.resize(static_cast<size_t>(image.height()) * rowBytes);
            auto* p = pixels.data();
            for (auto const row: std::views::iota(0, image.height()))
            {
                memcpy(p, image.constScanLine(row), rowBytes);
                p += rowBytes;
            }
            return pixels;
        });

    emit sessionChanged(newSession);
}

void TerminalDisplay::releaseSession()
{
    if (!_session)
        return;
    displayLog()("TerminalDisplay::releaseSession: dropping session {} (taken over by another display)",
                 (void*) _session);
    QObject::disconnect(_session, &TerminalSession::titleChanged, this, &TerminalDisplay::titleChanged);
    _session = nullptr;
    emit sessionChanged(nullptr);
}

vtbackend::PageSize TerminalDisplay::windowSize() const noexcept
{
    if (!_session)
        return vtbackend::PageSize { LineCount(25), ColumnCount(80) };

    return profile().terminalSize.value();
}

void TerminalDisplay::sizeChanged()
{
    if (!_session || !_renderTarget)
        return;

    if (width() == 0.0 || height() == 0.0)
        // This can happen when the window is minimized, or when the window is not yet fully initialized.
        return;

    // During initial display setup, the Wayland compositor may revert the window to
    // the stale pre-DPR-correction geometry via a configure event (e.g. in response to
    // showNormal()). Detect this by checking if BOTH dimensions match the saved stale
    // size (the window dimensions before DPR correction in createRenderer). This is more
    // specific than checking against implicitSize, which would also falsely trigger on
    // intentional WM-driven resizes (e.g. tiling WM assigning the window a tile size).
    //
    // Crucially, only treat the saved size as "stale" if the implicit size has
    // actually changed since createRenderer() — i.e. a DPR correction really
    // happened. Without that check, a tiling WM that maps the window directly at
    // its tile size (so width() == _lastVirtual* with no DPR correction) is
    // wrongly "corrected" down to the implicit 80x25, leaving a tiny near-square
    // window until a manual resize.
    auto const implicitSizeChanged = std::abs(implicitWidth() - _initialImplicitWidth) > 0.5
                                     || std::abs(implicitHeight() - _initialImplicitHeight) > 0.5;
    if (steady_clock::now() < _initialResizeDeadline && implicitSizeChanged
        && std::abs(width() - _lastVirtualWidth) <= 0.5 && std::abs(height() - _lastVirtualHeight) <= 0.5)
    {
        displayLog()("Correcting initial window size from {}x{} (stale) to {}x{} (implicit)",
                     width(),
                     height(),
                     implicitWidth(),
                     implicitHeight());
        post([this]() {
            if (auto* currentWindow = window(); currentWindow)
            {
                // Add the chrome offset (window minus this item) so the correction targets the
                // window size, not just the terminal's — otherwise it drops the title-bar height.
                // Ceil the (possibly fractional) implicit terminal size, then add the whole-pixel
                // chrome offset from the shared helper so this path rounds chrome the same way the
                // snap and size-constraint paths do.
                auto const chrome = chromeSize();
                currentWindow->resize(static_cast<int>(std::ceil(implicitWidth())) + chrome.width(),
                                      static_cast<int>(std::ceil(implicitHeight())) + chrome.height());
            }
        });
        return;
    }

    displayLog()("Size changed to {}x{} virtual", width(), height());

    auto const virtualSize = vtbackend::ImageSize { Width::cast_from(width()), Height::cast_from(height()) };
    auto const actualPixelSize = virtualSize * contentScale();
    displayLog()("Resizing view to {} virtual ({} actual).", virtualSize, actualPixelSize);
    applyResize(actualPixelSize, *_session, *_renderer);

    // Client-side snap to cell-grid boundaries.
    // On X11/macOS, setSizeIncrement() handles this in the WM — the snap is a no-op.
    // On Wayland (xdg-shell has no size-increment hint), this is the only mechanism.
    if (!_snapPending && !isFullScreen() && window()->visibility() != QQuickWindow::Visibility::Maximized
        && steady_clock::now() >= _initialResizeDeadline)
    {
        _snapPending = true;
        post([this]() {
            if (!_session || !_renderTarget || !window())
            {
                _snapPending = false;
                return;
            }
            if (isFullScreen() || window()->visibility() == QQuickWindow::Visibility::Maximized)
            {
                _snapPending = false;
                return;
            }

            auto const dpr = contentScale();
            auto const snappedActualSize = pixelSize();
            auto const snappedVirtualWidth =
                static_cast<int>(std::ceil(static_cast<double>(unbox(snappedActualSize.width)) / dpr));
            auto const snappedVirtualHeight =
                static_cast<int>(std::ceil(static_cast<double>(unbox(snappedActualSize.height)) / dpr));

            // The display item may not fill the whole window: a custom title bar (and any other
            // chrome) sits outside it. Snapping resizes the *window*, so add back the chrome offset
            // (window size minus this item's size) — otherwise each snap drops the chrome height and
            // the window shrinks on every frame until the terminal collapses.
            auto const chrome = chromeSize();
            auto const chromeWidth = chrome.width();
            auto const chromeHeight = chrome.height();

            auto const targetWidth = snappedVirtualWidth + chromeWidth;
            auto const targetHeight = snappedVirtualHeight + chromeHeight;

            auto const currentWidth = window()->width();
            auto const currentHeight = window()->height();

            if (targetWidth != currentWidth || targetHeight != currentHeight)
            {
                displayLog()("Snapping window from {}x{} to {}x{} virtual (grid-aligned, actual {}, "
                             "chrome {}x{})",
                             currentWidth,
                             currentHeight,
                             targetWidth,
                             targetHeight,
                             snappedActualSize,
                             chromeWidth,
                             chromeHeight);
                window()->resize(targetWidth, targetHeight);
            }
            _snapPending = false;
        });
    }
}

void TerminalDisplay::handleWindowChanged(QQuickWindow* newWindow)
{
    if (newWindow)
    {
        displayLog()("Attaching widget {} to window {}.", (void*) this, (void*) newWindow);
        connect(newWindow,
                &QQuickWindow::sceneGraphInitialized,
                this,
                &TerminalDisplay::onSceneGrapheInitialized,
                Qt::DirectConnection);

        connect(newWindow,
                &QQuickWindow::beforeSynchronizing,
                this,
                &TerminalDisplay::onBeforeSynchronize,
                Qt::DirectConnection);

        connect(newWindow,
                &QQuickWindow::sceneGraphInvalidated,
                this,
                &TerminalDisplay::cleanup,
                Qt::DirectConnection);

        connect(this, &QQuickItem::widthChanged, this, &TerminalDisplay::sizeChanged, Qt::DirectConnection);
        connect(this, &QQuickItem::heightChanged, this, &TerminalDisplay::sizeChanged, Qt::DirectConnection);

        // setSession() may have run before a window existed (window() was null), so the macOS native
        // frame for show_title_bar could not be applied then. Re-assert it now that we have a window.
        applyNativeTitleBar(_titleBarVisible);
    }
    else
        displayLog()("Detaching widget {} from window.", (void*) this);
}

class CleanupJob: public QRunnable
{
  public:
    explicit CleanupJob(OpenGLRenderer* renderer): _renderer { renderer } {}

    void run() override
    {
        delete _renderer;
        _renderer = nullptr;
    }

  private:
    OpenGLRenderer* _renderer;
};

void TerminalDisplay::releaseResources()
{
    displayLog()("Releasing resources.");
    // QQuickItem::releaseResources() runs on the GUI thread, but GL teardown must happen on the render
    // thread with the context current — so defer it via a render job. The renderer may already be gone
    // (the scene-graph node's releaseResources() destroys it on the render thread), in which case there
    // is nothing to schedule.
    if (_renderTarget)
    {
        window()->scheduleRenderJob(new CleanupJob(_renderTarget), QQuickWindow::BeforeSynchronizingStage);
        _renderTarget = nullptr;
    }
}

void TerminalDisplay::cleanup()
{
    displayLog()("Cleaning up.");
    destroyRenderer();
}

void TerminalDisplay::onAutoScrollTick()
{
    if (_autoScrollState.direction != 0 && _session)
        _session->performAutoScroll(_autoScrollState.direction,
                                    vtbackend::LineCount(_autoScrollState.linesPerTick));
}

void TerminalDisplay::onRefreshRateChanged()
{
    auto const rate = refreshRate();
    displayLog()("Refresh rate changed to {}.", rate.value);
    _session->terminal().setRefreshRate(rate);
}

void TerminalDisplay::configureScreenHooks()
{
    Require(window());

    QScreen* screen = window()->screen();

    connect(window(), SIGNAL(screenChanged(QScreen*)), this, SLOT(onScreenChanged()));
    connect(screen, SIGNAL(refreshRateChanged(qreal)), this, SLOT(onRefreshRateChanged()));
    connect(screen, SIGNAL(logicalDotsPerInchChanged(qreal)), this, SLOT(applyFontDPI()));
    // connect(screen, SIGNAL(physicalDotsPerInchChanged(qreal)), this, SLOT(applyFontDPI()));
}

void TerminalDisplay::onScreenChanged()
{
    displayLog()("Screen changed.");
    applyFontDPI();
}

void TerminalDisplay::applyFontDPI()
{
    auto const newFontDPI = fontDPI();
    if (newFontDPI == _lastFontDPI)
        return;

    displayLog()("Applying DPI {} (via content scale {}, {}).",
                 newFontDPI,
                 contentScale(),
                 window() ? "Window present" : "No window");

    // NB: _lastFontDPI is the dedup guard above; it is committed only once the renderer confirms it
    // actually applied the new DPI (see end of this function). Committing it here — before the staged
    // reload, which applyPendingReconfig() can fail and swallow (atlas reallocation, FreeType
    // instantiation) — would record success that never happened, and the guard above would then
    // permanently skip the retry when the same DPI is reported again, stranding the wrong DPI.

    // logDisplayInfo();

    if (!_session)
        return;

    Require(_renderer);

    // Stage a DPI-only change. setFontDPI() merges the new DPI into whatever font descriptions are
    // already staged (so a concurrent font-family change from a config reload is not clobbered), rather
    // than rebuilding the request from the live descriptions.
    _renderer->setFontDPI(newFontDPI);

    if (!_renderTarget)
    {
        // During createRenderer() the render target does not exist yet; the staged change is
        // materialized explicitly there via applyStagedReconfigDuringSetup(). With no render target
        // there is also no frame to drive. The implicit size / constraints still depend on the DPR
        // directly, so recompute those (they do not need the render target) and return.
        //
        // _lastFontDPI is deliberately NOT committed here: the staged DPI is not materialized yet (the
        // renderer still publishes the old DPI), so there is nothing to confirm. createRenderer() commits
        // it right after it materializes the staged reconfig — committing it speculatively here would
        // record a success that has not happened.
        if (window())
        {
            updateImplicitSize();
            updateSizeConstraints();
        }
        return;
    }

    // Apply the staged DPI change synchronously and re-derive geometry against the new cell size now,
    // via the shared policy used for every discrete font reconfiguration (see applyStagedFontReconfigNow).
    auto const cellSizeChanged = applyStagedFontReconfigNow();

    // The implicit (virtual) size and the WM size-increment hint derive from the DPR/contentScale, not
    // only from the cell pixel size, so a DPI change must recompute them even when the new cell metrics
    // round to the SAME pixel size (cellSizeChanged == false, so applyStagedFontReconfigNow() did not run
    // the geometry recompute). Skipping this on a fractional-scaling display left the window's virtual
    // size and resize increments computed at the old DPR. When the cell size did change,
    // applyStagedFontReconfigNow() already ran these, so only do it for the unchanged-cell-size case.
    if (!cellSizeChanged && window())
    {
        updateImplicitSize();
        updateSizeConstraints();
    }

    scheduleRedraw();

    // Commit the dedup guard only now that applyStagedReconfigDuringSetup() has run synchronously and the
    // renderer published the new DPI. If the staged reload threw (applyPendingReconfig() catches and keeps
    // the previous font), the published DPI stays old, _lastFontDPI is left unchanged, and the next
    // identical DPI signal correctly retries instead of being skipped forever. publishedFontDPI() reads
    // just the DPI under the lock (no full FontDescriptions copy).
    if (_renderer->publishedFontDPI() == newFontDPI)
        _lastFontDPI = newFontDPI;
}

void TerminalDisplay::logDisplayInfo()
{
    if (!_session)
        return;

    Require(_renderer);

    // clang-format off
    auto const fontSizeInPx = static_cast<int>(ceil((
        profile().fonts.value().size.pt / 72.0) * average(fontDPI())
    ));
    auto const normalScreenSize = vtbackend::ImageSize {
        Width::cast_from(window()->screen()->size().width()),
        Height::cast_from(window()->screen()->size().height())
    };
    auto const actualScreenSize = normalScreenSize * window()->effectiveDevicePixelRatio();
    // Snapshot the mutex-guarded grid metrics and font descriptions once each, then read fields off the
    // locals: gridMetrics()/fontDescriptions() each take _reconfigMutex and deep-copy a full struct, so
    // reading them per line (5 + 2 times) would do 7 locked deep copies for one diagnostic dump.
    auto const gm = gridMetrics();
    auto const fd = _renderer->fontDescriptions();
#if defined(CONTOUR_BUILD_TYPE)
    displayLog()("[FYI] Build type          : {}", CONTOUR_BUILD_TYPE);
#endif
    displayLog()("[FYI] Application PID     : {}", QCoreApplication::applicationPid());
    displayLog()("[FYI] Qt platform         : {}", QGuiApplication::platformName().toStdString());
    displayLog()("[FYI] Refresh rate        : {} Hz", refreshRate().value);
    displayLog()("[FYI] Screen size         : {}", actualScreenSize);
    displayLog()("[FYI] Device pixel ratio  : {}", window()->devicePixelRatio());
    displayLog()("[FYI] Effective DPR       : {}", window()->effectiveDevicePixelRatio());
    displayLog()("[FYI] Content scale       : {}", contentScale());
    displayLog()("[FYI] Font DPI            : {} ({})", fontDPI(), fd.dpi);
    displayLog()("[FYI] Font size           : {} ({} px)", fd.size, fontSizeInPx);
    displayLog()("[FYI] Cell size           : {} px", gm.cellSize);
    displayLog()("[FYI] Page size           : {}", gm.pageSize);
    displayLog()("[FYI] Font baseline       : {} px", gm.baseline);
    displayLog()("[FYI] Underline position  : {} px", gm.underline.position);
    displayLog()("[FYI] Underline thickness : {} px", gm.underline.thickness);
    // clang-format on
}

void TerminalDisplay::watchKdeDpiSetting()
{
#if defined(__unix__)
    auto const kcmFontsFile = kcmFontsFilePath();
    if (kcmFontsFile.has_value())
    {
        _filesystemWatcher.addPath(QString::fromStdString(kcmFontsFile->string()));
        connect(&_filesystemWatcher, SIGNAL(fileChanged(const QString&)), this, SLOT(onDpiConfigChanged()));
    }
#endif
}

void TerminalDisplay::onDpiConfigChanged()
{
    applyFontDPI();
    watchKdeDpiSetting(); // re-watch file
}

void TerminalDisplay::onSceneGrapheInitialized()
{
    displayLog()("onSceneGrapheInitialized ({}x{}, DPR {})", width(), height(), contentScale());

#if !defined(NDEBUG) && defined(GL_DEBUG_OUTPUT) && defined(CONTOUR_DEBUG_OPENGL)
    CHECKED_GL(glEnable(GL_DEBUG_OUTPUT));
    CHECKED_GL(glDebugMessageCallback(&glMessageCallback, this));
#endif
}

void TerminalDisplay::onBeforeSynchronize()
{
    if (!_session)
        return;

    if (width() < 1.0 || height() < 1.0)
        // e.g. this can happen when the window is not yet fully initialized
        return;

    window()->setScreen(findScreenWithBiggestWidth(window()->screen()));

    if (_sessionChanged)
    {
        _sessionChanged = false;
        createRenderer();
    }

    if (!_renderTarget)
    {
        // This is the first call, so create the renderer (on demand) now.
        createRenderer();

        // Also check if the terminal terminated faster than the frontend needed to render the first frame.
        if (terminal().device().isClosed())
            // Then we inform the session about it.
            _session->onClosed();
    }

    // The renderer is created lazily in updatePaintNode (also during this sync phase). On the very first
    // sync it may not exist yet; the initial geometry is established by createRenderer()/applyResize(), so
    // there is nothing to reconcile here yet.
    if (!_renderTarget)
        return;

    auto const geometry = itemDevicePixelGeometry();
    auto const dpr = geometry.dpr;

    // The terminal grid occupies this item's rectangle. The scene graph supplies the placement transform
    // (projection * node matrix) at render time (see renderFrame) and the render size is published from
    // updatePaintNode, so onBeforeSynchronize no longer sets any render-target geometry — it only
    // reconciles the grid page size against the actual surface below.
    auto const viewSize = ImageSize { Width::cast_from(geometry.width), Height::cast_from(geometry.height) };

    // Reconcile the terminal grid (page size) with the actual surface.
    //
    // This runs on the render thread against the item's device-pixel view size. The grid (page size) is
    // computed separately by the GUI-thread sizeChanged()/applyResize(). At initial map under a tiling WM
    // the final surface size can arrive here after sizeChanged() last ran, leaving the grid at a stale,
    // larger page size than the viewport — so the bottom rows render off-screen until a manual resize
    // nudges sizeChanged() again.
    //
    // Detect that divergence and post a sizeChanged() to the GUI thread (where the
    // resize must happen). applyResize() no-ops when the page size already matches,
    // and the per-frame cost when in sync is a single page-size comparison.
    auto const expectedPageSize = pageSizeForPixels(
        viewSize, _renderer->publishedCellSize(), applyContentScale(profile().margins.value(), dpr));
    if (expectedPageSize != _session->terminal().totalPageSize())
        post([this]() {
            if (_session && _renderTarget && window())
                sizeChanged();
        });
}

void TerminalDisplay::createRenderer()
{
    Require(!_renderTarget);
    Require(_renderer);
    Require(_session);
    Require(window());

    // Save the stale (pre-DPR-correction) window dimensions so sizeChanged() can
    // identify Wayland compositor configure reversions specifically, without
    // misidentifying intentional WM-driven resizes (e.g. tiling WM assigning a
    // tile size) as reversions.
    _lastVirtualWidth = width();
    _lastVirtualHeight = height();

    // Snapshot the implicit size before applyFontDPI() may recompute it. The
    // stale-geometry correction in sizeChanged() compares against this to tell a
    // real DPR-correction reversion (implicit size changed) from a legitimate
    // WM-assigned size that simply hasn't changed (implicit size unchanged).
    _initialImplicitWidth = implicitWidth();
    _initialImplicitHeight = implicitHeight();

    // Catch DPR corrections that occurred between setSession() and first render
    // (e.g., Qt correcting from integer-ceiling DPR=2 to actual fractional DPR=1.5
    // on KDE/Wayland). This reloads fonts at the correct DPI before creating the
    // render target, ensuring correct cell metrics from the start.
    applyFontDPI();

    // applyFontDPI() only *stages* the font reload (its no-render-target branch ran above): the render
    // thread is not running yet and there is no frame to apply it, so materialize it now to read the
    // correct cell size for the texture atlas tile. applyStagedReconfigDuringSetup() also consumes the
    // "font reconfig applied" one-shot signal raised by this setup-time apply and returns it — we
    // intentionally discard it: the geometry is sized correctly below (applyResize() + the implicit-size /
    // constraints setup), so the first painted frame must NOT also post a redundant resize/recompute that
    // would re-derive the page size on frame 1 and cause a brief geometry recompute/flicker at open.
    (void) _renderer->applyStagedReconfigDuringSetup();

    // The staged DPI is now materialized, but applyFontDPI()'s no-render-target branch could not commit its
    // _lastFontDPI dedup guard (the renderer had not yet published the new DPI when it ran). Commit it now,
    // so a later screen/DPI signal reporting this same (already-applied) DPI is correctly deduped instead
    // of triggering a full redundant font reload + geometry recompute + spurious resizeScreen()/SIGWINCH.
    _lastFontDPI = fontDPI();

    // applyFontDPI()'s no-render-target branch already computed updateImplicitSize() — but against the
    // still-stale (pre-correction) cell size, because the staged DPI was only materialized just above. On
    // a fractional-scaling display where Qt corrected the DPR between setSession() and here, that implicit
    // size is wrong, and applyResize() below derives the initial geometry from it, opening the window at
    // the wrong pixel size / column count for a frame. Recompute it now that the corrected cell size is
    // live. _initialImplicitWidth/Height (snapshotted above) intentionally keep the pre-correction values
    // for sizeChanged()'s reversion detection, so they are left untouched.
    if (window())
    {
        updateImplicitSize();
        updateSizeConstraints();
    }

    auto const textureTileSize = gridMetrics().cellSize;
    auto const viewportMargin = vtrasterizer::PageMargin {}; // TODO margin
    // The render target size is this item's device-pixel extent (the bottom-left-origin reference for the
    // inner smooth-scroll scissor); it is re-published each frame from updatePaintNode. The placement
    // within the window comes from the scene graph's transform at render time, not from a window-sized
    // surface, so only the item size is needed here.
    auto const precalculatedTargetSize = [this]() -> ImageSize {
        auto const uiSize = ImageSize { Width::cast_from(width()), Height::cast_from(height()) };
        return uiSize * contentScale();
    }();

    if (displayLog)
    {
        auto const dpr = contentScale();
        auto const viewSize =
            ImageSize { Width::cast_from(width() * dpr), Height::cast_from(height() * dpr) };
        auto const windowSize = window()->size() * dpr;
        displayLog()("Creating renderer: {}x+{}y+{}z ({} DPR, {} viewSize, {}x{} windowSize)\n",
                     x(),
                     y(),
                     z(),
                     dpr,
                     viewSize,
                     windowSize.width(),
                     windowSize.height());
    }

    _renderTarget = new OpenGLRenderer(precalculatedTargetSize, textureTileSize, viewportMargin);
    _renderer->setRenderTarget(*_renderTarget);

    // The terminal no longer paints from the window's beforeRendering/afterRendering signals (which fired
    // after the whole QML scene was composited and let the terminal overpaint popups). Rendering is now
    // driven by the scene graph through TerminalRenderNode (see updatePaintNode/renderFrame), so it
    // composites in z-order. The renderer's one-time GL initialization happens lazily on the first
    // renderFrame(), where the scene graph's GL context is current.

    configureScreenHooks();
    watchKdeDpiSetting();

    // Use implicit dimensions (correct for configured terminal size at current DPR)
    // rather than widget width()/height() which lag behind QML binding propagation
    // during beforeSynchronizing (GUI thread is blocked, bindings haven't evaluated).
    {
        auto const implicitVirtualSize = ImageSize { vtbackend::Width::cast_from(implicitWidth()),
                                                     vtbackend::Height::cast_from(implicitHeight()) };
        auto const actualPixelSize = implicitVirtualSize * contentScale();
        applyResize(actualPixelSize, *_session, *_renderer);
    }

    // Allow sizeChanged() to correct Wayland configure reversions for 500ms.
    _initialResizeDeadline = steady_clock::now() + std::chrono::milliseconds(500);

    // Defer configureDisplay() until the GUI thread processes QML binding propagation
    // and the window is committed at the correct implicit size (e.g. 1136x600 at DPR 1.5).
    // Calling it synchronously here (render thread, GUI blocked) causes setWindowNormal()
    // → showNormal() to trigger a Wayland configure event that uses the stale pre-DPR-correction
    // window geometry (e.g. 1115x585 at DPR 2.0), reverting the terminal to the wrong size.
    post([this]() { _session->configureDisplay(); });

    displayLog()("Implicit size: {}x{}", implicitWidth(), implicitHeight());
}

QMatrix4x4 TerminalDisplay::createModelMatrix() const
{
    QMatrix4x4 result;

    // Compose model matrix from our transform properties in the QML
    QQmlListProperty<QQuickTransform> transformations = const_cast<TerminalDisplay*>(this)->transform();
    auto const count = transformations.count(&transformations);
    for (int i = 0; i < count; i++)
    {
        QQuickTransform* transform = transformations.at(&transformations, i);
        transform->applyTo(&result);
    }

    return result;
}

void TerminalDisplay::prepareFrameRhi(QRhi* rhi,
                                      QRhiCommandBuffer* cb,
                                      QRhiRenderTarget* rt,
                                      QRhiRenderPassDescriptor* rpDesc,
                                      QMatrix4x4 const& itemToClip)
{
    if (!_renderTarget || rhi == nullptr || cb == nullptr || rt == nullptr || rpDesc == nullptr)
        return;

    // Lazily flag the renderer as initialized on the first prepare (sets host image-row alignment); the
    // actual RHI resources are built by createPipelines() below. This formerly lived in the renderFrame
    // initialize() call when the renderer still owned raw GL state.
    if (!_renderTarget->initialized())
    {
        logDisplayInfo();
        _renderTarget->initialize();
    }

    // Build (or reuse cached) graphics pipelines for the current render pass. prepare() runs before the
    // render pass is recording, which is the only valid point to create RHI graphics pipelines.
    _renderTarget->createPipelines(rhi, rpDesc);
    if (!_renderTarget->pipelinesReady())
        return;

    // Hand the per-frame RHI submission handles to the renderer so the staging done in paint() (terminal
    // render → execute()) queues its resource uploads onto this command buffer, before the scene graph
    // begins the render pass. The node clip (Qt's RenderState) is not available in prepare(); it is applied
    // in recordFrameRhi() at draw time. Any transient inner scissor captured during staging is stored raw
    // and intersected with the node clip there.
    _renderTarget->beginFrame(rhi, cb, rt);

    // Install the scene graph's transform: the renderer feeds item-local, top-left-origin pixel vertices
    // through this combined projection * node-matrix, so the grid lands exactly where this item sits in
    // the window — no own ortho or translation needed. The render size and model matrix were snapshotted
    // in updatePaintNode while the GUI thread was blocked.
    _renderTarget->setProjectionMatrix(itemToClip);

    // Run the terminal render (which calls the renderer's execute() one or more times to accumulate this
    // frame's geometry), then flush the accumulated vertex/atlas uploads onto the command buffer before the
    // scene graph begins the render pass.
    paint();
    _renderTarget->flushFrame();
}

void TerminalDisplay::recordFrameRhi(QSGRenderNode::RenderState const* state)
{
    if (!_renderTarget)
        return;

    // Install Qt's node clip for this frame (only available now, from the RenderState). recordDraws()
    // intersects it with each pass's transient inner scissor (smooth scroll / cursor) captured during
    // staging. std::nullopt when Qt is not clipping the node.
    if (state->scissorEnabled())
    {
        auto const r = state->scissorRect();
        _renderTarget->setNodeScissorRect(
            ScissorRect { .x = r.x(), .y = r.y(), .width = r.width(), .height = r.height() });
    }
    else
        _renderTarget->setNodeScissorRect(std::nullopt);

    // Issue the draw commands staged during prepareFrameRhi(), now that the scene graph's render pass is
    // recording. No-op if nothing was staged (e.g. pipelines were not ready in prepare()).
    _renderTarget->recordDraws();

    // Forget the node clip (member only), so a later code path never intersects against a stale rectangle.
    _renderTarget->clearNodeScissorRect();
}

void TerminalDisplay::paint()
{
    // We consider *this* the true initial start-time.
    // That shouldn't be significantly different from the object construction
    // time, but just to be sure, we'll update it here.
    if (_startTime == steady_clock::time_point::min())
        _startTime = steady_clock::now();

    if (!_renderTarget)
        return;

    try
    {
        [[maybe_unused]] auto const lastState = _state.fetchAndClear();

#if defined(CONTOUR_PERF_STATS)
        {
            ++renderCount_;
            auto const updateCount = stats_.updatesSinceRendering.exchange(0);
            auto const renderCount = stats_.consecutiveRenderCount.exchange(0);
            if (displayLog)
                displayLog()("paintGL/{}: {} renders, {} updates since last paint ({}/{}).",
                             renderCount_.load(),
                             renderCount,
                             updateCount,
                             lastState,
                             to_string(_session->terminal().renderBufferState()));
        }
#endif

        terminal().tick(steady_clock::now());

        auto const fontReconfigApplied = _renderer->render(terminal(), _renderingPressure);

        // The lazily-applied font/DPI change made the cell size current only now; re-derive page size,
        // implicit size (a DPI change alters it) and constraints against it (the triggering resize didn't).
        // render() consumes the "font reconfig applied" signal under _applyMutex and returns it, so this
        // render-thread path and the GUI-thread applyStagedReconfigNow() can never both consume it.
        //
        // The recompute must run on the GUI thread (it mutates Qt window state and resizes the terminal),
        // so it is post()ed and applied on frame N+1: for this one frame the new cell size is rendered
        // against the previous page size/margins. That single-frame divergence is an accepted trade-off
        // of keeping all grid-metrics mutation on the render thread (the deferral that fixed the
        // resize-time crashes); applying it inline here would touch Qt/terminal state off the GUI thread.
        if (fontReconfigApplied)
            post([this]() {
                // The display may be torn down between this post() (render thread) and its execution
                // (GUI thread): the session/renderer can be cleared and, crucially, the window may be
                // gone. recomputeGeometryAfterFontReconfig() requires a live window (Require(window())
                // inside it would std::abort()), so bail out unless the display is still fully alive.
                if (!_session || !_renderer || !window())
                    return;
                recomputeGeometryAfterFontReconfig();
            });

        if (_doDumpState)
        {
            doDumpStateInternal();
            _doDumpState = false;
        }

        if (_saveScreenshot)
        {
            std::visit(crispy::overloaded { [&](const std::filesystem::path& path) {
                                               screenshot().save(QString::fromStdString(path.string()));
                                           },
                                            [&](std::monostate) {
                                                if (QClipboard* clipboard = QGuiApplication::clipboard();
                                                    clipboard != nullptr)
                                                    clipboard->setImage(screenshot(), QClipboard::Clipboard);
                                            } },
                       _saveScreenshot.value());

            _saveScreenshot = std::nullopt;
        }

        // Schedule the next frame if needed. update() (re-evaluating the scene-graph node) must run on the
        // GUI thread, so it is post()ed from this render-thread path — mirroring scheduleRedraw().
        auto const requestUpdate = [this]() {
            post([this]() { update(); });
        };

        if (!_state.finish())
            requestUpdate();

        // Update the terminal's world clock, so nextRender() knows when to render next (if needed).
        terminal().tick(steady_clock::now());

        if (auto const timeoutOpt = terminal().nextRender(); timeoutOpt.has_value())
        {
            if (*timeoutOpt == chrono::milliseconds::min())
                requestUpdate();
            else
                post([this, timeout = *timeoutOpt]() { _updateTimer.start(timeout); });
        }
    }
    catch (exception const& e)
    {
        reportUnhandledException(__PRETTY_FUNCTION__, e);
    }
}

float TerminalDisplay::uptime() const noexcept
{
    using namespace std::chrono;
    auto const now = steady_clock::now();
    auto const uptimeMsecs = duration_cast<milliseconds>(now - _startTime).count();
    auto const uptimeSecs = static_cast<float>(uptimeMsecs) / 1000.0f;
    return uptimeSecs;
}

QSGNode* TerminalDisplay::updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData* /*data*/)
{
    // Runs on the render thread with the GUI thread blocked (safe to touch both). Create the OpenGL
    // renderer on demand (formerly done from the window's render signals) and the scene-graph node that
    // draws the terminal in z-order.
    if (width() < 1.0 || height() < 1.0 || !_session)
    {
        // Nothing to draw yet; drop any existing node so we recreate it once we have a size/session.
        delete oldNode;
        return nullptr;
    }

    if (!_renderTarget)
        createRenderer();

    // updatePaintNode runs during the synchronize phase with the GUI thread blocked, so this is the safe
    // point to read item geometry — the GUI thread cannot mutate it concurrently. Snapshot the item's
    // device-pixel render size here and push it to the renderer, rather than reading it from the render
    // thread in renderFrame() (where the GUI thread is unblocked). The render size is the bottom-left-origin
    // reference frame for the transient inner scissor.
    auto const geometry = itemDevicePixelGeometry();
    _renderTarget->setRenderSize(ImageSize { Width::cast_from(std::lround(geometry.width)),
                                             Height::cast_from(std::lround(geometry.height)) });

    // The model matrix (QML transforms applied to this item) is also read here while the GUI thread is
    // blocked; the scene-graph node only composes Qt's projection/node matrices on top of it at render.
    _renderTarget->setModelMatrix(createModelMatrix());

    auto* node = static_cast<TerminalRenderNode*>(oldNode);
    if (!node)
        node = new TerminalRenderNode(this);

    // Content changes every frame the terminal is dirty; mark the node so Qt invokes render() again.
    node->markDirty(QSGNode::DirtyMaterial);
    return node;
}

void TerminalDisplay::releaseRenderResources()
{
    // Called by TerminalRenderNode on the render thread as the node is torn down, where the scene graph's
    // GL context is current — the right place to free the renderer's GL resources.
    destroyRenderer();
}

void TerminalDisplay::destroyRenderer()
{
    // Single, idempotent renderer teardown shared by all entry points (the scene-graph node's
    // releaseResources, sceneGraphInvalidated → cleanup). delete of nullptr is a no-op, so calling this
    // more than once across those paths frees the renderer exactly once. Must run on the render thread
    // with the GL context current.
    delete _renderTarget;
    _renderTarget = nullptr;
}
// }}}

// {{{ Qt Display Input Event handling & forwarding
void TerminalDisplay::keyPressEvent(QKeyEvent* keyEvent)
{
    sendKeyEvent(keyEvent,
                 keyEvent->isAutoRepeat() ? vtbackend::KeyboardEventType::Repeat
                                          : vtbackend::KeyboardEventType::Press,
                 *_session);
}

void TerminalDisplay::keyReleaseEvent(QKeyEvent* keyEvent)
{
    if (keyEvent->isAutoRepeat())
        return;
    sendKeyEvent(keyEvent, vtbackend::KeyboardEventType::Release, *_session);
}

void TerminalDisplay::wheelEvent(QWheelEvent* event)
{
    sendWheelEvent(event, *_session);
}

void TerminalDisplay::mousePressEvent(QMouseEvent* event)
{
    sendMousePressEvent(event, *_session);
}

void TerminalDisplay::mouseMoveEvent(QMouseEvent* event)
{
    sendMouseMoveEvent(event, *_session);

    // Start, update, or stop auto-scroll based on whether the mouse is outside the content area
    // while the left button is pressed (i.e., during a drag-selection).
    if (event->buttons() & Qt::LeftButton)
    {
        _autoScrollState = computeAutoScrollInfo(event, *_session);
        if (_autoScrollState.direction != 0)
        {
            if (!_autoScrollTimer.isActive())
                _autoScrollTimer.start();
        }
        else
        {
            _autoScrollTimer.stop();
        }
    }
}

void TerminalDisplay::hoverMoveEvent(QHoverEvent* event)
{
    QQuickItem::hoverMoveEvent(event);
    sendMouseMoveEvent(event, *_session);
}

void TerminalDisplay::mouseReleaseEvent(QMouseEvent* event)
{
    _autoScrollTimer.stop();
    _autoScrollState = {};
    sendMouseReleaseEvent(event, *_session);
}

void TerminalDisplay::focusInEvent(QFocusEvent* event)
{
    QQuickItem::focusInEvent(event);

    if (_session)
    {
        // Cache the manager so ~TerminalDisplay can self-evict from _displayStates even after the
        // session is gone (see the destructor). This focus-in is also where the display first enters
        // _displayStates (FocusOnDisplay), so the cache and the registration are set together.
        _manager = _session->getTerminalManager();
        _manager->FocusOnDisplay(this);
        _session->sendFocusInEvent(); // TODO: paint with "normal" colors
    }
}

void TerminalDisplay::focusOutEvent(QFocusEvent* event)
{
    QQuickItem::focusOutEvent(event);
    if (_session)
        _session->sendFocusOutEvent(); // TODO maybe paint with "faint" colors
}

#if QT_CONFIG(im)
void TerminalDisplay::inputMethodEvent(QInputMethodEvent* event)
{
    terminal().updateInputMethodPreeditString(event->preeditString().toStdString());

    if (!event->commitString().isEmpty())
    {
        assert(event->preeditString().isEmpty());
        QKeyEvent keyEvent(QEvent::KeyPress, 0, Qt::NoModifier, event->commitString());
        keyPressEvent(&keyEvent);
    }

    event->accept();
}
#endif

QVariant TerminalDisplay::inputMethodQuery(Qt::InputMethodQuery query) const
{
    // Read the published cell size once (lock-free), then reuse it: avoids three locked GridMetrics
    // copies per query (this fires on every keystroke with an IME active) and prevents a torn read where
    // the width and height could come from different published values across a concurrent font apply.
    auto const cellSize = _renderer->publishedCellSize();

    auto const getCursorPosition = [&]() -> QPoint {
        QPoint cursorPos = QPoint();
        if (terminal().isCursorInViewport())
        {
            auto const dpr = contentScale();
            auto const gridCursorPos = terminal().currentScreen().cursor().position;
            cursorPos.setX(int(unbox<double>(gridCursorPos.column) * unbox<double>(cellSize.width)));
            cursorPos.setY(int(unbox<double>(gridCursorPos.line) * unbox<double>(cellSize.height)));
            cursorPos /= dpr;
        }
        return cursorPos;
    };

    switch (query)
    {
        case Qt::ImCursorRectangle: {
            auto theContentsRect = QRect(); // TODO: contentsRect();
            auto result = QRect();
            auto const dpr = contentScale();
            auto const cursorPos = getCursorPosition();
            result.setLeft(theContentsRect.left() + cursorPos.x());
            result.setTop(theContentsRect.top() + cursorPos.y());
            result.setWidth(
                int(unbox<double>(cellSize.width) / dpr)); // TODO: respect double-width characters
            result.setHeight(int(unbox<double>(cellSize.height) / dpr));
            return result;
            break;
        }
        // TODO?: case Qt::ImCursorRectangle:
        // case Qt::ImMicroFocus: {
        //     auto const cursorPos = getCursorPosition();
        //     return imageToDisplay(QRect(cursorPos.x(), cursorPos.y(), 1, 1));
        // }
        // case Qt::ImFont:
        //     return QFont("monospace", 10);
        case Qt::ImCursorPosition:
            // return the cursor position within the current line
            return getCursorPosition().x();
        case Qt::ImSurroundingText:
            // return the text from the current line
            if (terminal().isCursorInViewport())
                return QString::fromStdString(
                    terminal().currentScreen().lineTextAt(terminal().currentScreen().cursor().position.line));

            return QString();
        case Qt::ImCurrentSelection:
            // Nothing selected.
            return QString();
        default:
            // bubble up
            break;
    }
    return QQuickItem::inputMethodQuery(query);
}

bool TerminalDisplay::event(QEvent* event)
{
    try
    {
        if (event->type() == QEvent::Close)
        {
            assert(_session);
            _session->pty().close();
            emit terminated();
        }

        return QQuickItem::event(event);
    }
    catch (std::exception const& e)
    {
        std::cout << std::format("Unhandled exception for event {}: {}\n",
                                 (unsigned) event->type(),
                                 QMetaEnum::fromType<QEvent::Type>().valueToKey(event->type()));
        reportUnhandledException(__PRETTY_FUNCTION__, e);
        return false;
    }
}
// }}}

// {{{ helpers
void TerminalDisplay::onScrollBarValueChanged(int value)
{
    terminal().viewport().scrollTo(vtbackend::ScrollOffset::cast_from(value));
    scheduleRedraw();
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
std::optional<double> TerminalDisplay::queryContentScaleOverride() const
{
#if !defined(__APPLE__) && !defined(_WIN32)
    auto const kcmFontsFile = kcmFontsFilePath();
    if (!kcmFontsFile)
        return std::nullopt;

    auto const contents = crispy::readFileAsString(*kcmFontsFile);
    for (auto const line: crispy::split(contents, '\n'))
    {
        auto const fields = crispy::split(line, '=');
        if (fields.size() == 2 && fields[0] == "forceFontDPI"sv)
        {
            auto const forcedDPI = static_cast<double>(crispy::to_integer(fields[1]).value_or(0.0));
            if (forcedDPI >= 96.0)
            {
                auto const dpr = forcedDPI / 96.0;
                if (_lastReportedContentScale.value_or(0.0) != dpr)
                {
                    _lastReportedContentScale = dpr;
                    displayLog()("Forcing DPI to {} (DPR {}) as read from config file {}.",
                                 forcedDPI,
                                 dpr,
                                 kcmFontsFile.value().string());
                }
                return dpr;
            }
        }
    }
#endif
    return std::nullopt;
}

double TerminalDisplay::contentScale() const
{
    if (auto const contentScaleOverride = queryContentScaleOverride())
        return *contentScaleOverride;

    if (!window())
        return 1.0;

    return window()->devicePixelRatio();
}

/// Computes the required size of the widget to fit the given terminal size.
///
/// @param terminalSize the terminal size in rows and columns
/// @param cellSize     the size of a single cell in pixels (with content scale already applied)
constexpr ImageSize computeRequiredSize(config::WindowMargins margins,
                                        ImageSize cellSize,
                                        PageSize totalPageSize) noexcept
{
    // We multiply by 2 because the margins are applied to both sides of the terminal.
    auto const marginSize = ImageSize { vtbackend::Width::cast_from(unbox(margins.horizontal) * 2),
                                        vtbackend::Height::cast_from(unbox(margins.vertical) * 2) };

    return (cellSize * totalPageSize + marginSize);
}

void TerminalDisplay::updateImplicitSize()
{
    assert(_renderer);
    assert(_session);
    assert(window());

    auto const totalPageSize = _session->terminal().totalPageSize();
    auto const dpr = contentScale();

    auto const actualGridCellSize = _renderer->publishedCellSize();
    auto const scaledMargins = applyContentScale(_session->profile().margins.value(), dpr);

    // Compute the required size in actual pixels using exact integer arithmetic,
    // then convert to virtual pixels. This avoids compounding ceil-per-cell rounding errors
    // that would otherwise cause extra pixels and wrong column/line counts at fractional DPR.
    auto const actualRequiredSize = computeRequiredSize(scaledMargins, actualGridCellSize, totalPageSize);

    auto const virtualWidth = std::ceil(static_cast<double>(unbox(actualRequiredSize.width)) / dpr);
    auto const virtualHeight = std::ceil(static_cast<double>(unbox(actualRequiredSize.height)) / dpr);

    displayLog()("Implicit display size set to {}x{} (actualRequired: {}, cellSize: {}, contentScale: {}, "
                 "pageSize: {})",
                 virtualWidth,
                 virtualHeight,
                 actualRequiredSize,
                 actualGridCellSize,
                 dpr,
                 totalPageSize);

    setImplicitWidth(virtualWidth);
    setImplicitHeight(virtualHeight);
}

TerminalDisplay::DevicePixelGeometry TerminalDisplay::itemDevicePixelGeometry() const
{
    // The item's device-pixel extent. Scene position is no longer needed: the scene graph supplies the
    // item→clip transform at render time (see renderFrame), so callers want only the dpr and the size.
    auto const dpr = contentScale();
    return DevicePixelGeometry {
        .dpr = dpr,
        .width = width() * dpr,
        .height = height() * dpr,
    };
}

QSize TerminalDisplay::chromeSize() const noexcept
{
    auto const* currentWindow = window();
    if (currentWindow == nullptr)
        return QSize(0, 0);

    // Whole-pixel, >= 0 chrome offset. Using a single helper keeps every caller's rounding identical
    // (lround), so the snap, the min/base-size constraint and the initial size correction can never
    // disagree by a pixel and slowly shrink the window in only one of those paths.
    auto const chromeWidth = std::max(0, static_cast<int>(std::lround(currentWindow->width() - width())));
    auto const chromeHeight = std::max(0, static_cast<int>(std::lround(currentWindow->height() - height())));
    return QSize(chromeWidth, chromeHeight);
}

void TerminalDisplay::updateSizeConstraints()
{
    Require(window());
    Require(_renderer);
    assert(_session);

    auto const dpr = contentScale();
    auto const actualCellSize = _renderer->publishedCellSize();
    auto const margins = _session->profile().margins.value();

    // The display item may not fill the whole window (a custom title bar / chrome sits outside it).
    // All window-level constraints below describe the *window*, so add the chrome offset (window
    // size minus this item's size) to the terminal-derived minimum and base sizes — otherwise the
    // WM could shrink the window until the terminal area has zero rows.
    auto const chrome = chromeSize();
    auto const chromeWidth = chrome.width();
    auto const chromeHeight = chrome.height();

    // Minimum size (existing logic, plus chrome offset)
    auto constexpr MinimumTotalPageSize = PageSize { LineCount(5), ColumnCount(10) };
    auto const minimumSize = computeRequiredSize(margins, actualCellSize * (1.0 / dpr), MinimumTotalPageSize);
    window()->setMinimumSize(
        QSize(unbox<int>(minimumSize.width) + chromeWidth, unbox<int>(minimumSize.height) + chromeHeight));

    // Base size: the margin area not participating in the increment grid, plus the chrome.
    // Margins from config are in virtual pixels, applied on both sides.
    auto const baseWidth = static_cast<int>(2 * unbox(margins.horizontal)) + chromeWidth;
    auto const baseHeight = static_cast<int>(2 * unbox(margins.vertical)) + chromeHeight;
    window()->setBaseSize(QSize(baseWidth, baseHeight));

    // Size increment: virtual cell size.
    // ceil ensures the increment always covers the full cell at fractional DPR.
    auto const virtualCellWidth =
        static_cast<int>(std::ceil(static_cast<double>(unbox(actualCellSize.width)) / dpr));
    auto const virtualCellHeight =
        static_cast<int>(std::ceil(static_cast<double>(unbox(actualCellSize.height)) / dpr));
    window()->setSizeIncrement(QSize(virtualCellWidth, virtualCellHeight));

    displayLog()("Size constraints: minSize={}x{}, baseSize={}x{}, sizeIncrement={}x{} "
                 "(cellSize={}, dpr={})",
                 unbox<int>(minimumSize.width),
                 unbox<int>(minimumSize.height),
                 baseWidth,
                 baseHeight,
                 virtualCellWidth,
                 virtualCellHeight,
                 actualCellSize,
                 dpr);
}
// }}}

// {{{ TerminalDisplay: attributes
vtbackend::RefreshRate TerminalDisplay::refreshRate() const
{
    auto* const screen = window()->screen();
    if (!screen)
        return { vtbackend::RefreshRate { 30.0 } };

    auto const systemRefreshRate = vtbackend::RefreshRate { static_cast<double>(screen->refreshRate()) };
    return systemRefreshRate;
}

DPI TerminalDisplay::fontDPI() const noexcept
{
    return DPI { .x = 96, .y = 96 } * contentScale();
}

bool TerminalDisplay::isFullScreen() const
{
    return window()->visibility() == QQuickWindow::Visibility::FullScreen;
}

vtbackend::ImageSize TerminalDisplay::pixelSize() const
{
    assert(_session);
    auto const scaledWindowMargins = applyContentScale(_session->profile().margins.value(), contentScale());
    auto const scaledWindowMarginsPixels =
        vtbackend::ImageSize { Width::cast_from(unbox(scaledWindowMargins.horizontal) * 2),
                               Height::cast_from(unbox(scaledWindowMargins.vertical) * 2) };
    return _renderer->publishedCellSize() * _session->terminal().totalPageSize() + scaledWindowMarginsPixels;
}

vtbackend::ImageSize TerminalDisplay::cellSize() const
{
    // Lock-free published cell-size read; avoids taking the renderer's reconfig mutex and copying the
    // whole GridMetrics struct just to extract the cell size on these hot UI paths.
    return _renderer->publishedCellSize();
}
// }}}

// {{{ TerminalDisplay: (user requested) actions
void TerminalDisplay::post(std::function<void()> fn)
{
    postToObject(this, std::move(fn));
}

vtbackend::FontDef TerminalDisplay::getFontDef()
{
    Require(_renderer);
    return getFontDefinition(*_renderer);
}

void TerminalDisplay::copyToClipboard(std::string_view data)
{
    if (QClipboard* clipboard = QGuiApplication::clipboard(); clipboard != nullptr)
        clipboard->setText(QString::fromUtf8(data.data(), static_cast<int>(data.size())));
}

void TerminalDisplay::inspect()
{
    // Ensure we're invoked on GUI thread when calling doDumpState().
    QMetaObject::invokeMethod(this, &TerminalDisplay::doDumpState, Qt::QueuedConnection);
}

void TerminalDisplay::doDumpState()
{
    _doDumpState = true;
}

QImage TerminalDisplay::screenshot()
{
    // A one-off render for the screenshot; any font-reconfig recompute it signals is handled by the
    // regular paint() path, so the return value is intentionally ignored here.
    (void) _renderer->render(terminal(), _renderingPressure);
    auto [size, image] = _renderTarget->takeScreenshot();

    return QImage(image.data(),
                  size.width.as<int>(),
                  size.height.as<int>(),
                  QImage::Format_RGBA8888_Premultiplied)
        .transformed(QTransform().scale(1, -1));
}

void TerminalDisplay::doDumpStateInternal()
{

    auto finally = crispy::finally { [this] {
        if (_session->terminal().device().isClosed() && _session->app().dumpStateAtExit().has_value())
            _session->terminate();
    } };

    if (!QOpenGLContext::currentContext())
    {
        errorLog()("Cannot dump state: no OpenGL context available");
        return;
    }
    if (!QOpenGLContext::currentContext()->makeCurrent(window()))
    {
        errorLog()("Cannot dump state: cannot make current");
        return;
    }

    Require(_session);
    Require(_renderer);

    // clang-format off
    auto const targetBaseDir = _session->app().dumpStateAtExit().value_or(crispy::app::instance()->localStateDir() / "dump");
    auto const workDirName = fs::path(std::format("contour-dump-{:%Y-%m-%d-%H-%M-%S}", chrono::system_clock::now()));
    auto const targetDir = targetBaseDir / workDirName;
    auto const latestDirName = fs::path("latest");
    // clang-format on

    fs::create_directories(targetDir);

    if (fs::exists(targetBaseDir / latestDirName))
        fs::remove(targetBaseDir / latestDirName);

    fs::create_symlink(workDirName, targetBaseDir / latestDirName);

    displayLog()("Dumping state into directory: {}", targetDir.generic_string());

    // TODO: The above should be done from the outside and the targetDir being passed into this call.
    // TODO: maybe zip this dir in the end.

    // TODO: use this file store for everything that needs to be dumped.
    {
        auto const screenStateDump = [&]() {
            auto os = std::stringstream {};
            terminal().currentScreen().inspect("Screen state dump.", os);
            _renderer->inspect(os);
            return os.str();
        }();

        std::cout << screenStateDump;

        auto const screenStateDumpFilePath = targetDir / "screen-state-dump.vt";
        auto fs = ofstream { screenStateDumpFilePath.string(), ios::trunc };
        fs << screenStateDump;
        fs.close();
    }

    enum class ImageBufferFormat : uint8_t
    {
        RGBA,
        RGB,
        Alpha
    };

    vtrasterizer::RenderTarget& renderTarget = _renderer->renderTarget();

    do
    {
        auto infoOpt = renderTarget.readAtlas();
        if (!infoOpt.has_value())
            break;

        vtrasterizer::AtlasTextureScreenshot const& info = infoOpt.value();
        auto const fileName = targetDir / "texture-atlas-rgba.png";
        displayLog()("Saving image {} to: {}", info.size, fileName.generic_string());

        QImage(info.buffer.data(),
               info.size.width.as<int>(),
               info.size.height.as<int>(),
               QImage::Format_RGBA8888)
            .save(QString::fromStdString(fileName.generic_string()));
    } while (0);

    auto screenshotFilePath = targetDir / "screenshot.png";
    displayLog()("Saving screenshot to: {}", screenshotFilePath.generic_string());
    screenshot().save(QString::fromStdString(screenshotFilePath.string()));
}

void TerminalDisplay::resizeTerminalToDisplaySize()
{
    // Resize widget (same pixels, but adjusted terminal rows/columns and margin)
    Require(_renderer != nullptr);
    Require(_session != nullptr);

    auto const virtualDisplaySize =
        ImageSize { vtbackend::Width::cast_from(width()), vtbackend::Height::cast_from(height()) };
    auto const actualDisplaySize = virtualDisplaySize * contentScale();
    applyResize(actualDisplaySize, *_session, *_renderer);
}

void TerminalDisplay::resizeWindow(vtbackend::Width newWidth, vtbackend::Height newHeight)
{
    Require(_session != nullptr);

    if (isFullScreen())
    {
        displayLog()("Application request to resize window in full screen mode denied.");
        return;
    }

    applyResize(vtbackend::ImageSize { newWidth, newHeight }, *_session, *_renderer);
}

void TerminalDisplay::resizeWindow(vtbackend::LineCount newLineCount, vtbackend::ColumnCount newColumnCount)
{
    if (isFullScreen())
    {
        displayLog()("Application request to resize window in full screen mode denied.");
        return;
    }

    // The TUI app requests the usable area (what the PTY reports via TIOCSWINSZ),
    // not the total page size. Add back the status line height to get the total page size.
    auto requestedPageSize = terminal().totalPageSize();
    if (*newColumnCount)
        requestedPageSize.columns = newColumnCount;
    if (*newLineCount)
        requestedPageSize.lines = newLineCount + terminal().statusLineHeight();

    // Compute the target pixel size in device (scaled) pixels first, using the same
    // cell size and scaled margins that pageSizeForPixels() will use in the reverse
    // direction. This avoids cumulative rounding errors from the previous approach
    // of computing an unscaled cell size separately (which involved two std::ceil
    // operations that could inflate the pixel count).
    auto const cellSize = _renderer->publishedCellSize();
    auto const scaledMargins = applyContentScale(_session->profile().margins.value(), contentScale());
    auto const targetScaledSize = vtbackend::ImageSize {
        cellSize.width * boxed_cast<vtbackend::Width>(requestedPageSize.columns)
            + vtbackend::Width::cast_from(2 * unbox(scaledMargins.horizontal)),
        cellSize.height * boxed_cast<vtbackend::Height>(requestedPageSize.lines)
            + vtbackend::Height::cast_from(2 * unbox(scaledMargins.vertical)),
    };

    // Convert to logical (unscaled) pixels for Qt with a single ceil division.
    // sizeChanged() will multiply back by contentScale(), recovering targetScaledSize
    // (possibly +1 pixel due to ceil, which is safe: floor((target+delta)/cellSize) == M
    // as long as delta < cellSize, which holds for all reasonable contentScale values).
    auto const unscaledViewSize = targetScaledSize / contentScale();
    window()->resize(QSize(unscaledViewSize.width.as<int>(), unscaledViewSize.height.as<int>()));
}

void TerminalDisplay::recomputeGeometryAfterFontReconfig()
{
    // Re-derive the geometry that depends on the (now current) cell size after a font/DPI reconfiguration:
    // the terminal page size + render-surface margin (resizeTerminalToDisplaySize, which also drives the
    // resizeScreen()/SIGWINCH to the child) and the Qt window's implicit size + size constraints. Callers
    // must ensure the display is fully alive (live _session, _renderer and window()) — updateImplicitSize()
    // and updateSizeConstraints() Require(window()) and would abort otherwise.
    Require(_session && _renderer && window());
    resizeTerminalToDisplaySize();
    updateImplicitSize();
    updateSizeConstraints();
}

bool TerminalDisplay::applyStagedFontReconfigNow()
{
    // Apply a staged font/DPI reconfiguration synchronously, on the GUI thread, and re-derive geometry
    // against the resulting cell size in this same call — rather than deferring to a later painted frame.
    //
    // This is the single policy for ALL discrete font reconfigurations (size, family, DPI). Deferring to a
    // frame had three problems the synchronous path avoids: (1) the child process learned its new row/col
    // count one to two frames late (a TUI read of the terminal size right after the change saw stale
    // dimensions); (2) the intervening frame(s) rendered the new cell size against the previous page size,
    // a visible wrong-column-count flash; (3) it relied on a frame actually painting, so an exposed-but-
    // not-painting window (occluded, compositor withholding frames) could leave the change unapplied
    // indefinitely. Font reconfigurations are discrete user/config events, not the continuous
    // resize stream the render-thread deferral exists to protect, so applying them inline is appropriate.
    //
    // Safety: applyStagedReconfigDuringSetup() takes the renderer's _applyMutex, which renderImpl() also
    // holds across a whole frame, so this is mutually exclusive with an in-flight render-thread frame even
    // if the window has not yet observed losing exposure — it waits for that frame to finish reading the
    // grid metrics / atlas before mutating them. This is the same mechanism applyFontDPI() relied on.
    //
    // applyStagedReconfigDuringSetup() both applies the staged change and consumes the "font reconfig
    // applied" one-shot signal under _applyMutex, returning it. Consuming it there (not via a separate
    // consumeFontReconfigApplied() after the lock is released) is what prevents this GUI-thread path from
    // racing a render-thread frame for the signal.
    //
    // @return true if the cell size actually changed (a geometry recompute against it was performed). DPI
    //         changes that round to the same cell pixel size return false but still alter DPR-derived
    //         window geometry; applyFontDPI() recomputes the implicit size / size constraints itself for
    //         that case.
    auto const fontReconfigApplied = _renderer->applyStagedReconfigDuringSetup();
    if (fontReconfigApplied && _session && window())
    {
        // recomputeGeometryAfterFontReconfig() only *stages* the new geometry (page size, margin, render
        // surface) into the renderer's pending reconfig. Drain it synchronously too so it lands now —
        // including the resizeScreen()/SIGWINCH to the child — rather than waiting for a frame.
        recomputeGeometryAfterFontReconfig();
        (void) _renderer->applyStagedReconfigDuringSetup();
    }
    return fontReconfigApplied;
}

void TerminalDisplay::setFonts(vtrasterizer::FontDescriptions fontDescriptions)
{
    Require(_session != nullptr);
    Require(_renderTarget != nullptr);

    if (applyFontDescription(fontDPI(), *_renderer, std::move(fontDescriptions)))
    {
        // The font change is only *staged* (see applyFontDescription). Apply it synchronously and
        // re-derive geometry against the new cell size now; doing the recompute without the apply would
        // use the *stale* cell size.
        applyStagedFontReconfigNow();
        // logDisplayInfo();
    }
}

bool TerminalDisplay::setFontSize(text::font_size newFontSize)
{
    Require(_renderer != nullptr);

    displayLog()("Setting display font size and recompute metrics: {}pt", newFontSize.pt);

    if (!_renderer->setFontSize(newFontSize))
        return false;

    // The font change is only *staged*; apply it synchronously and re-derive the page size against the
    // new cell size now (recomputing without the apply would use the *stale* cell size).
    applyStagedFontReconfigNow();
    // logDisplayInfo();

    // Report whether the change actually took: the render-thread apply catches and swallows font-load
    // failures (keeping the previous font), so the caller must not record a size the renderer never
    // loaded. font_size has no operator==; compare the point size the apply published against the request.
    // The request is the exact value just staged (no arithmetic in between), so an exact compare is
    // correct: equal means the apply loaded it, unequal means it was swallowed.
    return _renderer->fontDescriptions().size.pt == newFontSize.pt;
}

bool TerminalDisplay::setPageSize(PageSize newPageSize)
{
    if (newPageSize == terminal().pageSize())
        return false;

    // Derive the view size from the *requested* page size (not the configured profile size), reading the
    // published cell size once (lock-free) to avoid two locked GridMetrics copies and a torn read.
    auto const cellSize = _renderer->publishedCellSize();
    auto const viewSize = ImageSize { Width(*cellSize.width * unbox<unsigned>(newPageSize.columns)),
                                      Height(*cellSize.height * unbox<unsigned>(newPageSize.lines)) };
    _renderer->setPageSize(newPageSize);
    auto const l = scoped_lock { terminal() };
    terminal().resizeScreen(newPageSize, viewSize);
    return true;
}

void TerminalDisplay::syncRendererGeometry(PageSize totalPageSize, vtbackend::ImageSize pixelSize)
{
    Require(_renderer != nullptr);
    // The caller resized the terminal directly (bypassing the renderer's geometry staging). Push the
    // *full* geometry — page size, render-surface pixel size and margin — into the renderer, not just the
    // page size: a page-size-only sync would leave the previous session's margin in the live grid metrics
    // until some later resize nudged it, so a session-switch could render with the wrong margins (bottom
    // rows mis-clipped) on a display whose first post-switch frame is delayed. The margin is derived from
    // the renderer's published cell size and the profile margins, exactly as helper::applyResize() does.
    auto const cellSize = _renderer->publishedCellSize();
    auto const margin = computeMargin(
        cellSize, totalPageSize, pixelSize, applyContentScale(profile().margins.value(), contentScale()));
    _renderer->applyResize(pixelSize, totalPageSize, margin);
}

void TerminalDisplay::setMouseCursorShape(MouseCursorShape newCursorShape)
{
    if (auto const qtShape = toQtMouseShape(newCursorShape); qtShape != cursor().shape())
        setCursor(qtShape);
}

void TerminalDisplay::setWindowFullScreen()
{
    window()->setSizeIncrement(QSize(0, 0));
    window()->showFullScreen();
}

void TerminalDisplay::setWindowMaximized()
{
    window()->setSizeIncrement(QSize(0, 0));
    window()->showMaximized();
    _maximizedState = true;
}

void TerminalDisplay::setWindowNormal()
{
    updateSizeConstraints();
    window()->showNormal();
    _maximizedState = false;
}

void TerminalDisplay::setBlurBehind(bool enable)
{
    BlurBehind::setEnabled(window(), enable);
}

void TerminalDisplay::toggleFullScreen()
{
    if (!isFullScreen())
    {
        _maximizedState = window()->visibility() == QQuickWindow::Visibility::Maximized;
        window()->setSizeIncrement(QSize(0, 0));
        window()->showFullScreen();
    }
    else if (_maximizedState)
    {
        window()->setSizeIncrement(QSize(0, 0));
        window()->showMaximized();
    }
    else
    {
        updateSizeConstraints();
        window()->showNormal();
    }
}

void TerminalDisplay::setTitleBarVisible(bool visible)
{
    if (_titleBarVisible != visible)
    {
        _titleBarVisible = visible;
        emit titleBarVisibleChanged();
    }
    // macOS keeps the native frame (see header / main.qml), so show_title_bar must drive that frame
    // there. Apply unconditionally (not only on change) so a setSession() re-init re-asserts the frame
    // even when _titleBarVisible already matched.
    applyNativeTitleBar(visible);
}

void TerminalDisplay::applyNativeTitleBar(bool visible)
{
    // show_title_bar selects the window decoration on every OS: when on, keep the native frame; when
    // off, make the window frameless so our client-side TitleBar is the only decoration. This is the
    // C++ counterpart of main.qml's `flags` binding (both keyed on the same titleBarVisible value) and
    // is what carries the runtime ToggleTitleBar action and the initial profile value through to the
    // native frame. main.qml drops our custom min/max/close controls whenever the native frame shows,
    // so the two decorations never stack.
    if (auto* w = window(); w != nullptr)
        w->setFlag(Qt::FramelessWindowHint, !visible);
}

void TerminalDisplay::toggleTitleBar()
{
    // Under client-side decoration the title bar is our custom QML TitleBar, not the OS frame. The
    // old implementation cleared Qt::FramelessWindowHint to bring the native frame back, but the
    // custom bar stayed visible (its QML flags binding never re-asserts framelessness), stacking two
    // title bars. Toggle the custom bar's visibility instead; the window stays frameless. On macOS
    // setTitleBarVisible() additionally toggles the native frame (there is no custom bar there).
    setTitleBarVisible(!titleBarVisible());
}

void TerminalDisplay::toggleInputMethodEditorHandling()
{
    auto const enabled = !static_cast<bool>(flags() & Flag::ItemAcceptsInputMethod);
    displayLog()("{} IME (input method editor) handling", enabled ? "Enabling" : "Disabling");
    setFlag(Flag::ItemAcceptsInputMethod, enabled);
}

void TerminalDisplay::setHyperlinkDecoration(vtrasterizer::Decorator normal, vtrasterizer::Decorator hover)
{
    _renderer->setHyperlinkDecoration(normal, hover);
}
// }}}

// {{{ TerminalDisplay: terminal events
void TerminalDisplay::scheduleRedraw()
{
    auto const currentHistoryLineCount = terminal().currentScreen().historyLineCount();
    if (currentHistoryLineCount != _lastHistoryLineCount)
    {
        // emit historyLineCountChanged(unbox<int>(currentHistoryLineCount));
        _lastHistoryLineCount = currentHistoryLineCount;
    }

    // QCoreApplication::postEvent(this, new QEvent(QEvent::UpdateRequest));
    if (window())
        post([this]() { window()->update(); });
}

void TerminalDisplay::renderBufferUpdated()
{
    scheduleRedraw();
}

void TerminalDisplay::closeDisplay()
{
    displayLog()("closeDisplay");
    emit terminated();
}

void TerminalDisplay::onSelectionCompleted()
{
    if (QClipboard* clipboard = QGuiApplication::clipboard(); clipboard != nullptr)
    {
        string const text = terminal().extractSelectionText();
        clipboard->setText(QString::fromUtf8(text.c_str(), static_cast<int>(text.size())),
                           QClipboard::Selection);
    }
}

void TerminalDisplay::bufferChanged(vtbackend::ScreenType type)
{
    using Type = vtbackend::ScreenType;
    switch (type)
    {
        case Type::Primary: setCursor(Qt::IBeamCursor); break;
        case Type::Alternate: setCursor(Qt::ArrowCursor); break;
    }
    emit terminalBufferChanged(type);
    // scheduleRedraw();
}

void TerminalDisplay::discardImage(vtbackend::Image const& image)
{
    _renderer->discardImage(image);
}

// }}}

} // namespace contour::display
