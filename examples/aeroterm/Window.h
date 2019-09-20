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
#pragma once

#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>

#include <functional>
#include <string>

class Window {
  public:
    struct Size {
        unsigned width;
        unsigned height;
    };
    using OnKey = std::function<void(int, int, int, int)>;
    using OnChar = std::function<void(char32_t)>;
    using OnResize = std::function<void()>;
    using OnContentScale = std::function<void(float, float)>;

    static void init();

    Window(Size const& _size, std::string const& _title,
           OnKey _onKey, OnChar _onChar, OnResize _onResize, OnContentScale _onContentScale);
    ~Window();

    bool enableBackgroundBlur();

    GLFWwindow* handle() const noexcept { return window_; }
    operator GLFWwindow* () noexcept { return window_; }

    Size const& size() const noexcept { return size_; }
    void resize(unsigned _width, unsigned _height);
    unsigned width() const noexcept { return size_.width; }
    unsigned height() const noexcept { return size_.height; }

    static std::pair<float, float> primaryMonitorContentScale();
    std::pair<float, float> contentScale();

    bool fullscreen() const noexcept { return fullscreen_; }
    void toggleFullScreen();

  private:
    static void onContentScale(GLFWwindow* _window, float _xs, float _ys);
    static void onKey(GLFWwindow* _window, int _key, int _scanCode, int _action, int _mods);
    static void onChar(GLFWwindow* _window, unsigned int _char);
    static void onResize(GLFWwindow* _window, int _width, int _height);

  private:
    GLFWwindow* window_;
    bool fullscreen_ = false;
    Size size_;
    Size lastSize_;
    glm::ivec2 oldPosition_{1, 1};
    OnKey onKey_;
    OnChar onChar_;
    OnResize onResize_;
    OnContentScale onContentScale_;
};
