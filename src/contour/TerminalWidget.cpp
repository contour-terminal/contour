/**
 * This file is part of the "contour" project
 *   Copyright (c) 2019-2020 Christian Parpart <christian@parpart.family>
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
#include <contour/TerminalWidget.h>
#include <contour/Actions.h>
#include <contour/helper.h>

#include <terminal/Color.h>
#include <terminal/Metrics.h>
#include <terminal/pty/Pty.h>

#if defined(_MSC_VER)
#include <terminal/pty/ConPty.h>
#else
#include <terminal/pty/UnixPty.h>
#endif

#include <crispy/debuglog.h>
#include <crispy/stdfs.h>

#include <terminal_renderer/opengl/OpenGLRenderer.h>

#include <QtCore/QDebug>
#include <QtCore/QFileInfo>
#include <QtCore/QMetaObject>
#include <QtCore/QProcess>
#include <QtCore/QStandardPaths>
#include <QtCore/QTimer>
#include <QtNetwork/QHostInfo>
#include <QtGui/QClipboard>
#include <QtGui/QDesktopServices>
#include <QtGui/QKeyEvent>
#include <QtGui/QScreen>
#include <QtGui/QWindow>
#include <QtWidgets/QApplication>
#include <QtWidgets/QMessageBox>

#if defined(CONTOUR_BLUR_PLATFORM_KWIN)
#include <KWindowEffects>
#endif

#include <algorithm>
#include <cstring>
// #include <execution>
#include <fstream>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <vector>

using namespace std::string_literals;

using crispy::Size;
using std::array;
using std::cerr;
using std::chrono::steady_clock;
using std::copy;
using std::endl;
using std::exception;
using std::get;
using std::holds_alternative;
using std::ios;
using std::lock_guard;
using std::make_unique;
using std::max;
using std::move;
using std::nullopt;
using std::ofstream;
using std::optional;
using std::pair;
using std::ref;
using std::runtime_error;
using std::scoped_lock;
using std::string;
using std::string_view;
using std::tuple;
using std::vector;

using namespace std::string_view_literals;

#if defined(CONTOUR_PERF_STATS)
#define STATS_INC(name)   ++(stats_. name)
#define STATS_ZERO(name)  (stats_. name = 0)
#define STATS_GET(name)   (stats_. name).load()
#define STATS_SET(name)   (stats_. name) =
#else
#define STATS_INC(name)   do {} while (0)
#define STATS_ZERO(name)  do {} while (0)
#define STATS_GET(name)   0
#define STATS_SET(name)   /*!*/
#endif

#if defined(_MSC_VER)
#define __PRETTY_FUNCTION__ __FUNCDNAME__
#endif

namespace contour {

using actions::Action;

namespace // {{{
{
    inline char const* signalName(int _signo)
    {
#if defined(__unix__) || defined(__APPLE__)
        return strsignal(_signo);
#else
        return "unknown";
#endif
    }

    /// Maps Qt KeyInputEvent to VT input event for special keys.
#if 0 // !defined(NDEBUG)
    QDebug operator<<(QDebug str, QEvent const& ev) {
        static int eventEnumIndex = QEvent::staticMetaObject.indexOfEnumerator("Type");
        str << "QEvent";
        if (QString const name = QEvent::staticMetaObject.enumerator(eventEnumIndex).valueToKey(ev.type());
                !name.isEmpty())
            str << name;
        else
            str << ev.type();
        return str.maybeSpace();
    }
#endif

#if !defined(NDEBUG) && defined(GL_DEBUG_OUTPUT)
    void glMessageCallback(
        GLenum _source,
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
                case GL_DEBUG_SOURCE_API_ARB:
                    return "API"s;
#endif
#if defined(GL_DEBUG_SOURCE_WINDOW_SYSTEM_ARB)
                case GL_DEBUG_SOURCE_WINDOW_SYSTEM_ARB:
                    return "window system"s;
#endif
#if defined(GL_DEBUG_SOURCE_SHADER_COMPILER_ARB)
                case GL_DEBUG_SOURCE_SHADER_COMPILER_ARB:
                    return "shader compiler"s;
#endif
#if defined(GL_DEBUG_SOURCE_THIRD_PARTY_ARB)
                case GL_DEBUG_SOURCE_THIRD_PARTY_ARB:
                    return "third party"s;
#endif
#if defined(GL_DEBUG_SOURCE_APPLICATION_ARB)
                case GL_DEBUG_SOURCE_APPLICATION_ARB:
                    return "application"s;
#endif
#if defined(GL_DEBUG_SOURCE_OTHER_ARB)
                case GL_DEBUG_SOURCE_OTHER_ARB:
                    return "other"s;
#endif
                default:
                    return fmt::format("{}", _severity);
            }
        }();
        string const typeName = [&]() {
            switch (_type)
            {
#if defined(GL_DEBUG_TYPE_ERROR)
                case GL_DEBUG_TYPE_ERROR:
                    return "error"s;
#endif
#if defined(GL_DEBUG_TYPE_DEPRECATED_BEHAVIOR)
                case GL_DEBUG_TYPE_DEPRECATED_BEHAVIOR:
                    return "deprecated"s;
#endif
#if defined(GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR)
                case GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR:
                    return "undefined"s;
#endif
#if defined(GL_DEBUG_TYPE_PORTABILITY)
                case GL_DEBUG_TYPE_PORTABILITY:
                    return "portability"s;
#endif
#if defined(GL_DEBUG_TYPE_PERFORMANCE)
                case GL_DEBUG_TYPE_PERFORMANCE:
                    return "performance"s;
#endif
#if defined(GL_DEBUG_TYPE_OTHER)
                case GL_DEBUG_TYPE_OTHER:
                    return "other"s;
#endif
                default:
                    return fmt::format("{}", _severity);
            }
        }();
        string const debugSeverity = [&]() {
            switch (_severity)
            {
#if defined(GL_DEBUG_SEVERITY_LOW)
                case GL_DEBUG_SEVERITY_LOW:
                    return "low"s;
#endif
#if defined(GL_DEBUG_SEVERITY_MEDIUM)
                case GL_DEBUG_SEVERITY_MEDIUM:
                    return "medium"s;
#endif
#if defined(GL_DEBUG_SEVERITY_HIGH)
                case GL_DEBUG_SEVERITY_HIGH:
                    return "high"s;
#endif
#if defined(GL_DEBUG_SEVERITY_NOTIFICATION)
                case GL_DEBUG_SEVERITY_NOTIFICATION:
                    return "notification"s;
#endif
                default:
                    return fmt::format("{}", _severity);
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
                default:
                    return "UNKNOWN";
            }
        }(_type);

