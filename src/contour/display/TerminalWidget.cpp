/**
 * This file is part of the "contour" project
 *   Copyright (c) 2019-2021 Christian Parpart <christian@parpart.family>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <contour/Actions.h>
#include <contour/BlurBehind.h>
#include <contour/ContourGuiApp.h>
#include <contour/display/OpenGLRenderer.h>
#include <contour/display/TerminalWidget.h>
#include <contour/helper.h>

#include <vtbackend/Color.h>
#include <vtbackend/Metrics.h>

#include <vtpty/Pty.h>

#include <crispy/App.h>
#include <crispy/logstore.h>
#include <crispy/stdfs.h>
#include <crispy/utils.h>

#include <fmt/chrono.h>
#include <fmt/format.h>

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

#include <algorithm>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <vector>

// Temporarily disabled (I think it was OS/X that didn't like glDebugMessageCallback).
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

using terminal::Height;
using terminal::ImageSize;
using terminal::Width;

using terminal::ColumnCount;
using terminal::LineCount;
using terminal::PageSize;
using terminal::RGBAColor;

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
                default: return fmt::format("{}", _severity);
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
                default: return fmt::format("{}", _severity);
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
                default: return fmt::format("{}", _severity);
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

        DisplayLog()("[OpenGL/{}]: type:{}, source:{}, severity:{}; {}",
                     tag,
                     typeName,
                     sourceName,
                     debugSeverity,
                     _message);
    }
#endif

    std::string unhandledExceptionMessage(std::string_view const& where, exception const& e)
    {
        return fmt::format("{}: Unhandled exception caught ({}). {}", where, typeid(e).name(), e.what());
    }

    void reportUnhandledException(std::string_view const& where, exception const& e)
    {
        DisplayLog()("{}", unhandledExceptionMessage(where, e));
        cerr << unhandledExceptionMessage(where, e) << endl;
    }

    // Returns the config file containing the user-configured DPI setting for KDE desktops.
    [[maybe_unused]] std::optional<FileSystem::path> kcmFontsFilePath()
    {
#if !defined(__APPLE__) && !defined(_WIN32)
        auto const xdgConfigHome = config::configHome("");
        auto const kcmFontsFile = xdgConfigHome / "kcmfonts";
        if (FileSystem::exists(kcmFontsFile))
            return { kcmFontsFile };
#endif

        return nullopt;
    }

} // namespace
// }}}

// {{{ Widget creation and QQuickItem overides
TerminalWidget::TerminalWidget(QQuickItem* parent):
    QQuickItem(parent),
    startTime_ { steady_clock::time_point::min() },
    lastFontDPI_ { fontDPI() },
    updateTimer_(this),
    filesystemWatcher_(this),
    mediaPlayer_(this)
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

    connect(this, &QQuickItem::windowChanged, this, &TerminalWidget::handleWindowChanged);

    // setMouseTracking(true);
    // setFormat(createSurfaceFormat());
    //
    // TODO: setAttribute(Qt::WA_InputMethodEnabled, true);

    updateTimer_.setSingleShot(true);
    connect(&updateTimer_, &QTimer::timeout, this, &TerminalWidget::scheduleRedraw, Qt::QueuedConnection);
}

TerminalWidget::~TerminalWidget()
{
    DisplayLog()("Destroying terminal widget.");
    if (session_)
        session_->detachDisplay(*this);
}

void TerminalWidget::setSession(TerminalSession* newSession)
{
    if (session_)
        return;

    // This will print the same pointer address for `this` but a new one for newSession (model data).
    DisplayLog()("Assigning session to terminal widget({} <- {}): shell={}, terminalSize={}, fontSize={}, "
                 "contentScale={}",
                 (void const*) this,
                 (void const*) newSession,
                 newSession->profile().shell.program,
                 newSession->profile().terminalSize,
                 newSession->profile().fonts.size,
                 contentScale());

    session_ = newSession;

    QObject::connect(newSession, &TerminalSession::titleChanged, this, &TerminalWidget::titleChanged);

    session_->start();

    window()->setFlag(Qt::FramelessWindowHint, !profile().show_title_bar);

    renderer_ = make_unique<terminal::rasterizer::Renderer>(
        newSession->profile().terminalSize,
        sanitizeFontDescription(profile().fonts, fontDPI()),
        newSession->profile().colors,
        newSession->config().textureAtlasHashtableSlots,
        newSession->config().textureAtlasTileCount,
        newSession->config().textureAtlasDirectMapping,
        newSession->profile().hyperlinkDecoration.normal,
        newSession->profile().hyperlinkDecoration.hover
        // TODO: , WindowMargin(windowMargin_.left, windowMargin_.bottom);
    );

    applyFontDPI();
    updateSizeProperties();

    session_->attachDisplay(*this); // NB: Requires Renderer to be instanciated to retrieve grid metrics.

    emit sessionChanged(newSession);
}

terminal::PageSize TerminalWidget::windowSize() const noexcept
{
    if (!session_)
        return terminal::PageSize { LineCount(25), ColumnCount(80) };

    return profile().terminalSize;
}

void TerminalWidget::sizeChanged()
{
    if (!session_ || !renderTarget_)
        return;

    DisplayLog()(
        "size changed to: {}x{} (session {})", width(), height(), session_ ? "available" : "not attached");

    auto const qtBaseWidgetSize =
        terminal::ImageSize { Width::cast_from(width()), Height::cast_from(height()) };
    auto const newPixelSize = qtBaseWidgetSize * contentScale();
    DisplayLog()("Resizing view to {}x{} virtual ({} actual).", width(), height(), newPixelSize);
    applyResize(newPixelSize, *session_, *renderer_);
}

void TerminalWidget::handleWindowChanged(QQuickWindow* newWindow)
{
    if (newWindow)
    {
        DisplayLog()("Attaching widget {} to window {}.", (void*) this, (void*) newWindow);
        connect(newWindow,
                &QQuickWindow::sceneGraphInitialized,
                this,
                &TerminalWidget::onSceneGrapheInitialized,
                Qt::DirectConnection);

        connect(newWindow,
                &QQuickWindow::beforeSynchronizing,
                this,
                &TerminalWidget::synchronize,
                Qt::DirectConnection);

        connect(newWindow,
                &QQuickWindow::sceneGraphInvalidated,
                this,
                &TerminalWidget::cleanup,
                Qt::DirectConnection);

        connect(this, &QQuickItem::widthChanged, this, &TerminalWidget::sizeChanged, Qt::DirectConnection);
        connect(this, &QQuickItem::heightChanged, this, &TerminalWidget::sizeChanged, Qt::DirectConnection);
    }
    else
        DisplayLog()("Detaching widget {} from window.", (void*) this);
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

void TerminalWidget::releaseResources()
{
    DisplayLog()("Releasing resources.");
    window()->scheduleRenderJob(new CleanupJob(renderTarget_), QQuickWindow::BeforeSynchronizingStage);
    renderTarget_ = nullptr;
}

void TerminalWidget::cleanup()
{
    DisplayLog()("Cleaning up.");
    delete renderTarget_;
    renderTarget_ = nullptr;
}

void TerminalWidget::onRefreshRateChanged()
{
    auto const rate = refreshRate();
    DisplayLog()("Refresh rate changed to {}.", rate.value);
    session_->terminal().setRefreshRate(rate);
}

void TerminalWidget::configureScreenHooks()
{
    Require(window());

    QScreen* screen = window()->screen();

    connect(window(), SIGNAL(screenChanged(QScreen*)), this, SLOT(onScreenChanged()));
    connect(screen, SIGNAL(refreshRateChanged(qreal)), this, SLOT(onRefreshRateChanged()));
    connect(screen, SIGNAL(logicalDotsPerInchChanged(qreal)), this, SLOT(applyFontDPI()));
    // connect(screen, SIGNAL(physicalDotsPerInchChanged(qreal)), this, SLOT(applyFontDPI()));
}

void TerminalWidget::onScreenChanged()
{
    DisplayLog()("Screen changed.");
    applyFontDPI();
}

void TerminalWidget::applyFontDPI()
{
    auto const newFontDPI = fontDPI();
    if (newFontDPI == lastFontDPI_)
        return;

    DisplayLog()("Applying DPI {}.", newFontDPI);
    lastFontDPI_ = newFontDPI;

    // logDisplayInfo();

    if (!session_)
        return;

    Require(renderer_);

    auto fd = renderer_->fontDescriptions();
    fd.dpi = newFontDPI;
    renderer_->setFonts(fd);

    session_->setContentScale(contentScale());

    if (!renderTarget_)
        return;

    auto const newPixelSize = terminal::ImageSize { Width::cast_from(width()), Height::cast_from(height()) };

    // Apply resize on same window metrics propagates proper recalculations and repaint.
    applyResize(newPixelSize, *session_, *renderer_);
}

void TerminalWidget::logDisplayInfo()
{
    if (!session_)
        return;

    Require(renderer_);

    // clang-format off
    auto const fontSizeInPx = static_cast<int>(ceil((
        profile().fonts.size.pt / 72.0) * average(fontDPI())
    ));
    auto const normalScreenSize = crispy::ImageSize {
        Width::cast_from(window()->screen()->size().width()),
        Height::cast_from(window()->screen()->size().height())
    };
    auto const actualScreenSize = normalScreenSize * window()->effectiveDevicePixelRatio();
#if defined(CONTOUR_BUILD_TYPE)
    DisplayLog()("[FYI] Build type          : {}", CONTOUR_BUILD_TYPE);
#endif
    DisplayLog()("[FYI] Application PID     : {}", QCoreApplication::applicationPid());
    DisplayLog()("[FYI] Qt platform         : {}", QGuiApplication::platformName().toStdString());
    DisplayLog()("[FYI] Refresh rate        : {} Hz", refreshRate().value);
    DisplayLog()("[FYI] Screen size         : {}", actualScreenSize);
    DisplayLog()("[FYI] Device pixel ratio  : {}", window()->devicePixelRatio());
    DisplayLog()("[FYI] Effective DPR       : {}", window()->effectiveDevicePixelRatio());
    DisplayLog()("[FYI] Content scale       : {}", contentScale());
    DisplayLog()("[FYI] Font DPI            : {} ({})", fontDPI(), renderer_->fontDescriptions().dpi);
    DisplayLog()("[FYI] Font size           : {} ({} px)", renderer_->fontDescriptions().size, fontSizeInPx);
    DisplayLog()("[FYI] Cell size           : {} px", gridMetrics().cellSize);
    DisplayLog()("[FYI] Page size           : {}", gridMetrics().pageSize);
    DisplayLog()("[FYI] Font baseline       : {} px", gridMetrics().baseline);
    DisplayLog()("[FYI] Underline position  : {} px", gridMetrics().underline.position);
    DisplayLog()("[FYI] Underline thickness : {} px", gridMetrics().underline.thickness);
    // clang-format on
}

void TerminalWidget::watchKdeDpiSetting()
{
#if defined(__unix__)
    auto const kcmFontsFile = kcmFontsFilePath();
    if (kcmFontsFile.has_value())
    {
        filesystemWatcher_.addPath(QString::fromStdString(kcmFontsFile->string()));
        connect(&filesystemWatcher_, SIGNAL(fileChanged(const QString&)), this, SLOT(onDpiConfigChanged()));
    }
#endif
}

void TerminalWidget::onDpiConfigChanged()
{
    applyFontDPI();
    watchKdeDpiSetting(); // re-watch file
}

void TerminalWidget::onSceneGrapheInitialized()
{
    // DisplayLog()("onSceneGrapheInitialized");

#if !defined(NDEBUG) && defined(GL_DEBUG_OUTPUT) && defined(CONTOUR_DEBUG_OPENGL)
    CHECKED_GL(glEnable(GL_DEBUG_OUTPUT));
    CHECKED_GL(glDebugMessageCallback(&glMessageCallback, this));
#endif
}

void TerminalWidget::synchronize()
{
    if (!session_)
        return;

    if (!renderTarget_)
        createRenderer();

    auto const dpr = contentScale();
    auto const windowSize = window()->size() * dpr;
    Require(width() > 1.0 && height() > 1.0);

    auto const viewSize = ImageSize { Width::cast_from(width() * dpr), Height::cast_from(height() * dpr) };

    renderTarget_->setRenderSize(
        ImageSize { Width::cast_from(windowSize.width()), Height::cast_from(windowSize.height()) });
    renderTarget_->setModelMatrix(createModelMatrix());
    renderTarget_->setTranslation(float(x() * dpr), float(y() * dpr), float(z() * dpr));
    renderTarget_->setViewSize(viewSize);
}

void TerminalWidget::createRenderer()
{
    Require(!renderTarget_);
    Require(session_);
    Require(renderer_);
    Require(window());

    auto const textureTileSize = gridMetrics().cellSize;
    auto const viewportMargin = terminal::rasterizer::PageMargin {}; // TODO margin
    auto const precalculatedViewSize = [this]() -> ImageSize {
        auto const uiSize = ImageSize { Width::cast_from(width()), Height::cast_from(height()) };
        return uiSize * contentScale();
    }();
    auto const precalculatedTargetSize = [this]() -> ImageSize {
        auto const uiSize =
            ImageSize { Width::cast_from(window()->width()), Height::cast_from(window()->height()) };
        return uiSize * contentScale();
    }();

    if (DisplayLog)
    {
        auto const dpr = contentScale();
        auto const viewSize =
            ImageSize { Width::cast_from(width() * dpr), Height::cast_from(height() * dpr) };
        auto const windowSize = window()->size() * dpr;
        DisplayLog()("Creating renderer: {}x+{}y+{}z ({} DPR, {} viewSize, {}x{} windowSize)\n",
                     x(),
                     y(),
                     z(),
                     dpr,
                     viewSize,
                     windowSize.width(),
                     windowSize.height());
    }

    renderTarget_ = new OpenGLRenderer(
        session_->profile().textShader.value_or(builtinShaderConfig(ShaderClass::Text)),
        session_->profile().backgroundShader.value_or(builtinShaderConfig(ShaderClass::Background)),
        precalculatedViewSize,
        precalculatedTargetSize,
        textureTileSize,
        viewportMargin);
    renderTarget_->setWindow(window());
    renderer_->setRenderTarget(*renderTarget_);

    connect(window(),
            &QQuickWindow::beforeRendering,
            this,
            &TerminalWidget::onBeforeRendering,
            Qt::ConnectionType::DirectConnection);

    // connect(window(),
    //         &QQuickWindow::beforeRenderPassRecording,
    //         this,
    //         &TerminalWidget::paint,
    //         Qt::DirectConnection);

    connect(window(),
            &QQuickWindow::afterRendering,
            this,
            &TerminalWidget::onAfterRendering,
            Qt::DirectConnection);

    configureScreenHooks();
    watchKdeDpiSetting();

    session_->configureDisplay();

    // {{{ Apply proper grid/pixel sizes to terminal
    {
        auto const qtBaseWidgetSize =
            ImageSize { terminal::Width::cast_from(width()), terminal::Height::cast_from(height()) };
        renderer_->setMargin(computeMargin(gridMetrics().cellSize, pageSize(), qtBaseWidgetSize));
        // resize widget (same pixels, but adjusted terminal rows/columns and margin)
        auto const actualWidgetSize = qtBaseWidgetSize * contentScale();
        applyResize(actualWidgetSize, *session_, *renderer_);
    }
    // }}}

    DisplayLog()("Implicit size: {}x{}", implicitWidth(), implicitHeight());
}

QMatrix4x4 TerminalWidget::createModelMatrix() const
{
    QMatrix4x4 result;

    // Compose model matrix from our transform properties in the QML
    QQmlListProperty<QQuickTransform> transformations = const_cast<TerminalWidget*>(this)->transform();
    auto const count = transformations.count(&transformations);
    for (int i = 0; i < count; i++)
    {
        QQuickTransform* transform = transformations.at(&transformations, i);
        transform->applyTo(&result);
    }

    return result;
}

void TerminalWidget::onBeforeRendering()
{
    if (renderTarget_->initialized())
        return;

    logDisplayInfo();
    renderTarget_->initialize();
}

void TerminalWidget::paint()
{
    // We consider *this* the true initial start-time.
    // That shouldn't be significantly different from the object construction
    // time, but just to be sure, we'll update it here.
    if (startTime_ == steady_clock::time_point::min())
        startTime_ = steady_clock::now();

    if (!renderTarget_)
        return;

    try
    {
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
        window()->beginExternalCommands();
        auto const _ = gsl::finally([this]() { window()->endExternalCommands(); });
#endif

        [[maybe_unused]] auto const lastState = state_.fetchAndClear();

#if defined(CONTOUR_PERF_STATS)
        {
            ++renderCount_;
            auto const updateCount = stats_.updatesSinceRendering.exchange(0);
            auto const renderCount = stats_.consecutiveRenderCount.exchange(0);
            if (DisplayLog)
                DisplayLog()("paintGL/{}: {} renders, {} updates since last paint ({}/{}).",
                             renderCount_.load(),
                             renderCount,
                             updateCount,
                             lastState,
                             to_string(session_->terminal().renderBufferState()));
        }
#endif

        terminal().tick(steady_clock::now());
        renderer_->render(terminal(), renderingPressure_);
    }
    catch (exception const& e)
    {
        reportUnhandledException(__PRETTY_FUNCTION__, e);
    }
}

float TerminalWidget::uptime() const noexcept
{
    using namespace std::chrono;
    auto const now = steady_clock::now();
    auto const uptimeMsecs = duration_cast<milliseconds>(now - startTime_).count();
    auto const uptimeSecs = static_cast<float>(uptimeMsecs) / 1000.0f;
    return uptimeSecs;
}

void TerminalWidget::onAfterRendering()
{
    // This method is called after the QML scene has been rendered.
    // We use this to schedule the next rendering frame, if needed.
    // This signal is emitted from the scene graph rendering thread
    paint();

    if (!state_.finish())
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
        post([this, timeout]() { updateTimer_.start(timeout); });
    }
}
// }}}

// {{{ Qt Widget Input Event handling & forwarding
void TerminalWidget::keyPressEvent(QKeyEvent* _keyEvent)
{
    sendKeyEvent(_keyEvent, *session_);
}

void TerminalWidget::wheelEvent(QWheelEvent* _event)
{
    sendWheelEvent(_event, *session_);
}

void TerminalWidget::mousePressEvent(QMouseEvent* _event)
{
    sendMousePressEvent(_event, *session_);
}

void TerminalWidget::mouseMoveEvent(QMouseEvent* _event)
{
    sendMouseMoveEvent(_event, *session_);
}

void TerminalWidget::hoverMoveEvent(QHoverEvent* event)
{
    QQuickItem::hoverMoveEvent(event);
    sendMouseMoveEvent(event, *session_);
}

void TerminalWidget::mouseReleaseEvent(QMouseEvent* _event)
{
    sendMouseReleaseEvent(_event, *session_);
}

void TerminalWidget::focusInEvent(QFocusEvent* _event)
{
    QQuickItem::focusInEvent(_event);
    if (session_)
        session_->sendFocusInEvent(); // TODO: paint with "normal" colors
}

void TerminalWidget::focusOutEvent(QFocusEvent* _event)
{
    QQuickItem::focusOutEvent(_event);
    if (session_)
        session_->sendFocusOutEvent(); // TODO maybe paint with "faint" colors
}

#if QT_CONFIG(im)
void TerminalWidget::inputMethodEvent(QInputMethodEvent* _event)
{
    terminal().updateInputMethodPreeditString(_event->preeditString().toStdString());

    if (!_event->commitString().isEmpty())
    {
        assert(_event->preeditString().isEmpty());
        QKeyEvent keyEvent(QEvent::KeyPress, 0, Qt::NoModifier, _event->commitString());
        keyPressEvent(&keyEvent);
    }

    _event->accept();
}
#endif

QVariant TerminalWidget::inputMethodQuery(Qt::InputMethodQuery _query) const
{
    QPoint cursorPos = QPoint();
    auto const dpr = contentScale();
    if (terminal().isCursorInViewport())
    {
        auto const gridCursorPos = terminal().currentScreen().cursor().position;
        cursorPos.setX(int(unbox<double>(gridCursorPos.column)
                           * unbox<double>(renderer_->gridMetrics().cellSize.width)));
        cursorPos.setY(
            int(unbox<double>(gridCursorPos.line) * unbox<double>(renderer_->gridMetrics().cellSize.height)));
        cursorPos /= dpr;
    }

    switch (_query)
    {
        case Qt::ImCursorRectangle: {
            auto const& gridMetrics = renderer_->gridMetrics();
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
        //     return imageToWidget(QRect(cursorPos.x(), cursorPos.y(), 1, 1));
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
    return QQuickItem::inputMethodQuery(_query);
}

bool TerminalWidget::event(QEvent* _event)
{
    try
    {
        if (_event->type() == QEvent::Close)
        {
            assert(session_);
            session_->pty().close();
            emit terminated();
        }

        return QQuickItem::event(_event);
    }
    catch (std::exception const& e)
    {
        fmt::print("Unhandled exception for event {}: {}\n",
                   (unsigned) _event->type(),
                   QMetaEnum::fromType<QEvent::Type>().valueToKey(_event->type()));
        reportUnhandledException(__PRETTY_FUNCTION__, e);
        return false;
    }
}
// }}}

// {{{ helpers
void TerminalWidget::onScrollBarValueChanged(int _value)
{
    terminal().viewport().scrollTo(terminal::ScrollOffset::cast_from(_value));
    scheduleRedraw();
}

double TerminalWidget::contentScale() const
{
#if !defined(__APPLE__) && !defined(_WIN32)
    if (auto const kcmFontsFile = kcmFontsFilePath())
    {
        auto const contents = crispy::readFileAsString(kcmFontsFile.value());
        for (auto const line: crispy::split(contents, '\n'))
        {
            auto const fields = crispy::split(line, '=');
            if (fields.size() == 2 && fields[0] == "forceFontDPI"sv)
            {
                auto const forcedDPI = static_cast<double>(crispy::to_integer(fields[1]).value_or(0.0));
                if (forcedDPI >= 96.0)
                {
                    auto const dpr = forcedDPI / 96.0;
                    return dpr;
                }
            }
        }
    }
#endif
    if (!window())
        // This can only happen during TerminalWidget instanciation
        return 1.0;

    return window()->devicePixelRatio();
}

void TerminalWidget::updateSizeProperties()
{
    Require(renderer_);
    assert(session_);

    // implicit width/height
    auto const dpr = contentScale();
    auto const implicitViewSize = renderer_->cellSize() * session_->terminal().pageSize() * (1.0 / dpr);
    setImplicitWidth(unbox<qreal>(implicitViewSize.width));
    setImplicitHeight(unbox<qreal>(implicitViewSize.height));

    Require(window());

    // minimum size
    auto const MinimumGridSize = PageSize { LineCount(5), ColumnCount(10) };
    auto const minSize =
        ImageSize { Width::cast_from(unbox<int>(gridMetrics().cellSize.width) * *MinimumGridSize.columns),
                    Height::cast_from(unbox<int>(gridMetrics().cellSize.width) * *MinimumGridSize.columns) };
    auto const scaledMinSize = minSize / dpr;

    window()->setMinimumSize(QSize(scaledMinSize.width.as<int>(), scaledMinSize.height.as<int>()));
}
// }}}

// {{{ TerminalDisplay: attributes
terminal::RefreshRate TerminalWidget::refreshRate() const
{
    auto const screen = window()->screen();
    if (!screen)
        return { profile().refreshRate.value != 0.0 ? profile().refreshRate
                                                    : terminal::RefreshRate { 30.0 } };

    auto const systemRefreshRate = terminal::RefreshRate { static_cast<double>(screen->refreshRate()) };
    if (1.0 < profile().refreshRate.value && profile().refreshRate.value < systemRefreshRate.value)
        return profile().refreshRate;
    else
        return systemRefreshRate;
}

DPI TerminalWidget::fontDPI() const noexcept
{
    return DPI { 96, 96 } * contentScale();
}

bool TerminalWidget::isFullScreen() const
{
    return window()->visibility() == QQuickWindow::Visibility::FullScreen;
}

terminal::ImageSize TerminalWidget::pixelSize() const
{
    assert(session_);
    return gridMetrics().cellSize * session_->terminal().pageSize();
}

terminal::ImageSize TerminalWidget::cellSize() const
{
    return gridMetrics().cellSize;
}
// }}}

// {{{ TerminalDisplay: (user requested) actions
void TerminalWidget::post(std::function<void()> _fn)
{
    postToObject(this, std::move(_fn));
}

terminal::FontDef TerminalWidget::getFontDef()
{
    Require(renderer_);
    return getFontDefinition(*renderer_);
}

void TerminalWidget::copyToClipboard(std::string_view _data)
{
    if (QClipboard* clipboard = QGuiApplication::clipboard(); clipboard != nullptr)
        clipboard->setText(QString::fromUtf8(_data.data(), static_cast<int>(_data.size())));
}

void TerminalWidget::inspect()
{
// Ensure we're invoked on GUI thread when calling doDumpState().
#if QT_VERSION >= QT_VERSION_CHECK(5, 10, 0)
    metaObject()->invokeMethod(this, &TerminalWidget::doDumpState, Qt::QueuedConnection);
#endif
}

void TerminalWidget::doDumpState()
{
    auto finally = crispy::finally { [this] {
        if (session_->terminal().device().isClosed() && session_->app().dumpStateAtExit().has_value())
            session_->terminate();
    } };

    if (!QOpenGLContext::currentContext())
    {
        errorlog()("Cannot dump state: no OpenGL context available");
        return;
    }
    if (!QOpenGLContext::currentContext()->makeCurrent(window()))
    {
        errorlog()("Cannot dump state: cannot make current");
        return;
    }

    Require(session_);
    Require(renderer_);

    // clang-format off
    auto const targetBaseDir = session_->app().dumpStateAtExit().value_or(crispy::App::instance()->localStateDir() / "dump");
    auto const workDirName = FileSystem::path(fmt::format("contour-dump-{:%Y-%m-%d-%H-%M-%S}", chrono::system_clock::now()));
    auto const targetDir = targetBaseDir / workDirName;
    auto const latestDirName = FileSystem::path("latest");
    // clang-format on

    FileSystem::create_directories(targetDir);

    if (FileSystem::exists(targetBaseDir / latestDirName))
        FileSystem::remove(targetBaseDir / latestDirName);

    FileSystem::create_symlink(workDirName, targetBaseDir / latestDirName);

    DisplayLog()("Dumping state into directory: {}", targetDir.generic_string());

    // TODO: The above should be done from the outside and the targetDir being passed into this call.
    // TODO: maybe zip this dir in the end.

    // TODO: use this file store for everything that needs to be dumped.
    {
        auto const screenStateDump = [&]() {
            auto os = std::stringstream {};
            terminal().currentScreen().inspect("Screen state dump.", os);
            renderer_->inspect(os);
            return os.str();
        }();

        std::cout << screenStateDump;

        auto const screenStateDumpFilePath = targetDir / "screen-state-dump.vt";
        auto fs = ofstream { screenStateDumpFilePath.string(), ios::trunc };
        fs << screenStateDump;
        fs.close();
    }

    enum class ImageBufferFormat
    {
        RGBA,
        RGB,
        Alpha
    };

    terminal::rasterizer::RenderTarget& renderTarget = renderer_->renderTarget();

    do
    {
        auto infoOpt = renderTarget.readAtlas();
        if (!infoOpt.has_value())
            break;

        terminal::rasterizer::AtlasTextureScreenshot const& info = infoOpt.value();
        auto const fileName = targetDir / "texture-atlas-rgba.png";
        DisplayLog()("Saving image {} to: {}", info.size, fileName.generic_string());

        QImage(info.buffer.data(),
               info.size.width.as<int>(),
               info.size.height.as<int>(),
               QImage::Format_RGBA8888)
            .save(QString::fromStdString(fileName.generic_string()));
    } while (0);

    auto screenshotFilePath = targetDir / "screenshot.png";
    DisplayLog()("Saving screenshot to: {}", screenshotFilePath.generic_string());
    window()->grabWindow().save(QString::fromStdString(screenshotFilePath.generic_string()));
}

void TerminalWidget::notify(std::string_view /*_title*/, std::string_view /*_body*/)
{
    // TODO: showNotification callback to Controller?
}

