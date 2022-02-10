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
#include <contour/ContourGuiApp.h>
#include <contour/helper.h>
#include <contour/opengl/OpenGLRenderer.h>
#include <contour/opengl/TerminalWidget.h>

#include <terminal/Color.h>
#include <terminal/Metrics.h>
#include <terminal/pty/Pty.h>

#include <crispy/App.h>
#include <crispy/logstore.h>
#include <crispy/stdfs.h>

#include <QtCore/QDebug>
#include <QtCore/QFileInfo>
#include <QtCore/QFileSystemWatcher>
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
#include <QtWidgets/QApplication>
#include <QtWidgets/QMessageBox>

#include <QtNetwork/QHostInfo>

#if defined(CONTOUR_BLUR_PLATFORM_KWIN)
    #include <KWindowEffects>
#endif

#include <fmt/chrono.h>
#include <fmt/format.h>

#include <range/v3/all.hpp>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <vector>

// Temporarily disabled (I think it was OS/X that didn't like glDebugMessageCallback).
// #define CONTOUR_DEBUG_OPENGL 1

#define CHECKED_GL(code)                                                      \
    do                                                                        \
    {                                                                         \
        (code);                                                               \
        GLenum err {};                                                        \
        while ((err = glGetError()) != GL_NO_ERROR)                           \
            LOGSTORE(DisplayLog)("OpenGL error {} for call: {}", err, #code); \
    } while (0)

#if defined(_MSC_VER)
    #define __PRETTY_FUNCTION__ __FUNCDNAME__
#endif

// Must be in global namespace
void initializeResourcesForContourFrontendOpenGL()
{
    Q_INIT_RESOURCE(contour_frontend_opengl);
}

namespace contour::opengl
{

using terminal::Height;
using terminal::ImageSize;
using terminal::Width;

using terminal::ColumnCount;
using terminal::LineCount;
using terminal::PageSize;
using terminal::RGBAColor;

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

        LOGSTORE(DisplayLog)
        ("[OpenGL/{}]: type:{}, source:{}, severity:{}; {}",
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
        LOGSTORE(DisplayLog)("{}", unhandledExceptionMessage(where, e));
        cerr << unhandledExceptionMessage(where, e) << endl;
    }

    // Returns the config file containing the user-configured DPI setting for KDE desktops.
    std::optional<FileSystem::path> kcmFontsFilePath()
    {
#if !defined(__APPLE__) && !defined(_WIN32)
        auto const xdgConfigHome = config::configHome("");
        auto const kcmFontsFile = xdgConfigHome / "kcmfonts";
        return { kcmFontsFile };
#endif

        return nullopt;
    }

} // namespace
// }}}

// {{{ Widget creation and QOpenGLWidget overides
TerminalWidget::TerminalWidget(TerminalSession& session,
                               function<void()> adaptSize,
                               function<void(bool)> enableBackgroundBlur):
    session_ { session },
    adaptSize_ { std::move(adaptSize) },
    enableBlurBehind_ { std::move(enableBackgroundBlur) },
    lastScreenDPI_ { screenDPI() },
    renderer_ {
        terminal().pageSize(),
        sanitizeFontDescription(profile().fonts, lastScreenDPI_),
        terminal().screen().colorPalette(),
        profile().backgroundOpacity,
        session_.config().textureAtlasHashtableSlots,
        session_.config().textureAtlasTileCount,
        session_.config().textureAtlasDirectMapping,
        profile().hyperlinkDecoration.normal,
        profile().hyperlinkDecoration.hover
        // TODO: , WindowMargin(windowMargin_.left, windowMargin_.bottom);
    },
    filesystemWatcher_(this)
{
    initializeResourcesForContourFrontendOpenGL();

    setMouseTracking(true);
    setFormat(surfaceFormat());

    setAttribute(Qt::WA_InputMethodEnabled, true);
    setAttribute(Qt::WA_OpaquePaintEvent);

    setMinimumSize(gridMetrics().cellSize.width.as<int>() * 3, gridMetrics().cellSize.height.as<int>() * 2);

    // setAttribute(Qt::WA_TranslucentBackground);
    // setAttribute(Qt::WA_NoSystemBackground, false);

    updateTimer_.setSingleShot(true);
    connect(&updateTimer_, &QTimer::timeout, [this]() { scheduleRedraw(); });

    connect(this, SIGNAL(frameSwapped()), this, SLOT(onFrameSwapped()));

    QOpenGLWidget::updateGeometry();
}

TerminalWidget::~TerminalWidget()
{
    LOGSTORE(DisplayLog)("~TerminalWidget");
    makeCurrent(); // XXX must be called.
    renderTarget_.reset();
    doneCurrent();
}

QSurfaceFormat TerminalWidget::surfaceFormat()
{
    QSurfaceFormat format;

    bool useOpenGLES = QOpenGLContext::openGLModuleType() == QOpenGLContext::LibGLES;

    if (useOpenGLES)
        format.setRenderableType(QSurfaceFormat::OpenGLES);
    else
        format.setRenderableType(QSurfaceFormat::OpenGL);

    format.setVersion(3, 3);
    format.setProfile(QSurfaceFormat::CoreProfile);
    format.setAlphaBufferSize(8);
    format.setSwapBehavior(QSurfaceFormat::DoubleBuffer);
    format.setSwapInterval(1);

#if !defined(NDEBUG)
    format.setOption(QSurfaceFormat::DebugContext);
#endif

    return format;
}

QSize TerminalWidget::minimumSizeHint() const
{
    auto constexpr MinimumPageSize = PageSize { LineCount(2), ColumnCount(3) };

    auto const cellSize = gridMetrics().cellSize;// / contentScale();

    return QSize(cellSize.width.as<int>() * MinimumPageSize.columns.as<int>(),
                 cellSize.height.as<int>() * MinimumPageSize.lines.as<int>());
}

QSize TerminalWidget::sizeHint() const
{
    auto const cellSize = renderer_.gridMetrics().cellSize;// / contentScale();
    auto const viewSize = ImageSize { Width(*cellSize.width * *profile().terminalSize.columns),
                                      Height(*cellSize.height * *profile().terminalSize.lines) };

    return QSize(viewSize.width.as<int>(), viewSize.height.as<int>());
}

void TerminalWidget::onRefreshRateChanged()
{
    auto const rate = refreshRate();
    LOGSTORE(DisplayLog)("Refresh rate changed to {}.", rate);
    session_.terminal().setRefreshRate(rate);
}

void TerminalWidget::configureScreenHooks()
{
    Require(window());
    Require(window()->windowHandle());

    QWindow* window = this->window()->windowHandle();
    QScreen* screen = screenOf(this);

    connect(window, SIGNAL(screenChanged(QScreen*)), this, SLOT(onScreenChanged()));
    connect(screen, SIGNAL(refreshRateChanged(qreal)), this, SLOT(onRefreshRateChanged()));
    connect(screen, SIGNAL(logicalDotsPerInchChanged(qreal)), this, SLOT(onScreenDpiChanged()));
    // connect(screen, SIGNAL(physicalDotsPerInchChanged(qreal)), this, SLOT(onScreenDpiChanged()));
}

void TerminalWidget::onScreenChanged()
{
    LOGSTORE(DisplayLog)("Screen changed.");
    onScreenDpiChanged();
}

void TerminalWidget::onScreenDpiChanged()
{
    auto const newScreenDPI = screenDPI();
    if (newScreenDPI == lastScreenDPI_)
        return;

    LOGSTORE(DisplayLog)("Screen DPI changed to {}.", newScreenDPI);
    lastScreenDPI_ = newScreenDPI;
    logDisplayInfo();
    auto fd = renderer_.fontDescriptions();
    fd.dpi = newScreenDPI;
    renderer_.setFonts(fd);

    renderTarget_->setContentScale(contentScale());
    session_.setContentScale(contentScale());

    auto const newPixelSize = terminal::ImageSize { Width(width()), Height(height()) };

    // Apply resize on same window metrics propagates proper recalculations and repaint.
    applyResize(newPixelSize, session_, renderer_);
}

void TerminalWidget::logDisplayInfo()
{
    // clang-format off
    auto const fontSizeInPx = static_cast<int>(ceil((
        profile().fonts.size.pt / 72.0) * average(screenDPI())
    ));
    LOGSTORE(DisplayLog)("[Display Info] Refresh rate        : {} Hz", refreshRate());
    LOGSTORE(DisplayLog)("[Display Info] Logical DPI         : {}", crispy::Size { logicalDpiX(), logicalDpiY() });
    LOGSTORE(DisplayLog)("[Display Info] Physical DPI        : {}", crispy::Size { physicalDpiX(), physicalDpiY() });
    LOGSTORE(DisplayLog)("[Display Info] Device pixel ratio  : {}", devicePixelRatioF());
    LOGSTORE(DisplayLog)("[Grid Metrics] Font DPI            : {}", crispy::Size{ renderer_.fontDescriptions().dpi.x, renderer_.fontDescriptions().dpi.y });
    LOGSTORE(DisplayLog)("[Grid Metrics] Font size           : {} ({} px)", profile().fonts.size, fontSizeInPx);
    LOGSTORE(DisplayLog)("[Grid Metrics] Cell size           : {} px", gridMetrics().cellSize);
    LOGSTORE(DisplayLog)("[Grid Metrics] Page size           : {}", gridMetrics().pageSize);
    LOGSTORE(DisplayLog)("[Grid Metrics] Font baseline       : {} px", gridMetrics().baseline);
    LOGSTORE(DisplayLog)("[Grid Metrics] Underline position  : {} px", gridMetrics().underline.position);
    LOGSTORE(DisplayLog)("[Grid Metrics] Underline thickness : {} px", gridMetrics().underline.thickness);
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
    onScreenDpiChanged();
    watchKdeDpiSetting(); // re-watch file
}

using opengl::ShaderClass;

void TerminalWidget::initializeGL()
{
    initializeOpenGLFunctions();
    configureScreenHooks();
    watchKdeDpiSetting();

    // Only now, and not early, we know the actual DPI / DPR / content scaling.
    auto fd = renderer_.fontDescriptions();
    fd.dpi = screenDPI();
    renderer_.setFonts(fd);

    auto const textureTileSize = renderer_.gridMetrics().cellSize;
    auto const viewportMargin = terminal::renderer::PageMargin {}; // TODO margin

    renderTarget_ = make_unique<OpenGLRenderer>(
        profile().textShader.value_or(builtinShaderConfig(ShaderClass::Text)),
        profile().backgroundShader.value_or(builtinShaderConfig(ShaderClass::Background)),
        profile().backgroundImageShader.value_or(builtinShaderConfig(ShaderClass::BackgroundImage)),
        ImageSize { Width(width()), Height(height()) },
        textureTileSize,
        viewportMargin);

    renderer_.setRenderTarget(*renderTarget_);

    // {{{ some info
    static bool infoPrinted = false;
    if (!infoPrinted)
    {
        // clang-format off
        infoPrinted = true;
        LOGSTORE(DisplayLog)("[FYI] Application PID : {}", QCoreApplication::applicationPid());
        LOGSTORE(DisplayLog)("[FYI] OpenGL type     : {}", (QOpenGLContext::currentContext()->isOpenGLES() ? "OpenGL/ES" : "OpenGL"));
        LOGSTORE(DisplayLog)("[FYI] OpenGL renderer : {}", glGetString(GL_RENDERER));
        LOGSTORE(DisplayLog)("[FYI] Qt platform     : {}", QGuiApplication::platformName().toStdString());
        // clang-format on

        GLint versionMajor {};
        GLint versionMinor {};
        QOpenGLContext::currentContext()->functions()->glGetIntegerv(GL_MAJOR_VERSION, &versionMajor);
        QOpenGLContext::currentContext()->functions()->glGetIntegerv(GL_MINOR_VERSION, &versionMinor);
        LOGSTORE(DisplayLog)("[FYI] OpenGL version  : {}.{}", versionMajor, versionMinor);

        auto glslVersionMsg =
            fmt::format("[FYI] GLSL version    : {}", glGetString(GL_SHADING_LANGUAGE_VERSION));

        // TODO: pass phys()/logical?) dpi to font manager, so font size can be applied right
        // TODO: also take window monitor switches into account

        GLint glslNumShaderVersions {};
#if defined(GL_NUM_SHADING_LANGUAGE_VERSIONS)
        glGetIntegerv(GL_NUM_SHADING_LANGUAGE_VERSIONS, &glslNumShaderVersions);
        glGetError(); // consume possible OpenGL error.
        if (glslNumShaderVersions > 0)
        {
            glslVersionMsg += " (";
            for (GLint k = 0, l = 0; k < glslNumShaderVersions; ++k)
                if (auto const str = glGetStringi(GL_SHADING_LANGUAGE_VERSION, k); str && *str)
                {
                    glslVersionMsg += (l ? ", " : "");
                    glslVersionMsg += (char const*) str;
                    l++;
                }
            glslVersionMsg += ')';
        }
#endif
        LOGSTORE(DisplayLog)(glslVersionMsg);
        logDisplayInfo();
    }
    // }}}

#if !defined(NDEBUG) && defined(GL_DEBUG_OUTPUT) && defined(CONTOUR_DEBUG_OPENGL)
    CHECKED_GL(glEnable(GL_DEBUG_OUTPUT));
    CHECKED_GL(glDebugMessageCallback(&glMessageCallback, this));
#endif

    initialized_ = true;
    session_.displayInitialized();
}

void TerminalWidget::resizeGL(int _width, int _height)
{
    QOpenGLWidget::resizeGL(_width, _height);
    auto const newPixelSize = terminal::ImageSize { Width(_width), Height(_height) };
    LOGSTORE(DisplayLog)("resizing to {}", newPixelSize);
    applyResize(newPixelSize, session_, renderer_);
}

void TerminalWidget::paintGL()
{
    try
    {
        [[maybe_unused]] auto const lastState = state_.fetchAndClear();

#if defined(CONTOUR_PERF_STATS)
        {
            ++renderCount_;
            auto const updateCount = stats_.updatesSinceRendering.exchange(0);
            auto const renderCount = stats_.consecutiveRenderCount.exchange(0);
            if (DisplayLog)
                LOGSTORE(DisplayLog)
            ("paintGL/{}: {} renders, {} updates since last paint ({}/{}).",
             renderCount_.load(),
             renderCount,
             updateCount,
             lastState,
             to_string(session_.terminal().renderBufferState()));
        }
#endif

        renderTarget_->clear(
            terminal().screen().isModeEnabled(terminal::DECMode::ReverseVideo)
                ? RGBAColor(profile().colors.defaultForeground, uint8_t(renderer_.backgroundOpacity()))
                : RGBAColor(profile().colors.defaultBackground, uint8_t(renderer_.backgroundOpacity())));
        renderer_.render(terminal(), renderingPressure_);
    }
    catch (exception const& e)
    {
        reportUnhandledException(__PRETTY_FUNCTION__, e);
    }
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
    sendKeyEvent(_keyEvent, session_);
}

void TerminalWidget::wheelEvent(QWheelEvent* _event)
{
    sendWheelEvent(_event, session_);
}

void TerminalWidget::mousePressEvent(QMouseEvent* _event)
{
    sendMousePressEvent(_event, session_);
}

void TerminalWidget::mouseMoveEvent(QMouseEvent* _event)
{
    sendMouseMoveEvent(_event, session_);
}

void TerminalWidget::mouseReleaseEvent(QMouseEvent* _event)
{
    sendMouseReleaseEvent(_event, session_);
}

void TerminalWidget::focusInEvent(QFocusEvent* _event)
{
    QOpenGLWidget::focusInEvent(_event);
    session_.sendFocusInEvent(); // TODO: paint with "normal" colors
}

void TerminalWidget::focusOutEvent(QFocusEvent* _event)
{
    QOpenGLWidget::focusOutEvent(_event);
    session_.sendFocusOutEvent(); // TODO maybe paint with "faint" colors
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
        // qDebug() << "TerminalWidget.event():" << _event;
        if (_event->type() == QEvent::Close)
        {
            session_.pty().close();
            emit terminated();
        }

        return QOpenGLWidget::event(_event);
    }
    catch (std::exception const& e)
    {
        reportUnhandledException(__PRETTY_FUNCTION__, e);
        return false;
    }
}
// }}}

