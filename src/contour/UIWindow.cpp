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

#include "UIWindow.h"

#include <GL/glew.h>
#include <GLFW/glfw3.h>

#include <algorithm>
#include <functional>
#include <stdexcept>
#include <string>

#if defined(_WIN32)
#include <Windows.h>
#include <dwmapi.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#endif

#if defined(CONTOUR_BLUR_PLATFORM_KWIN_X11)
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#define GLFW_EXPOSE_NATIVE_X11
#endif

#include <GLFW/glfw3native.h>

using namespace std;

void UIWindow::init()
{
    if (!glfwInit())
        throw runtime_error{ "Could not initialize GLFW." };
}

UIWindow::UIWindow(Size const& _size,
            	   string const& _title,
                   OnKey _onKey,
                   OnChar _onChar,
                   OnMouseScroll _onMouseScroll,
                   OnMouseButton _onMouseButton,
                   OnMousePosition _onMousePosition,
                   OnResize _onResize,
                   OnContentScale _onContentScale) :
    size_{ _size },
    onKey_{ move(_onKey) },
    onChar_{ move(_onChar) },
    onMouseScroll_{ move(_onMouseScroll) },
    onMouseButton_{ move(_onMouseButton) },
    onMousePosition_{ move(_onMousePosition) },
    onResize_{ move(_onResize) },
    onContentScale_{ move(_onContentScale) }
{
    init();

    glfwWindowHint(GLFW_RESIZABLE, GL_TRUE);
    glfwWindowHint(GLFW_DEPTH_BITS, 16);

#if (GLFW_VERSION_MAJOR == 3 && GLFW_VERSION_MINOR >= 3) || (GLFW_VERSION_MAJOR > 3)
    glfwWindowHint(GLFW_TRANSPARENT_FRAMEBUFFER, GLFW_TRUE);
#endif

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

#if defined(__APPLE__)
    glfwWindowHint(GLFW_COCOA_GRAPHICS_SWITCHING, GLFW_TRUE);
    glfwWindowHint(GLFW_COCOA_RETINA_FRAMEBUFFER, GLFW_TRUE);
#endif

    window_ = glfwCreateWindow(_size.width, _size.height, _title.c_str(), nullptr, nullptr);
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
    glfwSetKeyCallback(window_, &UIWindow::onKey);
    glfwSetCharCallback(window_, &UIWindow::onChar);
    glfwSetFramebufferSizeCallback(window_, &UIWindow::onResize);
    glfwSetScrollCallback(window_, &UIWindow::onMouseScroll);
    glfwSetMouseButtonCallback(window_, &UIWindow::onMouseButton);
    glfwSetCursorPosCallback(window_, &UIWindow::onMousePosition);

#if (GLFW_VERSION_MAJOR >= 4) || (GLFW_VERSION_MAJOR == 3 && GLFW_VERSION_MINOR >= 3)
    glfwSetInputMode(window_, GLFW_LOCK_KEY_MODS, GLFW_TRUE);
    glfwSetWindowContentScaleCallback(window_, &UIWindow::onContentScale);
#endif

    glEnable(GL_BLEND);
    glEnable(GL_DEPTH);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glViewport(0, 0, _size.width, _size.height);
}

UIWindow::~UIWindow()
{
    glfwTerminate();
}

bool UIWindow::enableBackgroundBlur(bool _enable)
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
				if (_enable)
				{
					ACCENTPOLICY policy = { 3, 0, 0, 0 }; // ACCENT_ENABLE_BLURBEHIND=3...
					WINCOMPATTRDATA data = { 19, &policy, sizeof(ACCENTPOLICY) }; // WCA_ACCENT_POLICY=19
					BOOL rs = SetWindowCompositionAttribute(hwnd, &data);
					success = rs != FALSE;
				}
				else
				{
					success = false; // TODO: how to disable me again?
				}
            }
            FreeLibrary(hModule);
        }
    }
    return success;
#elif defined(CONTOUR_BLUR_PLATFORM_KWIN_X11)
	Display* const display = glfwGetX11Display();
	Window const window = glfwGetX11Window(window_);
	Atom const blurBehindRegion = XInternAtom(display, "_KDE_NET_WM_BLUR_BEHIND_REGION", False);
	if (_enable)
		XChangeProperty(display, window, blurBehindRegion, XA_CARDINAL, 32, PropModeReplace, 0, 0);
	else
		XDeleteProperty(display, window, blurBehindRegion);
	return true;
#else
	if (_enable)
		// Get me working on Linux (and OS/X), please.
    	return false;
	else
		return true;
#endif
}

