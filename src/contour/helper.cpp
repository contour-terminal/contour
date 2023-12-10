// SPDX-License-Identifier: Apache-2.0
#include <contour/TerminalSession.h>
#include <contour/display/TerminalDisplay.h>
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

using crispy::point;
using crispy::size;
using crispy::Zero;

using vtbackend::Height;
using vtbackend::ImageSize;
using vtbackend::PageSize;
using vtbackend::PixelCoordinate;
using vtbackend::Width;

namespace contour
{

namespace
{
    vtbackend::CellLocation makeMouseCellLocation(int x, int y, TerminalSession const& session) noexcept
    {
        auto constexpr MarginTop = 0;
        auto constexpr MarginLeft = 0;

        auto const pageSize = session.terminal().totalPageSize();
        auto const cellSize = session.display()->cellSize();
        auto const dpr = session.contentScale();

        auto const sx = int(double(x) * dpr);
        auto const sy = int(double(y) * dpr);

        auto const row = vtbackend::LineOffset(
            clamp((sy - MarginTop) / cellSize.height.as<int>(), 0, *pageSize.lines - 1));

        auto const col = vtbackend::ColumnOffset(
            clamp((sx - MarginLeft) / cellSize.width.as<int>(), 0, *pageSize.columns - 1));

        return { row, col };
    }

    PixelCoordinate makeMousePixelPosition(QHoverEvent* event, double dpr) noexcept
    {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        auto const position = event->position();
#else
        auto const position = event->pos();
#endif
        // TODO: apply margin once supported
        return PixelCoordinate { PixelCoordinate::X { int(double(position.x()) * dpr) },
                                 PixelCoordinate::Y { int(double(position.y()) * dpr) } };
    }

    PixelCoordinate makeMousePixelPosition(QMouseEvent* event, double dpr) noexcept
    {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        auto const position = event->position();
#else
        auto const position = QPointF { static_cast<qreal>(event->x()), static_cast<qreal>(event->y()) };
#endif
        // TODO: apply margin once supported
        return PixelCoordinate { PixelCoordinate::X { int(double(position.x()) * dpr) },
                                 PixelCoordinate::Y { int(double(position.y()) * dpr) } };
    }

    PixelCoordinate makeMousePixelPosition(QWheelEvent* event, double dpr) noexcept
    {
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
        auto const position = event->position();
#else
        auto const position = event->posF();
#endif
        // TODO: apply margin once supported
        return PixelCoordinate { PixelCoordinate::X { int(double(position.x()) * dpr) },
                                 PixelCoordinate::Y { int(double(position.y()) * dpr) } };
    }

