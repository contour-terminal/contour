// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/core/Primitives.hpp>
#include <vtbackend/grid/CellUtil.hpp>
#include <vtbackend/input/MatchModes.hpp>
#include <vtbackend/input/vi/HintModeHandler.hpp>
#include <vtbackend/screen/Terminal.hpp>
#include <vtbackend/screen/TerminalTestFixtures.hpp>
#include <vtbackend/testing/MockTerm.hpp>
#include <vtbackend/testing/TestHelpers.hpp>

#include <vtpty/MockPty.hpp>

#include <core/Times.hpp>
#include <core/Utils.hpp>
#include <core/cli/App.hpp>
#include <core/testing/Environment.hpp>

#include <libunicode/convert.h>
#include <libunicode/width.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <latch>
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

TEST_CASE("mouse reports add the reported cell origin", "[vtbackend][mouse]")
{
    // A mirror smaller than the daemon's grid shows a window into it, so the cell under the pointer here is
    // the cell the origin further in there -- and the application must hear about the cell it drew.
    using namespace vtbackend;
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(10) } };
    auto constexpr NoPixel = PixelCoordinate {};
    mc.writeToScreen("\033[?1000h\033[?1006h"sv);
    mc.terminal.setReportedCellOrigin(CellLocation { .line = LineOffset(15), .column = ColumnOffset(30) });

    mc.resetReplyData();
    std::ignore =
        mc.terminal.sendMousePressEvent(Modifier::None, MouseButton::Left, CellLocation {}, NoPixel, false);
    CHECK(e(mc.replyData()) == e("\033[<0;31;16M"sv));

    mc.resetReplyData();
    std::ignore = mc.terminal.sendMouseReleaseEvent(Modifier::None, MouseButton::Left, NoPixel, false);
    CHECK(e(mc.replyData()) == e("\033[<0;31;16m"sv));
}

TEST_CASE("Terminal.KittyKeyRelease.sendKeyEvent", "[terminal]")
{
    auto mc = MockTerm { PageSize { LineCount(24), ColumnCount(80) } };
    auto& terminal = mc.terminal;

    terminal.changeKeyboardProtocol(
        [](auto& protocol) { protocol.enter(vtbackend::KeyboardEventFlag::DisambiguateEscapeCodes); });
    terminal.changeKeyboardProtocol(
        [](auto& protocol) { protocol.flags().enable(vtbackend::KeyboardEventFlag::ReportEventTypes); });

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

    terminal.changeKeyboardProtocol(
        [](auto& protocol) { protocol.enter(vtbackend::KeyboardEventFlag::DisambiguateEscapeCodes); });
    terminal.changeKeyboardProtocol(
        [](auto& protocol) { protocol.flags().enable(vtbackend::KeyboardEventFlag::ReportEventTypes); });

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

    terminal.changeKeyboardProtocol(
        [](auto& protocol) { protocol.enter(vtbackend::KeyboardEventFlag::DisambiguateEscapeCodes); });

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

    terminal.changeKeyboardProtocol(
        [](auto& protocol) { protocol.enter(vtbackend::KeyboardEventFlag::DisambiguateEscapeCodes); });
    terminal.changeKeyboardProtocol(
        [](auto& protocol) { protocol.flags().enable(vtbackend::KeyboardEventFlag::ReportEventTypes); });

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
                                mock.steadyClock.now());

    CHECK(e(mock.replyData()) == e("\033[97;65u"));
}

