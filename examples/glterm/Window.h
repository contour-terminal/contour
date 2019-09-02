#pragma once

#include <GL/glew.h>
#include <GLFW/glfw3.h>

#include <functional>
#include <stdexcept>
#include <string>

class Window {
public:
    using OnKey = std::function<void(int, int, int, int)>;
    using OnChar = std::function<void(char32_t)>;
    using OnResize = std::function<void(unsigned, unsigned)>;

    Window(unsigned _width, unsigned _height, std::string const& _title,
           OnKey _onKey, OnChar _onChar, OnResize _onResize = {});
    ~Window();

    GLFWwindow* handle() const noexcept { return window_; }
    operator GLFWwindow* () noexcept { return window_; }

    unsigned width() const noexcept { return width_; }
    unsigned height() const noexcept { return height_; }

private:
    static void onKey(GLFWwindow* _window, int _key, int _scanCode, int _action, int _mods)
    {
        if (auto self = reinterpret_cast<Window*>(glfwGetWindowUserPointer(_window)); self && self->onKey_)
            self->onKey_(_key, _scanCode, _action, _mods);
    }

    static void onChar(GLFWwindow* _window, unsigned int _char)
    {
        if (auto self = reinterpret_cast<Window*>(glfwGetWindowUserPointer(_window)); self && self->onChar_)
            self->onChar_(char32_t{ _char });
    }

    static void onResize(GLFWwindow* _window, int _width, int _height)
    {
        if (auto self = reinterpret_cast<Window*>(glfwGetWindowUserPointer(_window)); self)
        {
            self->width_ = static_cast<unsigned>(_width);
            self->height_ = static_cast<unsigned>(_height);

            if (self->onResize_)
                self->onResize_(self->width_, self->height_);
        }
    }

private:
    GLFWwindow* window_;
    unsigned width_;
    unsigned height_;
    OnKey onKey_;
    OnChar onChar_;
    OnResize onResize_;
};

inline Window::Window(unsigned _width, unsigned _height, std::string const& _title,
                      OnKey _onKey, OnChar _onChar, OnResize _onResize) :
    width_{ _width },
    height_{ _height },
    onKey_{ std::move(_onKey) },
    onChar_{ std::move(_onChar) },
    onResize_{ std::move(_onResize) }
{
    if (!glfwInit())
        throw std::runtime_error{ "Could not initialize GLFW." };

    window_ = glfwCreateWindow(_width, _height, _title.c_str(), nullptr, nullptr);
    if (!window_)
        throw std::runtime_error{ "Could not create GLFW window." };

    glfwMakeContextCurrent(window_);

    if (GLenum e = glewInit(); e != GLEW_OK)
        throw std::runtime_error{ std::string{"Could not initialize GLEW. "} +((char*)glewGetErrorString(e)) };

    glfwSetWindowUserPointer(window_, this);
    glfwSetKeyCallback(window_, &Window::onKey);
    glfwSetCharCallback(window_, &Window::onChar);
    glfwSetWindowSizeCallback(window_, &Window::onResize);

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_RESIZABLE, GL_TRUE);
    glfwSetInputMode(window_, GLFW_LOCK_KEY_MODS, GLFW_TRUE);

    glViewport(0, 0, _width, _height);
}

inline Window::~Window()
{
    glfwTerminate();
}
