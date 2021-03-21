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
#include <QtGui/QKeySequence>
#include <QtGui/QKeyEvent>

#include <cctype>

namespace terminal::view {
    class TerminalView;
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
            return value;
        else
            return std::tolower(value);
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

QKeySequence toKeySequence(QKeyEvent* _keyEvent);

std::optional<terminal::InputEvent> mapQtToTerminalKeyEvent(int _key, Qt::KeyboardModifiers _mods);

void configureTerminal(terminal::view::TerminalView& _terminalView,
                       config::Config const& _newConfig,
                       std::string const& _profileName);

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

}
