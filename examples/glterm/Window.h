#pragma once

#include <GL/glew.h>
#include <GLFW/glfw3.h>

#include <functional>
#include <string>

class Window {
  public:
    using OnKey = std::function<void(int, int, int, int)>;
    using OnChar = std::function<void(char32_t)>;
    using OnResize = std::function<void(unsigned, unsigned)>;
    using OnContentScale = std::function<void(float, float)>;

    static void init();

    Window(unsigned _width, unsigned _height, std::string const& _title,
           OnKey _onKey, OnChar _onChar, OnResize _onResize, OnContentScale _onContentScale);
    ~Window();

    GLFWwindow* handle() const noexcept { return window_; }
    operator GLFWwindow* () noexcept { return window_; }

    unsigned width() const noexcept { return width_; }
    unsigned height() const noexcept { return height_; }

    static std::pair<float, float> primaryMonitorContentScale();
    std::pair<float, float> contentScale();

  private:
    static void onContentScale(GLFWwindow* _window, float _xs, float _ys);
    static void onKey(GLFWwindow* _window, int _key, int _scanCode, int _action, int _mods);
    static void onChar(GLFWwindow* _window, unsigned int _char);
    static void onResize(GLFWwindow* _window, int _width, int _height);

  private:
    GLFWwindow* window_;
    unsigned width_;
    unsigned height_;
    OnKey onKey_;
    OnChar onChar_;
    OnResize onResize_;
    OnContentScale onContentScale_;
};