        debuglog(WidgetTag).write(
            "[OpenGL/{}]: type:{}, source:{}, severity:{}; {}",
            tag, typeName, sourceName, debugSeverity, _message
        );
    }
#endif

    std::string unhandledExceptionMessage(std::string_view const& where, exception const& e)
    {
        return fmt::format("{}: Unhandled exception caught ({}). {}", where, typeid(e).name(), e.what());
    }

    void reportUnhandledException(std::string_view const& where, exception const& e)
    {
        debuglog(WidgetTag).write("{}", unhandledExceptionMessage(where, e));
        cerr << unhandledExceptionMessage(where, e) << endl;
    }
} // }}}

TerminalWidget::TerminalWidget(config::Config _config,
                               bool _liveConfig,
                               string _profileName,
                               string _programPath) :
    QOpenGLWidget(),
    now_{ steady_clock::now() },
    config_{ std::move(_config) },
    profileName_{ std::move(_profileName) },
    profile_{ *config_.profile(profileName_) },
    programPath_{ std::move(_programPath) },
    fonts_{ profile().fonts },
    terminalView_{make_unique<terminal::view::TerminalView>(
        now_,
        *this,
        profile().maxHistoryLineCount,
        config_.wordDelimiters,
        config_.bypassMouseProtocolModifier,
        logicalDpiX(),
        logicalDpiY(),
        profile().fonts,
        profile().cursorShape,
        profile().cursorDisplay,
        profile().cursorBlinkInterval,
        profile().colors,
        profile().backgroundOpacity,
        profile().hyperlinkDecoration.normal,
        profile().hyperlinkDecoration.hover,
#if defined(_MSC_VER)
        make_unique<terminal::ConPty>(profile().terminalSize),
#else
        make_unique<terminal::UnixPty>(profile().terminalSize),
#endif
        profile().shell
    )},
    configFileChangeWatcher_{},
    updateTimer_(this)
{
    debuglog(WidgetTag).write("ctor: terminalSize={}, fontSize={}, contentScale={}, geometry={}:{}..{}:{}",
                              config_.profile(config_.defaultProfileName)->terminalSize,
                              profile().fonts.size,
                              contentScale(),
                              geometry().top(),
                              geometry().left(),
                              geometry().bottom(),
                              geometry().right());

    if (_liveConfig)
    {
        debuglog(WidgetTag).write("Enable live configuration reloading of file {}.",
                                  config_.backingFilePath.generic_string());
        configFileChangeWatcher_.emplace(config_.backingFilePath,
                                         [this](FileChangeWatcher::Event event) { onConfigReload(event); });
    }

    setMouseTracking(true);
    setFormat(surfaceFormat());

    setAttribute(Qt::WA_InputMethodEnabled, true);
    setAttribute(Qt::WA_OpaquePaintEvent);

    setMinimumSize(terminalView_->cellWidth() * 3, terminalView_->cellHeight() * 2);

    // setAttribute(Qt::WA_TranslucentBackground);
    // setAttribute(Qt::WA_NoSystemBackground, false);

    updateTimer_.setSingleShot(true);
    connect(&updateTimer_, &QTimer::timeout, this, QOverload<>::of(&TerminalWidget::blinkingCursorUpdate));

    connect(this, SIGNAL(frameSwapped()), this, SLOT(onFrameSwapped()));

    configureTerminal(*terminalView_, config_, profileName_);

    updateGeometry();
}

TerminalWidget::~TerminalWidget()
{
    debuglog(WidgetTag).write("TerminalWidget.dtor!");
    makeCurrent(); // XXX must be called.
    statsSummary();
}


int TerminalWidget::pointsToPixels(text::font_size _size) const noexcept
{
    return int(ceil(_size.pt / 72.0 * 96.0 * contentScale()));
}

void TerminalWidget::statsSummary()
{
#if defined(CONTOUR_VT_METRICS)
    std::cout << "Some small summary in VT sequences usage metrics\n";
    std::cout << "================================================\n\n";
    for (auto const& [name, freq] : terminalMetrics_.ordered())
        std::cout << fmt::format("{:>10}: {}\n", freq, name);
#endif
}

QSurfaceFormat TerminalWidget::surfaceFormat()
{
    QSurfaceFormat format;

    constexpr bool forceOpenGLES = (
#if defined(__linux__)
        true
#else
        false
#endif
    );

    if (forceOpenGLES || QOpenGLContext::openGLModuleType() == QOpenGLContext::LibGLES)
    {
        format.setVersion(3, 2);
        format.setRenderableType(QSurfaceFormat::OpenGLES);
        format.setProfile(QSurfaceFormat::CoreProfile);
    }
    else
    {
        format.setVersion(3, 3);
        format.setRenderableType(QSurfaceFormat::OpenGL);
        format.setProfile(QSurfaceFormat::CoreProfile);
    }

    format.setAlphaBufferSize(8);
    format.setSwapBehavior(QSurfaceFormat::DoubleBuffer);
    format.setSwapInterval(1);

#if !defined(NDEBUG)
    format.setOption(QSurfaceFormat::DebugContext);
#endif

    return format;
}

void TerminalWidget::blinkingCursorUpdate()
{
    update();
}

void TerminalWidget::onFrameSwapped()
{
#if defined(CONTOUR_PERF_STATS)
    qDebug() << QString::fromStdString(fmt::format(
        "Consecutive renders: {}, updates since last render: {}; {}",
        STATS_GET(consecutiveRenderCount),
        STATS_GET(updatesSinceRendering),
        terminalView_->renderer().metrics().to_string()
    ));
#endif

    for (;;)
    {
        auto state = state_.load();
        switch (state)
        {
            case State::DirtyIdle:
                //assert(!"The impossible happened, painting but painting. Shakesbeer.");
                qDebug() << "The impossible happened, onFrameSwapped() called in wrong state DirtyIdle.";
                renderingPressure_ = false;
                update();
                return;
            case State::DirtyPainting:
                // FIXME: Qt/Wayland!
                // QCoreApplication::postEvent() works on both, but horrorble performance
                // requestUpdate() works on X11 as well as Wayland but isn't mentioned in any QtGL docs.
                // update() is meant to be correct way and works on X11 but freezes on Wayland!

                //QCoreApplication::postEvent(this, new QEvent(QEvent::UpdateRequest));
                //requestUpdate();
                // TODO(don't do pressure-optimizations right now) renderingPressure_ = true;
                update();
                return;
            case State::CleanPainting:
                if (!state_.compare_exchange_strong(state, State::CleanIdle))
                    break;
                [[fallthrough]];
            case State::CleanIdle:
                renderingPressure_ = false;
                STATS_ZERO(consecutiveRenderCount);
                if (profile().cursorDisplay == terminal::CursorDisplay::Blink
                        && terminalView_->terminal().cursorVisibility())
                    updateTimer_.start(terminalView_->terminal().nextRender(steady_clock::now()));
                return;
        }
    }
}

