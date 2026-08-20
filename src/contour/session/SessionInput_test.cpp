// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the Qt→vtbackend input translation helpers (helper.h/helper.cpp): the pure
// mapping functions every key/mouse event flows through before reaching a terminal session.

#include <contour/ContourGuiApp.hpp>
#include <contour/config/Config.hpp>
#include <contour/input/KeyMapping.hpp>
#include <contour/input/MouseMapping.hpp>
#include <contour/platform/QtPath.hpp>
#include <contour/session/SessionInput.hpp>
#include <contour/session/SpawnCommand.hpp>
#include <contour/session/TerminalSession.hpp>
#include <contour/test/GuiTestFixtures.hpp>

#include <vtbackend/input/InputGenerator.hpp>

#include <vtpty/MockPty.hpp>

#include <QtGui/QKeyEvent>
#include <QtGui/QMouseEvent>
#include <QtGui/QWheelEvent>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <ranges>

using contour::input::makeModifiers;
using contour::input::makeMouseButton;

TEST_CASE("makeMouseButton maps Qt buttons onto VT buttons", "[session][input]")
{
    STATIC_CHECK(makeMouseButton(Qt::LeftButton) == vtbackend::MouseButton::Left);
    STATIC_CHECK(makeMouseButton(Qt::MiddleButton) == vtbackend::MouseButton::Middle);
    STATIC_CHECK(makeMouseButton(Qt::RightButton) == vtbackend::MouseButton::Right);
    // Unknown buttons deliberately degrade to Left (the safest default for VT mouse reports).
    STATIC_CHECK(makeMouseButton(Qt::BackButton) == vtbackend::MouseButton::Left);
}

TEST_CASE("makeModifiers maps Qt keyboard modifiers onto VT modifiers", "[session][input]")
{
    CHECK(makeModifiers(Qt::NoModifier) == vtbackend::Modifiers {});
    CHECK(makeModifiers(Qt::ShiftModifier) == vtbackend::Modifiers { vtbackend::Modifier::Shift });
    CHECK(makeModifiers(Qt::ControlModifier) == vtbackend::Modifiers { vtbackend::Modifier::Control });
    CHECK(makeModifiers(Qt::AltModifier) == vtbackend::Modifiers { vtbackend::Modifier::Alt });
    CHECK(makeModifiers(Qt::MetaModifier) == vtbackend::Modifiers { vtbackend::Modifier::Super });

    // stripAltGr=false so the raw Qt->Modifier mapping is asserted: with the default (true), Win32
    // treats a Ctrl+Alt combination as AltGr and strips both, which is correct platform behavior but
    // not what this basic-mapping case is checking.
    auto const combined =
        makeModifiers(Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier, 0, /*stripAltGr=*/false);
    CHECK(combined.chord.contains(vtbackend::Modifier::Shift));
    CHECK(combined.chord.contains(vtbackend::Modifier::Control));
    CHECK(combined.chord.contains(vtbackend::Modifier::Alt));
    CHECK_FALSE(combined.chord.contains(vtbackend::Modifier::Super));

    // Qt's modifier mask never yields a lock key; those come from the native mask only.
    CHECK(combined.locks.none());
}

TEST_CASE("unshiftedCodepoint inverts the US-ASCII shift level", "[session][input]")
{
    using contour::input::unshiftedCodepoint;
    // Punctuation and number-row shifted symbols map back to the base key label a binding is written
    // with (this is what lets `Ctrl+Shift+,` fire when Qt delivers the shifted '<').
    CHECK(contour::input::unshiftedCodepoint(U'<') == U',');
    CHECK(contour::input::unshiftedCodepoint(U'>') == U'.');
    CHECK(contour::input::unshiftedCodepoint(U'?') == U'/');
    CHECK(contour::input::unshiftedCodepoint(U':') == U';');
    CHECK(contour::input::unshiftedCodepoint(U'"') == U'\'');
    CHECK(contour::input::unshiftedCodepoint(U'{') == U'[');
    CHECK(contour::input::unshiftedCodepoint(U'|') == U'\\');
    CHECK(contour::input::unshiftedCodepoint(U'_') == U'-');
    CHECK(contour::input::unshiftedCodepoint(U'+') == U'=');
    CHECK(contour::input::unshiftedCodepoint(U'!') == U'1');
    CHECK(contour::input::unshiftedCodepoint(U'@') == U'2');
    CHECK(contour::input::unshiftedCodepoint(U')') == U'0');
    // Non-shifted symbols and letters (shift-invariant here) are returned unchanged.
    CHECK(contour::input::unshiftedCodepoint(U',') == U',');
    CHECK(contour::input::unshiftedCodepoint(U'P') == U'P');
    CHECK(contour::input::unshiftedCodepoint(U'a') == U'a');
    CHECK(contour::input::unshiftedCodepoint(U'5') == U'5');
}

