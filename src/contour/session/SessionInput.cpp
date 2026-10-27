// SPDX-License-Identifier: Apache-2.0
#include <contour/display/TerminalDisplay.hpp>
#include <contour/input/KeyMapping.hpp>
#include <contour/input/Logging.hpp>
#include <contour/input/MouseMapping.hpp>
#include <contour/session/FontControl.hpp>
#include <contour/session/Logging.hpp>
#include <contour/session/SessionInput.hpp>
#include <contour/session/TerminalSession.hpp>

#include <vtbackend/screen/Terminal.hpp>

#include <vtrasterizer/Renderer.hpp>

#include <core/Utils.hpp>

#include <QtGui/QGuiApplication>

#include <algorithm>
#include <array>
#include <mutex>

using std::array;
using std::clamp;
using std::nullopt;
using std::optional;
using std::pair;
using std::u32string;
using std::chrono::steady_clock;

using crispy::Point;

using vtbackend::PixelCoordinate;

namespace contour::session
{

namespace
{
    /// The device-pixel y of @p yLogical, as is: where whatever is pinned to the item is drawn.
    [[nodiscard]] int deviceY(double yLogical, TerminalSession const& session) noexcept
    {
        return static_cast<int>(yLogical * session.display()->devicePixelRatio());
    }

    /// The device-pixel y of @p yLogical, with the smooth-scroll shift taken out.
    ///
    /// Content is shifted DOWN by the smooth-scroll offset, so the offset comes off the pointer to
    /// map back onto the cell that is actually drawn there.
    [[nodiscard]] int mouseDeviceY(double yLogical, TerminalSession const& session) noexcept
    {
        return deviceY(yLogical, session) - static_cast<int>(session.terminal().smoothScrollPixelOffset());
    }

    /// The main-page row at the device-pixel y @p yDevice, or nullopt when that is not over the grid.
    ///
    /// The one row hit-test in the GUI that declines a pointer off the grid: every such caller comes
    /// through here, so none can disagree about where the grid starts -- which, with a status line above
    /// it, is not screen row zero. Whether the smooth-scroll shift comes off @p yDevice is the caller's
    /// to say, because only what scrolls is shifted: mouseDeviceY() for the grid's rows, deviceY() for
    /// what is pinned over them.
    [[nodiscard]] std::optional<vtbackend::LineOffset> gridRowAt(int yDevice,
                                                                 TerminalSession const& session) noexcept
    {
        auto const& terminal = session.terminal();
        auto const row = geometry::mainPageRowAt(yDevice,
                                                 session.display()->gridMetrics().pageMargin.top,
                                                 session.display()->cellSize().height.as<int>(),
                                                 unbox<int>(terminal.mainPageTopRow()),
                                                 unbox<int>(terminal.mainPageSize().lines));
        return row ? std::optional { vtbackend::LineOffset(*row) } : std::nullopt;
    }

    /// The main-page row at @p positionPx as the sticky header covers it -- the one hit-test its press and
    /// its motion share. Measured without the smooth-scroll shift: the header is pinned over row 0 and drawn
    /// unshifted, not carried by the content scrolling beneath it. And over the page's columns only, which is
    /// all the header spans: the gutter and the margins beside it keep their own meaning.
    /// @param positionPx The pointer position in logical pixels, relative to the item's top-left.
    /// @param session The session it belongs to; must have a display.
    /// @return The row, or nullopt when the position is not over the page's columns.
    [[nodiscard]] std::optional<vtbackend::LineOffset> stickyHeaderRowAt(
        QPointF positionPx, TerminalSession const& session) noexcept
    {
        auto const& display = *session.display();
        auto const column =
            geometry::pageColumnAt(static_cast<int>(positionPx.x() * display.devicePixelRatio()),
                                   display.gridMetrics().pageMargin.left,
                                   display.cellSize().width.as<int>(),
                                   unbox<int>(session.terminal().mainPageSize().columns));
        if (!column)
            return std::nullopt;
        return gridRowAt(deviceY(positionPx.y(), session), session);
    }

