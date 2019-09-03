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
#include <terminal/InputGenerator.h>
#include <terminal/OutputGenerator.h>
#include <terminal/Terminal.h>
#include <terminal/Process.h>
#include <terminal/UTF8.h>
#include <terminal/Util.h>

#include <cstdio>
#include <cctype>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <unordered_map>
#include <variant>

#include <GL/glew.h>
#include <GLFW/glfw3.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "Window.h"
#include "GLTerminal.h"

// TODOs:
// - [x] proper glterm termination (window close as well as process exit)
// - [x] Fix window-resize: call Screen::resize(), PseudoTerminal::updateWindowSize()
// - [ ] Fullscreen support (ALT+ENTER, or similar)
// - [ ] other SGRs (bold, italic, etc)
// - [ ] fix text positioning (chars seem pressed down instead of centered)
// - [ ] font loading on Linux
// - [ ] input: F13..F25
// - [ ] input: GLFW_KEY_PRINT_SCREEN
// - [ ] input: GLFW_KEY_PAUSE
// - [ ] input: rename Numpad_Dot to Numpad_Decimal, and others (Div -> Divide, etc)
// - [ ] input: GLFW_KEY_KP_EQUAL
// - [ ] show cursor (in correct shapes, with blinking)

#if defined(__unix__)
#include <unistd.h>
#endif

#if defined(_MSC_VER)
#include <Windows.h>
#endif

using namespace std;
using namespace std::placeholders;

class GLTerm {
  public:
    GLTerm(unsigned _width, unsigned _height, unsigned short _fontSize, std::string const& _shell);
    ~GLTerm();

    int main();

    terminal::WindowSize computeWindowSize() const noexcept;

  private:
    template <typename... Args>
    void log(std::string const& msg, Args... args);

    void render();
    void onResize(unsigned _width, unsigned _height);
    void onKey(int _key, int _scanCode, int _action, int _mods);
    void onChar(char32_t _char);
    void onScreenUpdateHook(std::vector<terminal::Command> const& _commands);

  private:
    Window window_;
    std::ofstream logger_;
    GLTerminal terminalView_;
};

GLTerm::GLTerm(unsigned _width, unsigned _height, unsigned short _fontSize, std::string const& _shell) :
    window_{ _width, _height, "glterm",
        bind(&GLTerm::onKey, this, _1, _2, _3, _4),
        bind(&GLTerm::onChar, this, _1),
        bind(&GLTerm::onResize, this, _1, _2)
    },
    logger_{ "glterm.log", ios::trunc },
    terminalView_{0, 0, _width, _height, _fontSize, _shell}
{
    onResize(_width, _height);
}

GLTerm::~GLTerm()
{
}

template <typename... Args>
void GLTerm::log(std::string const& msg, Args... args)
{
    logger_ << fmt::format(msg, args...) << '\n';
}

int GLTerm::main()
{
    while (!glfwWindowShouldClose(window_) && terminalView_.alive())
    {
        terminalView_.render();
        glfwPollEvents();
    }

    return EXIT_SUCCESS;
}

void GLTerm::render()
{
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    terminalView_.render();

    glfwSwapBuffers(window_);
}

void GLTerm::onResize(unsigned _width, unsigned _height)
{
    glViewport(0, 0, _width, _height);
    terminalView_.resize(_width, _height);
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

void GLTerm::onKey(int _key, int _scanCode, int _action, int _mods)
{
    if (_action == GLFW_PRESS || _action == GLFW_REPEAT)
    {
        terminal::Modifier const mods = makeModifier(_mods);

        char const* keyName = glfwGetKeyName(_key, _scanCode);

        logger_ << fmt::format("key: {} {}, action:{}, mod:{:02X} ({})\n",
                               _key,
                               keyName ? keyName : "",
                               _action,
                               static_cast<unsigned>(_mods),
                               terminal::to_string(mods));

        // Screenshot: ALT+CTRL+S
        if (_key == GLFW_KEY_S && mods == (terminal::Modifier::Control + terminal::Modifier::Alt))
        {
            logger_ << "Taking screenshot.\n";
            auto const screenshot = terminalView_.screenshot();
            ofstream ofs{ "screenshot.vt", ios::trunc | ios::binary };
            ofs << screenshot;
            ofs.flush();
            return;
        }

        if (auto const key = glfwKeyToTerminalKey(_key); key.has_value())
            terminalView_.send(key.value(), mods);
        else if (const char* cstr = glfwGetKeyName(_key, _scanCode); cstr != nullptr && strlen(cstr) == 1 && mods.some() && isalnum(*cstr))
            // allow only mods + alphanumerics
            terminalView_.send(*cstr, mods);
        //else
        //    logger << fmt::format("No key mapping found for key:{}, scanCode:{}, name:{} ({}).\n", _key, _scanCode, cstr, terminal::to_string(mods));

        glfwPostEmptyEvent();
    }
}

void GLTerm::onChar(char32_t _char)
{
    if (utf8::isASCII(_char) && isprint(_char))
        logger_ << fmt::format("char: {}\n", static_cast<char>(_char));
    else
        logger_ << fmt::format("char: 0x{:04X}\n", static_cast<uint32_t>(_char));

    terminalView_.send(_char, terminal::Modifier{});

    glfwPostEmptyEvent();
}

void GLTerm::onScreenUpdateHook([[maybe_unused]] vector<terminal::Command> const& _commands)
{
    // we could add some high level VT output logging here.
    glfwPostEmptyEvent();
    //logger_ << fmt::format("onScreenUpdate: {} instructions\n", _commands.size());

    //for (terminal::Command const& command : _commands)
    //    logger_ << to_string(command) << '\n';
}

int main(int argc, char const* argv[])
{
    try
    {
        unsigned const fontSize = 28;
        unsigned const charWidth = 15;
        unsigned const charHeight = 33;
        auto glterm = GLTerm{charWidth * 120, charHeight * 30, fontSize, terminal::Process::loginShell()};
        return glterm.main();
    }
    catch (exception const& e)
    {
        cerr << "Unhandled error caught. " << e.what() << endl;
        return EXIT_FAILURE;
    }
}
