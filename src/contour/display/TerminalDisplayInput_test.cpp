// SPDX-License-Identifier: Apache-2.0
//
// What a live display does with input-method events and with Qt's per-keystroke ShortcutOverride question:
// committed text enters the key pipeline, and for a keystroke the keyboard platform keeps from the input
// method the display answers Qt::ImEnabled false until its press arrives. Offscreen: the window is shown but
// never rendered.

#include <contour/config/KeyboardPlatformKind.hpp>
#include <contour/config/OptionAsAlt.hpp>
#include <contour/input/KeyboardLayout.hpp>
#include <contour/input/KeyboardPlatform.hpp>
#include <contour/test/DisplayHarness.hpp>

#include <vtbackend/screen/Terminal.hpp>

#include <QtCore/QCoreApplication>
#include <QtGui/QFocusEvent>
#include <QtGui/QInputMethodEvent>
#include <QtGui/QKeyEvent>

#include <catch2/catch_test_macros.hpp>

#include <mutex>

using contour::test::DisplayHarness;
using contour::test::HarnessWindow;

namespace
{
/// Commits @p text to the display, as a platform input method does once composing ends.
void commit(DisplayHarness const& h, QString const& text)
{
    auto event = QInputMethodEvent {};
    event.setCommitString(text);
    QCoreApplication::sendEvent(h.display.get(), &event);
}
} // namespace

TEST_CASE("IME commit text reaches the PTY", "[display][input]")
{
    DisplayHarness const h { HarnessWindow::Unrendered };
    commit(h, QStringLiteral("日本"));
    CHECK(h.sentInput() == "日本");
}

TEST_CASE("IME commit text still reaches modal bindings", "[display][input]")
{
    DisplayHarness const h { HarnessWindow::Unrendered };
    auto& terminal = h.session->terminal();
    terminal.inputHandler().setMode(vtbackend::ViMode::Normal);
    auto const before = core::locked(terminal, [&] { return terminal.normalModeCursorPosition(); });

    // On macOS ordinary typing can arrive as a commit: vi normal mode's `j` must still move the cursor.
    commit(h, QStringLiteral("j"));

    auto const after = core::locked(terminal, [&] { return terminal.normalModeCursorPosition(); });
    CHECK(after.line == before.line + 1);
    CHECK(h.sentInput().empty());
}

TEST_CASE("an Option key acting as Alt turns the input method off for its keystroke", "[display][input]")
{
    static auto const layout = contour::input::makeMacUsAnsiKeyboardLayout();
    DisplayHarness const h {
        HarnessWindow::Unrendered,
        [](contour::config::Config& config) {
            config.profile().optionAsAlt.value() = contour::config::OptionAsAlt::Left;
        },
        {},
        contour::input::makeKeyboardPlatformFactory(contour::config::KeyboardPlatformKind::Mac, *layout),
    };
    auto const imEnabled = [&] {
        return h.display->inputMethodQuery(Qt::ImEnabled).toBool();
    };
    // What Qt's Cocoa plugin hands over for Option+e: the ShortcutOverride first, then -- after it has asked
    // the focus object whether the input method is enabled -- the key press.
    auto const keystroke = [&](QEvent::Type type, uint32_t optionBits) {
        auto event = QKeyEvent(type,
                               Qt::Key_E,
                               Qt::AltModifier,
                               /*nativeScanCode=*/0x0e,
                               /*nativeVirtualKey=*/0x0e,
                               /*nativeModifiers=*/0x80000 | optionBits,
                               QStringLiteral("\u00e9"));
        event.ignore();
        QCoreApplication::sendEvent(h.display.get(), &event);
        return event.isAccepted();
    };
    REQUIRE(imEnabled());

    SECTION("the named left Option key bypasses the input method until its press arrives")
    {
        CHECK(keystroke(QEvent::ShortcutOverride, contour::input::MacLeftOptionBit));
        CHECK_FALSE(imEnabled());
        keystroke(QEvent::KeyPress, contour::input::MacLeftOptionBit);
        CHECK(imEnabled());
        CHECK(h.sentInput()
              == "\x1b"
                 "e");
    }

    SECTION("the unnamed right Option key composes, so the input method stays on")
    {
        keystroke(QEvent::ShortcutOverride, contour::input::MacRightOptionBit);
        CHECK(imEnabled());
    }

    SECTION("losing the focus forgets the pending keystroke")
    {
        keystroke(QEvent::ShortcutOverride, contour::input::MacLeftOptionBit);
        REQUIRE_FALSE(imEnabled());
        auto focusOut = QFocusEvent(QEvent::FocusOut);
        QCoreApplication::sendEvent(h.display.get(), &focusOut);
        CHECK(imEnabled());
    }
}
