/**
 * This file is part of the "contour" project
 *   Copyright (c) 2019 Christian Parpart <christian@parpart.family>
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
#include <contour/TerminalWindow.h>

#include <QClipboard>
#include <QDebug>
#include <QDesktopServices>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QOpenGLFunctions>
#include <QOpenGLWindow>
#include <QProcess>
#include <QScreen>
#include <QTimer>

#include <QMetaEnum>

#if defined(CONTOUR_BLUR_PLATFORM_KWIN)
#include <KWindowEffects>
#endif

#include <cstring>
#include <fstream>

using namespace std;
using namespace std::placeholders;

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

namespace contour {

using terminal::view::GLRenderer;
using actions::Action;

namespace {
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

        auto const modifiers = makeModifier(_mods);

        if (auto i = find_if(begin(mapping), end(mapping), [_key](auto const& x) { return x.first == _key; }); i != end(mapping))
            return { InputEvent{KeyInputEvent{i->second, modifiers}} };

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

    QSurfaceFormat surfaceFormat()
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
            format.setProfile(QSurfaceFormat::CompatibilityProfile);
            format.setRenderableType(QSurfaceFormat::OpenGL);
        }

        format.setAlphaBufferSize(8);
        format.setSwapBehavior(QSurfaceFormat::DoubleBuffer);
        format.setSwapInterval(1);

#if !defined(NDEBUG)
        format.setOption(QSurfaceFormat::DebugContext);
#endif

        return format;
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
}

TerminalWindow::TerminalWindow(config::Config _config, string _profileName, string _programPath) :
    now_{ chrono::steady_clock::now() },
    config_{ move(_config) },
    profileName_{ move(_profileName) },
    profile_{ *config_.profile(profileName_) },
    programPath_{ move(_programPath) },
    logger_{
        config_.logFilePath
            ? LoggingSink{config_.loggingMask, config_.logFilePath->string()}
            : LoggingSink{config_.loggingMask, &cout}
    },
    fontManager_{},
    regularFont_{
        fontManager_.load(
            profile().fontFamily,
            static_cast<unsigned>(profile().fontSize * contentScale())
        )
    },
    terminalView_{},
    configFileChangeWatcher_{
        config_.backingFilePath,
        bind(&TerminalWindow::onConfigReload, this, _1)
    },
    updateTimer_(this)
{
    // qDebug() << "TerminalWindow:"
    //     << QString::fromUtf8(fmt::format("{}x{}", config_.terminalSize.columns, config_.terminalSize.rows).c_str())
    //     << "fontSize:" << profile().fontSize
    //     << "contentScale:" << contentScale();

    setFormat(surfaceFormat());

    updateTimer_.setSingleShot(true);
    connect(&updateTimer_, &QTimer::timeout, this, QOverload<>::of(&TerminalWindow::blinkingCursorUpdate));

    connect(this, SIGNAL(screenChanged(QScreen*)), this, SLOT(onScreenChanged(QScreen*)));
    connect(this, SIGNAL(frameSwapped()), this, SLOT(onFrameSwapped()));

    if (!loggingSink_.good())
        throw runtime_error{ "Failed to open log file." };

    if (profile().backgroundBlur && !enableBackgroundBlur(true))
        throw runtime_error{ "Could not enable background blur." };

    if (!regularFont_.get().isFixedWidth())
        cerr << "Regular font is not a fixed-width font." << endl;

    resize(
        profile().terminalSize.columns * regularFont_.get().maxAdvance(),
        profile().terminalSize.rows * regularFont_.get().lineHeight()
    );
}

TerminalWindow::~TerminalWindow()
{
    makeCurrent(); // XXX must be called.
    // ...
}

void TerminalWindow::blinkingCursorUpdate()
{
    update();
}

void TerminalWindow::onFrameSwapped()
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
                assert(!"The impossible happened, painting but painting. Shakesbeer.");
                [[fallthrough]];
            case State::DirtyPainting:
                // FIXME: Qt/Wayland!
                // QCoreApplication::postEvent() works on both, but horrorble performance
                // requestUpdate() works on X11 as well as Wayland but isn't mentioned in any QtGL docs.
                // update() is meant to be correct way and works on X11 but freezes on Wayland!

                //QCoreApplication::postEvent(this, new QEvent(QEvent::UpdateRequest));
                //requestUpdate();
                update();
                return;
            case State::CleanPainting:
                if (!state_.compare_exchange_strong(state, State::CleanIdle))
                    break;
                [[fallthrough]];
            case State::CleanIdle:
                STATS_ZERO(consecutiveRenderCount);
                if (profile().cursorDisplay == terminal::CursorDisplay::Blink
                        && this->terminalView_->terminal().cursor().visible)
                    updateTimer_.start(terminalView_->terminal().nextRender(chrono::steady_clock::now()));
                return;
        }
    }
}

void TerminalWindow::onScreenChanged(QScreen* _screen)
{
    // TODO: Update font size and window size based on new screen's contentScale().
    (void) _screen;
}

void TerminalWindow::initializeGL()
{
    initializeOpenGLFunctions();

    qDebug() << "GL type:" << (QOpenGLContext::currentContext()->isOpenGLES() ? "OpenGL/ES" : "OpenGL");

    terminalView_ = make_unique<terminal::view::TerminalView>(
        now_,
        profile().terminalSize,
        profile().maxHistoryLineCount,
        config_.wordDelimiters,
        bind(&TerminalWindow::onSelectionComplete, this),
        bind(&TerminalWindow::onScreenBufferChanged, this, _1),
        bind(&TerminalWindow::onBell, this),
        regularFont_.get(),
        profile().cursorShape,
        profile().cursorDisplay,
        profile().cursorBlinkInterval,
        profile().colors,
        profile().backgroundOpacity,
        profile().shell,
        profile().env,
        ortho(0.0f, static_cast<float>(width()), 0.0f, static_cast<float>(height())),
        bind(&TerminalWindow::onScreenUpdate, this),
        bind(&TerminalWindow::onWindowTitleChanged, this),
        bind(&TerminalWindow::onDoResize, this, _1, _2, _3),
        bind(&TerminalWindow::onTerminalClosed, this),
        *config::Config::loadShaderConfig(config::ShaderClass::Background),
        *config::Config::loadShaderConfig(config::ShaderClass::Text),
        *config::Config::loadShaderConfig(config::ShaderClass::Cursor),
        ref(logger_)
    );

    terminalView_->terminal().setLogRawOutput((config_.loggingMask & LogMask::RawOutput) != LogMask::None);
    terminalView_->terminal().setLogTraceOutput((config_.loggingMask & LogMask::TraceOutput) != LogMask::None);
    terminalView_->terminal().setTabWidth(profile().tabWidth);
}

void TerminalWindow::resizeEvent(QResizeEvent* _event)
{
    try
    {
        QOpenGLWindow::resizeEvent(_event);

        if (width() != 0 && height() != 0)
        {
            terminalView_->resize(width(), height());
            terminalView_->setProjection(
                ortho(
                    0.0f, static_cast<float>(width()),
                    0.0f, static_cast<float>(height())
                )
            );
            setScreenDirty();
        }
    }
    catch (std::exception const& e)
    {
        cerr << fmt::format("resizeEvent: Unhandled exception caught ({}). {}\n", typeid(e).name(), e.what());
    }
}

void TerminalWindow::paintGL()
{
    try {
        STATS_INC(consecutiveRenderCount);
        state_.store(State::CleanPainting);
        now_ = chrono::steady_clock::now();

        glViewport(0, 0, width() * contentScale(), height() * contentScale());

        {
            auto calls = decltype(queuedCalls_){};
            {
                auto lg = lock_guard{queuedCallsLock_};
                swap(queuedCalls_, calls);
            }
            for_each(begin(calls), end(calls), [](auto& _call) { _call(); });
        }

        QVector4D const bg = GLRenderer::canonicalColor(profile().colors.defaultBackground, profile().backgroundOpacity);
        glClearColor(bg[0], bg[1], bg[2], bg[3]);
        glClear(GL_COLOR_BUFFER_BIT);

        //terminal::view::render(terminalView_, now_);
        STATS_SET(updatesSinceRendering) terminalView_->render(now_);
    }
    catch (exception const& ex)
    {
        cerr << "Unhandled exception caught in render thread! " << typeid(ex).name() << "; " << ex.what() << endl;
    }
}

bool TerminalWindow::reloadConfigValues()
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
        configLogger(fmt::format("Unhandled exception during configuration reload caught. {}", e.what()));
    }

    if (!newConfig.profile(profileName_))
        configLogger(fmt::format("Currently active profile with name '{}' gone.", profileName_));

    if (configFailures)
    {
        cerr << "Failed to load configuration.\n";
        return false;
    }

    logger_ =
        newConfig.logFilePath
            ? LoggingSink{newConfig.loggingMask, newConfig.logFilePath->string()}
            : LoggingSink{newConfig.loggingMask, &cout};

    terminalView_->terminal().setWordDelimiters(newConfig.wordDelimiters);
    config_ = move(newConfig);
    setProfile(*config_.profile(profileName_));

    terminalView_->terminal().setLogRawOutput((config_.loggingMask & LogMask::RawOutput) != LogMask::None);
    terminalView_->terminal().setLogTraceOutput((config_.loggingMask & LogMask::TraceOutput) != LogMask::None);

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

void TerminalWindow::keyPressEvent(QKeyEvent* _keyEvent)
{
    auto const keySeq = QKeySequence(isModifier(static_cast<Qt::Key>(_keyEvent->key()))
                                        ? _keyEvent->modifiers()
                                        : _keyEvent->modifiers() | _keyEvent->key());

    // if (!_keyEvent->text().isEmpty())
    //     qDebug() << "keyPress:" << "text:" << _keyEvent->text() << "seq:" << keySeq
    //         << "key:" << static_cast<Qt::Key>(_keyEvent->key())
    //         << QString::fromLatin1(fmt::format("0x{:x}", keySeq[0]).c_str());

    if (auto i = config_.keyMappings.find(keySeq); i != end(config_.keyMappings))
    {
        auto const& actions = i->second;
        for (auto const& action : actions)
            executeAction(action);
    }
    else if (auto const inputEvent = mapQtToTerminalKeyEvent(_keyEvent->key(), _keyEvent->modifiers()))
    {
        terminalView_->terminal().send(*inputEvent, now_);
    }
    else if (!_keyEvent->text().isEmpty())
    {
        for (auto const ch : _keyEvent->text().toUcs4())
        {
            auto const modifiers = makeModifier(_keyEvent->modifiers());
            auto const inputEvent = terminal::InputEvent{terminal::CharInputEvent{ch, modifiers}};
            terminalView_->terminal().send(inputEvent, now_);
        }
    }
}

void TerminalWindow::wheelEvent(QWheelEvent* _event)
{
    auto const button = _event->delta() > 0 ? terminal::MouseButton::WheelUp : terminal::MouseButton::WheelDown;
    auto const mouseEvent = terminal::MousePressEvent{button, makeModifier(_event->modifiers())};

    executeInput(mouseEvent);
}

void TerminalWindow::executeInput(terminal::MouseEvent const& _mouseEvent)
{
    now_ = chrono::steady_clock::now();

    if (auto mapping = config_.mouseMappings.find(_mouseEvent); mapping != config_.mouseMappings.end())
    {
        for (auto const& action : mapping->second)
            executeAction(action);
    }
    else
        terminalView_->terminal().send(_mouseEvent, now_);
}

void TerminalWindow::mousePressEvent(QMouseEvent* _event)
{
    auto const mouseButton = makeMouseButton(_event->button());
    executeInput(terminal::MousePressEvent{mouseButton, makeModifier(_event->modifiers())});

    setScreenDirty();
    update();
}

void TerminalWindow::mouseReleaseEvent(QMouseEvent* _mouseRelease)
{
    auto const mouseButton = makeMouseButton(_mouseRelease->button());
    executeInput(terminal::MouseReleaseEvent{mouseButton});

    if (terminalView_->terminal().isSelectionAvailable())
    {
        setScreenDirty();
        update();
    }
}

void TerminalWindow::mouseMoveEvent(QMouseEvent* _event)
{
    try
    {
        now_ = chrono::steady_clock::now();

        int const topPadding = abs(height() - static_cast<int>(terminalView_->terminal().screenSize().rows * terminalView_->cellHeight()));
        if (_event->y() < topPadding)
            return;

        unsigned const row = static_cast<unsigned>(1 + (max(_event->y(), 0) - topPadding) / terminalView_->cellHeight());
        unsigned const col = static_cast<unsigned>(1 + max(_event->x(), 0) / terminalView_->cellWidth());

        terminalView_->terminal().send(terminal::MouseMoveEvent{row, col}, now_);

        if (terminalView_->terminal().isSelectionAvailable()) // && only if selection has changed!
        {
            setScreenDirty();
            update();
        }
    }
    catch (std::exception const& e)
    {
        cerr << fmt::format("Unhandled exception caught ({}). {}\n", typeid(e).name(), e.what());
    }
}

void TerminalWindow::focusInEvent(QFocusEvent* _event) // TODO: paint with "normal" colors
{
    QOpenGLWindow::focusInEvent(_event);

    // as per Qt-documentation, some platform implementations reset the cursor when leaving the
    // window, so we have to re-apply our desired cursor in focusInEvent().
    using Type = terminal::ScreenBuffer::Type;
    switch (terminalView_->terminal().screenBufferType())
    {
        case Type::Main:
            setCursor(Qt::IBeamCursor);
            break;
        case Type::Alternate:
            setCursor(Qt::ArrowCursor);
            break;
    }
}

void TerminalWindow::focusOutEvent(QFocusEvent* _event) // TODO maybe paint with "faint" colors
{
    QOpenGLWindow::focusOutEvent(_event);
}

bool TerminalWindow::event(QEvent* _event)
{
    //qDebug() << "TerminalWindow.event:" << *_event;

    if (_event->type() == QEvent::Close)
        terminalView_->process().terminate(terminal::Process::TerminationHint::Hangup);

    return QOpenGLWindow::event(_event);
}

bool TerminalWindow::fullscreen() const
{
    return visibility() == QWindow::FullScreen;
}

void TerminalWindow::toggleFullScreen()
{
    if (visibility() == QWindow::FullScreen)
        setVisibility(QWindow::Windowed);
    else
        setVisibility(QWindow::FullScreen);
}

bool TerminalWindow::setFontSize(unsigned _fontSize)
{
    //qDebug() << "TerminalWindow.setFontSize" << _fontSize;

    if (_fontSize < 5) // Let's not be crazy.
        return false;

    if (_fontSize > 100)
        return false;

    if (!terminalView_->setFontSize(static_cast<unsigned>(_fontSize * contentScale())))
        return false;

    profile().fontSize = _fontSize;

    // resize terminalView (same pixels, but adjusted terminal rows/columns and margin)
    auto const windowSize = size();
    terminalView_->resize(windowSize.width(), windowSize.height());

    return true;
}

void TerminalWindow::executeAction(Action const& _action)
{
    bool const dirty = visit(overloaded{
        [this](actions::WriteScreen const& _write) -> bool {
            terminalView_->terminal().writeToScreen(_write.chars);
            return false;
        },
        [&](actions::ToggleFullScreen) -> bool {
            toggleFullScreen();
            return false;
        },
        [&](actions::IncreaseFontSize) -> bool {
            setFontSize(profile().fontSize + 1);
            return false;
        },
        [&](actions::DecreaseFontSize) -> bool {
            setFontSize(profile().fontSize - 1);
            return false;
        },
        [&](actions::IncreaseOpacity) -> bool {
            ++profile().backgroundOpacity;
            terminalView_->setBackgroundOpacity(profile().backgroundOpacity);
            return true;
        },
        [&](actions::DecreaseOpacity) -> bool {
            --profile().backgroundOpacity;
            terminalView_->setBackgroundOpacity(profile().backgroundOpacity);
            return true;
        },
        [&](actions::ScreenshotVT) -> bool {
            auto const screenshot = terminalView_->terminal().screenshot();
            ofstream ofs{ "screenshot.vt", ios::trunc | ios::binary };
            ofs << screenshot;
            return false;
        },
        [this](actions::SendChars const& chars) -> bool {
            for (auto const ch : chars.chars)
                terminalView_->terminal().send(terminal::CharInputEvent{static_cast<char32_t>(ch), terminal::Modifier::None}, now_);
            return false;
        },
        [this](actions::ScrollOneUp) -> bool {
            return terminalView_->terminal().scrollUp(1);
        },
        [this](actions::ScrollOneDown) -> bool {
            return terminalView_->terminal().scrollDown(1);
        },
        [this](actions::ScrollUp) -> bool {
            return terminalView_->terminal().scrollUp(profile().historyScrollMultiplier);
        },
        [this](actions::ScrollDown) -> bool {
            return terminalView_->terminal().scrollDown(profile().historyScrollMultiplier);
        },
        [this](actions::ScrollPageUp) -> bool {
            return terminalView_->terminal().scrollUp(profile().terminalSize.rows / 2);
        },
        [this](actions::ScrollPageDown) -> bool {
            return terminalView_->terminal().scrollDown(profile().terminalSize.rows / 2);
        },
        [this](actions::ScrollMarkUp) -> bool {
            return terminalView_->terminal().scrollMarkUp();
        },
        [this](actions::ScrollMarkDown) -> bool {
            return terminalView_->terminal().scrollMarkDown();
        },
        [this](actions::ScrollToTop) -> bool {
            return terminalView_->terminal().scrollToTop();
        },
        [this](actions::ScrollToBottom) -> bool {
            return terminalView_->terminal().scrollToBottom();
        },
        [this](actions::CopySelection) -> bool {
            string const text = extractSelectionText();
            if (QClipboard* clipboard = QGuiApplication::clipboard(); clipboard != nullptr)
                clipboard->setText(QString::fromUtf8(text.c_str(), static_cast<int>(text.size())));
            return false;
        },
        [this](actions::PasteSelection) -> bool {
            if (QClipboard* clipboard = QGuiApplication::clipboard(); clipboard != nullptr)
            {
                string const text = clipboard->text(QClipboard::Selection).toUtf8().toStdString();
                terminalView_->terminal().sendPaste(string_view{text});
            }
            return false;
        },
        [this](actions::PasteClipboard) -> bool {
            if (QClipboard* clipboard = QGuiApplication::clipboard(); clipboard != nullptr)
            {
                string const text = clipboard->text(QClipboard::Clipboard).toUtf8().toStdString();
                terminalView_->terminal().sendPaste(string_view{text});
            }
            return false;
        },
        [this](actions::ChangeProfile const& v) -> bool {
            cerr << fmt::format("Changing profile to '{}'.", v.name) << endl;
            if (auto newProfile = config_.profile(v.name); newProfile)
                setProfile(*newProfile);
            else
                cerr << fmt::format("No such profile: '{}'.", v.name) << endl;
            return true;
        },
        [this](actions::NewTerminal const& v) -> bool {
            spawnNewTerminal(v.profileName.value_or(profileName_));
            return false;
        },
        [this](actions::OpenConfiguration) -> bool {
            if (!QDesktopServices::openUrl(QUrl(QString::fromUtf8(config_.backingFilePath.string().c_str()))))
                cerr << "Could not open configuration file \"" << config_.backingFilePath << "\"" << endl;
            return false;
        },
        [this](actions::OpenFileManager) -> bool {
            // TODO open file manager at current window's current working directory (via /proc/self/cwd)
            return false;
        },
        [this](actions::Quit) -> bool {
            // XXX: later warn here when more then one terminal view is open
            terminalView_->terminal().device().close();
            return false;
        }
    }, _action);

    if (dirty)
    {
        setScreenDirty();
        update();
    }
}

void TerminalWindow::setProfile(config::TerminalProfile newProfile)
{
    terminalView_->terminal().setTabWidth(newProfile.tabWidth);
    if (newProfile.fontFamily != profile().fontFamily)
    {
        regularFont_ = fontManager_.load(
            newProfile.fontFamily,
            static_cast<unsigned>(newProfile.fontSize * contentScale())
        );
        terminalView_->setFont(regularFont_.get());
    }
    else if (newProfile.fontSize != profile().fontSize)
        setFontSize(newProfile.fontSize);

    if (newProfile.terminalSize != profile().terminalSize)
    {
        auto const screenSize = size();

        auto const terminalSize = terminal::WindowSize{
            screenSize.width() / regularFont_.get().maxAdvance(),
            screenSize.height() / regularFont_.get().lineHeight()
        };

        terminalView_->setTerminalSize(terminalSize);
    }

    terminalView_->terminal().setMaxHistoryLineCount(newProfile.maxHistoryLineCount);

    terminalView_->setColorProfile(newProfile.colors);

    if (newProfile.cursorShape != profile().cursorShape)
        terminalView_->setCursorShape(newProfile.cursorShape);

    if (newProfile.cursorDisplay != profile().cursorDisplay)
        terminalView_->terminal().setCursorDisplay(newProfile.cursorDisplay);

    if (newProfile.backgroundBlur != profile().backgroundBlur)
        enableBackgroundBlur(newProfile.backgroundBlur);

    if (newProfile.tabWidth != profile().tabWidth)
        terminalView_->terminal().setTabWidth(newProfile.tabWidth);

    profile_ = move(newProfile);
}

void TerminalWindow::onSelectionComplete()
{
    if (QClipboard* clipboard = QGuiApplication::clipboard(); clipboard != nullptr)
    {
        string const text = extractSelectionText();
        clipboard->setText(QString::fromUtf8(text.c_str(), static_cast<int>(text.size())), QClipboard::Selection);
    }
}

string TerminalWindow::extractSelectionText()
{
    using namespace terminal;
    cursor_pos_t lastColumn = 0;
    string text;
    string currentLine;

    terminalView_->terminal().renderSelection([&](cursor_pos_t /*_row*/, cursor_pos_t _col, Screen::Cell const& _cell) {
        if (_col <= lastColumn)
        {
            text += currentLine;
            text += '\n';
            currentLine.clear();
        }
        if (_cell.character)
            currentLine += utf8::to_string(utf8::encode(_cell.character));
        lastColumn = _col;
    });
    text += currentLine;

    return text;
}

