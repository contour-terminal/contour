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

#include <terminal/Color.h>
#include <terminal/Metrics.h>
#include <terminal/pty/Pty.h>

#include <crispy/App.h>
#include <crispy/logstore.h>
#include <crispy/stdfs.h>

#include <fmt/chrono.h>
#include <fmt/format.h>

#include <range/v3/all.hpp>

#include <QtCore/QDebug>
#include <QtCore/QFileInfo>
#include <QtCore/QFileSystemWatcher>
#include <QtCore/QMetaEnum>
#include <QtCore/QMetaObject>
#include <QtCore/QProcess>
#include <QtCore/QStandardPaths>
#include <QtCore/QTimer>
#include <QtGui/QClipboard>
#include <QtGui/QDesktopServices>
#include <QtGui/QGuiApplication>
#include <QtGui/QKeyEvent>
#include <QtGui/QScreen>
#include <QtGui/QWindow>
#include <QtNetwork/QHostInfo>
#include <QtWidgets/QApplication>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QStyle>

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

// {{{ Widget creation and QOpenGLWidget overides
TerminalWidget::TerminalWidget():
    QOpenGLWidget(nullptr, Qt::WindowFlags()),
    startTime_ { steady_clock::time_point::min() },
    lastFontDPI_ { fontDPI() },
    filesystemWatcher_(this)
{
    initializeResourcesForContourFrontendOpenGL();

    setMouseTracking(true);
    setFormat(createSurfaceFormat());

    setAttribute(Qt::WA_InputMethodEnabled, true);
    setAttribute(Qt::WA_OpaquePaintEvent);

    // setAttribute(Qt::WA_TranslucentBackground);
    // setAttribute(Qt::WA_NoSystemBackground, false);

    updateTimer_.setSingleShot(true);
    connect(&updateTimer_, &QTimer::timeout, [this]() { scheduleRedraw(); });

    connect(this, SIGNAL(frameSwapped()), this, SLOT(onFrameSwapped()));
}

void TerminalWidget::setSession(TerminalSession& newSession)
{
    DisplayLog()(
        "Assigning session to terminal widget: shell={}, terminalSize={}, fontSize={}, contentScale={}",
        newSession.profile().shell.program,
        newSession.profile().terminalSize,
        newSession.profile().fonts.size,
        contentScale());

    session_ = &newSession;

    framelessWidget_ = !profile().show_title_bar;

    renderer_ = make_unique<terminal::renderer::Renderer>(
        newSession.profile().terminalSize,
        sanitizeFontDescription(profile().fonts, fontDPI()),
        newSession.profile().colors,
        newSession.profile().backgroundOpacity,
        newSession.config().textureAtlasHashtableSlots,
        newSession.config().textureAtlasTileCount,
        newSession.config().textureAtlasDirectMapping,
        newSession.profile().hyperlinkDecoration.normal,
        newSession.profile().hyperlinkDecoration.hover
        // TODO: , WindowMargin(windowMargin_.left, windowMargin_.bottom);
    );

    auto const textureTileSize = gridMetrics().cellSize;
    auto const viewportMargin = terminal::renderer::PageMargin {}; // TODO margin

    auto const precalculatedVieewSize = [this]() -> ImageSize {
        auto const hint = sizeHint();
        auto const uiSize = ImageSize { Width::cast_from(hint.width()), Height::cast_from(hint.height()) };
        // auto const uiSize = ImageSize { Width::cast_from(width()), Height::cast_from(height()) };
        return uiSize * contentScale();
    }();

    renderTarget_ = make_unique<OpenGLRenderer>(
        session_->profile().textShader.value_or(builtinShaderConfig(ShaderClass::Text)),
        session_->profile().backgroundShader.value_or(builtinShaderConfig(ShaderClass::Background)),
        session_->profile().backgroundImageShader.value_or(builtinShaderConfig(ShaderClass::BackgroundImage)),
        precalculatedVieewSize,
        textureTileSize,
        viewportMargin);
    renderer_->setRenderTarget(*renderTarget_);

    applyFontDPI();
    updateMinimumSize();
    updateGeometry();

    logDisplayTopInfo();
}

