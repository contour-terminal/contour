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
#include <terminal/Metrics.h>

#include <terminal/pty/Pty.h>

#if defined(_MSC_VER)
#include <terminal/pty/ConPty.h>
#else
#include <terminal/pty/UnixPty.h>
#endif

#include <QtCore/QDebug>
#include <QtCore/QFileInfo>
#include <QtCore/QProcess>
#include <QtCore/QTimer>
#include <QtGui/QClipboard>
#include <QtGui/QDesktopServices>
#include <QtGui/QKeyEvent>
#include <QtGui/QOpenGLWindow>
#include <QtGui/QScreen>
#include <QtNetwork/QHostInfo>
#include <QtWidgets/QApplication>
#include <QtWidgets/QScrollBar>

#if defined(CONTOUR_BLUR_PLATFORM_KWIN)
#include <KWindowEffects>
#endif

#include <cstring>
#include <fstream>
#include <stdexcept>

using namespace std::string_literals;

using std::cerr;
using std::cout;

using std::array;
using std::string_view;
using std::ios;
using std::chrono::steady_clock;
using std::endl;
using std::exception;
using std::get;
using std::lock_guard;
using std::make_unique;
using std::max;
using std::move;
using std::nullopt;
using std::optional;
using std::pair;
using std::ref;
using std::runtime_error;
using std::ofstream;
using std::string;
using std::scoped_lock;
using std::holds_alternative;

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

using terminal::view::Renderer;
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
        if (_mods & Qt::ControlModifier)
            mods |= Modifier::Control;
        if (_mods & Qt::MetaModifier)
            mods |= Modifier::Meta;

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

    inline QMatrix4x4 ortho(float left, float right, float bottom, float top)
    {
        constexpr float nearPlane = -1.0f;
        constexpr float farPlane = 1.0f;

        QMatrix4x4 mat;
        mat.ortho(left, right, bottom, top, nearPlane, farPlane);
        return mat;
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

#if !defined(NDEBUG)
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
#if defined(GL_DEBUG_TYPE_ERROR)
            return  _type == GL_DEBUG_TYPE_ERROR ? "** GL ERROR **" : "";
#else
            return "";
#endif
        }(_type);
        fprintf(
            stderr,
            "GL CALLBACK: %s type = %s, source = %s, severity = %s, message = %s\n",
            tag,
            typeName.c_str(), sourceName.c_str(), debugSeverity.c_str(), _message
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
} // }}}

TerminalWidget::TerminalWidget(QWidget* _parent,
                               config::Config _config,
                               string _profileName,
                               string _programPath) :
    QOpenGLWidget{ nullptr, 0 },
    window_{ _parent },
    now_{ steady_clock::now() },
    config_{ std::move(_config) },
    profileName_{ std::move(_profileName) },
    profile_{ *config_.profile(profileName_) },
    programPath_{ std::move(_programPath) },
    logger_{
        config_.logFilePath
            ? LoggingSink{config_.loggingMask, config_.logFilePath->string()}
            : LoggingSink{config_.loggingMask, &cout}
    },
    fontLoader_{&cerr},
    fonts_{loadFonts(profile())},
    terminalView_{},
    configFileChangeWatcher_{
        config_.backingFilePath,
        [this](FileChangeWatcher::Event event) { onConfigReload(event); }
    },
    updateTimer_(this)
{
    // qDebug() << "TerminalWidget:"
    //     << QString::fromUtf8(fmt::format("{}", config_.profile(config_.defaultProfileName)->terminalSize).c_str())
    //     << "fontSize:" << profile().fontSize
    //     << "contentScale:" << contentScale();

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

    updateTimer_.setSingleShot(true);
    connect(&updateTimer_, &QTimer::timeout, this, QOverload<>::of(&TerminalWidget::blinkingCursorUpdate));

    //TODO: connect(this, SIGNAL(screenChanged(QScreen*)), this, SLOT(onScreenChanged(QScreen*)));
    connect(this, SIGNAL(frameSwapped()), this, SLOT(onFrameSwapped()));

    createScrollBar();

    // XXX later on, do this only for the first window.
    QSize const size(
        static_cast<int>(profile().terminalSize.width * fonts_.regular.first.get().maxAdvance()),
        static_cast<int>(profile().terminalSize.height * fonts_.regular.first.get().lineHeight())
    );
    window_->resize(size);
}

