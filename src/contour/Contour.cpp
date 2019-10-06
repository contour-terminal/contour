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

#include <iostream>
#include <fstream>
#include <cstdio>
#include <cctype>
#include <utility>

#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

using namespace std;
using namespace std::placeholders;

Contour::Contour(Config const& _config) :
    config_{_config},
    logger_{
        _config.logFilePath
            ? GLLogger{_config.loggingMask, _config.logFilePath->string()}
            : GLLogger{_config.loggingMask, &cout}
    },
    fontManager_{},
    regularFont_{
        fontManager_.load(
            _config.fontFamily,
            static_cast<unsigned>(_config.fontSize * Window::primaryMonitorContentScale().second)
        )
    },
    window_{
        Window::Size{
            _config.terminalSize.columns * regularFont_.get().maxAdvance(),
            _config.terminalSize.rows * regularFont_.get().lineHeight()
        },
        "contour",
        bind(&Contour::onKey, this, _1, _2, _3, _4),
        bind(&Contour::onChar, this, _1),
        {}, // TODO: onMouseButton
        bind(&Contour::onMouseScroll, this, _1, _2),
        bind(&Contour::onResize, this),
        bind(&Contour::onContentScale, this, _1, _2)
    },
    terminalView_{
        config_.terminalSize,
        config_.maxHistoryLineCount,
        regularFont_.get(),
        config_.cursorShape,
        glm::vec3{0.9, 0.9, 0.9}, // TODO: make cursor color configurable (part of color profile?)
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
        if (!window_.enableBackgroundBlur())
            throw runtime_error{ "Could not enable background blur." };
    }

    terminalView_.setTabWidth(config_.tabWidth);

    glViewport(0, 0, window_.width(), window_.height());
}

Contour::~Contour()
{
}

int Contour::main()
{
    while (terminalView_.alive() && !glfwWindowShouldClose(window_))
    {
        if (terminalView_.shouldRender())
            screenDirty_ = true;

        bool reloadPending = configReloadPending_.load();
        if (reloadPending && atomic_compare_exchange_strong(&configReloadPending_, &reloadPending, false))
        {
            if (reloadConfigValues())
                screenDirty_ = true;
        }

        if (resizePending_)
        {
            terminalView_.setTerminalSize(config_.terminalSize);
            auto const width = config_.terminalSize.columns * regularFont_.get().maxAdvance();
            auto const height = config_.terminalSize.rows * regularFont_.get().lineHeight();
            window_.resize(width, height);
            screenDirty_ = true;
        }

        if (screenDirty_)
        {
            render();
            screenDirty_ = false;
        }

        if (titleDirty_)
        {
            glfwSetWindowTitle(window_, terminalView_.windowTitle().c_str());
            titleDirty_ = false;
        }

        glfwWaitEventsTimeout(0.5);
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

    terminalView_.render();

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

void Contour::executeAction(Action _action)
{
    visit(overloaded{
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
            auto const screenshot = terminalView_.screenshot();
            ofstream ofs{ "screenshot.vt", ios::trunc | ios::binary };
            ofs << screenshot;
        },
        [&](actions::SendChars const& chars) {
            for (auto const ch : chars.chars)
                terminalView_.send(terminal::CharInputEvent{static_cast<char32_t>(ch), terminal::Modifier::None});
        },
        [this](actions::ScrollUp) {
            screenDirty_ = terminalView_.scrollUp(config_.historyScrollMultiplier) || screenDirty_;
        },
        [this](actions::ScrollDown) {
            screenDirty_ = terminalView_.scrollDown(config_.historyScrollMultiplier) || screenDirty_;
        },
        [this](actions::ScrollPageUp) {
            screenDirty_ = terminalView_.scrollUp(config_.terminalSize.rows / 2) || screenDirty_;
        },
        [this](actions::ScrollPageDown) {
            screenDirty_ = terminalView_.scrollDown(config_.terminalSize.rows / 2) || screenDirty_;
        },
        [this](actions::ScrollToTop) {
            screenDirty_ = terminalView_.scrollToTop() || screenDirty_;
        },
        [this](actions::ScrollToBottom) {
            screenDirty_ = terminalView_.scrollToBottom() || screenDirty_;
        }
    }, _action);
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
        case GLFW_KEY_LEFT_BRACKET: return CharInputEvent{'[', _mods};
        case GLFW_KEY_BACKSLASH: return CharInputEvent{'\\', _mods};
        case GLFW_KEY_RIGHT_BRACKET: return CharInputEvent{']', _mods};
        case GLFW_KEY_GRAVE_ACCENT: return CharInputEvent{'`', _mods};
    }

    return nullopt;
}