TerminalWidget::~TerminalWidget()
{
    makeCurrent(); // XXX must be called.
    renderTarget_.reset();
    doneCurrent();
}

terminal::PageSize TerminalWidget::windowSize() const noexcept
{
    if (!session_)
        return terminal::PageSize { LineCount(25), ColumnCount(80) };

    return profile().terminalSize;
}

QSize TerminalWidget::minimumSizeHint() const
{
    auto constexpr MinimumPageSize = PageSize { LineCount(2), ColumnCount(3) };
    auto const cellSize = gridMetrics().cellSize;
    auto const hint = cellSize * MinimumPageSize / contentScale();
    auto const qHint = QSize(unbox<int>(hint.width), unbox<int>(hint.height));
    // DisplayLog()("minimumSizeHint: {}", hint);
    return qHint;
}

QSize TerminalWidget::sizeHint() const
{
    if (!session_)
        return QOpenGLWidget::sizeHint();

    auto const hint = [this]() {
        auto const cellSize = gridMetrics().cellSize;
        auto const hint = cellSize * windowSize() / contentScale();
        return QSize(unbox<int>(hint.width), unbox<int>(hint.height));
    }();
    // DisplayLog()("sizeHint: {}x{} ({}x{})",
    //              hint.width(),
    //              hint.height(),
    //              hint.width() * devicePixelRatioF(),
    //              hint.height() * devicePixelRatioF());
    return hint;
}

void TerminalWidget::onRefreshRateChanged()
{
    auto const rate = refreshRate();
    DisplayLog()("Refresh rate changed to {}.", rate);
    session_->terminal().setRefreshRate(rate);
}

void TerminalWidget::configureScreenHooks()
{
    Require(window());
    Require(window()->windowHandle());

    QWindow* window = this->window()->windowHandle();
    QScreen* screen = screenOf(this);

    connect(window, SIGNAL(screenChanged(QScreen*)), this, SLOT(onScreenChanged()));
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

    logDisplayInfo();

    if (!session_)
        return;

    Require(renderer_);

    auto fd = renderer_->fontDescriptions();
    fd.dpi = newFontDPI;
    renderer_->setFonts(fd);

    session_->setContentScale(contentScale());

    auto const newPixelSize = terminal::ImageSize { Width::cast_from(width()), Height::cast_from(height()) };

    // Apply resize on same window metrics propagates proper recalculations and repaint.
    applyResize(newPixelSize, *session_, *renderer_);
}

void TerminalWidget::logDisplayTopInfo()
{
    static bool loggedOnce = false;
    if (loggedOnce)
        return;
    loggedOnce = true;

    Require(QOpenGLContext::currentContext() != nullptr);

    auto const openGLTypeString = QOpenGLContext::currentContext()->isOpenGLES() ? "OpenGL/ES" : "OpenGL";
#if defined(CONTOUR_BUILD_TYPE)
    DisplayLog()("[FYI] Build type          : {}", CONTOUR_BUILD_TYPE);
#endif
    DisplayLog()("[FYI] Application PID     : {}", QCoreApplication::applicationPid());
    DisplayLog()("[FYI] OpenGL type         : {}", openGLTypeString);
    DisplayLog()("[FYI] OpenGL renderer     : {}", (char const*) glGetString(GL_RENDERER));
    DisplayLog()("[FYI] Qt platform         : {}", QGuiApplication::platformName().toStdString());

    GLint versionMajor {};
    GLint versionMinor {};
    QOpenGLContext::currentContext()->functions()->glGetIntegerv(GL_MAJOR_VERSION, &versionMajor);
    QOpenGLContext::currentContext()->functions()->glGetIntegerv(GL_MINOR_VERSION, &versionMinor);
    DisplayLog()("[FYI] OpenGL version      : {}.{}", versionMajor, versionMinor);
    DisplayLog()("[FYI] Content scaling     : {:.2}", contentScale());

    string glslVersions = (char const*) glGetString(GL_SHADING_LANGUAGE_VERSION);

    // TODO: pass phys()/logical?) dpi to font manager, so font size can be applied right
    // TODO: also take window monitor switches into account

#if defined(GL_NUM_SHADING_LANGUAGE_VERSIONS)
    GLint glslNumShaderVersions {};
    glGetIntegerv(GL_NUM_SHADING_LANGUAGE_VERSIONS, &glslNumShaderVersions);
    glGetError(); // consume possible OpenGL error.
    if (glslNumShaderVersions > 0)
    {
        glslVersions += " (";
        for (GLint k = 0, l = 0; k < glslNumShaderVersions; ++k)
            if (auto const str = glGetStringi(GL_SHADING_LANGUAGE_VERSION, GLuint(k)); str && *str)
            {
                glslVersions += (l ? ", " : "");
                glslVersions += (char const*) str;
                l++;
            }
        glslVersions += ')';
    }
#endif
    DisplayLog()("[FYI] GLSL version        : {}", glslVersions);

    logDisplayInfo();
}

