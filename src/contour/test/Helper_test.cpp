// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the Qt→vtbackend input translation helpers (helper.h/helper.cpp): the pure
// mapping functions every key/mouse event flows through before reaching a terminal session.

#include <contour/Config.h>
#include <contour/ContourGuiApp.h>
#include <contour/TerminalSession.h>
#include <contour/helper.h>
#include <contour/test/GuiTestFixtures.h>

#include <vtbackend/InputGenerator.h>

#include <vtpty/MockPty.h>

#include <QtGui/QKeyEvent>
#include <QtGui/QMouseEvent>
#include <QtGui/QWheelEvent>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <memory>
#include <ranges>

using contour::makeModifiers;
using contour::makeMouseButton;

TEST_CASE("makeMouseButton maps Qt buttons onto VT buttons", "[helper][input]")
{
    STATIC_CHECK(makeMouseButton(Qt::LeftButton) == vtbackend::MouseButton::Left);
    STATIC_CHECK(makeMouseButton(Qt::MiddleButton) == vtbackend::MouseButton::Middle);
    STATIC_CHECK(makeMouseButton(Qt::RightButton) == vtbackend::MouseButton::Right);
    // Unknown buttons deliberately degrade to Left (the safest default for VT mouse reports).
    STATIC_CHECK(makeMouseButton(Qt::BackButton) == vtbackend::MouseButton::Left);
}

TEST_CASE("makeModifiers maps Qt keyboard modifiers onto VT modifiers", "[helper][input]")
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

TEST_CASE("unshiftedCodepoint inverts the US-ASCII shift level", "[helper][input]")
{
    using contour::unshiftedCodepoint;
    // Punctuation and number-row shifted symbols map back to the base key label a binding is written
    // with (this is what lets `Ctrl+Shift+,` fire when Qt delivers the shifted '<').
    CHECK(unshiftedCodepoint(U'<') == U',');
    CHECK(unshiftedCodepoint(U'>') == U'.');
    CHECK(unshiftedCodepoint(U'?') == U'/');
    CHECK(unshiftedCodepoint(U':') == U';');
    CHECK(unshiftedCodepoint(U'"') == U'\'');
    CHECK(unshiftedCodepoint(U'{') == U'[');
    CHECK(unshiftedCodepoint(U'|') == U'\\');
    CHECK(unshiftedCodepoint(U'_') == U'-');
    CHECK(unshiftedCodepoint(U'+') == U'=');
    CHECK(unshiftedCodepoint(U'!') == U'1');
    CHECK(unshiftedCodepoint(U'@') == U'2');
    CHECK(unshiftedCodepoint(U')') == U'0');
    // Non-shifted symbols and letters (shift-invariant here) are returned unchanged.
    CHECK(unshiftedCodepoint(U',') == U',');
    CHECK(unshiftedCodepoint(U'P') == U'P');
    CHECK(unshiftedCodepoint(U'a') == U'a');
    CHECK(unshiftedCodepoint(U'5') == U'5');
}

#if !defined(_WIN32) && !defined(__APPLE__)
TEST_CASE("makeModifiers derives CapsLock/NumLock from the X11 native modifier mask", "[helper][input]")
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
[[nodiscard]] std::unique_ptr<contour::TerminalSession> makeSession(contour::ContourGuiApp& app)
{
    auto pty =
        std::make_unique<vtpty::MockPty>(vtpty::PageSize { vtpty::LineCount(24), vtpty::ColumnCount(80) });
    return std::make_unique<contour::TerminalSession>(&app.sessionsManager(), std::move(pty), app);
}

using contour::test::mockPtyOf;
} // namespace

