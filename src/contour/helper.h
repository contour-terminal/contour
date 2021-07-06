/**
 * This file is part of the "contour" project
 *   Copyright (c) 2019-2020 Christian Parpart <christian@parpart.family>
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

#include <contour/Config.h>
#include <terminal/InputGenerator.h>

#include <QtCore/Qt>
#include <QtCore/QCoreApplication>
#include <QtGui/QKeyEvent>

#include <cctype>
#include <map>
#include <string>
#include <string_view>

namespace terminal::renderer
{
    class Renderer;
}

namespace contour {

namespace detail {
    template <typename F>
    class FunctionCallEvent : public QEvent
    {
      private:
       using Fun = typename std::decay<F>::type;
       Fun fun;
      public:
       FunctionCallEvent(Fun && fun) : QEvent(QEvent::None), fun(std::move(fun)) {}
       FunctionCallEvent(Fun const& fun) : QEvent(QEvent::None), fun(fun) {}
       ~FunctionCallEvent() { fun(); }
    };
}

enum class MouseCursorShape
{
    Hidden,
    PointingHand,
    IBeam,
    Arrow,
};

template <typename F>
void postToObject(QObject* obj, F fun)
{
#if 0
    // Qt >= 5.10
    QMetaObject::invokeMethod(obj, std::forward<F>(fun));
#else
    // Qt < 5.10
    QCoreApplication::postEvent(obj, new detail::FunctionCallEvent<F>(std::forward<F>(fun)));
#endif
}

constexpr inline bool isModifier(Qt::Key _key)
{
    switch (_key)
    {
        case Qt::Key_Alt:
        case Qt::Key_Control:
        case Qt::Key_Shift:
        case Qt::Key_Meta:
            return true;
        default:
            return false;
    }
}

constexpr inline char32_t makeChar(Qt::Key _key, Qt::KeyboardModifiers _mods)
{
    auto const value = static_cast<int>(_key);
    if (value >= 'A' && value <= 'Z')
    {
        if (_mods & Qt::ShiftModifier)
            return static_cast<char32_t>(value);
        else
            return static_cast<char32_t>(std::tolower(value));
    }
    return 0;
}

constexpr inline terminal::Modifier makeModifier(Qt::KeyboardModifiers _mods)
{
    using terminal::Modifier;

    Modifier mods{};

    if (_mods & Qt::AltModifier)
        mods |= Modifier::Alt;
    if (_mods & Qt::ShiftModifier)
        mods |= Modifier::Shift;
#if defined(__APPLE__)
    // XXX https://doc.qt.io/qt-5/qt.html#KeyboardModifier-enum
    //     "Note: On macOS, the ControlModifier value corresponds to the Command keys on the keyboard,
    //      and the MetaModifier value corresponds to the Control keys."
    if (_mods & Qt::MetaModifier)
        mods |= Modifier::Control;
    if (_mods & Qt::ControlModifier)
        mods |= Modifier::Meta;
#else
    if (_mods & Qt::ControlModifier)
        mods |= Modifier::Control;
    if (_mods & Qt::MetaModifier)
        mods |= Modifier::Meta;
#endif

    return mods;
}

constexpr inline terminal::MouseButton makeMouseButton(Qt::MouseButton _button)
{
    switch (_button)
    {
        case Qt::MouseButton::RightButton:
            return terminal::MouseButton::Right;
        case Qt::MiddleButton:
            return terminal::MouseButton::Middle;
        case Qt::LeftButton:
            [[fallthrough]];
        default: // d'oh
            return terminal::MouseButton::Left;
    }
}

class TerminalSession;
bool sendKeyEvent(QKeyEvent* _keyEvent, TerminalSession& _session);
void sendWheelEvent(QWheelEvent* _event, TerminalSession& _session);
void sendMousePressEvent(QMouseEvent* _event, TerminalSession& _session);
void sendMouseMoveEvent(QMouseEvent* _event, TerminalSession& _session);
void sendMouseReleaseEvent(QMouseEvent* _event, TerminalSession& _session);

void spawnNewTerminal(std::string const& _programPath,
                      std::string const& _configPath,
                      std::string const& _profileName,
                      std::string const& _cwdUrl);

auto const inline KeyboardTag = crispy::debugtag::make("system.keyboard", "Logs OS keyboard related debug information.");
auto const inline WindowTag = crispy::debugtag::make("system.window", "Logs system window debug events.");
auto const inline WidgetTag = crispy::debugtag::make("system.widget", "Logs system widget related debug information.");

#define CHECKED_GL(code) \
    do { \
        (code); \
        GLenum err{}; \
        while ((err = glGetError()) != GL_NO_ERROR) \
            debuglog(WidgetTag).write("OpenGL error {} for call: {}", err, #code); \
    } while (0)

using PermissionCache = std::map<std::string, bool>;

bool requestPermission(PermissionCache& _cache,
                       QWidget* _parent,
                       config::Permission _allowedByConfig,
                       std::string_view _topicText);

terminal::FontDef getFontDefinition(terminal::renderer::Renderer& _renderer);

terminal::renderer::PageMargin computeMargin(terminal::ImageSize _cellSize,
                                             terminal::PageSize _charCells,
                                             terminal::ImageSize _pixels) noexcept;

bool applyFontDescription(
    terminal::ImageSize _cellSize,
    terminal::PageSize _screenSize,
    terminal::ImageSize _pixelSize,
    crispy::Point _screenDPI,
    terminal::renderer::Renderer& _renderer,
    terminal::renderer::FontDescriptions _fontDescriptions);

constexpr Qt::CursorShape toQtMouseShape(MouseCursorShape _shape)
{
    switch (_shape)
    {
        case contour::MouseCursorShape::Hidden:
            return Qt::CursorShape::BlankCursor;
        case contour::MouseCursorShape::Arrow:
            return Qt::CursorShape::ArrowCursor;
        case contour::MouseCursorShape::IBeam:
            return Qt::CursorShape::IBeamCursor;
        case contour::MouseCursorShape::PointingHand:
            return Qt::CursorShape::PointingHandCursor;
    }

    // should never be reached
    return Qt::CursorShape::ArrowCursor;
}

}
