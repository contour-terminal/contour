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

#include <terminal/Color.h>
#include <terminal/Metrics.h>
#include <terminal/pty/Pty.h>

#if defined(_MSC_VER)
#include <terminal/pty/ConPty.h>
#else
#include <terminal/pty/UnixPty.h>
#endif

#include <crispy/debuglog.h>

#include <terminal_renderer/opengl/OpenGLRenderer.h>

#include <QtCore/QDebug>
#include <QtCore/QMetaObject>
#include <QtCore/QFileInfo>
#include <QtCore/QProcess>
#include <QtCore/QTimer>
#include <QtNetwork/QHostInfo>
#include <QtGui/QClipboard>
#include <QtGui/QDesktopServices>
#include <QtGui/QKeyEvent>
#include <QtGui/QScreen>
#include <QtGui/QWindow>
#include <QtWidgets/QApplication>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QMessageBox>

#if defined(CONTOUR_BLUR_PLATFORM_KWIN)
#include <KWindowEffects>
#endif

#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string_view>

using namespace std::string_literals;

using std::cerr;

using std::array;
using std::chrono::steady_clock;
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

namespace {
    auto const WidgetTag = crispy::debugtag::make("terminal.widget", "Logs system widget related debug information.");
    auto const KeyboardTag = crispy::debugtag::make("keyboard", "Logs OS keyboard related debug information.");
}

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

    constexpr inline terminal::Modifier makeModifier(int _mods)
    {
        using terminal::Modifier;

        Modifier mods{};

        if (_mods & Qt::AltModifier)
            mods |= Modifier::Alt;
        if (_mods & Qt::ShiftModifier)
            mods |= Modifier::Shift;
#if defined(__APPLE__)
        // XXX https://doc.qt.io/qt-5/qt.html#KeyboardModifier-enum
        //     "Note: On macOS, the ControlModifier value corresponds to the Command keys on the keyboard,
        //      and the MetaModifier value corresponds to the Control keys."
        if (_mods & Qt::MetaModifier)
            mods |= Modifier::Control;
        if (_mods & Qt::ControlModifier)
            mods |= Modifier::Meta;
#else
        if (_mods & Qt::ControlModifier)
            mods |= Modifier::Control;
        if (_mods & Qt::MetaModifier)
            mods |= Modifier::Meta;
#endif

        return mods;
    }

    constexpr terminal::MouseButton makeMouseButton(Qt::MouseButton _button)
    {
        switch (_button)
        {
            case Qt::MouseButton::RightButton:
                return terminal::MouseButton::Right;
            case Qt::MiddleButton:
                return terminal::MouseButton::Middle;
            case Qt::LeftButton:
                [[fallthrough]];
            default: // d'oh
                return terminal::MouseButton::Left;
        }
    }

    /// Maps Qt KeyInputEvent to VT input event for special keys.
    optional<terminal::InputEvent> mapQtToTerminalKeyEvent(int _key, Qt::KeyboardModifiers _mods)
    {
        using terminal::Key;
        using terminal::InputEvent;
        using terminal::KeyInputEvent;
        using terminal::CharInputEvent;

        static auto constexpr mapping = array{
            pair{Qt::Key_Insert, Key::Insert},
            pair{Qt::Key_Delete, Key::Delete},
            pair{Qt::Key_Right, Key::RightArrow},
            pair{Qt::Key_Left, Key::LeftArrow},
            pair{Qt::Key_Down, Key::DownArrow},
            pair{Qt::Key_Up, Key::UpArrow},
            pair{Qt::Key_PageDown, Key::PageDown},
            pair{Qt::Key_PageUp, Key::PageUp},
            pair{Qt::Key_Home, Key::Home},
            pair{Qt::Key_End, Key::End},
            pair{Qt::Key_F1, Key::F1},
            pair{Qt::Key_F2, Key::F2},
            pair{Qt::Key_F3, Key::F3},
            pair{Qt::Key_F4, Key::F4},
            pair{Qt::Key_F5, Key::F5},
            pair{Qt::Key_F6, Key::F6},
            pair{Qt::Key_F7, Key::F7},
            pair{Qt::Key_F8, Key::F8},
            pair{Qt::Key_F9, Key::F9},
            pair{Qt::Key_F10, Key::F10},
            pair{Qt::Key_F11, Key::F11},
            pair{Qt::Key_F12, Key::F12},
            // todo: F13..F25
            // TODO: NumPad
            // pair{Qt::Key_0, Key::Numpad_0},
            // pair{Qt::Key_1, Key::Numpad_1},
            // pair{Qt::Key_2, Key::Numpad_2},
            // pair{Qt::Key_3, Key::Numpad_3},
            // pair{Qt::Key_4, Key::Numpad_4},
            // pair{Qt::Key_5, Key::Numpad_5},
            // pair{Qt::Key_6, Key::Numpad_6},
            // pair{Qt::Key_7, Key::Numpad_7},
            // pair{Qt::Key_8, Key::Numpad_8},
            // pair{Qt::Key_9, Key::Numpad_9},
            // pair{Qt::Key_Period, Key::Numpad_Decimal},
            // pair{Qt::Key_Slash, Key::Numpad_Divide},
            // pair{Qt::Key_Asterisk, Key::Numpad_Multiply},
            // pair{Qt::Key_Minus, Key::Numpad_Subtract},
            // pair{Qt::Key_Plus, Key::Numpad_Add},
            // pair{Qt::Key_Enter, Key::Numpad_Enter},
            // pair{Qt::Key_Equal, Key::Numpad_Equal},
        };

        if (auto i = find_if(begin(mapping), end(mapping), [_key](auto const& x) { return x.first == _key; }); i != end(mapping))
            return { InputEvent{KeyInputEvent{i->second, makeModifier(_mods)}} };

        if (_key == Qt::Key_Backtab)
            return { InputEvent{CharInputEvent{'\t', makeModifier(_mods | Qt::ShiftModifier)}} };

        return nullopt;
    }

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
        cerr << unhandledExceptionMessage(where, e) << endl;
    }

    template <typename F>
    class FunctionCallEvent : public QEvent {
      private:
       using Fun = typename std::decay<F>::type;
       Fun fun;
      public:
       FunctionCallEvent(Fun && fun) : QEvent(QEvent::None), fun(std::move(fun)) {}
       FunctionCallEvent(Fun const& fun) : QEvent(QEvent::None), fun(fun) {}
       ~FunctionCallEvent() { fun(); }
    };

    template <typename F>
    void postToObject(QObject* obj, F fun)
    {
#if 0
        // Qt >= 5.10
        QMetaObject::invokeMethod(obj, std::forward<F>(fun));
#else
        // Qt < 5.10
        QCoreApplication::postEvent(obj, new FunctionCallEvent<F>(std::forward<F>(fun)));
#endif
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
    terminalView_{},
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

    // QPalette p = QApplication::palette();
    // QColor backgroundColor(0x30, 0x30, 0x30, 0x80);
    // backgroundColor.setAlphaF(0.3);
    // p.setColor(QPalette::Window, backgroundColor);
    // setPalette(p);

    setFormat(surfaceFormat());

    setAttribute(Qt::WA_InputMethodEnabled, true);
    setAttribute(Qt::WA_OpaquePaintEvent);

    // setAttribute(Qt::WA_TranslucentBackground);
    // setAttribute(Qt::WA_NoSystemBackground, false);

    createScrollBar();

    updateTimer_.setSingleShot(true);
    connect(&updateTimer_, &QTimer::timeout, this, QOverload<>::of(&TerminalWidget::blinkingCursorUpdate));

    connect(this, SIGNAL(frameSwapped()), this, SLOT(onFrameSwapped()));

    //TODO: connect(this, SIGNAL(screenChanged(QScreen*)), this, SLOT(onScreenChanged(QScreen*)));
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

void TerminalWidget::createScrollBar()
{
    scrollBar_ = new QScrollBar(this);

    scrollBar_->resize(scrollBar_->sizeHint().width(), contentsRect().height());
    scrollBar_->setMinimum(0);
    scrollBar_->setMaximum(0);
    scrollBar_->setValue(0);
    scrollBar_->setCursor(Qt::ArrowCursor);

    connect(scrollBar_, &QScrollBar::valueChanged, this, QOverload<>::of(&TerminalWidget::onScrollBarValueChanged));
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

void TerminalWidget::onScreenChanged(QScreen* _screen)
{
    // TODO: Update font size and window size based on new screen's contentScale().
    (void) _screen;
}

void TerminalWidget::initializeGL()
{
    initializeOpenGLFunctions();

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
#endif
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
        debuglog(WidgetTag).write(glslVersionMsg);
    }
    // }}}

#if !defined(NDEBUG) && defined(GL_DEBUG_OUTPUT)
    glEnable(GL_DEBUG_OUTPUT);
    glDebugMessageCallback(&glMessageCallback, this);
#endif

    auto shell = profile().shell;
    shell.env["TERMINAL_NAME"] = "contour";
    shell.env["TERMINAL_VERSION_TRIPLE"] = fmt::format("{}.{}.{}", CONTOUR_VERSION_MAJOR, CONTOUR_VERSION_MINOR, CONTOUR_VERSION_PATCH);
    shell.env["TERMINAL_VERSION_STRING"] = CONTOUR_VERSION_STRING;

    terminalView_ = make_unique<terminal::view::TerminalView>(
        now_,
        *this,
        profile().maxHistoryLineCount,
        config_.wordDelimiters,
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
        shell,
        make_unique<terminal::renderer::opengl::OpenGLRenderer>(
            *config::Config::loadShaderConfig(config::ShaderClass::Text),
            *config::Config::loadShaderConfig(config::ShaderClass::Background),
            width(),
            height(),
            0, // TODO left margin
            0 // TODO bottom margin
        )
    );

    terminal::Screen& screen = terminalView_->terminal().screen();

    screen.setTabWidth(profile().tabWidth);

    // Sixel-scrolling default is *only* loaded during startup and NOT reloading during config file
    // hot reloading, because this value may have changed manually by an application already.
    screen.setMode(terminal::DECMode::SixelScrolling, config_.sixelScrolling);
    screen.setMaxImageSize(config_.maxImageSize);
    screen.setMaxImageColorRegisters(config_.maxImageColorRegisters);
    screen.setSixelCursorConformance(config_.sixelCursorConformance);

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
    debuglog(WidgetTag).write("width={}, height={}, scrollbarPos={}", _width, _height, config_.scrollbarPosition);
    if (_width == 0 || _height == 0)
        return;

    scrollBar_->resize(scrollBar_->sizeHint().width(), contentsRect().height());
    switch (config_.scrollbarPosition)
    {
        case config::ScrollBarPosition::Left:
            scrollBar_->move(0, 0);
            break;
        case config::ScrollBarPosition::Right:
            scrollBar_->move(size().width() - scrollBar_->sizeHint().width(), 0);
            break;
        case config::ScrollBarPosition::Hidden:
            break;
    }

    auto const viewWidth = width() - scrollBar_->sizeHint().width();
    auto const viewHeight = height();

    debuglog(WidgetTag).write("widget: {}, view: {}, geometry: {}/{}",
                              terminal::Size{_width, _height},
                              terminal::Size{viewWidth, viewHeight},
                              terminal::Size{geometry().top(), geometry().left()},
                              terminal::Size{geometry().width(), geometry().height()});

    terminalView_->resize(viewWidth, viewHeight);
    setMinimumSize(terminalView_->cellWidth() * 3, terminalView_->cellHeight() * 2);

    // if (setScreenDirty())
    //     update();
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

bool TerminalWidget::reloadConfigValues()
{
    return reloadConfigValues(profileName_);
}

bool TerminalWidget::reloadConfigValues(std::string const& _profileName)
{
    auto filePath = config_.backingFilePath.string();
    auto newConfig = config::Config{};

    auto configFailures = int{0};
    auto const configLogger = [&](string const& _msg)
    {
        cerr << "Configuration failure. " << _msg << '\n';
        ++configFailures;
    };

    try
    {
        loadConfigFromFile(newConfig, filePath);
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

    return reloadConfigValues(std::move(newConfig), _profileName);
}

bool TerminalWidget::reloadConfigValues(config::Config _newConfig)
{
    auto const profileName = profileName_;
    return reloadConfigValues(std::move(_newConfig), profileName);
}

bool TerminalWidget::reloadConfigValues(config::Config _newConfig, string const& _profileName)
{
    debuglog(WidgetTag).write("Loading configuration from {} with profile {}",
                              _newConfig.backingFilePath.string(),
                              _profileName);

    terminalView_->terminal().setWordDelimiters(_newConfig.wordDelimiters);

    terminalView_->terminal().screen().setMaxImageSize(_newConfig.maxImageSize);
    terminalView_->terminal().screen().setMaxImageColorRegisters(config_.maxImageColorRegisters);
    terminalView_->terminal().screen().setSixelCursorConformance(config_.sixelCursorConformance);

    config_ = std::move(_newConfig);
    if (config::TerminalProfile *profile = config_.profile(_profileName); profile != nullptr)
        activateProfile(_profileName, *profile);

    return true;
}

constexpr inline bool isModifier(Qt::Key _key)
{
    switch (_key)
    {
        case Qt::Key_Alt:
        case Qt::Key_Control:
        case Qt::Key_Shift:
        case Qt::Key_Meta:
            return true;
        default:
            return false;
    }
}

char32_t makeChar(Qt::Key _key, Qt::KeyboardModifiers _mods)
{
    auto const value = static_cast<int>(_key);
    if (value >= 'A' && value <= 'Z')
    {
        if (_mods & Qt::ShiftModifier)
            return value;
        else
            return std::tolower(value);
    }
    return 0;
}

QKeySequence toKeySequence(QKeyEvent *_keyEvent)
{
    auto const mod = [](int _qtMod) constexpr -> int {
        int res = 0;
        if (_qtMod & Qt::AltModifier) res += Qt::ALT;
        if (_qtMod & Qt::ShiftModifier) res += Qt::SHIFT;
#if defined(__APPLE__)
        // XXX https://doc.qt.io/qt-5/qt.html#KeyboardModifier-enum
        //     "Note: On macOS, the ControlModifier value corresponds to the Command keys on the keyboard,
        //      and the MetaModifier value corresponds to the Control keys."
        if (_qtMod & Qt::ControlModifier) res += Qt::META;
        if (_qtMod & Qt::MetaModifier) res += Qt::CTRL;
#else
        if (_qtMod & Qt::ControlModifier) res += Qt::CTRL;
        if (_qtMod & Qt::MetaModifier) res += Qt::META;
#endif
        return res;
    }(_keyEvent->modifiers());

    // only modifier but no key press?
    if (isModifier(static_cast<Qt::Key>(_keyEvent->key())))
        return QKeySequence();

    // modifier AND key press?
    if (_keyEvent->key() && mod)
        return QKeySequence(int(_keyEvent->modifiers() | _keyEvent->key()));

    return QKeySequence();
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
        if (terminalView_->terminal().screen().contains(currentMousePosition))
        {
            if (terminalView_->terminal().screen().at(currentMousePosition).hyperlink())
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
            updateScrollBarValue();
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
            copyToClipboard(extractLastMarkRange());
            return Result::Silently;
        },
        [this](actions::CopySelection) -> Result {
            string const text = extractSelectionText();
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
                return reloadConfigValues(action.profileName.value()) ? Result::Dirty : Result::Nothing;
            else
                return reloadConfigValues() ? Result::Dirty : Result::Nothing;
        },
        [this](actions::ResetConfig) -> Result {
            auto const ec = config::createDefaultConfig(config_.backingFilePath);
            if (ec)
            {
                cerr << fmt::format("Failed to load default config at {}; ({}) {}\n",
                                    config_.backingFilePath.string(),
                                    ec.category().name(),
                                    ec.message());
                return Result::Silently;
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
            return reloadConfigValues(defaultConfig) ? Result::Dirty : Result::Nothing;
        },
        [this](actions::FollowHyperlink) -> Result {
            auto const _l = scoped_lock{terminalView_->terminal()};
            auto const currentMousePosition = terminalView_->terminal().currentMousePosition();
            if (terminalView_->terminal().screen().contains(currentMousePosition))
            {
                if (auto hyperlink = terminalView_->terminal().screen().at(currentMousePosition).hyperlink(); hyperlink != nullptr)
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
        args.append("-c");
        args.append(QString::fromStdString(config_.backingFilePath.string()));
        args.append(QString::fromUtf8(_hyperlink.path().data(), static_cast<int>(_hyperlink.path().size())));
        QProcess::execute(QString::fromStdString(programPath_), args);
    }
    else if (isLocal && fileInfo.isFile() && editorEnv && *editorEnv)
    {
        QStringList args;
        args.append("-c");
        args.append(QString::fromStdString(config_.backingFilePath.string()));
        args.append(QString::fromStdString(editorEnv));
        args.append(QString::fromUtf8(_hyperlink.path().data(), static_cast<int>(_hyperlink.path().size())));
        QProcess::execute(QString::fromStdString(programPath_), args);
    }
    else
        QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromUtf8(std::string(_hyperlink.path()).c_str())));
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
    if (newProfile.fonts != profile().fonts)
    {
        terminalView_->renderer().setFonts(newProfile.fonts);
        terminalView_->updateFontMetrics();
    }
    else
        setFontSize(newProfile.fonts.size);

    auto const newScreenSize = terminal::Size{
        size().width() / gridMetrics().cellSize.width,
        size().height() / gridMetrics().cellSize.height
    };

    if (newScreenSize != terminalView_->terminal().screenSize())
        terminalView_->setTerminalSize(newScreenSize);
        // TODO: maybe update margin after this call?
    terminalView_->terminal().screen().setMaxHistoryLineCount(newProfile.maxHistoryLineCount);

    terminalView_->setColorProfile(newProfile.colors);

    terminalView_->setHyperlinkDecoration(newProfile.hyperlinkDecoration.normal,
                                          newProfile.hyperlinkDecoration.hover);

    if (newProfile.cursorShape != profile().cursorShape)
        terminalView_->setCursorShape(newProfile.cursorShape);

    if (newProfile.cursorDisplay != profile().cursorDisplay)
        terminalView_->terminal().setCursorDisplay(newProfile.cursorDisplay);

    if (newProfile.backgroundBlur != profile().backgroundBlur)
        emit setBackgroundBlur(newProfile.backgroundBlur);

    if (newProfile.tabWidth != profile().tabWidth)
        terminalView_->terminal().screen().setTabWidth(newProfile.tabWidth);

    if (newProfile.maximized)
        window()->showMaximized();
    else
        window()->showNormal();

    if (newProfile.fullscreen != window()->isFullScreen())
        toggleFullscreen();

    updateScrollBarPosition();

    profile_ = std::move(newProfile);
    profileName_ = _name;
}

string TerminalWidget::extractSelectionText()
{
    using namespace terminal;
    int lastColumn = 0;
    string text;
    string currentLine;

    auto const trimRight = [](string& value)
    {
        while (!value.empty() && std::isspace(value.back()))
            value.pop_back();
    };

    terminalView_->terminal().renderSelection([&](Coordinate const& _pos, Cell const& _cell) {
        auto const _lock = scoped_lock{ terminalView_->terminal() };
        auto const isNewLine = _pos.column <= lastColumn;
        auto const isLineWrapped = terminalView_->terminal().lineWrapped(_pos.row);
        bool const touchesRightPage = _pos.row > 0
            && terminalView_->terminal().isSelectedAbsolute({_pos.row - 1, gridMetrics().pageSize.width});
        if (isNewLine && (!isLineWrapped || !touchesRightPage))
        {
            // TODO: handle logical line in word-selection (don't include LF in wrapped lines)
            trimRight(currentLine);
            text += currentLine;
            text += '\n';
            currentLine.clear();
        }
        currentLine += _cell.toUtf8();
        lastColumn = _pos.column;
    });

    trimRight(currentLine);
    text += currentLine;

    return text;
}

string TerminalWidget::extractLastMarkRange()
{
    using terminal::Coordinate;
    using terminal::Cell;

    auto const _l = std::lock_guard{terminalView_->terminal()};

    auto const& screen = terminalView_->terminal().screen();
    auto const colCount = screen.size().width;
    auto const bottomLine = screen.historyLineCount() + screen.cursor().position.row - 1;

    auto const marker1 = optional{bottomLine};

    auto const marker0 = screen.findMarkerBackward(marker1.value());
    if (!marker0.has_value())
        return {};

    // +1 each for offset change from 0 to 1 and because we only want to start at the line *after* the mark.
    auto const firstLine = *marker0 - screen.historyLineCount() + 2;
    auto const lastLine = *marker1 - screen.historyLineCount();

    string text;

    for (auto lineNum = firstLine; lineNum <= lastLine; ++lineNum)
    {
        for (auto colNum = 1; colNum < colCount; ++colNum)
            text += screen.at({lineNum, colNum}).toUtf8();
        text += '\n';
    }

    return text;
}

void TerminalWidget::spawnNewTerminal(std::string const& _profileName)
{
    // TODO: config option to either spawn new terminal via new process (default) or just as second window.
    QString const program = QString::fromUtf8(programPath_.c_str());
    QStringList args;

    if (!config_.backingFilePath.empty())
        args << "-c" << QString::fromStdString(config_.backingFilePath.generic_string());

    if (!_profileName.empty())
        args << "-p" << QString::fromStdString(_profileName);

    auto const wd = [&]() -> QString {
        auto const _l = scoped_lock{terminalView_->terminal()};
        auto const url = QUrl(QString::fromUtf8(terminalView_->terminal().screen().currentWorkingDirectory().c_str()));
        if (url.host() == QHostInfo::localHostName())
            return url.path();
        else
            return QString();
    }();

    if (!wd.isEmpty())
        args << "-w" << wd;

    QProcess::startDetached(program, args);
}

float TerminalWidget::contentScale() const
{
    if (!window()->windowHandle())
        return 1.0f;

    return window()->windowHandle()->screen()->devicePixelRatio();
}

void TerminalWidget::onConfigReload(FileChangeWatcher::Event /*_event*/)
{
    post([this]() {
        reloadConfigValues();
    });

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
        string const text = extractSelectionText();
        clipboard->setText(QString::fromUtf8(text.c_str(), static_cast<int>(text.size())), QClipboard::Selection);
    }
}

void TerminalWidget::bufferChanged(terminal::ScreenType)
{
    post([this] ()
    {
        setDefaultCursor();

        if (terminalView_->terminal().screen().isPrimaryScreen())
            scrollBar_->setMaximum(terminalView_->terminal().screen().historyLineCount());
        else
            scrollBar_->setMaximum(0);

        updateScrollBarPosition();
        updateScrollBarValue();
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
            scrollBar_->setMaximum(terminalView_->terminal().screen().historyLineCount());
            if (profile().autoScrollOnUpdate && terminalView_->terminal().viewport().scrolled())
                terminalView_->terminal().viewport().scrollToBottom();
            updateScrollBarValue();
        });
    }

    if (setScreenDirty())
        update(); //QCoreApplication::postEvent(this, new QEvent(QEvent::UpdateRequest));
}

void TerminalWidget::updateScrollBarValue()
{
    if (auto const s = terminalView_->terminal().viewport().absoluteScrollOffset(); s.has_value())
        scrollBar_->setValue(s.value());
    else
        scrollBar_->setValue(scrollBar_->maximum());
}

void TerminalWidget::onScrollBarValueChanged()
{
    terminalView_->terminal().viewport().scrollToAbsolute(scrollBar_->value());
    if (setScreenDirty())
        update();
}

void TerminalWidget::updateScrollBarPosition()
{
    if (view()->terminal().screen().isAlternateScreen())
    {
        if (config_.hideScrollbarInAltScreen)
            scrollBar_->hide();
        else
            scrollBar_->show();
    }
    else
    {
        switch (config_.scrollbarPosition)
        {
            case config::ScrollBarPosition::Left:
                scrollBar_->move(0, 0);
                scrollBar_->show();
                break;
            case config::ScrollBarPosition::Right:
                scrollBar_->move(size().width() - scrollBar_->sizeHint().width(), 0);
                scrollBar_->show();
                break;
            case config::ScrollBarPosition::Hidden:
                scrollBar_->hide();
                break;
        }
    }
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
        auto const newScreenSize = terminal::Size{width, height};
        post([this, newScreenSize]() { setSize(newScreenSize); });
    }
    else
    {
        if (!_width)
            _width = profile().terminalSize.width;

        if (!_height)
            _height = profile().terminalSize.height;

        auto const newScreenSize = terminal::Size{_width, _height};
        post([this, newScreenSize]() { setSize(newScreenSize); });
    }
}

