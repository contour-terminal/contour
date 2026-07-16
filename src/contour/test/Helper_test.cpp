// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the Qt→vtbackend input translation helpers (helper.h/helper.cpp): the pure
// mapping functions every key/mouse event flows through before reaching a terminal session.

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

#include <memory>

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

[[nodiscard]] vtpty::MockPty& mockPtyOf(contour::TerminalSession& s)
{
    return dynamic_cast<vtpty::MockPty&>(s.terminal().device());
}
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