void TerminalWidget::resizeWindow(terminal::Width _width, terminal::Height _height)
{
    Require(session_ != nullptr);

    if (isFullScreen())
    {
        DisplayLog()("Application request to resize window in full screen mode denied.");
        return;
    }

    auto requestedPageSize = terminal().pageSize();
    auto const pixelSize =
        terminal::ImageSize { terminal::Width(*_width ? *_width : (unsigned) width()),
                              terminal::Height(*_height ? *_height : (unsigned) height()) };
    requestedPageSize.columns =
        terminal::ColumnCount(unbox<int>(pixelSize.width) / unbox<int>(gridMetrics().cellSize.width));
    requestedPageSize.lines =
        terminal::LineCount(unbox<int>(pixelSize.height) / unbox<int>(gridMetrics().cellSize.height));

    // setSizePolicy(QSizePolicy::Policy::Fixed, QSizePolicy::Policy::Fixed);
    const_cast<config::TerminalProfile&>(profile()).terminalSize = requestedPageSize;
    renderer_->setPageSize(requestedPageSize);
    auto const pixels =
        terminal::ImageSize { terminal::Width::cast_from(unbox<int>(requestedPageSize.columns)
                                                         * unbox<int>(gridMetrics().cellSize.width)),
                              terminal::Height::cast_from(unbox<int>(requestedPageSize.lines)
                                                          * unbox<int>(gridMetrics().cellSize.height)) };
    terminal().resizeScreen(requestedPageSize, pixels);
}

