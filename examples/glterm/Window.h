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
    using OnContentScale = std::function<void(float, float)>;

    Window(unsigned _width, unsigned _height, std::string const& _title,
           OnKey _onKey, OnChar _onChar, OnResize _onResize, OnContentScale _onContentScale);
    ~Window();

    GLFWwindow* handle() const noexcept { return window_; }
    operator GLFWwindow* () noexcept { return window_; }

    unsigned width() const noexcept { return width_; }
    unsigned height() const noexcept { return height_; }

    std::pair<float, float> contentScale()
    {
        #if (GLFW_VERSION_MAJOR == 3 && GLFW_VERSION_MINOR >= 3)
        float xs{};
        float ys{};
        glfwGetWindowContentScale(window_, &xs, &ys);
        return {xs, ys};
        #else
        return {1.0f, 1.0f};
        #endif
    }

private:
    static void onContentScale(GLFWwindow* _window, float _xs, float _ys)
    {
        if (auto self = reinterpret_cast<Window*>(glfwGetWindowUserPointer(_window)); self && self->onContentScale_)
            self->onContentScale_(_xs, _ys);
    }

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
    OnContentScale onContentScale_;
};

inline Window::Window(unsigned _width, unsigned _height, std::string const& _title,
                      OnKey _onKey, OnChar _onChar, OnResize _onResize, OnContentScale _onContentScale) :
    width_{ _width },
    height_{ _height },
    onKey_{ std::move(_onKey) },
    onChar_{ std::move(_onChar) },
    onResize_{ std::move(_onResize) },
    onContentScale_{ std::move(_onContentScale) }
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

    #if (GLFW_VERSION_MAJOR == 3 && GLFW_VERSION_MINOR >= 3)
    glfwSetInputMode(window_, GLFW_LOCK_KEY_MODS, GLFW_TRUE);
    glfwSetWindowContentScaleCallback(window_, &Window::onContentScale);
    #endif

    //glEnable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glViewport(0, 0, _width, _height);
}

inline Window::~Window()
{
    glfwTerminate();
}
