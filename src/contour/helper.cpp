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
#include <contour/TerminalDisplay.h>
#include <contour/TerminalSession.h>
#include <terminal/Terminal.h>
#include <terminal_renderer/Renderer.h>

#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QUrl>
#include <QtCore/QProcess>
#include <QtNetwork/QHostInfo>
#include <QtWidgets/QMessageBox>

#include <array>
#include <algorithm>
#include <mutex>

using std::array;
using std::chrono::steady_clock;
using std::get;
using std::holds_alternative;
using std::max;
using std::monostate;
using std::nullopt;
using std::optional;
using std::pair;
using std::scoped_lock;
using std::string;
using std::u32string;
using std::variant;
using std::vector;

using crispy::Point;
using crispy::Size;
using crispy::Zero;

namespace contour {

bool sendKeyEvent(QKeyEvent* _event, TerminalSession& _session)
{
    using terminal::Key;
    using terminal::Modifier;

    auto const now = steady_clock::now();

    static auto constexpr keyMappings = array{ // {{{
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
        pair{Qt::Key_F13, Key::F13},
        pair{Qt::Key_F14, Key::F14},
        pair{Qt::Key_F15, Key::F15},
        pair{Qt::Key_F16, Key::F16},
        pair{Qt::Key_F17, Key::F17},
        pair{Qt::Key_F18, Key::F18},
        pair{Qt::Key_F19, Key::F19},
        pair{Qt::Key_F20, Key::F20},
    }; // }}}

    static auto constexpr charMappings = array{ // {{{
        pair{Qt::Key_AsciiCircum, '^'},
        pair{Qt::Key_AsciiTilde, '~'},
        pair{Qt::Key_Backslash, '\\'},
        pair{Qt::Key_Bar, '|'},
        pair{Qt::Key_BraceLeft, '{'},
        pair{Qt::Key_BraceRight, '}'},
        pair{Qt::Key_BracketLeft, '['},
        pair{Qt::Key_BracketRight, ']'},
        pair{Qt::Key_QuoteLeft, '`'},
        pair{Qt::Key_Underscore, '_'},
    }; // }}}

    auto const modifiers = makeModifier(_event->modifiers());
    auto const key = _event->key();

    if (auto i = find_if(begin(keyMappings), end(keyMappings), [_event](auto const& x) { return x.first == _event->key(); }); i != end(keyMappings))
    {
        _session.sendKeyPressEvent(i->second, modifiers, now);
        return true;
    }

    if (auto i = find_if(begin(charMappings), end(charMappings), [_event](auto const& x) { return x.first == _event->key(); }); i != end(charMappings))
    {
        _session.sendCharPressEvent(static_cast<char32_t>(i->second), modifiers, now);
        return true;
    }

    if (key == Qt::Key_Backtab)
    {
        _session.sendCharPressEvent(U'\t', modifiers.with(Modifier::Shift), now);
        return true;
    }

    if (modifiers.contains(Modifier::Control))
    {
        if (key >= 0x20 && key < 0x7F)
        {
            _session.sendCharPressEvent(static_cast<char32_t>(key),
                                        modifiers,
                                        now);
            return true;
        }
        switch (key)
        {
            case Qt::Key_BraceLeft: _session.sendCharPressEvent(L'[', modifiers, now);
                return true;
            case Qt::Key_Equal:
                _session.sendCharPressEvent(L'=', modifiers, now);
                return true;
            case Qt::Key_BraceRight:
                _session.sendCharPressEvent(L']', modifiers, now);
                return true;
        }
    }

    if (!_event->text().isEmpty())
    {
        for (auto const ch: _event->text().toUcs4())
            _session.sendCharPressEvent(ch, modifiers, now);
        return true;
    }

    return false;
}

void sendWheelEvent(QWheelEvent* _event, TerminalSession& _session)
{
    auto const yDelta = [&]() -> int
    {
        switch (_event->orientation())
        {
            case Qt::Orientation::Horizontal:
                return _event->pixelDelta().x() ? _event->pixelDelta().x()
                                                : _event->angleDelta().x();
            case Qt::Orientation::Vertical:
                return _event->pixelDelta().y() ? _event->pixelDelta().y()
                                                : _event->angleDelta().y();
        }
        return 0;
    }();

    if (yDelta)
    {
        auto const button = yDelta > 0 ? terminal::MouseButton::WheelUp
                                       : terminal::MouseButton::WheelDown;

        auto const modifier = makeModifier(_event->modifiers());

        _session.sendMousePressEvent(button, modifier, steady_clock::now());
    }
}

void sendMousePressEvent(QMouseEvent* _event, TerminalSession& _session)
{
    _session.sendMousePressEvent(makeMouseButton(_event->button()),
                                 makeModifier(_event->modifiers()),
                                 steady_clock::now());
    _event->accept();
}

void sendMouseReleaseEvent(QMouseEvent* _event, TerminalSession& _session)
{
    _session.sendMouseReleaseEvent(makeMouseButton(_event->button()),
                                   makeModifier(_event->modifiers()),
                                   steady_clock::now());
    _event->accept();
}

void sendMouseMoveEvent(QMouseEvent* _event, TerminalSession& _session)
{
    // NB: This translation depends on the display's margin, so maybe
    //     the display should provide the translation?
    auto constexpr MarginTop = 0;
    auto constexpr MarginLeft = 0;
    auto const cellSize = _session.display()->cellSize();
    auto const row = int{1 + (max(_event->y(), 0) - MarginTop) /  cellSize.height.as<int>()};
    auto const col = int{1 + (max(_event->x(), 0) - MarginLeft) / cellSize.width.as<int>()};

    _session.sendMouseMoveEvent(row, col,
                                makeModifier(_event->modifiers()),
                                steady_clock::now());
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

bool requestPermission(PermissionCache& _cache,
                       QWidget* _parent,
                       config::Permission _allowedByConfig,
                       std::string_view _topicText)
{
    switch (_allowedByConfig)
    {
        case config::Permission::Allow:
            debuglog(WidgetTag).write("Permission for {} allowed by configuration.", _topicText);
            return true;
        case config::Permission::Deny:
            debuglog(WidgetTag).write("Permission for {} denied by configuration.", _topicText);
            return false;
        case config::Permission::Ask:
            break;
    }

    // Did we remember a last interactive question?
    if (auto const i = _cache.find(string(_topicText)); i != _cache.end())
        return i->second;

    debuglog(WidgetTag).write("Permission for {} requires asking user.", _topicText);

    auto const reply = QMessageBox::question(_parent,
        fmt::format("{} requested", _topicText).c_str(),
        QString::fromStdString(fmt::format("The application has requested for {}. Do you allow this?", _topicText)),
        QMessageBox::StandardButton::Yes
            | QMessageBox::StandardButton::YesToAll
            | QMessageBox::StandardButton::No
            | QMessageBox::StandardButton::NoToAll,
        QMessageBox::StandardButton::NoButton
    );

    switch (reply)
    {
        case QMessageBox::StandardButton::NoToAll:
            _cache[string(_topicText)] = false;
            break;
        case QMessageBox::StandardButton::YesToAll:
            _cache[string(_topicText)] = true;
            [[fallthrough]];
        case QMessageBox::StandardButton::Yes:
            return true;
        default:
            break;
    }

    return false;
}

terminal::FontDef getFontDefinition(terminal::renderer::Renderer& _renderer)
{
    auto const fontByStyle = [&](text::font_weight _weight, text::font_slant _slant) -> text::font_description const&
    {
        auto const bold = _weight != text::font_weight::normal;
        auto const italic = _slant != text::font_slant::normal;
        if (bold && italic)
            return _renderer.fontDescriptions().boldItalic;
        else if (bold)
            return _renderer.fontDescriptions().bold;
        else if (italic)
            return _renderer.fontDescriptions().italic;
        else
            return _renderer.fontDescriptions().regular;
    };
    auto const nameOfStyledFont = [&](text::font_weight _weight, text::font_slant _slant) -> string
    {
        auto const& regularFont = _renderer.fontDescriptions().regular;
        auto const& styledFont = fontByStyle(_weight, _slant);
        if (styledFont.familyName == regularFont.familyName)
            return "auto";
        else
            return styledFont.toPattern();
    };
    return {
        _renderer.fontDescriptions().size.pt,
        _renderer.fontDescriptions().regular.familyName,
        nameOfStyledFont(text::font_weight::bold, text::font_slant::normal),
        nameOfStyledFont(text::font_weight::normal, text::font_slant::italic),
        nameOfStyledFont(text::font_weight::bold, text::font_slant::italic),
        _renderer.fontDescriptions().emoji.toPattern()
    };
}

terminal::renderer::PageMargin computeMargin(terminal::ImageSize _cellSize,
                                             terminal::PageSize _charCells,
                                             terminal::ImageSize _pixels) noexcept
{
    auto const usedHeight = static_cast<int>(*_charCells.lines * *_cellSize.height);
    auto const freeHeight = static_cast<int>(*_pixels.height - usedHeight);
    auto const bottomMargin = freeHeight;

    //auto const usedWidth = _charCells.columns * regularFont_.maxAdvance();
    //auto const freeWidth = _pixels.width - usedWidth;
    auto constexpr leftMargin = 0;

    return {leftMargin, bottomMargin};
}

bool applyFontDescription(terminal::ImageSize _cellSize,
                          terminal::PageSize _screenSize,
                          terminal::ImageSize _pixelSize,
                          Point _screenDPI,
                          terminal::renderer::Renderer& _renderer,
                          terminal::renderer::FontDescriptions _fontDescriptions)
{
    if (_renderer.fontDescriptions() == _fontDescriptions)
        return false;

    auto const windowMargin = computeMargin(_cellSize, _screenSize, _pixelSize);

    if (_fontDescriptions.dpi == Zero<Point>)
        _fontDescriptions.dpi = _screenDPI;

    _renderer.setFonts(_fontDescriptions);
    _renderer.setMargin(windowMargin);
    _renderer.updateFontMetrics();

    return true;
}

} // end namespace