void UIWindow::onResize(GLFWwindow* _window, int _width, int _height)
{
    if (auto self = reinterpret_cast<UIWindow*>(glfwGetWindowUserPointer(_window)); self)
    {
        if (_width && _height)
        {
            self->lastSize_ = self->size_;
            self->size_ = Size{static_cast<unsigned>(_width), static_cast<unsigned>(_height)};
            if (self->onResize_)
                self->onResize_();
        }
    }
}

void UIWindow::onMouseScroll(GLFWwindow* _window, double _xOffset, double _yOffset)
{
    if (auto self = reinterpret_cast<UIWindow*>(glfwGetWindowUserPointer(_window)); self && self->onMouseScroll_)
        self->onMouseScroll_(_xOffset, _yOffset);
}

void UIWindow::onMouseButton(GLFWwindow* _window, int _button, int _action, int _mods)
{
    //TODO: printf("mouse: button: %d, action:%d, mods:%d\n", _button, _action, _mods);
    if (auto self = reinterpret_cast<UIWindow*>(glfwGetWindowUserPointer(_window)); self && self->onMouseButton_)
        self->onMouseButton_(_button, _action, _mods);
}

void UIWindow::onMousePosition(GLFWwindow* _window, double _x, double _y)
{
    if (auto self = reinterpret_cast<UIWindow*>(glfwGetWindowUserPointer(_window)); self && self->onMousePosition_)
        self->onMousePosition_(_x, _y);
}

void UIWindow::onContentScale(GLFWwindow* _window, float _xs, float _ys)
{
    if (auto self = reinterpret_cast<UIWindow*>(glfwGetWindowUserPointer(_window)); self && self->onContentScale_)
        self->onContentScale_(_xs, _ys);
}

void UIWindow::onKey(GLFWwindow* _window, int _key, int _scanCode, int _action, int _mods)
{
    if (auto self = reinterpret_cast<UIWindow*>(glfwGetWindowUserPointer(_window)); self && self->onKey_)
        self->onKey_(_key, _scanCode, _action, _mods);
}

void UIWindow::onChar(GLFWwindow* _window, unsigned int _char)
{
    if (auto self = reinterpret_cast<UIWindow*>(glfwGetWindowUserPointer(_window)); self && self->onChar_)
        self->onChar_(char32_t{ _char });
}

pair<float, float> UIWindow::primaryMonitorContentScale()
{
    init();
#if (GLFW_VERSION_MAJOR >= 4) || (GLFW_VERSION_MAJOR == 3 && GLFW_VERSION_MINOR >= 3)
    float xs{};
    float ys{};
    glfwGetMonitorContentScale(glfwGetPrimaryMonitor(), &xs, &ys);

    // XXX do not allow content scaling below 1.0
    xs = max(xs, 1.0f);
    ys = max(ys, 1.0f);

    return { xs, ys };
#else
    return { 1.0f, 1.0f };
#endif
}

pair<float, float> UIWindow::contentScale()
{
#if (GLFW_VERSION_MAJOR >= 4) || (GLFW_VERSION_MAJOR == 3 && GLFW_VERSION_MINOR >= 3)
    float xs{};
    float ys{};
    glfwGetWindowContentScale(window_, &xs, &ys);

    // XXX do not allow content scaling below 1.0
    xs = max(xs, 1.0f);
    ys = max(ys, 1.0f);

    return { xs, ys };
#else
    return { 1.0f, 1.0f };
#endif
}

void UIWindow::resize(unsigned _width, unsigned _height)
{
    glfwSetWindowSize(window_, _width, _height);
}

UIWindow::Size UIWindow::screenSize()
{
    GLFWmonitor* monitor = glfwGetPrimaryMonitor();
    GLFWvidmode const* vidMode = glfwGetVideoMode(monitor);

    assert(vidMode->width > 0);
    assert(vidMode->height > 0);

    return Size{static_cast<unsigned>(vidMode->width), static_cast<unsigned>(vidMode->height)};
}

void UIWindow::toggleFullScreen()
{
    fullscreen_ = !fullscreen_;
    if (fullscreen_)
    {
        // Get top/left position of window in windowed mode.
        glfwGetWindowPos(window_, &oldPosition_.x, &oldPosition_.y);

        // Get current window pixel resolution.
        GLFWmonitor* monitor = glfwGetPrimaryMonitor();
        GLFWvidmode const* vidMode = glfwGetVideoMode(monitor);

        // Get into fullscreen of primary monitor.
        glfwSetWindowMonitor(window_, monitor, 0, 0, vidMode->width, vidMode->height, GLFW_DONT_CARE);
    }
    else
    {
        // Get out of fullscreen into windowed mode of old settings.
        glfwSetWindowMonitor(
            window_,
            nullptr,
            oldPosition_.x,
            oldPosition_.y,
            lastSize_.width,
            lastSize_.height,
            GLFW_DONT_CARE
        );
    }
}