TEST_CASE("Terminal.win32_input_mode_reports_unicode_for_escape_and_numpad", "[terminal][locks]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };

    // CSI ? 9001 h enables Win32 input mode, exactly as ConPTY does on Windows.
    mock.writeToScreen("\033[?9001h");
    mock.terminal.flushInput();

    auto const now = mock.steadyClock.now();

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

        CHECK(
            mc.terminal
                .sendMousePressEvent(
                    none, vtbackend::MouseButton::WheelRight, mc.terminal.currentMousePosition(), pos, false)
                .value);
    }

    SECTION("passive tracking does not")
    {
        auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
        mc.writeToScreen("\033[?1000h");
        mc.writeToScreen("\033[?2029h"); // DECSET 2029: passive mouse tracking
        mc.terminal.flushInput();

        CHECK_FALSE(
            mc.terminal
                .sendMousePressEvent(
                    none, vtbackend::MouseButton::WheelRight, mc.terminal.currentMousePosition(), pos, false)
                .value);
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

    auto observed = std::atomic<size_t> { 0 };

    // The production bounds guard (imeCursorAddressable, in the frontend) forwards to
    // strictlyContains(); driving the test through the SAME predicate is what keeps this concurrency
    // reader mirroring production even if the bounds rule is later corrected. Only meaningful under the
    // state lock.
    auto const addressable = [&](CellLocation cursor) noexcept {
        return strictlyContains(cursor, terminal.pageSize());
    };

    // The writer half mirrors production: parsing mutates the grid under the state lock
    // (processInputOnce), and the frontend resizes under an explicit lock (TerminalSession).
    auto const usableSizes = std::array { PageSize { LineCount(6), ColumnCount(20) },
                                          PageSize { LineCount(24), ColumnCount(80) } };
    {
        // Catch2 assertion macros are not thread-safe: the reader only collects, the main thread asserts.
        auto const imeReader = vtbackend::test::writeOnOwnThread([&] {
            auto const lock = std::lock_guard { terminal };
            if (!terminal.isCursorInViewport())
                return;
            auto const cursor = terminal.currentScreen().cursor().position;
            if (!addressable(cursor))
                return;
            // Summed into the atomic so the grid reads stay observable — the reads ARE the test.
            auto const cellWidth = terminal.currentScreen().cellWidthAt(cursor);
            observed.fetch_add(terminal.currentScreen().lineTextAt(cursor.line).size() + cellWidth,
                               std::memory_order_relaxed);
        });

        for (auto const round: std::views::iota(0, 200))
        {
            mc.writeToScreen("wide \u4E16界 and combining ᬦᬸ e\u0301\r\n");
            if (round % 25 == 24)
            {
                auto const usable = usableSizes[static_cast<size_t>((round / 25) % 2)];
                auto const lock = std::lock_guard { terminal };
                terminal.resizeScreen(PageSize { .lines = usable.lines + terminal.statusLineHeight(),
                                                 .columns = usable.columns });
            }
        }
    }

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

// {{{ The Vi key path and the state lock
TEST_CASE("Terminal.a Vi command waits for the state lock", "[terminal][vi][threading]")
{
    // `mm` writes a row's flags and the user-mark index, `za` the fold set, and the parser thread moves both
    // under the state lock whenever output scrolls the rows they name. So the key path runs a Vi command
    // under that lock too. Asserted as the contract rather than raced: with the lock held as the parser
    // thread holds it, `mm` has to wait for it.
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(40) }, LineCount(100) };
    mc.writeToScreen("one\r\ntwo\033[H");
    mc.terminal.inputHandler().setMode(vtbackend::ViMode::Normal);

    auto parserHold = std::unique_lock { mc.terminal };
    auto started = std::atomic<bool> { false };
    auto completed = std::atomic<bool> { false };
    auto worker = std::thread { [&]() {
        started.store(true, std::memory_order_release);
        mc.sendCharSequence("mm");
        completed.store(true, std::memory_order_release);
    } };

    // Running first, so that the check below cannot pass merely because the thread was not scheduled yet.
    while (!started.load(std::memory_order_acquire))
        std::this_thread::yield();
    std::this_thread::sleep_for(200ms);
    CHECK_FALSE(completed.load(std::memory_order_acquire));

    parserHold.unlock();
    worker.join();

    CHECK(completed.load(std::memory_order_acquire));
    CHECK(mc.terminal.primaryScreen().isLineFlagEnabledAt(LineOffset(0), vtbackend::LineFlag::UserMark));
}

TEST_CASE("Terminal.a key typed in Insert mode does not wait for the state lock", "[terminal][vi][threading]")
{
    // No Vi command wants the key: it is encoded and written to the application, which needs nothing the
    // parser guards. So it must not queue behind a long parse.
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(40) } };

    auto parserHold = std::unique_lock { mc.terminal };
    auto completed = std::atomic<bool> { false };
    auto worker = std::thread { [&]() {
        mc.sendCharEvent(U'a');
        completed.store(true, std::memory_order_release);
    } };

    // Bounded, so that a key that does wait fails the check rather than hanging the test.
    for ([[maybe_unused]] auto const attempt: std::views::iota(0, 500))
    {
        if (completed.load(std::memory_order_acquire))
            break;
        std::this_thread::sleep_for(10ms);
    }
    auto const typedWhileHeld = completed.load(std::memory_order_acquire);

    parserHold.unlock();
    worker.join();

    CHECK(typedWhileHeld);
    CHECK(mc.replyData() == "a");
}

