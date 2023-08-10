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
#include <contour/TerminalSession.h>
#include <contour/display/TerminalWidget.h>
#include <contour/helper.h>

#include <vtbackend/Terminal.h>

#include <vtrasterizer/Renderer.h>

#include <QtCore/QProcess>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QUrl>
#include <QtGui/QGuiApplication>
#include <QtNetwork/QHostInfo>
#include <QtQml/QQmlApplicationEngine>
#include <QtQml/QQmlEngine>
#include <QtQuick/QQuickView>

#include <algorithm>
#include <array>
#include <mutex>

#include "vtbackend/primitives.h"

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

using terminal::height;
using terminal::image_size;
using terminal::PageSize;
using terminal::pixel_coordinate;
using terminal::width;

namespace contour
{

namespace
{
    terminal::cell_location makeMouseCellLocation(int x, int y, TerminalSession const& session) noexcept
    {
        auto constexpr MarginTop = 0;
        auto constexpr MarginLeft = 0;

        auto const pageSize = session.terminal().totalPageSize();
        auto const cellSize = session.display()->cellSize();
        auto const dpr = session.contentScale();

        auto const sx = int(double(x) * dpr);
        auto const sy = int(double(y) * dpr);

        auto const row = terminal::line_offset(
            clamp((sy - MarginTop) / cellSize.height.as<int>(), 0, *pageSize.lines - 1));

        auto const col = terminal::column_offset(
            clamp((sx - MarginLeft) / cellSize.width.as<int>(), 0, *pageSize.columns - 1));

        return { row, col };
    }

    pixel_coordinate makeMousePixelPosition(QHoverEvent* _event, double dpr) noexcept
    {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        auto const position = _event->position();
#else
        auto const position = _event->pos();
#endif
        // TODO: apply margin once supported
        return pixel_coordinate { { int(double(position.x()) * dpr) }, { int(double(position.y()) * dpr) } };
    }

    pixel_coordinate makeMousePixelPosition(QMouseEvent* _event, double dpr) noexcept
    {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        auto const position = _event->position();
#else
        auto const position = QPointF { static_cast<qreal>(_event->x()), static_cast<qreal>(_event->y()) };
#endif
        // TODO: apply margin once supported
        return pixel_coordinate { { int(double(position.x()) * dpr) }, { int(double(position.y()) * dpr) } };
    }