void TerminalWidget::resizeWindow(terminal::LineCount _lines, terminal::ColumnCount _columns)
{
    if (isFullScreen())
    {
        DisplayLog()("Application request to resize window in full screen mode denied.");
        return;
    }

    auto requestedPageSize = terminal().pageSize();
    if (*_columns)
        requestedPageSize.columns = _columns;
    if (*_lines)
        requestedPageSize.lines = _lines;

    // setSizePolicy(QSizePolicy::Policy::Fixed, QSizePolicy::Policy::Fixed);
    const_cast<config::TerminalProfile&>(profile()).terminalSize = requestedPageSize;
    renderer_->setPageSize(requestedPageSize);
    auto const pixels = terminal::ImageSize {
        terminal::Width(unbox<unsigned>(requestedPageSize.columns) * *gridMetrics().cellSize.width),
        terminal::Height(unbox<unsigned>(requestedPageSize.lines) * *gridMetrics().cellSize.height)
    };
    terminal().resizeScreen(requestedPageSize, pixels);
}

void TerminalWidget::setFonts(terminal::rasterizer::FontDescriptions fonts)
{
    Require(session_ != nullptr);
    Require(renderTarget_ != nullptr);

    if (applyFontDescription(gridMetrics().cellSize, pageSize(), pixelSize(), fontDPI(), *renderer_, fonts))
    {
        // resize widget (same pixels, but adjusted terminal rows/columns and margin)
        applyResize(pixelSize(), *session_, *renderer_);
        // logDisplayInfo();
    }
}