TEST_CASE("Terminal.a Vi paste reaches the frontend after the state lock is released",
          "[terminal][vi][threading]")
{
    // A frontend answers `p` by reading its clipboard, which it must not do under the terminal's lock (a
    // clipboard owned by another process is a round trip), and by pasting through sendPaste(). The key path
    // holds that lock while the Vi command runs, so the request is raised once it is released.
    struct PastingTerm final: MockTerm<>
    {
        using MockTerm<>::MockTerm;

        void pasteFromClipboard(unsigned /*count*/, vtbackend::PasteStrip /*strip*/) override
        {
            atPaste.probe(terminal);
            terminal.sendPaste("pasted");
        }

        LockProbe atPaste;
    };

    auto mc = PastingTerm { PageSize { LineCount(10), ColumnCount(40) } };
    mc.terminal.inputHandler().setMode(vtbackend::ViMode::Normal);

    mc.sendCharEvent(U'p');

    CHECK(mc.atPaste.lockWasFree());
    CHECK(mc.replyData() == "pasted");
}

TEST_CASE("Terminal.a hint pasted with local echo on does not deadlock the key path", "[terminal][hintmode]")
{
    // Picking a hint with the Paste action pastes it from inside the key path, under the state lock. With SRM
    // reset the paste echoes, and echoing parses, which takes that lock: the bytes go out once it is
    // released.
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(80) } };
    mock.writeToScreen("\033[12lgo https://example.com now");
    mock.terminal.flushInput();
    mock.resetReplyData();
    mock.terminal.activateHintMode(
        vtbackend::HintModeRequest { .patterns = {}, .action = vtbackend::HintAction::Paste });
    REQUIRE(mock.terminal.hintMatches().size() == 1);

    failOnDeadlock(std::chrono::seconds(10), [&] { mock.sendCharEvent(U'a'); });

    CHECK(mock.replyData() == "https://example.com");
    CHECK(mock.terminal.primaryScreen()
              .grid()
              .lineText(LineOffset(0))
              .starts_with("go https://example.com nowhttps://example.com"));
}

TEST_CASE("Terminal.leaving Vi normal mode with local echo on does not deadlock the key path",
          "[terminal][vi]")
{
    // `i` takes the indicator status line away, which resizes the page, and with in-band resize reports on
    // the resize writes a report from inside the key path, under the state lock. With SRM reset the report
    // echoes, and echoing parses, which takes that lock: the report goes out once it is released.
    auto mock = MockTerm { PageSize { LineCount(10), ColumnCount(40) } };
    mock.writeToScreen("\033[12l\033[?2048h");
    mock.terminal.inputHandler().setMode(vtbackend::ViMode::Normal);
    mock.terminal.flushInput();
    mock.resetReplyData();

    failOnDeadlock(std::chrono::seconds(10), [&] { mock.sendCharEvent(U'i'); });

    CHECK(mock.terminal.inputHandler().mode() == vtbackend::ViMode::Insert);
    CHECK(mock.replyData().starts_with("\033[48;10;40;"));
}

TEST_CASE(
    "Terminal.a document a Vi command or a hint opens reaches the frontend after the state lock is released",
    "[terminal][vi][hintmode][threading]")
{
    // A frontend opens a document by handing it to the desktop, a round trip to another process that it must
    // not make under the terminal's lock. The key path holds that lock while `oo` or a picked hint runs, so
    // the request is raised once it is released.
    struct OpeningTerm final: MockTerm<>
    {
        using MockTerm<>::MockTerm;

        void openDocument(std::string_view fileOrUrl) override
        {
            atOpen.probe(terminal);
            opened = fileOrUrl;
        }

        void openLink(vtbackend::LinkTarget const& link) override
        {
            atOpen.probe(terminal);
            opened = link.target;
        }

        LockProbe atOpen;
        std::string opened; ///< The document or the link's target, whichever was opened.
    };

    auto mock = OpeningTerm { PageSize { LineCount(3), ColumnCount(40) } };
    mock.writeToScreen("https://example.com");

    SECTION("`oo` in Normal mode")
    {
        mock.writeToScreen("\033[H");
        mock.terminal.inputHandler().setMode(vtbackend::ViMode::Normal);
        mock.sendCharSequence("oo");
    }

    SECTION("a hint picked to be opened")
    {
        mock.terminal.activateHintMode(
            vtbackend::HintModeRequest { .patterns = {}, .action = vtbackend::HintAction::Open });
        REQUIRE(mock.terminal.hintMatches().size() == 1);
        mock.sendCharEvent(U'a');
    }

    CHECK(mock.atOpen.lockWasFree());
    CHECK(mock.opened.starts_with("https://example.com"));
}