TEST_CASE("sendKeyEvent maps Qt key events onto the terminal's PTY encoding", "[helper][input]")
{
    contour::test::TestApp app;
    auto session = makeSession(app.app());
    auto& pty = mockPtyOf(*session);

    // A printable character key writes its text.
    {
        QKeyEvent ev(QEvent::KeyPress, Qt::Key_X, Qt::NoModifier, QStringLiteral("x"));
        pty.stdinBuffer().clear();
        contour::sendKeyEvent(&ev, vtbackend::KeyboardEventType::Press, *session);
        CHECK(pty.stdinBuffer() == "x");
    }

    // A mapped special key (Enter) writes CR.
    {
        QKeyEvent ev(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        pty.stdinBuffer().clear();
        contour::sendKeyEvent(&ev, vtbackend::KeyboardEventType::Press, *session);
        CHECK(pty.stdinBuffer() == "\r");
    }

    // A cursor key emits an escape sequence.
    {
        QKeyEvent ev(QEvent::KeyPress, Qt::Key_Up, Qt::NoModifier);
        pty.stdinBuffer().clear();
        contour::sendKeyEvent(&ev, vtbackend::KeyboardEventType::Press, *session);
        CHECK(pty.stdinBuffer().find('\033') != std::string::npos);
    }

    // Ctrl+C encodes as 0x03.
    {
        QKeyEvent ev(QEvent::KeyPress, Qt::Key_C, Qt::ControlModifier, QStringLiteral("\x03"));
        pty.stdinBuffer().clear();
        contour::sendKeyEvent(&ev, vtbackend::KeyboardEventType::Press, *session);
        CHECK(pty.stdinBuffer().find('\x03') != std::string::npos);
    }
}

TEST_CASE("the browser tab-switch chords are claimed before the terminal encodes them", "[helper][input]")
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
        contour::sendKeyEvent(&ev, vtbackend::KeyboardEventType::Press, *session);
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

TEST_CASE("sendKeyEvent covers the whole special-key mapping table", "[helper][input]")
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
        contour::sendKeyEvent(&ev, vtbackend::KeyboardEventType::Press, *session);
        INFO("key = " << static_cast<int>(key));
        CHECK_FALSE(pty.stdinBuffer().empty());
    }

    // Backtab (Shift+Tab) and keypad keys go through their own rows.
    {
        QKeyEvent ev(QEvent::KeyPress, Qt::Key_Backtab, Qt::ShiftModifier);
        pty.stdinBuffer().clear();
        contour::sendKeyEvent(&ev, vtbackend::KeyboardEventType::Press, *session);
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
        contour::sendKeyEvent(&ev, vtbackend::KeyboardEventType::Press, *session);
        INFO("keypad key = " << static_cast<int>(key));
        CHECK_FALSE(pty.stdinBuffer().empty());
    }

    // An unmapped key with no text is not handled and writes nothing.
    {
        QKeyEvent ev(QEvent::KeyPress, Qt::Key_CapsLock, Qt::NoModifier);
        pty.stdinBuffer().clear();
        contour::sendKeyEvent(&ev, vtbackend::KeyboardEventType::Press, *session);
        CHECK(pty.stdinBuffer().empty());
    }
}

TEST_CASE("wheel and mouse event helpers route through the session", "[helper][input]")
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
        contour::sendWheelEvent(&ev, *session);
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
        contour::sendMousePressEvent(&press, *session);
        QMouseEvent release(QEvent::MouseButtonRelease,
                            QPointF(12, 12),
                            QPointF(12, 12),
                            Qt::LeftButton,
                            Qt::NoButton,
                            Qt::NoModifier);
        contour::sendMouseReleaseEvent(&release, *session);
        QMouseEvent move(
            QEvent::MouseMove, QPointF(30, 30), QPointF(30, 30), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        contour::sendMouseMoveEvent(&move, *session);
    }
    SUCCEED("display-less mouse/wheel routing is crash-free");
}

