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

#include <cstring>
#include <fstream>

using namespace std;
using namespace std::placeholders;

#if defined(CONTOUR_PERF_STATS)
#define STATS_INC(name)   ++(stats_. name)
#define STATS_ZERO(name)  (stats_. name = 0)
#define STATS_GET(name)   (stats_. name).load()
#else
#define STATS_INC(name)   do {} while (0)
#define STATS_ZERO(name)  do {} while (0)
#define STATS_GET(name)   0
#endif

namespace contour {

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
        format.setRedBufferSize(8);
        format.setGreenBufferSize(8);
        format.setBlueBufferSize(8);
        format.setAlphaBufferSize(8);
        format.setRenderableType(QSurfaceFormat::OpenGLES);
        format.setSwapBehavior(QSurfaceFormat::DoubleBuffer);
        format.setProfile(QSurfaceFormat::CoreProfile);
        format.setVersion(3, 3);
        format.setSwapInterval(1);

        return format;
    }
}

TerminalWindow::TerminalWindow(Config _config, std::string _programPath) :
    now_{ chrono::steady_clock::now() },
    config_{ move(_config) },
    programPath_{ move(_programPath) },
    logger_{
        config_.logFilePath
            ? LoggingSink{config_.loggingMask, config_.logFilePath->string()}
            : LoggingSink{config_.loggingMask, &cout}
    },
    fontManager_{},
    regularFont_{
        fontManager_.load(
            config_.fontFamily,
            static_cast<unsigned>(config_.fontSize * contentScale())
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
    //     << QString::fromUtf8(fmt::format("{}x{}", _config.terminalSize.columns, _config.terminalSize.rows).c_str())
    //     << "fontSize:" << config_.fontSize
    //     << "contentScale:" << contentScale();

    // FIXME: blinking cursor
    // updateTimer_.setInterval(config_.cursorBlinkInterval.count());
    updateTimer_.setSingleShot(true);
    connect(&updateTimer_, &QTimer::timeout, this, QOverload<>::of(&TerminalWindow::connectAndUpdate));

    connect(this, SIGNAL(screenChanged(QScreen*)), this, SLOT(onScreenChanged(QScreen*)));

    if (!loggingSink_.good())
        throw runtime_error{ "Failed to open log file." };

    if (!regularFont_.get().isFixedWidth())
        throw runtime_error{ "Regular font is not a fixed-width font." };

    if (config_.backgroundBlur && !enableBackgroundBlur(true))
        throw runtime_error{ "Could not enable background blur." };

    resize(
        config_.terminalSize.columns * regularFont_.get().maxAdvance(),
        config_.terminalSize.rows * regularFont_.get().lineHeight()
    );
}

void TerminalWindow::connectAndUpdate()
{
    bool updating = updating_.load();
    if (!updating && updating_.compare_exchange_strong(updating, true))
        connect(this, SIGNAL(frameSwapped()), this, SLOT(onFrameSwapped()));

    update();
}

TerminalWindow::~TerminalWindow()
{
    makeCurrent(); // XXX must be called.
    // ...
}

void TerminalWindow::onFrameSwapped()
{
#if defined(CONTOUR_PERF_STATS)
    qDebug() << QString::fromStdString(fmt::format(
        "Consecutive renders: {}, updates since last render: {}, last swap=: {}; {}",
        STATS_GET(consecutiveRenderCount),
        STATS_GET(updatesSinceRendering),
        STATS_GET(updatesSinceLastSwap),
        terminalView_->renderer().metrics().to_string()
    ));
#endif

    bool const dirty = screenDirty_.load();
    bool updating = updating_.load();

    STATS_ZERO(updatesSinceLastSwap);

    if (dirty)
        update();
    else
    {
        if (updating && updating_.compare_exchange_strong(updating, false))
        {
            STATS_ZERO(consecutiveRenderCount);
            disconnect(this, SIGNAL(frameSwapped()), this, SLOT(onFrameSwapped()));
        }

        if (config_.cursorDisplay == terminal::CursorDisplay::Blink
                && this->terminalView_->terminal().cursor().visible)
            updateTimer_.start(terminalView_->terminal().nextRender(chrono::steady_clock::now()));
    }
}

void TerminalWindow::onScreenChanged(QScreen* _screen)
{
    // TODO: Update font size and window size based on new screen's contentScale().
    (void) _screen;
}

void TerminalWindow::initializeGL()
{
    setFormat(surfaceFormat());
    initializeOpenGLFunctions();

    terminalView_ = make_unique<terminal::view::TerminalView>(
        now_,
        config_.terminalSize,
        config_.maxHistoryLineCount,
        config_.wordDelimiters,
        regularFont_.get(),
        config_.cursorShape,
        config_.cursorDisplay,
        config_.cursorBlinkInterval,
        config_.colorProfile,
        config_.backgroundOpacity,
        config_.shell,
        config_.env,
        ortho(0.0f, static_cast<float>(width()), 0.0f, static_cast<float>(height())),
        bind(&TerminalWindow::onScreenUpdate, this),
        bind(&TerminalWindow::onWindowTitleChanged, this),
        bind(&TerminalWindow::onDoResize, this, _1, _2, _3),
        bind(&TerminalWindow::onTerminalClosed, this),
        ref(logger_)
    );

    terminalView_->terminal().setTabWidth(config_.tabWidth);
}

void TerminalWindow::resizeEvent(QResizeEvent* _event)
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
        screenDirty_ = true;
    }
}