    int mouseWheelDelta(QWheelEvent* event) noexcept
    {
#if 1
        // FIXME: Temporarily addressing a really bad Qt implementation detail
        // as tracked here:
        // https://github.com/contour-terminal/contour/issues/394
        if (event->pixelDelta().y())
            return event->pixelDelta().y();
        if (event->angleDelta().y())
            return event->angleDelta().y();

        return 0;
#else
        // switch (event->orientation())
        // {
        //     case Qt::Orientation::Horizontal:
        //         return event->pixelDelta().x() ? event->pixelDelta().x()
        //                                         : event->angleDelta().x();
        //     case Qt::Orientation::Vertical:
        //         return event->pixelDelta().y() ? event->pixelDelta().y()
        //                                         : event->angleDelta().y();
        // }
        return event->angleDelta().y();
#endif
    }

} // namespace

bool sendKeyEvent(QKeyEvent* event, vtbackend::KeyboardEventType eventType, TerminalSession& session)
{
    qDebug() << "sendKeyEvent: " << event->key() << " " << event->text() << " " << event->modifiers();

    using vtbackend::Key;
    using vtbackend::Modifier;

    auto const now = steady_clock::now();

    static auto constexpr KeyMappings = array {
        // {{{
        pair { Qt::Key_Escape, Key::Escape },
        pair { Qt::Key_Enter, Key::Enter },
        pair { Qt::Key_Return, Key::Enter },
        pair { Qt::Key_Tab, Key::Tab },
        pair { Qt::Key_Backspace, Key::Backspace },

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
        pair { Qt::Key_F21, Key::F21 },
        pair { Qt::Key_F22, Key::F22 },
        pair { Qt::Key_F23, Key::F23 },
        pair { Qt::Key_F24, Key::F24 },
        pair { Qt::Key_F25, Key::F25 },
        pair { Qt::Key_F26, Key::F26 },
        pair { Qt::Key_F27, Key::F27 },
        pair { Qt::Key_F28, Key::F28 },
        pair { Qt::Key_F29, Key::F29 },
        pair { Qt::Key_F30, Key::F30 },
        pair { Qt::Key_F31, Key::F31 },
        pair { Qt::Key_F32, Key::F32 },
        pair { Qt::Key_F33, Key::F33 },
        pair { Qt::Key_F34, Key::F34 },
        pair { Qt::Key_F35, Key::F35 },

        pair { Qt::Key_MediaPlay, Key::MediaPlay },
        pair { Qt::Key_MediaStop, Key::MediaStop },
        pair { Qt::Key_MediaPrevious, Key::MediaPrevious },
        pair { Qt::Key_MediaNext, Key::MediaNext },
        pair { Qt::Key_MediaPause, Key::MediaPause },
        pair { Qt::Key_MediaTogglePlayPause, Key::MediaTogglePlayPause },

        pair { Qt::Key_VolumeUp, Key::VolumeUp },
        pair { Qt::Key_VolumeDown, Key::VolumeDown },
        pair { Qt::Key_VolumeMute, Key::VolumeMute },

        pair { Qt::Key_Control, Key::LeftControl }, // NB: Qt cannot distinguish between left and right
        pair { Qt::Key_Alt, Key::LeftAlt },         // NB: Qt cannot distinguish between left and right
        pair { Qt::Key_Meta, Key::LeftMeta },       // NB: Qt cannot distinguish between left and right
        pair { Qt::Key_Super_L, Key::LeftSuper },
        pair { Qt::Key_Super_R, Key::RightSuper },
        pair { Qt::Key_Hyper_L, Key::LeftHyper },
        pair { Qt::Key_Hyper_R, Key::RightHyper },

        pair { Qt::Key_CapsLock, Key::CapsLock },
        pair { Qt::Key_ScrollLock, Key::ScrollLock },
        pair { Qt::Key_NumLock, Key::NumLock },
        pair { Qt::Key_Print, Key::PrintScreen },
        pair { Qt::Key_Pause, Key::Pause },
        pair { Qt::Key_Menu, Key::Menu },
    }; // }}}

    static auto constexpr CharMappings = array {
        // {{{
        // pair { Qt::Key_Return, '\r' },
        pair { Qt::Key_AsciiCircum, '^' }, pair { Qt::Key_AsciiTilde, '~' },
        pair { Qt::Key_Backslash, '\\' },  pair { Qt::Key_Bar, '|' },
        pair { Qt::Key_BraceLeft, '{' },   pair { Qt::Key_BraceRight, '}' },
        pair { Qt::Key_BracketLeft, '[' }, pair { Qt::Key_BracketRight, ']' },
        pair { Qt::Key_QuoteLeft, '`' },   pair { Qt::Key_Underscore, '_' },
    }; // }}}

    auto const modifiers = makeModifiers(event->modifiers());
    auto const key = event->key();

    if (event->modifiers().testFlag(Qt::KeypadModifier))
    {
        std::optional<Key> mappedKey = nullopt;
        switch (key)
        {
            case Qt::Key_0: mappedKey = Key::Numpad_0; break;
            case Qt::Key_1: mappedKey = Key::Numpad_1; break;
            case Qt::Key_2: mappedKey = Key::Numpad_2; break;
            case Qt::Key_3: mappedKey = Key::Numpad_3; break;
            case Qt::Key_4: mappedKey = Key::Numpad_4; break;
            case Qt::Key_5: mappedKey = Key::Numpad_5; break;
            case Qt::Key_6: mappedKey = Key::Numpad_6; break;
            case Qt::Key_7: mappedKey = Key::Numpad_7; break;
            case Qt::Key_8: mappedKey = Key::Numpad_8; break;
            case Qt::Key_9: mappedKey = Key::Numpad_9; break;
            case Qt::Key_Asterisk: mappedKey = Key::Numpad_Multiply; break;
            case Qt::Key_Plus: mappedKey = Key::Numpad_Add; break;
            case Qt::Key_Minus: mappedKey = Key::Numpad_Subtract; break;
            case Qt::Key_Period: mappedKey = Key::Numpad_Decimal; break;
            case Qt::Key_Slash: mappedKey = Key::Numpad_Divide; break;
            case Qt::Key_Enter: mappedKey = Key::Numpad_Enter; break;
            default: break;
        }
        if (mappedKey)
        {
            session.sendKeyEvent(*mappedKey, modifiers, eventType, now);
            return true;
        }
    }

    // NOLINTNEXTLINE(readability-qualified-auto)
    if (auto const i = find_if(
            begin(KeyMappings), end(KeyMappings), [event](auto const& x) { return x.first == event->key(); });
        i != end(KeyMappings))
    {
        session.sendKeyEvent(i->second, modifiers, eventType, now);
        event->accept();
        return true;
    }

    auto const physicalKey = event->nativeVirtualKey();
    // NOLINTNEXTLINE(readability-qualified-auto)
    if (auto const i = find_if(begin(CharMappings),
                               end(CharMappings),
                               [event](auto const& x) { return x.first == event->key(); });
        i != end(CharMappings))
    {
        session.sendCharEvent(static_cast<char32_t>(i->second), physicalKey, modifiers, eventType, now);
        event->accept();
        return true;
    }

    if (key == Qt::Key_Backtab)
    {
        session.sendCharEvent(U'\t', physicalKey, modifiers.with(Modifier::Shift), eventType, now);
        event->accept();
        return true;
    }

#if defined(__apple__)
    if (0x20 <= key && key < 0x80 && (modifiers.alt() && session.profile().optionKeyAsAlt))
    {
        auto const ch = static_cast<char32_t>(modifiers.shift() ? std::toupper(key) : std::tolower(key));
        session.sendCharEvent(ch, physicalKey, modifiers, eventType, now);
        event->accept();
        return true;
    }
#endif

    if (0x20 <= key && key < 0x80 && (modifiers & Modifier::Control))
    {
        session.sendCharEvent(static_cast<char32_t>(key), physicalKey, modifiers, eventType, now);
        event->accept();
        return true;
    }

    switch (key)
    {
            // clang-format off
        case Qt::Key_BraceLeft: session.sendCharEvent(L'[', physicalKey, modifiers, eventType, now); return true;
        case Qt::Key_Equal: session.sendCharEvent(L'=', physicalKey, modifiers, eventType, now); return true;
        case Qt::Key_BraceRight: session.sendCharEvent(L']', physicalKey, modifiers, eventType, now); return true;
        case Qt::Key_Backspace: session.sendCharEvent(0x08, physicalKey, modifiers, eventType, now); return true;
        default: break;
            // clang-format on
    }

    if (!event->text().isEmpty())
    {
        auto const codepoints = event->text().toUcs4();
        assert(codepoints.size() == 1);
#if defined(__APPLE__)
        // On OS/X the Alt-modifier does not seem to be passed to the terminal apps
        // but rather remapped to whatever OS/X is mapping them to.
        for (char32_t const ch: codepoints)
            session.sendCharEvent(ch, physicalKey, modifiers.without(Modifier::Alt), eventType, now);
#else
        for (char32_t const ch: codepoints)
            session.sendCharEvent(ch, physicalKey, modifiers, eventType, now);
#endif
        event->accept();
        return true;
    }

    inputLog()("Input not handled for mods {} key 0x{:X}", modifiers, key);
    return false;
}

void sendWheelEvent(QWheelEvent* event, TerminalSession& session)
{
    using VTMouseButton = vtbackend::MouseButton;

    auto const xDelta = event->angleDelta().x() > 0 ? 1 : event->angleDelta().x() < 0 ? -1 : 0;
    if (xDelta)
    {
        auto const modifier = makeModifiers(event->modifiers());
        auto const button = xDelta > 0 ? VTMouseButton::WheelRight : VTMouseButton::WheelLeft;
        auto const pixelPosition = makeMousePixelPosition(event, session.contentScale());

        session.sendMousePressEvent(modifier, button, pixelPosition);
        event->accept();
    }

    auto const yDelta = mouseWheelDelta(event);
    if (yDelta)
    {
        auto const modifier = makeModifiers(event->modifiers());
        auto const button = yDelta > 0 ? VTMouseButton::WheelUp : VTMouseButton::WheelDown;
        auto const pixelPosition = makeMousePixelPosition(event, session.contentScale());

        session.sendMousePressEvent(modifier, button, pixelPosition);
        event->accept();
    }
}

void sendMousePressEvent(QMouseEvent* event, TerminalSession& session)
{
    session.sendMousePressEvent(makeModifiers(event->modifiers()),
                                makeMouseButton(event->button()),
                                makeMousePixelPosition(event, session.contentScale()));
    event->accept();
}

void sendMouseReleaseEvent(QMouseEvent* event, TerminalSession& session)
{
    session.sendMouseReleaseEvent(makeModifiers(event->modifiers()),
                                  makeMouseButton(event->button()),
                                  makeMousePixelPosition(event, session.contentScale()));
    event->accept();
}

void sendMouseMoveEvent(QMouseEvent* event, TerminalSession& session)
{
    session.sendMouseMoveEvent(makeModifiers(event->modifiers()),
                               makeMouseCellLocation(event->pos().x(), event->pos().y(), session),
                               makeMousePixelPosition(event, session.contentScale()));
    event->accept();
}

void sendMouseMoveEvent(QHoverEvent* event, TerminalSession& session)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    auto const position = event->position().toPoint();
#else
    auto const position = event->pos();
#endif
    session.sendMouseMoveEvent(makeModifiers(event->modifiers()),
                               makeMouseCellLocation(position.x(), position.y(), session),
                               makeMousePixelPosition(event, session.contentScale()));
    event->accept();
}