void TerminalWidget::initializeGL()
{
    initializeOpenGLFunctions();

    renderTarget_ = make_unique<terminal::renderer::opengl::OpenGLRenderer>(
        *config::Config::loadShaderConfig(config::ShaderClass::Text),
        *config::Config::loadShaderConfig(config::ShaderClass::Background),
        Size{width(), height()},
        0, // TODO left margin
        0 // TODO bottom margin
    );

    terminalView_->setRenderTarget(*renderTarget_);

    // {{{ some info
    static bool infoPrinted = false;
    if (!infoPrinted)
    {
        infoPrinted = true;
        debuglog(WidgetTag).write("[FYI] DPI             : {}x{} physical; {}x{} logical",
                         physicalDpiX(), physicalDpiY(),
                         logicalDpiX(), logicalDpiY());
        debuglog(WidgetTag).write("[FYI] Font size       : {}pt ({}px)", profile().fonts.size, pointsToPixels(profile().fonts.size));
        debuglog(WidgetTag).write("[FYI] OpenGL type     : {}", (QOpenGLContext::currentContext()->isOpenGLES() ? "OpenGL/ES" : "OpenGL"));
        debuglog(WidgetTag).write("[FYI] OpenGL renderer : {}", glGetString(GL_RENDERER));
        debuglog(WidgetTag).write("[FYI] Qt platform     : {}", QGuiApplication::platformName().toStdString());

        GLint versionMajor{};
        GLint versionMinor{};
        QOpenGLContext::currentContext()->functions()->glGetIntegerv(GL_MAJOR_VERSION, &versionMajor);
        QOpenGLContext::currentContext()->functions()->glGetIntegerv(GL_MINOR_VERSION, &versionMinor);
        debuglog(WidgetTag).write("[FYI] OpenGL version  : {}.{}", versionMajor, versionMinor);

        auto glslVersionMsg = fmt::format("[FYI] GLSL version    : {}", glGetString(GL_SHADING_LANGUAGE_VERSION));

        // TODO: pass phys()/logical?) dpi to font manager, so font size can be applied right
        // TODO: also take window monitor switches into account

        GLint glslNumShaderVersions{};
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
        debuglog(WidgetTag).write(glslVersionMsg);
    }
    // }}}

#if !defined(NDEBUG) && defined(GL_DEBUG_OUTPUT)
    CHECKED_GL( glEnable(GL_DEBUG_OUTPUT) );
    // CHECKED_GL( glDebugMessageCallback(&glMessageCallback, this) );
#endif

    if (profile_.maximized)
        window()->showMaximized();

    if (profile_.fullscreen)
    {
        maximizedState_ = window()->isMaximized();
        window()->showFullScreen();
    }
}

void TerminalWidget::resizeGL(int _width, int _height)
{
    debuglog(WidgetTag).write("resizing to {}", Size{_width, _height});
    QOpenGLWidget::resizeGL(_width, _height);

    if (_width == 0 || _height == 0)
        return;

    // debuglog(WidgetTag).write("widget: {}, geometry: {}/{}",
    //                           Size{_width, _height},
    //                           Size{geometry().top(), geometry().left()},
    //                           Size{geometry().width(), geometry().height()});

    terminalView_->resize(Size{_width, _height});
}

void TerminalWidget::paintGL()
{
// #if defined(CONTOUR_PERF_STATS)
//     qDebug() << QString::fromStdString(fmt::format(
//         "paintGL: consecutive renders: {}, updates since last render: {}; {}",
//         STATS_GET(consecutiveRenderCount),
//         STATS_GET(updatesSinceRendering),
//         terminalView_->renderer().metrics().to_string()
//     ));
// #endif

    try {
        STATS_INC(consecutiveRenderCount);
        state_.store(State::CleanPainting);
        now_ = steady_clock::now();

        bool const reverseVideo =
            terminalView_->terminal().screen().isModeEnabled(terminal::DECMode::ReverseVideo);

        auto const bg =
            reverseVideo
                ? terminal::RGBAColor(profile().colors.defaultForeground, uint8_t(profile().backgroundOpacity))
                : terminal::RGBAColor(profile().colors.defaultBackground, uint8_t(profile().backgroundOpacity));

        if (bg != renderStateCache_.backgroundColor)
        {
            auto const clearColor = array<float, 4>{
                float(bg.red()) / 255.0f,
                float(bg.green()) / 255.0f,
                float(bg.blue()) / 255.0f,
                float(bg.alpha()) / 255.0f
            };
            glClearColor(clearColor[0], clearColor[1], clearColor[2], clearColor[3]);
            renderStateCache_.backgroundColor = bg;
        }

        glClear(GL_COLOR_BUFFER_BIT);

        //terminal::view::render(terminalView_, now_);
        STATS_SET(updatesSinceRendering) terminalView_->render(now_, renderingPressure_);
    }
    catch (exception const& e)
    {
        reportUnhandledException(__PRETTY_FUNCTION__, e);
    }
}

bool TerminalWidget::resetConfig()
{
    auto const ec = config::createDefaultConfig(config_.backingFilePath);
    if (ec)
    {
        cerr << fmt::format("Failed to load default config at {}; ({}) {}\n",
                            config_.backingFilePath.string(),
                            ec.category().name(),
                            ec.message());
        return false;
    }

    config::Config defaultConfig;
    try
    {
        config::loadConfigFromFile(config_.backingFilePath);
    }
    catch (std::exception const& e)
    {
        debuglog(WidgetTag).write("Failed to load default config: {}", e.what());
    }

    return reloadConfig(defaultConfig, defaultConfig.defaultProfileName);
}

bool TerminalWidget::reloadConfigWithProfile(std::string const& _profileName)
{
    auto newConfig = config::Config{};
    auto configFailures = int{0};
    auto const configLogger = [&](string const& _msg)
    {
        cerr << "Configuration failure. " << _msg << '\n';
        ++configFailures;
    };

    try
    {
        loadConfigFromFile(newConfig, config_.backingFilePath.string());
    }
    catch (exception const& e)
    {
        //TODO: logger_.error(e.what());
        configLogger(unhandledExceptionMessage(__PRETTY_FUNCTION__, e));
    }

    if (!newConfig.profile(_profileName))
        configLogger(fmt::format("Currently active profile with name '{}' gone.", _profileName));

    if (configFailures)
    {
        cerr << "Failed to load configuration.\n";
        return false;
    }

    return reloadConfig(std::move(newConfig), _profileName);
}

