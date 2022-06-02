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
#include <contour/TerminalDisplay.h>
#include <contour/TerminalSession.h>
#include <contour/helper.h>

#include <terminal/Terminal.h>

#include <terminal_renderer/Renderer.h>

#include <QtCore/QProcess>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QUrl>
#include <QtGui/QGuiApplication>
#include <QtWidgets/QMessageBox>

#include <algorithm>
#include <array>
#include <mutex>

#include <QtNetwork/QHostInfo>

using std::array;
using std::clamp;
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
using std::chrono::steady_clock;

using crispy::Point;
using crispy::Size;
using crispy::Zero;

using terminal::Height;
using terminal::ImageSize;
using terminal::PageSize;
using terminal::PixelCoordinate;
using terminal::Width;

namespace contour
{

namespace
{
    terminal::CellLocation makeMouseCellLocation(QMouseEvent* event, TerminalSession const& session) noexcept
    {
        auto constexpr MarginTop = 0;
        auto constexpr MarginLeft = 0;

        auto const pageSize = session.terminal().pageSize();
        auto const cellSize = session.display()->cellSize();
        auto const dpr = session.contentScale();

        auto const sx = int(double(event->pos().x()) * dpr);
        auto const sy = int(double(event->pos().y()) * dpr);

        auto const row =
            terminal::LineOffset(clamp((sy - MarginTop) / cellSize.height.as<int>(), 0, *pageSize.lines - 1));

        auto const col = terminal::ColumnOffset(
            clamp((sx - MarginLeft) / cellSize.width.as<int>(), 0, *pageSize.columns - 1));

        return { row, col };
    }

    PixelCoordinate makeMousePixelPosition(QMouseEvent* _event, double dpr) noexcept
    {
        // TODO: apply margin once supported
        return PixelCoordinate { PixelCoordinate::X { int(double(_event->x()) * dpr) },
                                 PixelCoordinate::Y { int(double(_event->y()) * dpr) } };
    }

    PixelCoordinate makeMousePixelPosition(QWheelEvent* _event, double dpr) noexcept
    {
        // TODO: apply margin once supported
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
        return PixelCoordinate { PixelCoordinate::X { int(double(_event->position().x()) * dpr) },
                                 PixelCoordinate::Y { int(double(_event->position().y()) * dpr) } };
#else
        return PixelCoordinate { PixelCoordinate::X { int(double(_event->x()) * dpr) },
                                 PixelCoordinate::Y { int(double(_event->y()) * dpr) } };
#endif
    }

