// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/core/Primitives.hpp>
#include <vtbackend/grid/CellUtil.hpp>
#include <vtbackend/input/vi/HintModeHandler.hpp>
#include <vtbackend/screen/Terminal.hpp>
#include <vtbackend/screen/TerminalTestFixtures.hpp>
#include <vtbackend/testing/MockTerm.hpp>
#include <vtbackend/testing/TestHelpers.hpp>

#include <vtpty/MockPty.hpp>

#include <crispy/App.hpp>
#include <crispy/Times.hpp>
#include <crispy/Utils.hpp>
#include <crispy/testing/Environment.hpp>

#include <libunicode/convert.h>
#include <libunicode/width.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <ranges>
#include <string>
#include <thread>
#include <utility>
#include <vector>
using namespace std;
using namespace std::chrono_literals;
using vtbackend::CellLocation;
using vtbackend::ColumnCount;
using vtbackend::ColumnOffset;
using vtbackend::LineCount;
using vtbackend::LineOffset;
using vtbackend::MockTerm;
using vtbackend::Modifier;
using vtbackend::PageSize;
using namespace vtbackend::test;

// NOLINTBEGIN(misc-const-correctness)

TEST_CASE("Terminal.IME.CursorVisibleDuringComposition", "[terminal]")
{
    auto mc = MockTerm { ColumnCount { 6 }, LineCount { 4 } };
    auto& terminal = mc.terminal;
    terminal.setCursorDisplay(vtbackend::CursorDisplay::Blink);
    auto constexpr BlinkInterval = chrono::milliseconds(500);
    terminal.setCursorBlinkingInterval(BlinkInterval);

    auto const clockBase = chrono::steady_clock::time_point();

    // Advance time past the first blink toggle so the cursor is in its "off" phase.
    auto const clockBlinkOff = clockBase + BlinkInterval + chrono::milliseconds(1);
    terminal.tick(clockBlinkOff);
    terminal.ensureFreshRenderBuffer();
    CHECK(!terminal.cursorCurrentlyVisible());

    // Activate IME composition. Prior to the fix, the next line crashed inside
    // tryRenderInputMethodEditor() because _output->cursor was nullopt while
    // _cursorPosition was set.
    terminal.updateInputMethodPreeditString("test");

    // Advance the clock past the refresh interval so ensureFreshRenderBuffer
    // actually rebuilds the buffer rather than skipping as a duplicate.
    auto const clockAfterRefreshInterval = clockBlinkOff + chrono::milliseconds(100);
    terminal.tick(clockAfterRefreshInterval);
    terminal.ensureFreshRenderBuffer();

    // Post-fix: the render buffer cursor must be present -- IME composition
    // overrides the blink-off state so the cursor anchors the preedit text.
    auto const renderBuffer = terminal.renderBuffer();
    REQUIRE(renderBuffer.get().cursor.has_value());
    CHECK(renderBuffer.get().cursor->position.column == ColumnOffset(0));
    CHECK(renderBuffer.get().cursor->position.line == LineOffset(0));
}

