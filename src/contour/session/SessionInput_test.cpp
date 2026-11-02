// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the Qt→vtbackend input translation helpers (helper.h/helper.cpp): the pure
// mapping functions every key/mouse event flows through before reaching a terminal session.

#include <contour/ContourGuiApp.hpp>
#include <contour/config/ConfigFwd.hpp>
#include <contour/input/KeyMapping.hpp>
#include <contour/input/KeyRouter.hpp>
#include <contour/input/KeyboardLayout.hpp>
#include <contour/input/KeyboardPlatform.hpp>
#include <contour/input/MouseMapping.hpp>
#include <contour/input/QtKeyCapture.hpp>
#include <contour/platform/QtPath.hpp>
#include <contour/session/SessionInput.hpp>
#include <contour/session/SpawnCommand.hpp>
#include <contour/session/TerminalSession.hpp>
#include <contour/test/AppFixture.hpp>
#include <contour/test/GuiTestFixtures.hpp>

#include <vtbackend/input/InputGenerator.hpp>

#include <vtpty/MockPty.hpp>

#include <QtGui/QKeyEvent>
#include <QtGui/QMouseEvent>
#include <QtGui/QWheelEvent>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <ranges>
#include <tuple>

using contour::input::makeMouseButton;

TEST_CASE("namedKeyOf names the keys that are terminal keys and no character", "[contour][input]")
{
    using contour::input::namedKeyOf;
    using contour::input::NamedKeyRows;

    // Every row answers for its own Qt key, and no Qt key is listed twice.
    for (auto const& row: NamedKeyRows)
    {
        REQUIRE(namedKeyOf(row.qt) == std::optional { row.key });
        CHECK(std::ranges::count(NamedKeyRows, row.qt, &contour::input::NamedKeyRow::qt) == 1);
    }

    CHECK(namedKeyOf(Qt::Key_F6) == std::optional { vtbackend::Key::F6 });
    CHECK(namedKeyOf(Qt::Key_Home) == std::optional { vtbackend::Key::Home });
    // A character key is matched by the char mappings, not by name.
    CHECK_FALSE(namedKeyOf(Qt::Key_A).has_value());
    CHECK_FALSE(namedKeyOf(Qt::Key_Space).has_value());
}

TEST_CASE("makeMouseButton maps Qt buttons onto VT buttons", "[session][input]")
{
    STATIC_CHECK(makeMouseButton(Qt::LeftButton) == vtbackend::MouseButton::Left);
    STATIC_CHECK(makeMouseButton(Qt::MiddleButton) == vtbackend::MouseButton::Middle);
    STATIC_CHECK(makeMouseButton(Qt::RightButton) == vtbackend::MouseButton::Right);
    // Unknown buttons deliberately degrade to Left (the safest default for VT mouse reports).
    STATIC_CHECK(makeMouseButton(Qt::BackButton) == vtbackend::MouseButton::Left);
}

TEST_CASE("chordOf maps Qt keyboard modifiers onto VT modifiers", "[session][input]")
{
    using contour::input::chordOf;

    CHECK(chordOf(Qt::NoModifier) == vtbackend::Modifiers {});
    CHECK(chordOf(Qt::ShiftModifier) == vtbackend::Modifiers { vtbackend::Modifier::Shift });
    CHECK(chordOf(Qt::ControlModifier) == vtbackend::Modifiers { vtbackend::Modifier::Control });
    CHECK(chordOf(Qt::AltModifier) == vtbackend::Modifiers { vtbackend::Modifier::Alt });
    CHECK(chordOf(Qt::MetaModifier) == vtbackend::Modifiers { vtbackend::Modifier::Super });

    auto const combined = chordOf(Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier);
    CHECK(combined.contains(vtbackend::Modifier::Shift));
    CHECK(combined.contains(vtbackend::Modifier::Control));
    CHECK(combined.contains(vtbackend::Modifier::Alt));
    CHECK_FALSE(combined.contains(vtbackend::Modifier::Super));
}

TEST_CASE("a mouse chord keeps Ctrl+Alt as Ctrl+Alt", "[session][input]")
{
    // A mouse event has no text to tell an AltGr level from a chord, so on Windows too Ctrl+Alt+click is
    // the chord it says.
    using contour::input::chordOf;
    CHECK(chordOf(Qt::ControlModifier | Qt::AltModifier)
          == vtbackend::Modifiers { vtbackend::Modifier::Control, vtbackend::Modifier::Alt });
}

namespace
{
/// The built-in character rows macOS carries: its tab, find and pane chords among them.
[[nodiscard]] std::vector<contour::config::FallbackCharMapping> const& macTable()
{
    return contour::config::builtinFallbackCharMappings(contour::config::KeyboardPlatformKind::Mac);
}
} // namespace

