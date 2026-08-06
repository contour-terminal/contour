// SPDX-License-Identifier: Apache-2.0
#include <contour/display/TerminalDisplay.hpp>
#include <contour/input/KeyMapping.hpp>
#include <contour/input/Logging.hpp>
#include <contour/input/MouseMapping.hpp>
#include <contour/session/Logging.hpp>
#include <contour/session/SessionInput.hpp>
#include <contour/session/TerminalSession.hpp>

#include <vtbackend/Terminal.hpp>

#include <vtrasterizer/Renderer.hpp>

#include <QtGui/QGuiApplication>

#include <algorithm>
#include <array>

using std::array;
using std::clamp;
using std::nullopt;
using std::optional;
using std::pair;
using std::u32string;
using std::chrono::steady_clock;

using crispy::point;

using vtbackend::PixelCoordinate;

namespace contour::session
{

namespace
{
    vtbackend::CellLocation makeMouseCellLocation(int x, int y, TerminalSession const& session) noexcept
    {
        auto const pageSize = session.terminal().totalPageSize();
        auto const cellSize = session.display()->cellSize();
        auto const dpr = session.display()->contentScale();

        // The renderer's own origin, not the profile margin re-derived: they are equal while the
        // grid starts at the top-left, and they must STAY equal when it does not. A daemon-hosted
        // grid can be larger than this client's viewport, and the pan that shows a window into it
        // rides this margin (@see contour::geometry::viewportOrigin) -- so a hit-test computing the
        // origin for itself would report the cell the user clicked minus the pan.
        //
        // Correct from the FIRST event, not merely after one: the renderer is constructed with the
        // profile's scaled margin (@see its constructor), so an event that arrives before the first
        // applyResize() is no longer mapped through a zero origin.
        auto const pageMargin = session.display()->gridMetrics().pageMargin;
        auto const marginTop = pageMargin.top;
        auto const marginLeft = pageMargin.left;

        auto const sx = int(double(x) * dpr);
        auto sy = int(double(y) * dpr);

        // Adjust for smooth scroll pixel offset: content is shifted down by pixelOffset,
        // so subtract the offset from the mouse position to map to the correct cell.
        sy -= static_cast<int>(session.terminal().smoothScrollPixelOffset());

        auto const row = vtbackend::LineOffset(
            clamp((sy - marginTop) / cellSize.height.as<int>(), 0, *pageSize.lines - 1));

        auto const col = vtbackend::ColumnOffset(
            clamp((sx - marginLeft) / cellSize.width.as<int>(), 0, *pageSize.columns - 1));

        return { .line = row, .column = col };
    }

    /// @param event The Qt event to take the position from.
    /// @param pageMargin The renderer's grid origin in device pixels (@see makeMouseCellLocation
    ///        for why this is read rather than re-derived).
    /// @param dpr Device pixels per logical pixel.
    PixelCoordinate makeMousePixelPosition(QHoverEvent* event,
                                           vtrasterizer::PageMargin pageMargin,
                                           double dpr) noexcept
    {
        auto const position = event->position();
        return PixelCoordinate {
            .x = PixelCoordinate::X { int(double(position.x()) * dpr) - pageMargin.left },
            .y = PixelCoordinate::Y { int(double(position.y()) * dpr) - pageMargin.top }
        };
    }

    /// @param event The Qt event to take the position from.
    /// @param pageMargin The renderer's grid origin in device pixels (@see makeMouseCellLocation
    ///        for why this is read rather than re-derived).
    /// @param dpr Device pixels per logical pixel.
    PixelCoordinate makeMousePixelPosition(QMouseEvent* event,
                                           vtrasterizer::PageMargin pageMargin,
                                           double dpr) noexcept
    {
        auto const position = event->position();
        return PixelCoordinate {
            .x = PixelCoordinate::X { int(double(position.x()) * dpr) - pageMargin.left },
            .y = PixelCoordinate::Y { int(double(position.y()) * dpr) - pageMargin.top }
        };
    }