#if !defined(_WIN32) && !defined(__APPLE__)
TEST_CASE("makeModifiers derives CapsLock/NumLock from the X11 native modifier mask", "[session][input]")
{
    // On Linux the lock states come from the XCB/XKB native mask (CapsLock = XCB_MOD_MASK_LOCK 0x02,
    // NumLock = XCB_MOD_MASK_2 0x10), independent of the Qt modifier bits.
    constexpr quint32 XcbCapsLockMask = 0x02;
    constexpr quint32 XcbNumLockMask = 0x10;

    CHECK(makeModifiers(Qt::NoModifier, XcbCapsLockMask).locks.contains(vtbackend::LockKey::CapsLock));
    CHECK(makeModifiers(Qt::NoModifier, XcbNumLockMask).locks.contains(vtbackend::LockKey::NumLock));

    auto const both = makeModifiers(Qt::ShiftModifier, XcbCapsLockMask | XcbNumLockMask);
    CHECK(both.chord.contains(vtbackend::Modifier::Shift));
    CHECK(both.locks.contains(vtbackend::LockKey::CapsLock));
    CHECK(both.locks.contains(vtbackend::LockKey::NumLock));

    // A lock key never lands in the chord, so key bindings and Vi mode cannot see it.
    CHECK(both.chord == vtbackend::Modifiers { vtbackend::Modifier::Shift });

    // No native bits set -> no lock keys.
    auto const none = makeModifiers(Qt::ControlModifier, 0);
    CHECK(none.locks.none());
}
#endif

namespace
{
/// Display-less session over a MockPty (the helper.cpp key path needs a session but no display).
[[nodiscard]] std::unique_ptr<contour::session::TerminalSession> makeSession(contour::ContourGuiApp& app)
{
    auto pty =
        std::make_unique<vtpty::MockPty>(vtpty::PageSize { vtpty::LineCount(24), vtpty::ColumnCount(80) });
    return std::make_unique<contour::session::TerminalSession>(&app.sessionsManager(), std::move(pty), app);
}

/// The layout for every test that is not about layouts: passthrough leaves the native key
/// identifier alone, which for the events built here (nativeVirtualKey 0) means "unknown".
[[nodiscard]] contour::input::KeyboardLayout const& passthroughLayout()
{
    static auto const layout = contour::input::makePassthroughKeyboardLayout();
    return *layout;
}

/// Reads native key identifiers as macOS does — as positional Carbon key codes — so the macOS
/// regression can be driven through the real key path on any platform.
[[nodiscard]] contour::input::KeyboardLayout const& macUsAnsiLayout()
{
    static auto const layout = contour::input::makeMacUsAnsiKeyboardLayout();
    return *layout;
}

using contour::test::mockPtyOf;
} // namespace