inline QVector4D makeColor(terminal::RGBColor const& _color, terminal::Opacity _opacity)
{
    return QVector4D{
        static_cast<float>(_color.red) / 255.0f,
        static_cast<float>(_color.green) / 255.0f,
        static_cast<float>(_color.blue) / 255.0f,
        static_cast<float>(_opacity) / 255.0f};
}

void TerminalWindow::paintGL()
{
    try {
        STATS_INC(consecutiveRenderCount);
        screenDirty_ = false;
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

        QVector4D const bg = makeColor(config_.colorProfile.defaultBackground, config_.backgroundOpacity);
        glClearColor(bg[0], bg[1], bg[2], bg[3]);
        glClear(GL_COLOR_BUFFER_BIT);

        //terminal::view::render(terminalView_, now_);
        terminalView_->render(now_);
    }
    catch (exception const& ex)
    {
        cerr << "Unhandled exception caught in render thread! " << typeid(ex).name() << "; " << ex.what() << endl;
    }
}

bool TerminalWindow::reloadConfigValues()
{
    auto filePath = config_.backingFilePath.string();
    auto newConfig = Config{};

    try
    {
        loadConfigFromFile(newConfig, filePath);
    }
    catch (exception const& e)
    {
        //TODO: logger_.error(e.what());
        cerr << "Failed to load configuration. " << e.what() << endl;
        return false;
    }

    logger_ =
        newConfig.logFilePath
            ? LoggingSink{newConfig.loggingMask, newConfig.logFilePath->string()}
            : LoggingSink{newConfig.loggingMask, &cout};

    bool windowResizeRequired = false;

    terminalView_->terminal().setTabWidth(newConfig.tabWidth);
    if (newConfig.fontFamily != config_.fontFamily)
    {
        regularFont_ = fontManager_.load(
            newConfig.fontFamily,
            static_cast<unsigned>(newConfig.fontSize * contentScale())
        );
        terminalView_->setFont(regularFont_.get());
        windowResizeRequired = true;
    }
    else if (newConfig.fontSize != config_.fontSize)
    {
        windowResizeRequired |= setFontSize(newConfig.fontSize, false);
    }

    if (newConfig.terminalSize != config_.terminalSize && !fullscreen())
        windowResizeRequired |= terminalView_->setTerminalSize(config_.terminalSize);

    terminalView_->terminal().setWordDelimiters(newConfig.wordDelimiters);

    if (windowResizeRequired && !fullscreen())
    {
        auto const width = newConfig.terminalSize.columns * regularFont_.get().maxAdvance();
        auto const height = newConfig.terminalSize.rows * regularFont_.get().lineHeight();
        resize(width, height);
    }

    terminalView_->terminal().setMaxHistoryLineCount(newConfig.maxHistoryLineCount);

    if (newConfig.colorProfile.cursor != config_.colorProfile.cursor)
        terminalView_->setCursorColor(newConfig.colorProfile.cursor);

    if (newConfig.cursorShape != config_.cursorShape)
        terminalView_->setCursorShape(newConfig.cursorShape);

    if (newConfig.cursorDisplay != config_.cursorDisplay)
        terminalView_->terminal().setCursorDisplay(newConfig.cursorDisplay);

    if (newConfig.backgroundBlur != config_.backgroundBlur)
        enableBackgroundBlur(newConfig.backgroundBlur);


    if (newConfig.tabWidth != config_.tabWidth)
        terminalView_->terminal().setTabWidth(newConfig.tabWidth);

    config_ = move(newConfig);

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

    if (terminalView_->terminal().isSelectionAvailable())
    {
        screenDirty_ = true;
        update();
    }
}