TEST_CASE("Terminal.mouse coordinate modes are one mutually-exclusive setting", "[terminal][mouse]")
{
    using namespace vtbackend;

    // xterm keeps ONE `screen->extend_coords` for 1005/1006/1015/1016 and documents the
    // consequence: "they are mutually exclusive. For consistency, a reset is only effective against
    // the matching mode." Contour treated them as four independent flags, so a reset of any of them
    // clobbered whichever encoding was in effect -- and 1005/1015 ignored the set/reset bit
    // entirely, making a RESET turn them ON.
    auto mc = MockTerm { ColumnCount(20), LineCount(5) };
    REQUIRE(mc.terminal.mouseTransport() == MouseTransport::Default);

    SECTION("a reset of a mode that is not in effect leaves the active encoding alone")
    {
        mc.writeToScreen("\033[?1006h"sv);
        REQUIRE(mc.terminal.mouseTransport() == MouseTransport::SGR);

        // The three the application did NOT choose. Each used to overwrite the transport.
        mc.writeToScreen("\033[?1005l"sv);
        CHECK(mc.terminal.mouseTransport() == MouseTransport::SGR);
        mc.writeToScreen("\033[?1015l"sv);
        CHECK(mc.terminal.mouseTransport() == MouseTransport::SGR);
        mc.writeToScreen("\033[?1016l"sv);
        CHECK(mc.terminal.mouseTransport() == MouseTransport::SGR);
    }

    SECTION("a reset of the mode that IS in effect returns to the default encoding")
    {
        mc.writeToScreen("\033[?1006h"sv);
        REQUIRE(mc.terminal.mouseTransport() == MouseTransport::SGR);
        mc.writeToScreen("\033[?1006l"sv);
        CHECK(mc.terminal.mouseTransport() == MouseTransport::Default);
    }

    SECTION("resetting 1005 from the default must not ENABLE it")
    {
        mc.writeToScreen("\033[?1005l"sv);
        CHECK(mc.terminal.mouseTransport() == MouseTransport::Default);
        mc.writeToScreen("\033[?1015l"sv);
        CHECK(mc.terminal.mouseTransport() == MouseTransport::Default);
    }

    SECTION("setting one encoding replaces another")
    {
        mc.writeToScreen("\033[?1005h"sv);
        REQUIRE(mc.terminal.mouseTransport() == MouseTransport::Extended);
        mc.writeToScreen("\033[?1016h"sv);
        CHECK(mc.terminal.mouseTransport() == MouseTransport::SGRPixels);
        mc.writeToScreen("\033[?1015h"sv);
        CHECK(mc.terminal.mouseTransport() == MouseTransport::URXVT);
    }

    SECTION("replaying a whole mode set keeps the encoding the application chose")
    {
        // What a thin client does on attach: it knows WHICH modes are set and states the rest as
        // reset. Before the fix the trailing resets landed after the set and left Default, so the
        // mirror encoded X10 coordinates for an application that had asked for SGR.
        mc.writeToScreen("\033[?1005l\033[?1006h\033[?1015l\033[?1016l"sv);
        CHECK(mc.terminal.mouseTransport() == MouseTransport::SGR);
    }
}

TEST_CASE("Terminal.KittyKeyRelease.sendKeyEvent", "[terminal]")
{
    auto mc = MockTerm { PageSize { LineCount(24), ColumnCount(80) } };
    auto& terminal = mc.terminal;

    terminal.keyboardProtocol().enter(vtbackend::KeyboardEventFlag::DisambiguateEscapeCodes);
    terminal.keyboardProtocol().flags().enable(vtbackend::KeyboardEventFlag::ReportEventTypes);

    auto constexpr Now = std::chrono::steady_clock::time_point {};

    mc.resetReplyData();
    terminal.sendKeyEvent(
        vtbackend::Key::UpArrow, vtbackend::Modifier::None, vtbackend::KeyboardEventType::Press, Now);
    CHECK(e(mc.replyData()) == e("\033[A"s));

    mc.resetReplyData();
    terminal.sendKeyEvent(
        vtbackend::Key::UpArrow, vtbackend::Modifier::None, vtbackend::KeyboardEventType::Release, Now);
    CHECK(!mc.replyData().empty());
    CHECK(e(mc.replyData()) == e("\033[1;1:3A"s));
}