    vtbackend::CellLocation makeMouseCellLocation(int x, int y, TerminalSession const& session) noexcept
    {
        // The MAIN page, not the total: the row this returns is main-page relative -- which is the
        // space Viewport's coordinate translation speaks -- so clamping it against a total that
        // includes the status line would let it name a row past the end of the grid. Read race-free, as
        // every hit-test here runs on the GUI thread without the terminal lock.
        auto const pageSize = session.terminal().mainPageSize();
        auto const cellSize = session.display()->cellSize();
        auto const dpr = session.display()->devicePixelRatio();

        // The renderer's own origin, not the profile margin re-derived: they are equal while the
        // grid starts at the top-left, and they must STAY equal when it does not. A daemon-hosted
        // grid can be larger than this client's viewport, and the pan that shows a window into it
        // rides this margin (@see vthost::client::viewportOrigin) -- so a hit-test computing the
        // origin for itself would report the cell the user clicked minus the pan.
        //
        // Correct from the FIRST event, not merely after one: the renderer is constructed with the
        // profile's scaled margin (@see its constructor), so an event that arrives before the first
        // applyResize() is no longer mapped through a zero origin.
        auto const pageMargin = session.display()->gridMetrics().pageMargin;
        auto const marginTop = pageMargin.top;
        auto const marginLeft = pageMargin.left;

        auto const sx = int(double(x) * dpr);
        auto const sy = mouseDeviceY(y, session);

        // Clamped to the main page, so a drag that leaves the grid keeps naming its nearest row --
        // auto-scroll depends on that. Off the MAIN page (over a status line) the nearest row is the
        // grid edge, which gridRowAt() declines to invent and the move path therefore skips.
        //
        // Through the same tested header the declining hit-test uses, so the two cannot disagree about
        // where the grid starts (@see geometry::mainPageRowNear).
        auto const row =
            vtbackend::LineOffset(geometry::mainPageRowNear(sy,
                                                            marginTop,
                                                            cellSize.height.as<int>(),
                                                            unbox<int>(session.terminal().mainPageTopRow()),
                                                            *pageSize.lines));

        auto const col = vtbackend::ColumnOffset(
            clamp((sx - marginLeft) / cellSize.width.as<int>(), 0, *pageSize.columns - 1));

        return { .line = row, .column = col };
    }

    /// Where over the gutter @p positionPx is: the grid line beside it and the segment under it.
    ///
    /// The one place that decides where the gutter IS and what a point on it names -- shared by the
    /// click that toggles a fold and the hover that highlights one, because those two disagreeing would
    /// light up one block and toggle another. The layout is the configured one, the very layout the
    /// window geometry reserved the strip for.
    ///
    /// Deliberately NOT routed through makeMouseCellLocation(): that clamps the column onto the page,
    /// so a gutter position would come back reading as column 0 of the grid.
    ///
    /// @param positionPx The pointer position in logical pixels, relative to the item's top-left.
    /// @param session The session it belongs to; must have a display.
    /// @return The hit, or nullopt when the position is not over the gutter.
    [[nodiscard]] std::optional<GutterHit> gutterHitAt(QPointF positionPx, TerminalSession const& session)
    {
        auto const& display = *session.display();
        auto const dpr = display.devicePixelRatio();
        auto const cellSize = display.gridMetrics().cellSize;
        auto const pageMargin = display.gridMetrics().pageMargin;
        auto const layout = configuredGutterLayout(session.config());

        auto const sx = static_cast<int>(positionPx.x() * dpr);
        // The layout built once, for the extent and the segment both: with timestamps on, building one
        // formats a sample time, and this runs on every pointer motion.
        auto const column = geometry::gutterColumnAt(
            sx, pageMargin.left, gutterExtentFor(layout, cellSize, dpr), unbox<int>(cellSize.width));
        auto const segment = column ? vtbackend::gutterSegmentAt(layout, *column) : std::nullopt;
        if (!segment)
            return std::nullopt;

        // Through the shared row hit-test, so the gutter and the grid agree about where the page
        // begins. They did not: this used to feed a raw SCREEN row to a translation that speaks
        // main-page rows, so with a status line above the grid every fold click landed one block off.
        //
        // Row 0 is measured unshifted, as the sticky header pinned beside it is: mid-animation the band the
        // header covers reaches above where the scrolled row 0 begins, and beside it the fold column is
        // still row 0's. Every other row scrolls, and is measured where it is drawn.
        auto const pinnedRow = gridRowAt(deviceY(positionPx.y(), session), session);
        auto const row = pinnedRow == vtbackend::LineOffset(0)
                             ? pinnedRow
                             : gridRowAt(mouseDeviceY(positionPx.y(), session), session);
        if (!row)
            return std::nullopt;

        auto const l = std::scoped_lock { session.terminal() };
        return GutterHit { .gridLine = session.terminal().viewport().translateScreenToGridLine(*row),
                           .screenRow = session.terminal().mainPageTopRow() + *row,
                           .segment = *segment };
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

    void sendWheelEvent(crispy::Point const& pixelDelta,
                        crispy::Point const& angleDelta,
                        vtbackend::CellLocation wheelCell,
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

        // Smooth scrolling path: bypass line quantization while page 1 -- the primary screen -- is on
        // display, wherever the cursor writes. Skip smooth scrolling when modifiers are held so that
        // modifier+wheel bindings (e.g. Alt+Wheel for opacity, Ctrl+Wheel for font size) are handled by the
        // binding system.
        if (terminal.settings().smoothScrolling
            && terminal.displayedScreenType() == vtbackend::ScreenType::Primary && modifiers.none())
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

            // Locked for the reason TerminalSession::smoothScrollUp() states: these advance the
            // viewport offset, the sub-cell remainder and the momentum state, all of which the parser
            // thread touches too. And the clamp inside applySmoothScrollPixelDelta() now reads the
            // scrollable count, which builds the fold projection lazily -- a cache the render pass
            // clears and refills under this same lock.
            //
            // Scoped to this branch alone, and the result carried out rather than returned from
            // inside: the lock is not recursive and the line-based path below takes it again per
            // synthesized mouse event. Through runUnderStateLock(): a scroll during a drag extends the
            // selection, whose report (DECSET 2030) is sent once the lock is released.
            auto const consumed = terminal.runUnderStateLock([&] {
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
                        return terminal.injectWheelMomentum(effectivePixelDelta, now)
                               == vtbackend::SmoothScrollResult::Applied;
                    }

                    terminal.handleScrollPhase(scrollPhase, effectivePixelDelta, now);
                    return terminal.applySmoothScrollPixelDelta(effectivePixelDelta)
                           == vtbackend::SmoothScrollResult::Applied;
                }

                if (scrollPhase == vtbackend::ScrollPhase::End
                    || scrollPhase == vtbackend::ScrollPhase::Begin)
                {
                    // Handle zero-delta phase events (e.g. ScrollEnd often has zero delta).
                    terminal.handleScrollPhase(scrollPhase, 0.0f, now);
                    return true;
                }

                return false;
            });