void TerminalWindow::mouseReleaseEvent(QMouseEvent* _mouseRelease)
{
    auto const mouseButton = makeMouseButton(_mouseRelease->button());
    executeInput(terminal::MouseReleaseEvent{mouseButton});

    if (terminalView_->terminal().isSelectionAvailable())
    {
        screenDirty_ = true;
        update();
    }
}

void TerminalWindow::mouseMoveEvent(QMouseEvent* _event)
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
        screenDirty_ = true;
        update();
    }
}

void TerminalWindow::focusInEvent(QFocusEvent* _event) // TODO: paint with "normal" colors
{
    (void) _event;
}

void TerminalWindow::focusOutEvent(QFocusEvent* _event) // TODO maybe paint with "faint" colors
{
    (void) _event;
}

bool TerminalWindow::event(QEvent* _event)
{
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

bool TerminalWindow::setFontSize(unsigned _fontSize, bool _resizeWindowIfNeeded)
{
    //qDebug() << "TerminalWindow.setFontSize" << _fontSize << (_resizeWindowIfNeeded ? "resizeWindowIfNeeded" : "");

    if (_fontSize < 5) // Let's not be crazy.
        return false;

    if (_fontSize > 100)
        return false;

    if (!terminalView_->setFontSize(static_cast<unsigned>(_fontSize * contentScale())))
        return false;

    config_.fontSize = _fontSize;

    if (!fullscreen())
    {
        // resize window
        auto const width = config_.terminalSize.columns * regularFont_.get().maxAdvance();
        auto const height = config_.terminalSize.rows * regularFont_.get().lineHeight();
        if (_resizeWindowIfNeeded)
            resize(width, height);
    }
    else
    {
        // resize terminalView (same pixels, but adjusted terminal rows/columns and margin)
        auto const windowSize = size();
        terminalView_->resize(windowSize.width(), windowSize.height());
    }

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
            setFontSize(config_.fontSize + 1, true);
            return false;
        },
        [&](actions::DecreaseFontSize) -> bool {
            setFontSize(config_.fontSize - 1, true);
            return false;
        },
        [&](actions::IncreaseOpacity) -> bool {
            ++config_.backgroundOpacity;
            terminalView_->setBackgroundOpacity(config_.backgroundOpacity);
            return true;
        },
        [&](actions::DecreaseOpacity) -> bool {
            --config_.backgroundOpacity;
            terminalView_->setBackgroundOpacity(config_.backgroundOpacity);
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
            return terminalView_->terminal().scrollUp(config_.historyScrollMultiplier);
        },
        [this](actions::ScrollDown) -> bool {
            return terminalView_->terminal().scrollDown(config_.historyScrollMultiplier);
        },
        [this](actions::ScrollPageUp) -> bool {
            return terminalView_->terminal().scrollUp(config_.terminalSize.rows / 2);
        },
        [this](actions::ScrollPageDown) -> bool {
            return terminalView_->terminal().scrollDown(config_.terminalSize.rows / 2);
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
            string const text = extractSelectionText();
            terminalView_->terminal().sendPaste(string_view{text});
            return false;
        },
        [this](actions::PasteClipboard) -> bool {
            terminalView_->terminal().sendPaste(getClipboardString());
            return false;
        },
        [this](actions::NewTerminal) -> bool {
            spawnNewTerminal();
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
        screenDirty_ = true;
        update();
    }
}