bool TerminalWidget::reloadConfig(config::Config _newConfig, string const& _profileName)
{
    debuglog(WidgetTag).write("Reloading configuration from {} with profile {}",
                              _newConfig.backingFilePath.string(),
                              _profileName);

    configureTerminal(*terminalView_, _newConfig, _profileName);

    config_ = std::move(_newConfig);
    profileName_ = _profileName;

    return true;
}

void TerminalWidget::keyPressEvent(QKeyEvent* _keyEvent)
{
    auto const keySeq = toKeySequence(_keyEvent);

    debuglog(KeyboardTag).write(
        "text:{}, seq:{}, seqEmpty?:{}, key:0x{:X}, mod:0x{:X}, keySeq[0]:{}",
         _keyEvent->text().toStdString(),
         keySeq.toString().toStdString(),
         keySeq.isEmpty(),
         static_cast<Qt::Key>(_keyEvent->key()),
         _keyEvent->modifiers(),
         keySeq[0]
    );

    if (auto const i = config_.keyMappings.find(keySeq); i != end(config_.keyMappings))
    {
        executeAllActions(i->second);
        return;
    }

    if (auto const inputEvent = mapQtToTerminalKeyEvent(_keyEvent->key(), _keyEvent->modifiers()))
    {
        terminalView_->terminal().send(*inputEvent, now_);
        scrollToBottomAndRedraw();
        return;
    }

    if (cursor().shape() != Qt::CursorShape::BlankCursor)
        setCursor(Qt::CursorShape::BlankCursor);

    auto const modifiers = makeModifier(_keyEvent->modifiers());

    if (_keyEvent->key() && modifiers.any() && !modifiers.shift())
    {
        if (_keyEvent->key() >= 'A' && _keyEvent->key() <= 'Z')
        {
            terminalView_->terminal().send(
                terminal::CharInputEvent{
                    static_cast<char32_t>(tolower(_keyEvent->key())),
                    modifiers
                },
                now_
            );
            return;
        }
    }

    if (!_keyEvent->text().isEmpty())
    {
        for (auto const ch : _keyEvent->text().toUcs4())
            terminalView_->terminal().send(terminal::CharInputEvent{ch, modifiers}, now_);

        scrollToBottomAndRedraw();
    }
}

void TerminalWidget::wheelEvent(QWheelEvent* _event)
{
    auto const button = _event->delta() > 0 ? terminal::MouseButton::WheelUp : terminal::MouseButton::WheelDown;
    auto const mouseEvent = terminal::MousePressEvent{button, makeModifier(_event->modifiers())};

    executeInput(mouseEvent);
}

bool TerminalWidget::executeInput(terminal::MouseEvent const& _mouseEvent)
{
    now_ = steady_clock::now();

    if (auto mapping = config_.mouseMappings.find(_mouseEvent); mapping != config_.mouseMappings.end())
    {
        bool const handled = executeAllActions(mapping->second);
        if (handled)
            return true;
    }

    // No input mapping found, forward event.
    return terminalView_->terminal().send(_mouseEvent, now_);
}

void TerminalWidget::mousePressEvent(QMouseEvent* _event)
{
    auto const mouseButton = makeMouseButton(_event->button());
    auto const handled = executeInput(terminal::MousePressEvent{mouseButton, makeModifier(_event->modifiers())});

    // Force redraw if the event was handled.
    // This includes selection intiiation as well as selection clearing actions.
    if (handled)
    {
        setScreenDirty();
        update();
    }
}

void TerminalWidget::mouseReleaseEvent(QMouseEvent* _event)
{
    auto const mouseButton = makeMouseButton(_event->button());
    auto const handled = executeInput(terminal::MouseReleaseEvent{mouseButton});

    if (handled)
    {
        setScreenDirty();
        update();
    }
}

void TerminalWidget::mouseMoveEvent(QMouseEvent* _event)
{
    now_ = steady_clock::now();

    auto constexpr MarginTop = 0;
    auto constexpr MarginLeft = 0;

    auto const row = int{1 + (max(_event->y(), 0) - MarginTop) / terminalView_->cellHeight()};
    auto const col = int{1 + (max(_event->x(), 0) - MarginLeft) / terminalView_->cellWidth()};
    auto const mod = makeModifier(_event->modifiers());

    {
        auto const _l = scoped_lock{terminalView_->terminal()};
        auto const currentMousePosition = terminalView_->terminal().currentMousePosition();
        auto const currentMousePositionRel = terminal::Coordinate{
            currentMousePosition.row - terminalView_->terminal().viewport().relativeScrollOffset(),
            currentMousePosition.column
        };

        if (terminalView_->terminal().screen().contains(currentMousePosition))
        {
            if (terminalView_->terminal().screen().at(currentMousePositionRel).hyperlink())
                setCursor(Qt::CursorShape::PointingHandCursor);
            else
                setDefaultCursor();
        }
    }

    auto const handled = terminalView_->terminal().send(terminal::MouseMoveEvent{row, col, mod}, now_);

    // XXX always update as we don't know if a hyperlink is visible and its hover-state has changed.
    // We could implement an actual check by keeping track of how many grid cells do contain a
    // hyperlink whose number eventually updates upon every cell write.
    bool constexpr hyperlinkVisible = true;

    if (hyperlinkVisible || handled || terminalView_->terminal().isSelectionAvailable()) // && only if selection has changed!
    {
        setScreenDirty();
        update();
    }
}

void TerminalWidget::setDefaultCursor()
{
    using Type = terminal::ScreenType;
    switch (terminalView_->terminal().screen().bufferType())
    {
        case Type::Main:
            setCursor(Qt::IBeamCursor);
            break;
        case Type::Alternate:
            setCursor(Qt::ArrowCursor);
            break;
    }
}

void TerminalWidget::scrollToBottomAndRedraw()
{
    auto const dirty = terminalView_->terminal().viewport().scrollToBottom();
    if (dirty)
    {
        setScreenDirty();
        update();
    }
}

void TerminalWidget::focusInEvent(QFocusEvent* _event) // TODO: paint with "normal" colors
{
    QOpenGLWidget::focusInEvent(_event);

    // as per Qt-documentation, some platform implementations reset the cursor when leaving the
    // window, so we have to re-apply our desired cursor in focusInEvent().
    if (cursor().shape() != Qt::CursorShape::BlankCursor)
        setDefaultCursor();
    else
        setCursor(Qt::CursorShape::BlankCursor);

    terminalView_->terminal().screen().setFocus(true);
    terminalView_->terminal().send(terminal::FocusInEvent{}, now_);

    emit setBackgroundBlur(profile_.backgroundBlur);

    // force redraw because of setFocus()-change otherwise sometimes not being shown in realtime
    setScreenDirty();
    update();
}