TEST_CASE("Terminal.typing reads the keyboard action and local echo modes without racing the parser thread",
          "[terminal][threading]")
{
    // KAM locks the keyboard, and SRM decides whether what a key sends is echoed. A key typed in Insert mode
    // reads both without the state lock, so as not to wait for a parse, while the parser thread sets the ANSI
    // modes under it -- IRM here, which is kept beside them. The case to run under ThreadSanitizer, which
    // reports the reads against those writes unless the modes are atomic.
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(40) } };

    auto const keys =
        runWhileWriting([&] { mc.writeToScreen("\033[4h\033[4l"); }, [&] { mc.sendCharEvent(U'a'); });

    CHECK(mc.replyData() == std::string(keys, 'a'));
}

TEST_CASE("Terminal.a function key reads its user-defined string without racing the parser thread",
          "[terminal][threading]")
{
    // The parser thread programs the user-defined keys (DECUDK) -- clearing the table first here, as Pc 0
    // asks -- while a function key looks its string up on the GUI thread. The case to run under
    // ThreadSanitizer. Both take the input lock for it, so the lookup never finds the table half rewritten:
    // every F6 sends the string it was programmed with.
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(40) } };
    auto constexpr Programming = "\033P0;1|17/41;18/42;19/43;20/44\033\\"sv;
    mc.writeToScreen(Programming);

    auto const keys =
        runWhileWriting([&] { mc.writeToScreen(Programming); }, [&] { mc.sendKeyEvent(vtbackend::Key::F6); });

    CHECK(mc.replyData() == std::string(keys, 'A'));
}

TEST_CASE("Terminal.a key does not wait for the state lock", "[terminal][threading]")
{
    // The parser thread holds the state lock for a whole batch, and Windows' lock is not fair: a key that
    // waited for it could wait out a flood of output. No key does, not even F6 to F20, whose user-defined
    // strings (DECUDK) the parser programs. Another thread holds the lock for 500 ms; each key is sent within
    // 100 ms.
    struct Row
    {
        vtbackend::Key key;
        std::string_view sent;
    };
    auto const rows = std::array { Row { .key = vtbackend::Key::Enter, .sent = "\r" },
                                   Row { .key = vtbackend::Key::F6, .sent = "A" } };
    for (auto const& [key, sent]: rows)
    {
        auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(40) } };
        mc.writeToScreen("\033P0;1|17/41\033\\");

        auto held = std::latch { 1 };
        auto holder = std::jthread { [&]() {
            auto const _ = std::scoped_lock { mc.terminal };
            held.count_down();
            std::this_thread::sleep_for(500ms);
        } };
        held.wait();

        auto const started = std::chrono::steady_clock::now();
        mc.sendKeyEvent(key);
        auto const took = std::chrono::steady_clock::now() - started;
        holder.join();

        CHECK(took < 100ms);
        CHECK(mc.replyData() == sent);
    }
}