void TerminalWindow::onScreenBufferChanged(terminal::ScreenBuffer::Type _type)
{
    using Type = terminal::ScreenBuffer::Type;
    switch (_type)
    {
        case Type::Main:
            setCursor(Qt::IBeamCursor);
            break;
        case Type::Alternate:
            setCursor(Qt::ArrowCursor);
            break;
    }
}

void TerminalWindow::onBell()
{
    if (logger_.sink())
        *logger_.sink() << "TODO: Beep!\n";
    // QApplication::beep() requires Qt Widgets dependency. doesn't suound good.
    // so maybe just a visual bell then? That would require additional OpenGL/shader work then though.
}

void TerminalWindow::spawnNewTerminal(std::string const& _profileName)
{
    // TODO: config option to either spawn new terminal via new process (default) or just as second window.
    QString const program = QString::fromUtf8(programPath_.c_str());
    QStringList args;
    if (!_profileName.empty())
        args << "-p" << QString::fromStdString(_profileName);

    QProcess::startDetached(program, args);
}

float TerminalWindow::contentScale() const
{
    return screen()->devicePixelRatio();
}

void TerminalWindow::onScreenUpdate()
{
    if (profile().autoScrollOnUpdate && terminalView_->terminal().scrollOffset())
        terminalView_->terminal().scrollToBottom();

    if (setScreenDirty())
        QCoreApplication::postEvent(this, new QEvent(QEvent::UpdateRequest));
}