TerminalWidget::~TerminalWidget()
{
    std::cout << "TerminalWidget.dtor!\n";
    makeCurrent(); // XXX must be called.
    statsSummary();
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
                renderingPressure_ = true;
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

    // {{{ some stats
    cout << fmt::format("DPI             : {}x{} physical; {}x{} logical\n",
                        physicalDpiX(), physicalDpiY(),
                        logicalDpiX(), logicalDpiY());
    cout << fmt::format("OpenGL type     : {}\n", (QOpenGLContext::currentContext()->isOpenGLES() ? "OpenGL/ES" : "OpenGL"));
    cout << fmt::format("OpenGL renderer : {}\n", glGetString(GL_RENDERER));
    cout << fmt::format("Qt platform     : {}\n", QGuiApplication::platformName().toStdString());

    GLint versionMajor{};
    GLint versionMinor{};
    QOpenGLContext::currentContext()->functions()->glGetIntegerv(GL_MAJOR_VERSION, &versionMajor);
    QOpenGLContext::currentContext()->functions()->glGetIntegerv(GL_MINOR_VERSION, &versionMinor);
    cout << fmt::format("OpenGL version  : {}.{}\n", versionMajor, versionMinor);
    cout << fmt::format("GLSL version    : {}", glGetString(GL_SHADING_LANGUAGE_VERSION));

    // TODO: pass phys()/logical?) dpi to font manager, so font size can be applied right
    // TODO: also take window monitor switches into account

    GLint glslNumShaderVersions{};
#if defined(GL_NUM_SHADING_LANGUAGE_VERSIONS)
    glGetIntegerv(GL_NUM_SHADING_LANGUAGE_VERSIONS, &glslNumShaderVersions);
#endif
    if (glslNumShaderVersions > 0)
    {
        cout << " (";
        for (GLint k = 0, l = 0; k < glslNumShaderVersions; ++k)
            if (auto const str = glGetStringi(GL_SHADING_LANGUAGE_VERSION, k); str && *str)
            {
                cout << (l ? ", " : "") << str;
                l++;
            }
        cout << ')';
    }
    cout << "\n\n";
    // }}}

#if !defined(NDEBUG) && defined(GL_DEBUG_OUTPUT)
    glEnable(GL_DEBUG_OUTPUT);
    glDebugMessageCallback(&glMessageCallback, this);
#endif

    terminalView_ = make_unique<terminal::view::TerminalView>(
        now_,
        *this,
        profile().maxHistoryLineCount,
        config_.wordDelimiters,
        fonts_,
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
        profile().shell,
        ortho(0.0f, static_cast<float>(width()), 0.0f, static_cast<float>(height())),
        *config::Config::loadShaderConfig(config::ShaderClass::Background),
        *config::Config::loadShaderConfig(config::ShaderClass::Text),
        ref(logger_)
    );

    terminalView_->terminal().screen().setLogRaw((config_.loggingMask & LogMask::RawOutput) != LogMask::None);
    terminalView_->terminal().screen().setLogTrace((config_.loggingMask & LogMask::TraceOutput) != LogMask::None);
    terminalView_->terminal().screen().setTabWidth(profile().tabWidth);

    // Sixel-scrolling default is *only* loaded during startup and NOT reloading during config file
    // hot reloading, because this value may have changed manually by an application already.
    terminalView_->terminal().screen().setMode(terminal::Mode::SixelScrolling, config_.sixelScrolling);
    terminalView_->terminal().screen().setMaxImageSize(config_.maxImageSize);
    terminalView_->terminal().screen().setMaxImageColorRegisters(config_.maxImageColorRegisters);
    terminalView_->terminal().screen().setSixelCursorConformance(config_.sixelCursorConformance);
}