bool TerminalWidget::setFontSize(text::font_size _size)
{
    Require(session_ != nullptr);
    Require(renderTarget_ != nullptr);

    DisplayLog()("Setting display font size and recompute metrics: {}pt", _size.pt);

    if (!renderer_->setFontSize(_size))
        return false;

    auto const qtBaseWidgetSize =
        ImageSize { terminal::Width::cast_from(width()), terminal::Height::cast_from(height()) };
    renderer_->setMargin(computeMargin(gridMetrics().cellSize, pageSize(), qtBaseWidgetSize));
    // resize widget (same pixels, but adjusted terminal rows/columns and margin)
    auto const actualWidgetSize = qtBaseWidgetSize * contentScale();
    applyResize(actualWidgetSize, *session_, *renderer_);
    updateSizeProperties();
    // logDisplayInfo();
    return true;
}

bool TerminalWidget::setPageSize(PageSize _newPageSize)
{
    if (_newPageSize == terminal().pageSize())
        return false;

    auto const viewSize =
        ImageSize { Width(*gridMetrics().cellSize.width * unbox<unsigned>(profile().terminalSize.columns)),
                    Height(*gridMetrics().cellSize.width * unbox<unsigned>(profile().terminalSize.columns)) };
    renderer_->setPageSize(_newPageSize);
    terminal().resizeScreen(_newPageSize, viewSize);
    return true;
}