    /// @param event The Qt event to take the position from.
    /// @param pageMargin The renderer's grid origin in device pixels (@see makeMouseCellLocation
    ///        for why this is read rather than re-derived).
    /// @param dpr Device pixels per logical pixel.
    PixelCoordinate makeMousePixelPosition(QWheelEvent* event,
                                           vtrasterizer::PageMargin pageMargin,
                                           double dpr) noexcept
    {
        auto const position = event->position();
        return PixelCoordinate {
            .x = PixelCoordinate::X { int(double(position.x()) * dpr) - pageMargin.left },
            .y = PixelCoordinate::Y { int(double(position.y()) * dpr) - pageMargin.top }
        };
    }

    QPoint transposed(QPoint p) noexcept
    {
        // NB: We cannot use QPoint::transposed(), because it is not available in older Qt versions.
        return QPoint { p.y(), p.x() };
    }

    void sendWheelEvent(crispy::point const& pixelDelta,
                        crispy::point const& angleDelta,
                        PixelCoordinate const& currentMousePixelPosition,
                        vtbackend::Modifiers modifiers,
                        vtbackend::ScrollPhase scrollPhase,
                        bool platformInverted,
                        TerminalSession& session)
    {
        using VTMouseButton = vtbackend::MouseButton;

        auto& terminal = session.terminal();

        // BEFORE any of the early returns below: a purely horizontal swipe carries no vertical motion, so
        // the smooth-scroll path treats its Begin and End as zero-delta phase events and consumes them.
        // The horizontal gesture would then never see its own boundaries — and having spent its one
        // navigation step, no later swipe could ever switch a tab again. (Holding a modifier skips the
        // smooth-scroll path entirely, which is why the bug looked like "it only works with Shift".)
        session.noteScrollPhase(scrollPhase);

        // Discard OS-generated momentum events when our own momentum scrolling is active.
        if (scrollPhase == vtbackend::ScrollPhase::Momentum && terminal.isMomentumScrollActive())
            return;

        // Smooth scrolling path: bypass line quantization for the primary screen.
        // Skip smooth scrolling when modifiers are held so that modifier+wheel bindings
        // (e.g. Alt+Wheel for opacity, Ctrl+Wheel for font size) are handled by the binding system.
        if (terminal.settings().smoothScrolling && !terminal.isAlternateScreen() && modifiers.none())
        {
            auto const now = steady_clock::now();
            auto effectivePixelDelta = 0.0f;

            if (angleDelta.y != 0)
            {
                // Prefer angleDelta: it is standardized across platforms (120 units = 1 notch)
                // and allows consistent scaling. The old line-based path uses AngleStepSize=40
                // to quantize into scroll events, each scrolling historyScrollMultiplier lines.
                // Match that rate: effective lines per event = (angleDelta / 40) * multiplier.
                auto const multiplier = session.profile().history.value().historyScrollMultiplier;
                auto const cellHeight = terminal.cellPixelSize().height.as<float>();
                constexpr auto AngleStepSize = 40.0f;
                auto const pixelsPerUnit = unbox<float>(multiplier) * cellHeight / AngleStepSize;
                effectivePixelDelta = static_cast<float>(angleDelta.y) * pixelsPerUnit;
            }
            else if (pixelDelta.y != 0)
            {
                // Fallback for pure trackpad input that only provides pixel deltas.
                effectivePixelDelta = static_cast<float>(pixelDelta.y);
            }

            if (effectivePixelDelta != 0.0f)
            {
                // Mouse wheels carry no gesture phase; route their discrete notches through a
                // momentum impulse so they glide smoothly instead of snapping whole lines. Phased
                // touchpad input keeps the existing velocity-tracking + immediate-apply path.
                if (scrollPhase == vtbackend::ScrollPhase::NoPhase)
                {
                    // Only consume the notch when a glide was actually armed. If injectWheelMomentum
                    // could not arm (cell size still unknown during startup/rebind, alt screen, or a
                    // degenerate zero net velocity) fall through to the legacy line-based path below
                    // instead of silently swallowing the event.
                    if (terminal.injectWheelMomentum(effectivePixelDelta, now)
                        == vtbackend::SmoothScrollResult::Applied)
                        return;
                }
                else
                {
                    terminal.handleScrollPhase(scrollPhase, effectivePixelDelta, now);
                    if (terminal.applySmoothScrollPixelDelta(effectivePixelDelta)
                        == vtbackend::SmoothScrollResult::Applied)
                        return;
                }
            }
            else if (scrollPhase == vtbackend::ScrollPhase::End
                     || scrollPhase == vtbackend::ScrollPhase::Begin)
            {
                // Handle zero-delta phase events (e.g. ScrollEnd often has zero delta).
                terminal.handleScrollPhase(scrollPhase, 0.0f, now);
                return;
            }
        }

        // Existing line-based scrolling (unchanged)
        session.addToAccumulatedScroll(pixelDelta, angleDelta, scrollPhase, platformInverted);
        auto const [linesScroll, columnsScroll] = session.consumeScroll();

        input::inputLog()(
            "[{}] Accumulate scroll with by value {} pixelDelta / {} angleDelta, {} lines, {} columns "
            "(against {})",
            modifiers,
            pixelDelta,
            angleDelta,
            linesScroll,
            columnsScroll,
            session.terminal().cellPixelSize());

        auto const horizontalScrollEvent =
            columnsScroll.as<int>() > 0 ? VTMouseButton::WheelRight : VTMouseButton::WheelLeft;

        for (int i = 0; i < std::abs(columnsScroll.as<int>()); ++i)
        {
            session.sendMousePressEvent(modifiers, horizontalScrollEvent, currentMousePixelPosition);
        }

        auto const verticalScrollEvent =
            linesScroll.as<int>() > 0 ? VTMouseButton::WheelUp : VTMouseButton::WheelDown;

        for (int i = 0; i < std::abs(linesScroll.as<int>()); ++i)
        {
            session.sendMousePressEvent(modifiers, verticalScrollEvent, currentMousePixelPosition);
        }
    }

} // namespace

bool sendKeyEvent(QKeyEvent* event,
                  vtbackend::KeyboardEventType eventType,
                  TerminalSession& session,
                  input::KeyboardLayout const& keyboardLayout)
{
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

        pair { Qt::Key_Shift, Key::LeftShift },     // NB: Qt cannot distinguish between left and right
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
        // clang-format off
        pair { Qt::Key_Space, ' ' },
        pair { Qt::Key_Exclam, '!' },
        pair { Qt::Key_QuoteDbl, '"' },
        pair { Qt::Key_NumberSign, '#' },
        pair { Qt::Key_Dollar, '$' },
        pair { Qt::Key_Percent, '%' },
        pair { Qt::Key_Ampersand, '&' },
        pair { Qt::Key_Apostrophe, '\'' },
        pair { Qt::Key_ParenLeft, '(' },
        pair { Qt::Key_ParenRight, ')' },
        pair { Qt::Key_Asterisk, '*' },
        pair { Qt::Key_Plus, '+' },
        pair { Qt::Key_Comma, ',' },
        pair { Qt::Key_Minus, '-' },
        pair { Qt::Key_Period, '.' },
        pair { Qt::Key_Slash, '/' },
        pair { Qt::Key_0, '0' },
        pair { Qt::Key_1, '1' },
        pair { Qt::Key_2, '2' },
        pair { Qt::Key_3, '3' },
        pair { Qt::Key_4, '4' },
        pair { Qt::Key_5, '5' },
        pair { Qt::Key_6, '6' },
        pair { Qt::Key_7, '7' },
        pair { Qt::Key_8, '8' },
        pair { Qt::Key_9, '9' },
        pair { Qt::Key_Colon, ':' },
        pair { Qt::Key_Semicolon, ';' },
        pair { Qt::Key_Less, '<' },
        pair { Qt::Key_Equal, '=' },
        pair { Qt::Key_Greater, '>' },
        pair { Qt::Key_Question, '?' },
        pair { Qt::Key_At, '@' },
        pair { Qt::Key_A, 'A' },
        pair { Qt::Key_B, 'B' },
        pair { Qt::Key_C, 'C' },
        pair { Qt::Key_D, 'D' },
        pair { Qt::Key_E, 'E' },
        pair { Qt::Key_F, 'F' },
        pair { Qt::Key_G, 'G' },
        pair { Qt::Key_H, 'H' },
        pair { Qt::Key_I, 'I' },
        pair { Qt::Key_J, 'J' },
        pair { Qt::Key_K, 'K' },
        pair { Qt::Key_L, 'L' },
        pair { Qt::Key_M, 'M' },
        pair { Qt::Key_N, 'N' },
        pair { Qt::Key_O, 'O' },
        pair { Qt::Key_P, 'P' },
        pair { Qt::Key_Q, 'Q' },
        pair { Qt::Key_R, 'R' },
        pair { Qt::Key_S, 'S' },
        pair { Qt::Key_T, 'T' },
        pair { Qt::Key_U, 'U' },
        pair { Qt::Key_V, 'V' },
        pair { Qt::Key_W, 'W' },
        pair { Qt::Key_X, 'X' },
        pair { Qt::Key_Y, 'Y' },
        pair { Qt::Key_Z, 'Z' },
        pair { Qt::Key_BracketLeft, '[' },
        pair { Qt::Key_Backslash, '\\' },
        pair { Qt::Key_BracketRight, ']' },
        pair { Qt::Key_AsciiCircum, '^' },
        pair { Qt::Key_Underscore, '_' },
        pair { Qt::Key_QuoteLeft, '`' },
        pair { Qt::Key_BraceLeft, '{' },
        pair { Qt::Key_Bar, '|' },
        pair { Qt::Key_BraceRight, '}' },
        pair { Qt::Key_AsciiTilde, '~' },
        // clang-format on
    }; // }}}

