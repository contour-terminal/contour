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

#include <iostream>
#include <fstream>
#include <cstdio>
#include <cctype>

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
            _config.terminalSize.columns * regularFont_.maxAdvance(),
            _config.terminalSize.rows * regularFont_.lineHeight()
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
        window_.width(),
        window_.height(),
        regularFont_,
        config_.cursorShape,
        glm::vec4{0.9, 0.9, 0.9, 1.0}, // TODO: make cursor color configurable (part of color profile?)
        config_.colorProfile,
        config_.backgroundOpacity,
        config_.shell,
        glm::ortho(0.0f, static_cast<GLfloat>(window_.width()), 0.0f, static_cast<GLfloat>(window_.height())),
        bind(&Contour::onScreenUpdate, this),
        logger_
    },
    configFileChangeWatcher_{
        _config.backingFilePath,
        bind(&Contour::onConfigReload, this, _1)
    }
{
    if (!loggingSink_.good())
        throw runtime_error{ "Failed to open log file." };

    if (!regularFont_.isFixedWidth())
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
        bool reloadPending = configReloadPending_.load();
        if (terminalView_.shouldRender())
            screenDirty_ = true;
        if (reloadPending && atomic_compare_exchange_strong(&configReloadPending_, &reloadPending, false))
        {
            if (loadConfigValues())
                screenDirty_ = true;
        }

        if (screenDirty_)
            render();
        screenDirty_ = false;
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

    glfwSwapBuffers(window_);
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

optional<terminal::Key> glfwKeyToTerminalKey(int _key)
{
    using terminal::Key;

    static auto constexpr mapping = array{
        make_pair(GLFW_KEY_ESCAPE, Key::Escape),
        make_pair(GLFW_KEY_ENTER, Key::Enter),
        make_pair(GLFW_KEY_TAB, Key::Tab),
        make_pair(GLFW_KEY_BACKSPACE, Key::Backspace),
        make_pair(GLFW_KEY_INSERT, Key::Insert),
        make_pair(GLFW_KEY_DELETE, Key::Delete),
        make_pair(GLFW_KEY_RIGHT, Key::RightArrow),
        make_pair(GLFW_KEY_LEFT, Key::LeftArrow),
        make_pair(GLFW_KEY_DOWN, Key::DownArrow),
        make_pair(GLFW_KEY_UP, Key::UpArrow),
        make_pair(GLFW_KEY_PAGE_DOWN, Key::PageDown),
        make_pair(GLFW_KEY_PAGE_UP, Key::PageUp),
        make_pair(GLFW_KEY_HOME, Key::Home),
        make_pair(GLFW_KEY_END, Key::End),
        // TODO: some of those below...
        //#define GLFW_KEY_CAPS_LOCK          280
        //#define GLFW_KEY_SCROLL_LOCK        281
        //#define GLFW_KEY_NUM_LOCK           282
        //#define GLFW_KEY_PRINT_SCREEN       283
        //#define GLFW_KEY_PAUSE              284
        make_pair(GLFW_KEY_F1, Key::F1),
        make_pair(GLFW_KEY_F2, Key::F2),
        make_pair(GLFW_KEY_F3, Key::F3),
        make_pair(GLFW_KEY_F4, Key::F4),
        make_pair(GLFW_KEY_F5, Key::F5),
        make_pair(GLFW_KEY_F6, Key::F6),
        make_pair(GLFW_KEY_F7, Key::F7),
        make_pair(GLFW_KEY_F8, Key::F8),
        make_pair(GLFW_KEY_F9, Key::F9),
        make_pair(GLFW_KEY_F10, Key::F10),
        make_pair(GLFW_KEY_F11, Key::F11),
        make_pair(GLFW_KEY_F12, Key::F12),
        // todo: F13..F25
        make_pair(GLFW_KEY_KP_0, Key::Numpad_0),
        make_pair(GLFW_KEY_KP_1, Key::Numpad_1),
        make_pair(GLFW_KEY_KP_2, Key::Numpad_2),
        make_pair(GLFW_KEY_KP_3, Key::Numpad_3),
        make_pair(GLFW_KEY_KP_4, Key::Numpad_4),
        make_pair(GLFW_KEY_KP_5, Key::Numpad_5),
        make_pair(GLFW_KEY_KP_6, Key::Numpad_6),
        make_pair(GLFW_KEY_KP_7, Key::Numpad_7),
        make_pair(GLFW_KEY_KP_8, Key::Numpad_8),
        make_pair(GLFW_KEY_KP_9, Key::Numpad_9),
        make_pair(GLFW_KEY_KP_DECIMAL, Key::Numpad_Decimal),
        make_pair(GLFW_KEY_KP_DIVIDE, Key::Numpad_Divide),
        make_pair(GLFW_KEY_KP_MULTIPLY, Key::Numpad_Multiply),
        make_pair(GLFW_KEY_KP_SUBTRACT, Key::Numpad_Subtract),
        make_pair(GLFW_KEY_KP_ADD, Key::Numpad_Add),
        make_pair(GLFW_KEY_KP_ENTER, Key::Numpad_Enter),
        make_pair(GLFW_KEY_KP_EQUAL, Key::Numpad_Equal),
        #if 0
        #define GLFW_KEY_LEFT_SHIFT         340
        #define GLFW_KEY_LEFT_CONTROL       341
        #define GLFW_KEY_LEFT_ALT           342
        #define GLFW_KEY_LEFT_SUPER         343
        #define GLFW_KEY_RIGHT_SHIFT        344
        #define GLFW_KEY_RIGHT_CONTROL      345
        #define GLFW_KEY_RIGHT_ALT          346
        #define GLFW_KEY_RIGHT_SUPER        347
        #define GLFW_KEY_MENU               348
        #endif
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

void Contour::onMouseScroll(double _xOffset, double _yOffset)
{
    enum class VerticalDirection { Up, Down };
    VerticalDirection const vertical = _yOffset > 0.0 ? VerticalDirection::Up: VerticalDirection::Down;

    switch (modifier_)
    {
        case terminal::Modifier::Control: // increase/decrease font size
            if (vertical == VerticalDirection::Up)
                setFontSize(config_.fontSize + 1, true);
            else
                setFontSize(config_.fontSize - 1, true);
            break;
        case terminal::Modifier::Alt: // TODO: increase/decrease transparency
            if (vertical == VerticalDirection::Up)
                --config_.backgroundOpacity;
            else
                ++config_.backgroundOpacity;
            terminalView_.setBackgroundOpacity(config_.backgroundOpacity);
            screenDirty_ = true;
            glfwPostEmptyEvent();
            break;
        case terminal::Modifier::None: // TODO: scroll in history
            break;
        default:
            break;
    }
}

void Contour::onKey(int _key, int _scanCode, int _action, int _mods)
{
    // TODO: investigate how to handle when one of these state vars are true, and the window loses focus.
    // They should be recaptured after focus gain again.
    modifier_ = makeModifier(_mods);

    keyHandled_ = false;
    if (_action == GLFW_PRESS || _action == GLFW_REPEAT)
    {
        // Screenshot: ALT+CTRL+S
        if (_key == GLFW_KEY_S && modifier_ == (terminal::Modifier::Control + terminal::Modifier::Alt))
        {
            auto const screenshot = terminalView_.screenshot();
            ofstream ofs{ "screenshot.vt", ios::trunc | ios::binary };
            ofs << screenshot;
            keyHandled_ = true;
        }
        else if (_key == GLFW_KEY_EQUAL && modifier_ == (terminal::Modifier::Control + terminal::Modifier::Shift))
        {
            setFontSize(config_.fontSize + 1, true);
            keyHandled_ = true;
        }
        else if (_key == GLFW_KEY_MINUS && modifier_ == (terminal::Modifier::Control + terminal::Modifier::Shift) && config_.fontSize > 5)
        {
            setFontSize(config_.fontSize - 1, true);
            keyHandled_ = true;
        }
        else if (_key == GLFW_KEY_ENTER && modifier_ == terminal::Modifier::Alt)
        {
            window_.toggleFullScreen();
            keyHandled_ = true;
        }
        else if (auto const key = glfwKeyToTerminalKey(_key); key.has_value())
        {
            terminalView_.send(key.value(), modifier_);
            keyHandled_ = true;
        }
        else if (const char* cstr = glfwGetKeyName(_key, _scanCode);
               cstr != nullptr
            && modifier_.some() && modifier_ != terminal::Modifier::Shift
            && strlen(cstr) == 1
            && isalnum(*cstr))
        {
            // allow only mods + alphanumerics
            terminalView_.send(*cstr, modifier_);
            keyHandled_ = true;
        }
        else if (_key == GLFW_KEY_SPACE && modifier_)
        {
            terminalView_.send(L' ', modifier_);
            keyHandled_ = true;
        }
        // else if (modifier_ && modifier_ != terminal::Modifier::Shift)
        //    cout << fmt::format(
        //        "key:{}, scanCode:{}, name:{} ({})",
        //        _key, _scanCode, cstr ? cstr : "(null)", terminal::to_string(modifier_)
        //    ) << endl;
    }
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
        auto const width = config_.terminalSize.columns * regularFont_.maxAdvance();
        auto const height = config_.terminalSize.rows * regularFont_.lineHeight();
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

void Contour::onChar(char32_t _char)
{
    if (!keyHandled_)
        terminalView_.send(_char, terminal::Modifier{});

    keyHandled_ = false;
}

void Contour::onScreenUpdate()
{
    glfwPostEmptyEvent();
}

void Contour::onConfigReload(FileChangeWatcher::Event _event)
{
    configReloadPending_.store(true);
    glfwPostEmptyEvent();
}

bool Contour::loadConfigValues()
{
    auto filePath = config_.backingFilePath.string();
    auto newConfig = Config{};
    try
    {
        loadConfigFromFile(newConfig, filePath);
    }
    catch (exception const& e)
    {
        //logger_.log(ErrorEvent{e.what()});
        cerr << "Failed to load configuration. " << e.what() << endl;
        return false;
    }

    logger_ =
        newConfig.logFilePath
            ? GLLogger{newConfig.loggingMask, newConfig.logFilePath->string()}
            : GLLogger{newConfig.loggingMask, &cout};

    terminalView_.setTabWidth(newConfig.tabWidth);

    bool windowResizeRequired = false;
    if (newConfig.fontSize != config_.fontSize)
        windowResizeRequired |= setFontSize(newConfig.fontSize, false);

    if (newConfig.terminalSize != config_.terminalSize && !window_.fullscreen())
        windowResizeRequired |= terminalView_.setTerminalSize(config_.terminalSize);

    if (windowResizeRequired && !window_.fullscreen())
    {
        auto const width = newConfig.terminalSize.columns * regularFont_.maxAdvance();
        auto const height = newConfig.terminalSize.rows * regularFont_.lineHeight();
        window_.resize(width, height);
    }

    // TODO... (all the rest)

    config_ = move(newConfig);
    return true;
}