TEST_CASE("macOS Command chords reach the tab actions as Qt delivers them", "[session][input]")
{
    // The macOS host: Qt's Control/Meta swap is off, so Command is Qt::MetaModifier, which chordOf()
    // reads as Super; Shift+[ arrives as '{' and is retried under its unshifted base, as TerminalSession
    // does.
    using vtbackend::Modifier;
    auto const command = Qt::MetaModifier;
    auto const commandShift = Qt::MetaModifier | Qt::ShiftModifier;
    auto const lookUp = [](Qt::KeyboardModifiers qtModifiers, char32_t input) {
        auto const modifiers = contour::input::chordOf(qtModifiers);
        auto const folded = contour::config::foldedBindingCodepoint(input);
        auto const& rows = macTable();
        return std::ranges::find_if(rows, [&](auto const& row) {
            return row.mapping.input == contour::input::unshiftedCodepoint(folded)
                   && row.mapping.modifiers == modifiers;
        });
    };
    auto const& table = macTable();

    auto const newTab = lookUp(command, U't');
    REQUIRE(newTab != table.end());
    CHECK(std::holds_alternative<contour::actions::CreateNewTab>(newTab->mapping.binding.front()));

    auto const third = lookUp(command, U'3');
    REQUIRE(third != table.end());
    CHECK(std::get<contour::actions::SwitchToTab>(third->mapping.binding.front()).position == 3);

    auto const previous = lookUp(commandShift, U'{');
    REQUIRE(previous != table.end());
    CHECK(std::holds_alternative<contour::actions::SwitchToTabLeft>(previous->mapping.binding.front()));

    // Control is not Command: Ctrl+T stays with the application.
    CHECK(lookUp(Qt::ControlModifier, U't') == table.end());
}