TEST_CASE("Terminal.KittyKeyRelease.sendCharEvent", "[terminal]")
{
    auto mc = MockTerm { PageSize { LineCount(24), ColumnCount(80) } };
    auto& terminal = mc.terminal;

    terminal.keyboardProtocol().enter(vtbackend::KeyboardEventFlag::DisambiguateEscapeCodes);
    terminal.keyboardProtocol().flags().enable(vtbackend::KeyboardEventFlag::ReportEventTypes);

    auto constexpr Now = std::chrono::steady_clock::time_point {};

    mc.resetReplyData();
    terminal.sendCharEvent('a',
                           vtbackend::KeyIdentity { .unshiftedKey = 'a' },
                           vtbackend::Modifier::Control,
                           vtbackend::KeyboardEventType::Press,
                           Now);
    CHECK(e(mc.replyData()) == e("\033[97;5u"s));

    mc.resetReplyData();
    terminal.sendCharEvent('a',
                           vtbackend::KeyIdentity { .unshiftedKey = 'a' },
                           vtbackend::Modifier::Control,
                           vtbackend::KeyboardEventType::Release,
                           Now);
    CHECK(!mc.replyData().empty());
    CHECK(e(mc.replyData()) == e("\033[97;5:3u"s));
}

TEST_CASE("Terminal.KittyKeyRelease.NoOutputWithoutFlag", "[terminal]")
{
    auto mc = MockTerm { PageSize { LineCount(24), ColumnCount(80) } };
    auto& terminal = mc.terminal;

    terminal.keyboardProtocol().enter(vtbackend::KeyboardEventFlag::DisambiguateEscapeCodes);

    auto constexpr Now = std::chrono::steady_clock::time_point {};

    mc.resetReplyData();
    terminal.sendCharEvent('a',
                           vtbackend::KeyIdentity { .unshiftedKey = 'a' },
                           vtbackend::Modifier::Control,
                           vtbackend::KeyboardEventType::Press,
                           Now);
    CHECK(!mc.replyData().empty());

    mc.resetReplyData();
    terminal.sendCharEvent('a',
                           vtbackend::KeyIdentity { .unshiftedKey = 'a' },
                           vtbackend::Modifier::Control,
                           vtbackend::KeyboardEventType::Release,
                           Now);
    CHECK(mc.replyData().empty());
}

TEST_CASE("Terminal.KittyKeyRelease.RepeatStillWorks", "[terminal]")
{
    auto mc = MockTerm { PageSize { LineCount(24), ColumnCount(80) } };
    auto& terminal = mc.terminal;

    terminal.keyboardProtocol().enter(vtbackend::KeyboardEventFlag::DisambiguateEscapeCodes);
    terminal.keyboardProtocol().flags().enable(vtbackend::KeyboardEventFlag::ReportEventTypes);

    auto constexpr Now = std::chrono::steady_clock::time_point {};

    mc.resetReplyData();
    terminal.sendKeyEvent(
        vtbackend::Key::UpArrow, vtbackend::Modifier::None, vtbackend::KeyboardEventType::Repeat, Now);
    CHECK(e(mc.replyData()) == e("\033[1;1:2A"s));
}

TEST_CASE("Terminal.numpad_digit_keeps_NumLock_for_the_input_generator", "[terminal][locks]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    mock.terminal.setApplicationkeypadMode(true);
    mock.resetReplyData();

    mock.sendKeyEvent(vtbackend::Key::Numpad_5, NumLockOnly);

    CHECK(e(mock.replyData()) == e("5"));
}