    pixel_coordinate makeMousePixelPosition(QWheelEvent* _event, double dpr) noexcept
    {
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
        auto const position = _event->position();
#else
        auto const position = _event->posF();
#endif
        // TODO: apply margin once supported
        return pixel_coordinate { { int(double(position.x()) * dpr) }, { int(double(position.y()) * dpr) } };
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

bool sendKeyEvent(QKeyEvent* _event, TerminalSession& _session)
{
    using terminal::key;
    using terminal::modifier;

    auto const now = steady_clock::now();

    static auto constexpr keyMappings = array {
        // {{{
        pair { Qt::Key_Insert, key::Insert },
        pair { Qt::Key_Delete, key::Delete },
        pair { Qt::Key_Right, key::RightArrow },
        pair { Qt::Key_Left, key::LeftArrow },
        pair { Qt::Key_Down, key::DownArrow },
        pair { Qt::Key_Up, key::UpArrow },
        pair { Qt::Key_PageDown, key::PageDown },
        pair { Qt::Key_PageUp, key::PageUp },
        pair { Qt::Key_Home, key::Home },
        pair { Qt::Key_End, key::End },
        pair { Qt::Key_F1, key::F1 },
        pair { Qt::Key_F2, key::F2 },
        pair { Qt::Key_F3, key::F3 },
        pair { Qt::Key_F4, key::F4 },
        pair { Qt::Key_F5, key::F5 },
        pair { Qt::Key_F6, key::F6 },
        pair { Qt::Key_F7, key::F7 },
        pair { Qt::Key_F8, key::F8 },
        pair { Qt::Key_F9, key::F9 },
        pair { Qt::Key_F10, key::F10 },
        pair { Qt::Key_F11, key::F11 },
        pair { Qt::Key_F12, key::F12 },
        pair { Qt::Key_F13, key::F13 },
        pair { Qt::Key_F14, key::F14 },
        pair { Qt::Key_F15, key::F15 },
        pair { Qt::Key_F16, key::F16 },
        pair { Qt::Key_F17, key::F17 },
        pair { Qt::Key_F18, key::F18 },
        pair { Qt::Key_F19, key::F19 },
        pair { Qt::Key_F20, key::F20 },
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
        _session.sendCharPressEvent(U'\t', modifiers.with(modifier::Shift), now);
        return true;
    }

    if (modifiers.control() && key >= 0x20 && key < 0x80)
    {
        _session.sendCharPressEvent(static_cast<char32_t>(key), modifiers, now);
        return true;
    }

    switch (key)
    {
        case Qt::Key_BraceLeft: _session.sendCharPressEvent(L'[', modifiers, now); return true;
        case Qt::Key_Equal: _session.sendCharPressEvent(L'=', modifiers, now); return true;
        case Qt::Key_BraceRight: _session.sendCharPressEvent(L']', modifiers, now); return true;
        case Qt::Key_Backspace: _session.sendCharPressEvent(0x08, modifiers, now); return true;
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

    InputLog()("Input not handled for mods {} key 0x{:X}", modifiers, key);
    return false;
}

void sendWheelEvent(QWheelEvent* _event, TerminalSession& _session)
{
    auto const yDelta = mouseWheelDelta(_event);

    if (yDelta)
    {
        auto const modifier = makeModifier(_event->modifiers());
        auto const button = yDelta > 0 ? terminal::mouse_button::WheelUp : terminal::mouse_button::WheelDown;
        auto const pixelPosition = makeMousePixelPosition(_event, _session.contentScale());

        _session.sendMousePressEvent(modifier, button, pixelPosition);
        _event->accept();
    }
}

void sendMousePressEvent(QMouseEvent* _event, TerminalSession& _session)
{
    _session.sendMousePressEvent(makeModifier(_event->modifiers()),
                                 makeMouseButton(_event->button()),
                                 makeMousePixelPosition(_event, _session.contentScale()));
    _event->accept();
}

void sendMouseReleaseEvent(QMouseEvent* _event, TerminalSession& _session)
{
    _session.sendMouseReleaseEvent(makeModifier(_event->modifiers()),
                                   makeMouseButton(_event->button()),
                                   makeMousePixelPosition(_event, _session.contentScale()));
    _event->accept();
}

void sendMouseMoveEvent(QMouseEvent* _event, TerminalSession& _session)
{
    _session.sendMouseMoveEvent(makeModifier(_event->modifiers()),
                                makeMouseCellLocation(_event->pos().x(), _event->pos().y(), _session),
                                makeMousePixelPosition(_event, _session.contentScale()));
    _event->accept();
}

void sendMouseMoveEvent(QHoverEvent* _event, TerminalSession& _session)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    auto const position = _event->position().toPoint();
#else
    auto const position = _event->pos();
#endif
    _session.sendMouseMoveEvent(makeModifier(_event->modifiers()),
                                makeMouseCellLocation(position.x(), position.y(), _session),
                                makeMousePixelPosition(_event, _session.contentScale()));
    _event->accept();
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

terminal::FontDef getFontDefinition(terminal::rasterizer::Renderer& _renderer)
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

terminal::rasterizer::PageMargin computeMargin(image_size _cellSize,
                                               PageSize _charCells,
                                               image_size _pixels) noexcept
{
    auto const usedHeight = unbox<int>(_charCells.lines) * unbox<int>(_cellSize.height);
    auto const freeHeight = unbox<int>(_pixels.height) - usedHeight;
    auto const bottomMargin = freeHeight;
    auto const topMargin = 0;

    // auto const usedWidth = _charCells.columns * regularFont_.maxAdvance();
    // auto const freeWidth = _pixels.width - usedWidth;
    auto constexpr leftMargin = 0;

    return { leftMargin, topMargin, bottomMargin };
}

terminal::rasterizer::FontDescriptions sanitizeFontDescription(terminal::rasterizer::FontDescriptions _fonts,
                                                               text::DPI _dpi)
{
    if (_fonts.dpi.x <= 0 || _fonts.dpi.y <= 0)
        _fonts.dpi = _dpi;
    if (std::fabs(_fonts.size.pt) <= std::numeric_limits<double>::epsilon())
        _fonts.size.pt = 12;
    return _fonts;
}

bool applyFontDescription(image_size _cellSize,
                          PageSize _pageSize,
                          image_size _pixelSize,
                          text::DPI _dpi,
                          terminal::rasterizer::Renderer& _renderer,
                          terminal::rasterizer::FontDescriptions _fontDescriptions)
{
    if (_renderer.fontDescriptions() == _fontDescriptions)
        return false;

    auto const windowMargin = computeMargin(_cellSize, _pageSize, _pixelSize);

    _renderer.setFonts(sanitizeFontDescription(std::move(_fontDescriptions), _dpi));
    _renderer.setMargin(windowMargin);
    _renderer.updateFontMetrics();

    return true;
}

void applyResize(terminal::image_size _newPixelSize,
                 TerminalSession& _session,
                 terminal::rasterizer::Renderer& _renderer)
{
    if (*_newPixelSize.width == 0 || *_newPixelSize.height == 0)
        return;

    auto const newPageSize = pageSizeForPixels(_newPixelSize, _renderer.gridMetrics().cellSize);
    terminal::Terminal& terminal = _session.terminal();
    terminal::image_size cellSize = _renderer.gridMetrics().cellSize;

    Require(_renderer.hasRenderTarget());
    _renderer.renderTarget().setRenderSize(_newPixelSize);
    _renderer.setPageSize(newPageSize);
    _renderer.setMargin(computeMargin(_renderer.gridMetrics().cellSize, newPageSize, _newPixelSize));

    auto const viewSize = cellSize * newPageSize;
    DisplayLog()("Applying resize: {} (new pixel size) {} (view size)", _newPixelSize, viewSize);

    if (newPageSize == terminal.pageSize())
        return;

    terminal.resizeScreen(newPageSize, viewSize);
    terminal.clearSelection();
}

} // namespace contour
