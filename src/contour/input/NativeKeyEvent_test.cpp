// SPDX-License-Identifier: Apache-2.0
//
// Tests for the plain-data key event: its recording format and its capture from Qt.

#include <contour/config/KeyboardPlatformKind.hpp>
#include <contour/input/KeyMapping.hpp>
#include <contour/input/NativeKeyEvent.hpp>
#include <contour/input/QtKeyCapture.hpp>

#include <QtGui/QKeyEvent>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using contour::input::captureInputMethodCommit;
using contour::input::captureKeyEvent;
using contour::input::formatRecorded;
using contour::input::KeyEventOrigin;
using contour::input::NativeKeyEvent;
using contour::input::parseRecorded;
using contour::input::toQKeyEvent;
using vtbackend::KeyboardEventType;

TEST_CASE("recorded native key events round-trip", "[input][capture]")
{
    auto const rows = std::vector<NativeKeyEvent> {
        { .qtKey = Qt::Key_X, .text = U"x" },
        { .qtKey = Qt::Key_C, .text = U"\x03", .qtModifiers = static_cast<uint32_t>(Qt::ControlModifier) },
        { .qtKey = Qt::Key_E,
          .qtModifiers = static_cast<uint32_t>(Qt::AltModifier),
          .nativeModifiers = 0x00080020,
          .nativeScanCode = 14 },
        { .text = U"é", .origin = KeyEventOrigin::InputMethodCommit },
        { .qtKey = Qt::Key_Shift,
          .qtModifiers = static_cast<uint32_t>(Qt::ShiftModifier),
          .type = KeyboardEventType::Release },
        { .qtKey = Qt::Key_Space, .text = U" \\ \n", .type = KeyboardEventType::Repeat },
    };

    for (auto const& row: rows)
    {
        auto const line = formatRecorded(row);
        INFO(line);
        CHECK_FALSE(line.contains('\n'));
        auto const parsed = parseRecorded(line);
        REQUIRE(parsed.has_value());
        CHECK(*parsed == row);
    }
}

TEST_CASE("parseRecorded rejects malformed lines", "[input][capture]")
{
    CHECK_FALSE(parseRecorded("").has_value());
    CHECK_FALSE(parseRecorded("key=zz").has_value());

    auto const good = formatRecorded(NativeKeyEvent { .qtKey = Qt::Key_A, .text = U"a" });
    REQUIRE(parseRecorded(good).has_value());

    // A line missing type=.
    auto const missingType =
        std::string { "key=0x41 mods=0x0 native=0x0 vk=0x0 scan=0x0 origin=keyboard text=a" };
    CHECK_FALSE(parseRecorded(missingType).has_value());

    // An unknown type name, and a truncated escape.
    CHECK_FALSE(parseRecorded("key=0x41 mods=0x0 native=0x0 vk=0x0 scan=0x0 type=tap origin=keyboard text=a")
                    .has_value());
    CHECK_FALSE(
        parseRecorded("key=0x41 mods=0x0 native=0x0 vk=0x0 scan=0x0 type=press origin=keyboard text=\\x4")
            .has_value());
}

TEST_CASE("captureKeyEvent keeps every field Qt reports", "[input][capture]")
{
    auto const event =
        QKeyEvent(QEvent::KeyPress, Qt::Key_A, Qt::ShiftModifier, 38, 0x41, 0x11, QStringLiteral("A"), true);
    auto const captured = captureKeyEvent(event, KeyboardEventType::Press);

    CHECK(captured.qtKey == Qt::Key_A);
    CHECK(captured.text == U"A");
    CHECK(captured.qtModifiers == static_cast<uint32_t>(Qt::ShiftModifier));
    CHECK(captured.nativeScanCode == 38);
    CHECK(captured.nativeVirtualKey == 0x41);
    CHECK(captured.origin == KeyEventOrigin::Keyboard);
    if constexpr (contour::config::BuildKeyboardPlatformKind != contour::config::KeyboardPlatformKind::Win32)
        CHECK(captured.nativeModifiers == 0x11);

    auto const replayed = toQKeyEvent(captured);
    CHECK(replayed.key() == Qt::Key_A);
    CHECK(replayed.modifiers() == Qt::ShiftModifier);
    CHECK(replayed.text() == QStringLiteral("A"));
    CHECK(replayed.nativeScanCode() == 38);
    CHECK(replayed.nativeVirtualKey() == 0x41);
    CHECK(replayed.nativeModifiers() == captured.nativeModifiers);
    CHECK(replayed.type() == QEvent::KeyPress);
}

TEST_CASE("captureInputMethodCommit is an unmodified text-only event", "[input][capture]")
{
    auto const captured = captureInputMethodCommit(QStringLiteral("日本"));
    CHECK(captured.origin == KeyEventOrigin::InputMethodCommit);
    CHECK(captured.qtKey == 0);
    CHECK(captured.qtModifiers == 0);
    CHECK(captured.text == U"日本");
    // The lock toggles are sampled as for a key -- live on Windows, zero elsewhere -- so the commit's text
    // carries them as typed text would.
    CHECK(captured.nativeModifiers == contour::input::nativeModifiersWithLockState(0));
}