void Contour::onKey(int _key, int _scanCode, int _action, int _mods)
{
    // TODO: investigate how to handle when one of these state vars are true, and the window loses focus.
    // They should be recaptured after focus gain again.
    modifier_ = makeModifier(_mods);

    keyHandled_ = false;
    if (_action == GLFW_PRESS || _action == GLFW_REPEAT)
    {
        if (auto const inputEvent = makeInputEvent(_key, modifier_); inputEvent.has_value())
        {
            if (auto const mapping = config_.inputMapping.find(inputEvent.value()); mapping != end(config_.inputMapping))
            {
                executeAction(mapping->second);
                keyHandled_ = true;
            }
            else if (!holds_alternative<terminal::CharInputEvent>(inputEvent.value()) || modifier_ != terminal::Modifier::Shift)
            {
                terminalView_.send(inputEvent.value());
                keyHandled_ = true;
            }
        }
        // else if (modifier_ && modifier_ != terminal::Modifier::Shift) // Debug print unhandled characters
        // {
        //     char const* cstr = glfwGetKeyName(_key, _scanCode);
        //     cerr << fmt::format(
        //         "key:{}, scanCode:{}, name:{} ({})",
        //         _key, _scanCode, cstr ? cstr : "(null)", terminal::to_string(modifier_)
        //     ) << endl;
        // }
    }
}

void Contour::onChar(char32_t _char)
{
    if (!keyHandled_)
    {
        auto const inputEvent = terminal::CharInputEvent{_char, modifier_};

        if (auto const mapping = config_.inputMapping.find(inputEvent); mapping != end(config_.inputMapping))
            executeAction(mapping->second);
        else
            terminalView_.send(terminal::CharInputEvent{_char, terminal::Modifier{}});

        keyHandled_ = false;
    }
}

void Contour::onMouseScroll(double _xOffset, double _yOffset)
{
    auto const button = _yOffset > 0.0 ? terminal::MouseButton::WheelUp : terminal::MouseButton::WheelDown;
    auto const inputEvent = terminal::MousePressEvent{button, modifier_};

    if (auto const mapping = config_.inputMapping.find(inputEvent); mapping != end(config_.inputMapping))
        executeAction(mapping->second);
}

bool Contour::setFontSize(unsigned _fontSize, bool _resizeWindowIfNeeded)
{
    if (!terminalView_.setFontSize(static_cast<unsigned>(_fontSize * Window::primaryMonitorContentScale().second)))
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
    if (config_.autoScrollOnUpdate && terminalView_.scrollOffset())
        terminalView_.scrollToBottom();

    glfwPostEmptyEvent();
}

void Contour::onWindowTitleChanged()
{
    titleDirty_ = true;
    glfwPostEmptyEvent();
}

void Contour::doResize(unsigned _width, unsigned _height, bool _inPixels)
{
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
        resizePending_ = true;
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
            resizePending_ = true;
        }
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
            ? GLLogger{newConfig.loggingMask, newConfig.logFilePath->string()}
            : GLLogger{newConfig.loggingMask, &cout};

    bool windowResizeRequired = false;

    terminalView_.setTabWidth(newConfig.tabWidth);
    if (newConfig.fontFamily != config_.fontFamily)
    {
        regularFont_ = fontManager_.load(
            newConfig.fontFamily,
            static_cast<unsigned>(newConfig.fontSize * Window::primaryMonitorContentScale().second)
        );
        terminalView_.setFont(regularFont_.get());
        windowResizeRequired = true;
    }
    else if (newConfig.fontSize != config_.fontSize)
        windowResizeRequired |= setFontSize(newConfig.fontSize, false);

    if (newConfig.terminalSize != config_.terminalSize && !window_.fullscreen())
        windowResizeRequired |= terminalView_.setTerminalSize(config_.terminalSize);

    if (windowResizeRequired && !window_.fullscreen())
    {
        auto const width = newConfig.terminalSize.columns * regularFont_.get().maxAdvance();
        auto const height = newConfig.terminalSize.rows * regularFont_.get().lineHeight();
        window_.resize(width, height);
    }

    terminalView_.setMaxHistoryLineCount(newConfig.maxHistoryLineCount);

    // TODO: cursor shape
    // TODO: cursor blinking
    // TODO: tab width
    // TODO: background blur

    config_ = move(newConfig);

    return true;
}