void TerminalWidget::focusOutEvent(QFocusEvent* _event) // TODO maybe paint with "faint" colors
{
    QOpenGLWidget::focusOutEvent(_event);

    terminalView_->terminal().screen().setFocus(false);
    terminalView_->terminal().send(terminal::FocusOutEvent{}, now_);

    // force redraw because of setFocus()-change otherwise sometimes not being shown in realtime
    setScreenDirty();
    update();
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
    const QPoint cursorPos = QPoint(); // TODO: cursorPosition();
    switch (_query) {
    // TODO?: case Qt::ImCursorRectangle:
    // case Qt::ImMicroFocus:
    //     return imageToWidget(QRect(cursorPos.x(), cursorPos.y(), 1, 1));
    case Qt::ImFont:
        return font();
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
    case Qt::ImCurrentSelection:
        return QString();
    default:
        break;
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
            terminalView_->process().terminate(terminal::Process::TerminationHint::Hangup);

            emit terminated(this);
        }

        return QOpenGLWidget::event(_event);
    }
    catch (std::exception const& e)
    {
        reportUnhandledException(__PRETTY_FUNCTION__, e);
        return false;
    }
}

bool TerminalWidget::fullscreen() const
{
    return window()->isFullScreen();
    // return window_.isFullScreen();
}

void TerminalWidget::toggleFullscreen()
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

bool TerminalWidget::setFontSize(text::font_size _fontSize)
{
    if (_fontSize.pt < 5.) // Let's not be crazy.
        return false;

    if (_fontSize.pt > 200.)
        return false;

    terminalView_->setFontSize(_fontSize);
    profile().fonts.size = _fontSize;

    setMinimumSize(terminalView_->cellWidth() * 3, terminalView_->cellHeight() * 2);

    return true;
}

bool TerminalWidget::executeAllActions(std::vector<actions::Action> const& _actions)
{
    auto handled = false;

    for (actions::Action const& action : _actions)
        handled = executeAction(action) || handled;

    return handled;
}

bool TerminalWidget::executeAction(Action const& _action)
{
    enum class Result { Nothing, Silently, Dirty };
    auto constexpr OnePt = text::font_size{ 1.0 };

    auto const postScroll = [this](bool _dirty) -> Result {
        if (_dirty)
            emit viewportChanged(this);
        return _dirty ? Result::Dirty : Result::Nothing;
    };

    Result const result = visit(overloaded{
        [&](actions::WriteScreen const& _write) -> Result {
            terminalView_->terminal().writeToScreen(_write.chars);
            return Result::Silently;
        },
        [&](actions::ToggleFullscreen) -> Result {
            toggleFullscreen();
            return Result::Silently;
        },
        [&](actions::IncreaseFontSize) -> Result {
            setFontSize(profile().fonts.size + OnePt);
            return Result::Dirty;
        },
        [&](actions::DecreaseFontSize) -> Result {
            setFontSize(profile().fonts.size - OnePt);
            return Result::Dirty;
        },
        [&](actions::IncreaseOpacity) -> Result {
            if (static_cast<uint8_t>(profile().backgroundOpacity) < 0xFF)
            {
                ++profile().backgroundOpacity;
                terminalView_->setBackgroundOpacity(profile().backgroundOpacity);
                return Result::Dirty;
            }
            return Result::Nothing;
        },
        [&](actions::DecreaseOpacity) -> Result {
            if (static_cast<uint8_t>(profile().backgroundOpacity) > 0)
            {
                --profile().backgroundOpacity;
                terminalView_->setBackgroundOpacity(profile().backgroundOpacity);
                return Result::Dirty;
            }
            return Result::Nothing;
        },
        [&](actions::ScreenshotVT) -> Result {
            auto _l = lock_guard{ terminalView_->terminal() };
            auto const screenshot = terminalView_->terminal().screen().screenshot();
            ofstream ofs{ "screenshot.vt", ios::trunc | ios::binary };
            ofs << screenshot;
            return Result::Silently;
        },
        [this](actions::SendChars const& chars) -> Result {
            for (auto const ch : chars.chars)
                terminalView_->terminal().send(terminal::CharInputEvent{static_cast<char32_t>(ch), terminal::Modifier::None}, now_);
            return Result::Silently;
        },
        [this, postScroll](actions::ScrollOneUp) -> Result {
            return postScroll(terminalView_->terminal().viewport().scrollUp(1));
        },
        [this, postScroll](actions::ScrollOneDown) -> Result {
            return postScroll(terminalView_->terminal().viewport().scrollDown(1));
        },
        [this, postScroll](actions::ScrollUp) -> Result {
            return postScroll(terminalView_->terminal().viewport().scrollUp(profile().historyScrollMultiplier));
        },
        [this, postScroll](actions::ScrollDown) -> Result {
            return postScroll(terminalView_->terminal().viewport().scrollDown(profile().historyScrollMultiplier));
        },
        [this, postScroll](actions::ScrollPageUp) -> Result {
            return postScroll(terminalView_->terminal().viewport().scrollUp(profile().terminalSize.height / 2));
        },
        [this, postScroll](actions::ScrollPageDown) -> Result {
            return postScroll(terminalView_->terminal().viewport().scrollDown(profile().terminalSize.height / 2));
        },
        [this, postScroll](actions::ScrollMarkUp) -> Result {
            return postScroll(terminalView_->terminal().viewport().scrollMarkUp());
        },
        [this, postScroll](actions::ScrollMarkDown) -> Result {
            return postScroll(terminalView_->terminal().viewport().scrollMarkDown());
        },
        [this, postScroll](actions::ScrollToTop) -> Result {
            return postScroll(terminalView_->terminal().viewport().scrollToTop());
        },
        [this, postScroll](actions::ScrollToBottom) -> Result {
            return postScroll(terminalView_->terminal().viewport().scrollToBottom());
        },
        [this](actions::CopyPreviousMarkRange) -> Result {
            copyToClipboard(terminalView_->terminal().extractLastMarkRange());
            return Result::Silently;
        },
        [this](actions::CopySelection) -> Result {
            string const text = terminalView_->terminal().extractSelectionText();
            if (QClipboard* clipboard = QGuiApplication::clipboard(); clipboard != nullptr)
                clipboard->setText(QString::fromUtf8(text.c_str(), static_cast<int>(text.size())));
            return Result::Silently;
        },
        [this](actions::PasteSelection) -> Result {
            if (QClipboard* clipboard = QGuiApplication::clipboard(); clipboard != nullptr)
            {
                string const text = clipboard->text(QClipboard::Selection).toUtf8().toStdString();
                terminalView_->terminal().sendPaste(string_view{text});
            }
            return Result::Silently;
        },
        [this](actions::PasteClipboard) -> Result {
            if (QClipboard* clipboard = QGuiApplication::clipboard(); clipboard != nullptr)
            {
                string const text = clipboard->text(QClipboard::Clipboard).toUtf8().toStdString();
                terminalView_->terminal().sendPaste(string_view{text});
            }
            return Result::Silently;
        },
        [this](actions::ChangeProfile const& v) -> Result {
            if (v.name != profileName_)
            {
                activateProfile(v.name);
                return Result::Dirty;
            }
            else
                return Result::Silently;
        },
        [this](actions::NewTerminal const& v) -> Result {
            spawnNewTerminal(v.profileName.value_or(profileName_));
            return Result::Silently;
        },
        [this](actions::OpenConfiguration) -> Result {
            if (!QDesktopServices::openUrl(QUrl(QString::fromUtf8(config_.backingFilePath.string().c_str()))))
                cerr << "Could not open configuration file \"" << config_.backingFilePath << "\"" << endl;
            return Result::Silently;
        },
        [this](actions::OpenFileManager) -> Result {
            auto const _l = scoped_lock{terminalView_->terminal()};
            auto const& cwd = terminalView_->terminal().screen().currentWorkingDirectory();
            if (!QDesktopServices::openUrl(QUrl(QString::fromUtf8(cwd.c_str()))))
                cerr << "Could not open file \"" << cwd << "\"" << endl;
            return Result::Silently;
        },
        [this](actions::Quit) -> Result {
            //TODO: later warn here when more then one terminal view is open
            terminalView_->terminal().device().close();
            exit(EXIT_SUCCESS);
            return Result::Silently;
        },
        [this](actions::ResetFontSize) -> Result {
            setFontSize(config_.profile(profileName_)->fonts.size);
            return Result::Dirty;
        },
        [this](actions::ReloadConfig const& action) -> Result {
            if (action.profileName.has_value())
                return reloadConfigWithProfile(action.profileName.value()) ? Result::Dirty : Result::Nothing;
            else
                return reloadConfigWithProfile(profileName_) ? Result::Dirty : Result::Nothing;
        },
        [this](actions::ResetConfig) -> Result {
            if (resetConfig())
                return Result::Dirty;
            else
                return Result::Silently;
        },
        [this](actions::FollowHyperlink) -> Result {
            auto const _l = scoped_lock{terminalView_->terminal()};
            auto const currentMousePosition = terminalView_->terminal().currentMousePosition();
            auto const currentMousePositionRel = terminal::Coordinate{
                currentMousePosition.row - terminalView_->terminal().viewport().relativeScrollOffset(),
                currentMousePosition.column
            };
            if (terminalView_->terminal().screen().contains(currentMousePosition))
            {
                if (auto hyperlink = terminalView_->terminal().screen().at(currentMousePositionRel).hyperlink(); hyperlink != nullptr)
                {
                    followHyperlink(*hyperlink);
                    return Result::Silently;
                }
            }
            return Result::Nothing;
        }
    }, _action);

    switch (result)
    {
        case Result::Dirty:
            setScreenDirty();
            update();
            return true;
        case Result::Silently:
            return true;
        case Result::Nothing:
            break;
    }
    return false;
}

