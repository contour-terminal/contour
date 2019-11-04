/**
 * This file is part of the "libterminal" project
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
#include "Contour.h"
#include <terminal/Color.h>
#include <terminal/InputGenerator.h>
#include <ground/overloaded.h>

#include <cctype>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <utility>

#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

using namespace std;
using namespace std::placeholders;

string Contour::launcher() const
{
	// TODO: could be configurable with the below as default
	#if defined(__APPLE__)
		return "open"s;
	#elif defined(_WIN32)
		return "explorer";
	#elif defined(__unix__)
		return "xdg-open";
	#else
		#error "Unknown platform"
	#endif
}

Contour::Contour(string _programPath, Config const& _config) :
    now_{chrono::steady_clock::now()},
	programPath_{move(_programPath)},
    config_{_config},
    logger_{
        _config.logFilePath
            ? terminal::view::GLLogger{_config.loggingMask, _config.logFilePath->string()}
            : terminal::view::GLLogger{_config.loggingMask, &cout}
    },
    fontManager_{},
    regularFont_{
        fontManager_.load(
            _config.fontFamily,
            static_cast<unsigned>(_config.fontSize * UIWindow::primaryMonitorContentScale().second)
        )
    },
    window_{
        UIWindow::Size{
            _config.terminalSize.columns * regularFont_.get().maxAdvance(),
            _config.terminalSize.rows * regularFont_.get().lineHeight()
        },
        "contour",
        bind(&Contour::onKey, this, _1, _2, _3, _4),
        bind(&Contour::onChar, this, _1),
        bind(&Contour::onMouseScroll, this, _1, _2),
        bind(&Contour::onMouseButton, this, _1, _2, _3),
        bind(&Contour::onMousePosition, this, _1, _2),
        bind(&Contour::onResize, this),
        bind(&Contour::onContentScale, this, _1, _2)
    },
    terminalView_{
        now_,
        config_.terminalSize,
        config_.maxHistoryLineCount,
        config_.wordDelimiters,
        regularFont_.get(),
        config_.cursorShape,
		config_.cursorDisplay,
        config_.colorProfile,
        config_.backgroundOpacity,
        config_.shell,
        glm::ortho(0.0f, static_cast<GLfloat>(window_.width()), 0.0f, static_cast<GLfloat>(window_.height())),
        bind(&Contour::onScreenUpdate, this),
        bind(&Contour::onWindowTitleChanged, this),
        bind(&Contour::doResize, this, _1, _2, _3),
        logger_
    },
    configFileChangeWatcher_{
        _config.backingFilePath,
        bind(&Contour::onConfigReload, this, _1)
    }
{
    if (!loggingSink_.good())
        throw runtime_error{ "Failed to open log file." };

    if (!regularFont_.get().isFixedWidth())
        throw runtime_error{ "Regular font is not a fixed-width font." };

    if (config_.backgroundBlur)
    {
        if (!window_.enableBackgroundBlur(true))
            throw runtime_error{ "Could not enable background blur." };
    }

    terminalView_.terminal().setTabWidth(config_.tabWidth);

    glViewport(0, 0, window_.width(), window_.height());
}

Contour::~Contour()
{
}

void Contour::post(function<void()> _fn)
{
	auto lg = lock_guard{queuedCallsLock_};
	queuedCalls_.emplace_back(move(_fn));
    glfwPostEmptyEvent();
}

int Contour::main()
{
    try {
    while (terminalView_.alive() && !glfwWindowShouldClose(window_))
    {
		now_ = chrono::steady_clock::now();

		{
			auto lg = lock_guard{queuedCallsLock_};
			auto calls = decltype(queuedCalls_){};
			swap(queuedCalls_, calls);
			for_each(begin(calls), end(calls), [](auto& _call) { _call(); });
		}

        if (terminalView_.terminal().shouldRender(now_))
            screenDirty_ = true;

		// TODO: move into channeled function (queued calls)
        bool reloadPending = configReloadPending_.load();
        if (reloadPending && atomic_compare_exchange_strong(&configReloadPending_, &reloadPending, false))
        {
            if (reloadConfigValues())
                screenDirty_ = true;
        }

        if (screenDirty_)
        {
            render();
            screenDirty_ = false;
        }

		// The wait timeout is determined by the interval of a blinking cursor.
		glfwWaitEventsTimeout(chrono::duration<double>(terminalView_.terminal().cursorBlinkInterval()).count());
    }
    }
    catch (exception const& e)
    {
        cerr << fmt::format("Unhandled exception caught ({}). {}", typeid(e).name(), e.what()) << endl;
    }

    return EXIT_SUCCESS;
}

inline glm::vec4 makeColor(terminal::RGBColor const& _color, terminal::Opacity _opacity)
{
    return glm::vec4{
        _color.red / 255.0f,
        _color.green / 255.0f,
        _color.blue / 255.0f,
        static_cast<float>(_opacity) / 255.0f};
}

void Contour::render()
{
    glm::vec4 const& bg = makeColor(config_.colorProfile.defaultBackground, config_.backgroundOpacity);
    glClearColor(bg.r, bg.g, bg.b, bg.a);
    glClear(GL_COLOR_BUFFER_BIT);

    terminalView_.render(now_);
    //terminal::view::render(terminalView_, now_);

    window_.swapBuffers();
}

void Contour::onContentScale(float _xs, float _ys)
{
    cout << fmt::format("Updated content scale to: {:.2f} by {:.2f}\n", _xs, _ys);
    // TODO: scale fontSize by factor _ys.
}

void Contour::onResize()
{
    terminalView_.resize(window_.width(), window_.height());
    terminalView_.setProjection(
        glm::ortho(
            0.0f, static_cast<GLfloat>(window_.width()),
            0.0f, static_cast<GLfloat>(window_.height())
        )
    );
    glViewport(0, 0, window_.width(), window_.height());

	now_ = chrono::steady_clock::now();
    render();
}

optional<terminal::Key> glfwFunctionKeyToTerminalKey(int _key)
{
    using terminal::Key;

    static auto constexpr mapping = array{
        pair{GLFW_KEY_ESCAPE, Key::Escape},
        pair{GLFW_KEY_ENTER, Key::Enter},
        pair{GLFW_KEY_TAB, Key::Tab},
        pair{GLFW_KEY_BACKSPACE, Key::Backspace},
        pair{GLFW_KEY_INSERT, Key::Insert},
        pair{GLFW_KEY_DELETE, Key::Delete},
        pair{GLFW_KEY_RIGHT, Key::RightArrow},
        pair{GLFW_KEY_LEFT, Key::LeftArrow},
        pair{GLFW_KEY_DOWN, Key::DownArrow},
        pair{GLFW_KEY_UP, Key::UpArrow},
        pair{GLFW_KEY_PAGE_DOWN, Key::PageDown},
        pair{GLFW_KEY_PAGE_UP, Key::PageUp},
        pair{GLFW_KEY_HOME, Key::Home},
        pair{GLFW_KEY_END, Key::End},
        // TODO: some of those below...
        //#define GLFW_KEY_CAPS_LOCK          280
        //#define GLFW_KEY_SCROLL_LOCK        281
        //#define GLFW_KEY_NUM_LOCK           282
        //#define GLFW_KEY_PRINT_SCREEN       283
        //#define GLFW_KEY_PAUSE              284
        pair{GLFW_KEY_F1, Key::F1},
        pair{GLFW_KEY_F2, Key::F2},
        pair{GLFW_KEY_F3, Key::F3},
        pair{GLFW_KEY_F4, Key::F4},
        pair{GLFW_KEY_F5, Key::F5},
        pair{GLFW_KEY_F6, Key::F6},
        pair{GLFW_KEY_F7, Key::F7},
        pair{GLFW_KEY_F8, Key::F8},
        pair{GLFW_KEY_F9, Key::F9},
        pair{GLFW_KEY_F10, Key::F10},
        pair{GLFW_KEY_F11, Key::F11},
        pair{GLFW_KEY_F12, Key::F12},
        // todo: F13..F25
        pair{GLFW_KEY_KP_0, Key::Numpad_0},
        pair{GLFW_KEY_KP_1, Key::Numpad_1},
        pair{GLFW_KEY_KP_2, Key::Numpad_2},
        pair{GLFW_KEY_KP_3, Key::Numpad_3},
        pair{GLFW_KEY_KP_4, Key::Numpad_4},
        pair{GLFW_KEY_KP_5, Key::Numpad_5},
        pair{GLFW_KEY_KP_6, Key::Numpad_6},
        pair{GLFW_KEY_KP_7, Key::Numpad_7},
        pair{GLFW_KEY_KP_8, Key::Numpad_8},
        pair{GLFW_KEY_KP_9, Key::Numpad_9},
        pair{GLFW_KEY_KP_DECIMAL, Key::Numpad_Decimal},
        pair{GLFW_KEY_KP_DIVIDE, Key::Numpad_Divide},
        pair{GLFW_KEY_KP_MULTIPLY, Key::Numpad_Multiply},
        pair{GLFW_KEY_KP_SUBTRACT, Key::Numpad_Subtract},
        pair{GLFW_KEY_KP_ADD, Key::Numpad_Add},
        pair{GLFW_KEY_KP_ENTER, Key::Numpad_Enter},
        pair{GLFW_KEY_KP_EQUAL, Key::Numpad_Equal},
    };

    if (auto i = find_if(begin(mapping), end(mapping), [_key](auto const& x) { return x.first == _key; }); i != end(mapping))
        return { i->second };

    return nullopt;
}

constexpr terminal::Modifier makeModifier(int _mods)
{
    using terminal::Modifier;

    Modifier mods{};

    if (_mods & GLFW_MOD_ALT)
        mods |= Modifier::Alt;
    if (_mods & GLFW_MOD_SHIFT)
        mods |= Modifier::Shift;
    if (_mods & GLFW_MOD_CONTROL)
        mods |= Modifier::Control;
    if (_mods & GLFW_MOD_SUPER)
        mods |= Modifier::Meta;

    return mods;
}

void Contour::executeInput(terminal::InputEvent const& _inputEvent)
{
    bool handled = false;
    for (InputMapping const& mapping : config_.inputMappings)
    {
        if (_inputEvent == mapping.input)
        {
            executeAction(mapping.action);
            handled = true;
        }
    }

    visit(overloaded{
        [&](terminal::KeyInputEvent const&) {
            if (!handled)
                if (modifier_ != terminal::Modifier::Shift)
                    terminalView_.terminal().send(_inputEvent, now_);
            keyHandled_ = true;
        },
        [&](terminal::CharInputEvent const&) {
            if (!handled && modifier_ != terminal::Modifier::Shift)
            {
                terminalView_.terminal().send(_inputEvent, now_);
                keyHandled_ = true;
            }
        },
        [&](terminal::MousePressEvent const&) {
            terminalView_.terminal().send(_inputEvent, now_);
        },
        [&](terminal::MouseMoveEvent const&) {
            terminalView_.terminal().send(_inputEvent, now_);
        },
        [&](terminal::MouseReleaseEvent const&) {
            terminalView_.terminal().send(_inputEvent, now_);
        }
    }, _inputEvent);
}

void Contour::executeAction(Action const& _action)
{
    visit(overloaded{
        [this](actions::WriteScreen const& _write) {
            terminalView_.terminal().writeToScreen(_write.chars);
        },
        [&](actions::ToggleFullScreen) {
            window_.toggleFullScreen();
        },
        [&](actions::IncreaseFontSize) {
            setFontSize(config_.fontSize + 1, true);
        },
        [&](actions::DecreaseFontSize) {
            setFontSize(config_.fontSize - 1, true);
        },
        [&](actions::IncreaseOpacity) {
            ++config_.backgroundOpacity;
            terminalView_.setBackgroundOpacity(config_.backgroundOpacity);
            screenDirty_ = true;
            glfwPostEmptyEvent();
        },
        [&](actions::DecreaseOpacity) {
            --config_.backgroundOpacity;
            terminalView_.setBackgroundOpacity(config_.backgroundOpacity);
            screenDirty_ = true;
            glfwPostEmptyEvent();
        },
        [&](actions::ScreenshotVT) {
            auto const screenshot = terminalView_.terminal().screenshot();
            ofstream ofs{ "screenshot.vt", ios::trunc | ios::binary };
            ofs << screenshot;
        },
        [this](actions::SendChars const& chars) {
            for (auto const ch : chars.chars)
                terminalView_.terminal().send(terminal::CharInputEvent{static_cast<char32_t>(ch), terminal::Modifier::None}, now_);
        },
        [this](actions::ScrollOneUp) {
            screenDirty_ = terminalView_.terminal().scrollUp(1) || screenDirty_;
        },
        [this](actions::ScrollOneDown) {
            screenDirty_ = terminalView_.terminal().scrollDown(1) || screenDirty_;
        },
        [this](actions::ScrollUp) {
            screenDirty_ = terminalView_.terminal().scrollUp(config_.historyScrollMultiplier) || screenDirty_;
        },
        [this](actions::ScrollDown) {
            screenDirty_ = terminalView_.terminal().scrollDown(config_.historyScrollMultiplier) || screenDirty_;
        },
        [this](actions::ScrollPageUp) {
            screenDirty_ = terminalView_.terminal().scrollUp(config_.terminalSize.rows / 2) || screenDirty_;
        },
        [this](actions::ScrollPageDown) {
            screenDirty_ = terminalView_.terminal().scrollDown(config_.terminalSize.rows / 2) || screenDirty_;
        },
        [this](actions::ScrollToTop) {
            screenDirty_ = terminalView_.terminal().scrollToTop() || screenDirty_;
        },
        [this](actions::ScrollToBottom) {
            screenDirty_ = terminalView_.terminal().scrollToBottom() || screenDirty_;
        },
        [this](actions::CopySelection) {
            string const text = extractSelectionText();
            glfwSetClipboardString(window_, text.c_str());
        },
        [this](actions::PasteSelection) {
            string const text = extractSelectionText();
            terminalView_.terminal().sendPaste(string_view{text});
        },
        [this](actions::PasteClipboard) {
            terminalView_.terminal().sendPaste(glfwGetClipboardString(window_));
        },
		[this](actions::NewTerminal) {
			spawnNewTerminal();
		},
		[this](actions::OpenConfiguration) {
			auto const fileName = FileSystem::absolute(config_.backingFilePath);
			auto const cmd = fmt::format("{} \"{}\"", launcher(), fileName.string());
			if (system(cmd.c_str()) != EXIT_SUCCESS)
				cerr << "Couldn't open configuration.\n";
		}
    }, _action);
}

string Contour::extractSelectionText()
{
    using namespace terminal;
    cursor_pos_t lastColumn = 0;
    string text;
    string currentLine;

    terminalView_.terminal().renderSelection([&](cursor_pos_t _row, cursor_pos_t _col, Screen::Cell const& _cell) {
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

    terminalView_.terminal().clearSelection();

    return text;
}

optional<terminal::InputEvent> makeInputEvent(int _key, terminal::Modifier _mods)
{
    using terminal::InputEvent;
    using terminal::CharInputEvent;
    using terminal::KeyInputEvent;

    // function keys
    if (auto const key = glfwFunctionKeyToTerminalKey(_key))
        return InputEvent{KeyInputEvent{key.value(), _mods}};

    // printable keys
    switch (_key)
    {
        case GLFW_KEY_SPACE: return CharInputEvent{' ', _mods};
        case GLFW_KEY_APOSTROPHE: return CharInputEvent{'\'', _mods};
        case GLFW_KEY_COMMA: return CharInputEvent{',', _mods};
        case GLFW_KEY_MINUS: return CharInputEvent{'-', _mods};
        case GLFW_KEY_PERIOD: return CharInputEvent{'.', _mods};
        case GLFW_KEY_SLASH: return CharInputEvent{'/', _mods};
        case GLFW_KEY_0: return CharInputEvent{'0', _mods};
        case GLFW_KEY_1: return CharInputEvent{'1', _mods};
        case GLFW_KEY_2: return CharInputEvent{'2', _mods};
        case GLFW_KEY_3: return CharInputEvent{'3', _mods};
        case GLFW_KEY_4: return CharInputEvent{'4', _mods};
        case GLFW_KEY_5: return CharInputEvent{'5', _mods};
        case GLFW_KEY_6: return CharInputEvent{'6', _mods};
        case GLFW_KEY_7: return CharInputEvent{'7', _mods};
        case GLFW_KEY_8: return CharInputEvent{'8', _mods};
        case GLFW_KEY_9: return CharInputEvent{'9', _mods};
        case GLFW_KEY_SEMICOLON: return CharInputEvent{';', _mods};
        case GLFW_KEY_EQUAL: return CharInputEvent{'=', _mods};
        case GLFW_KEY_LEFT_BRACKET: return CharInputEvent{'[', _mods};
        case GLFW_KEY_BACKSLASH: return CharInputEvent{'\\', _mods};
        case GLFW_KEY_RIGHT_BRACKET: return CharInputEvent{']', _mods};
        case GLFW_KEY_GRAVE_ACCENT: return CharInputEvent{'`', _mods};
    }

    if (_mods)
    {
        // Do these mappings only iff modifiers are present, because
        // we do not want to track CAPSLOCK, hence,
        // the character callback (onChar) will kick in.
        switch (_key)
        {
            case GLFW_KEY_A: return CharInputEvent{'a', _mods};
            case GLFW_KEY_B: return CharInputEvent{'b', _mods};
            case GLFW_KEY_C: return CharInputEvent{'c', _mods};
            case GLFW_KEY_D: return CharInputEvent{'d', _mods};
            case GLFW_KEY_E: return CharInputEvent{'e', _mods};
            case GLFW_KEY_F: return CharInputEvent{'f', _mods};
            case GLFW_KEY_G: return CharInputEvent{'g', _mods};
            case GLFW_KEY_H: return CharInputEvent{'h', _mods};
            case GLFW_KEY_I: return CharInputEvent{'i', _mods};
            case GLFW_KEY_J: return CharInputEvent{'j', _mods};
            case GLFW_KEY_K: return CharInputEvent{'k', _mods};
            case GLFW_KEY_L: return CharInputEvent{'l', _mods};
            case GLFW_KEY_M: return CharInputEvent{'m', _mods};
            case GLFW_KEY_N: return CharInputEvent{'n', _mods};
            case GLFW_KEY_O: return CharInputEvent{'o', _mods};
            case GLFW_KEY_P: return CharInputEvent{'p', _mods};
            case GLFW_KEY_Q: return CharInputEvent{'q', _mods};
            case GLFW_KEY_R: return CharInputEvent{'r', _mods};
            case GLFW_KEY_S: return CharInputEvent{'s', _mods};
            case GLFW_KEY_T: return CharInputEvent{'t', _mods};
            case GLFW_KEY_U: return CharInputEvent{'u', _mods};
            case GLFW_KEY_V: return CharInputEvent{'v', _mods};
            case GLFW_KEY_W: return CharInputEvent{'w', _mods};
            case GLFW_KEY_X: return CharInputEvent{'x', _mods};
            case GLFW_KEY_Y: return CharInputEvent{'y', _mods};
            case GLFW_KEY_Z: return CharInputEvent{'z', _mods};
        }
    }

    return nullopt;
}

static void updateModifier(terminal::Modifier& _modifier, int _glfwKey, bool _enable)
{
    static auto constexpr mappings = array{
        pair{GLFW_KEY_LEFT_SHIFT, terminal::Modifier::Shift},
        pair{GLFW_KEY_LEFT_CONTROL, terminal::Modifier::Control},
        pair{GLFW_KEY_LEFT_ALT, terminal::Modifier::Alt},
        pair{GLFW_KEY_LEFT_SUPER, terminal::Modifier::Meta},
        pair{GLFW_KEY_RIGHT_SHIFT, terminal::Modifier::Shift},
        pair{GLFW_KEY_RIGHT_CONTROL, terminal::Modifier::Control},
        pair{GLFW_KEY_RIGHT_ALT, terminal::Modifier::Alt},
        pair{GLFW_KEY_RIGHT_SUPER, terminal::Modifier::Meta},
    };

    for (auto const& mapping : mappings)
    {
        if (mapping.first == _glfwKey)
        {
            if (_enable)
                _modifier.enable(mapping.second);
            else
                _modifier.disable(mapping.second);
            break;
        }
    }
}

void Contour::onKey(int _key, int _scanCode, int _action, int _mods)
{
    //modifier_ = makeModifier(_mods);

    keyHandled_ = false;
    if (_action == GLFW_PRESS || _action == GLFW_REPEAT)
    {
        updateModifier(modifier_, _key, true);

        if (auto const inputEvent = makeInputEvent(_key, modifier_); inputEvent.has_value())
        {
            executeInput(inputEvent.value());
        }
        // else if (modifier_ && modifier_ != terminal::Modifier::Shift) // Debug print unhandled characters
        // {
        //     char const* cstr = glfwGetKeyName(_key, _scanCode);
        //     logger_(terminal::TraceInputEvent{fmt::format(
        //         "unhandled key: {}, scanCode: {}, name: {} ({})",
        //         _key, _scanCode, cstr ? cstr : "(null)", terminal::to_string(modifier_)
        //     )});
        // }
    }
    else if (_action == GLFW_RELEASE)
    {
        updateModifier(modifier_, _key, false);
    }
}

void Contour::onChar(char32_t _char)
{
    if (!keyHandled_)
    {
		now_ = chrono::steady_clock::now();
        //executeInput(terminal::CharInputEvent{_char, modifier_});
        terminalView_.terminal().send(terminal::CharInputEvent{_char, modifier_}, now_);
        keyHandled_ = true;
    }
}

void Contour::onMouseScroll(double _xOffset, double _yOffset)
{
    auto const button = _yOffset > 0.0 ? terminal::MouseButton::WheelUp : terminal::MouseButton::WheelDown;
    executeInput(terminal::MousePressEvent{button, modifier_});
}

void Contour::onMouseButton(int _button, int _action, int _mods)
{
	now_ = chrono::steady_clock::now();

    auto const static makeMouseButton = [](int _button) -> terminal::MouseButton {
        switch (_button)
        {
            case GLFW_MOUSE_BUTTON_RIGHT:
                return terminal::MouseButton::Right;
            case GLFW_MOUSE_BUTTON_MIDDLE:
                return terminal::MouseButton::Middle;
            case GLFW_MOUSE_BUTTON_LEFT:
            default: // d'oh
                return terminal::MouseButton::Left;
        }
    };

    auto const mouseButton = makeMouseButton(_button);

    if (_action == GLFW_PRESS)
    {
        executeInput(terminal::MousePressEvent{mouseButton, modifier_});
    }
    else if (_action == GLFW_RELEASE)
    {
        terminalView_.terminal().send(terminal::MouseReleaseEvent{mouseButton}, now_);
    }
}

void Contour::onMousePosition(double _x, double _y)
{
	now_ = chrono::steady_clock::now();

    terminalView_.terminal().send(
        terminal::MouseMoveEvent{
            static_cast<unsigned>(1 + max(static_cast<int>(_y), 0) / terminalView_.cellHeight()),
            static_cast<unsigned>(1 + max(static_cast<int>(_x), 0) / terminalView_.cellWidth())
        },
        now_
    );
}

bool Contour::setFontSize(unsigned _fontSize, bool _resizeWindowIfNeeded)
{
    if (!terminalView_.setFontSize(static_cast<unsigned>(_fontSize * window_.contentScale().second)))
        return false;

    if (_fontSize < 5) // Let's not be crazy.
        return false;

    if (_fontSize > 100)
        return false;

    config_.fontSize = _fontSize;
    if (!window_.fullscreen())
    {
        // resize window
        auto const width = config_.terminalSize.columns * regularFont_.get().maxAdvance();
        auto const height = config_.terminalSize.rows * regularFont_.get().lineHeight();
        if (_resizeWindowIfNeeded)
            window_.resize(width, height);
    }
    else
    {
        // resize terminalView (same pixels, but adjusted terminal rows/columns and margin)
        terminalView_.resize(window_.size().width, window_.size().height);
    }
    return true;
}

void Contour::onScreenUpdate()
{
    if (config_.autoScrollOnUpdate && terminalView_.terminal().scrollOffset())
        terminalView_.terminal().scrollToBottom();

    glfwPostEmptyEvent();
}

void Contour::onWindowTitleChanged()
{
	post([this]() { glfwSetWindowTitle(window_, terminalView_.terminal().windowTitle().c_str()); });
}

void Contour::doResize(unsigned _width, unsigned _height, bool _inPixels)
{
	bool resizePending = false;
    if (window_.fullscreen())
    {
        cerr << "Application request to resize window in full screen mode denied." << endl;
    }
    else if (_inPixels)
    {
        if (_width == 0 && _height == 0)
        {
            auto const screenSize = window_.screenSize();
            _width = screenSize.width;
            _height = screenSize.height;
        }
        else
        {
            if (!_width)
                _width = window_.size().width;

            if (!_height)
                _height = window_.size().height;
        }
        config_.terminalSize.columns = _width / regularFont_.get().maxAdvance();
        config_.terminalSize.rows = _height / regularFont_.get().lineHeight();
        resizePending = true;
    }
    else
    {
        if (_width == 0 && _height == 0)
            window_.resize(_width, _height);
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
            terminalView_.setTerminalSize(config_.terminalSize);
            auto const width = config_.terminalSize.columns * regularFont_.get().maxAdvance();
            auto const height = config_.terminalSize.rows * regularFont_.get().lineHeight();
            window_.resize(width, height);
            screenDirty_ = true;
		});
	}
}

void Contour::onConfigReload(ground::FileChangeWatcher::Event _event)
{
    configReloadPending_.store(true);
    glfwPostEmptyEvent();
}

bool Contour::reloadConfigValues()
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
            ? terminal::view::GLLogger{newConfig.loggingMask, newConfig.logFilePath->string()}
            : terminal::view::GLLogger{newConfig.loggingMask, &cout};

    bool windowResizeRequired = false;

    terminalView_.terminal().setTabWidth(newConfig.tabWidth);
    if (newConfig.fontFamily != config_.fontFamily)
    {
        regularFont_ = fontManager_.load(
            newConfig.fontFamily,
            static_cast<unsigned>(newConfig.fontSize * window_.contentScale().second)
        );
        terminalView_.setFont(regularFont_.get());
        windowResizeRequired = true;
    }
    else if (newConfig.fontSize != config_.fontSize)
    {
        windowResizeRequired |= setFontSize(newConfig.fontSize, false);
    }

    if (newConfig.terminalSize != config_.terminalSize && !window_.fullscreen())
        windowResizeRequired |= terminalView_.setTerminalSize(config_.terminalSize);

    terminalView_.terminal().setWordDelimiters(newConfig.wordDelimiters);

    if (windowResizeRequired && !window_.fullscreen())
    {
        auto const width = newConfig.terminalSize.columns * regularFont_.get().maxAdvance();
        auto const height = newConfig.terminalSize.rows * regularFont_.get().lineHeight();
        window_.resize(width, height);
    }

    terminalView_.terminal().setMaxHistoryLineCount(newConfig.maxHistoryLineCount);

    if (newConfig.colorProfile.cursor != config_.colorProfile.cursor)
        terminalView_.setCursorColor(newConfig.colorProfile.cursor);

    if (newConfig.cursorShape != config_.cursorShape)
        terminalView_.setCursorShape(newConfig.cursorShape);

    if (newConfig.cursorDisplay != config_.cursorDisplay)
        terminalView_.terminal().setCursorDisplay(newConfig.cursorDisplay);

    if (newConfig.backgroundBlur != config_.backgroundBlur)
        window_.enableBackgroundBlur(newConfig.backgroundBlur);

    // TODO: tab width

    config_ = move(newConfig);

    return true;
}

void Contour::spawnNewTerminal()
{
	terminal::Process{
		programPath_,
		vector<string>{
			programPath_,
			"-c"s,
			config_.backingFilePath.string()
		},
		{/*env*/},
		terminalView_.process().workingDirectory(),
		true
	};
}