TEST_CASE("buildSpawnTerminalCommand assembles arguments and resolves the working directory URL",
          "[helper][spawn]")
{
    // Full set: config + profile + a local-host cwd URL all forwarded, in order.
    {
        auto const cmd =
            contour::buildSpawnTerminalCommand("/usr/bin/contour", "/tmp/c.yml", "main", "file:///tmp");
        CHECK(cmd.program == "/usr/bin/contour");
        CHECK(cmd.arguments
              == QStringList { "config", "/tmp/c.yml", "profile", "main", "working-directory", "/tmp" });
    }

    // Empty config + empty profile omit those flags; a non-local host drops the working directory.
    {
        auto const cmd = contour::buildSpawnTerminalCommand(
            "/usr/bin/contour", "", "", "file://not-this-host.invalid/somewhere");
        CHECK(cmd.program == "/usr/bin/contour");
        CHECK(cmd.arguments.isEmpty());
    }

    // A bare (host-less) path URL is forwarded as the working directory.
    {
        auto const cmd = contour::buildSpawnTerminalCommand("/usr/bin/contour", "", "work", "file:///home/x");
        CHECK(cmd.arguments == QStringList { "profile", "work", "working-directory", "/home/x" });
    }
}

TEST_CASE("foldedBindingCodepoint folds only the ASCII letter case", "[helper][input]")
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

TEST_CASE("the binding fold and unshiftedCodepoint cannot fight", "[helper][input]")
{
    using contour::unshiftedCodepoint;
    using contour::config::foldedBindingCodepoint;

    // TerminalSession::sendCharEvent applies both: it folds the delivered codepoint, then retries
    // through unshiftedCodepoint when Shift is held. That is only sound because their domains are
    // disjoint -- unshiftedCodepoint rewrites digits and punctuation, the fold rewrites letters.
    for (auto const ch: std::views::iota(char32_t { 0x20 }, char32_t { 0x80 }))
    {
        CAPTURE(static_cast<uint32_t>(ch));

        // Whatever unshiftedCodepoint rewrites, the fold leaves alone...
        if (unshiftedCodepoint(ch) != ch)
            CHECK(foldedBindingCodepoint(ch) == ch);

        // ...and whatever the fold rewrites, unshiftedCodepoint leaves alone.
        if (foldedBindingCodepoint(ch) != ch)
            CHECK(unshiftedCodepoint(ch) == ch);

        // Therefore they commute, and applying one before the other cannot change the outcome.
        CHECK(unshiftedCodepoint(foldedBindingCodepoint(ch))
              == foldedBindingCodepoint(unshiftedCodepoint(ch)));
    }
}

TEST_CASE("a bound letter chord fires whichever route delivered it", "[helper][input]")
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
        contour::sendKeyEvent(&ev, vtbackend::KeyboardEventType::Press, *session);
        CHECK(pty.stdinBuffer().empty());
    }

    SECTION("the CharMappings table reports the uppercase key label")
    {
        // Empty text, so sendKeyEvent falls back to its Qt::Key -> character table.
        QKeyEvent ev(QEvent::KeyPress, Qt::Key_P, ctrlShift, QString());
        pty.stdinBuffer().clear();
        contour::sendKeyEvent(&ev, vtbackend::KeyboardEventType::Press, *session);
        CHECK(pty.stdinBuffer().empty());
    }

    SECTION("the text route reports whatever the layout produced -- here, lowercase")
    {
        // An Alt chord (no Control) is what actually reaches event->text(); a Ctrl chord is claimed
        // by the Ctrl branch first, no matter what text is attached to it. An unshifted letter
        // arrives here in LOWERCASE. On macOS this same press is claimed by the option-as-Alt branch
        // instead, which derives the case from Shift XOR CapsLock and so also delivers 'p' -- the
        // fold is what stops a latched lock key from deciding whether a shortcut fires.
        QKeyEvent ev(QEvent::KeyPress, Qt::Key_P, Qt::AltModifier, QStringLiteral("p"));
        pty.stdinBuffer().clear();
        contour::sendKeyEvent(&ev, vtbackend::KeyboardEventType::Press, *session);
        CHECK(pty.stdinBuffer().empty());
    }

    SECTION("an UNBOUND letter still reaches the PTY")
    {
        // The negative control: without it, every section above would also pass if the fold had
        // simply made the session swallow all input.
        QKeyEvent ev(QEvent::KeyPress, Qt::Key_Y, ctrlShift, QStringLiteral("y"));
        pty.stdinBuffer().clear();
        contour::sendKeyEvent(&ev, vtbackend::KeyboardEventType::Press, *session);
        CHECK_FALSE(pty.stdinBuffer().empty());
    }
}