void TerminalWidget::followHyperlink(terminal::HyperlinkInfo const& _hyperlink)
{
    auto const fileInfo = QFileInfo(QString::fromStdString(std::string(_hyperlink.path())));
    auto const isLocal = _hyperlink.isLocal() && _hyperlink.host() == QHostInfo::localHostName().toStdString();
    auto const editorEnv = getenv("EDITOR");

    if (isLocal && fileInfo.isFile() && fileInfo.isExecutable())
    {
        QStringList args;
        args.append("config");
        args.append(QString::fromStdString(config_.backingFilePath.string()));
        args.append(QString::fromUtf8(_hyperlink.path().data(), static_cast<int>(_hyperlink.path().size())));
        QProcess::execute(QString::fromStdString(programPath_), args);
    }
    else if (isLocal && fileInfo.isFile() && editorEnv && *editorEnv)
    {
        QStringList args;
        args.append("config");
        args.append(QString::fromStdString(config_.backingFilePath.string()));
        args.append(QString::fromStdString(editorEnv));
        args.append(QString::fromUtf8(_hyperlink.path().data(), static_cast<int>(_hyperlink.path().size())));
        QProcess::execute(QString::fromStdString(programPath_), args);
    }
    else if (isLocal)
        QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromUtf8(std::string(_hyperlink.path()).c_str())));
    else
        QDesktopServices::openUrl(QString::fromUtf8(_hyperlink.uri.c_str()));
}

void TerminalWidget::activateProfile(string const& _newProfileName)
{
    if (auto newProfile = config_.profile(_newProfileName); newProfile)
    {
        debuglog(WidgetTag).write("Changing profile to '{}'.", _newProfileName);
        activateProfile(_newProfileName, *newProfile);
    }
    else
        debuglog(WidgetTag).write("Cannot change profile. No such profile: '{}'.", _newProfileName);
}

void TerminalWidget::activateProfile(string const& _name, config::TerminalProfile newProfile)
{
    setMinimumSize(terminalView_->cellWidth() * 3, terminalView_->cellHeight() * 2);

    if (newProfile.backgroundBlur != profile().backgroundBlur)
        emit setBackgroundBlur(newProfile.backgroundBlur);

    if (newProfile.maximized)
        window()->showMaximized();
    else
        window()->showNormal();

    if (newProfile.fullscreen != window()->isFullScreen())
        toggleFullscreen();

    profile_ = std::move(newProfile);
    profileName_ = _name;

    emit profileChanged(this);
}

void TerminalWidget::spawnNewTerminal(std::string const& _profileName)
{
    ::contour::spawnNewTerminal(
        programPath_,
        config_.backingFilePath.generic_string(),
        _profileName,
        [terminal = &terminalView_->terminal()]() -> string {
            auto const _l = scoped_lock{*terminal};
            return terminal->screen().currentWorkingDirectory();
        }()
    );
}

float TerminalWidget::contentScale() const
{
    if (!window()->windowHandle())
        return 1.0f;

    return window()->windowHandle()->screen()->devicePixelRatio();
}

void TerminalWidget::onConfigReload(FileChangeWatcher::Event /*_event*/)
{
    post([this]() { reloadConfigWithProfile(profileName_); });

    if (setScreenDirty())
        update();
}

void TerminalWidget::post(std::function<void()> _fn)
{
    postToObject(this, std::move(_fn));
}

// {{{ TerminalView::Events overrides

void TerminalWidget::bell()
{
    debuglog(WidgetTag).write("TODO: Beep!");
    QApplication::beep();
    // QApplication::beep() requires Qt Widgets dependency. doesn't suound good.
    // so maybe just a visual bell then? That would require additional OpenGL/shader work then though.
}

void TerminalWidget::notify(std::string_view const& _title, std::string_view const& _content)
{
    emit showNotification(QString::fromUtf8(_title.data(), static_cast<int>(_title.size())),
                          QString::fromUtf8(_content.data(), static_cast<int>(_content.size())));
}