std::string TerminalWindow::getClipboardString()
{
    if (QClipboard* clipboard = QGuiApplication::clipboard(); clipboard != nullptr)
        return clipboard->text().toUtf8().toStdString();
    else
        return "";
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
            cout << "Copy: \"" << currentLine << '"' << endl;
            currentLine.clear();
        }
        if (_cell.character)
            currentLine += utf8::to_string(utf8::encode(_cell.character));
        lastColumn = _col;
    });
    text += currentLine;
    cout << "Copy: \"" << currentLine << '"' << endl;

    terminalView_->terminal().clearSelection();

    return text;
}

void TerminalWindow::spawnNewTerminal()
{
    // TODO: config option to either spawn new terminal via new process (default) or just as second window.
    QString const program = QString::fromUtf8(programPath_.c_str());
    QStringList const args; // TODO: Do we need to pass args?
    QProcess::startDetached(program, args);
}

float TerminalWindow::contentScale() const
{
    return screen()->devicePixelRatio();
}

void TerminalWindow::onScreenUpdate()
{
    screenDirty_ = true;

    if (config_.autoScrollOnUpdate && terminalView_->terminal().scrollOffset())
        terminalView_->terminal().scrollToBottom();

    bool updating = updating_.load();
    if (!updating && updating_.compare_exchange_strong(updating, true))
    {
        connect(this, SIGNAL(frameSwapped()), this, SLOT(onFrameSwapped()));
        QCoreApplication::postEvent(this, new QEvent(QEvent::UpdateRequest));
        STATS_ZERO(updatesSinceRendering);
    }

    STATS_INC(updatesSinceRendering);
    STATS_INC(updatesSinceLastSwap);
}

void TerminalWindow::onWindowTitleChanged()
{
    setTitle(QString::fromUtf8(terminalView_->terminal().windowTitle().c_str()));
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
        config_.terminalSize.columns = _width / regularFont_.get().maxAdvance();
        config_.terminalSize.rows = _height / regularFont_.get().lineHeight();
        resizePending = true;
    }
    else
    {
        if (_width == 0 && _height == 0)
            resize(_width, _height);
        else
        {
            if (!_width)
                _width = config_.terminalSize.columns;

            if (!_height)
                _height = config_.terminalSize.rows;

            config_.terminalSize.columns = _width;
            config_.terminalSize.rows = _height;
            resizePending = true;
        }
    }

    if (resizePending)
    {
        post([this]() {
            terminalView_->setTerminalSize(config_.terminalSize);
            auto const width = config_.terminalSize.columns * regularFont_.get().maxAdvance();
            auto const height = config_.terminalSize.rows * regularFont_.get().lineHeight();
            resize(width, height);
            screenDirty_ = true;
            update();
        });
    }
}

void TerminalWindow::onConfigReload(FileChangeWatcher::Event /*_event*/)
{
    post([this]() {
        if (reloadConfigValues())
        {
            screenDirty_ = true;
            update();
        }
    });
}

bool TerminalWindow::enableBackgroundBlur(bool /*_enable*/) // TODO
{
    return false;
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