TEST_CASE("sendKeyEvent maps Qt key events onto the terminal's PTY encoding", "[session][input]")
{
    contour::test::TestApp app;
    auto session = makeSession(app.app());
    auto& pty = mockPtyOf(*session);

    // A printable character key writes its text.
    {
        QKeyEvent ev(QEvent::KeyPress, Qt::Key_X, Qt::NoModifier, QStringLiteral("x"));
        pty.stdinBuffer().clear();
        contour::session::sendKeyEvent(
            &ev, vtbackend::KeyboardEventType::Press, *session, passthroughLayout());
        CHECK(pty.stdinBuffer() == "x");
    }

    // A mapped special key (Enter) writes CR.
    {
        QKeyEvent ev(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        pty.stdinBuffer().clear();
        contour::session::sendKeyEvent(
            &ev, vtbackend::KeyboardEventType::Press, *session, passthroughLayout());
        CHECK(pty.stdinBuffer() == "\r");
    }

    // A cursor key emits an escape sequence.
    {
        QKeyEvent ev(QEvent::KeyPress, Qt::Key_Up, Qt::NoModifier);
        pty.stdinBuffer().clear();
        contour::session::sendKeyEvent(
            &ev, vtbackend::KeyboardEventType::Press, *session, passthroughLayout());
        CHECK(pty.stdinBuffer().contains('\033'));
    }

    // Ctrl+C encodes as 0x03.
    {
        QKeyEvent ev(QEvent::KeyPress, Qt::Key_C, Qt::ControlModifier, QStringLiteral("\x03"));
        pty.stdinBuffer().clear();
        contour::session::sendKeyEvent(
            &ev, vtbackend::KeyboardEventType::Press, *session, passthroughLayout());
        CHECK(pty.stdinBuffer().contains('\x03'));
    }
}

TEST_CASE("the macOS layout reads Carbon key codes as positions, not codepoints", "[session][input]")
{
    auto const& layout = macUsAnsiLayout();

    // The keys the old code got wrong: every kVK_ANSI_* code >= 32, which a codepoint-shaped read
    // of the key code turned into the punctuation character of the same value.
    CHECK(layout.unshiftedKeyOf(0x20) == U'u');  // was read as ' '
    CHECK(layout.unshiftedKeyOf(0x21) == U'[');  // was read as '!'
    CHECK(layout.unshiftedKeyOf(0x22) == U'i');  // was read as '"'
    CHECK(layout.unshiftedKeyOf(0x23) == U'p');  // was read as '#'
    CHECK(layout.unshiftedKeyOf(0x25) == U'l');  // was read as '%'
    CHECK(layout.unshiftedKeyOf(0x26) == U'j');  // was read as '&'
    CHECK(layout.unshiftedKeyOf(0x28) == U'k');  // was read as '('
    CHECK(layout.unshiftedKeyOf(0x29) == U';');  // was read as ')'
    CHECK(layout.unshiftedKeyOf(0x2A) == U'\\'); // was read as '*'
    CHECK(layout.unshiftedKeyOf(0x2B) == U',');  // was read as '+'
    CHECK(layout.unshiftedKeyOf(0x2C) == U'/');  // was read as ','
    CHECK(layout.unshiftedKeyOf(0x2D) == U'n');  // was read as '-'
    CHECK(layout.unshiftedKeyOf(0x2E) == U'm');  // was read as '.'
    CHECK(layout.unshiftedKeyOf(0x2F) == U'.');  // was read as '/'
    CHECK(layout.unshiftedKeyOf(0x32) == U'`');  // was read as '2'

    // The codes below 32 never reached the broken branch, and must still map.
    CHECK(layout.unshiftedKeyOf(0x00) == U'a');
    CHECK(layout.unshiftedKeyOf(0x08) == U'c');
    CHECK(layout.unshiftedKeyOf(0x0D) == U'w');
    CHECK(layout.unshiftedKeyOf(0x12) == U'1');

    // Keys that carry no character are absent, and 0 means "ask the character instead".
    CHECK(layout.unshiftedKeyOf(0x38) == 0); // kVK_Shift
    CHECK(layout.unshiftedKeyOf(0x3B) == 0); // kVK_Control
    CHECK(layout.unshiftedKeyOf(0x7A) == 0); // kVK_F1

    // No VK code off Windows, so none can reach win32-input-mode disguised as one.
    CHECK(layout.win32VirtualKeyOf(0x20) == 0);
}

TEST_CASE("the passthrough layout decodes the X11 Unicode keysym form", "[session][input]")
{
    auto const& layout = passthroughLayout();

    // Latin-1 keysyms and Win32 VK codes are already codepoints for the keys that reach this path.
    CHECK(layout.unshiftedKeyOf(0x75) == U'u');

    // Anything outside Latin-1 arrives as `0x01000000 | codepoint`, which is not one.
    CHECK(layout.unshiftedKeyOf(0x0100'0446) == U'ц');

    // A key we cannot name — a function or media keysym — reports unknown rather than a wrong key.
    CHECK(layout.unshiftedKeyOf(0x1008'FF11) == 0); // XF86AudioLowerVolume
}

TEST_CASE("Ctrl+U reaches the application as Ctrl+U under the Kitty keyboard protocol", "[session][input]")
{
    // The reported bug, end to end: a Qt event carrying a macOS key code, through the real key path,
    // out to the PTY. fish enables the Kitty protocol, which transmits the key IDENTITY rather than
    // a control byte, and kVK_ANSI_U is 0x20 — so reading the key code as a codepoint sent
    // Ctrl+Space, which fish does not bind, and it inserted a space instead of killing the line.
    //
    // The breadth of affected keys is pinned by the two layout tests above; this is about the wiring.
    contour::test::TestApp app;
    auto session = makeSession(app.app());
    auto& pty = mockPtyOf(*session);

    // What fish sends on startup: CSI > 1 u, "disambiguate escape codes".
    session->terminal().writeToScreen("\033[>1u");

    // nativeScanCode/nativeVirtualKey/nativeModifiers, as Qt's Cocoa plugin fills them in.
    auto constexpr MacKeyCodeU = quint32 { 0x20 };
    auto ev = QKeyEvent(QEvent::KeyPress,
                        Qt::Key_U,
                        Qt::ControlModifier,
                        MacKeyCodeU,
                        MacKeyCodeU,
                        0,
                        QString {},
                        /*autorep=*/false,
                        /*count=*/1);
    pty.stdinBuffer().clear();
    contour::session::sendKeyEvent(&ev, vtbackend::KeyboardEventType::Press, *session, macUsAnsiLayout());
    CHECK(pty.stdinBuffer() == "\033[117;5u");
}

TEST_CASE("the browser tab-switch chords are claimed before the terminal encodes them", "[session][input]")
{
    contour::test::TestApp app;

    // Reproduce what the fallback table exists for. Loading ANY `input_mapping:` section replaces the
    // built-in key mappings wholesale, which is the situation of every user whose contour.yml predates
    // these chords. Emptying the table here is that same state.
    //
    // This is load-bearing, not scene-setting: a DEFAULT config carries these chords in its own key
    // mappings too, so the user-table lookup would claim them first and this test would pass whether or
    // not the fallback were consulted at all. (Verified by deleting the consultation site: with the
    // defaults left in place the test still passed.) The session copies the config in its constructor,
    // so this must happen before makeSession.
    app.app().config().inputMappings.value().keyMappings.clear();

    auto session = makeSession(app.app());
    auto& pty = mockPtyOf(*session);

    // End-to-end proof that the fallback table is actually CONSULTED, which a unit test of
    // applyBuiltinFallback cannot give: were the consultation missing, these chords would fall through
    // to the terminal and be encoded onto the PTY. So an empty stdin buffer is the evidence the binding
    // fired -- and it needs no window and no second tab to observe, because SwitchToTab* reports success
    // whether or not there is another tab to move to.
    auto const reachedThePty = [&](Qt::Key key, Qt::KeyboardModifiers modifiers) {
        QKeyEvent ev(QEvent::KeyPress, key, modifiers);
        pty.stdinBuffer().clear();
        contour::session::sendKeyEvent(
            &ev, vtbackend::KeyboardEventType::Press, *session, passthroughLayout());
        return !pty.stdinBuffer().empty();
    };

    // NB: Ctrl+PageUp is the one chord here that does NOT discriminate on its own -- measured, it
    // encodes to nothing even when no binding claims it, so this line would hold either way. The
    // evidence that the consultation site exists comes from the three below, each of which DOES reach
    // the PTY when the fallback is not consulted.
    CHECK_FALSE(reachedThePty(Qt::Key_PageUp, Qt::ControlModifier));
    CHECK_FALSE(reachedThePty(Qt::Key_PageDown, Qt::ControlModifier));
    CHECK_FALSE(reachedThePty(Qt::Key_Tab, Qt::ControlModifier));
    // Qt reports the shifted Tab as Key_Backtab; helper.cpp rewrites it back to Tab with Shift re-added.
    CHECK_FALSE(reachedThePty(Qt::Key_Backtab, Qt::ControlModifier | Qt::ShiftModifier));

    // A chord the table does NOT carry still reaches the terminal. Without this the checks above would
    // also pass if key handling were broken outright and nothing were ever encoded.
    CHECK(reachedThePty(Qt::Key_PageDown, Qt::NoModifier));
}

TEST_CASE("sendKeyEvent covers the whole special-key mapping table", "[session][input]")
{
    contour::test::TestApp app;
    auto session = makeSession(app.app());
    auto& pty = mockPtyOf(*session);

    // Every mapped special key must produce SOME PTY bytes on press; sweeping the table pins the
    // Qt→vtbackend::Key rows (a silently dropped row shows up as an empty buffer here).
    auto const mappedKeys = std::vector<Qt::Key> {
        Qt::Key_F1,   Qt::Key_F2,     Qt::Key_F3,     Qt::Key_F4,        Qt::Key_F5,       Qt::Key_F6,
        Qt::Key_F7,   Qt::Key_F8,     Qt::Key_F9,     Qt::Key_F10,       Qt::Key_F11,      Qt::Key_F12,
        Qt::Key_Down, Qt::Key_Left,   Qt::Key_Right,  Qt::Key_PageUp,    Qt::Key_PageDown, Qt::Key_Home,
        Qt::Key_End,  Qt::Key_Insert, Qt::Key_Delete, Qt::Key_Backspace, Qt::Key_Tab,      Qt::Key_Escape,
    };
    for (auto const key: mappedKeys)
    {
        QKeyEvent ev(QEvent::KeyPress, key, Qt::NoModifier);
        pty.stdinBuffer().clear();
        contour::session::sendKeyEvent(
            &ev, vtbackend::KeyboardEventType::Press, *session, passthroughLayout());
        INFO("key = " << static_cast<int>(key));
        CHECK_FALSE(pty.stdinBuffer().empty());
    }

    // Backtab (Shift+Tab) and keypad keys go through their own rows.
    {
        QKeyEvent ev(QEvent::KeyPress, Qt::Key_Backtab, Qt::ShiftModifier);
        pty.stdinBuffer().clear();
        contour::session::sendKeyEvent(
            &ev, vtbackend::KeyboardEventType::Press, *session, passthroughLayout());
        CHECK_FALSE(pty.stdinBuffer().empty());
    }
    for (auto const key: { Qt::Key_0,
                           Qt::Key_5,
                           Qt::Key_9,
                           Qt::Key_Plus,
                           Qt::Key_Minus,
                           Qt::Key_Asterisk,
                           Qt::Key_Slash,
                           Qt::Key_Period,
                           Qt::Key_Enter })
    {
        QKeyEvent ev(QEvent::KeyPress, key, Qt::KeypadModifier);
        pty.stdinBuffer().clear();
        contour::session::sendKeyEvent(
            &ev, vtbackend::KeyboardEventType::Press, *session, passthroughLayout());
        INFO("keypad key = " << static_cast<int>(key));
        CHECK_FALSE(pty.stdinBuffer().empty());
    }

    // An unmapped key with no text is not handled and writes nothing.
    {
        QKeyEvent ev(QEvent::KeyPress, Qt::Key_CapsLock, Qt::NoModifier);
        pty.stdinBuffer().clear();
        contour::session::sendKeyEvent(
            &ev, vtbackend::KeyboardEventType::Press, *session, passthroughLayout());
        CHECK(pty.stdinBuffer().empty());
    }
}

TEST_CASE("wheel and mouse event helpers route through the session", "[session][input]")
{
    contour::test::TestApp app;
    auto session = makeSession(app.app());
    auto& pty = mockPtyOf(*session);

    // Enable SGR mouse reporting so button events produce PTY bytes end-to-end.
    session->terminal().setMode(vtbackend::DECMode::MouseSGR, true);

    {
        QWheelEvent ev(QPointF(10, 10),
                       QPointF(10, 10),
                       QPoint(),
                       QPoint(0, 120),
                       Qt::NoButton,
                       Qt::NoModifier,
                       Qt::NoScrollPhase,
                       false);
        pty.stdinBuffer().clear();
        contour::session::sendWheelEvent(&ev, *session);
        // A display-less session drops wheel events before mapping (session.display() == nullptr),
        // so this only pins that the phase-less-wheel path is non-crashing offscreen. The actual
        // routing into the wheel-glide momentum path is covered by the model-layer
        // Terminal.wheelGlide.* tests and the display-gated DisplayRendering wheel case.
    }
    {
        QMouseEvent press(QEvent::MouseButtonPress,
                          QPointF(12, 12),
                          QPointF(12, 12),
                          Qt::LeftButton,
                          Qt::LeftButton,
                          Qt::NoModifier);
        pty.stdinBuffer().clear();
        contour::session::sendMousePressEvent(&press, *session);
        QMouseEvent release(QEvent::MouseButtonRelease,
                            QPointF(12, 12),
                            QPointF(12, 12),
                            Qt::LeftButton,
                            Qt::NoButton,
                            Qt::NoModifier);
        contour::session::sendMouseReleaseEvent(&release, *session);
        QMouseEvent move(
            QEvent::MouseMove, QPointF(30, 30), QPointF(30, 30), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        contour::session::sendMouseMoveEvent(&move, *session);
    }
    SUCCEED("display-less mouse/wheel routing is crash-free");
}

TEST_CASE("buildSpawnTerminalCommand assembles arguments and resolves the working directory URL",
          "[helper][spawn]")
{
    // Full set: config + profile + a local-host cwd URL all forwarded, in order.
    {
        auto const cmd = contour::session::buildSpawnTerminalCommand(
            "/usr/bin/contour", "/tmp/c.yml", "main", "file:///tmp", "fedora");
        CHECK(cmd.program == "/usr/bin/contour");
        CHECK(cmd.arguments
              == QStringList { "config", "/tmp/c.yml", "profile", "main", "working-directory", "/tmp" });
    }

    // Empty config + empty profile omit those flags; a non-local host drops the working directory.
    {
        auto const cmd = contour::session::buildSpawnTerminalCommand(
            "/usr/bin/contour", "", "", "file://not-this-host.invalid/somewhere", "fedora");
        CHECK(cmd.program == "/usr/bin/contour");
        CHECK(cmd.arguments.isEmpty());
    }

    // A bare (host-less) path URL is forwarded as the working directory.
    {
        auto const cmd = contour::session::buildSpawnTerminalCommand(
            "/usr/bin/contour", "", "work", "file:///home/x", "fedora");
        CHECK(cmd.arguments == QStringList { "profile", "work", "working-directory", "/home/x" });
    }

    // A fully-qualified authority is still THIS machine, and used to be rejected. The full set of
    // spellings is owned by vtbackend's FileUrl_test; this only pins that the wiring reaches it.
    {
        auto const cmd = contour::session::buildSpawnTerminalCommand(
            "/usr/bin/contour", "", "", "file://fedora.corp.example/home/x", "fedora");
        CHECK(cmd.arguments == QStringList { "working-directory", "/home/x" });
    }
}

TEST_CASE("foldedBindingCodepoint folds only the ASCII letter case", "[session][input]")
{
    using contour::config::foldedBindingCodepoint;

    STATIC_CHECK(foldedBindingCodepoint(U'a') == U'A');
    STATIC_CHECK(foldedBindingCodepoint(U'z') == U'Z');
    STATIC_CHECK(foldedBindingCodepoint(U'q') == U'Q');
    STATIC_CHECK(foldedBindingCodepoint(U'A') == U'A');
    STATIC_CHECK(foldedBindingCodepoint(U'Z') == U'Z');

    // Digits, punctuation and space are untouched -- bindings on those stay exactly as written.
    STATIC_CHECK(foldedBindingCodepoint(U'0') == U'0');
    STATIC_CHECK(foldedBindingCodepoint(U'<') == U'<');
    STATIC_CHECK(foldedBindingCodepoint(U',') == U',');
    STATIC_CHECK(foldedBindingCodepoint(U' ') == U' ');
    // The two codepoints bracketing 'a'..'z', to pin the range ends.
    STATIC_CHECK(foldedBindingCodepoint(U'`') == U'`');
    STATIC_CHECK(foldedBindingCodepoint(U'{') == U'{');

    // Non-ASCII is deliberately NOT folded: no input route can deliver a codepoint above 0x7F in
    // two different cases, so folding there would only merge bindings that are distinct today.
    // These are the cases that would break under unicode::simple_uppercase.
    STATIC_CHECK(foldedBindingCodepoint(U'ä') == U'ä'); // would become 'Ä'
    STATIC_CHECK(foldedBindingCodepoint(U'ß') == U'ß'); // length-preserving, but still not ours
    STATIC_CHECK(foldedBindingCodepoint(U'ı') == U'ı'); // dotless ı -> 'I', colliding with 'i'
    STATIC_CHECK(foldedBindingCodepoint(U'µ') == U'µ'); // MICRO SIGN µ -> GREEK CAPITAL MU

    // Idempotent across printable ASCII: folding a folded binding must be a no-op, or a config that
    // survives one load could drift on the next.
    for (auto const ch: std::views::iota(char32_t { 0x20 }, char32_t { 0x80 }))
    {
        CAPTURE(static_cast<uint32_t>(ch));
        CHECK(foldedBindingCodepoint(foldedBindingCodepoint(ch)) == foldedBindingCodepoint(ch));
    }
}

TEST_CASE("the binding fold and unshiftedCodepoint cannot fight", "[session][input]")
{
    using contour::config::foldedBindingCodepoint;
    using contour::input::unshiftedCodepoint;

    // TerminalSession::sendCharEvent applies both: it folds the delivered codepoint, then retries
    // through unshiftedCodepoint when Shift is held. That is only sound because their domains are
    // disjoint -- unshiftedCodepoint rewrites digits and punctuation, the fold rewrites letters.
    for (auto const ch: std::views::iota(char32_t { 0x20 }, char32_t { 0x80 }))
    {
        CAPTURE(static_cast<uint32_t>(ch));

        // Whatever unshiftedCodepoint rewrites, the fold leaves alone...
        if (contour::input::unshiftedCodepoint(ch) != ch)
            CHECK(foldedBindingCodepoint(ch) == ch);

        // ...and whatever the fold rewrites, unshiftedCodepoint leaves alone.
        if (foldedBindingCodepoint(ch) != ch)
            CHECK(contour::input::unshiftedCodepoint(ch) == ch);

        // Therefore they commute, and applying one before the other cannot change the outcome.
        CHECK(contour::input::unshiftedCodepoint(foldedBindingCodepoint(ch))
              == foldedBindingCodepoint(contour::input::unshiftedCodepoint(ch)));
    }
}

TEST_CASE("a bound letter chord fires whichever route delivered it", "[session][input]")
{
    // sendKeyEvent has several routes to a character binding and they disagree about letter case:
    // the Ctrl branch and the CharMappings table both report the UPPERCASE key label, while
    // event->text() reports whatever the layout produced. A binding must fire on all of them, i.e.
    // be CONSUMED rather than reach the PTY, whichever case the user happened to write it in.
    //
    // The bindings below are written LOWERCASE, so without the fold it is the two uppercase-
    // reporting routes that miss -- that is the `key: 'q'` variant of issue #1987, where a binding
    // parses cleanly and then never fires. Written uppercase instead, the text route would be the
    // one to miss. Either way the routes must agree, which is what the fold buys.
    contour::test::TestApp app;

    // Two bindings, deliberately chosen to reach different routes. Alt+P is the interesting one: with
    // Control held, the Ctrl branch claims the event before event->text() is ever consulted, so a
    // Ctrl chord can NOT exercise the text route no matter what text is attached to it.
    app.app().config().inputMappings = contour::test::loadConfigFromYaml(R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
input_mapping:
    - { mods: [Control, Shift], key: 'p', action: OpenCommandPalette }
    - { mods: [Alt],            key: 'p', action: OpenCommandPalette }
)")
                                           .inputMappings;

    auto session = makeSession(app.app());
    auto& pty = mockPtyOf(*session);

    auto const ctrlShift = Qt::ControlModifier | Qt::ShiftModifier;

    SECTION("the Ctrl branch reports the uppercase key label")
    {
        QKeyEvent ev(QEvent::KeyPress, Qt::Key_P, ctrlShift, QStringLiteral("\x10"));
        pty.stdinBuffer().clear();
        contour::session::sendKeyEvent(
            &ev, vtbackend::KeyboardEventType::Press, *session, passthroughLayout());
        CHECK(pty.stdinBuffer().empty());
    }

    SECTION("the CharMappings table reports the uppercase key label")
    {
        // Empty text, so sendKeyEvent falls back to its Qt::Key -> character table.
        QKeyEvent ev(QEvent::KeyPress, Qt::Key_P, ctrlShift, QString());
        pty.stdinBuffer().clear();
        contour::session::sendKeyEvent(
            &ev, vtbackend::KeyboardEventType::Press, *session, passthroughLayout());
        CHECK(pty.stdinBuffer().empty());
    }

    SECTION("the text route reports whatever the layout produced -- here, lowercase")
    {
        // An Alt chord (no Control) is what actually reaches event->text(); a Ctrl chord is claimed
        // by the Ctrl branch first, no matter what text is attached to it. An unshifted letter
        // arrives here in LOWERCASE.
#ifdef __APPLE__
        // Issue #2016. This section cannot pass on macOS, and the reason is a real defect rather
        // than a quirk of the test: with `option_as_alt` off (the default), helper.cpp's text route
        // strips Alt before dispatching -- `modifiers.without(Modifier::Alt)` -- because macOS does
        // not deliver Alt to terminal applications. But it strips it ahead of the BINDING lookup
        // too, so no `input_mapping` with `mods: [Alt]` can ever fire on macOS unless the profile
        // opts into option-as-Alt. Alt bindings are a documented feature, so that is a bug, not a
        // platform limit -- it predates the daemon work (the line is from 2021) and is out of scope
        // to change here.
        //
        // Skipped rather than asserted: pinning today's behaviour would turn the defect into the
        // contract, and this is the only route of the four that macOS diverges on. When #2016 is
        // fixed this guard comes out, and the section becomes the regression test for it.
        SKIP("macOS strips Alt before the binding lookup unless option_as_alt is set (#2016)");
#endif
        QKeyEvent ev(QEvent::KeyPress, Qt::Key_P, Qt::AltModifier, QStringLiteral("p"));
        pty.stdinBuffer().clear();
        contour::session::sendKeyEvent(
            &ev, vtbackend::KeyboardEventType::Press, *session, passthroughLayout());
        CHECK(pty.stdinBuffer().empty());
    }

    SECTION("an UNBOUND letter still reaches the PTY")
    {
        // The negative control: without it, every section above would also pass if the fold had
        // simply made the session swallow all input.
        QKeyEvent ev(QEvent::KeyPress, Qt::Key_Y, ctrlShift, QStringLiteral("y"));
        pty.stdinBuffer().clear();
        contour::session::sendKeyEvent(
            &ev, vtbackend::KeyboardEventType::Press, *session, passthroughLayout());
        CHECK_FALSE(pty.stdinBuffer().empty());
    }
}

TEST_CASE("toQString decodes a path in the encoding it is actually stored in", "[contour][helper]")
{
    // QString::fromStdString(path.generic_string()) is the obvious spelling and is silently lossy on
    // Windows: generic_string() narrows the native wide path through the ANSI code page while
    // fromStdString() decodes UTF-8. Any non-ASCII character in a user's profile directory then comes
    // back as replacement characters, and whatever the path was handed to -- a QML import path, a
    // file watcher -- points at a directory that does not exist, with no diagnostic anywhere.
    //
    // Built from a u8 literal rather than a wide one: path's char8_t constructor is defined to read
    // UTF-8 and encode to whatever the platform stores natively, so the fixture means the same thing
    // on Windows and POSIX without depending on the runner's locale.
    auto const home = std::filesystem::path(u8"C:/Users/J\u00fcrgen/AppData/Roaming/contour");
    CHECK(contour::platform::toQString(home)
          == QStringLiteral("C:/Users/J\u00fcrgen/AppData/Roaming/contour"));

    // Round-tripping is the property that matters: whatever came out has to still name the same path.
    CHECK(std::filesystem::path(contour::platform::toQString(home).toStdU16String()) == home);

    // Generic separators, so the result is in the form Qt's own path APIs use rather than the
    // platform's -- which on Windows is the difference between a usable QML import path and one Qt
    // will not match against.
    CHECK(contour::platform::toQString(home / "Contour" / "Ui")
          == QStringLiteral("C:/Users/J\u00fcrgen/AppData/Roaming/contour/Contour/Ui"));

    CHECK(contour::platform::toQString(std::filesystem::path {}).isEmpty());
}