void TerminalWidget::reply(std::string_view const& _reply)
{
    post([this, data = string(_reply)]() {
        terminalView_->terminal().sendRaw(data);
    });
}

void TerminalWidget::setWindowTitle(std::string_view const& _title)
{
    post([this, terminalTitle = string(_title)]() {
        auto const title = terminalTitle.empty()
            ? "contour"s
            : fmt::format("{} - contour", terminalTitle);
        if (window()->windowHandle())
            window()->windowHandle()->setTitle(QString::fromUtf8(title.c_str()));
    });
}

void TerminalWidget::setTerminalProfile(std::string const& _configProfileName)
{
    post([this, name = string(_configProfileName)]() {
        activateProfile(name);
    });
}

void TerminalWidget::onSelectionComplete()
{
    if (QClipboard* clipboard = QGuiApplication::clipboard(); clipboard != nullptr)
    {
        string const text = terminalView_->terminal().extractSelectionText();
        clipboard->setText(QString::fromUtf8(text.c_str(), static_cast<int>(text.size())), QClipboard::Selection);
    }
}

void TerminalWidget::bufferChanged(terminal::ScreenType _type)
{
    currentScreenType_ = _type;
    post([this, _type] ()
    {
        setDefaultCursor();
        emit terminalBufferChanged(this, _type);
    });

    if (setScreenDirty())
        update();
}

void TerminalWidget::screenUpdated()
{
#if defined(CONTOUR_VT_METRICS)
    // TODO
    // for (auto const& command : _commands)
    //     terminalMetrics_(command);
#endif

    if (terminalView_->terminal().screen().isPrimaryScreen())
    {
        post([this]()
        {
            if (profile().autoScrollOnUpdate && terminalView_->terminal().viewport().scrolled())
                terminalView_->terminal().viewport().scrollToBottom();
            emit screenUpdated(this);
        });
    }

    if (setScreenDirty())
        update(); //QCoreApplication::postEvent(this, new QEvent(QEvent::UpdateRequest));
}

void TerminalWidget::onScrollBarValueChanged(int _value)
{
    view()->terminal().viewport().scrollToAbsolute(_value);
    if (setScreenDirty())
        update();
}

void TerminalWidget::resizeWindow(int _width, int _height, bool _inPixels)
{
    debuglog(WidgetTag).write("Application request to resize window: {}x{} {}", _width, _height, _inPixels ? "px" : "cells");

    if (fullscreen())
    {
        cerr << "Application request to resize window in full screen mode denied." << endl;
    }
    else if (_inPixels)
    {
        auto const screenSize = size();

        if (!_width)
            _width = screenSize.width();

        if (!_height)
            _height = screenSize.height();

        auto const width = _width / gridMetrics().cellSize.width;
        auto const height = _height / gridMetrics().cellSize.height;
        auto const newScreenSize = Size{width, height};
        post([this, newScreenSize]() { setSize(newScreenSize); });
    }
    else
    {
        if (!_width)
            _width = profile().terminalSize.width;

        if (!_height)
            _height = profile().terminalSize.height;

        auto const newScreenSize = Size{_width, _height};
        post([this, newScreenSize]() { setSize(newScreenSize); });
    }
}

QSize TerminalWidget::minimumSizeHint() const
{
    auto constexpr MinimumScreenSize = Size{1, 1};

    auto const cellSize = gridMetrics().cellSize;
    auto const viewSize = MinimumScreenSize * cellSize;

    debuglog(WidgetTag).write("{}", viewSize);

    return QSize(viewSize.width, viewSize.height);
}

QSize TerminalWidget::sizeHint() const
{
    auto const cellSize = terminalView_->renderer().gridMetrics().cellSize;
    auto const viewSize = cellSize * profile().terminalSize;

    debuglog(WidgetTag).write(
        "hint: {}, cellSize: {}, terminalSize: {}",
        viewSize,
        cellSize,
        profile().terminalSize
    );

    return QSize(viewSize.width, viewSize.height);
}

void TerminalWidget::setSize(Size _size)
{
    debuglog(WidgetTag).write("Calling setSize with {}", _size);

    profile().terminalSize = _size;
    terminalView_->setTerminalSize(profile().terminalSize);

    updateGeometry();

    if (setScreenDirty())
        update();
}

void TerminalWidget::onClosed()
{
    using terminal::Process;

    // TODO: silently quit instantly when window/terminal has been spawned already since N seconds.
    // This message should only be printed for "fast" terminal terminations.

    terminalView_->waitForProcessExit();
    terminal::Process::ExitStatus const ec = *terminalView_->process().checkStatus();
    if (holds_alternative<Process::SignalExit>(ec))
        terminalView_->terminal().writeToScreen(fmt::format("\r\nShell has terminated with signal {} ({}).",
                                                            get<Process::SignalExit>(ec).signum,
                                                            signalName(get<Process::SignalExit>(ec).signum)));
    else if (auto const normalExit = get<Process::NormalExit>(ec); normalExit.exitCode != EXIT_SUCCESS)
        terminalView_->terminal().writeToScreen(fmt::format("\r\nShell has terminated with exit code {}.",
                                                            normalExit.exitCode));
    else
        close(); // TODO: call this only from within the GUI thread!
}

void TerminalWidget::requestCaptureBuffer(int _absoluteStartLine, int _lineCount)
{
    post([this, _absoluteStartLine, _lineCount]() {
        if (requestPermission(profile().permissions.captureBuffer, "capture screen buffer"))
        {
            terminalView_->terminal().screen().captureBuffer(_absoluteStartLine, _lineCount);
        }
    });
}

void TerminalWidget::setFontDef(terminal::FontDef const& _fontDef)
{
    post([this, spec = terminal::FontDef(_fontDef)]() {
        if (requestPermission(profile().permissions.changeFont, "changing font"))
        {
            auto const& currentFonts = terminalView_->renderer().fontDescriptions();
            terminal::renderer::FontDescriptions newFonts = currentFonts;

            if (spec.size != 0.0)
                newFonts.size = text::font_size{ spec.size };

            if (!spec.regular.empty())
                newFonts.regular = currentFonts.regular;

            auto const styledFont = [&](string_view _font) -> text::font_description {
                // if a styled font is "auto" then infer froom regular font"
                if (_font == "auto"sv)
                    return currentFonts.regular;
                else
                    return text::font_description::parse(_font);
            };

            if (!spec.bold.empty())
                fonts_.bold = styledFont(spec.bold);

            if (!spec.italic.empty())
                fonts_.italic = styledFont(spec.italic);

            if (!spec.boldItalic.empty())
                fonts_.boldItalic = styledFont(spec.boldItalic);

            if (!spec.emoji.empty() && spec.emoji != "auto"sv)
                fonts_.emoji = text::font_description::parse(spec.emoji);

            terminalView_->renderer().setFonts(newFonts);
        }
    });
}