void TerminalWidget::setMouseCursorShape(MouseCursorShape _shape)
{
    if (auto const newShape = toQtMouseShape(_shape); newShape != cursor().shape())
        setCursor(newShape);
}

void TerminalWidget::setWindowFullScreen()
{
    window()->showFullScreen();
}

void TerminalWidget::setWindowMaximized()
{
    window()->showMaximized();
    maximizedState_ = true;
}

void TerminalWidget::setWindowNormal()
{
    updateSizeProperties();
    window()->showNormal();
    maximizedState_ = false;
}

void TerminalWidget::setBlurBehind(bool _enable)
{
    BlurBehind::setEnabled(window(), _enable);
}

void TerminalWidget::toggleFullScreen()
{
    if (!isFullScreen())
    {
        maximizedState_ = window()->visibility() == QQuickWindow::Visibility::Maximized;
        window()->showFullScreen();
    }
    else if (maximizedState_)
        window()->showMaximized();
    else
        window()->showNormal();
}

void TerminalWidget::toggleTitleBar()
{
    auto const currentlyFrameless = (window()->flags() & Qt::FramelessWindowHint) != 0;
    maximizedState_ = window()->visibility() == QQuickWindow::Visibility::Maximized;

    window()->setFlag(Qt::FramelessWindowHint, !currentlyFrameless);
}