void spawnNewTerminal(string const& programPath,
                      string const& configPath,
                      string const& profileName,
                      string const& cwdUrl)
{
    auto const wd = [&]() -> QString {
        auto const url = QUrl(QString::fromUtf8(cwdUrl.c_str()));

        if (url.host().isEmpty())
            return url.path();

        if (url.host() == QHostInfo::localHostName())
            return url.path();
        else
            return QString();
    }();

    QString const program = QString::fromUtf8(programPath.c_str());
    QStringList args;

    if (!configPath.empty())
        args << "config" << QString::fromStdString(configPath);

    if (!profileName.empty())
        args << "profile" << QString::fromStdString(profileName);

    if (!wd.isEmpty())
        args << "working-directory" << wd;

    QProcess::startDetached(program, args);
}

vtbackend::FontDef getFontDefinition(vtrasterizer::Renderer& renderer)
{
    auto const fontByStyle = [&](text::font_weight weight,
                                 text::font_slant slant) -> text::font_description const& {
        auto const bold = weight != text::font_weight::normal;
        auto const italic = slant != text::font_slant::normal;
        if (bold && italic)
            return renderer.fontDescriptions().boldItalic;
        else if (bold)
            return renderer.fontDescriptions().bold;
        else if (italic)
            return renderer.fontDescriptions().italic;
        else
            return renderer.fontDescriptions().regular;
    };
    auto const nameOfStyledFont = [&](text::font_weight weight, text::font_slant slant) -> string {
        auto const& regularFont = renderer.fontDescriptions().regular;
        auto const& styledFont = fontByStyle(weight, slant);
        if (styledFont.familyName == regularFont.familyName)
            return "auto";
        else
            return styledFont.toPattern();
    };
    return { renderer.fontDescriptions().size.pt,
             renderer.fontDescriptions().regular.familyName,
             nameOfStyledFont(text::font_weight::bold, text::font_slant::normal),
             nameOfStyledFont(text::font_weight::normal, text::font_slant::italic),
             nameOfStyledFont(text::font_weight::bold, text::font_slant::italic),
             renderer.fontDescriptions().emoji.toPattern() };
}