TEST_CASE("Terminal.no_key_encoding_depends_on_lock_modifiers", "[terminal][locks]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };

    auto const encodeKey = [&](vtbackend::Key key, vtbackend::KeyboardModifiers modifiers) {
        mock.resetReplyData();
        mock.sendKeyEvent(key, modifiers);
        return std::string { mock.replyData() };
    };

    auto const encodeChar = [&](char32_t ch, vtbackend::KeyboardModifiers modifiers) {
        mock.resetReplyData();
        mock.sendCharEvent(ch, modifiers);
        return std::string { mock.replyData() };
    };

    SECTION("every key")
    {
        // The Key enumerators run contiguously from F1 to Numpad_9.
        for (auto const rawKey: std::views::iota(static_cast<int>(vtbackend::Key::F1),
                                                 static_cast<int>(vtbackend::Key::Numpad_9) + 1))
        {
            auto const key = static_cast<vtbackend::Key>(rawKey);
            auto const baseline = encodeKey(key, {});
            for (auto const locks: LockCombinations)
            {
                INFO(std::format("key {} with lock keys {}", key, locks));
                CHECK(e(encodeKey(key, locks)) == e(baseline));
            }
        }
    }

    SECTION("every printable character")
    {
        for (auto const codepoint: std::views::iota(0x20, 0x7F))
        {
            auto const ch = static_cast<char32_t>(codepoint);
            auto const baseline = encodeChar(ch, {});
            for (auto const locks: LockCombinations)
            {
                INFO(std::format("character U+{:04X} with lock keys {}", codepoint, locks));
                CHECK(e(encodeChar(ch, locks)) == e(baseline));
            }
        }
    }

    SECTION("every printable character under modifyOtherKeys mode 2")
    {
        mock.terminal.setModifyOtherKeys(2);
        for (auto const codepoint: std::views::iota(0x20, 0x7F))
        {
            auto const ch = static_cast<char32_t>(codepoint);
            auto const baseline = encodeChar(ch, {});
            for (auto const locks: LockCombinations)
            {
                INFO(std::format("character U+{:04X} with lock keys {}", codepoint, locks));
                CHECK(e(encodeChar(ch, locks)) == e(baseline));
            }
        }
    }
}

TEST_CASE("Terminal.kitty_keyboard_protocol_reports_lock_modifiers", "[terminal][locks]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };

    // CSI > 9 u: DisambiguateEscapeCodes (1) | ReportAllKeysAsEscapeCodes (8).
    mock.writeToScreen("\033[>9u");
    mock.terminal.flushInput();
    mock.resetReplyData();

    // Key code 97 (lowercase a), modifier 65 == 1 + LockKey::CapsLock.
    mock.terminal.sendCharEvent(U'a',
                                vtbackend::KeyIdentity { .unshiftedKey = U'a' },
                                CapsLockOnly,
                                vtbackend::KeyboardEventType::Press,
                                std::chrono::steady_clock::now());

    CHECK(e(mock.replyData()) == e("\033[97;65u"));
}

TEST_CASE("Terminal.win32_input_mode_reports_unicode_for_escape_and_numpad", "[terminal][locks]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };

    // CSI ? 9001 h enables Win32 input mode, exactly as ConPTY does on Windows.
    mock.writeToScreen("\033[?9001h");
    mock.terminal.flushInput();

    auto const now = std::chrono::steady_clock::now();

    SECTION("Escape carries its Unicode char 0x1B")
    {
        mock.resetReplyData();
        mock.terminal.sendKeyEvent(vtbackend::Key::Escape, {}, vtbackend::KeyboardEventType::Press, now);
        // CSI Vk ; Sc ; Uc ; Kd ; Cs ; Rc _  -- VK_ESCAPE=27, Uc=ESC=27.
        CHECK(e(mock.replyData()) == e("\033[27;0;27;1;0;1_"));
    }

    SECTION("numpad digit carries its Unicode char while NumLock is latched")
    {
        mock.resetReplyData();
        mock.terminal.sendKeyEvent(
            vtbackend::Key::Numpad_5, NumLockOnly, vtbackend::KeyboardEventType::Press, now);
        // VK_NUMPAD5=101, Uc='5'=53, CS=NUMLOCK_ON=32.
        CHECK(e(mock.replyData()) == e("\033[101;0;53;1;32;1_"));
    }
}

TEST_CASE("Terminal.passive mouse tracking declines the event so the UI may act on it")
{
    // "Passive" is the whole contract: the application WATCHES the mouse without claiming it, so the
    // frontend stays free to act — which is what lets a horizontal wheel switch tabs with no modifier
    // held while a passive-tracking shell is running.
    auto const none = vtbackend::Modifiers {};
    auto const pos = vtbackend::PixelCoordinate {};

    SECTION("ordinary tracking claims the event")
    {
        auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
        mc.writeToScreen("\033[?1000h");
        mc.terminal.flushInput();

        CHECK(mc.terminal.sendMousePressEvent(none, vtbackend::MouseButton::WheelRight, pos, false).value);
    }

    SECTION("passive tracking does not")
    {
        auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
        mc.writeToScreen("\033[?1000h");
        mc.writeToScreen("\033[?2029h"); // DECSET 2029: passive mouse tracking
        mc.terminal.flushInput();

        CHECK_FALSE(
            mc.terminal.sendMousePressEvent(none, vtbackend::MouseButton::WheelRight, pos, false).value);
    }
}