void TerminalWidget::resizeEvent(QResizeEvent* _event)
{
    QOpenGLWidget::resizeEvent(_event);

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

    if (width() != 0 && height() != 0)
    {
        terminalView_->resize(width(), height());
        terminalView_->setProjection(
            ortho(
                0.0f, static_cast<float>(width()),      // left, right
#if defined(LIBTERMINAL_VIEW_NATURAL_COORDS)
                0.0f, static_cast<float>(height())      // bottom, top
#else
                static_cast<float>(height()), 0.0f      // bottom, top
#endif
            )
        );
        if (setScreenDirty())
            update();
    }
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

        // It may be that this repaint was triggered by a viewport scrolling action.
        updateScrollBarValue();

        QPoint const viewport{
            static_cast<int>(static_cast<float>(width()) * contentScale()),
            static_cast<int>(static_cast<float>(height()) * contentScale())
        };

        if (viewport != renderStateCache_.viewport)
        {
            glViewport(0, 0, static_cast<GLsizei>(viewport.x()), static_cast<GLsizei>(viewport.y()));
            renderStateCache_.viewport = viewport;
        }

        {
            auto calls = decltype(queuedCalls_){};
            {
                auto lg = lock_guard{queuedCallsLock_};
                swap(queuedCalls_, calls);
            }
            for_each(begin(calls), end(calls), [](auto& _call) { _call(); });
        }

        bool const reverseVideo =
            terminalView_->terminal().screen().isModeEnabled(terminal::Mode::ReverseVideo);

        QVector4D const bg = Renderer::canonicalColor(
            reverseVideo
                ? profile().colors.defaultForeground
                : profile().colors.defaultBackground,
            profile().backgroundOpacity);

        if (bg != renderStateCache_.backgroundColor)
        {
            glClearColor(bg[0], bg[1], bg[2], bg[3]);
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
        loadConfigFromFile(newConfig, filePath, configLogger);
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
    auto const profileName = _newConfig.defaultProfileName;
    return reloadConfigValues(std::move(_newConfig), profileName);
}

bool TerminalWidget::reloadConfigValues(config::Config _newConfig, string const& _profileName)
{
    cout << fmt::format("Loading configuration from {} with profile {}\n",
                        _newConfig.backingFilePath.string(),
                        _profileName);

    logger_ =
        _newConfig.logFilePath
            ? LoggingSink{_newConfig.loggingMask, _newConfig.logFilePath->string()}
            : LoggingSink{_newConfig.loggingMask, &cout};

    terminalView_->terminal().setWordDelimiters(_newConfig.wordDelimiters);

    terminalView_->terminal().screen().setMaxImageSize(_newConfig.maxImageSize);
    terminalView_->terminal().screen().setMaxImageColorRegisters(config_.maxImageColorRegisters);
    terminalView_->terminal().screen().setSixelCursorConformance(config_.sixelCursorConformance);

    terminalView_->terminal().screen().setLogRaw((_newConfig.loggingMask & LogMask::RawOutput) != LogMask::None);
    terminalView_->terminal().screen().setLogTrace((_newConfig.loggingMask & LogMask::TraceOutput) != LogMask::None);

    config_ = std::move(_newConfig);
    if (config::TerminalProfile *profile = config_.profile(_profileName); profile != nullptr)
        setProfile(*profile);

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

void TerminalWidget::keyPressEvent(QKeyEvent* _keyEvent)
{
    auto const keySeq = QKeySequence(
        isModifier(static_cast<Qt::Key>(_keyEvent->key()))
            ? _keyEvent->modifiers()
            : _keyEvent->modifiers() | _keyEvent->key()
    );

    // if (!_keyEvent->text().isEmpty())
    //     qDebug() << "keyPress:"
    //         << "text:" << _keyEvent->text()
    //         << "seq:" << keySeq
    //         << "seq.empty?" << keySeq.isEmpty()
    //         << "key:" << static_cast<Qt::Key>(_keyEvent->key())
    //         << QString::fromLatin1(fmt::format("0x{:x}", keySeq[0]).c_str());

    if (!_keyEvent->text().isEmpty() && cursor().shape() != Qt::CursorShape::BlankCursor)
        setCursor(Qt::CursorShape::BlankCursor);

    if (auto i = config_.keyMappings.find(keySeq); i != end(config_.keyMappings))
    {
        executeAllActions(i->second);
    }
    else if (auto const inputEvent = mapQtToTerminalKeyEvent(_keyEvent->key(), _keyEvent->modifiers()))
    {
        terminalView_->terminal().send(*inputEvent, now_);
        scrollToBottomAndRedraw();
    }
    else if (!_keyEvent->text().isEmpty())
    {
        for (auto const ch : _keyEvent->text().toUcs4())
        {
            auto const modifiers = makeModifier(_keyEvent->modifiers());
            auto const inputEvent = terminal::InputEvent{terminal::CharInputEvent{ch, modifiers}};
            terminalView_->terminal().send(inputEvent, now_);
        }
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
        // qDebug() << "TerminalWidget.event:" << _event;
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
    return false; // TODO: window_->visibility() == QWindow::FullScreen;
}

void TerminalWidget::toggleFullScreen()
{
    // TODO:
    // if (window_->visibility() == QWindow::FullScreen)
    //     window_->setVisibility(QWindow::Windowed);
    // else
    //     window_->setVisibility(QWindow::FullScreen);
}

bool TerminalWidget::setFontSize(int _fontSize)
{
    if (_fontSize < 5) // Let's not be crazy.
        return false;

    if (_fontSize > 100)
        return false;

    float const fontSize = (static_cast<float>(_fontSize) / 72.0f) * static_cast<float>(logicalDpiX());

    // cout << fmt::format("TerminalWidget.setFontSize: {} -> {}; {} * {}\n",
    //                     profile().fontSize, _fontSize, fontSize, contentScale());

    terminalView_->setFontSize(static_cast<int>(fontSize * contentScale()));

    profile().fontSize = static_cast<short>(_fontSize);

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

    Result const result = visit(overloaded{
        [&](actions::WriteScreen const& _write) -> Result {
            terminalView_->terminal().writeToScreen(_write.chars);
            return Result::Silently;
        },
        [&](actions::ToggleFullScreen) -> Result {
            toggleFullScreen();
            return Result::Silently;
        },
        [&](actions::IncreaseFontSize) -> Result {
            setFontSize(profile().fontSize + 1);
            return Result::Silently;
        },
        [&](actions::DecreaseFontSize) -> Result {
            setFontSize(profile().fontSize - 1);
            return Result::Silently;
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
        [this](actions::ScrollOneUp) -> Result {
            return terminalView_->terminal().viewport().scrollUp(1) ? Result::Dirty : Result::Nothing;
        },
        [this](actions::ScrollOneDown) -> Result {
            return terminalView_->terminal().viewport().scrollDown(1) ? Result::Dirty : Result::Nothing;
        },
        [this](actions::ScrollUp) -> Result {
            return terminalView_->terminal().viewport().scrollUp(profile().historyScrollMultiplier) ? Result::Dirty : Result::Nothing;
        },
        [this](actions::ScrollDown) -> Result {
            return terminalView_->terminal().viewport().scrollDown(profile().historyScrollMultiplier) ? Result::Dirty : Result::Nothing;
        },
        [this](actions::ScrollPageUp) -> Result {
            return terminalView_->terminal().viewport().scrollUp(profile().terminalSize.height / 2) ? Result::Dirty : Result::Nothing;
        },
        [this](actions::ScrollPageDown) -> Result {
            return terminalView_->terminal().viewport().scrollDown(profile().terminalSize.height / 2) ? Result::Dirty : Result::Nothing;
        },
        [this](actions::ScrollMarkUp) -> Result {
            return terminalView_->terminal().viewport().scrollMarkUp() ? Result::Dirty : Result::Nothing;
        },
        [this](actions::ScrollMarkDown) -> Result {
            return terminalView_->terminal().viewport().scrollMarkDown() ? Result::Dirty : Result::Nothing;
        },
        [this](actions::ScrollToTop) -> Result {
            return terminalView_->terminal().viewport().scrollToTop() ? Result::Dirty : Result::Nothing;
        },
        [this](actions::CopyPreviousMarkRange) -> Result {
            copyToClipboard(extractLastMarkRange());
            return Result::Silently;
        },
        [this](actions::ScrollToBottom) -> Result {
            return terminalView_->terminal().viewport().scrollToBottom() ? Result::Dirty : Result::Nothing;
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
            cerr << fmt::format("Changing profile to '{}'.", v.name) << endl;
            if (auto newProfile = config_.profile(v.name); newProfile)
                setProfile(*newProfile);
            else
                cerr << fmt::format("No such profile: '{}'.", v.name) << endl;
            return Result::Dirty;
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
        [](actions::OpenFileManager) -> Result {
            // TODO open file manager at current window's current working directory (via /proc/self/cwd)
            return Result::Silently;
        },
        [this](actions::Quit) -> Result {
            //TODO: later warn here when more then one terminal view is open
            terminalView_->terminal().device().close();
            exit(EXIT_SUCCESS);
            return Result::Silently;
        },
        [this](actions::ResetFontSize) -> Result {
            setFontSize(config_.profile(profileName_)->fontSize);
            return Result::Silently;
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
            auto const defaultConfig = config::loadConfigFromFile(
                config_.backingFilePath,
                [&](auto const& msg) {
                    cerr << "Failed to load default config: " << msg << endl;
                }
            );
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
        case Result::Nothing:
            break;
        case Result::Silently:
            return true;
        case Result::Dirty:
            setScreenDirty();
            update();
            return true;
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

terminal::view::FontConfig TerminalWidget::loadFonts(config::TerminalProfile const& _profile)
{
    int const fontSize = static_cast<int>((static_cast<float>(_profile.fontSize) / 72.0f) * static_cast<float>(logicalDpiX()));

    // cout << fmt::format("TerminalWidget.loadFonts: size: {}; {} * {}\n",
    //                     _profile.fontSize, fontSize, contentScale());

    // TODO: make these fonts customizable even further for the user
    return terminal::view::FontConfig{
        fontLoader_.load(_profile.fonts.regular.pattern, fontSize),
        fontLoader_.load(_profile.fonts.bold.pattern, fontSize),
        fontLoader_.load(_profile.fonts.italic.pattern, fontSize),
        fontLoader_.load(_profile.fonts.boldItalic.pattern, fontSize),
        fontLoader_.load("emoji", fontSize)
    };
}

void TerminalWidget::setProfile(config::TerminalProfile newProfile)
{
    if (newProfile.fonts != profile().fonts)
    {
        fonts_ = loadFonts(newProfile);
        terminalView_->setFont(fonts_);
    }
    else
        setFontSize(newProfile.fontSize);

    auto const newScreenSize = terminal::Size{
        size().width() / fonts_.regular.first.get().maxAdvance(),
        size().height() / fonts_.regular.first.get().lineHeight()
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

    updateScrollBarPosition();

    profile_ = std::move(newProfile);
}

string TerminalWidget::extractSelectionText()
{
    using namespace terminal;
    int lastColumn = 0;
    string text;
    string currentLine;

    terminalView_->terminal().renderSelection([&](Coordinate const& _pos, Cell const& _cell) {
        if (_pos.column <= lastColumn)
        {
            text += currentLine;
            text += '\n';
            currentLine.clear();
        }
        currentLine += _cell.toUtf8();
        lastColumn = _pos.column;
    });
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
    if (!_profileName.empty())
        args << "-p" << QString::fromStdString(_profileName);

    QProcess::startDetached(program, args);
}

float TerminalWidget::contentScale() const
{
    if (!windowHandle())
        return 1.0f;

    return windowHandle()->screen()->devicePixelRatio();
}

void TerminalWidget::onConfigReload(FileChangeWatcher::Event /*_event*/)
{
    post([this]() {
        if (reloadConfigValues())
        {
            setScreenDirty();
            update();
        }
    });
}

void TerminalWidget::post(std::function<void()> _fn)
{
	auto lg = lock_guard{queuedCallsLock_};
    queuedCalls_.emplace_back(std::move(_fn));
    QCoreApplication::postEvent(this, new QEvent(QEvent::UpdateRequest));
}

// {{{ TerminalView::Events overrides

void TerminalWidget::bell()
{
    if (logger_.sink())
        *logger_.sink() << "TODO: Beep!\n";
    QApplication::beep();
    // QApplication::beep() requires Qt Widgets dependency. doesn't suound good.
    // so maybe just a visual bell then? That would require additional OpenGL/shader work then though.
}

void TerminalWidget::notify(std::string_view const& _title, std::string_view const& _content)
{
    emit showNotification(QString::fromUtf8(_title.data(), static_cast<int>(_title.size())),
                          QString::fromUtf8(_content.data(), static_cast<int>(_content.size())));
}

void TerminalWidget::setWindowTitle(std::string_view const& _title)
{
    post([this, terminalTitle = string(_title)]() {
        auto const title = terminalTitle.empty()
            ? "contour"s
            : fmt::format("{} - contour", terminalTitle);
        window_->setWindowTitle(QString::fromUtf8(title.c_str()));
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
    setDefaultCursor();

    updateScrollBarPosition();
    updateScrollBarValue();
}

void TerminalWidget::commands()
{
#if defined(CONTOUR_VT_METRICS)
    // TODO
    // for (auto const& command : _commands)
    //     terminalMetrics_(command);
#endif

    if (profile().autoScrollOnUpdate && terminalView_->terminal().viewport().relativeScrollOffset())
        terminalView_->terminal().viewport().scrollToBottom();

    if (terminalView_->terminal().screen().isPrimaryScreen())
        scrollBar_->setMaximum(terminalView_->terminal().screen().historyLineCount());
    else
        scrollBar_->setMaximum(0);
    updateScrollBarValue();

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
    // cerr << fmt::format("Application request to resize window: {}x{} {}\n",
    //                     _width, _height, _inPixels ? "px" : "cells");

    bool resizePending = false;
    if (fullscreen())
    {
        cerr << "Application request to resize window in full screen mode denied." << endl;
    }
    else if (_inPixels)
    {
        auto const screenSize = size();
        if (_width == 0 && _height == 0)
        {
            _width = screenSize.width();
            _height = screenSize.height();
        }
        else
        {
            if (!_width)
                _width = screenSize.width();

            if (!_height)
                _height = screenSize.height();
        }
        profile().terminalSize.width = _width / fonts_.regular.first.get().maxAdvance();
        profile().terminalSize.height = _height / fonts_.regular.first.get().lineHeight();
        resizePending = true;
    }
    else
    {
        if (_width == 0 && _height == 0)
            resize(_width, _height);
        else
        {
            if (!_width)
                _width = profile().terminalSize.width;

            if (!_height)
                _height = profile().terminalSize.height;

            profile().terminalSize.width = _width;
            profile().terminalSize.height = _height;
            resizePending = true;
        }
    }

    if (resizePending)
    {
        post([this]() {
            terminalView_->setTerminalSize(profile().terminalSize);
            auto const width = profile().terminalSize.width * fonts_.regular.first.get().maxAdvance();
            auto const height = profile().terminalSize.height * fonts_.regular.first.get().lineHeight();
            resize(width, height);
            setScreenDirty();
            update();
        });
    }
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
        close();
}

void TerminalWidget::copyToClipboard(std::string_view const& _text)
{
    if (QClipboard* clipboard = QGuiApplication::clipboard(); clipboard != nullptr)
        clipboard->setText(QString::fromUtf8(_text.data(), static_cast<int>(_text.size())));
}

void TerminalWidget::dumpState()
{
    terminalView_->terminal().screen().dumpState("Dump screen state.");
    terminalView_->renderer().dumpState(std::cout);
}
// }}}

} // namespace contour