void TerminalWidget::logDisplayInfo()
{
    if (!session_)
        return;

    // clang-format off
    auto const fontSizeInPx = static_cast<int>(ceil((
        profile().fonts.size.pt / 72.0) * average(fontDPI())
    ));
    auto const normalScreenSize = crispy::ImageSize {
        Width::cast_from(screenOf(this)->size().width()),
        Height::cast_from(screenOf(this)->size().height())
    };
    auto const actualScreenSize = normalScreenSize * devicePixelRatioF();
    DisplayLog()("[FYI] Refresh rate        : {} Hz", refreshRate());
    DisplayLog()("[FYI] Screen size         : {}", actualScreenSize);
    DisplayLog()("[FYI] Logical DPI         : {}", logicalDPI());
    DisplayLog()("[FYI] Physical DPI        : {}", physicalDPI());
    if (devicePixelRatioF() != trunc(devicePixelRatioF()))
        DisplayLog()("[FYI] Device pixel ratio  : {} ({})", devicePixelRatio(), devicePixelRatioF());
    else
        DisplayLog()("[FYI] Device pixel ratio  : {}", devicePixelRatioF());
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

void TerminalWidget::initializeGL()
{
    DisplayLog()("initializeGL: size={}x{}, scale={}", size().width(), size().height(), contentScale());
    initializeOpenGLFunctions();
    configureScreenHooks();
    watchKdeDpiSetting();

#if !defined(NDEBUG) && defined(GL_DEBUG_OUTPUT) && defined(CONTOUR_DEBUG_OPENGL)
    CHECKED_GL(glEnable(GL_DEBUG_OUTPUT));
    CHECKED_GL(glDebugMessageCallback(&glMessageCallback, this));
#endif

    emit displayInitialized();
}

void TerminalWidget::resizeGL(int _width, int _height)
{
    QOpenGLWidget::resizeGL(_width, _height);

    if (!session_)
        return;

    auto const qtBaseWidgetSize =
        terminal::ImageSize { Width::cast_from(_width), Height::cast_from(_height) };
    auto const newPixelSize = qtBaseWidgetSize * contentScale();
    DisplayLog()("Resizing view to {}x{} virtual ({} actual).", _width, _height, newPixelSize);
    applyResize(newPixelSize, *session_, *renderer_);
}

void TerminalWidget::paintGL()
{
    // We consider *this* the true initial start-time.
    // That shouldn't be significantly different from the object construction
    // time, but just to be sure, we'll update it here.
    if (startTime_ == steady_clock::time_point::min())
        startTime_ = steady_clock::now();

    try
    {
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

        static_cast<OpenGLRenderer*>(renderTarget_.get())->setTime(steady_clock::now());

        renderTarget_->clear(
            terminal().isModeEnabled(terminal::DECMode::ReverseVideo)
                ? RGBAColor(profile().colors.defaultForeground, uint8_t(renderer_->backgroundOpacity()))
                : RGBAColor(profile().colors.defaultBackground, uint8_t(renderer_->backgroundOpacity())));
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

void TerminalWidget::onFrameSwapped()
{
    if (!state_.finish())
        update();
    else if (auto timeout = terminal().nextRender(); timeout.has_value())
        updateTimer_.start(timeout.value());
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

void TerminalWidget::mouseReleaseEvent(QMouseEvent* _event)
{
    sendMouseReleaseEvent(_event, *session_);
}

void TerminalWidget::focusInEvent(QFocusEvent* _event)
{
    QOpenGLWidget::focusInEvent(_event);
    session_->sendFocusInEvent(); // TODO: paint with "normal" colors
}

void TerminalWidget::focusOutEvent(QFocusEvent* _event)
{
    QOpenGLWidget::focusOutEvent(_event);
    session_->sendFocusOutEvent(); // TODO maybe paint with "faint" colors
}

void TerminalWidget::inputMethodEvent(QInputMethodEvent* _event)
{
    if (!_event->commitString().isEmpty())
    {
        QKeyEvent keyEvent(QEvent::KeyPress, 0, Qt::NoModifier, _event->commitString());
        keyPressEvent(&keyEvent);
        // TODO: emit keyPressedSignal(&keyEvent);
    }

    // if (_readOnly && isCursorOnDisplay())
    // {
    //     // _inputMethodData.preeditString = event->preeditString();
    //     // update(preeditRect() | _inputMethodData.previousPreeditRect);
    // }

    _event->accept();
}

QVariant TerminalWidget::inputMethodQuery(Qt::InputMethodQuery _query) const
{
    const QPoint cursorPos = QPoint(); // TODO: realCursorPosition();
    switch (_query)
    {
        // TODO?: case Qt::ImCursorRectangle:
        // case Qt::ImMicroFocus:
        //     return imageToWidget(QRect(cursorPos.x(), cursorPos.y(), 1, 1));
        case Qt::ImFont: return font();
        case Qt::ImCursorPosition:
            // return the cursor position within the current line
            return cursorPos.x();
        // case Qt::ImSurroundingText:
        // {
        //     // return the text from the current line
        //     QString lineText;
        //     QTextStream stream(&lineText);
        //     PlainTextDecoder decoder;
        //     decoder.begin(&stream);
        //     if (isCursorOnDisplay()) {
        //         decoder.decodeLine(&_image[loc(0, cursorPos.y())], _usedColumns, LINE_DEFAULT);
        //     }
        //     decoder.end();
        //     return lineText;
        // }
        case Qt::ImCurrentSelection: return QString();
        default: break;
    }

    return QVariant();
}

bool TerminalWidget::event(QEvent* _event)
{
    try
    {
        if (_event->type() == QEvent::Close)
        {
            session_->pty().close();
            emit terminated();
        }

        return QOpenGLWidget::event(_event);
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
    auto const dpiScale = session_ ? session_->profile().fonts.dpiScale : 1.0;

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
                    auto const dpr = (forcedDPI * dpiScale) / 96.0;
                    return dpr;
                }
            }
        }
    }
#endif

    return devicePixelRatio() * dpiScale;
}

void TerminalWidget::updateMinimumSize()
{
    auto const MinimumGridSize = PageSize { LineCount(5), ColumnCount(10) };
    auto const minSize =
        ImageSize { Width::cast_from(unbox<int>(gridMetrics().cellSize.width) * *MinimumGridSize.columns),
                    Height::cast_from(unbox<int>(gridMetrics().cellSize.width) * *MinimumGridSize.columns) };
    auto const scaledMinSize = minSize / contentScale();
    setMinimumSize(scaledMinSize.width.as<int>(), scaledMinSize.height.as<int>());
    parentWidget()->setMinimumSize(scaledMinSize.width.as<int>(), scaledMinSize.height.as<int>());
}
// }}}

// {{{ TerminalDisplay: attributes
double TerminalWidget::refreshRate() const
{
    auto const screen = screenOf(this);
    if (!screen)
        return profile().refreshRate != 0.0 ? profile().refreshRate : 30.0;

    auto const systemRefreshRate = static_cast<double>(screen->refreshRate());
    if (1.0 < profile().refreshRate && profile().refreshRate < systemRefreshRate)
        return profile().refreshRate;
    else
        return systemRefreshRate;
}

DPI TerminalWidget::fontDPI() const noexcept
{
#if 0 // defined(__APPLE__)
    return physicalDPI() * contentScale();
#else
    return logicalDPI() * contentScale();
#endif
}

DPI TerminalWidget::logicalDPI() const noexcept
{
    return DPI { logicalDpiX(), logicalDpiY() };
}

DPI TerminalWidget::physicalDPI() const noexcept
{
    return DPI { physicalDpiX(), physicalDpiY() };
}

bool TerminalWidget::isFullScreen() const
{
    return window()->isFullScreen();
}

terminal::ImageSize TerminalWidget::pixelSize() const
{
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

bool TerminalWidget::requestPermission(config::Permission _allowedByConfig, string_view _topicText)
{
    return contour::requestPermission(rememberedPermissions_, this, _allowedByConfig, _topicText);
}

terminal::FontDef TerminalWidget::getFontDef()
{
    Require(renderer_);
    return getFontDefinition(*renderer_);
}

void TerminalWidget::bell()
{
    QApplication::beep();
}

void TerminalWidget::copyToClipboard(std::string_view _data)
{
    if (QClipboard* clipboard = QGuiApplication::clipboard(); clipboard != nullptr)
        clipboard->setText(QString::fromUtf8(_data.data(), static_cast<int>(_data.size())));
}

void TerminalWidget::inspect()
{
    post([this]() { doDumpState(); });
}

void TerminalWidget::doDumpState()
{
    makeCurrent();

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

    terminal::renderer::RenderTarget& renderTarget = renderer_->renderTarget();

    do
    {
        auto infoOpt = renderTarget.readAtlas();
        if (!infoOpt.has_value())
            break;

        terminal::renderer::AtlasTextureScreenshot const& info = infoOpt.value();
        auto const fileName = targetDir / "texture-atlas-rgba.png";
        DisplayLog()("Saving image {} to: {}", info.size, fileName.generic_string());

        QImage(info.buffer.data(),
               info.size.width.as<int>(),
               info.size.height.as<int>(),
               QImage::Format_RGBA8888)
            .mirrored(false, true)
            .save(QString::fromStdString(fileName.generic_string()));
    } while (0);

    renderTarget.scheduleScreenshot(
        [this, targetDir, saveImage](std::vector<uint8_t> const& rgbaPixels, ImageSize imageSize) {
            saveImage(targetDir / "screenshot.png", rgbaPixels, imageSize);

            // If this dump-state was triggered due to the PTY being closed
            // and a dump was requested at the end, then terminate this session here now.
            if (session_->terminal().device().isClosed() && session_->app().dumpStateAtExit().has_value())
            {
                session_->terminate();
            }
        });

    // force an update to actually render the screenshot
    update();
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
    updateGeometry();
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
    updateGeometry();
}

void TerminalWidget::setFonts(terminal::renderer::FontDescriptions fonts)
{
    Require(session_ != nullptr);

    if (applyFontDescription(gridMetrics().cellSize, pageSize(), pixelSize(), fontDPI(), *renderer_, fonts))
    {
        // resize widget (same pixels, but adjusted terminal rows/columns and margin)
        applyResize(pixelSize(), *session_, *renderer_);
        logDisplayInfo();
    }
}

bool TerminalWidget::setFontSize(text::font_size _size)
{
    Require(session_ != nullptr);

    DisplayLog()("Setting display font size and recompute metrics: {}pt", _size.pt);

    if (!renderer_->setFontSize(_size))
        return false;

    auto const qtBaseWidgetSize =
        ImageSize { terminal::Width::cast_from(width()), terminal::Height::cast_from(height()) };
    renderer_->setMargin(computeMargin(gridMetrics().cellSize, pageSize(), qtBaseWidgetSize));
    // resize widget (same pixels, but adjusted terminal rows/columns and margin)
    auto const actualWidgetSize = qtBaseWidgetSize * contentScale();
    applyResize(actualWidgetSize, *session_, *renderer_);
    updateMinimumSize();
    logDisplayInfo();
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

void TerminalWidget::setWindowTitle(string_view _title)
{
    auto const title = _title.empty() ? "contour"s : fmt::format("{} - contour", _title);

    // TODO: since we do not control the whole window, it would be best to emit a signal (or call back)
    // instead.
    if (window() && window()->windowHandle())
        window()->windowHandle()->setTitle(QString::fromUtf8(title.c_str()));
}

void TerminalWidget::setWindowFullScreen()
{
    window()->windowHandle()->showFullScreen();
}

void TerminalWidget::setWindowMaximized()
{
    window()->showMaximized();
    maximizedState_ = true;
}

void TerminalWidget::setWindowNormal()
{
    updateMinimumSize();
    window()->windowHandle()->showNormal();
    maximizedState_ = false;
}

void TerminalWidget::setBlurBehind(bool _enable)
{
    BlurBehind::setEnabled(window()->windowHandle(), _enable);
}

void TerminalWidget::setBackgroundImage(
    std::shared_ptr<terminal::BackgroundImage const> const& backgroundImage)
{
    assert(renderTarget_ != nullptr);
    renderTarget_->setBackgroundImage(backgroundImage);
}

void TerminalWidget::toggleFullScreen()
{
    if (window()->isFullScreen())
    {
        window()->showNormal();
        if (maximizedState_)
            window()->showMaximized();
    }
    else
    {
        maximizedState_ = window()->isMaximized();
        window()->showFullScreen();
    }

    // if (window_.visibility() == QWindow::FullScreen)
    //     window_.setVisibility(QWindow::Windowed);
    // else
    //     window_.setVisibility(QWindow::FullScreen);
}

void TerminalWidget::toggleTitleBar()
{
    bool fullscreenState = window()->isFullScreen();
    maximizedState_ = window()->isMaximized();
    auto pos = window()->pos();
    auto windowYCoordinate = pos.y();
    if (framelessWidget_)
        windowYCoordinate += window()->style()->pixelMetric(QStyle::PM_TitleBarHeight)
                             + window()->style()->pixelMetric(QStyle::PM_SizeGripSize);
    pos.setY(windowYCoordinate);
    window()->setWindowFlag(Qt::FramelessWindowHint, !framelessWidget_);
    framelessWidget_ = !framelessWidget_;
    window()->showNormal();
    terminal().sendFocusInEvent();
    if (fullscreenState)
        toggleFullScreen();
    if (maximizedState_)
        window()->showMaximized();
    window()->move(pos);
}

void TerminalWidget::setHyperlinkDecoration(terminal::renderer::Decorator _normal,
                                            terminal::renderer::Decorator _hover)
{
    renderer_->setHyperlinkDecoration(_normal, _hover);
}

void TerminalWidget::setBackgroundOpacity(terminal::Opacity _opacity)
{
    renderer_->setBackgroundOpacity(_opacity);

    if (session_)
        session_->terminal().breakLoopAndRefreshRenderBuffer();
}
// }}}

// {{{ TerminalDisplay: terminal events
void TerminalWidget::scheduleRedraw()
{
    if (setScreenDirty())
    {
        update(); // QCoreApplication::postEvent(this, new QEvent(QEvent::UpdateRequest));

        emit terminalBufferUpdated(); // TODO: should not be invoked, as it's not guarranteed to be updated.
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