void TerminalWindow::onWindowTitleChanged()
{
    post([this]() {
        auto const terminalTitle = terminalView_->terminal().windowTitle();
        auto const title = terminalTitle.empty()
            ? "contour"s
            : fmt::format("{} - contour", terminalTitle);
        setTitle(QString::fromUtf8(title.c_str()));
    });
}

void TerminalWindow::onDoResize(unsigned _width, unsigned _height, bool _inPixels)
{
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
        profile().terminalSize.columns = _width / regularFont_.get().maxAdvance();
        profile().terminalSize.rows = _height / regularFont_.get().lineHeight();
        resizePending = true;
    }
    else
    {
        if (_width == 0 && _height == 0)
            resize(_width, _height);
        else
        {
            if (!_width)
                _width = profile().terminalSize.columns;

            if (!_height)
                _height = profile().terminalSize.rows;

            profile().terminalSize.columns = _width;
            profile().terminalSize.rows = _height;
            resizePending = true;
        }
    }

    if (resizePending)
    {
        post([this]() {
            terminalView_->setTerminalSize(profile().terminalSize);
            auto const width = profile().terminalSize.columns * regularFont_.get().maxAdvance();
            auto const height = profile().terminalSize.rows * regularFont_.get().lineHeight();
            resize(width, height);
            setScreenDirty();
            update();
        });
    }
}