    auto const isWin32Mode = session.terminal().isModeEnabled(vtbackend::DECMode::Win32InputMode);
    auto const modifiers = input::makeModifiers(event->modifiers(),
                                                input::nativeModifiersWithLockState(event->nativeModifiers()),
                                                /*stripAltGr=*/!isWin32Mode);
    auto const key = event->key();

    if (event->modifiers().testFlag(Qt::KeypadModifier))
    {
        std::optional<Key> mappedKey = nullopt;
        auto inferredModifiers = modifiers;
        switch (key)
        {
            // Qt reports Key_0..9 with KeypadModifier only when NumLock is ON.
            // When NumLock is OFF, Qt reports navigation keys instead (Insert, End, etc.).
            // So the presence of these key codes definitionally implies NumLock is active.
            case Qt::Key_0:
            case Qt::Key_1:
            case Qt::Key_2:
            case Qt::Key_3:
            case Qt::Key_4:
            case Qt::Key_5:
            case Qt::Key_6:
            case Qt::Key_7:
            case Qt::Key_8:
            case Qt::Key_9:
                mappedKey = static_cast<Key>(static_cast<int>(Key::Numpad_0) + (key - Qt::Key_0));
                inferredModifiers.locks |= vtbackend::LockKey::NumLock;
                break;
            // Operator and Enter keys have the same Qt key code regardless of NumLock state,
            // but produce text when NumLock is active.
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
            // For operator/Enter keys, infer NumLock from non-empty text
            if (!event->text().isEmpty())
                inferredModifiers.locks |= vtbackend::LockKey::NumLock;
            session.sendKeyEvent(*mappedKey, inferredModifiers, eventType, now);
            return true;
        }
    }