bool TerminalWidget::requestPermission(config::Permission _allowedByConfig, string_view _topicText)
{
    switch (_allowedByConfig)
    {
        case config::Permission::Allow:
            debuglog(WidgetTag).write("Permission for {} allowed by configuration.", _topicText);
            return true;
        case config::Permission::Deny:
            debuglog(WidgetTag).write("Permission for {} denied by configuration.", _topicText);
            return false;
        case config::Permission::Ask:
            break;
    }

    // Did we remember a last interactive question?
    if (auto const i = rememberedPermissions_.mapping.find(string(_topicText)); i != rememberedPermissions_.mapping.end())
        return i->second;

    debuglog(WidgetTag).write("Permission for {} requires asking user.", _topicText);

    auto const reply = QMessageBox::question(this,
        fmt::format("{} requested", _topicText).c_str(),
        QString::fromStdString(fmt::format("The application has requested for {}. Do you allow this?", _topicText)),
        QMessageBox::StandardButton::Yes
            | QMessageBox::StandardButton::YesToAll
            | QMessageBox::StandardButton::No
            | QMessageBox::StandardButton::NoToAll,
        QMessageBox::StandardButton::NoButton
    );

    switch (reply)
    {
        case QMessageBox::StandardButton::NoToAll:
            rememberedPermissions_.mapping[string(_topicText)] = false;
            break;
        case QMessageBox::StandardButton::YesToAll:
            rememberedPermissions_.mapping[string(_topicText)] = true;
            [[fallthrough]];
        case QMessageBox::StandardButton::Yes:
            return true;
        default:
            break;
    }

    return false;
}

void TerminalWidget::copyToClipboard(std::string_view const& _text)
{
    if (QClipboard* clipboard = QGuiApplication::clipboard(); clipboard != nullptr)
        clipboard->setText(QString::fromUtf8(_text.data(), static_cast<int>(_text.size())));
}

void TerminalWidget::dumpState()
{
    post([this]() { doDumpState(); });
}

void TerminalWidget::doDumpState()
{
    makeCurrent();
    auto const tmpDir = FileSystem::path(QStandardPaths::writableLocation(QStandardPaths::TempLocation).toStdString());
    auto const targetDir = tmpDir / FileSystem::path("contour-debug");
    FileSystem::create_directories(targetDir);
    debuglog(WidgetTag).write("Dumping state into directory: {}", targetDir.generic_string());
    // TODO: The above should be done from the outside and the targetDir being passed into this call.
    // TODO: maybe zip this dir in the end.

    // TODO: use this file store for everything that needs to be dumped.
    terminalView_->terminal().screen().dumpState("Dump screen state.");
    terminalView_->renderer().dumpState(std::cout);

    enum class ImageBufferFormat { RGBA, RGB, Alpha };

    auto screenshotSaver = [](FileSystem::path const& _filename, ImageBufferFormat _format) {
        auto const [qImageFormat, elementCount] = [&]() -> tuple<QImage::Format, int> {
            switch (_format) {
                case ImageBufferFormat::RGBA: return tuple{QImage::Format_RGBA8888, 4};
                case ImageBufferFormat::RGB: return tuple{QImage::Format_RGB888, 3};
                case ImageBufferFormat::Alpha: return tuple{QImage::Format_Grayscale8, 1};
            }
            return tuple{QImage::Format_Grayscale8, 1};
        }();

        // That's a little workaround for MacOS/X's C++ Clang compiler.
        auto const theImageFormat = qImageFormat;
        auto const theElementCount = elementCount;

        return [_filename, theImageFormat, theElementCount](vector<uint8_t> const& _buffer, Size _size) {
            auto image = make_unique<QImage>(_size.width, _size.height, theImageFormat);
            // Vertically flip the image, because the coordinate system between OpenGL and desktop screens is inverse.
            crispy::for_each(
                // TODO: std::execution::seq,
                crispy::times(_size.height),
                [&_buffer, &image, theElementCount, _size](int i) {
                    uint8_t const* sourceLine = &_buffer.data()[i * _size.width * theElementCount];
                    copy(sourceLine, sourceLine + _size.width * theElementCount, image->scanLine(_size.height - i - 1));
                }
            );
            image->save(QString::fromStdString(_filename.generic_string()));
        };
    };

    auto const atlasScreenshotSaver = [&screenshotSaver, &targetDir](std::string const& _allocatorName,
                                                                     unsigned _instanceId,
                                                                     vector<uint8_t> const& _buffer,
                                                                     Size _size) {
        return [&screenshotSaver, &targetDir, &_buffer, _size, _allocatorName, _instanceId](ImageBufferFormat _format) {
            auto const formatText = [&]() {
                switch (_format) {
                    case ImageBufferFormat::RGBA: return "rgba"sv;
                    case ImageBufferFormat::RGB: return "rgb"sv;
                    case ImageBufferFormat::Alpha: return "alpha"sv;
                }
                return "unknown"sv;
            }();
            auto const fileName = targetDir / fmt::format("atlas-{}-{}-{}.png", _allocatorName, formatText, _instanceId);
            return screenshotSaver(fileName, _format)(_buffer, _size);
        };
    };

    terminal::renderer::RenderTarget& renderTarget = terminalView_->renderer().renderTarget();

    for (auto const* allocator: {&renderTarget.monochromeAtlasAllocator(), &renderTarget.coloredAtlasAllocator(), &renderTarget.lcdAtlasAllocator()})
    {
        for (unsigned instanceId = allocator->instanceBaseId(); instanceId <= allocator->currentInstance(); ++instanceId)
        {
            auto infoOpt = renderTarget.readAtlas(*allocator, instanceId);
            if (!infoOpt.has_value())
                continue;

            terminal::renderer::AtlasTextureInfo& info = infoOpt.value();
            auto const saveScreenshot = atlasScreenshotSaver(allocator->name(), instanceId, info.buffer, info.size);
            switch (info.format)
            {
                case terminal::renderer::atlas::Format::RGBA:
                    saveScreenshot(ImageBufferFormat::RGBA);
                    break;
                case terminal::renderer::atlas::Format::RGB:
                    saveScreenshot(ImageBufferFormat::RGB);
                    break;
                case terminal::renderer::atlas::Format::Red:
                    saveScreenshot(ImageBufferFormat::Alpha);
                    break;
            }
        }
    }

    renderTarget.scheduleScreenshot(screenshotSaver(targetDir / "screenshot.png", ImageBufferFormat::RGBA));
}
// }}}

} // namespace contour