void TerminalWindow::onConfigReload(FileChangeWatcher::Event /*_event*/)
{
    post([this]() {
        if (reloadConfigValues())
        {
            setScreenDirty();
            update();
        }
    });
}

bool TerminalWindow::enableBackgroundBlur([[maybe_unused]] bool _enable)
{
#if defined(CONTOUR_BLUR_PLATFORM_KWIN)
    KWindowEffects::enableBlurBehind(winId(), _enable);
    KWindowEffects::enableBackgroundContrast(winId(), !_enable);
    return true;
#elif defined(_WIN32)
    // Awesome hack with the noteworty links:
    // * https://gist.github.com/ethanhs/0e157e4003812e99bf5bc7cb6f73459f (used as code template)
    // * https://github.com/riverar/sample-win32-acrylicblur/blob/master/MainWindow.xaml.cs
    // * https://stackoverflow.com/questions/44000217/mimicking-acrylic-in-a-win32-app
    // p.s.: if you find a more official way to do it, please PR me. :)

    bool success = false;
    if (HWND hwnd = (HWND)winId(); hwnd != nullptr)
    {
        const HINSTANCE hModule = LoadLibrary(TEXT("user32.dll"));
        if (hModule)
        {
           enum WindowCompositionAttribute : int {
               // ...
               WCA_ACCENT_POLICY = 19,
               // ...
           };
           enum AcceptState : int {
               ACCENT_DISABLED = 0,
               ACCENT_ENABLE_GRADIENT = 1,
               ACCENT_ENABLE_TRANSPARENTGRADIENT = 2,
               ACCENT_ENABLE_BLURBEHIND = 3,
               ACCENT_ENABLE_ACRYLICBLURBEHIND = 4,
               ACCENT_ENABLE_HOSTBACKDROP = 5,
           };
            struct ACCENTPOLICY
            {
                AcceptState nAccentState;
                int nFlags;
                int nColor;
                int nAnimationId;
            };
            struct WINCOMPATTRDATA
            {
                WindowCompositionAttribute nAttribute;
                void const* pData;
                ULONG ulDataSize;
            };
            typedef BOOL(WINAPI *pSetWindowCompositionAttribute)(HWND, WINCOMPATTRDATA const*);
            const pSetWindowCompositionAttribute SetWindowCompositionAttribute = (pSetWindowCompositionAttribute)GetProcAddress(hModule, "SetWindowCompositionAttribute");
            if (SetWindowCompositionAttribute)
            {
               auto const policy = _enable
                   ? ACCENTPOLICY{ ACCENT_ENABLE_BLURBEHIND, 0, 0, 0 }
                   : ACCENTPOLICY{ ACCENT_DISABLED, 0, 0, 0 };
               auto const data = WINCOMPATTRDATA{ WCA_ACCENT_POLICY, &policy, sizeof(ACCENTPOLICY) };
               BOOL rs = SetWindowCompositionAttribute(hwnd, &data);
               success = rs != FALSE;
            }
            FreeLibrary(hModule);
        }
    }
    return success;
#else
   if (_enable)
       // Get me working on other platforms/compositors (such as OSX, Gnome, ...), please.
       return false;
   else
       return true;
#endif
}

void TerminalWindow::post(std::function<void()> _fn)
{
	auto lg = lock_guard{queuedCallsLock_};
    queuedCalls_.emplace_back(move(_fn));
    QCoreApplication::postEvent(this, new QEvent(QEvent::UpdateRequest));
}

inline char const* signalName(int _signo)
{
#if defined(__unix__) || defined(APPLE)
    return strsignal(_signo);
#else
    return "unknown";
#endif
}

void TerminalWindow::onTerminalClosed()
{
    using terminal::Process;

    terminal::Process::ExitStatus const ec = terminalView_->process().wait();
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

} // namespace contour
