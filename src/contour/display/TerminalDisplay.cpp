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
#include <QtMultimedia/QAudioOutput>
#include <QtNetwork/QHostInfo>
#include <QtQml/QQmlContext>
#include <QtQuick/QQuickWindow>

#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <string_view>
#include <tuple>
#include <vector>

namespace fs = std::filesystem;

// Temporarily disabled (I think it was macOS that didn't like glDebugMessageCallback).
// #define CONTOUR_DEBUG_OPENGL 1

#if defined(_MSC_VER)
    #define __PRETTY_FUNCTION__ __FUNCDNAME__
#endif

// Must be in global namespace
// NB: must be publically visible, and due to -Wmissing-declarations, we better tell the compiler.
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

} // namespace
// }}}

// {{{ Display creation and QQuickItem overides
TerminalDisplay::TerminalDisplay(QQuickItem* parent):
    QQuickItem(parent),
    _startTime { steady_clock::time_point::min() },
    _lastFontDPI { fontDPI() },
    _updateTimer(this),
    _filesystemWatcher(this),
    _mediaPlayer(this)
{
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
    if (_session)
        return;

    // This will print the same pointer address for `this` but a new one for newSession (model data).
    displayLog()("Assigning session to terminal widget({} <- {}): shell={}, terminalSize={}, fontSize={}, "
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

    _session = newSession;

    QObject::connect(newSession, &TerminalSession::titleChanged, this, &TerminalDisplay::titleChanged);

    _session->start();

    window()->setFlag(Qt::FramelessWindowHint, !profile().showTitleBar.value());

    _renderer =
        make_unique<vtrasterizer::Renderer>(newSession->profile().terminalSize.value(),
                                            sanitizeFontDescription(profile().fonts.value(), fontDPI()),
                                            _session->terminal().colorPalette(),
                                            newSession->config().textureAtlasHashtableSlots.value(),
                                            newSession->config().textureAtlasTileCount.value(),
                                            newSession->config().textureAtlasDirectMapping.value(),
                                            newSession->profile().hyperlinkDecorationNormal.value(),
                                            newSession->profile().hyperlinkDecorationHover.value()
                                            // TODO: , WindowMargin(windowMargin_.left, windowMargin_.bottom);
        );

    applyFontDPI();
    updateImplicitSize();
    updateMinimumSize();

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

    displayLog()(
        "size changed to: {}x{} (session {})", width(), height(), _session ? "available" : "not attached");

    auto const qtBaseDisplaySize =
        vtbackend::ImageSize { Width::cast_from(width()), Height::cast_from(height()) };
    auto const actualPixelSize = qtBaseDisplaySize * contentScale();
    displayLog()("Resizing view to {}x{} virtual ({} actual).", width(), height(), actualPixelSize);
    applyResize(actualPixelSize, *_session, *_renderer);
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

    displayLog()("Applying DPI {}.", newFontDPI);
    _lastFontDPI = newFontDPI;

    // logDisplayInfo();

    if (!_session)
        return;

    Require(_renderer);

    auto fd = _renderer->fontDescriptions();
    fd.dpi = newFontDPI;
    _renderer->setFonts(fd);

    _session->setContentScale(contentScale());

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
    // displayLog()("onSceneGrapheInitialized");

#if !defined(NDEBUG) && defined(GL_DEBUG_OUTPUT) && defined(CONTOUR_DEBUG_OPENGL)
    CHECKED_GL(glEnable(GL_DEBUG_OUTPUT));
    CHECKED_GL(glDebugMessageCallback(&glMessageCallback, this));
#endif
}

void TerminalDisplay::onBeforeSynchronize()
{
    if (!_session)
        return;

    // find screen with biggest width
    auto* screenToUse = window()->screen();
    for (auto* screen: window()->screen()->virtualSiblings())
    {
        if (screen->size().width() > screenToUse->size().width())
        {
            screenToUse = screen;
        }
    }
    window()->setScreen(screenToUse);

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
    Require(width() > 1.0 && height() > 1.0);

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
    Require(_session);
    Require(_renderer);
    Require(window());

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

    _session->configureDisplay();

    // {{{ Apply proper grid/pixel sizes to terminal
    {
        auto const qtBaseDisplaySize =
            ImageSize { vtbackend::Width::cast_from(width()), vtbackend::Height::cast_from(height()) };

        auto const actualDisplaySize = qtBaseDisplaySize * contentScale();

        applyResize(actualDisplaySize, *_session, *_renderer);
    }
    // }}}

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
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
        window()->beginExternalCommands();
        auto const _ = gsl::finally([this]() { window()->endExternalCommands(); });
#endif

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
        _session->sendFocusInEvent(); // TODO: paint with "normal" colors
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
    QPoint cursorPos = QPoint();
    auto const dpr = contentScale();
    if (terminal().isCursorInViewport())
    {
        auto const gridCursorPos = terminal().currentScreen().cursor().position;
        cursorPos.setX(int(unbox<double>(gridCursorPos.column)
                           * unbox<double>(_renderer->gridMetrics().cellSize.width)));
        cursorPos.setY(
            int(unbox<double>(gridCursorPos.line) * unbox<double>(_renderer->gridMetrics().cellSize.height)));
        cursorPos /= dpr;
    }

    switch (query)
    {
        case Qt::ImCursorRectangle: {
            auto const& gridMetrics = _renderer->gridMetrics();
            auto theContentsRect = QRect(); // TODO: contentsRect();
            auto result = QRect();
            result.setLeft(theContentsRect.left() + cursorPos.x());
            result.setTop(theContentsRect.top() + cursorPos.y());
            result.setWidth(int(unbox<double>(gridMetrics.cellSize.width)
                                / dpr)); // TODO: respect double-width characters
            result.setHeight(int(unbox<double>(gridMetrics.cellSize.height) / dpr));
            return result;
            break;
        }
        // TODO?: case Qt::ImCursorRectangle:
        // case Qt::ImMicroFocus:
        //     return imageToDisplay(QRect(cursorPos.x(), cursorPos.y(), 1, 1));
        // case Qt::ImFont:
        //     return QFont("monospace", 10);
        case Qt::ImCursorPosition:
            // return the cursor position within the current line
            return cursorPos.x();
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
        // This can only happen during TerminalDisplay instanciation
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

    auto const requiredSize = computeRequiredSize(_session->profile().margins.value(),
                                                  _renderer->cellSize() * (1.0 / contentScale()),
                                                  _session->terminal().totalPageSize());

    setImplicitWidth(unbox<qreal>(requiredSize.width));
    setImplicitHeight(unbox<qreal>(requiredSize.height));
}

void TerminalDisplay::updateMinimumSize()
{
    Require(window());
    Require(_renderer);
    assert(_session);

    auto constexpr MinimumTotalPageSize = PageSize { LineCount(5), ColumnCount(10) };
    auto const minimumSize = computeRequiredSize(_session->profile().margins.value(),
                                                 _renderer->cellSize() * (1.0 / contentScale()),
                                                 MinimumTotalPageSize);

    window()->setMinimumSize(QSize(unbox<int>(minimumSize.width), unbox<int>(minimumSize.height)));
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
    return DPI { 96, 96 } * contentScale();
}

bool TerminalDisplay::isFullScreen() const
{
    return window()->visibility() == QQuickWindow::Visibility::FullScreen;
}

vtbackend::ImageSize TerminalDisplay::pixelSize() const
{
    assert(_session);
    return gridMetrics().cellSize * _session->terminal().pageSize();
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
#if QT_VERSION >= QT_VERSION_CHECK(5, 10, 0)
    QMetaObject::invokeMethod(this, &TerminalDisplay::doDumpState, Qt::QueuedConnection);
#endif
}

void TerminalDisplay::doDumpState()
{
    _doDumpState = true;
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
    auto [size, image] = _renderTarget->takeScreenshot();
    QImage(image.data(), size.width.as<int>(), size.height.as<int>(), QImage::Format_RGBA8888_Premultiplied)
        .mirrored(false, true)
        .save(QString::fromStdString(screenshotFilePath.string()));
}

void TerminalDisplay::notify(std::string_view /*_title*/, std::string_view /*_body*/)
{
    // TODO: showNotification callback to Controller?
}

void TerminalDisplay::adaptToWidgetSize()
{
    // Resize widget (same pixels, but adjusted terminal rows/columns and margin)
    Require(_renderer != nullptr);
    Require(_session != nullptr);

    auto const qtBaseDisplaySize =
        ImageSize { vtbackend::Width::cast_from(width()), vtbackend::Height::cast_from(height()) };
    auto const actualDisplaySize = qtBaseDisplaySize * contentScale();
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

    auto requestedPageSize = terminal().totalPageSize();
    if (*newColumnCount)
        requestedPageSize.columns = newColumnCount;
    if (*newLineCount)
        requestedPageSize.lines = newLineCount;

    // Qt uses unscaled pixels, so we need to adjust the requested size to the actual content scale.
    auto const unscaledCellSize = gridMetrics().cellSize / contentScale();
    auto const unscaledViewSize = vtbackend::ImageSize {
        unscaledCellSize.width * boxed_cast<vtbackend::Width>(requestedPageSize.columns),
        unscaledCellSize.height * boxed_cast<vtbackend::Height>(requestedPageSize.lines)
    };

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
        // logDisplayInfo();
    }
}

bool TerminalDisplay::setFontSize(text::font_size newFontSize)
{
    Require(_renderer != nullptr);

    displayLog()("Setting display font size and recompute metrics: {}pt", newFontSize.pt);

    if (!_renderer->setFontSize(newFontSize))
        return false;

    adaptToWidgetSize();
    updateMinimumSize();
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
    window()->showFullScreen();
}

void TerminalDisplay::setWindowMaximized()
{
    window()->showMaximized();
    _maximizedState = true;
}

void TerminalDisplay::setWindowNormal()
{
    updateMinimumSize();
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
        window()->showFullScreen();
    }
    else if (_maximizedState)
        window()->showMaximized();
    else
        window()->showNormal();
}

void TerminalDisplay::toggleTitleBar()
{
    auto const currentlyFrameless = (window()->flags() & Qt::FramelessWindowHint) != 0;
    _maximizedState = window()->visibility() == QQuickWindow::Visibility::Maximized;

    window()->setFlag(Qt::FramelessWindowHint, !currentlyFrameless);
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