TEST_CASE("macOS Command+D and Command+Shift+D reach the split actions as Qt delivers them",
          "[session][input][panes]")
{
    // Command is Qt::MetaModifier on macOS, which chordOf() reads as Super; the letter arrives folded,
    // as TerminalSession folds it before the lookup.
    auto const lookUp = [](Qt::KeyboardModifiers qtModifiers, char32_t input) {
        auto const modifiers = contour::input::chordOf(qtModifiers);
        auto const folded = contour::config::foldedBindingCodepoint(input);
        auto const& rows = macTable();
        return std::ranges::find_if(rows, [&](auto const& row) {
            return row.mapping.input == folded && row.mapping.modifiers == modifiers;
        });
    };
    auto const& table = macTable();

    auto const side = lookUp(Qt::MetaModifier, U'd');
    REQUIRE(side != table.end());
    CHECK(std::holds_alternative<contour::actions::SplitVertical>(side->mapping.binding.front()));

    auto const stacked = lookUp(Qt::MetaModifier | Qt::ShiftModifier, U'D');
    REQUIRE(stacked != table.end());
    CHECK(std::holds_alternative<contour::actions::SplitHorizontal>(stacked->mapping.binding.front()));

    // Control+Shift+E is the desktop's chord, not a Mac one.
    CHECK(lookUp(Qt::ControlModifier | Qt::ShiftModifier, U'e') == table.end());
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

TEST_CASE("xcbLockKeysOf derives CapsLock/NumLock from the X11 native modifier mask", "[session][input]")
{
    // On X11 and Wayland the lock states come from the XCB/XKB native mask (CapsLock = XCB_MOD_MASK_LOCK
    // 0x02, NumLock = XCB_MOD_MASK_2 0x10), independent of the Qt modifier bits.
    using contour::input::xcbLockKeysOf;
    constexpr quint32 XcbCapsLockMask = 0x02;
    constexpr quint32 XcbNumLockMask = 0x10;

    CHECK(xcbLockKeysOf(XcbCapsLockMask).contains(vtbackend::LockKey::CapsLock));
    CHECK(xcbLockKeysOf(XcbNumLockMask).contains(vtbackend::LockKey::NumLock));

    auto const both = xcbLockKeysOf(XcbCapsLockMask | XcbNumLockMask);
    CHECK(both.contains(vtbackend::LockKey::CapsLock));
    CHECK(both.contains(vtbackend::LockKey::NumLock));

    // No native bits set -> no lock keys.
    CHECK(xcbLockKeysOf(0).none());
}

namespace
{
/// Display-less session over a MockPty (the helper.cpp key path needs a session but no display).
[[nodiscard]] std::unique_ptr<contour::session::TerminalSession> makeSession(contour::ContourGuiApp& app)
{
    auto pty =
        std::make_unique<vtpty::MockPty>(vtpty::PageSize { vtpty::LineCount(24), vtpty::ColumnCount(80) });
    return contour::test::makeTestSession(&app.sessionsManager(), std::move(pty), app);
}

/// The layout for every test that is not about layouts: with no keymap it reads the native key identifier
/// as a keysym, which for the events built here (nativeVirtualKey 0) means "unknown".
[[nodiscard]] contour::input::KeyboardLayout const& xkbLayout()
{
    static auto const layout = contour::input::makeXkbKeyboardLayout(nullptr);
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

#ifdef _WIN32
/// Presses a key with Ctrl+Alt held, as Qt's Windows plugin reports both a genuine Ctrl+Alt chord
/// and the AltGr key, and returns what reached the PTY.
/// @param session The session to deliver the event to.
/// @param key The Qt key code.
/// @param virtualKey The Win32 VK code of the key.
/// @param text The text the platform produced for the event.
/// @return The bytes the event wrote to the PTY.
[[nodiscard]] std::string pressWithCtrlAlt(contour::session::TerminalSession& session,
                                           Qt::Key key,
                                           quint32 virtualKey,
                                           QString const& text)
{
    auto& pty = mockPtyOf(session);
    auto ev = QKeyEvent(QEvent::KeyPress, key, Qt::ControlModifier | Qt::AltModifier, 0, virtualKey, 0, text);
    pty.stdinBuffer().clear();
    contour::session::sendKeyEvent(&ev, vtbackend::KeyboardEventType::Press, session);
    return pty.stdinBuffer();
}
#endif
} // namespace

TEST_CASE("Option as Alt applies to the configured side only", "[session][input]")
{
    // The macOS normalizer, injected: its Option rule is tested on every host.
    using contour::config::OptionAsAlt;
    constexpr auto OptionLeft = uint32_t { 0x80000 | contour::input::MacLeftOptionBit };
    constexpr auto OptionRight = uint32_t { 0x80000 | contour::input::MacRightOptionBit };

    // What reaches the PTY for Option+E with @p option held, under @p setting.
    auto const typed = [](OptionAsAlt setting, uint32_t option) {
        contour::test::TestApp app;
        app.app().config().profile().optionAsAlt.value() = setting;
        auto session = contour::test::makeTestSession(
            &app.app().sessionsManager(),
            std::make_unique<vtpty::MockPty>(
                vtpty::PageSize { vtpty::LineCount(24), vtpty::ColumnCount(80) }),
            app.app(),
            contour::input::makeKeyboardPlatformFactory(contour::config::KeyboardPlatformKind::Mac,
                                                        macUsAnsiLayout()));
        auto& pty = mockPtyOf(*session);
        auto const event = contour::input::NativeKeyEvent {
            .qtKey = Qt::Key_E, .text = U"\u00e9", .qtModifiers = Qt::AltModifier, .nativeModifiers = option
        };
        pty.stdinBuffer().clear();
        std::ignore = session->sendKeyboardEvent(session->keyboardPlatform().normalize(event),
                                                 std::chrono::steady_clock::now());
        return pty.stdinBuffer();
    };
    auto const alt = std::string { "\x1b"
                                   "e" };
    auto const composed = std::string { "\xc3\xa9" };

    CHECK(typed(OptionAsAlt::Left, OptionLeft) == alt);
    CHECK(typed(OptionAsAlt::Left, OptionRight) == composed);
    CHECK(typed(OptionAsAlt::Right, OptionRight) == alt);
    CHECK(typed(OptionAsAlt::Right, OptionLeft) == composed);
    CHECK(typed(OptionAsAlt::Both, OptionLeft) == alt);
    CHECK(typed(OptionAsAlt::Both, OptionRight) == alt);
    CHECK(typed(OptionAsAlt::Off, OptionLeft) == composed);
    CHECK(typed(OptionAsAlt::Off, OptionRight) == composed);
}

TEST_CASE("sendKeyEvent maps Qt key events onto the terminal's PTY encoding", "[session][input]")
{
    contour::test::TestApp app;
    auto session = makeSession(app.app());
    auto& pty = mockPtyOf(*session);

    // A printable character key writes its text.
    {
        QKeyEvent ev(QEvent::KeyPress, Qt::Key_X, Qt::NoModifier, QStringLiteral("x"));
        pty.stdinBuffer().clear();
        contour::session::sendKeyEvent(&ev, vtbackend::KeyboardEventType::Press, *session);
        CHECK(pty.stdinBuffer() == "x");
    }

    // A mapped special key (Enter) writes CR.
    {
        QKeyEvent ev(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        pty.stdinBuffer().clear();
        contour::session::sendKeyEvent(&ev, vtbackend::KeyboardEventType::Press, *session);
        CHECK(pty.stdinBuffer() == "\r");
    }

    // A cursor key emits an escape sequence.
    {
        QKeyEvent ev(QEvent::KeyPress, Qt::Key_Up, Qt::NoModifier);
        pty.stdinBuffer().clear();
        contour::session::sendKeyEvent(&ev, vtbackend::KeyboardEventType::Press, *session);
        CHECK(pty.stdinBuffer().contains('\033'));
    }

    // Ctrl+C encodes as 0x03.
    {
        QKeyEvent ev(QEvent::KeyPress, Qt::Key_C, Qt::ControlModifier, QStringLiteral("\x03"));
        pty.stdinBuffer().clear();
        contour::session::sendKeyEvent(&ev, vtbackend::KeyboardEventType::Press, *session);
        CHECK(pty.stdinBuffer().contains('\x03'));
    }
}

TEST_CASE("the macOS layout reads Carbon key codes as positions, not codepoints", "[session][input]")
{
    auto const& layout = macUsAnsiLayout();

    // The keys the old code got wrong: every kVK_ANSI_* code >= 32, which a codepoint-shaped read
    // of the key code turned into the punctuation character of the same value.
    CHECK(layout.unshiftedKeyOf({ .virtualKey = 0x20 }) == U'u');  // was read as ' '
    CHECK(layout.unshiftedKeyOf({ .virtualKey = 0x21 }) == U'[');  // was read as '!'
    CHECK(layout.unshiftedKeyOf({ .virtualKey = 0x22 }) == U'i');  // was read as '"'
    CHECK(layout.unshiftedKeyOf({ .virtualKey = 0x23 }) == U'p');  // was read as '#'
    CHECK(layout.unshiftedKeyOf({ .virtualKey = 0x25 }) == U'l');  // was read as '%'
    CHECK(layout.unshiftedKeyOf({ .virtualKey = 0x26 }) == U'j');  // was read as '&'
    CHECK(layout.unshiftedKeyOf({ .virtualKey = 0x28 }) == U'k');  // was read as '('
    CHECK(layout.unshiftedKeyOf({ .virtualKey = 0x29 }) == U';');  // was read as ')'
    CHECK(layout.unshiftedKeyOf({ .virtualKey = 0x2A }) == U'\\'); // was read as '*'
    CHECK(layout.unshiftedKeyOf({ .virtualKey = 0x2B }) == U',');  // was read as '+'
    CHECK(layout.unshiftedKeyOf({ .virtualKey = 0x2C }) == U'/');  // was read as ','
    CHECK(layout.unshiftedKeyOf({ .virtualKey = 0x2D }) == U'n');  // was read as '-'
    CHECK(layout.unshiftedKeyOf({ .virtualKey = 0x2E }) == U'm');  // was read as '.'
    CHECK(layout.unshiftedKeyOf({ .virtualKey = 0x2F }) == U'.');  // was read as '/'
    CHECK(layout.unshiftedKeyOf({ .virtualKey = 0x32 }) == U'`');  // was read as '2'

    // The codes below 32 never reached the broken branch, and must still map.
    CHECK(layout.unshiftedKeyOf({ .virtualKey = 0x00 }) == U'a');
    CHECK(layout.unshiftedKeyOf({ .virtualKey = 0x08 }) == U'c');
    CHECK(layout.unshiftedKeyOf({ .virtualKey = 0x0D }) == U'w');
    CHECK(layout.unshiftedKeyOf({ .virtualKey = 0x12 }) == U'1');

    // Keys that carry no character are absent, and 0 means "ask the character instead".
    CHECK(layout.unshiftedKeyOf({ .virtualKey = 0x38 }) == 0); // kVK_Shift
    CHECK(layout.unshiftedKeyOf({ .virtualKey = 0x3B }) == 0); // kVK_Control
    CHECK(layout.unshiftedKeyOf({ .virtualKey = 0x7A }) == 0); // kVK_F1

    // No VK code off Windows, so none can reach win32-input-mode disguised as one.
    CHECK(layout.win32VirtualKeyOf({ .virtualKey = 0x20 }) == 0);
}

TEST_CASE("the Xkb layout decodes the X11 Unicode keysym form", "[session][input]")
{
    auto const& layout = xkbLayout();

    // Latin-1 keysyms are already codepoints.
    CHECK(layout.unshiftedKeyOf({ .virtualKey = 0x75 }) == U'u');

    // Anything outside Latin-1 arrives as `0x01000000 | codepoint`, which is not one.
    CHECK(layout.unshiftedKeyOf({ .virtualKey = 0x0100'0446 }) == U'ц');

    // A key we cannot name — a function or media keysym — reports unknown rather than a wrong key.
    CHECK(layout.unshiftedKeyOf({ .virtualKey = 0x1008'FF11 }) == 0); // XF86AudioLowerVolume
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
    auto session = contour::test::makeTestSession(
        &app.app().sessionsManager(),
        std::make_unique<vtpty::MockPty>(vtpty::PageSize { vtpty::LineCount(24), vtpty::ColumnCount(80) }),
        app.app(),
        contour::input::makeKeyboardPlatformFactory(contour::config::KeyboardPlatformKind::Mac,
                                                    macUsAnsiLayout()));
    auto& pty = mockPtyOf(*session);

    // What fish sends on startup: CSI > 1 u, "disambiguate escape codes".
    session->terminal().writeToScreen("\033[>1u");

    // nativeScanCode/nativeVirtualKey/nativeModifiers, as Qt's Cocoa plugin fills them in. Captured data
    // rather than a QKeyEvent, so that no host samples its live lock toggles into it.
    auto constexpr MacKeyCodeU = uint32_t { 0x20 };
    auto const event = contour::input::NativeKeyEvent { .qtKey = Qt::Key_U,
                                                        .qtModifiers = Qt::ControlModifier,
                                                        .nativeVirtualKey = MacKeyCodeU,
                                                        .nativeScanCode = MacKeyCodeU };
    pty.stdinBuffer().clear();
    std::ignore = session->sendKeyboardEvent(session->keyboardPlatform().normalize(event),
                                             std::chrono::steady_clock::now());
    CHECK(pty.stdinBuffer() == "\033[117;5u");
}

#ifdef _WIN32
TEST_CASE("AltGr characters reach ConPTY verbatim under Win32 input mode (issue #2127)", "[session][input]")
{
    // Windows reports AltGr to Qt as Ctrl+Alt. ConPTY enables win32-input-mode on startup, and the
    // encoder Ctrl-maps the character of a Ctrl chord the way ToUnicodeEx does -- so a Swedish
    // AltGr+8 used to leave as ESC (UC=27) instead of '['. What tells the two apart is the text the
    // OS produced: AltGr yields a graphical character, a genuine Ctrl+Alt chord does not.
    contour::test::TestApp app;
    auto session = makeSession(app.app());
    session->terminal().writeToScreen("\033[?9001h");

    // sendKeyEvent reads the live CapsLock/NumLock toggles, which ConPTY's dwControlKeyState
    // reports; take them from the same source so the expectation does not depend on the machine.
    auto const locks = contour::input::win32LockKeysOf(contour::input::nativeModifiersWithLockState(0));
    using vtbackend::Win32ControlKeyFlag;
    using vtbackend::Win32ControlKeyState;
    auto lockState = Win32ControlKeyState {};
    if (locks.contains(vtbackend::LockKey::CapsLock))
        lockState.enable(Win32ControlKeyFlag::CapsLockOn);
    if (locks.contains(vtbackend::LockKey::NumLock))
        lockState.enable(Win32ControlKeyFlag::NumLockOn);
    auto const win32Key =
        [&](unsigned virtualKey, unsigned unicodeChar, Win32ControlKeyState controlKeyState) {
            return std::format(
                "\033[{};0;{};1;{};1_", virtualKey, unicodeChar, controlKeyState.with(lockState).value());
        };

    // Swedish layout: AltGr+8 is '[', AltGr+2 is '@'. Ctrl and Alt are the AltGr layer, not a chord.
    CHECK(pressWithCtrlAlt(*session, Qt::Key_BracketLeft, 0x38, QStringLiteral("["))
          == win32Key(0x38, '[', {}));
    CHECK(pressWithCtrlAlt(*session, Qt::Key_At, 0x32, QStringLiteral("@")) == win32Key(0x32, '@', {}));

    // A genuine Ctrl+Alt+A produces no text and keeps its chord: UC=^A, CS=LeftAlt|LeftCtrl.
    auto const leftCtrlAlt = Win32ControlKeyState { Win32ControlKeyFlag::LeftAltPressed }.with(
        Win32ControlKeyFlag::LeftCtrlPressed);
    CHECK(pressWithCtrlAlt(*session, Qt::Key_A, 0x41, QString {}) == win32Key(0x41, 0x01, leftCtrlAlt));
}

TEST_CASE("a genuine Ctrl+Alt chord keeps its modifiers outside Win32 input mode", "[session][input]")
{
    // Windows reports AltGr as Ctrl+Alt, and every encoding but win32-input-mode used to strip that
    // pair unconditionally -- so a real Ctrl+Alt+A typed a bare 'A', and a binding on it never
    // fired. Only a Ctrl+Alt that produced graphical text is AltGr.
    contour::test::TestApp app;
    app.app().config().inputMappings = contour::test::loadConfigFromYaml(R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
input_mapping:
    - { mods: [Control, Alt], key: 'p', action: OpenCommandPalette }
)")
                                           .inputMappings;
    auto session = makeSession(app.app());

    SECTION("legacy encoding")
    {
        // Qt may report the chord with no text or with the control character; either way ESC ^A.
        CHECK(pressWithCtrlAlt(*session, Qt::Key_A, 0x41, QString {}) == "\033\x01");
        CHECK(pressWithCtrlAlt(*session, Qt::Key_A, 0x41, QStringLiteral("\x01")) == "\033\x01");

        // A Swedish AltGr+8 is still the character it typed.
        CHECK(pressWithCtrlAlt(*session, Qt::Key_BracketLeft, 0x38, QStringLiteral("[")) == "[");
    }

    SECTION("kitty keyboard protocol")
    {
        session->terminal().writeToScreen("\033[>1u");
        auto const locks =
            contour::input::win32LockKeysOf(contour::input::nativeModifiersWithLockState(0)).value();
        CHECK(pressWithCtrlAlt(*session, Qt::Key_A, 0x41, QString {})
              == std::format("\033[97;{}u", 7u + locks));
        CHECK(pressWithCtrlAlt(*session, Qt::Key_BracketLeft, 0x38, QStringLiteral("[")) == "[");
    }

    SECTION("a Ctrl+Alt binding fires")
    {
        CHECK(pressWithCtrlAlt(*session, Qt::Key_P, 0x50, QString {}).empty());
    }
}
#endif

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
        contour::session::sendKeyEvent(&ev, vtbackend::KeyboardEventType::Press, *session);
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

#ifndef __APPLE__
TEST_CASE("the pane chords are claimed before the terminal encodes them", "[session][input][panes]")
{
    contour::test::TestApp app;

    // The tab-switch test above explains why the table is emptied for the second pass: a user whose
    // contour.yml enumerates the defaults of an older release has none of the rows added since, and only
    // the fallback table can reach them. The first pass is the freshly generated configuration.
    auto const emptyTables = GENERATE(false, true);
    if (emptyTables)
    {
        app.app().config().inputMappings.value().keyMappings.clear();
        app.app().config().inputMappings.value().charMappings.clear();
    }

    auto session = makeSession(app.app());
    auto& pty = mockPtyOf(*session);

    auto const reachedThePty = [&](Qt::Key key, Qt::KeyboardModifiers modifiers, QString const& text) {
        QKeyEvent ev(QEvent::KeyPress, key, modifiers, text);
        pty.stdinBuffer().clear();
        contour::session::sendKeyEvent(&ev, vtbackend::KeyboardEventType::Press, *session);
        return !pty.stdinBuffer().empty();
    };

    SECTION("Alt+Shift+arrow moves the focus between panes")
    {
        for (auto const key: { Qt::Key_Left, Qt::Key_Right, Qt::Key_Up, Qt::Key_Down })
            CHECK_FALSE(reachedThePty(key, Qt::AltModifier | Qt::ShiftModifier, QString()));
    }

    SECTION("a bare Alt+arrow stays the shell's word motion")
    {
        for (auto const key: { Qt::Key_Left, Qt::Key_Right })
            CHECK(reachedThePty(key, Qt::AltModifier, QString()));
    }

    SECTION("Ctrl+Shift+E and Ctrl+Shift+O split")
    {
        CHECK_FALSE(
            reachedThePty(Qt::Key_E, Qt::ControlModifier | Qt::ShiftModifier, QStringLiteral("\x05")));
        CHECK_FALSE(
            reachedThePty(Qt::Key_O, Qt::ControlModifier | Qt::ShiftModifier, QStringLiteral("\x0f")));
    }
}
#endif

TEST_CASE("a declined Ctrl+Shift+G does not reach the shell as ^G", "[session][input][semanticblocks]")
{
    contour::test::TestApp app;
    auto session = makeSession(app.app());
    auto& pty = mockPtyOf(*session);

    auto const reachedThePty = [&](Qt::KeyboardModifiers modifiers, vtbackend::KeyboardEventType type) {
        QKeyEvent ev(QEvent::KeyPress, Qt::Key_G, modifiers, QStringLiteral("\x07"));
        pty.stdinBuffer().clear();
        contour::session::sendKeyEvent(&ev, type, *session);
        return pty.stdinBuffer();
    };

    // No command has finished, so there is nothing to open. A ^G would abort the line readline is editing.
    auto const ctrlShift = Qt::ControlModifier | Qt::ShiftModifier;
    CHECK(reachedThePty(ctrlShift, vtbackend::KeyboardEventType::Press).empty());
    CHECK(reachedThePty(ctrlShift, vtbackend::KeyboardEventType::Repeat).empty());

    // Ctrl+G is the shell's, and an alternate-screen application keeps the chord.
    CHECK_FALSE(reachedThePty(Qt::ControlModifier, vtbackend::KeyboardEventType::Press).empty());
    session->terminal().writeToScreen("\033[?1049h");
    CHECK_FALSE(reachedThePty(ctrlShift, vtbackend::KeyboardEventType::Press).empty());
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
        contour::session::sendKeyEvent(&ev, vtbackend::KeyboardEventType::Press, *session);
        INFO("key = " << static_cast<int>(key));
        CHECK_FALSE(pty.stdinBuffer().empty());
    }

    // Backtab (Shift+Tab) and keypad keys go through their own rows.
    {
        QKeyEvent ev(QEvent::KeyPress, Qt::Key_Backtab, Qt::ShiftModifier);
        pty.stdinBuffer().clear();
        contour::session::sendKeyEvent(&ev, vtbackend::KeyboardEventType::Press, *session);
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
        contour::session::sendKeyEvent(&ev, vtbackend::KeyboardEventType::Press, *session);
        INFO("keypad key = " << static_cast<int>(key));
        CHECK_FALSE(pty.stdinBuffer().empty());
    }

    // An unmapped key with no text is not handled and writes nothing.
    {
        QKeyEvent ev(QEvent::KeyPress, Qt::Key_CapsLock, Qt::NoModifier);
        pty.stdinBuffer().clear();
        contour::session::sendKeyEvent(&ev, vtbackend::KeyboardEventType::Press, *session);
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

TEST_CASE("buildAttachHostCommand runs contour client --host with the active configuration",
          "[helper][spawn][hosts]")
{
    auto const cmd = contour::session::buildAttachHostCommand(
        "/usr/bin/contour", "/home/me/.config/contour/contour.yml", "home");
    CHECK(cmd.program == "/usr/bin/contour");
    CHECK(cmd.arguments
          == QStringList { "client", "--host", "home", "--config", "/home/me/.config/contour/contour.yml" });

    // No configuration file: the new process loads the default one.
    auto const bare = contour::session::buildAttachHostCommand("/usr/bin/contour", "", "home");
    CHECK(bare.arguments == QStringList { "client", "--host", "home" });
}

TEST_CASE("buildSpawnTerminalCommand assembles arguments and forwards the working directory",
          "[helper][spawn]")
{
    // Full set: config + profile + the working directory, in order.
    {
        auto const cmd =
            contour::session::buildSpawnTerminalCommand("/usr/bin/contour", "/tmp/c.yml", "main", "/tmp");
        CHECK(cmd.program == "/usr/bin/contour");
        CHECK(cmd.arguments
              == QStringList { "config", "/tmp/c.yml", "profile", "main", "working-directory", "/tmp" });
    }

    // Empty config, profile and working directory omit those flags.
    {
        auto const cmd = contour::session::buildSpawnTerminalCommand("/usr/bin/contour", "", "", "");
        CHECK(cmd.program == "/usr/bin/contour");
        CHECK(cmd.arguments.isEmpty());
    }

    // The directory is a path the session already resolved, forwarded as it is: nothing in it is an escape,
    // a query or a fragment, and a drive letter is no scheme.
    {
        auto const cmd =
            contour::session::buildSpawnTerminalCommand("/usr/bin/contour", "", "work", "/home/x/50%41#a?b");
        CHECK(cmd.arguments == QStringList { "profile", "work", "working-directory", "/home/x/50%41#a?b" });
        auto const drive =
            contour::session::buildSpawnTerminalCommand("/usr/bin/contour", "", "", "C:/Users/x");
        CHECK(drive.arguments == QStringList { "working-directory", "C:/Users/x" });
        // Nor is a relative directory whose first component happens to be "file:" a URL.
        auto const odd =
            contour::session::buildSpawnTerminalCommand("/usr/bin/contour", "", "", "file://x/y");
        CHECK(odd.arguments == QStringList { "working-directory", "file://x/y" });
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
        contour::session::sendKeyEvent(&ev, vtbackend::KeyboardEventType::Press, *session);
        CHECK(pty.stdinBuffer().empty());
    }

    SECTION("the CharMappings table reports the uppercase key label")
    {
        // Empty text, so sendKeyEvent falls back to its Qt::Key -> character table.
        QKeyEvent ev(QEvent::KeyPress, Qt::Key_P, ctrlShift, QString());
        pty.stdinBuffer().clear();
        contour::session::sendKeyEvent(&ev, vtbackend::KeyboardEventType::Press, *session);
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
        contour::session::sendKeyEvent(&ev, vtbackend::KeyboardEventType::Press, *session);
        CHECK(pty.stdinBuffer().empty());
    }

    SECTION("an UNBOUND letter still reaches the PTY")
    {
        // The negative control: without it, every section above would also pass if the fold had
        // simply made the session swallow all input.
        QKeyEvent ev(QEvent::KeyPress, Qt::Key_Y, ctrlShift, QStringLiteral("y"));
        pty.stdinBuffer().clear();
        contour::session::sendKeyEvent(&ev, vtbackend::KeyboardEventType::Press, *session);
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

namespace
{
/// A session on the macOS key path: its normalizer, and the default bindings it carries, are the Mac's, so
/// that the chords macOS users press can be driven through the real key path on any machine.
/// @param app The test application whose configuration the session reads.
/// @param option Which Option keys act as Alt.
/// @return The session; its PTY is a MockPty.
[[nodiscard]] std::unique_ptr<contour::session::TerminalSession> makeMacSession(
    contour::test::TestApp& app, contour::config::OptionAsAlt option = contour::config::OptionAsAlt::Off)
{
    auto& config = app.app().config();
    config.keyboardPlatformKind = contour::config::KeyboardPlatformKind::Mac;
    config.inputMappings.value() = contour::config::makeDefaultInputMappings(
        config.defaultsEdition, contour::config::KeyboardPlatformKind::Mac);
    config.profile().optionAsAlt.value() = option;
    return contour::test::makeTestSession(
        &app.app().sessionsManager(),
        std::make_unique<vtpty::MockPty>(vtpty::PageSize { vtpty::LineCount(24), vtpty::ColumnCount(80) }),
        app.app(),
        contour::input::makeKeyboardPlatformFactory(contour::config::KeyboardPlatformKind::Mac,
                                                    macUsAnsiLayout()));
}

/// The name of the first action the session would run for @p event, as the router resolves it.
/// @param session The session whose live configuration and binding modes decide.
/// @param event The event, as Qt delivered it.
/// @return The action's name, or nullopt when nothing binds the event.
[[nodiscard]] std::optional<std::string> resolvedAction(contour::session::TerminalSession const& session,
                                                        QKeyEvent const& event)
{
    auto const normalized = session.keyboardPlatform().normalize(
        contour::input::captureKeyEvent(event, vtbackend::KeyboardEventType::Press));
    auto const context =
        contour::input::RouteContext { .config = session.config(),
                                       .modes = session.terminal().bindingModes(),
                                       .superEncoding = contour::input::SuperEncoding::Unrepresentable };
    auto const route = [&] {
        if (normalized.key)
            return contour::input::routeKey(
                *normalized.key, normalized.modifiers, normalized.appModifier, context);
        return contour::input::routeChar(normalized.text.front(),
                                         normalized.identity,
                                         normalized.modifiers,
                                         normalized.appModifier,
                                         context);
    }();
    auto const* bound = route.user.actions != nullptr ? route.user.actions : route.fallback.actions;
    if (bound == nullptr || bound->empty())
        return std::nullopt;
    return std::string { contour::actions::name(bound->front()) };
}

/// A Cocoa key press with the Carbon key code @p carbon and the text @p text.
[[nodiscard]] QKeyEvent cocoaPress(Qt::Key key,
                                   Qt::KeyboardModifiers modifiers,
                                   quint32 carbon,
                                   QString const& text,
                                   quint32 nativeModifiers = 0)
{
    return { QEvent::KeyPress, key, modifiers, 0, carbon, nativeModifiers, text };
}
} // namespace

TEST_CASE("macOS chords reach their actions as Qt delivers them", "[session][input][macos]")
{
    constexpr auto Command = Qt::MetaModifier;
    constexpr auto OptionBits = uint32_t { 0x80000 | contour::input::MacLeftOptionBit };

    struct Row
    {
        std::string_view name;
        Qt::Key key;
        Qt::KeyboardModifiers modifiers;
        quint32 carbon;
        QString text;
        quint32 nativeModifiers;
        std::string_view action;
        std::string_view written; ///< What the action puts on the PTY; empty for those that write nothing.
    };
    auto const rows = std::vector<Row> {
        { "Command+C", Qt::Key_C, Command, 0x08, "c", 0, "CopySelection", "" },
        { "Command+V", Qt::Key_V, Command, 0x09, "v", 0, "PasteClipboard", "" },
        { "Command+K", Qt::Key_K, Command, 0x28, "k", 0, "ClearHistoryAndReset", "" },
        { "Command+comma", Qt::Key_Comma, Command, 0x2B, ",", 0, "OpenConfiguration", "" },
        { "Command+N", Qt::Key_N, Command, 0x2D, "n", 0, "NewTerminal", "" },
        { "Command+equal", Qt::Key_Equal, Command, 0x18, "=", 0, "IncreaseFontSize", "" },
        // The plus sign is Shift+equal on a US layout: it arrives as '+' with Shift, retried under '='.
        { "Command+plus", Qt::Key_Plus, Command | Qt::ShiftModifier, 0x18, "+", 0, "IncreaseFontSize", "" },
        { "Command+minus", Qt::Key_Minus, Command, 0x1B, "-", 0, "DecreaseFontSize", "" },
        { "Command+0", Qt::Key_0, Command, 0x1D, "0", 0, "ResetFontSize", "" },
        { "Option+Left", Qt::Key_Left, Qt::AltModifier, 0x7B, "", OptionBits, "SendChars", "\033b" },
        { "Option+Right", Qt::Key_Right, Qt::AltModifier, 0x7C, "", OptionBits, "SendChars", "\033f" },
        { "Command+Left", Qt::Key_Left, Command, 0x7B, "", 0, "SendChars", "\001" },
        { "Command+Right", Qt::Key_Right, Command, 0x7C, "", 0, "SendChars", "\005" },
        { "Command+Backspace", Qt::Key_Backspace, Command, 0x33, "", 0, "SendChars", "\025" },
    };

    contour::test::TestApp app;
    auto session = makeMacSession(app);
    auto& pty = mockPtyOf(*session);

    for (auto const& row: rows)
    {
        DYNAMIC_SECTION(row.name)
        {
            auto event = cocoaPress(row.key, row.modifiers, row.carbon, row.text, row.nativeModifiers);
            CHECK(resolvedAction(*session, event) == std::optional { std::string { row.action } });

            // Only the actions that write bytes are run: the others open windows and dialogs.
            if (row.action == "SendChars")
            {
                pty.stdinBuffer().clear();
                CHECK(contour::session::sendKeyEvent(&event, vtbackend::KeyboardEventType::Press, *session));
                CHECK(pty.stdinBuffer() == row.written);
            }
        }
    }
}

TEST_CASE("Command+C with nothing selected writes nothing", "[session][input][macos]")
{
    contour::test::TestApp app;
    auto session = makeMacSession(app);
    auto& pty = mockPtyOf(*session);

    // Declined, and consumed: the chord never reaches the application as a bare "c".
    auto event = cocoaPress(Qt::Key_C, Qt::MetaModifier, 0x08, "c");
    pty.stdinBuffer().clear();
    CHECK(contour::session::sendKeyEvent(&event, vtbackend::KeyboardEventType::Press, *session));
    CHECK(pty.stdinBuffer().empty());
}

TEST_CASE("Option+Left writes ESC b with option_as_alt off", "[session][input][macos]")
{
    constexpr auto OptionBits = uint32_t { 0x80000 | contour::input::MacLeftOptionBit };

    contour::test::TestApp app;
    auto session = makeMacSession(app, contour::config::OptionAsAlt::Off);
    auto& pty = mockPtyOf(*session);
    auto event = cocoaPress(Qt::Key_Left, Qt::AltModifier, 0x7B, "", OptionBits);

    SECTION("on the primary screen it is the shell's backward word")
    {
        pty.stdinBuffer().clear();
        CHECK(contour::session::sendKeyEvent(&event, vtbackend::KeyboardEventType::Press, *session));
        CHECK(pty.stdinBuffer() == "\033b");
    }

    SECTION("on the alternate screen the application gets the chord, as Alt+Left")
    {
        session->terminal().writeToScreen("\033[?1049h");
        pty.stdinBuffer().clear();
        CHECK(contour::session::sendKeyEvent(&event, vtbackend::KeyboardEventType::Press, *session));
        CHECK(pty.stdinBuffer() == "\033[1;3D");
    }
}

TEST_CASE("Command+X and Command+S are bound to nothing on macOS and are consumed", "[session][input][macos]")
{
    contour::test::TestApp app;
    auto session = makeMacSession(app);
    auto& pty = mockPtyOf(*session);

    auto cut = cocoaPress(Qt::Key_X, Qt::MetaModifier, 0x07, "x");
    auto save = cocoaPress(Qt::Key_S, Qt::MetaModifier, 0x01, "s");
    for (auto* event: { &cut, &save })
    {
        CHECK_FALSE(resolvedAction(*session, *event).has_value());
        // Consumed rather than typed: an unbound Command chord never reaches the application as a letter.
        pty.stdinBuffer().clear();
        std::ignore = contour::session::sendKeyEvent(event, vtbackend::KeyboardEventType::Press, *session);
        CHECK(pty.stdinBuffer().empty());
    }
}

TEST_CASE("Command+Backspace on the alternate screen is the application's", "[session][input][macos]")
{
    contour::test::TestApp app;
    auto session = makeMacSession(app);
    auto& pty = mockPtyOf(*session);
    auto event = cocoaPress(Qt::Key_Backspace, Qt::MetaModifier, 0x33, "");

    session->terminal().writeToScreen("\033[?1049h");
    CHECK_FALSE(resolvedAction(*session, event).has_value());
    pty.stdinBuffer().clear();
    std::ignore = contour::session::sendKeyEvent(&event, vtbackend::KeyboardEventType::Press, *session);
    CHECK(pty.stdinBuffer() != "\025");
}