QSize TerminalWidget::minimumSizeHint() const
{
    auto constexpr MinimumScreenSize = terminal::Size{1, 1};

    auto const cellSize = terminalView_ ? terminalView_->renderer().gridMetrics().cellSize
                                        : terminal::Size{10, 20};

    auto const w = MinimumScreenSize.width * cellSize.width;
    auto const h = MinimumScreenSize.height * cellSize.height;
    // auto const w = MinimumScreenSize.width * gridMetrics().cellSize.width;
    // auto const h = MinimumScreenSize.height * gridMetrics().cellSize.height;

    return QSize(w, h);
}

QSize TerminalWidget::sizeHint() const
{
    auto const scrollbarWidth = scrollBar_->isHidden() ? 0 : scrollBar_->sizeHint().width();

    auto const cellSize = terminalView_ ? terminalView_->renderer().gridMetrics().cellSize
                                        : terminal::Size{100, 100};

    auto const viewWidth = profile().terminalSize.width * cellSize.width;
    auto const viewHeight = profile().terminalSize.height * cellSize.height;
    // auto const viewWidth = profile().terminalSize.width * gridMetrics().cellSize.width;
    // auto const viewHeight = profile().terminalSize.height * gridMetrics().cellSize.height;

    debuglog(WidgetTag).write(
        "Calling sizeHint: {}, SBW: {}, terminalSize: {}",
        terminal::Size{viewWidth + scrollbarWidth, viewHeight},
        scrollbarWidth,
        profile().terminalSize
    );

    return QSize(viewWidth + scrollbarWidth, viewHeight);
}

