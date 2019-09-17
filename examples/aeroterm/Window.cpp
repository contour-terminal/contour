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
#include "Window.h"

#include <GL/glew.h>
#include <GLFW/glfw3.h>

#include <functional>
#include <stdexcept>
#include <string>

#if defined(_WIN32)
#include <Windows.h>
#include <dwmapi.h>

#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#endif

using namespace std;

void Window::init()
{
    if (!glfwInit())
        throw runtime_error{ "Could not initialize GLFW." };
}

Window::Window(unsigned _width, unsigned _height, string const& _title,
               OnKey _onKey, OnChar _onChar, OnResize _onResize, OnContentScale _onContentScale) :
    width_{ _width },
    height_{ _height },
    onKey_{ move(_onKey) },
    onChar_{ move(_onChar) },
    onResize_{ move(_onResize) },
    onContentScale_{ move(_onContentScale) }
{
    init();

    glfwWindowHint(GLFW_RESIZABLE, GL_TRUE);
    glfwWindowHint(GLFW_DEPTH_BITS, 16);
#if (GLFW_VERSION_MAJOR == 3 && GLFW_VERSION_MINOR >= 3) || (GLFW_VERSION_MAJOR > 3)
    glfwWindowHint(GLFW_TRANSPARENT_FRAMEBUFFER, GLFW_TRUE);
#endif

    // FIXME: enabling this causes background to go away.
    //glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    //glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    //glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    window_ = glfwCreateWindow(_width, _height, _title.c_str(), nullptr, nullptr);
    if (!window_)
    {
#if (GLFW_VERSION_MAJOR == 3 && GLFW_VERSION_MINOR >= 3) || (GLFW_VERSION_MAJOR > 3)
        char const* desc = nullptr;
        glfwGetError(&desc);
        throw runtime_error{ string{"Could not create GLFW window. "} + desc };
#else
        throw runtime_error{ "Could not create GLFW window." };
#endif
    }

    glfwMakeContextCurrent(window_);

    if (GLenum e = glewInit(); e != GLEW_OK)
        throw runtime_error{ string{"Could not initialize GLEW. "} +((char*)glewGetErrorString(e)) };

    glfwSetWindowUserPointer(window_, this);
    glfwSetKeyCallback(window_, &Window::onKey);
    glfwSetCharCallback(window_, &Window::onChar);
    glfwSetWindowSizeCallback(window_, &Window::onResize);

#if (GLFW_VERSION_MAJOR >= 4) || (GLFW_VERSION_MAJOR == 3 && GLFW_VERSION_MINOR >= 3)
    glfwSetInputMode(window_, GLFW_LOCK_KEY_MODS, GLFW_TRUE);
    glfwSetWindowContentScaleCallback(window_, &Window::onContentScale);
#endif

    glEnable(GL_BLEND);
    glEnable(GL_DEPTH);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glViewport(0, 0, _width, _height);
}

Window::~Window()
{
    glfwTerminate();
}

bool Window::enableBackgroundBlur()
{
#if defined(_WIN32)
    // Awesome hack with the noteworty links:
    // * https://gist.github.com/ethanhs/0e157e4003812e99bf5bc7cb6f73459f (used as code template)
    // * https://github.com/riverar/sample-win32-acrylicblur/blob/master/MainWindow.xaml.cs
    // * https://stackoverflow.com/questions/44000217/mimicking-acrylic-in-a-win32-app
    // p.s.: if you find a more official way to do it, please PR me. :)

    bool success = false;
    if (HWND hwnd = glfwGetWin32Window(window_); hwnd != nullptr)
    {
        const HINSTANCE hModule = LoadLibrary(TEXT("user32.dll"));
        if (hModule)
        {
            struct ACCENTPOLICY
            {
                int nAccentState;
                int nFlags;
                int nColor;
                int nAnimationId;
            };
            struct WINCOMPATTRDATA
            {
                int nAttribute;
                PVOID pData;
                ULONG ulDataSize;
            };
            typedef BOOL(WINAPI *pSetWindowCompositionAttribute)(HWND, WINCOMPATTRDATA*);
            const pSetWindowCompositionAttribute SetWindowCompositionAttribute = (pSetWindowCompositionAttribute)GetProcAddress(hModule, "SetWindowCompositionAttribute");
            if (SetWindowCompositionAttribute)
            {
                ACCENTPOLICY policy = { 3, 0, 0, 0 }; // ACCENT_ENABLE_BLURBEHIND=3...
                WINCOMPATTRDATA data = { 19, &policy, sizeof(ACCENTPOLICY) }; // WCA_ACCENT_POLICY=19
                BOOL rs = SetWindowCompositionAttribute(hwnd, &data);
                success = rs != FALSE;
            }
            FreeLibrary(hModule);
        }
    }
    return success;
#else
    // Get me working on Linux (and OS/X), please.
    return false;
#endif
}

void Window::onResize(GLFWwindow* _window, int _width, int _height)
{
    if (auto self = reinterpret_cast<Window*>(glfwGetWindowUserPointer(_window)); self)
    {
        self->width_ = static_cast<unsigned>(_width);
        self->height_ = static_cast<unsigned>(_height);

        if (self->onResize_)
            self->onResize_(self->width_, self->height_);
    }
}

void Window::onContentScale(GLFWwindow* _window, float _xs, float _ys)
{
    if (auto self = reinterpret_cast<Window*>(glfwGetWindowUserPointer(_window)); self && self->onContentScale_)
        self->onContentScale_(_xs, _ys);
}

void Window::onKey(GLFWwindow* _window, int _key, int _scanCode, int _action, int _mods)
{
    if (auto self = reinterpret_cast<Window*>(glfwGetWindowUserPointer(_window)); self && self->onKey_)
        self->onKey_(_key, _scanCode, _action, _mods);
}

void Window::onChar(GLFWwindow* _window, unsigned int _char)
{
    if (auto self = reinterpret_cast<Window*>(glfwGetWindowUserPointer(_window)); self && self->onChar_)
        self->onChar_(char32_t{ _char });
}

pair<float, float> Window::primaryMonitorContentScale()
{
    init();
#if (GLFW_VERSION_MAJOR >= 4) || (GLFW_VERSION_MAJOR == 3 && GLFW_VERSION_MINOR >= 3)
    float xs{};
    float ys{};
    glfwGetMonitorContentScale(glfwGetPrimaryMonitor(), &xs, &ys);
    return { xs, ys };
#else
    return { 1.0f, 1.0f };
#endif
}

pair<float, float> Window::contentScale()
{
#if (GLFW_VERSION_MAJOR >= 4) || (GLFW_VERSION_MAJOR == 3 && GLFW_VERSION_MINOR >= 3)
    float xs{};
    float ys{};
    glfwGetWindowContentScale(window_, &xs, &ys);
    //glfwGetMonitorContentScale(glfwGetPrimaryMonitor(), &xs, &ys);
    return { xs, ys };
#else
    return { 1.0f, 1.0f };
#endif
}