TEST_CASE("Terminal.the binding modes follow every state they name", "[terminal][input]")
{
    // A key reads the modes a binding can ask for without the state lock, from the word each writer of the
    // state publishes them in. A writer that forgot to would leave a binding matching on a mode that is gone,
    // or missing one that holds. Each row turns its mode on and off again through every path that changes it.
    using Flag = vtbackend::MatchModes::Flag;
    struct Row
    {
        std::string_view path;
        Flag flag;
        std::function<void(MockTerm<>&)> enter;
        std::function<void(MockTerm<>&)> leave;
    };
    auto const write = [](std::string_view text) {
        return [text = std::string(text)](MockTerm<>& mc) {
            mc.writeToScreen(text);
        };
    };
    auto const rows = std::array {
        Row { .path = "DECSET 1049",
              .flag = Flag::AlternateScreen,
              .enter = write("\033[?1049h"),
              .leave = write("\033[?1049l") },
        Row { .path = "DECCKM",
              .flag = Flag::AppCursor,
              .enter = write("\033[?1h"),
              .leave = write("\033[?1l") },
        Row { .path = "DECCKM, then RIS",
              .flag = Flag::AppCursor,
              .enter = write("\033[?1h"),
              .leave = write("\033c") },
        Row { .path = "DECKPAM", .flag = Flag::AppKeypad, .enter = write("\033="), .leave = write("\033>") },
        Row { .path = "DECKPAM, then RIS",
              .flag = Flag::AppKeypad,
              .enter = write("\033="),
              .leave = write("\033c") },
        Row { .path = "a selection",
              .flag = Flag::Select,
              .enter = [](auto& mc) { mc.terminal.selectAll(); },
              .leave = [](auto& mc) { mc.terminal.clearSelection(); } },
        Row { .path = "a click, released where it was pressed",
              .flag = Flag::Select,
              .enter =
                  [](auto& mc) {
                      (void) mc.terminal.sendMousePressEvent(vtbackend::Modifiers {},
                                                             vtbackend::MouseButton::Left,
                                                             CellLocation {},
                                                             vtbackend::PixelCoordinate {},
                                                             /*uiHandledHint=*/false);
                  },
              .leave =
                  [](auto& mc) {
                      (void) mc.terminal.sendMouseReleaseEvent(vtbackend::Modifiers {},
                                                               vtbackend::MouseButton::Left,
                                                               vtbackend::PixelCoordinate {},
                                                               /*uiHandledHint=*/false);
                  } },
        Row { .path = "a search",
              .flag = Flag::Search,
              .enter =
                  [](auto& mc) { (void) mc.terminal.setNewSearchTerm(U"x", vtbackend::SearchOrigin::Typed); },
              .leave = [](auto& mc) { mc.terminal.clearSearch(); } },
        Row { .path = "Insert mode",
              .flag = Flag::Insert,
              .enter = [](auto& mc) { mc.terminal.inputHandler().setMode(vtbackend::ViMode::Insert); },
              .leave = [](auto& mc) { mc.terminal.inputHandler().setMode(vtbackend::ViMode::Normal); } },
        Row { .path = "trace mode",
              .flag = Flag::Trace,
              .enter =
                  [](auto& mc) { mc.terminal.setExecutionMode(vtbackend::ExecutionMode::BreakAtEmptyQueue); },
              .leave = [](auto& mc) { mc.terminal.setExecutionMode(vtbackend::ExecutionMode::Normal); } },
    };

    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    auto const holds = [&](Flag flag) {
        return (mc.terminal.bindingModes() & static_cast<uint8_t>(flag)) != 0;
    };
    for (auto const& row: rows)
    {
        INFO(row.path);
        row.leave(mc);
        CHECK_FALSE(holds(row.flag));
        row.enter(mc);
        CHECK(holds(row.flag));
        row.leave(mc);
        CHECK_FALSE(holds(row.flag));
    }
}

TEST_CASE("Terminal.a key reads the modes it is encoded with without racing the parser thread",
          "[terminal][threading]")
{
    // What a key sends depends on modes the parser thread sets: the cursor keys and keypad modes (DECCKM,
    // DECNKM), LNM, DECBKM, the Kitty keyboard flags, modifyOtherKeys, win32-input-mode, and for a paste,
    // bracketed paste. A key typed in Insert mode is encoded under the input lock alone, so as not to wait
    // for a parse. The case to run under ThreadSanitizer, which reports each read against its write unless
    // the parser sets them under the input lock too.
    struct Row
    {
        std::string_view mode;
        std::string_view toggle;
        std::function<void(MockTerm<>&)> type;
    };
    auto const rows = std::array {
        Row { .mode = "DECCKM",
              .toggle = "\033[?1h\033[?1l",
              .type = [](auto& mc) { mc.sendKeyEvent(vtbackend::Key::UpArrow); } },
        Row { .mode = "DECNKM",
              .toggle = "\033=\033>",
              .type = [](auto& mc) { mc.sendKeyEvent(vtbackend::Key::Numpad_Add); } },
        Row { .mode = "LNM",
              .toggle = "\033[20h\033[20l",
              .type = [](auto& mc) { mc.sendKeyEvent(vtbackend::Key::Enter); } },
        Row { .mode = "DECBKM",
              .toggle = "\033[?67h\033[?67l",
              .type = [](auto& mc) { mc.sendKeyEvent(vtbackend::Key::Backspace); } },
        Row { .mode = "Kitty keyboard flags",
              .toggle = "\033[>1u\033[<u",
              .type = [](auto& mc) { mc.sendKeyEvent(vtbackend::Key::Escape); } },
        Row { .mode = "modifyOtherKeys",
              .toggle = "\033[>4;2m\033[>4;0m",
              .type = [](auto& mc) { mc.sendCharEvent(U'a', vtbackend::Modifiers { Modifier::Control }); } },
        Row { .mode = "win32-input-mode",
              .toggle = "\033[?9001h\033[?9001l",
              .type = [](auto& mc) { mc.sendCharEvent(U'a'); } },
        Row { .mode = "bracketed paste",
              .toggle = "\033[?2004h\033[?2004l",
              .type = [](auto& mc) { mc.terminal.sendPaste("x"); } },
    };

    for (auto const& row: rows)
    {
        INFO(row.mode);
        auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(40) } };
        (void) runWhileWriting([&] { mc.writeToScreen(row.toggle); }, [&] { row.type(mc); });
        CHECK_FALSE(mc.replyData().empty());
    }
}