    // NOLINTNEXTLINE(readability-qualified-auto)
    if (auto const i =
            std::ranges::find_if(KeyMappings, [event](auto const& x) { return x.first == event->key(); });
        i != end(KeyMappings))
    {
        session.sendKeyEvent(i->second, modifiers, eventType, now);
        event->accept();
        return true;
    }

    // What the platform natively calls this key, translated into the two vocabularies the keyboard
    // protocols speak. Both go through the layout rather than being copied across raw: only it knows
    // how to read nativeVirtualKey(), which is a positional Carbon key code on macOS, a keysym on
    // X11 and a VK code on Win32.
    auto const nativeVirtualKey = event->nativeVirtualKey();
    auto const keyIdentity = vtbackend::KeyIdentity {
        .unshiftedKey = keyboardLayout.unshiftedKeyOf(nativeVirtualKey),
        .nativeVirtualKey = keyboardLayout.win32VirtualKeyOf(nativeVirtualKey),
    };

    if (event->text().isEmpty())
    {
        // NOLINTNEXTLINE(readability-qualified-auto)
        if (auto const i = std::ranges::find_if(CharMappings,
                                                [event](auto const& x) { return x.first == event->key(); });
            i != end(CharMappings))
        {
            session.sendCharEvent(static_cast<char32_t>(i->second), keyIdentity, modifiers, eventType, now);
            event->accept();
            return true;
        }
    }

