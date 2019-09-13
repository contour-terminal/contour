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
    window_ = glfwCreateWindow(_width, _height, _title.c_str(), nullptr, nullptr);
    if (!window_)
        throw runtime_error{ "Could not create GLFW window." };

    glfwMakeContextCurrent(window_);

    if (GLenum e = glewInit(); e != GLEW_OK)
        throw runtime_error{ string{"Could not initialize GLEW. "} +((char*)glewGetErrorString(e)) };

    glfwSetWindowUserPointer(window_, this);
    glfwSetKeyCallback(window_, &Window::onKey);
    glfwSetCharCallback(window_, &Window::onChar);
    glfwSetWindowSizeCallback(window_, &Window::onResize);

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_RESIZABLE, GL_TRUE);

#if (GLFW_VERSION_MAJOR >= 4) || (GLFW_VERSION_MAJOR == 3 && GLFW_VERSION_MINOR >= 3)
    glfwSetInputMode(window_, GLFW_LOCK_KEY_MODS, GLFW_TRUE);
    glfwSetWindowContentScaleCallback(window_, &Window::onContentScale);
#endif

    //glEnable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glViewport(0, 0, _width, _height);
}

Window::~Window()
{
    glfwTerminate();
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