TEST_CASE("Terminal.a key repeat reads DECARM without racing the parser thread", "[terminal][threading]")
{
    // A repeat is dropped while DECARM is reset. A key repeat reads it without the state lock while the
    // parser thread sets the DEC modes under it -- DECCKM here, kept in the same word. The case to run under
    // ThreadSanitizer, which reports the read against the write unless the DEC modes are atomic.
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(40) } };

    auto const keys =
        runWhileWriting([&] { mc.writeToScreen("\033[?1h\033[?1l"); },
                        [&] {
                            mc.terminal.sendCharEvent(U'a',
                                                      vtbackend::KeyIdentity { .unshiftedKey = U'a' },
                                                      vtbackend::KeyboardModifiers {},
                                                      vtbackend::KeyboardEventType::Repeat,
                                                      mc.steadyClock.now());
                        });

    CHECK(mc.replyData() == std::string(keys, 'a'));
}

TEST_CASE("Terminal.typing scrolls the view back down without racing the parser thread",
          "[terminal][threading]")
{
    // A key brings a view scrolled back into the history down to the bottom. The other threads move the view
    // under the state lock: here one scrolls it back, and output keeps a scrolled-back view on the lines it
    // shows. A key must not wait for that lock while the view is at the bottom already. The case to run under
    // ThreadSanitizer, which reports the key's read of the view against those writes.
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(20) }, LineCount(100) };
    vtbackend::test::printLines(mc, "line", 20);

    auto const keys = runWhileWriting(
        [&] {
            {
                auto const _ = std::scoped_lock { mc.terminal };
                (void) mc.terminal.viewport().scrollUp(LineCount(1));
            }
            mc.writeToScreen("x\r\n");
        },
        [&] { mc.sendCharEvent(U'a'); });

    CHECK(mc.replyData() == std::string(keys, 'a'));
}

TEST_CASE("Terminal.a key restarts the cursor blink without racing the render thread",
          "[terminal][threading]")
{
    // A key shows the cursor and restarts its blink, which the render thread advances as it ticks. Neither
    // takes the state lock for it. The case to run under ThreadSanitizer, which reports the key's writes
    // against the tick's.
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(40) } };
    mc.terminal.setCursorDisplay(vtbackend::CursorDisplay::Blink);
    auto const start = std::chrono::steady_clock::now();

    auto ticks = std::chrono::seconds(0);
    auto const keys = runWhileWriting(
        [&] {
            ticks += std::chrono::seconds(1);
            mc.terminal.tick(start + ticks);
        },
        [&] { mc.sendCharEvent(U'a'); });

    CHECK(mc.replyData() == std::string(keys, 'a'));
}
TEST_CASE("Terminal.composing does not race the render thread", "[terminal][threading]")
{
    // An input method's composition is drawn at the cursor. The GUI thread hands it over as it changes,
    // without the state lock, which composing does not wait for, as typing does not; the render thread copies
    // it as it fills a frame, under that lock. The case to run under ThreadSanitizer, which reports the GUI
    // thread's writes against the fill's copy unless the two share a lock.
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(40) } };
    auto const compositions = runWhileWriting([&] { (void) mc.terminal.refreshRenderBuffer(); },
                                              [&] {
                                                  mc.terminal.updateInputMethodPreeditString("kana");
                                                  mc.terminal.updateInputMethodPreeditString("");
                                              });
    CHECK(compositions > 0);
}
// }}}

// NOLINTEND(misc-const-correctness)