vtrasterizer::PageMargin computeMargin(ImageSize cellSize, PageSize charCells, ImageSize pixels) noexcept
{
    auto const usedHeight = unbox(charCells.lines) * unbox(cellSize.height);
    auto const freeHeight = unbox(pixels.height) - usedHeight;
    auto const bottomMargin = freeHeight;
    auto const topMargin = 0;

    // auto const usedWidth = charCells.columns * regularFont_.maxAdvance();
    // auto const freeWidth = pixels.width - usedWidth;
    auto constexpr LeftMargin = 0;

    return { LeftMargin, topMargin, static_cast<int>(bottomMargin) };
}

vtrasterizer::FontDescriptions sanitizeFontDescription(vtrasterizer::FontDescriptions fonts, text::DPI dpi)
{
    if (fonts.dpi.x <= 0 || fonts.dpi.y <= 0)
        fonts.dpi = dpi;
    if (std::fabs(fonts.size.pt) <= std::numeric_limits<double>::epsilon())
        fonts.size.pt = 12;
    return fonts;
}

bool applyFontDescription(ImageSize cellSize,
                          PageSize pageSize,
                          ImageSize pixelSize,
                          text::DPI dpi,
                          vtrasterizer::Renderer& renderer,
                          vtrasterizer::FontDescriptions fontDescriptions)
{
    if (renderer.fontDescriptions() == fontDescriptions)
        return false;

    auto const windowMargin = computeMargin(cellSize, pageSize, pixelSize);

    renderer.setFonts(sanitizeFontDescription(std::move(fontDescriptions), dpi));
    renderer.setMargin(windowMargin);
    renderer.updateFontMetrics();

    return true;
}