void TerminalWidget::setHyperlinkDecoration(terminal::rasterizer::Decorator _normal,
                                            terminal::rasterizer::Decorator _hover)
{
    renderer_->setHyperlinkDecoration(_normal, _hover);
}
// }}}

// {{{ TerminalDisplay: terminal events
void TerminalWidget::scheduleRedraw()
{
    auto const currentHistoryLineCount = terminal().currentScreen().historyLineCount();
    if (currentHistoryLineCount != lastHistoryLineCount_)
    {
        // emit historyLineCountChanged(unbox<int>(currentHistoryLineCount));
        lastHistoryLineCount_ = currentHistoryLineCount;
    }

    if (setScreenDirty())
    {
        // QCoreApplication::postEvent(this, new QEvent(QEvent::UpdateRequest));
        if (window())
            post([this]() { window()->update(); });
    }
}

void TerminalWidget::renderBufferUpdated()
{
    scheduleRedraw();
}

void TerminalWidget::closeDisplay()
{
    DisplayLog()("closeDisplay");
    emit terminated();
}

void TerminalWidget::onSelectionCompleted()
{
    if (QClipboard* clipboard = QGuiApplication::clipboard(); clipboard != nullptr)
    {
        string const text = terminal().extractSelectionText();
        clipboard->setText(QString::fromUtf8(text.c_str(), static_cast<int>(text.size())),
                           QClipboard::Selection);
    }
}

void TerminalWidget::bufferChanged(terminal::ScreenType _type)
{
    using Type = terminal::ScreenType;
    switch (_type)
    {
        case Type::Primary: setCursor(Qt::IBeamCursor); break;
        case Type::Alternate: setCursor(Qt::ArrowCursor); break;
    }
    emit terminalBufferChanged(_type);
    // scheduleRedraw();
}

void TerminalWidget::discardImage(terminal::Image const& _image)
{
    renderer_->discardImage(_image);
}
// }}}

} // namespace contour::display