    if (key == Qt::Key_Backtab)
    {
        session.sendKeyEvent(Key::Tab, modifiers.with(Modifier::Shift), eventType, now);
        event->accept();
        return true;
    }

#ifdef __APPLE__
    if (0x20 <= key && key < 0x80
        && (modifiers.chord.test(Modifier::Alt) && session.profile().optionKeyAsAlt.value()))
    {
        // CapsLock inverts the effect of Shift on letter case, which is the one thing a lock key
        // legitimately decides here.
        bool const shiftPressed =
            modifiers.chord.test(Modifier::Shift) ^ modifiers.locks.test(vtbackend::LockKey::CapsLock);
        auto const ch = static_cast<char32_t>(shiftPressed ? std::toupper(key) : std::tolower(key));
        session.sendCharEvent(ch, keyIdentity, modifiers, eventType, now);
        event->accept();
        return true;
    }
#endif

    if (0x20 <= key && key < 0x80 && modifiers.chord.test(Modifier::Control))
    {
        session.sendCharEvent(static_cast<char32_t>(key), keyIdentity, modifiers, eventType, now);
        event->accept();
        return true;
    }

    if (!event->text().isEmpty())
    {
        auto const codepoints = event->text().toUcs4();
        assert(!codepoints.empty());
#ifdef __APPLE__
        // On macOS the Alt-modifier does not seem to be passed to the terminal apps
        // but rather remapped to whatever macOS is mapping them to.
        for (char32_t const ch: codepoints)
            session.sendCharEvent(ch, keyIdentity, modifiers.without(Modifier::Alt), eventType, now);
#else
        for (char32_t const ch: codepoints)
            session.sendCharEvent(ch, keyIdentity, modifiers, eventType, now);
#endif
        event->accept();
        return true;
    }

    input::inputLog()("Input not handled for mods {} key 0x{:X}", modifiers, key);
    return false;
}

void sendWheelEvent(QWheelEvent* event, TerminalSession& session)
{
    // A display-less session (a background pane during a split/tab rebind) has no geometry or
    // content scale to map display-space coordinates with; drop the event, matching the other
    // display-dependent event paths' guards.
    if (session.display() == nullptr)
        return;
    auto const phase = event->phase();
    // Allow zero-delta events through if they carry phase info (e.g. ScrollEnd).
    if (event->pixelDelta().isNull() && event->angleDelta().isNull() && phase != Qt::ScrollEnd
        && phase != Qt::ScrollBegin)
        return;

    using vtbackend::Modifier;

    // Mouse events carry no lock state: makeModifiers() derives it from the native modifier mask,
    // which Qt only populates for key events. Take the chord and be explicit about it.
    auto const modifiers = input::makeModifiers(event->modifiers()).chord;

    auto const pixelPosition = makeMousePixelPosition(
        event, session.display()->gridMetrics().pageMargin, session.display()->contentScale());

    // NOTE: Qt is playing some weird games with the mouse wheel events, i.e. if Alt is pressed
    //       it will send horizontal wheel events instead of vertical ones. We need to compensate
    //       for that here.

    auto const pixelDelta = [&]() -> crispy::point {
        if (event->pixelDelta().isNull())
            return { .x = 0, .y = 0 };

        auto const scaledPixelDelta = session.display()->contentScale() * event->pixelDelta();
        auto const x = scaledPixelDelta.x();
        auto const y = scaledPixelDelta.y();
        if (modifiers & Modifier::Alt)
            return { .x = y, .y = x };
        else
            return { .x = x, .y = y };
    }();

    auto const angleDelta = [&]() -> crispy::point {
        if (event->angleDelta().isNull())
            return { .x = 0, .y = 0 };

        auto const numDegrees =
            ((modifiers & Modifier::Alt) ? transposed(event->angleDelta()) : event->angleDelta());

        return { .x = numDegrees.x(), .y = numDegrees.y() };
    }();

    auto const scrollPhase = input::mapScrollPhase(event->phase());
    sendWheelEvent(pixelDelta, angleDelta, pixelPosition, modifiers, scrollPhase, event->inverted(), session);
    event->accept();
}