// {{{ helpers
void TerminalWidget::assertInitialized()
{
    if (initialized_)
        return;

    throw std::runtime_error("Internal error. "
                             "TerminalWidget function invoked before initialization has finished.");
}

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
                // fmt::print("contentScale: forcedDPI={}\n", forcedDPI);
                if (forcedDPI >= 96.0)
                    return forcedDPI / 96.0;
            }
        }
    }
#endif

    // fmt::print("contentScale: DPI={}\n", devicePixelRatioF());
    return devicePixelRatioF();
}

void TerminalWidget::updateMinimumSize()
{
    auto const MinimumGridSize = PageSize { LineCount(2), ColumnCount(3) };
    auto const minSize = ImageSize { Width(*gridMetrics().cellSize.width * *MinimumGridSize.columns),
                                     Height(*gridMetrics().cellSize.width * *MinimumGridSize.columns) };
    setMinimumSize(minSize.width.as<int>(), minSize.height.as<int>());
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

crispy::Point TerminalWidget::screenDPI() const
{
    return systemScreenDPI() * profile().fonts.dpiScale;
}

crispy::Point TerminalWidget::systemScreenDPI() const
{
    return crispy::Point { logicalDpiX(), logicalDpiY() } * contentScale();
}

bool TerminalWidget::isFullScreen() const
{
    return window()->isFullScreen();
}

terminal::ImageSize TerminalWidget::pixelSize() const
{
    return gridMetrics().cellSize * session_.terminal().screen().pageSize();
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
    return getFontDefinition(renderer_);
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

    // clang-format off
    auto const targetBaseDir = session_.app().dumpStateAtExit().value_or(crispy::App::instance()->localStateDir() / "dump");
    auto const workDirName = FileSystem::path(fmt::format("contour-dump-{:%Y-%m-%d-%H-%M-%S}", chrono::system_clock::now()));
    auto const targetDir = targetBaseDir / workDirName;
    auto const latestDirName = FileSystem::path("latest");
    // clang-format on

    FileSystem::create_directories(targetDir);

    if (FileSystem::exists(targetBaseDir / latestDirName))
        FileSystem::remove(targetBaseDir / latestDirName);

    FileSystem::create_symlink(workDirName, targetBaseDir / latestDirName);

    LOGSTORE(DisplayLog)("Dumping state into directory: {}", targetDir.generic_string());

    // TODO: The above should be done from the outside and the targetDir being passed into this call.
    // TODO: maybe zip this dir in the end.

    // TODO: use this file store for everything that needs to be dumped.
    {
        auto const screenStateDump = [&]() {
            auto os = std::stringstream {};
            terminal().screen().inspect("Screen state dump.", os);
            renderer_.inspect(os);
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

    auto screenshotSaver = [](FileSystem::path const& _filename, ImageBufferFormat _format) {
        auto const [qImageFormat, elementCount] = [&]() -> tuple<QImage::Format, int> {
            switch (_format)
            {
            case ImageBufferFormat::RGBA: return tuple { QImage::Format_RGBA8888, 4 };
            case ImageBufferFormat::RGB: return tuple { QImage::Format_RGB888, 3 };
            case ImageBufferFormat::Alpha: return tuple { QImage::Format_Grayscale8, 1 };
            }
            return tuple { QImage::Format_Grayscale8, 1 };
        }();

        // That's a little workaround for MacOS/X's C++ Clang compiler.
        auto const theImageFormat = qImageFormat;
        auto const theElementCount = elementCount;

        return [_filename, theImageFormat, theElementCount, _format](vector<uint8_t> const& _buffer,
                                                                     ImageSize _size) {
            LOGSTORE(DisplayLog)("Saving image {} to: {}", _size, _filename.generic_string());
            auto image = QImage(_size.width.as<int>(), _size.height.as<int>(), theImageFormat);
            auto const pitch = unbox<int>(_size.width) * theElementCount;
            for (int i = 0; i < unbox<int>(_size.height); ++i)
            {
                // Vertically flip the image, because the coordinate system
                // between OpenGL and desktop screens is inverse.
                uint8_t const* sourceLine = _buffer.data() + static_cast<ptrdiff_t>(i * pitch);
                uint8_t const* sourceLineEnd = sourceLine + pitch;
                uint8_t* targetLine = image.scanLine(unbox<int>(_size.height) - 1 - i);
                copy(sourceLine, sourceLineEnd, targetLine);
            }
            image.save(QString::fromStdString(_filename.generic_string()));
        };
    };

    auto const atlasScreenshotSaver = [screenshotSaver, targetDir](vector<uint8_t> const& _buffer,
                                                                   ImageSize _size) {
        return [screenshotSaver, targetDir, &_buffer, _size](ImageBufferFormat _format) {
            auto const formatText = [&]() {
                switch (_format)
                {
                case ImageBufferFormat::RGBA: return "rgba"sv;
                case ImageBufferFormat::RGB: return "rgb"sv;
                case ImageBufferFormat::Alpha: return "alpha"sv;
                }
                return "unknown"sv;
            }();
            auto const fileName = targetDir / fmt::format("texture-atlas-{}.png", formatText);
            return screenshotSaver(fileName, _format)(_buffer, _size);
        };
    };

    terminal::renderer::RenderTarget& renderTarget = renderer_.renderTarget();

    do
    {
        auto infoOpt = renderTarget.readAtlas();
        if (!infoOpt.has_value())
            break;

        terminal::renderer::AtlasTextureScreenshot const& info = infoOpt.value();
        auto const saveScreenshot = atlasScreenshotSaver(info.buffer, info.size);
        switch (info.format)
        {
        case terminal::renderer::atlas::Format::RGBA: saveScreenshot(ImageBufferFormat::RGBA); break;
        case terminal::renderer::atlas::Format::RGB: saveScreenshot(ImageBufferFormat::RGB); break;
        case terminal::renderer::atlas::Format::Red: saveScreenshot(ImageBufferFormat::Alpha); break;
        }
    } while (0);

    renderTarget.scheduleScreenshot(
        [this, targetDir, screenshotSaver](std::vector<uint8_t> const& rgbaPixels, ImageSize imageSize) {
            auto const take = screenshotSaver(targetDir / "screenshot.png", ImageBufferFormat::RGBA);
            take(rgbaPixels, imageSize);

            // If this dump-state was triggered due to the PTY being closed
            // and a dump was requested at the end, then terminate this session here now.
            if (session_.terminal().device().isClosed() && session_.app().dumpStateAtExit().has_value())
            {
                session_.terminate();
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
    if (isFullScreen())
    {
        LOGSTORE(DisplayLog)("Application request to resize window in full screen mode denied.");
        return;
    }

    auto requestedPageSize = terminal().pageSize();
    auto const pixelSize = terminal::ImageSize { terminal::Width(*_width ? *_width : width()),
                                                 terminal::Height(*_height ? *_height : height()) };
    requestedPageSize.columns = terminal::ColumnCount(*pixelSize.width / *gridMetrics().cellSize.width);
    requestedPageSize.lines = terminal::LineCount(*pixelSize.height / *gridMetrics().cellSize.height);

    // setSizePolicy(QSizePolicy::Policy::Fixed, QSizePolicy::Policy::Fixed);
    const_cast<config::TerminalProfile&>(profile()).terminalSize = requestedPageSize;
    renderer_.setPageSize(requestedPageSize);
    auto const pixels =
        terminal::ImageSize { terminal::Width(*requestedPageSize.columns * *gridMetrics().cellSize.width),
                              terminal::Height(*requestedPageSize.lines * *gridMetrics().cellSize.height) };
    terminal().resizeScreen(requestedPageSize, pixels);
    updateGeometry();
    adaptSize_();
}

void TerminalWidget::resizeWindow(terminal::LineCount _lines, terminal::ColumnCount _columns)
{
    if (isFullScreen())
    {
        LOGSTORE(DisplayLog)("Application request to resize window in full screen mode denied.");
        return;
    }

    auto requestedPageSize = terminal().pageSize();
    if (*_columns)
        requestedPageSize.columns = _columns;
    if (*_lines)
        requestedPageSize.lines = _lines;

    // setSizePolicy(QSizePolicy::Policy::Fixed, QSizePolicy::Policy::Fixed);
    const_cast<config::TerminalProfile&>(profile()).terminalSize = requestedPageSize;
    renderer_.setPageSize(requestedPageSize);
    auto const pixels =
        terminal::ImageSize { terminal::Width(*requestedPageSize.columns * *gridMetrics().cellSize.width),
                              terminal::Height(*requestedPageSize.lines * *gridMetrics().cellSize.height) };
    terminal().resizeScreen(requestedPageSize, pixels);
    updateGeometry();
    adaptSize_();
}

void TerminalWidget::setFonts(terminal::renderer::FontDescriptions _fontDescriptions)
{
    if (applyFontDescription(
            gridMetrics().cellSize, pageSize(), pixelSize(), screenDPI(), renderer_, _fontDescriptions))
        // resize widget (same pixels, but adjusted terminal rows/columns and margin)
        applyResize(pixelSize(), session_, renderer_);
}

bool TerminalWidget::setFontSize(text::font_size _size)
{
    LOGSTORE(DisplayLog)("Setting display font size and recompute metrics: {}pt", _size.pt);

    if (!renderer_.setFontSize(_size))
        return false;

    auto currentWidgetPixelSize =
        ImageSize { terminal::Width(width()), terminal::Height(height()) };
    renderer_.setMargin(computeMargin(gridMetrics().cellSize, pageSize(), currentWidgetPixelSize));
    // resize widget (same pixels, but adjusted terminal rows/columns and margin)
    applyResize(currentWidgetPixelSize, session_, renderer_);
    updateMinimumSize();
    return true;
}

bool TerminalWidget::setPageSize(PageSize _newPageSize)
{
    if (_newPageSize == terminal().pageSize())
        return false;

    auto const viewSize =
        ImageSize { Width(*gridMetrics().cellSize.width * *profile().terminalSize.columns),
                    Height(*gridMetrics().cellSize.width * *profile().terminalSize.columns) };
    renderer_.setPageSize(_newPageSize);
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
    assertInitialized();
    window()->windowHandle()->showFullScreen();
}

void TerminalWidget::setWindowMaximized()
{
    assertInitialized();
    window()->showMaximized();
    maximizedState_ = true;
}

void TerminalWidget::setWindowNormal()
{
    assertInitialized();
    updateMinimumSize();
    window()->windowHandle()->showNormal();
    maximizedState_ = false;
}

void TerminalWidget::setBlurBehind(bool _enable)
{
    if (!enableBlurBehind_)
        return;

    enableBlurBehind_(_enable);
}

void TerminalWidget::setBackgroundImage(
    std::shared_ptr<terminal::BackgroundImage const> const& backgroundImage)
{
    renderTarget_->setBackgroundImage(backgroundImage);
}

void TerminalWidget::toggleFullScreen()
{
    assertInitialized();

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

void TerminalWidget::setHyperlinkDecoration(terminal::renderer::Decorator _normal,
                                            terminal::renderer::Decorator _hover)
{
    renderer_.setHyperlinkDecoration(_normal, _hover);
}

void TerminalWidget::setBackgroundOpacity(terminal::Opacity _opacity)
{
    renderer_.setBackgroundOpacity(_opacity);
    session_.terminal().breakLoopAndRefreshRenderBuffer();
}
// }}}

// {{{ TerminalDisplay: terminal events
void TerminalWidget::scheduleRedraw()
{
    if (!initialized_.load())
        return;

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
    LOGSTORE(DisplayLog)("closeDisplay");
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
    case Type::Main: setCursor(Qt::IBeamCursor); break;
    case Type::Alternate: setCursor(Qt::ArrowCursor); break;
    }
    emit terminalBufferChanged(_type);
    // scheduleRedraw();
}

void TerminalWidget::discardImage(terminal::Image const& _image)
{
    renderer_.discardImage(_image);
}
// }}}

} // namespace contour::opengl