void TerminalWidget::setSize(terminal::Size _size)
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

void TerminalWidget::setFontDef(terminal::FontDef const& _fontDef)
{
    post([this, spec = terminal::FontDef(_fontDef)]() {
        if (requestPermissionChangeFont())
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

bool TerminalWidget::requestPermissionChangeFont()
{
    switch (profile().permissions.changeFont)
    {
        case config::Permission::Allow:
            debuglog(WidgetTag).write("Permission for font change allowed by configuration.");
            return true;
        case config::Permission::Deny:
            debuglog(WidgetTag).write("Permission for font change denied by configuration.");
            return false;
        case config::Permission::Ask:
            break;
    }

    if (rememberedPermissions_.changeFont.has_value())
        return rememberedPermissions_.changeFont.value();

    debuglog(WidgetTag).write("Permission for font change requires asking user.");

    auto const reply = QMessageBox::question(this,
        "Font change requested",
        QString::fromStdString(fmt::format("The application has requested to change the font. Do you allow this?")),
        QMessageBox::StandardButton::Yes
            | QMessageBox::StandardButton::YesToAll
            | QMessageBox::StandardButton::No
            | QMessageBox::StandardButton::NoToAll,
        QMessageBox::StandardButton::NoButton
    );

    switch (reply)
    {
        case QMessageBox::StandardButton::NoToAll:
            rememberedPermissions_.changeFont = false;
            break;
        case QMessageBox::StandardButton::YesToAll:
            rememberedPermissions_.changeFont = true;
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
    // TODO: log this to debuglog(...)?
    terminalView_->terminal().screen().dumpState("Dump screen state.");
    //XXX terminalView_->renderer().dumpState(std::cout);
}
// }}}

} // namespace contour
