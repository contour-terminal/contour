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
#include <contour/helper.h>
#include <terminal_view/TerminalView.h>

#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QUrl>
#include <QtCore/QProcess>
#include <QtNetwork/QHostInfo>

#include <array>
#include <algorithm>
#include <mutex>

using std::array;
using std::scoped_lock;
using std::nullopt;
using std::optional;
using std::string;
using std::pair;

namespace contour {

QKeySequence toKeySequence(QKeyEvent *_keyEvent)
{
    auto const mod = [](int _qtMod) constexpr -> int {
        int res = 0;
        if (_qtMod & Qt::AltModifier) res += Qt::ALT;
        if (_qtMod & Qt::ShiftModifier) res += Qt::SHIFT;
#if defined(__APPLE__)
        // XXX https://doc.qt.io/qt-5/qt.html#KeyboardModifier-enum
        //     "Note: On macOS, the ControlModifier value corresponds to the Command keys on the keyboard,
        //      and the MetaModifier value corresponds to the Control keys."
        if (_qtMod & Qt::ControlModifier) res += Qt::META;
        if (_qtMod & Qt::MetaModifier) res += Qt::CTRL;
#else
        if (_qtMod & Qt::ControlModifier) res += Qt::CTRL;
        if (_qtMod & Qt::MetaModifier) res += Qt::META;
#endif
        return res;
    }(_keyEvent->modifiers());

    // only modifier but no key press?
    if (isModifier(static_cast<Qt::Key>(_keyEvent->key())))
        return QKeySequence();

    // modifier AND key press?
    if (_keyEvent->key() && mod)
        return QKeySequence(int(_keyEvent->modifiers() | _keyEvent->key()));

    return QKeySequence();
}

optional<terminal::InputEvent> mapQtToTerminalKeyEvent(int _key, Qt::KeyboardModifiers _mods)
{
    using terminal::Key;
    using terminal::InputEvent;
    using terminal::KeyInputEvent;
    using terminal::CharInputEvent;

    static auto constexpr mapping = array{
        pair{Qt::Key_Insert, Key::Insert},
        pair{Qt::Key_Delete, Key::Delete},
        pair{Qt::Key_Right, Key::RightArrow},
        pair{Qt::Key_Left, Key::LeftArrow},
        pair{Qt::Key_Down, Key::DownArrow},
        pair{Qt::Key_Up, Key::UpArrow},
        pair{Qt::Key_PageDown, Key::PageDown},
        pair{Qt::Key_PageUp, Key::PageUp},
        pair{Qt::Key_Home, Key::Home},
        pair{Qt::Key_End, Key::End},
        pair{Qt::Key_F1, Key::F1},
        pair{Qt::Key_F2, Key::F2},
        pair{Qt::Key_F3, Key::F3},
        pair{Qt::Key_F4, Key::F4},
        pair{Qt::Key_F5, Key::F5},
        pair{Qt::Key_F6, Key::F6},
        pair{Qt::Key_F7, Key::F7},
        pair{Qt::Key_F8, Key::F8},
        pair{Qt::Key_F9, Key::F9},
        pair{Qt::Key_F10, Key::F10},
        pair{Qt::Key_F11, Key::F11},
        pair{Qt::Key_F12, Key::F12},
        // todo: F13..F25
        // TODO: NumPad
        // pair{Qt::Key_0, Key::Numpad_0},
        // pair{Qt::Key_1, Key::Numpad_1},
        // pair{Qt::Key_2, Key::Numpad_2},
        // pair{Qt::Key_3, Key::Numpad_3},
        // pair{Qt::Key_4, Key::Numpad_4},
        // pair{Qt::Key_5, Key::Numpad_5},
        // pair{Qt::Key_6, Key::Numpad_6},
        // pair{Qt::Key_7, Key::Numpad_7},
        // pair{Qt::Key_8, Key::Numpad_8},
        // pair{Qt::Key_9, Key::Numpad_9},
        // pair{Qt::Key_Period, Key::Numpad_Decimal},
        // pair{Qt::Key_Slash, Key::Numpad_Divide},
        // pair{Qt::Key_Asterisk, Key::Numpad_Multiply},
        // pair{Qt::Key_Minus, Key::Numpad_Subtract},
        // pair{Qt::Key_Plus, Key::Numpad_Add},
        // pair{Qt::Key_Enter, Key::Numpad_Enter},
        // pair{Qt::Key_Equal, Key::Numpad_Equal},
    };

    if (auto i = find_if(begin(mapping), end(mapping), [_key](auto const& x) { return x.first == _key; }); i != end(mapping))
        return { InputEvent{KeyInputEvent{i->second, makeModifier(_mods)}} };

    if (_key == Qt::Key_Backtab)
        return { InputEvent{CharInputEvent{'\t', makeModifier(_mods | Qt::ShiftModifier)}} };

    return nullopt;
}

void configureTerminal(terminal::view::TerminalView& _terminalView,
                       config::Config const& _newConfig,
                       std::string const& _profileName)
{
    terminal::Terminal& terminal = _terminalView.terminal();
    terminal::Screen& screen = terminal.screen();
    config::TerminalProfile const* profile = _newConfig.profile(_profileName);
    auto const _l = scoped_lock{terminal};

    terminal.setWordDelimiters(_newConfig.wordDelimiters);
    terminal.setMouseProtocolBypassModifier(_newConfig.bypassMouseProtocolModifier);

    terminal.screen().setRespondToTCapQuery(_newConfig.experimentalFeatures.count("tcap"));

    screen.setSixelCursorConformance(_newConfig.sixelCursorConformance);
    screen.setMaxImageColorRegisters(_newConfig.maxImageColorRegisters);
    screen.setMaxImageSize(_newConfig.maxImageSize);
    screen.setMode(terminal::DECMode::SixelScrolling, _newConfig.sixelScrolling);

    if (profile != nullptr)
        return;

    _terminalView.renderer().setFonts(profile->fonts);
    _terminalView.updateFontMetrics();

    auto const newScreenSize = _terminalView.size() / _terminalView.gridMetrics().cellSize;
    if (newScreenSize != _terminalView.terminal().screenSize())
        _terminalView.setTerminalSize(newScreenSize);
        // TODO: maybe update margin after this call?

    screen.setTabWidth(profile->tabWidth);
    screen.setMaxHistoryLineCount(profile->maxHistoryLineCount);
    terminal.setCursorDisplay(profile->cursorDisplay);
    terminal.setCursorShape(profile->cursorShape);
    _terminalView.setColorProfile(profile->colors);
    _terminalView.setHyperlinkDecoration(profile->hyperlinkDecoration.normal,
                                         profile->hyperlinkDecoration.hover);
}

void spawnNewTerminal(string const& _programPath,
                      string const& _configPath,
                      string const& _profileName,
                      string const& _cwdUrl)
{
    auto const wd = [&]() -> QString {
        auto const url = QUrl(QString::fromUtf8(_cwdUrl.c_str()));
        if (url.host() == QHostInfo::localHostName())
            return url.path();
        else
            return QString();
    }();

    QString const program = QString::fromUtf8(_programPath.c_str());
    QStringList args;

    if (!_configPath.empty())
        args << "config" << QString::fromStdString(_configPath);

    if (!_profileName.empty())
        args << "profile" << QString::fromStdString(_profileName);

    if (!wd.isEmpty())
        args << "working-directory" << wd;

    QProcess::startDetached(program, args);
}

} // end namespace