TEST_CASE("Terminal.IME queries answered under the state lock survive concurrent output and resize",
          "[terminal][ime]")
{
    // Mirrors TerminalDisplay::inputMethodQuery(): the GUI thread answers platform input-method
    // queries from the live grid while the terminal thread keeps parsing output and resizes
    // reallocate the grid's lines. Every read sits under the state lock, in ONE scope per query so
    // the cursor cannot be checked against one page and dereferenced in another, and behind the
    // page-bounds guard. The oracle is the sanitizers: an unlocked or unguarded read here is a
    // torn-Line dereference, which ASan/TSan turn into a hard failure.
    auto mc = MockTerm { PageSize { LineCount(24), ColumnCount(80) }, LineCount(100) };
    auto& terminal = mc.terminal;

    auto stop = std::atomic<bool> { false };
    auto observed = std::atomic<size_t> { 0 };

    // The production bounds guard (imeCursorAddressable, in the frontend) forwards to
    // strictlyContains(); driving the test through the SAME predicate is what keeps this concurrency
    // reader mirroring production even if the bounds rule is later corrected. Only meaningful under the
    // state lock.
    auto const addressable = [&](CellLocation cursor) noexcept {
        return strictlyContains(cursor, terminal.pageSize());
    };

    // Catch2 assertion macros are not thread-safe: the reader only collects, the main thread asserts.
    auto imeReader = std::thread { [&]() {
        while (!stop.load(std::memory_order_relaxed))
        {
            auto const lock = std::lock_guard { terminal };
            if (!terminal.isCursorInViewport())
                continue;
            auto const cursor = terminal.currentScreen().cursor().position;
            if (!addressable(cursor))
                continue;
            // Summed into the atomic so the grid reads stay observable — the reads ARE the test.
            auto const cellWidth = terminal.currentScreen().cellWidthAt(cursor);
            observed.fetch_add(terminal.currentScreen().lineTextAt(cursor.line).size() + cellWidth,
                               std::memory_order_relaxed);
        }
    } };

    // The writer half mirrors production: parsing mutates the grid under the state lock
    // (processInputOnce), and the frontend resizes under an explicit lock (TerminalSession).
    auto const usableSizes = std::array { PageSize { LineCount(6), ColumnCount(20) },
                                          PageSize { LineCount(24), ColumnCount(80) } };
    for (auto const round: std::views::iota(0, 200))
    {
        mc.writeToScreen("wide \u4E16界 and combining ᬦᬸ e\u0301\r\n");
        if (round % 25 == 24)
        {
            auto const usable = usableSizes[static_cast<size_t>((round / 25) % 2)];
            auto const lock = std::lock_guard { terminal };
            terminal.resizeScreen(
                PageSize { .lines = usable.lines + terminal.statusLineHeight(), .columns = usable.columns });
        }
    }

    stop = true;
    imeReader.join();

    // The same query sequence once more, deterministically: the cursor of a live main page must be
    // addressable, and both grid reads must answer.
    auto const lock = std::lock_guard { terminal };
    INFO(std::format("concurrent reader observed {} line-text bytes and cell widths", observed.load()));
    REQUIRE(terminal.isCursorInViewport());
    auto const cursor = terminal.currentScreen().cursor().position;
    REQUIRE(addressable(cursor));
    CHECK(terminal.currentScreen().cellWidthAt(cursor) <= 2);
    CHECK(terminal.currentScreen().lineTextAt(cursor.line).size() <= 4uz * 80);
}

// NOLINTEND(misc-const-correctness)