            if (consumed)
                return;
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

        // These are synthesized where the wheel turned, which is where the pointer rests: the cell the
        // wheel event names, as its pixel position names the pixel. Not the terminal's last-tracked cell,
        // which is not the pointer's before any pointer event (its default, the top-left cell) nor once the
        // pointer has left the grid (the cell it left from) -- a notch aimed there was reported there, and
        // put the pointer back on it.
        for (int i = 0; i < std::abs(columnsScroll.as<int>()); ++i)
        {
            session.sendMousePressEvent(
                modifiers, horizontalScrollEvent, wheelCell, currentMousePixelPosition);
        }

        auto const verticalScrollEvent =
            linesScroll.as<int>() > 0 ? VTMouseButton::WheelUp : VTMouseButton::WheelDown;

        for (int i = 0; i < std::abs(linesScroll.as<int>()); ++i)
        {
            session.sendMousePressEvent(modifiers, verticalScrollEvent, wheelCell, currentMousePixelPosition);
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

    auto const modifiers = input::makeModifiers(event->modifiers(),
                                                input::nativeModifiersWithLockState(event->nativeModifiers()),
                                                input::ctrlAltRoleOf(event->modifiers(), event->text()));
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

    if (auto const named = input::namedKeyOf(event->key()))
    {
        session.sendKeyEvent(*named, modifiers, eventType, now);
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
        event, session.display()->gridMetrics().pageMargin, session.display()->devicePixelRatio());

    // NOTE: Qt is playing some weird games with the mouse wheel events, i.e. if Alt is pressed
    //       it will send horizontal wheel events instead of vertical ones. We need to compensate
    //       for that here.

    auto const pixelDelta = [&]() -> crispy::Point {
        if (event->pixelDelta().isNull())
            return { .x = 0, .y = 0 };

        auto const scaledPixelDelta = session.display()->devicePixelRatio() * event->pixelDelta();
        auto const x = scaledPixelDelta.x();
        auto const y = scaledPixelDelta.y();
        if (modifiers & Modifier::Alt)
            return { .x = y, .y = x };
        else
            return { .x = x, .y = y };
    }();

    auto const angleDelta = [&]() -> crispy::Point {
        if (event->angleDelta().isNull())
            return { .x = 0, .y = 0 };

        auto const numDegrees =
            ((modifiers & Modifier::Alt) ? transposed(event->angleDelta()) : event->angleDelta());

        return { .x = numDegrees.x(), .y = numDegrees.y() };
    }();

    auto const scrollPhase = input::mapScrollPhase(event->phase());
    auto const position = event->position().toPoint();
    sendWheelEvent(pixelDelta,
                   angleDelta,
                   makeMouseCellLocation(position.x(), position.y(), session),
                   pixelPosition,
                   modifiers,
                   scrollPhase,
                   event->inverted(),
                   session);
    event->accept();
}

void sendMousePressEvent(QMouseEvent* event, TerminalSession& session)
{
    // A display-less session (a background pane during a split/tab rebind) has no geometry or
    // content scale to map display-space coordinates with; drop the event, matching the other
    // display-dependent event paths' guards.
    if (session.display() == nullptr)
        return;

    if (session.sendGutterPressEvent(gutterHitAt(event->position(), session),
                                     input::makeMouseButton(event->button()),
                                     input::makeModifiers(event->modifiers()).chord)
        == ConsumedByGutter::Yes)
    {
        event->accept();
        return;
    }

    // After the gutter, whose strip beside row 0 keeps its own meaning, and before the grid.
    if (session.sendStickyHeaderPressEvent(stickyHeaderRowAt(event->position(), session),
                                           input::makeMouseButton(event->button()),
                                           input::makeModifiers(event->modifiers()).chord)
        == ConsumedByStickyHeader::Yes)
    {
        event->accept();
        return;
    }

    session.sendMousePressEvent(input::makeModifiers(event->modifiers()).chord,
                                input::makeMouseButton(event->button()),
                                makeMouseCellLocation(event->pos().x(), event->pos().y(), session),
                                makeMousePixelPosition(event,
                                                       session.display()->gridMetrics().pageMargin,
                                                       session.display()->devicePixelRatio()));
    event->accept();
}

void sendMouseReleaseEvent(QMouseEvent* event, TerminalSession& session)
{
    // A display-less session (a background pane during a split/tab rebind) has no geometry or
    // content scale to map display-space coordinates with; drop the event, matching the other
    // display-dependent event paths' guards.
    if (session.display() == nullptr)
        return;

    // A release owed to the gutter or the sticky header, whichever took its press, is theirs alone.
    if (session.sendOwedRelease(input::makeMouseButton(event->button())) != PressOwner::None)
    {
        event->accept();
        return;
    }

    session.sendMouseReleaseEvent(input::makeModifiers(event->modifiers()).chord,
                                  input::makeMouseButton(event->button()),
                                  makeMousePixelPosition(event,
                                                         session.display()->gridMetrics().pageMargin,
                                                         session.display()->devicePixelRatio()));
    event->accept();
}

void sendMouseMoveEvent(QMouseEvent* event, TerminalSession& session)
{
    // A display-less session (a background pane during a split/tab rebind) has no geometry or
    // content scale to map display-space coordinates with; drop the event, matching the other
    // display-dependent event paths' guards.
    if (session.display() == nullptr)
        return;

    // A mouse move is a drag: the hover comes as a QHoverEvent. One the grid's own press began stays the
    // grid's over the gutter, clamped onto column 0 as it is past the page's right edge, so a selection
    // dragged out to the left keeps extending. Only a drag from a press the gutter took is the gutter's.
    if (session.pressOwner() != PressOwner::None
        && session.sendGutterHoverEvent(gutterHitAt(event->position(), session)) == ConsumedByGutter::Yes)
    {
        event->accept();
        return;
    }

    // After the gutter and before the grid, as the press is.
    if (session.sendStickyHeaderHoverEvent(stickyHeaderRowAt(event->position(), session))
        == ConsumedByStickyHeader::Yes)
    {
        event->accept();
        return;
    }

    session.sendMouseMoveEvent(input::makeModifiers(event->modifiers()).chord,
                               makeMouseCellLocation(event->pos().x(), event->pos().y(), session),
                               makeMousePixelPosition(event,
                                                      session.display()->gridMetrics().pageMargin,
                                                      session.display()->devicePixelRatio()));
    event->accept();
}

void sendMouseMoveEvent(QHoverEvent* event, TerminalSession& session)
{
    // A display-less session (a background pane during a split/tab rebind) has no geometry or
    // content scale to map display-space coordinates with; drop the event, matching the other
    // display-dependent event paths' guards.
    if (session.display() == nullptr)
        return;

    if (session.sendGutterHoverEvent(gutterHitAt(event->position(), session)) == ConsumedByGutter::Yes)
    {
        event->accept();
        return;
    }

    if (session.sendStickyHeaderHoverEvent(stickyHeaderRowAt(event->position(), session))
        == ConsumedByStickyHeader::Yes)
    {
        event->accept();
        return;
    }

    auto const position = event->position().toPoint();
    session.sendMouseMoveEvent(input::makeModifiers(event->modifiers()).chord,
                               makeMouseCellLocation(position.x(), position.y(), session),
                               makeMousePixelPosition(event,
                                                      session.display()->gridMetrics().pageMargin,
                                                      session.display()->devicePixelRatio()));
    event->accept();
}

AutoScrollInfo computeAutoScrollInfo(QMouseEvent const* event, TerminalSession const& session) noexcept
{
    auto const dpr = session.display()->devicePixelRatio();
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
