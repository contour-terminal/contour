// SPDX-License-Identifier: Apache-2.0
#include <contour/Actions.h>
#include <contour/BlurBehind.h>
#include <contour/ContourGuiApp.h>
#include <contour/display/OpenGLRenderer.h>
#include <contour/display/TerminalDisplay.h>
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
}

TerminalDisplay::~TerminalDisplay()
{
    displayLog()("Destroying terminal widget.");
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

    QObject::connect(newSession, &TerminalSession::titleChanged, this, &TerminalDisplay::titleChanged);

    auto const imeEnabled = profile().inputMethodEditor.value();
    setFlag(Flag::ItemAcceptsInputMethod, imeEnabled);
    displayLog()("IME enabled: {}", imeEnabled);

    {
        auto const timer = ScopedTimer(startupLog, "Session start");
        _session->start();
    }

    window()->setFlag(Qt::FramelessWindowHint, !profile().showTitleBar.value());

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

    emit sessionChanged(newSession);
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
    // showNormal()). Detect this by checking if BOTH dimensions mismatch the implicit
    // size — single-dimension mismatch indicates normal QML binding propagation
    // (which updates width and height sequentially, not atomically).
    if (steady_clock::now() < _initialResizeDeadline && std::abs(width() - implicitWidth()) > 0.5
        && std::abs(height() - implicitHeight()) > 0.5)
    {
        displayLog()("Correcting initial window size from {}x{} to {}x{}",
                     width(),
                     height(),
                     implicitWidth(),
                     implicitHeight());
        post([this]() {
            if (auto* currentWindow = window(); currentWindow)
                currentWindow->resize(static_cast<int>(std::ceil(implicitWidth())),
                                      static_cast<int>(std::ceil(implicitHeight())));
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

            auto const currentWidth = window()->width();
            auto const currentHeight = window()->height();

            if (snappedVirtualWidth != currentWidth || snappedVirtualHeight != currentHeight)
            {
                displayLog()("Snapping window from {}x{} to {}x{} virtual (grid-aligned, actual {})",
                             currentWidth,
                             currentHeight,
                             snappedVirtualWidth,
                             snappedVirtualHeight,
                             snappedActualSize);
                window()->resize(snappedVirtualWidth, snappedVirtualHeight);
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
    window()->scheduleRenderJob(new CleanupJob(_renderTarget), QQuickWindow::BeforeSynchronizingStage);
    _renderTarget = nullptr;
}

void TerminalDisplay::cleanup()
{
    displayLog()("Cleaning up.");
    delete _renderTarget;
    _renderTarget = nullptr;
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
    _lastFontDPI = newFontDPI;

    // logDisplayInfo();

    if (!_session)
        return;

    Require(_renderer);

    auto fd = _renderer->fontDescriptions();
    fd.dpi = newFontDPI;
    _renderer->setFonts(std::move(fd));

    // Recompute implicit/minimum size when font DPI changes (e.g., DPR correction
    // from 2.0 → 1.5 on KDE/Wayland with fractional scaling). The implicit size
    // does not require a render target, so this must happen before the guard below.
    if (window())
    {
        updateImplicitSize();
        updateSizeConstraints();
    }

    if (!_renderTarget)
        return;

    auto const newPixelSize =
        vtbackend::ImageSize { Width::cast_from(width()), Height::cast_from(height()) } * contentScale();

    // Apply resize on same window metrics propagates proper recalculations and repaint.
    applyResize(newPixelSize, *_session, *_renderer);
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
    displayLog()("[FYI] Font DPI            : {} ({})", fontDPI(), _renderer->fontDescriptions().dpi);
    displayLog()("[FYI] Font size           : {} ({} px)", _renderer->fontDescriptions().size, fontSizeInPx);
    displayLog()("[FYI] Cell size           : {} px", gridMetrics().cellSize);
    displayLog()("[FYI] Page size           : {}", gridMetrics().pageSize);
    displayLog()("[FYI] Font baseline       : {} px", gridMetrics().baseline);
    displayLog()("[FYI] Underline position  : {} px", gridMetrics().underline.position);
    displayLog()("[FYI] Underline thickness : {} px", gridMetrics().underline.thickness);
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

    auto const dpr = contentScale();
    auto const windowSize = window()->size() * dpr;

    auto const viewSize =
        ImageSize { Width::cast_from(windowSize.width()), Height::cast_from(windowSize.height()) };

    _renderTarget->setRenderSize(
        ImageSize { Width::cast_from(windowSize.width()), Height::cast_from(windowSize.height()) });
    _renderTarget->setModelMatrix(createModelMatrix());
    _renderTarget->setTranslation(float(x() * dpr), float(y() * dpr), float(z() * dpr));
    _renderTarget->setViewSize(viewSize);
}

void TerminalDisplay::createRenderer()
{
    Require(!_renderTarget);
    Require(_renderer);
    Require(_session);
    Require(window());

    // Catch DPR corrections that occurred between setSession() and first render
    // (e.g., Qt correcting from integer-ceiling DPR=2 to actual fractional DPR=1.5
    // on KDE/Wayland). This reloads fonts at the correct DPI before creating the
    // render target, ensuring correct cell metrics from the start.
    applyFontDPI();

    auto const textureTileSize = gridMetrics().cellSize;
    auto const viewportMargin = vtrasterizer::PageMargin {}; // TODO margin
    auto const precalculatedViewSize = [this]() -> ImageSize {
        auto const uiSize = ImageSize { Width::cast_from(width()), Height::cast_from(height()) };
        return uiSize * contentScale();
    }();
    auto const precalculatedTargetSize = [this]() -> ImageSize {
        auto const uiSize =
            ImageSize { Width::cast_from(window()->width()), Height::cast_from(window()->height()) };
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

    _renderTarget = new OpenGLRenderer(builtinShaderConfig(ShaderClass::Text),
                                       builtinShaderConfig(ShaderClass::Background),
                                       precalculatedViewSize,
                                       precalculatedTargetSize,
                                       textureTileSize,
                                       viewportMargin);
    _renderTarget->setWindow(window());
    _renderer->setRenderTarget(*_renderTarget);

    connect(window(),
            &QQuickWindow::beforeRendering,
            this,
            &TerminalDisplay::onBeforeRendering,
            Qt::ConnectionType::DirectConnection);

    // connect(window(),
    //         &QQuickWindow::beforeRenderPassRecording,
    //         this,
    //         &TerminalDisplay::paint,
    //         Qt::DirectConnection);

    connect(window(),
            &QQuickWindow::afterRendering,
            this,
            &TerminalDisplay::onAfterRendering,
            Qt::DirectConnection);

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

void TerminalDisplay::onBeforeRendering()
{
    if (_renderTarget->initialized())
        return;

    logDisplayInfo();
    _renderTarget->initialize();
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
        window()->beginExternalCommands();
        auto const _ = gsl::finally([this]() { window()->endExternalCommands(); });

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
        _renderer->render(terminal(), _renderingPressure);
        if (_doDumpState)
        {
            doDumpStateInternal();
            _doDumpState = false;
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

void TerminalDisplay::onAfterRendering()
{
    // This method is called after the QML scene has been rendered.
    // We use this to schedule the next rendering frame, if needed.
    // This signal is emitted from the scene graph rendering thread
    paint();
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

    if (!_state.finish())
    {
        if (window())
            window()->update();
    }

    // Update the terminal's world clock, such that nextRender() knows when to render next (if needed).
    terminal().tick(steady_clock::now());

    auto timeoutOpt = terminal().nextRender();
    if (!timeoutOpt.has_value())
        return;

    auto const timeout = *timeoutOpt;
    if (timeout == chrono::milliseconds::min())
    {
        if (window())
            window()->update();
    }
    else
    {
        post([this, timeout]() { _updateTimer.start(timeout); });
    }
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
}

void TerminalDisplay::hoverMoveEvent(QHoverEvent* event)
{
    QQuickItem::hoverMoveEvent(event);
    sendMouseMoveEvent(event, *_session);
}

void TerminalDisplay::mouseReleaseEvent(QMouseEvent* event)
{
    sendMouseReleaseEvent(event, *_session);
}

void TerminalDisplay::focusInEvent(QFocusEvent* event)
{
    QQuickItem::focusInEvent(event);

    if (_session)
    {
        _session->getTerminalManager()->FocusOnDisplay(this);
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
    auto const getCursorPosition = [&]() -> QPoint {
        QPoint cursorPos = QPoint();
        if (terminal().isCursorInViewport())
        {
            auto const dpr = contentScale();
            auto const gridCursorPos = terminal().currentScreen().cursor().position;
            cursorPos.setX(int(unbox<double>(gridCursorPos.column)
                               * unbox<double>(_renderer->gridMetrics().cellSize.width)));
            cursorPos.setY(int(unbox<double>(gridCursorPos.line)
                               * unbox<double>(_renderer->gridMetrics().cellSize.height)));
            cursorPos /= dpr;
        }
        return cursorPos;
    };

    switch (query)
    {
        case Qt::ImCursorRectangle: {
            auto const& gridMetrics = _renderer->gridMetrics();
            auto theContentsRect = QRect(); // TODO: contentsRect();
            auto result = QRect();
            auto const dpr = contentScale();
            auto const cursorPos = getCursorPosition();
            result.setLeft(theContentsRect.left() + cursorPos.x());
            result.setTop(theContentsRect.top() + cursorPos.y());
            result.setWidth(int(unbox<double>(gridMetrics.cellSize.width)
                                / dpr)); // TODO: respect double-width characters
            result.setHeight(int(unbox<double>(gridMetrics.cellSize.height) / dpr));
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

    auto const actualGridCellSize = _renderer->cellSize();
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

void TerminalDisplay::updateSizeConstraints()
{
    Require(window());
    Require(_renderer);
    assert(_session);

    auto const dpr = contentScale();
    auto const actualCellSize = _renderer->cellSize();
    auto const margins = _session->profile().margins.value();

    // Minimum size (existing logic, unchanged)
    auto constexpr MinimumTotalPageSize = PageSize { LineCount(5), ColumnCount(10) };
    auto const minimumSize = computeRequiredSize(margins, actualCellSize * (1.0 / dpr), MinimumTotalPageSize);
    window()->setMinimumSize(QSize(unbox<int>(minimumSize.width), unbox<int>(minimumSize.height)));

    // Base size: the margin area not participating in the increment grid.
    // Margins from config are in virtual pixels, applied on both sides.
    auto const baseWidth = static_cast<int>(2 * unbox(margins.horizontal));
    auto const baseHeight = static_cast<int>(2 * unbox(margins.vertical));
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
    return gridMetrics().cellSize * _session->terminal().totalPageSize() + scaledWindowMarginsPixels;
}

vtbackend::ImageSize TerminalDisplay::cellSize() const
{
    return gridMetrics().cellSize;
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
    _renderer->render(terminal(), _renderingPressure);
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
    auto const cellSize = gridMetrics().cellSize;
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

void TerminalDisplay::setFonts(vtrasterizer::FontDescriptions fontDescriptions)
{
    Require(_session != nullptr);
    Require(_renderTarget != nullptr);

    if (applyFontDescription(fontDPI(), *_renderer, std::move(fontDescriptions)))
    {
        // resize widget (same pixels, but adjusted terminal rows/columns and margin)
        applyResize(pixelSize(), *_session, *_renderer);
        updateSizeConstraints(); // cell size changed
        // logDisplayInfo();
    }
}

bool TerminalDisplay::setFontSize(text::font_size newFontSize)
{
    Require(_renderer != nullptr);

    displayLog()("Setting display font size and recompute metrics: {}pt", newFontSize.pt);

    if (!_renderer->setFontSize(newFontSize))
        return false;

    resizeTerminalToDisplaySize();
    updateSizeConstraints();
    // logDisplayInfo();
    return true;
}

bool TerminalDisplay::setPageSize(PageSize newPageSize)
{
    if (newPageSize == terminal().pageSize())
        return false;

    auto const viewSize = ImageSize {
        Width(*gridMetrics().cellSize.width * unbox<unsigned>(profile().terminalSize.value().columns)),
        Height(*gridMetrics().cellSize.width * unbox<unsigned>(profile().terminalSize.value().columns))
    };
    _renderer->setPageSize(newPageSize);
    auto const l = scoped_lock { terminal() };
    terminal().resizeScreen(newPageSize, viewSize);
    return true;
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

void TerminalDisplay::toggleTitleBar()
{
    auto const currentlyFrameless = (window()->flags() & Qt::FramelessWindowHint) != 0;
    _maximizedState = window()->visibility() == QQuickWindow::Visibility::Maximized;

    window()->setFlag(Qt::FramelessWindowHint, !currentlyFrameless);
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

#if defined(GOOD_IMAGE_PROTOCOL)
std::optional<vtbackend::Image> TerminalDisplay::decodeImage(std::span<uint8_t const> imageData)
{
    QImage image;
    image.loadFromData(static_cast<uchar const*>(imageData.data()), static_cast<int>(imageData.size()));

    if (image.isNull())
        return std::nullopt;

    if (image.hasAlphaChannel() && image.format() != QImage::Format_ARGB32)
        image = image.convertToFormat(QImage::Format_ARGB32);
    else
        image = image.convertToFormat(QImage::Format_RGB888);

    static auto nextImageId = vtbackend::ImageId(0);

    vtbackend::Image::Data pixels;
    pixels.resize(static_cast<size_t>(image.bytesPerLine() * image.height()));
    auto* p = pixels.data();
    for (int i = 0; i < image.height(); ++i)
    {
        memcpy(p, image.constScanLine(i), static_cast<size_t>(image.bytesPerLine()));
        p += image.bytesPerLine();
    }

    auto format = vtbackend::ImageFormat::RGBA;
    switch (image.format())
    {
        case QImage::Format_RGBA8888: format = vtbackend::ImageFormat::RGBA; break;
        case QImage::Format_RGB888: format = vtbackend::ImageFormat::RGB; break;
        default: return std::nullopt;
    }

    auto const size = vtbackend::ImageSize { vtbackend::Width::cast_from(image.width()),
                                             vtbackend::Height::cast_from(image.height()) };
    auto onRemove = vtbackend::Image::OnImageRemove {};

    return vtbackend::Image(nextImageId++, format, std::move(pixels), size, std::move(onRemove));
}
#endif
// }}}

} // namespace contour::display