void applyResize(vtbackend::ImageSize newPixelSize,
                 TerminalSession& session,
                 vtrasterizer::Renderer& renderer)
{
    if (*newPixelSize.width == 0 || *newPixelSize.height == 0)
        return;

    auto const oldPageSize = session.terminal().pageSize();
    auto const newPageSize = pageSizeForPixels(newPixelSize, renderer.gridMetrics().cellSize);
    vtbackend::Terminal& terminal = session.terminal();
    vtbackend::ImageSize cellSize = renderer.gridMetrics().cellSize;

    Require(renderer.hasRenderTarget());
    renderer.renderTarget().setRenderSize(newPixelSize);
    renderer.setPageSize(newPageSize);
    renderer.setMargin(computeMargin(renderer.gridMetrics().cellSize, newPageSize, newPixelSize));

    if (oldPageSize.lines != newPageSize.lines)
        emit session.lineCountChanged(newPageSize.lines.as<int>());

    if (oldPageSize.columns != newPageSize.columns)
        emit session.columnsCountChanged(newPageSize.columns.as<int>());

    auto const viewSize = cellSize * newPageSize;
    displayLog()("Applying resize {}/{} pixels and {} -> {} cells.",
                 viewSize,
                 newPixelSize,
                 terminal.pageSize(),
                 newPageSize);

    auto const l = scoped_lock { terminal };

    if (newPageSize == terminal.pageSize())
    {
        displayLog()("No resize necessary. New size is same as old size of {}.", newPageSize);
        return;
    }

    terminal.resizeScreen(newPageSize, viewSize);
    terminal.clearSelection();
}

} // namespace contour