void sendMousePressEvent(QMouseEvent* event, TerminalSession& session)
{
    // A display-less session (a background pane during a split/tab rebind) has no geometry or
    // content scale to map display-space coordinates with; drop the event, matching the other
    // display-dependent event paths' guards.
    if (session.display() == nullptr)
        return;
    session.sendMousePressEvent(input::makeModifiers(event->modifiers()).chord,
                                input::makeMouseButton(event->button()),
                                makeMousePixelPosition(event,
                                                       session.display()->gridMetrics().pageMargin,
                                                       session.display()->contentScale()));
    event->accept();
}

void sendMouseReleaseEvent(QMouseEvent* event, TerminalSession& session)
{
    // A display-less session (a background pane during a split/tab rebind) has no geometry or
    // content scale to map display-space coordinates with; drop the event, matching the other
    // display-dependent event paths' guards.
    if (session.display() == nullptr)
        return;
    session.sendMouseReleaseEvent(input::makeModifiers(event->modifiers()).chord,
                                  input::makeMouseButton(event->button()),
                                  makeMousePixelPosition(event,
                                                         session.display()->gridMetrics().pageMargin,
                                                         session.display()->contentScale()));
    event->accept();
}

void sendMouseMoveEvent(QMouseEvent* event, TerminalSession& session)
{
    // A display-less session (a background pane during a split/tab rebind) has no geometry or
    // content scale to map display-space coordinates with; drop the event, matching the other
    // display-dependent event paths' guards.
    if (session.display() == nullptr)
        return;
    session.sendMouseMoveEvent(input::makeModifiers(event->modifiers()).chord,
                               makeMouseCellLocation(event->pos().x(), event->pos().y(), session),
                               makeMousePixelPosition(event,
                                                      session.display()->gridMetrics().pageMargin,
                                                      session.display()->contentScale()));
    event->accept();
}

void sendMouseMoveEvent(QHoverEvent* event, TerminalSession& session)
{
    // A display-less session (a background pane during a split/tab rebind) has no geometry or
    // content scale to map display-space coordinates with; drop the event, matching the other
    // display-dependent event paths' guards.
    if (session.display() == nullptr)
        return;
    auto const position = event->position().toPoint();
    session.sendMouseMoveEvent(input::makeModifiers(event->modifiers()).chord,
                               makeMouseCellLocation(position.x(), position.y(), session),
                               makeMousePixelPosition(event,
                                                      session.display()->gridMetrics().pageMargin,
                                                      session.display()->contentScale()));
    event->accept();
}

AutoScrollInfo computeAutoScrollInfo(QMouseEvent const* event, TerminalSession const& session) noexcept
{
    auto const dpr = session.display()->contentScale();
    auto const cellHeight = session.display()->cellSize().height.as<int>();
    auto const marginTop = static_cast<int>(unbox(session.profile().margins.value().vertical) * dpr);
    auto const pageLines = *session.terminal().totalPageSize().lines;
    auto const contentBottom = marginTop + (pageLines * cellHeight);

    auto const mouseY = static_cast<int>(event->position().y() * dpr);

    if (mouseY < marginTop)
    {
        auto const distance = marginTop - mouseY;
        return { .direction = -1, .linesPerTick = std::max(1, distance / cellHeight) };
    }

    if (mouseY > contentBottom)
    {
        auto const distance = mouseY - contentBottom;
        return { .direction = 1, .linesPerTick = std::max(1, distance / cellHeight) };
    }

    return {};
}

} // namespace contour::session