    int mouseWheelDelta(QWheelEvent* _event) noexcept
    {
#if 1
        // FIXME: Temporarily addressing a really bad Qt implementation detail
        // as tracked here:
        // https://github.com/contour-terminal/contour/issues/394
        if (_event->pixelDelta().y())
            return _event->pixelDelta().y();
        if (_event->angleDelta().y())
            return _event->angleDelta().y();

        if (_event->pixelDelta().x())
            return _event->pixelDelta().x();
        if (_event->angleDelta().x())
            return _event->angleDelta().x();

        return 0;

#else
        // switch (_event->orientation())
        // {
        //     case Qt::Orientation::Horizontal:
        //         return _event->pixelDelta().x() ? _event->pixelDelta().x()
        //                                         : _event->angleDelta().x();
        //     case Qt::Orientation::Vertical:
        //         return _event->pixelDelta().y() ? _event->pixelDelta().y()
        //                                         : _event->angleDelta().y();
        // }
        return _event->angleDelta().y();
#endif
    }

} // namespace

QScreen* screenOf(QWidget const* _widget)
{
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
    return _widget->screen();
#elif QT_VERSION >= QT_VERSION_CHECK(5, 10, 0)
    // #warning "Using alternative implementation of screenOf() for Qt >= 5.10.0"
    if (auto topLevel = _widget->window())
    {
        if (auto screenByPos = QGuiApplication::screenAt(topLevel->geometry().center()))
            return screenByPos;
    }
    return QGuiApplication::primaryScreen();
#else
    // #warning "Using alternative implementation of screenOf() for Qt < 5.10.0"
    return QGuiApplication::primaryScreen();
#endif
}

bool sendKeyEvent(QKeyEvent* _event, TerminalSession& _session)
{
    using terminal::Key;
    using terminal::Modifier;

    auto const now = steady_clock::now();

    static auto constexpr keyMappings = array {
        // {{{
        pair { Qt::Key_Insert, Key::Insert },
        pair { Qt::Key_Delete, Key::Delete },
        pair { Qt::Key_Right, Key::RightArrow },
        pair { Qt::Key_Left, Key::LeftArrow },
        pair { Qt::Key_Down, Key::DownArrow },
        pair { Qt::Key_Up, Key::UpArrow },
        pair { Qt::Key_PageDown, Key::PageDown },
        pair { Qt::Key_PageUp, Key::PageUp },
        pair { Qt::Key_Home, Key::Home },
        pair { Qt::Key_End, Key::End },
        pair { Qt::Key_F1, Key::F1 },
        pair { Qt::Key_F2, Key::F2 },
        pair { Qt::Key_F3, Key::F3 },
        pair { Qt::Key_F4, Key::F4 },
        pair { Qt::Key_F5, Key::F5 },
        pair { Qt::Key_F6, Key::F6 },
        pair { Qt::Key_F7, Key::F7 },
        pair { Qt::Key_F8, Key::F8 },
        pair { Qt::Key_F9, Key::F9 },
        pair { Qt::Key_F10, Key::F10 },
        pair { Qt::Key_F11, Key::F11 },
        pair { Qt::Key_F12, Key::F12 },
        pair { Qt::Key_F13, Key::F13 },
        pair { Qt::Key_F14, Key::F14 },
        pair { Qt::Key_F15, Key::F15 },
        pair { Qt::Key_F16, Key::F16 },
        pair { Qt::Key_F17, Key::F17 },
        pair { Qt::Key_F18, Key::F18 },
        pair { Qt::Key_F19, Key::F19 },
        pair { Qt::Key_F20, Key::F20 },
    }; // }}}

    static auto constexpr charMappings = array {
        // {{{
        pair { Qt::Key_Return, '\r' },      pair { Qt::Key_AsciiCircum, '^' },
        pair { Qt::Key_AsciiTilde, '~' },   pair { Qt::Key_Backslash, '\\' },
        pair { Qt::Key_Bar, '|' },          pair { Qt::Key_BraceLeft, '{' },
        pair { Qt::Key_BraceRight, '}' },   pair { Qt::Key_BracketLeft, '[' },
        pair { Qt::Key_BracketRight, ']' }, pair { Qt::Key_QuoteLeft, '`' },
        pair { Qt::Key_Underscore, '_' },
    }; // }}}

    auto const modifiers = makeModifier(_event->modifiers());
    auto const key = _event->key();

    if (auto i = find_if(begin(keyMappings),
                         end(keyMappings),
                         [_event](auto const& x) { return x.first == _event->key(); });
        i != end(keyMappings))
    {
        _session.sendKeyPressEvent(i->second, modifiers, now);
        return true;
    }

    if (auto i = find_if(begin(charMappings),
                         end(charMappings),
                         [_event](auto const& x) { return x.first == _event->key(); });
        i != end(charMappings))
    {
        _session.sendCharPressEvent(static_cast<char32_t>(i->second), modifiers, now);
        return true;
    }

    if (key == Qt::Key_Backtab)
    {
        _session.sendCharPressEvent(U'\t', modifiers.with(Modifier::Shift), now);
        return true;
    }

    if (modifiers.control() && key >= 0x20 && key < 0x80)
    {
        _session.sendCharPressEvent(static_cast<char32_t>(key), modifiers, now);
        return true;
    }

    if (!_event->text().isEmpty())
    {
#if defined(__APPLE__)
        // On OS/X the Alt-modifier does not seem to be passed to the terminal apps
        // but rather remapped to whatever OS/X is mapping them to.
        for (char32_t const ch: _event->text().toUcs4())
            _session.sendCharPressEvent(ch, modifiers.without(Modifier::Alt), now);
#else
        for (char32_t const ch: _event->text().toUcs4())
            _session.sendCharPressEvent(ch, modifiers, now);
#endif

        return true;
    }

    switch (key)
    {
        case Qt::Key_BraceLeft: _session.sendCharPressEvent(L'[', modifiers, now); return true;
        case Qt::Key_Equal: _session.sendCharPressEvent(L'=', modifiers, now); return true;
        case Qt::Key_BraceRight: _session.sendCharPressEvent(L']', modifiers, now); return true;
    }

    errorlog()("Input not handled for mods {} key {}", modifiers, key);
    return false;
}

void sendWheelEvent(QWheelEvent* _event, TerminalSession& _session)
{
    auto const yDelta = mouseWheelDelta(_event);

    if (yDelta)
    {
        auto const modifier = makeModifier(_event->modifiers());
        auto const button = yDelta > 0 ? terminal::MouseButton::WheelUp : terminal::MouseButton::WheelDown;
        auto const pixelPosition = makeMousePixelPosition(_event, _session.contentScale());

        _session.sendMousePressEvent(modifier, button, pixelPosition, steady_clock::now());
    }
}

void sendMousePressEvent(QMouseEvent* _event, TerminalSession& _session)
{
    _session.sendMousePressEvent(makeModifier(_event->modifiers()),
                                 makeMouseButton(_event->button()),
                                 makeMousePixelPosition(_event, _session.contentScale()),
                                 steady_clock::now());
    _event->accept();
}

void sendMouseReleaseEvent(QMouseEvent* _event, TerminalSession& _session)
{
    _session.sendMouseReleaseEvent(makeModifier(_event->modifiers()),
                                   makeMouseButton(_event->button()),
                                   makeMousePixelPosition(_event, _session.contentScale()),
                                   steady_clock::now());
    _event->accept();
}

void sendMouseMoveEvent(QMouseEvent* _event, TerminalSession& _session)
{
    _session.sendMouseMoveEvent(makeModifier(_event->modifiers()),
                                makeMouseCellLocation(_event, _session),
                                makeMousePixelPosition(_event, _session.contentScale()),
                                steady_clock::now());
}

void spawnNewTerminal(string const& _programPath,
                      string const& _configPath,
                      string const& _profileName,
                      string const& _cwdUrl)
{
    auto const wd = [&]() -> QString {
        auto const url = QUrl(QString::fromUtf8(_cwdUrl.c_str()));

        if (url.host().isEmpty())
            return url.path();

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
            SessionLog()("Permission for {} allowed by configuration.", _topicText);
            return true;
        case config::Permission::Deny:
            SessionLog()("Permission for {} denied by configuration.", _topicText);
            return false;
        case config::Permission::Ask: break;
    }

    // Did we remember a last interactive question?
    if (auto const i = _cache.find(string(_topicText)); i != _cache.end())
        return i->second;

    SessionLog()("Permission for {} requires asking user.", _topicText);

    auto const reply =
        QMessageBox::question(_parent,
                              fmt::format("{} requested", _topicText).c_str(),
                              QString::fromStdString(fmt::format(
                                  "The application has requested for {}. Do you allow this?", _topicText)),
                              QMessageBox::StandardButton::Yes | QMessageBox::StandardButton::YesToAll
                                  | QMessageBox::StandardButton::No | QMessageBox::StandardButton::NoToAll,
                              QMessageBox::StandardButton::NoButton);

    switch (reply)
    {
        case QMessageBox::StandardButton::NoToAll: _cache[string(_topicText)] = false; break;
        case QMessageBox::StandardButton::YesToAll: _cache[string(_topicText)] = true; [[fallthrough]];
        case QMessageBox::StandardButton::Yes: return true;
        default: break;
    }

    return false;
}

terminal::FontDef getFontDefinition(terminal::renderer::Renderer& _renderer)
{
    auto const fontByStyle = [&](text::font_weight _weight,
                                 text::font_slant _slant) -> text::font_description const& {
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
    auto const nameOfStyledFont = [&](text::font_weight _weight, text::font_slant _slant) -> string {
        auto const& regularFont = _renderer.fontDescriptions().regular;
        auto const& styledFont = fontByStyle(_weight, _slant);
        if (styledFont.familyName == regularFont.familyName)
            return "auto";
        else
            return styledFont.toPattern();
    };
    return { _renderer.fontDescriptions().size.pt,
             _renderer.fontDescriptions().regular.familyName,
             nameOfStyledFont(text::font_weight::bold, text::font_slant::normal),
             nameOfStyledFont(text::font_weight::normal, text::font_slant::italic),
             nameOfStyledFont(text::font_weight::bold, text::font_slant::italic),
             _renderer.fontDescriptions().emoji.toPattern() };
}

terminal::renderer::PageMargin computeMargin(ImageSize _cellSize,
                                             PageSize _charCells,
                                             ImageSize _pixels) noexcept
{
    auto const usedHeight = unbox<int>(_charCells.lines) * unbox<int>(_cellSize.height);
    auto const freeHeight = unbox<int>(_pixels.height) - usedHeight;
    auto const bottomMargin = freeHeight;

    // auto const usedWidth = _charCells.columns * regularFont_.maxAdvance();
    // auto const freeWidth = _pixels.width - usedWidth;
    auto constexpr leftMargin = 0;

    return { leftMargin, bottomMargin };
}

terminal::renderer::FontDescriptions sanitizeFontDescription(terminal::renderer::FontDescriptions _fonts,
                                                             text::DPI _dpi)
{
    if (_fonts.dpi.x <= 0 || _fonts.dpi.y <= 0)
        _fonts.dpi = _dpi;
    if (std::fabs(_fonts.size.pt) <= std::numeric_limits<double>::epsilon())
        _fonts.size.pt = 12;
    return _fonts;
}

bool applyFontDescription(ImageSize _cellSize,
                          PageSize _pageSize,
                          ImageSize _pixelSize,
                          text::DPI _dpi,
                          terminal::renderer::Renderer& _renderer,
                          terminal::renderer::FontDescriptions _fontDescriptions)
{
    if (_renderer.fontDescriptions() == _fontDescriptions)
        return false;

    auto const windowMargin = computeMargin(_cellSize, _pageSize, _pixelSize);

    _renderer.setFonts(sanitizeFontDescription(std::move(_fontDescriptions), _dpi));
    _renderer.setMargin(windowMargin);
    _renderer.updateFontMetrics();

    return true;
}

void applyResize(terminal::ImageSize _newPixelSize,
                 TerminalSession& _session,
                 terminal::renderer::Renderer& _renderer)
{
    if (*_newPixelSize.width == 0 || *_newPixelSize.height == 0)
        return;

    auto const newPageSize = pageSizeForPixels(_newPixelSize, _renderer.gridMetrics().cellSize);
    terminal::Terminal& terminal = _session.terminal();
    terminal::ImageSize cellSize = _renderer.gridMetrics().cellSize;

    _renderer.renderTarget().setRenderSize(_newPixelSize);
    _renderer.setPageSize(newPageSize);
    _renderer.setMargin(computeMargin(_renderer.gridMetrics().cellSize, newPageSize, _newPixelSize));

    if (newPageSize == terminal.pageSize())
        return;

    auto const viewSize = cellSize * newPageSize;
    terminal.resizeScreen(newPageSize, viewSize);
    terminal.clearSelection();
}

} // namespace contour
