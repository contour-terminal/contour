// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/core/Primitives.hpp>
#include <vtbackend/grid/CellUtil.hpp>
#include <vtbackend/input/vi/HintModeHandler.hpp>
#include <vtbackend/screen/StatusLineBuilder.hpp>
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
#include <catch2/generators/catch_generators.hpp>

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
using vtbackend::CellFlag;
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

namespace
{

/// Writes six numbered lines, so a 4-line page ends up with scrollback to scroll into.
///
/// Defaulted over the PTY like every other MockTerm helper (@see trimmedTextScreenshot); every
/// caller here uses the default one.
template <typename T = vtpty::MockPty>
void fillScrollback(MockTerm<T>& mc)
{
    mc.writeToScreen("line1\r\n"
                     "line2\r\n"
                     "line3\r\n"
                     "line4\r\n"
                     "line5\r\n"
                     "line6\r\n");
}

/// Fills the scrollback and parks the viewport two lines up in it.
///
/// The prologue every scroll-offset case below shares, stated once. Setting `autoScrollOnUpdate` is
/// left to the caller, because that is the one thing those cases disagree about.
template <typename T = vtpty::MockPty>
void parkViewportInScrollback(MockTerm<T>& mc)
{
    fillScrollback(mc);
    mc.terminal.viewport().scrollUp(LineCount(2));
    REQUIRE(mc.terminal.viewport().scrolled());
}

} // namespace

TEST_CASE("Terminal.BlinkingCursor", "[terminal]")
{
    auto mc = MockTerm { ColumnCount { 6 }, LineCount { 4 } };
    auto& terminal = mc.terminal;
    terminal.setCursorDisplay(vtbackend::CursorDisplay::Blink);
    auto constexpr BlinkInterval = chrono::milliseconds(500);
    terminal.setCursorBlinkingInterval(BlinkInterval);

    auto const clockBase = chrono::steady_clock::time_point();

    SECTION("over time")
    {
        auto const clockBeforeTurn = clockBase + BlinkInterval - chrono::milliseconds(1);
        terminal.tick(clockBeforeTurn);
        terminal.ensureFreshRenderBuffer();
        CHECK(terminal.cursorCurrentlyVisible());

        auto const clockAfterTurn = clockBase + BlinkInterval + chrono::milliseconds(1);
        terminal.tick(clockAfterTurn);
        terminal.ensureFreshRenderBuffer();
        CHECK(!terminal.cursorCurrentlyVisible());
    }

    SECTION("force show on keyboard input")
    {
        // get a state where the blinking cursor is not visible
        auto const clockBeforeTurn = clockBase + BlinkInterval + chrono::milliseconds(1);
        terminal.tick(clockBeforeTurn);
        terminal.ensureFreshRenderBuffer();
        CHECK(!terminal.cursorCurrentlyVisible());

        // type something into the terminal
        auto const clockAtInputEvent = clockBase + BlinkInterval + chrono::milliseconds(10);
        mc.sendCharEvent('x', vtbackend::Modifier {}, clockAtInputEvent);

        // now the cursor is visible before the interval has passed
        terminal.tick(clockBeforeTurn);
        terminal.ensureFreshRenderBuffer();
        CHECK(terminal.cursorCurrentlyVisible());
    }
}

TEST_CASE("Terminal.ModifierKeysDoNotScrollViewport", "[terminal]")
{
    // Set up a terminal with history capacity to allow scrollback
    auto mc = MockTerm { PageSize { LineCount(4), ColumnCount(6) }, LineCount(10) };
    auto& terminal = mc.terminal;

    // Enable ReportAllKeysAsEscapeCodes (kitty keyboard protocol)
    terminal.keyboardProtocol().enter(vtbackend::KeyboardEventFlag::ReportAllKeysAsEscapeCodes);

    // Fill terminal and generate scrollback history
    fillScrollback(mc);

    // Scroll up so viewport is not at bottom
    terminal.viewport().scrollUp(LineCount(2));
    REQUIRE(terminal.viewport().scrolled());
    auto const scrollOffsetBeforeKey = terminal.viewport().scrollOffset();

    SECTION("modifier-only press does not scroll")
    {
        mc.resetReplyData();
        terminal.sendKeyEvent(vtbackend::Key::LeftShift,
                              vtbackend::Modifiers { vtbackend::Modifier::Shift },
                              vtbackend::KeyboardEventType::Press,
                              std::chrono::steady_clock::now());

        // Viewport should remain scrolled
        CHECK(terminal.viewport().scrolled());
        CHECK(terminal.viewport().scrollOffset() == scrollOffsetBeforeKey);
        // Escape sequence should still have been sent to the application
        CHECK(!mc.replyData().empty());
    }

    SECTION("non-modifier press does scroll")
    {
        mc.resetReplyData();
        terminal.sendKeyEvent(vtbackend::Key::Enter,
                              vtbackend::Modifiers { vtbackend::Modifier::None },
                              vtbackend::KeyboardEventType::Press,
                              std::chrono::steady_clock::now());

        // Viewport should scroll to bottom
        CHECK(!terminal.viewport().scrolled());
    }

    SECTION("various modifier keys")
    {
        auto const modifierKeys = std::vector<vtbackend::Key> {
            vtbackend::Key::LeftShift,      vtbackend::Key::RightShift,     vtbackend::Key::LeftControl,
            vtbackend::Key::RightControl,   vtbackend::Key::LeftAlt,        vtbackend::Key::RightAlt,
            vtbackend::Key::LeftSuper,      vtbackend::Key::RightSuper,     vtbackend::Key::LeftHyper,
            vtbackend::Key::RightHyper,     vtbackend::Key::LeftMeta,       vtbackend::Key::RightMeta,
            vtbackend::Key::IsoLevel3Shift, vtbackend::Key::IsoLevel5Shift, vtbackend::Key::CapsLock,
            vtbackend::Key::NumLock,
        };

        for (auto const modKey: modifierKeys)
        {
            INFO("Testing modifier key: " << std::format("{}", modKey));

            // Reset viewport to scrolled position
            terminal.viewport().scrollUp(LineCount(2));
            REQUIRE(terminal.viewport().scrolled());

            terminal.sendKeyEvent(modKey,
                                  vtbackend::Modifiers { vtbackend::Modifier::None },
                                  vtbackend::KeyboardEventType::Press,
                                  std::chrono::steady_clock::now());

            CHECK(terminal.viewport().scrolled());
        }
    }
}

TEST_CASE("Terminal.localPathAtMousePosition", "[terminal]")
{
    namespace fs = std::filesystem;

    auto const tmpRoot =
        fs::temp_directory_path()
        / std::format("contour-local-path-{}", std::chrono::steady_clock::now().time_since_epoch().count());
    fs::create_directories(tmpRoot / "nested");
    {
        auto file = std::ofstream(tmpRoot / "nested" / "file.txt");
        file << "test";
    }

    auto const cleanup = crispy::Finally { [&]() { fs::remove_all(tmpRoot); } };
    auto constexpr PixelCoordinate = vtbackend::PixelCoordinate {};
    auto constexpr UiHandledHint = false;

    // Use forward-slash (generic) path forms throughout: that is how OSC-7 delivers the
    // working directory URL and how the path-detection regex expects paths to look, on every
    // platform (including Windows, where native paths use backslashes the regex would reject).
    auto const tmpRootUrl = "file://" + tmpRoot.generic_string();
    auto const expectedFile = (tmpRoot / "nested" / "file.txt").lexically_normal().string();

    SECTION("relative path")
    {
        auto mc = MockTerm { PageSize { LineCount(2), ColumnCount(80) } };
        auto& terminal = mc.terminal;
        terminal.setCurrentWorkingDirectory(tmpRootUrl);
        mc.writeToScreen("open nested/file.txt now");

        terminal.sendMouseMoveEvent(Modifier::None,
                                    CellLocation { .line = LineOffset(0), .column = ColumnOffset(10) },
                                    PixelCoordinate,
                                    UiHandledHint);

        auto const path = terminal.localPathAtMousePosition();
        REQUIRE(path.has_value());
        CHECK(*path == expectedFile);
    }

    SECTION("absolute path")
    {
        auto mc = MockTerm { PageSize { LineCount(2), ColumnCount(240) } };
        auto& terminal = mc.terminal;
        auto const absolutePath = (tmpRoot / "nested" / "file.txt").generic_string();
        mc.writeToScreen("open " + absolutePath);

        terminal.sendMouseMoveEvent(Modifier::None,
                                    CellLocation { .line = LineOffset(0), .column = ColumnOffset(8) },
                                    PixelCoordinate,
                                    UiHandledHint);

        auto const path = terminal.localPathAtMousePosition();
        REQUIRE(path.has_value());
        CHECK(*path == expectedFile);
    }

    SECTION("absolute path with tilde in a component")
    {
        // Windows resolves long user names to 8.3 short names containing a '~'
        // (e.g. C:\Users\RUNNER~1\...). The detection regex must keep such a component
        // intact instead of truncating the match at the tilde. A directory literally
        // named "short~1" reproduces the same shape on every platform.
        auto const shortDir = tmpRoot / "short~1";
        fs::create_directories(shortDir);
        {
            auto file = std::ofstream(shortDir / "file.txt");
            file << "test";
        }
        auto const tildePath = (shortDir / "file.txt").generic_string();
        auto const expectedTildeFile = (shortDir / "file.txt").lexically_normal().string();

        auto mc = MockTerm { PageSize { LineCount(2), ColumnCount(240) } };
        auto& terminal = mc.terminal;
        mc.writeToScreen("open " + tildePath);

        terminal.sendMouseMoveEvent(Modifier::None,
                                    CellLocation { .line = LineOffset(0), .column = ColumnOffset(8) },
                                    PixelCoordinate,
                                    UiHandledHint);

        auto const path = terminal.localPathAtMousePosition();
        REQUIRE(path.has_value());
        CHECK(*path == expectedTildeFile);
    }

    SECTION("missing path")
    {
        auto mc = MockTerm { PageSize { LineCount(2), ColumnCount(80) } };
        auto& terminal = mc.terminal;
        terminal.setCurrentWorkingDirectory(tmpRootUrl);
        mc.writeToScreen("open nested/missing.txt now");

        terminal.sendMouseMoveEvent(Modifier::None,
                                    CellLocation { .line = LineOffset(0), .column = ColumnOffset(10) },
                                    PixelCoordinate,
                                    UiHandledHint);

        CHECK_FALSE(terminal.localPathAtMousePosition().has_value());
    }
}

TEST_CASE("Terminal.AutoScrollOnUpdate", "[terminal]")
{
    // Set up a terminal with history capacity to allow scrollback.
    auto mc = MockTerm { PageSize { LineCount(4), ColumnCount(6) }, LineCount(10) };
    auto& terminal = mc.terminal;

    // Fill terminal and generate scrollback history.
    fillScrollback(mc);

    auto const anyModifiers = vtbackend::Modifiers { vtbackend::Modifier::None };

    SECTION("keypress honors autoScrollOnUpdate=No")
    {
        // `No` means the viewport stays where the user parked it, and a keystroke is no exception:
        // a snap on input is the terminal moving the viewport on the user's behalf, which is exactly
        // what the setting exists to decline. It is xterm's own default too -- its scrollKey
        // resource is false while scrollTtyOutput is true.
        terminal.settings().autoScrollOnUpdate = vtbackend::AutoScrollOnUpdate::No;
        terminal.viewport().scrollUp(LineCount(2));
        REQUIRE(terminal.viewport().scrolled());
        auto const offsetBefore = terminal.viewport().scrollOffset();

        terminal.sendKeyEvent(vtbackend::Key::Enter,
                              anyModifiers,
                              vtbackend::KeyboardEventType::Press,
                              std::chrono::steady_clock::now());

        CHECK(terminal.viewport().scrollOffset() == offsetBefore);
    }

    SECTION("keypress honors autoScrollOnUpdate=Yes (default)")
    {
        REQUIRE(terminal.settings().autoScrollOnUpdate == vtbackend::AutoScrollOnUpdate::Yes);
        terminal.viewport().scrollUp(LineCount(2));
        REQUIRE(terminal.viewport().scrolled());

        terminal.sendKeyEvent(vtbackend::Key::Enter,
                              anyModifiers,
                              vtbackend::KeyboardEventType::Press,
                              std::chrono::steady_clock::now());

        CHECK(!terminal.viewport().scrolled());
    }

    SECTION("char input honors autoScrollOnUpdate=No")
    {
        // The same rule as the keypress case above, exercised through the char path.
        terminal.settings().autoScrollOnUpdate = vtbackend::AutoScrollOnUpdate::No;
        terminal.viewport().scrollUp(LineCount(2));
        REQUIRE(terminal.viewport().scrolled());
        auto const offsetBefore = terminal.viewport().scrollOffset();

        terminal.sendCharEvent(U'a',
                               vtbackend::KeyIdentity { .unshiftedKey = U'a' },
                               anyModifiers,
                               vtbackend::KeyboardEventType::Press,
                               std::chrono::steady_clock::now());

        CHECK(terminal.viewport().scrollOffset() == offsetBefore);
    }

    SECTION("char input honors autoScrollOnUpdate=Yes")
    {
        REQUIRE(terminal.settings().autoScrollOnUpdate == vtbackend::AutoScrollOnUpdate::Yes);
        terminal.viewport().scrollUp(LineCount(2));
        REQUIRE(terminal.viewport().scrolled());

        terminal.sendCharEvent(U'a',
                               vtbackend::KeyIdentity { .unshiftedKey = U'a' },
                               anyModifiers,
                               vtbackend::KeyboardEventType::Press,
                               std::chrono::steady_clock::now());

        CHECK(!terminal.viewport().scrolled());
    }

    // A key/char *release* is never typed content. When the active keyboard protocol reports
    // releases to the application (win32-input-mode here, or the Kitty keyboard protocol) the release
    // still produces PTY input, but it must NOT snap the viewport back to the bottom -- otherwise
    // releasing a viewport-scroll shortcut such as Shift+Up (whose press the GUI already consumed as
    // a ScrollOneUp action) would immediately undo the scroll. @see Terminal::sendKeyEvent, which
    // states the same at the guard.
    SECTION("key release does not scroll to bottom even when it generates input")
    {
        // Win32 input mode reports both presses and releases to the application, so the release below
        // actually reaches the input generator (in legacy mode releases generate nothing at all).
        terminal.setMode(vtbackend::DECMode::Win32InputMode, true);

        // Sanity check: a *press* in this mode really does generate input and snap to the bottom, so
        // the release assertion below is not vacuous.
        terminal.viewport().scrollUp(LineCount(2));
        REQUIRE(terminal.viewport().scrolled());
        terminal.sendKeyEvent(vtbackend::Key::UpArrow,
                              anyModifiers,
                              vtbackend::KeyboardEventType::Press,
                              std::chrono::steady_clock::now());
        REQUIRE(!terminal.viewport().scrolled());

        // The release generates input too, but must leave the viewport parked where the user put it.
        terminal.viewport().scrollUp(LineCount(2));
        REQUIRE(terminal.viewport().scrolled());
        auto const offsetBefore = terminal.viewport().scrollOffset();

        terminal.sendKeyEvent(vtbackend::Key::UpArrow,
                              anyModifiers,
                              vtbackend::KeyboardEventType::Release,
                              std::chrono::steady_clock::now());

        CHECK(terminal.viewport().scrolled());
        CHECK(terminal.viewport().scrollOffset() == offsetBefore);
    }

    SECTION("char release does not scroll to bottom even when it generates input")
    {
        terminal.setMode(vtbackend::DECMode::Win32InputMode, true);

        terminal.viewport().scrollUp(LineCount(2));
        REQUIRE(terminal.viewport().scrolled());
        auto const offsetBefore = terminal.viewport().scrollOffset();

        terminal.sendCharEvent(U'a',
                               vtbackend::KeyIdentity { .unshiftedKey = U'a' },
                               anyModifiers,
                               vtbackend::KeyboardEventType::Release,
                               std::chrono::steady_clock::now());

        CHECK(terminal.viewport().scrolled());
        CHECK(terminal.viewport().scrollOffset() == offsetBefore);
    }

    SECTION("scrollbackBufferCleared (CSI 3 J) honors autoScrollOnUpdate=No")
    {
        terminal.settings().autoScrollOnUpdate = vtbackend::AutoScrollOnUpdate::No;
        terminal.viewport().scrollUp(LineCount(2));
        REQUIRE(terminal.viewport().scrolled());
        auto const offsetBefore = terminal.viewport().scrollOffset();

        mc.writeToScreen("\x1b[3J");

        CHECK(terminal.viewport().scrollOffset() == offsetBefore);
    }

    // `bufferChanged` is exercised directly here to pin the funnel itself; the full DECSET
    // 1049/1047/47 path has its own case (@see Terminal.AltScreen.PreservesPrimaryScrollOffset).
    SECTION("bufferChanged honors autoScrollOnUpdate=No")
    {
        terminal.settings().autoScrollOnUpdate = vtbackend::AutoScrollOnUpdate::No;
        terminal.viewport().scrollUp(LineCount(2));
        REQUIRE(terminal.viewport().scrolled());
        auto const offsetBefore = terminal.viewport().scrollOffset();

        terminal.bufferChanged(vtbackend::ScreenType::Primary);

        CHECK(terminal.viewport().scrollOffset() == offsetBefore);
    }

    SECTION("bufferChanged honors autoScrollOnUpdate=Yes")
    {
        REQUIRE(terminal.settings().autoScrollOnUpdate == vtbackend::AutoScrollOnUpdate::Yes);
        terminal.viewport().scrollUp(LineCount(2));
        REQUIRE(terminal.viewport().scrolled());

        terminal.bufferChanged(vtbackend::ScreenType::Primary);

        CHECK(!terminal.viewport().scrolled());
    }

    SECTION("leaving Vi mode honors autoScrollOnUpdate=No")
    {
        // Escape out of Normal mode means "back to typing", and the snap that used to follow is the
        // same automatic move as the one on a keystroke -- so the same setting declines it. Reading
        // the scrollback in Vi mode and then leaving it should not throw the position away.
        terminal.settings().autoScrollOnUpdate = vtbackend::AutoScrollOnUpdate::No;
        terminal.inputHandler().setMode(vtbackend::ViMode::Normal);
        terminal.viewport().scrollUp(LineCount(2));
        REQUIRE(terminal.viewport().scrolled());
        auto const offsetBefore = terminal.viewport().scrollOffset();

        terminal.inputHandler().setMode(vtbackend::ViMode::Insert);

        CHECK(terminal.viewport().scrollOffset() == offsetBefore);
    }

    SECTION("leaving Vi mode honors autoScrollOnUpdate=Yes")
    {
        REQUIRE(terminal.settings().autoScrollOnUpdate == vtbackend::AutoScrollOnUpdate::Yes);
        terminal.inputHandler().setMode(vtbackend::ViMode::Normal);
        terminal.viewport().scrollUp(LineCount(2));
        REQUIRE(terminal.viewport().scrolled());

        terminal.inputHandler().setMode(vtbackend::ViMode::Insert);

        CHECK(!terminal.viewport().scrolled());
    }
}

TEST_CASE("Terminal.AltScreen.PreservesPrimaryScrollOffset", "[terminal]")
{
    // The scroll offset is per-screen state. Entering the alternate screen must not consume the
    // primary screen's viewport position, and leaving must hand it back.
    //
    // Driven through the real DECSET sequences rather than through bufferChanged(): the entry
    // sequence also CLEARS the alternate page, and that clear is the whole bug -- Screen::clearScreen
    // scrolls a full page into "history", which used to clamp the shared scroll offset against the
    // alternate screen's (always empty) scrollback and so destroyed the primary position outright,
    // before bufferChanged() ever got to consult autoScrollOnUpdate.
    //
    // 1049, 1047 and 47 are three views of one piece of state and one code path
    // (@see alternateScreenBehavior), so each must preserve the offset the same way.
    auto const mode = GENERATE(1049, 1047, 47);
    CAPTURE(mode);

    auto mc = MockTerm { PageSize { LineCount(4), ColumnCount(6) }, LineCount(10) };
    auto& terminal = mc.terminal;

    terminal.settings().autoScrollOnUpdate = vtbackend::AutoScrollOnUpdate::No;
    parkViewportInScrollback(mc);
    auto const offsetBefore = terminal.viewport().scrollOffset();

    mc.writeToScreen(std::format("\033[?{}h", mode));
    REQUIRE(terminal.isAlternateScreen());

    // The alternate screen has no scrollback of its own, so it can only ever sit at the bottom.
    CHECK(terminal.viewport().scrollOffset() == vtbackend::ScrollOffset(0));

    mc.writeToScreen(std::format("\033[?{}l", mode));
    REQUIRE(!terminal.isAlternateScreen());

    CHECK(terminal.viewport().scrollOffset() == offsetBefore);
}

TEST_CASE("Terminal.MultiPage.PreservesPrimaryScrollOffset", "[terminal]")
{
    // The scroll offset is per PAGE, not per primary/alternate pair. VT420 page memory gives every
    // one of the sixteen pages its own Screen with its own Grid, and only page 1 is built with any
    // scrollback at all (@see the Terminal constructor, which hands pages 2..16 LineCount(0)) -- so
    // "the primary pages share the primary scrollback" is false, and keying the saved offset on
    // primary-vs-alternate would let a round trip through any of them clobber page 1's position:
    // its zero-history clamp drives the live offset to zero, and coming back parks that zero in the
    // slot the real offset was in.
    auto mc = MockTerm { PageSize { LineCount(4), ColumnCount(6) }, LineCount(10) };
    auto& terminal = mc.terminal;

    terminal.settings().autoScrollOnUpdate = vtbackend::AutoScrollOnUpdate::No;
    parkViewportInScrollback(mc);
    auto const offsetBefore = terminal.viewport().scrollOffset();

    mc.writeToScreen("\033[U"); // NP: forward one page, onto a page with no scrollback.
    REQUIRE(terminal.cursorPageIndex() == vtbackend::PageIndex(1));
    CHECK(terminal.viewport().scrollOffset() == vtbackend::ScrollOffset(0));

    mc.writeToScreen("\033[V"); // PP: back to page 1.
    REQUIRE(terminal.cursorPageIndex() == vtbackend::PageIndex(0));

    CHECK(terminal.viewport().scrollOffset() == offsetBefore);
}

TEST_CASE("Terminal.StatusLine.ScrollDoesNotMoveMainViewport", "[terminal]")
{
    // A status line is a Screen of its own -- one row, no scrollback -- and DECSASD makes it the
    // active display, so an application writing past that row scrolls IT. Screen::scrollUp reports
    // that like any other scroll, and following it clamped the main display's offset against the
    // status line's (empty) history, i.e. straight to zero.
    auto mc = MockTerm { PageSize { LineCount(4), ColumnCount(6) }, LineCount(10) };
    auto& terminal = mc.terminal;

    terminal.settings().autoScrollOnUpdate = vtbackend::AutoScrollOnUpdate::No;
    parkViewportInScrollback(mc);
    auto const offsetBefore = terminal.viewport().scrollOffset();

    mc.writeToScreen("\033[2$~"); // DECSSDT: show the host-writable status line
    mc.writeToScreen("\033[1$}"); // DECSASD: make it the active display
    mc.writeToScreen("status\r\n\r\n\r\n");
    mc.writeToScreen("\033[0$}"); // DECSASD: back to the main display

    CHECK(terminal.viewport().scrollOffset() == offsetBefore);
}

TEST_CASE("Terminal.MultiPage.DecoupledPageChangeLeavesViewportAlone", "[terminal]")
{
    // With DECPCCM reset the drawn page stops following the cursor page, so NP/PP moves where output
    // GOES without changing what the user is looking at. The viewport belongs to the drawn page, so a
    // page switch that does not change the drawn page must not touch it -- saving and restoring on the
    // cursor page instead yanked the live viewport to the incoming page's offset (zero, for the pages
    // built with no scrollback) and back again.
    auto mc = MockTerm { PageSize { LineCount(4), ColumnCount(6) }, LineCount(10) };
    auto& terminal = mc.terminal;

    fillScrollback(mc);
    terminal.settings().autoScrollOnUpdate = vtbackend::AutoScrollOnUpdate::No;
    parkViewportInScrollback(mc);
    auto const offsetBefore = terminal.viewport().scrollOffset();

    mc.writeToScreen("\033[?64l"); // DECPCCM reset: decouple the drawn page from the cursor page.
    REQUIRE(!terminal.isModeEnabled(vtbackend::DECMode::PageCursorCoupling));

    mc.writeToScreen("\033[U"); // NP: forward one page -- the CURSOR's page, not the drawn one.
    REQUIRE(terminal.cursorPageIndex() == vtbackend::PageIndex(1));
    CHECK(terminal.viewport().scrollOffset() == offsetBefore);

    mc.writeToScreen("\033[V"); // PP: back again.
    REQUIRE(terminal.cursorPageIndex() == vtbackend::PageIndex(0));
    CHECK(terminal.viewport().scrollOffset() == offsetBefore);
}

TEST_CASE("Terminal.TopAnchoredRegion.PartialScrollKeepsViCursorOnItsText", "[terminal]")
{
    // The Normal-mode cursor is an anchor into the grid like the viewport is, so the same rebase
    // applies to it: a top-anchored region scroll pushes a line into the scrollback, and a cursor
    // parked up there has to move with its text. It used to be left where it was while the viewport
    // followed -- so it drifted onto different text, one row per scroll, which is the same bug the
    // viewport had and is why the boundary is now reported rather than inferred.
    auto mc = MockTerm { PageSize { LineCount(6), ColumnCount(8) }, LineCount(50) };
    auto& terminal = mc.terminal;

    for (auto const i: std::views::iota(1, 21))
        mc.writeToScreen(std::format("h{:02}\r\n", i));

    terminal.settings().autoScrollOnUpdate = vtbackend::AutoScrollOnUpdate::No;

    // Normal mode FIRST, so that scrolling the viewport runs onViewportChanged()'s clamp and parks the
    // Vi cursor on a scrollback row -- entering the mode alone leaves it on the live cursor.
    terminal.inputHandler().setMode(vtbackend::ViMode::Normal);
    terminal.viewport().scrollUp(LineCount(8));
    REQUIRE(terminal.viewport().scrolled());

    // Up into the MIDDLE of the viewport. At an edge the cursor is dragged along by
    // onViewportChanged()'s clamp on every scroll, which happens to track the content and would hide
    // the drift this pins; in the middle the clamp never fires, so only a real rebase can keep it on
    // its text.
    for ([[maybe_unused]] auto const _: std::views::iota(0, 3))
        terminal.sendCharEvent(U'k',
                               vtbackend::KeyIdentity { .unshiftedKey = U'k' },
                               vtbackend::Modifiers {},
                               vtbackend::KeyboardEventType::Press,
                               std::chrono::steady_clock::now());

    auto const cursorLineBefore = terminal.normalModeCursorPosition().line;
    auto const cursorLineTextBefore = terminal.primaryScreen().grid().lineText(cursorLineBefore);
    REQUIRE(cursorLineTextBefore.starts_with("h"));
    REQUIRE(cursorLineBefore > terminal.viewport().topLine());

    // Top-anchored partial region (rows 1..3), cursor at the region bottom.
    mc.writeToScreen("\033[1;3r");
    mc.writeToScreen("\033[3;1H");

    for ([[maybe_unused]] auto const _: std::views::iota(0, 10))
        mc.writeToScreen("\033[S");

    CHECK(terminal.primaryScreen().grid().lineText(terminal.normalModeCursorPosition().line)
          == cursorLineTextBefore);
}

TEST_CASE("Terminal.AltScreen.HonorsAutoScrollOnUpdateWhenLeaving", "[terminal]")
{
    // With the setting ON, coming back from the alternate screen snaps to the bottom -- the restored
    // offset is what bufferChanged()'s gated snap then acts upon, which is the whole point of
    // restoring before raising it.
    auto mc = MockTerm { PageSize { LineCount(4), ColumnCount(6) }, LineCount(10) };
    auto& terminal = mc.terminal;

    REQUIRE(terminal.settings().autoScrollOnUpdate == vtbackend::AutoScrollOnUpdate::Yes);
    parkViewportInScrollback(mc);

    mc.writeToScreen("\033[?1049h");
    mc.writeToScreen("\033[?1049l");

    CHECK(!terminal.viewport().scrolled());
}

TEST_CASE("Terminal.DECCARA", "[terminal]")
{
    auto mock = MockTerm { ColumnCount(5), LineCount(5) };
    auto constexpr ClockBase = chrono::steady_clock::time_point();
    mock.terminal.tick(ClockBase);
    mock.terminal.ensureFreshRenderBuffer();
    CHECK(trimmedTextScreenshot(mock).empty());

    mock.writeToScreen("12345\r\n"
                       "67890\r\n"
                       "ABCDE\r\n"
                       "abcde\r\n"
                       "fghij");

    mock.terminal.tick(ClockBase + chrono::seconds(1));
    mock.terminal.ensureFreshRenderBuffer();
    CHECK("12345\n67890\nABCDE\nabcde\nfghij" == trimmedTextScreenshot(mock));

    auto const top = 2;
    auto const left = 3;
    auto const bottom = 4;
    auto const right = 5;
    mock.writeToScreen(
        std::format("\033[{};{};{};{};{}$r", top, left, bottom, right, "1;38:2::171:178:191;4"));

    mock.terminal.tick(ClockBase + chrono::seconds(2));
    mock.terminal.ensureFreshRenderBuffer();
    CHECK("12345\n67890\nABCDE\nabcde\nfghij" == trimmedTextScreenshot(mock));

    // Just peak into it and test. That's not really 100% precise, tbh., but
    // i'd like to keep on going and have fun doing the project and not die
    // early due to a overdose of TDD. :-)
    for (auto line = top; line <= bottom; ++line)
        for (auto column = left; column <= right; ++column)
        {
            // clang-format off
            auto const& someCell = mock.terminal.primaryScreen().at(LineOffset(line - 1), ColumnOffset(column - 1));
            auto const rgb = someCell.foregroundColor().rgb();
            auto const colorDec = std::format("{}/{}/{}", unsigned(rgb.red), unsigned(rgb.green), unsigned(rgb.blue));
            INFO(std::format("at line {} column {}, flags {}", line, column, someCell.flags()));
            CHECK(colorDec == "171/178/191");
            CHECK(someCell.isFlagEnabled(vtbackend::CellFlag::Bold));
            CHECK(someCell.isFlagEnabled(vtbackend::CellFlag::Underline));
            // clang-format on
        }
}

TEST_CASE("Terminal.CaptureScreenBuffer")
{
    auto constexpr ClockBase = chrono::steady_clock::time_point();
    auto constexpr NoLogicalLines = 0; // 0=false
    auto constexpr NumberOfLinesToCapture = 7;
    auto constexpr MaxHistoryLineCount = LineCount(20);

    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(5) }, MaxHistoryLineCount };

    logScreenText(mock.terminal, "init");

    mock.terminal.tick(ClockBase);
    mock.terminal.ensureFreshRenderBuffer();

    // fill screen buffer (5 lines into history + full 5 lines page buffer)
    for (int i = 1; i <= 10; ++i)
    {
        mock.writeToScreen(std::format("\r\n{}", i));
        logScreenText(mock.terminal, std::format("write i {}", i));
    }

    mock.terminal.tick(ClockBase + chrono::seconds(1));
    mock.terminal.ensureFreshRenderBuffer();
    auto const actualScreen1 = trimmedTextScreenshot(mock);
    REQUIRE("6\n7\n8\n9\n10" == actualScreen1);
    logScreenText(mock.terminal, "fini");

    // XTCAPTURE now carries a ',' intermediate (`CSI > Ps ; Ps , t`) so the bare `CSI > Ps t` can be
    // the standard xterm XTSMTITLE. @see Functions.h XTCAPTURE / XTSMTITLE.
    mock.writeToScreen(std::format("\033[>{};{},t", NoLogicalLines, NumberOfLinesToCapture));
    mock.terminal.flushInput();
    logScreenText(mock.terminal, "after flush");

    mock.terminal.tick(ClockBase + chrono::seconds(1));
    mock.terminal.ensureFreshRenderBuffer();
    auto const actualScreen2 = trimmedTextScreenshot(mock);
    CHECK(actualScreen1 == actualScreen2);

    CHECK(e(mock.replyData()) == e("\033^314;4\n5\n6\n7\n8\n9\n10\n\033\\\033^314;\033\\"));

    // I just realized we have a test as Screen.captureBuffer already.
    // But here we test the full cycle.
}

TEST_CASE("Terminal.RIS", "[terminal]")
{
    using namespace vtbackend;

    constexpr auto RIS = "\033c"sv;

    auto mc = MockTerm { ColumnCount(20), LineCount(5) };
    mc.terminal.ensureFreshRenderBuffer();

    mc.terminal.tick(mc.terminal.currentTime() + chrono::milliseconds(500));
    mc.terminal.ensureFreshRenderBuffer();

    mc.terminal.setStatusDisplay(StatusDisplayType::Indicator);
    mc.terminal.tick(mc.terminal.currentTime() + chrono::milliseconds(500));
    mc.terminal.ensureFreshRenderBuffer();
    mc.writeToScreen(RIS);
    mc.terminal.forceRedraw({});

    CHECK(mc.terminal.statusDisplayType() == StatusDisplayType::None);
}

TEST_CASE("Terminal.RIS.keepsFrozenModesAppliedToInputGenerator", "[terminal]")
{
    using namespace vtbackend;

    // DECCKM, DECNKM and DECBKM are mirrored inside the input generator rather than read back from
    // the mode register, so hardReset() has to clear that mirror BEFORE it replays `frozenModes` --
    // not after. Clearing it afterwards silently undoes the replay, and because setMode() early-
    // returns on a frozen mode, nothing can ever resync the two again: the arrow keys would emit
    // `ESC [ A` for the rest of the session while DECRQM kept reporting DECCKM as set.
    auto mc = MockTerm { ColumnCount(20), LineCount(5) };
    mc.terminal.settings().frozenModes[DECMode::UseApplicationCursorKeys] = true;

    SECTION("a frozen DECCKM survives RIS")
    {
        mc.writeToScreen("\033[?1h"sv); // DECSET DECCKM -- refused, the mode is frozen on
        mc.terminal.freezeMode(DECMode::UseApplicationCursorKeys, true);
        REQUIRE(mc.terminal.applicationCursorKeys());

        mc.writeToScreen("\033c"sv); // RIS

        CHECK(mc.terminal.isModeEnabled(DECMode::UseApplicationCursorKeys));
        CHECK(mc.terminal.applicationCursorKeys()); // the mirror must agree with the mode register
    }

    SECTION("an unfrozen DECCKM is still reset by RIS")
    {
        mc.terminal.settings().frozenModes.clear();
        mc.writeToScreen("\033[?1h"sv); // DECSET DECCKM
        REQUIRE(mc.terminal.applicationCursorKeys());

        mc.writeToScreen("\033c"sv); // RIS

        CHECK(!mc.terminal.isModeEnabled(DECMode::UseApplicationCursorKeys));
        CHECK(!mc.terminal.applicationCursorKeys());
    }
}

TEST_CASE("Terminal.RIS.resetsPassiveMouseTracking", "[terminal]")
{
    using namespace vtbackend;

    // DECMode 2029 is mirrored in the input generator as well. It was the one mirror hardReset() did
    // not clear, so after RIS the mode register read "off" while the generator still appended the
    // passive-tracking parameter to every SGR mouse report -- a fourth field the application cannot
    // parse -- and kept the alternate-scroll path gated shut.
    auto mc = MockTerm { ColumnCount(20), LineCount(5) };

    mc.writeToScreen("\033[?2029h"sv);
    REQUIRE(mc.terminal.isModeEnabled(DECMode::MousePassiveTracking));

    mc.writeToScreen("\033c"sv); // RIS

    CHECK(!mc.terminal.isModeEnabled(DECMode::MousePassiveTracking));

    // Re-enabling must take effect, which it cannot if the mirror never went back to false.
    mc.writeToScreen("\033[?2029h"sv);
    CHECK(mc.terminal.isModeEnabled(DECMode::MousePassiveTracking));
}

TEST_CASE("Terminal.forceRedraw keeps the cell size", "[terminal]")
{
    using namespace vtbackend;

    // forceRedraw() briefly resizes to one extra column to make applications redraw, then back.
    // resizeScreen() derives the cell size as pixels/page, so each call must carry the pixel size OF
    // THE PAGE IT NAMES: handing the real page's pixels to the one-column-wider page derived a cell
    // width of cellW*columns/(columns+1) and pushed that to the child on the first SIGWINCH. A
    // program that reads TIOCGWINSZ once at startup -- img2sixel and chafa both do -- sizes its image
    // canvas from that, and the restoring resize issues no third SIGWINCH to correct it.
    //
    // The artificialSleep hook runs between the two resizes, which is the only place the transient
    // state is observable at all.
    auto constexpr CellSize = ImageSize { Width(9), Height(18) };

    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(10) } };
    mc.terminal.setCellPixelSize(CellSize);
    REQUIRE(mc.terminal.cellPixelSize() == CellSize);

    auto cellSizeWhileWidened = ImageSize {};
    mc.terminal.forceRedraw([&]() { cellSizeWhileWidened = mc.terminal.cellPixelSize(); });

    CHECK(cellSizeWhileWidened == CellSize);
    CHECK(mc.terminal.cellPixelSize() == CellSize);
}

TEST_CASE("Terminal.clampedTotalPageSize", "[terminal]")
{
    using namespace vtbackend;

    // clampedTotalPageSize() is the single authority for the resize lower-bound: the total page must leave
    // at least one main-display line ON TOP of the visible status line(s). Frontend callers (helper.cpp's
    // resize early-out, TerminalSessionManager's renderer-geometry sync) query it so their bookkeeping
    // matches what resizeScreen() actually applies. This pins that contract for both status-line states.
    auto mc = MockTerm { ColumnCount(20), LineCount(5) };

    SECTION("no status line: clamps only to a 1x1 floor")
    {
        mc.terminal.setStatusDisplay(StatusDisplayType::None);
        REQUIRE(mc.terminal.statusLineHeight() == LineCount(0));

        // A sub-1 request is raised to the 1x1 minimum; a comfortably-sized request passes through.
        CHECK(mc.terminal.clampedTotalPageSize(PageSize { LineCount(0), ColumnCount(0) })
              == PageSize { LineCount(1), ColumnCount(1) });
        CHECK(mc.terminal.clampedTotalPageSize(PageSize { LineCount(10), ColumnCount(40) })
              == PageSize { LineCount(10), ColumnCount(40) });
    }

    SECTION("indicator status line: floor rises to statusLineHeight()+1")
    {
        mc.terminal.setStatusDisplay(StatusDisplayType::Indicator);
        REQUIRE(mc.terminal.statusLineHeight() == LineCount(1));

        // The GUI-default case behind the resize-early-out finding: a 1-line request clamps up to 2, so a
        // caller comparing its own LineCount(1)-clamped value against the applied size must query this to
        // agree (otherwise 1 != 2 defeats the early-out forever below two cell-rows).
        CHECK(mc.terminal.clampedTotalPageSize(PageSize { LineCount(1), ColumnCount(20) })
              == PageSize { LineCount(2), ColumnCount(20) });
        // A total already above the floor is unchanged.
        CHECK(mc.terminal.clampedTotalPageSize(PageSize { LineCount(5), ColumnCount(20) })
              == PageSize { LineCount(5), ColumnCount(20) });
    }

    SECTION("the clamp matches what resizeScreen() actually applies")
    {
        mc.terminal.setStatusDisplay(StatusDisplayType::Indicator);
        auto const requested = PageSize { LineCount(1), ColumnCount(20) };
        {
            auto const _ = std::scoped_lock { mc.terminal };
            mc.terminal.resizeScreen(requested, std::nullopt);
        }
        // resizeScreen() clamped the total internally; clampedTotalPageSize() predicts that exact result.
        CHECK(mc.terminal.totalPageSize() == mc.terminal.clampedTotalPageSize(requested));
    }
}

TEST_CASE("Terminal.DECCOLM.doesNotDoubleCountStatusLine", "[terminal]")
{
    using namespace vtbackend;

    // DECCOLM changes only the column count. The line count it hands the frontend must be the USABLE
    // main-page count: the frontend adds the status-line height back itself (exactly as it does for
    // XTWINOPS `CSI 8 t`). Passing the *total* double-counts the status line, so the window grows one
    // row on every DECCOLM whenever the indicator status line is shown (29 -> 30 -> 31). Regression for
    // that, driven through the same requestWindowResize()/resizeScreen() path as the real frontend.
    auto mc = MockTerm { PageSize { LineCount(24), ColumnCount(80) } };
    mc.terminal.setStatusDisplay(StatusDisplayType::Indicator);
    REQUIRE(mc.terminal.statusLineHeight() == LineCount(1));

    auto const usableBefore = mc.terminal.pageSize().lines; // main page only; excludes the status line

    // Allow 80<->132, then switch to 132 columns.
    mc.writeToScreen("\033[?40h\033[?3h");

    REQUIRE(mc.requestedPageSize.has_value());
    CHECK(mc.requestedPageSize->columns == ColumnCount(132));
    // The request names the USABLE line count, unchanged — NOT the total (usable + status line).
    CHECK(mc.requestedPageSize->lines == usableBefore);

    // The main page did not creep: only the columns changed.
    CHECK(mc.terminal.pageSize().lines == usableBefore);
    CHECK(mc.terminal.pageSize().columns == ColumnCount(132));

    // A second DECCOLM (back to 80 columns) still does not creep the line count.
    mc.writeToScreen("\033[?3l");
    CHECK(mc.terminal.pageSize().lines == usableBefore);
    CHECK(mc.terminal.pageSize().columns == ColumnCount(80));
}

TEST_CASE("Terminal.DECSCPP.doesNotDoubleCountStatusLine", "[terminal]")
{
    using namespace vtbackend;

    // DECSCPP (`CSI Ps $ |`) selects 80/132 columns per page and shares DECCOLM's frontend contract: the
    // USABLE line count is passed and the frontend adds the status-line height back. Passing the total
    // double-counts it. Same regression as DECCOLM, different sequence.
    auto mc = MockTerm { PageSize { LineCount(24), ColumnCount(80) } };
    mc.terminal.setStatusDisplay(StatusDisplayType::Indicator);
    REQUIRE(mc.terminal.statusLineHeight() == LineCount(1));

    auto const usableBefore = mc.terminal.pageSize().lines;

    mc.writeToScreen("\033[132$|");
    REQUIRE(mc.requestedPageSize.has_value());
    CHECK(mc.requestedPageSize->columns == ColumnCount(132));
    CHECK(mc.requestedPageSize->lines == usableBefore);
    CHECK(mc.terminal.pageSize().lines == usableBefore);
    CHECK(mc.terminal.pageSize().columns == ColumnCount(132));
}

TEST_CASE("Terminal.DECCOLM.resizesGridSynchronously", "[terminal]")
{
    using namespace vtbackend;

    // DECCOLM must resize the grid authoritatively and synchronously with sequence processing, so that
    // an application which switches to 132 columns and immediately draws to the new width — vttest's
    // cursor-movement test addresses its box border at absolute column 132 right after the switch —
    // lands its cells on the new width, even when the frontend cannot resize the window yet. Model that
    // frontend with refuseWindowResize: the grid must be 132 wide right after DECCOLM regardless.
    auto mc = MockTerm { PageSize { LineCount(24), ColumnCount(80) } };
    mc.refuseWindowResize = true;

    mc.writeToScreen("\033[?40h\033[?3h"); // allow 80<->132, then DECCOLM 132
    // Authoritative: the grid is 132 wide now, not one GUI round-trip later.
    REQUIRE(mc.terminal.pageSize().columns == ColumnCount(132));

    // Absolute addressing to the far right lands on the new width: a marker at column 132 is at
    // column-offset 131. On the old 80-column grid it would have clamped to column 80.
    mc.writeToScreen("\033[1;132HX");
    CHECK(mc.terminal.currentScreen().at(LineOffset(0), ColumnOffset(131)).toUtf8() == "X");

    // And back to 80 columns, synchronously.
    mc.writeToScreen("\033[?3l");
    CHECK(mc.terminal.pageSize().columns == ColumnCount(80));
}

TEST_CASE("Terminal.RIS.resetsDECCOLM", "[terminal]")
{
    using namespace vtbackend;

    // esctest RISTests.test_RIS_ResetDECCOLM: RIS returns a DECCOLM 132-column switch to 80 columns,
    // provided 80/132 switching was allowed and the terminal is currently in 132 columns. The check must
    // precede the mode reset (older xterm reset the mode first and could no longer tell).
    SECTION("RIS returns a 132-column DECCOLM switch to 80")
    {
        auto mc = MockTerm { PageSize { LineCount(24), ColumnCount(80) } };
        mc.writeToScreen("\033[?40h\033[?3h"); // allow 80<->132, then DECCOLM 132
        REQUIRE(mc.terminal.pageSize().columns == ColumnCount(132));
        mc.writeToScreen("\033c"); // RIS
        CHECK(mc.terminal.pageSize().columns == ColumnCount(80));
    }

    SECTION("RIS leaves an 80-column terminal at 80 (no spurious resize)")
    {
        auto mc = MockTerm { PageSize { LineCount(24), ColumnCount(80) } };
        mc.writeToScreen("\033c"); // RIS with no DECCOLM in effect
        CHECK(mc.terminal.pageSize().columns == ColumnCount(80));
    }
}

TEST_CASE("Terminal.DECNCSM", "[terminal]")
{
    using namespace vtbackend;

    // DECNCSM (DEC private mode 95): when set, DECCOLM (80<->132) preserves page memory; when reset
    // (the default), a column-width change clears the screen (VT100 behaviour).
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(80) } };
    mc.writeToScreen("\033[?40h"); // allow the 80<->132 switch

    SECTION("default (reset): DECCOLM clears the screen")
    {
        mc.writeToScreen("HELLO");
        mc.writeToScreen("\033[?3h"); // DECCOLM -> 132
        CHECK(!mc.terminal.primaryScreen().grid().lineText(LineOffset(0)).contains('H'));
    }

    SECTION("DECNCSM set: DECCOLM preserves the screen")
    {
        mc.writeToScreen("\033[?95h"); // DECNCSM
        mc.writeToScreen("HELLO");
        mc.writeToScreen("\033[?3h"); // DECCOLM -> 132
        CHECK(mc.terminal.primaryScreen().grid().lineText(LineOffset(0)).substr(0, 5) == "HELLO");
    }
}

TEST_CASE("Terminal.SynchronizedOutput", "[terminal]")
{
    constexpr auto BatchOn = "\033[?2026h"sv;
    constexpr auto BatchOff = "\033[?2026l"sv;

    auto const now = chrono::steady_clock::now();
    auto mc = MockTerm { ColumnCount(20), LineCount(1) };

    mc.writeToScreen(BatchOn);
    mc.writeToScreen("Hello ");
    mc.terminal.tick(now);
    mc.terminal.ensureFreshRenderBuffer();
    CHECK(trimmedTextScreenshot(mc).empty());

    mc.writeToScreen(" World");
    mc.terminal.tick(now);
    mc.terminal.ensureFreshRenderBuffer();
    CHECK(trimmedTextScreenshot(mc).empty());

    mc.writeToScreen(BatchOff);
    mc.terminal.tick(now);
    mc.terminal.ensureFreshRenderBuffer();
    CHECK("Hello  World" == trimmedTextScreenshot(mc));
}

TEST_CASE("Terminal.XTPUSHCOLORS_and_XTPOPCOLORS", "[terminal]")
{
    using namespace vtbackend;

    auto mc = MockTerm { ColumnCount(20), LineCount(1) };
    auto& vt = mc.terminal;

    auto const originalPalette = vt.colorPalette();

    auto modifiedPalette = ColorPalette {};
    modifiedPalette.palette[0] = 0xFF6600_rgb;

    SECTION("pop on empty")
    {
        mc.writeToScreen("\033[#Q");
        REQUIRE(vt.savedColorPalettes().empty());
        REQUIRE(vt.colorPalette().palette == originalPalette.palette);
    }

    SECTION("default")
    {
        mc.writeToScreen("\033[#P"); // XTPUSHCOLORS (default)
        REQUIRE(vt.savedColorPalettes().size() == 1);
        REQUIRE(vt.savedColorPalettes().back().palette == originalPalette.palette);
        vt.colorPalette().palette[0] = 0x123456_rgb;
        REQUIRE(vt.colorPalette().palette != originalPalette.palette);
        mc.writeToScreen("\033[#Q"); // XTPOPCOLORS
        REQUIRE(vt.colorPalette().palette == originalPalette.palette);
    }

    SECTION("0")
    {
        mc.writeToScreen("\033[0#P"); // push current color palette to slot 1 (default).
        REQUIRE(vt.savedColorPalettes().size() == 1);
    }

    SECTION("1")
    {
        REQUIRE(vt.savedColorPalettes().empty());
        mc.writeToScreen("\033[1#P"); // push current color palette to slot 1.
        REQUIRE(vt.savedColorPalettes().size() == 1);
    }

    SECTION("2")
    {
        REQUIRE(vt.savedColorPalettes().empty());
        mc.writeToScreen("\033[2#P"); // push current color palette to slot 1.
        REQUIRE(vt.savedColorPalettes().size() == 2);
        mc.writeToScreen("\033[#R");
        mc.terminal.flushInput();
        REQUIRE(e("\033[2;2#Q") == e(mc.replyData()));
    }

    SECTION("10")
    {
        REQUIRE(vt.savedColorPalettes().empty());
        mc.writeToScreen("\033[10#P"); // push current color palette to slot 10.
        REQUIRE(vt.savedColorPalettes().size() == 10);
        mc.writeToScreen("\033[#R");
        mc.terminal.flushInput();
        REQUIRE(e("\033[10;10#Q") == e(mc.replyData()));
    }

    SECTION("11")
    {
        REQUIRE(vt.savedColorPalettes().empty());
        mc.writeToScreen("\033[11#P"); // push current color palette to slot 11: overflow.
        REQUIRE(vt.savedColorPalettes().empty());
    }

    SECTION("push and direct copy")
    {
        vt.colorPalette().palette[1] = 0x101010_rgb;
        auto const p1 = vt.colorPalette();
        mc.writeToScreen("\033[#P");

        vt.colorPalette().palette[3] = 0x303030_rgb;
        auto const p3 = vt.colorPalette();
        mc.writeToScreen("\033[3#P");

        vt.colorPalette().palette[2] = 0x202020_rgb;
        auto const p2 = vt.colorPalette();
        mc.writeToScreen("\033[2#P");

        REQUIRE(vt.savedColorPalettes().size() == 3);
        REQUIRE(vt.colorPalette().palette == vt.savedColorPalettes()[2 - 1].palette);

        mc.writeToScreen("\033[1#Q"); // XTPOPCOLORS
        REQUIRE(vt.savedColorPalettes().size() == 3);
        REQUIRE(vt.colorPalette().palette == vt.savedColorPalettes()[1 - 1].palette);

        mc.writeToScreen("\033[2#Q"); // XTPOPCOLORS
        REQUIRE(vt.savedColorPalettes().size() == 3);
        REQUIRE(vt.colorPalette().palette == vt.savedColorPalettes()[2 - 1].palette);

        mc.writeToScreen("\033[3#Q"); // XTPOPCOLORS
        REQUIRE(vt.savedColorPalettes().size() == 3);
        REQUIRE(vt.colorPalette().palette == vt.savedColorPalettes()[3 - 1].palette);

        mc.writeToScreen("\033[#Q"); // XTPOPCOLORS
        REQUIRE(vt.savedColorPalettes().size() == 2);
        REQUIRE(vt.colorPalette().palette == p3.palette);

        mc.writeToScreen("\033[#Q"); // XTPOPCOLORS
        REQUIRE(vt.savedColorPalettes().size() == 1);
        REQUIRE(vt.colorPalette().palette == p2.palette);

        mc.writeToScreen("\033[#Q"); // XTPOPCOLORS
        REQUIRE(vt.savedColorPalettes().empty());
        REQUIRE(vt.colorPalette().palette == p1.palette);

        mc.writeToScreen("\033[#Q"); // XTPOPCOLORS (underflow)
        REQUIRE(vt.savedColorPalettes().empty());
        REQUIRE(vt.colorPalette().palette == p1.palette);
    }
}

TEST_CASE("Terminal.DECAC", "[terminal]")
{
    using namespace vtbackend;

    auto mc = MockTerm { ColumnCount(20), LineCount(1) };
    auto& vt = mc.terminal;

    auto const originalPalette = vt.colorPalette();

    SECTION("item 1: normal text sets default fg/bg from palette indices")
    {
        // CSI 1 ; 7 ; 4 , |  -> default fg = palette[7], default bg = palette[4].
        mc.writeToScreen("\033[1;7;4,|");
        REQUIRE(vt.colorPalette().defaultForeground == originalPalette.indexedColor(7));
        REQUIRE(vt.colorPalette().defaultBackground == originalPalette.indexedColor(4));
    }

    SECTION("item 1: bare form resets default fg/bg")
    {
        mc.writeToScreen("\033[1;2;5,|"); // set something first
        REQUIRE(vt.colorPalette().defaultForeground == originalPalette.indexedColor(2));
        mc.writeToScreen("\033[1,|"); // bare item -> reset
        REQUIRE(vt.colorPalette().defaultForeground == vt.defaultColorPalette().defaultForeground);
        REQUIRE(vt.colorPalette().defaultBackground == vt.defaultColorPalette().defaultBackground);
    }

    SECTION("item 2: window frame fires setWindowFrameColor with the background index")
    {
        REQUIRE(mc.windowFrameColorChangeCount == 0);
        REQUIRE(!mc.windowFrameColor.has_value());
        // CSI 2 ; 15 ; 1 , |  -> frame/tab color = palette[1] (the background index).
        mc.writeToScreen("\033[2;15;1,|");
        REQUIRE(mc.windowFrameColorChangeCount == 1);
        REQUIRE(mc.windowFrameColor == originalPalette.indexedColor(1));
    }

    SECTION("item 2: bare form resets the window frame color")
    {
        mc.writeToScreen("\033[2;15;1,|");
        REQUIRE(mc.windowFrameColor.has_value());
        mc.writeToScreen("\033[2,|"); // bare item -> reset
        REQUIRE(mc.windowFrameColorChangeCount == 2);
        REQUIRE(!mc.windowFrameColor.has_value());
    }

    SECTION("item 2: hard reset (RIS) clears the window frame color")
    {
        mc.writeToScreen("\033[2;15;1,|");
        REQUIRE(mc.windowFrameColor.has_value());
        mc.writeToScreen("\033c"); // RIS
        REQUIRE(!mc.windowFrameColor.has_value());
    }

    SECTION("invalid item is rejected without touching state")
    {
        mc.writeToScreen("\033[3;7;4,|"); // item 3 does not exist
        REQUIRE(vt.colorPalette().defaultForeground == originalPalette.defaultForeground);
        REQUIRE(vt.colorPalette().defaultBackground == originalPalette.defaultBackground);
        REQUIRE(mc.windowFrameColorChangeCount == 0);
    }

    SECTION("out-of-range palette index is rejected without touching state")
    {
        mc.writeToScreen("\033[1;300;4,|"); // fg index > 255
        REQUIRE(vt.colorPalette().defaultForeground == originalPalette.defaultForeground);
        mc.writeToScreen("\033[1;4;300,|"); // bg index > 255
        REQUIRE(vt.colorPalette().defaultBackground == originalPalette.defaultBackground);
    }

    SECTION("item 2 validates the foreground index too, even though it consumes only the background")
    {
        // Deliberate strictness: the DEC window-frame item carries a foreground of its own (Windows
        // Terminal keeps a FrameForeground alias for it), and Contour's tab strip simply derives its
        // label color by contrast instead of using it. An out-of-range index is therefore a malformed
        // sequence, not a parameter to quietly ignore.
        mc.writeToScreen("\033[2;300;5,|");
        REQUIRE(mc.windowFrameColorChangeCount == 0);
        REQUIRE(!mc.windowFrameColor.has_value());
    }

    SECTION("a two-parameter form is malformed and neither sets nor resets")
    {
        // First set a frame color, then send the ambiguous 2-param form: it must be rejected, leaving
        // the previously-set color intact (NOT reset).
        mc.writeToScreen("\033[2;15;1,|");
        REQUIRE(mc.windowFrameColor == originalPalette.indexedColor(1));
        auto const changesBefore = mc.windowFrameColorChangeCount;
        mc.writeToScreen("\033[2;5,|");                           // item + one color: malformed
        REQUIRE(mc.windowFrameColorChangeCount == changesBefore); // no set, no reset
        REQUIRE(mc.windowFrameColor == originalPalette.indexedColor(1));
    }
}

TEST_CASE("Terminal.DECATC", "[terminal]")
{
    using namespace vtbackend;

    auto mc = MockTerm { ColumnCount(20), LineCount(1) };
    auto& vt = mc.terminal;

    // DECATC colors only take effect in DECSTGLT "Alternate color" mode; enter it so the resolver
    // applies the overrides. (A dedicated section below verifies the default AnsiSgr mode ignores them.)
    mc.writeToScreen("\033[1){"); // DECSTGLT 1 = alternate color
    REQUIRE(vt.colorPalette().colorLookupTable == ColorLookupTable::Alternate);

    // Resolve a cell's colors the way the renderer does, for a given attribute combination, optional
    // screen-wide reverse-video (DECSCNM) state, and optional SGR colors on the cell itself.
    auto resolve = [&](CellFlags flags,
                       bool reverseVideo = false,
                       Color foreground = defaultColor(),
                       Color background = defaultColor()) {
        return CellUtil::makeColors(vt.colorPalette(),
                                    vt.colorPalette().colorLookupTable,
                                    flags,
                                    reverseVideo,
                                    foreground,
                                    background,
                                    /*blinkingState*/ 1.0f,
                                    /*rapidBlinkState*/ 1.0f);
    };

    SECTION("attribute 0 (normal text) overrides plain, unattributed cells")
    {
        // CSI 0 ; 2 ; 5 , }  -> plain text: fg = palette[2], bg = palette[5].
        mc.writeToScreen("\033[0;2;5,}");
        REQUIRE(vt.colorPalette().alternateTextColors[0].has_value());
        auto const plain = resolve(CellFlags {});
        REQUIRE(plain.foreground == vt.colorPalette().indexedColor(2));
        REQUIRE(plain.background == vt.colorPalette().indexedColor(5));
        // A cell WITH an attribute is a different combination, so the attribute-0 entry does not apply;
        // having no entry of its own, it falls back to the default text colors.
        auto const bold = resolve(CellFlags { CellFlag::Bold });
        REQUIRE(bold.foreground == vt.colorPalette().defaultForeground);
        REQUIRE(bold.background == vt.colorPalette().defaultBackground);
    }

    SECTION("reverse bit tracks visual state under DECSCNM (SGR 7 XOR screen reverse)")
    {
        mc.writeToScreen("\033[2;7;1,}"); // attribute 2 = reverse
        // Under DECSCNM, a plain cell is visually reversed, so the reverse override fires.
        auto const plainReversed = resolve(CellFlags {}, /*reverseVideo*/ true);
        REQUIRE(plainReversed.foreground == vt.colorPalette().indexedColor(7));
        REQUIRE(plainReversed.background == vt.colorPalette().indexedColor(1));
        // A cell WITH SGR 7 under DECSCNM is visually NON-reversed, so the override does NOT fire.
        auto const doublyReversed = resolve(CellFlags { CellFlag::Inverse }, /*reverseVideo*/ true);
        REQUIRE(doublyReversed.foreground != vt.colorPalette().indexedColor(7));
    }

    SECTION("a two-parameter form is malformed and does not clear an existing entry")
    {
        mc.writeToScreen("\033[1;7;4,}");
        REQUIRE(vt.colorPalette().alternateTextColors[1].has_value());
        mc.writeToScreen("\033[1;5,}");                                // attribute + one color: malformed
        REQUIRE(vt.colorPalette().alternateTextColors[1].has_value()); // still set
        REQUIRE(vt.colorPalette().alternateTextColors[1]->foreground == vt.colorPalette().indexedColor(7));
    }

    SECTION("assigns colors to the bold attribute combination")
    {
        // CSI 1 ; 7 ; 4 , }  -> bold text: fg = palette[7], bg = palette[4].
        mc.writeToScreen("\033[1;7;4,}");
        auto const& stored = vt.colorPalette().alternateTextColors[1];
        REQUIRE(stored.has_value());
        REQUIRE(stored->foreground == vt.colorPalette().indexedColor(7));
        REQUIRE(stored->background == vt.colorPalette().indexedColor(4));

        // A bold cell renders with the DECATC colors...
        auto const bold = resolve(CellFlags { CellFlag::Bold });
        REQUIRE(bold.foreground == vt.colorPalette().indexedColor(7));
        REQUIRE(bold.background == vt.colorPalette().indexedColor(4));

        // ...but a plain (non-bold) cell is unaffected.
        auto const plain = resolve(CellFlags {});
        REQUIRE(plain.foreground != vt.colorPalette().indexedColor(7));
    }

    SECTION("underline matches all underline variants")
    {
        mc.writeToScreen("\033[3;2;1,}"); // attribute 3 = Underline (DEC enumerated index)
        for (auto const flag: { CellFlag::Underline,
                                CellFlag::DoublyUnderlined,
                                CellFlag::CurlyUnderlined,
                                CellFlag::DottedUnderline,
                                CellFlag::DashedUnderline })
        {
            auto const c = resolve(CellFlags { flag });
            REQUIRE(c.foreground == vt.colorPalette().indexedColor(2));
            REQUIRE(c.background == vt.colorPalette().indexedColor(1));
        }
    }

    SECTION("blink matches both blink speeds")
    {
        mc.writeToScreen("\033[4;3;5,}"); // attribute 4 = Blink (DEC enumerated index)
        // blinkingState=1.0 so the mix returns the (overridden) full color.
        auto const slow = resolve(CellFlags { CellFlag::Blinking });
        REQUIRE(slow.foreground == vt.colorPalette().indexedColor(3));
        auto const rapid = resolve(CellFlags { CellFlag::RapidBlinking });
        REQUIRE(rapid.foreground == vt.colorPalette().indexedColor(3));
    }

    SECTION("reverse-video override is the final appearance (not swapped again)")
    {
        // attribute 2 = reverse. The override must be the final fg/bg of the reversed cell, i.e.
        // applied AFTER the inverse swap, so fg stays palette[7] and bg stays palette[1].
        mc.writeToScreen("\033[2;7;1,}");
        auto const c = resolve(CellFlags { CellFlag::Inverse });
        REQUIRE(c.foreground == vt.colorPalette().indexedColor(7));
        REQUIRE(c.background == vt.colorPalette().indexedColor(1));
    }

    SECTION("attribute indices follow the DEC enumerated table, not a bitmask")
    {
        // Per VT525 §5-22 the Ps1 index is an enumerated combination, NOT an OR of bits:
        // 3 = Underline (a bitmask would read 3 as Bold+Reverse), 6 = Bold underline (not 1|4=5).
        mc.writeToScreen("\033[3;2;1,}"); // attribute 3 = Underline
        auto const underline = resolve(CellFlags { CellFlag::Underline });
        REQUIRE(underline.foreground == vt.colorPalette().indexedColor(2));

        mc.writeToScreen("\033[6;10;11,}"); // attribute 6 = Bold underline
        auto const boldUnderline = resolve(CellFlags { CellFlag::Bold } | CellFlag::Underline);
        REQUIRE(boldUnderline.foreground == vt.colorPalette().indexedColor(10));
        REQUIRE(boldUnderline.background == vt.colorPalette().indexedColor(11));

        // Bold alone (index 1) is a different, here-unset entry, so it is NOT affected by either.
        auto const boldOnly = resolve(CellFlags { CellFlag::Bold });
        REQUIRE(boldOnly.foreground != vt.colorPalette().indexedColor(2));
        REQUIRE(boldOnly.foreground != vt.colorPalette().indexedColor(10));
    }

    SECTION("in the default ANSI SGR mode DECATC overrides are ignored")
    {
        mc.writeToScreen("\033[3){");     // DECSTGLT 3 = ANSI SGR color (the power-up default)
        mc.writeToScreen("\033[1;7;4,}"); // assign bold colors — stored, but must not render
        REQUIRE(vt.colorPalette().alternateTextColors[1].has_value());
        auto const bold = resolve(CellFlags { CellFlag::Bold });
        REQUIRE(bold.foreground != vt.colorPalette().indexedColor(7));
        REQUIRE(bold.background != vt.colorPalette().indexedColor(4));
    }

    SECTION("bare form resets an assigned combination")
    {
        mc.writeToScreen("\033[1;7;4,}");
        REQUIRE(vt.colorPalette().alternateTextColors[1].has_value());
        mc.writeToScreen("\033[1,}"); // bare attribute -> reset
        REQUIRE_FALSE(vt.colorPalette().alternateTextColors[1].has_value());
    }

    SECTION("attribute 0 can be reset individually")
    {
        // Regression: a lone '0' parameter collapses to "no parameters" under the VT convention, so
        // `CSI 0 , }` reaches the handler with an empty parameter list. Registering DECATC with a
        // minimum of one parameter dropped the sequence entirely, leaving the normal-text entry the one
        // combination that could be assigned but never individually cleared.
        mc.writeToScreen("\033[0;2;5,}");
        REQUIRE(vt.colorPalette().alternateTextColors[0].has_value());
        mc.writeToScreen("\033[0,}"); // bare attribute 0 -> reset
        REQUIRE_FALSE(vt.colorPalette().alternateTextColors[0].has_value());

        // The wholly parameterless form defaults the attribute to 0 and means the same thing.
        mc.writeToScreen("\033[0;2;5,}");
        REQUIRE(vt.colorPalette().alternateTextColors[0].has_value());
        mc.writeToScreen("\033[,}");
        REQUIRE_FALSE(vt.colorPalette().alternateTextColors[0].has_value());

        // Resetting entry 0 must not disturb any other entry.
        mc.writeToScreen("\033[1;7;4,}");
        mc.writeToScreen("\033[0;2;5,}");
        mc.writeToScreen("\033[0,}");
        REQUIRE_FALSE(vt.colorPalette().alternateTextColors[0].has_value());
        REQUIRE(vt.colorPalette().alternateTextColors[1].has_value());
    }

    SECTION("an unassigned combination uses the default text colors, not the cell's SGR colors")
    {
        // In Alternate mode the ANSI SGR color parameters are ignored *entirely*: a combination the
        // application never assigned renders in the default foreground/background, exactly as if the
        // application had assigned those. (xterm expresses the same rule by seeding all sixteen entries
        // from the default pair.) Anything else would let SGR colors leak into a mode whose whole point
        // is that attribute combinations, and only those, choose the color.
        auto const& palette = vt.colorPalette();
        auto const red = IndexedColor::Red;
        auto const blue = IndexedColor::Blue;

        auto const unassigned = resolve(CellFlags { CellFlag::Bold }, false, red, blue);
        REQUIRE(unassigned.foreground == palette.defaultForeground);
        REQUIRE(unassigned.background == palette.defaultBackground);

        // Assigning that combination makes it, and only it, follow the assignment.
        mc.writeToScreen("\033[1;7;4,}"); // attribute 1 = bold
        auto const assigned = resolve(CellFlags { CellFlag::Bold }, false, red, blue);
        REQUIRE(assigned.foreground == palette.indexedColor(7));
        REQUIRE(assigned.background == palette.indexedColor(4));

        // Clearing it again returns the cell to the default colors, never to its SGR colors.
        mc.writeToScreen("\033[1,}");
        auto const cleared = resolve(CellFlags { CellFlag::Bold }, false, red, blue);
        REQUIRE(cleared.foreground == palette.defaultForeground);
        REQUIRE(cleared.background == palette.defaultBackground);
    }

    SECTION("out-of-range attribute is rejected")
    {
        mc.writeToScreen("\033[16;7;4,}"); // attribute 16 is out of the 0..15 range
        for (auto const& entry: vt.colorPalette().alternateTextColors)
            REQUIRE_FALSE(entry.has_value());
    }

    SECTION("out-of-range palette index is rejected without touching the entry")
    {
        mc.writeToScreen("\033[1;7;4,}"); // set something first
        REQUIRE(vt.colorPalette().alternateTextColors[1].has_value());
        mc.writeToScreen("\033[1;300;4,}"); // fg index > 255: rejected, entry NOT cleared
        REQUIRE(vt.colorPalette().alternateTextColors[1]->foreground == vt.colorPalette().indexedColor(7));
        mc.writeToScreen("\033[1;7;300,}"); // bg index > 255: likewise
        REQUIRE(vt.colorPalette().alternateTextColors[1]->background == vt.colorPalette().indexedColor(4));
    }

    SECTION("hard reset (RIS) clears all assignments")
    {
        mc.writeToScreen("\033[1;7;4,}");
        mc.writeToScreen("\033[8;3;5,}");
        mc.writeToScreen("\033c"); // RIS
        for (auto const& entry: vt.colorPalette().alternateTextColors)
            REQUIRE_FALSE(entry.has_value());
    }

    SECTION("soft reset (DECSTR) also clears assignments")
    {
        mc.writeToScreen("\033[1;7;4,}");
        mc.writeToScreen("\033[!p"); // DECSTR
        for (auto const& entry: vt.colorPalette().alternateTextColors)
            REQUIRE_FALSE(entry.has_value());
    }
}

TEST_CASE("Terminal.DECSTGLT", "[terminal]")
{
    using namespace vtbackend;

    auto mc = MockTerm { ColumnCount(20), LineCount(1) };
    auto& vt = mc.terminal;

    // Power-up default is ANSI SGR color.
    REQUIRE(vt.colorPalette().colorLookupTable == ColorLookupTable::AnsiSgr);

    SECTION("parameters 1 and 2 select alternate color, 3 selects ANSI SGR")
    {
        mc.writeToScreen("\033[1){");
        CHECK(vt.colorPalette().colorLookupTable == ColorLookupTable::Alternate);
        mc.writeToScreen("\033[2){");
        CHECK(vt.colorPalette().colorLookupTable == ColorLookupTable::Alternate);
        mc.writeToScreen("\033[3){");
        CHECK(vt.colorPalette().colorLookupTable == ColorLookupTable::AnsiSgr);
    }

    SECTION("an omitted or lone-zero parameter selects the default ANSI SGR color table")
    {
        // The parser collapses a lone 0 to "no parameters" (VT convention), so CSI 0 ) { and CSI ) {
        // are identical and both mean the default. This is why the spec's monochrome table, numbered 0,
        // is not modelled: it could never be selected.
        mc.writeToScreen("\033[1){"); // move away from the default first
        REQUIRE(vt.colorPalette().colorLookupTable == ColorLookupTable::Alternate);
        mc.writeToScreen("\033[){"); // no parameter -> default
        CHECK(vt.colorPalette().colorLookupTable == ColorLookupTable::AnsiSgr);
        mc.writeToScreen("\033[1){");
        mc.writeToScreen("\033[0){"); // lone zero -> also default
        CHECK(vt.colorPalette().colorLookupTable == ColorLookupTable::AnsiSgr);
    }

    SECTION("an out-of-range parameter is rejected and leaves the mode untouched")
    {
        mc.writeToScreen("\033[1){"); // alternate
        REQUIRE(vt.colorPalette().colorLookupTable == ColorLookupTable::Alternate);
        mc.writeToScreen("\033[4){"); // no such table
        CHECK(vt.colorPalette().colorLookupTable == ColorLookupTable::Alternate);
    }

    SECTION("hard reset (RIS) restores the default color look-up table")
    {
        mc.writeToScreen("\033[1){");
        REQUIRE(vt.colorPalette().colorLookupTable == ColorLookupTable::Alternate);
        mc.writeToScreen("\033c"); // RIS
        CHECK(vt.colorPalette().colorLookupTable == ColorLookupTable::AnsiSgr);
    }
}

TEST_CASE("Terminal.DECATC.doesNotRecolorTheStatusLine", "[terminal]")
{
    using namespace vtbackend;

    // The status line is host-owned chrome, painted from the colors the host configured for it. An
    // application that selects DECSTGLT Alternate mode and assigns DECATC colors recolors its own text,
    // never the terminal's furniture. Regression guard: colorLookupTable and the DECATC assignments live
    // on the terminal-global palette, which the status-line screens share with the main screen, so the
    // color mode has to be chosen per rendered screen rather than merely read off the palette.
    auto mc = MockTerm { PageSize { .lines = LineCount(4), .columns = ColumnCount(20) }, LineCount(0) };
    auto& vt = mc.terminal;
    auto constexpr ClockBase = chrono::steady_clock::time_point();
    vt.tick(ClockBase);
    vt.setStatusDisplay(StatusDisplayType::Indicator);

    // The indicator status line renders below the main page, at the first line offset past it.
    auto const statusLine = LineOffset::cast_from(unbox(vt.pageSize().lines));

    // Every rendered (foreground, background) pair on @p line, covering both the per-cell and the
    // batched trivial-line render paths.
    auto sample = [&](LineOffset line) {
        vt.refreshRenderBuffer();
        auto const buf = vt.renderBuffer();
        auto colors = std::vector<std::pair<RGBColor, RGBColor>> {};
        for (auto const& cell: buf.get().cells)
            if (cell.position.line == line)
                colors.emplace_back(cell.attributes.foregroundColor, cell.attributes.backgroundColor);
        for (auto const& renderLine: buf.get().lines)
            if (renderLine.lineOffset == line)
            {
                colors.emplace_back(renderLine.textAttributes.foregroundColor,
                                    renderLine.textAttributes.backgroundColor);
                colors.emplace_back(renderLine.fillAttributes.foregroundColor,
                                    renderLine.fillAttributes.backgroundColor);
            }
        return colors;
    };

    mc.writeToScreen("AB");
    auto const statusBefore = sample(statusLine);
    auto const pageBefore = sample(LineOffset(0));
    REQUIRE_FALSE(statusBefore.empty()); // the status line must actually be rendered, or this proves nothing
    REQUIRE_FALSE(pageBefore.empty());

    mc.writeToScreen("\033[1){");     // DECSTGLT 1 = alternate color
    mc.writeToScreen("\033[0;2;5,}"); // DECATC: normal text -> palette[2] on palette[5]

    // The application's own text takes the assigned colors ...
    CHECK(sample(LineOffset(0)) != pageBefore);

    // ... while the status line keeps the colors the host configured for it.
    CHECK(sample(statusLine) == statusBefore);
}

TEST_CASE("Terminal.UnderlineStyleClearing", "[terminal]")
{
    // Each subsequent underline style should clear the former if present.

    auto const now = chrono::steady_clock::now();
    auto mc = MockTerm { ColumnCount(20), LineCount(1) };

    mc.writeToScreen("\033[4:1mAB\033[21mCD\033[4:3mEF\033[24mGH\033[4:2mIJ\033[mKL");
    mc.terminal.tick(now);
    mc.terminal.ensureFreshRenderBuffer();
    CHECK("ABCDEFGHIJKL" == trimmedTextScreenshot(mc));

    auto& screen = mc.terminal.primaryScreen();

    CHECK(screen.at(LineOffset(0), ColumnOffset(0)).isFlagEnabled(CellFlag::Underline));
    CHECK(screen.at(LineOffset(0), ColumnOffset(1)).isFlagEnabled(CellFlag::Underline));
    CHECK(!screen.at(LineOffset(0), ColumnOffset(2)).isFlagEnabled(CellFlag::Underline));
    CHECK(!screen.at(LineOffset(0), ColumnOffset(3)).isFlagEnabled(CellFlag::Underline));
    CHECK(!screen.at(LineOffset(0), ColumnOffset(4)).isFlagEnabled(CellFlag::Underline));
    CHECK(!screen.at(LineOffset(0), ColumnOffset(5)).isFlagEnabled(CellFlag::Underline));
    CHECK(!screen.at(LineOffset(0), ColumnOffset(6)).isFlagEnabled(CellFlag::Underline));
    CHECK(!screen.at(LineOffset(0), ColumnOffset(7)).isFlagEnabled(CellFlag::Underline));
    CHECK(!screen.at(LineOffset(0), ColumnOffset(8)).isFlagEnabled(CellFlag::Underline));
    CHECK(!screen.at(LineOffset(0), ColumnOffset(9)).isFlagEnabled(CellFlag::Underline));
    CHECK(!screen.at(LineOffset(0), ColumnOffset(10)).isFlagEnabled(CellFlag::Underline));
    CHECK(!screen.at(LineOffset(0), ColumnOffset(11)).isFlagEnabled(CellFlag::Underline));

    CHECK(!screen.at(LineOffset(0), ColumnOffset(0)).isFlagEnabled(CellFlag::DoublyUnderlined));
    CHECK(!screen.at(LineOffset(0), ColumnOffset(1)).isFlagEnabled(CellFlag::DoublyUnderlined));
    CHECK(screen.at(LineOffset(0), ColumnOffset(2)).isFlagEnabled(CellFlag::DoublyUnderlined));
    CHECK(screen.at(LineOffset(0), ColumnOffset(3)).isFlagEnabled(CellFlag::DoublyUnderlined));
    CHECK(!screen.at(LineOffset(0), ColumnOffset(4)).isFlagEnabled(CellFlag::DoublyUnderlined));
    CHECK(!screen.at(LineOffset(0), ColumnOffset(5)).isFlagEnabled(CellFlag::DoublyUnderlined));
    CHECK(!screen.at(LineOffset(0), ColumnOffset(6)).isFlagEnabled(CellFlag::DoublyUnderlined));
    CHECK(!screen.at(LineOffset(0), ColumnOffset(7)).isFlagEnabled(CellFlag::DoublyUnderlined));
    CHECK(screen.at(LineOffset(0), ColumnOffset(8)).isFlagEnabled(CellFlag::DoublyUnderlined));
    CHECK(screen.at(LineOffset(0), ColumnOffset(9)).isFlagEnabled(CellFlag::DoublyUnderlined));
    CHECK(!screen.at(LineOffset(0), ColumnOffset(10)).isFlagEnabled(CellFlag::DoublyUnderlined));
    CHECK(!screen.at(LineOffset(0), ColumnOffset(11)).isFlagEnabled(CellFlag::DoublyUnderlined));

    CHECK(!screen.at(LineOffset(0), ColumnOffset(0)).isFlagEnabled(CellFlag::CurlyUnderlined));
    CHECK(!screen.at(LineOffset(0), ColumnOffset(1)).isFlagEnabled(CellFlag::CurlyUnderlined));
    CHECK(!screen.at(LineOffset(0), ColumnOffset(2)).isFlagEnabled(CellFlag::CurlyUnderlined));
    CHECK(!screen.at(LineOffset(0), ColumnOffset(3)).isFlagEnabled(CellFlag::CurlyUnderlined));
    CHECK(screen.at(LineOffset(0), ColumnOffset(4)).isFlagEnabled(CellFlag::CurlyUnderlined));
    CHECK(screen.at(LineOffset(0), ColumnOffset(5)).isFlagEnabled(CellFlag::CurlyUnderlined));
    CHECK(!screen.at(LineOffset(0), ColumnOffset(6)).isFlagEnabled(CellFlag::CurlyUnderlined));
    CHECK(!screen.at(LineOffset(0), ColumnOffset(7)).isFlagEnabled(CellFlag::CurlyUnderlined));
    CHECK(!screen.at(LineOffset(0), ColumnOffset(8)).isFlagEnabled(CellFlag::CurlyUnderlined));
    CHECK(!screen.at(LineOffset(0), ColumnOffset(9)).isFlagEnabled(CellFlag::CurlyUnderlined));
    CHECK(!screen.at(LineOffset(0), ColumnOffset(10)).isFlagEnabled(CellFlag::CurlyUnderlined));
    CHECK(!screen.at(LineOffset(0), ColumnOffset(11)).isFlagEnabled(CellFlag::CurlyUnderlined));

    CHECK(!screen.at(LineOffset(0), ColumnOffset(0)).isFlagEnabled(CellFlag::Italic));
    CHECK(!screen.at(LineOffset(0), ColumnOffset(1)).isFlagEnabled(CellFlag::Italic));
    CHECK(!screen.at(LineOffset(0), ColumnOffset(2)).isFlagEnabled(CellFlag::Italic));
    CHECK(!screen.at(LineOffset(0), ColumnOffset(3)).isFlagEnabled(CellFlag::Italic));
    CHECK(!screen.at(LineOffset(0), ColumnOffset(4)).isFlagEnabled(CellFlag::Italic));
    CHECK(!screen.at(LineOffset(0), ColumnOffset(5)).isFlagEnabled(CellFlag::Italic));
    CHECK(!screen.at(LineOffset(0), ColumnOffset(6)).isFlagEnabled(CellFlag::Italic));
    CHECK(!screen.at(LineOffset(0), ColumnOffset(7)).isFlagEnabled(CellFlag::Italic));
    CHECK(!screen.at(LineOffset(0), ColumnOffset(8)).isFlagEnabled(CellFlag::Italic));
    CHECK(!screen.at(LineOffset(0), ColumnOffset(9)).isFlagEnabled(CellFlag::Italic));
    CHECK(!screen.at(LineOffset(0), ColumnOffset(10)).isFlagEnabled(CellFlag::Italic));
    CHECK(!screen.at(LineOffset(0), ColumnOffset(11)).isFlagEnabled(CellFlag::Italic));
}

TEST_CASE("Terminal.a_one_column_selection_still_breaks_lines", "[terminal]")
{
    // The line-break test asks "has anything been written yet", and used the accumulated text to
    // answer -- the same proxy flushLine was fixed away from, in the other of the two places that
    // used it. A one-column selection emits every callback at the same column, so the only thing that
    // could trigger a flush is that proxy, and it stays false because text only becomes non-empty
    // inside the flush it is gating.
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(3), ColumnCount(4) } };
    mock.writeToScreen("a\r\nb\r\nc"sv);

    // Rectangular: every callback lands on the same column, which is what leaves the line-break test
    // with nothing else to go on.
    mock.terminal.setSelector(std::make_unique<vtbackend::RectangularSelection>(
        mock.terminal.selectionHelper(),
        CellLocation { .line = LineOffset(0), .column = ColumnOffset(0) },
        []() {}));
    (void) mock.terminal.selector()->extend(
        CellLocation { .line = LineOffset(2), .column = ColumnOffset(0) });
    mock.terminal.selector()->complete();

    CHECK(mock.terminal.extractSelectionText() == "a\nb\nc");
}

TEST_CASE("Terminal.CurlyUnderline", "[terminal]")
{
    auto const now = chrono::steady_clock::now();
    auto mc = MockTerm { ColumnCount(20), LineCount(1) };

    mc.writeToScreen("\033[4:3mAB\033[mCD");
    mc.terminal.tick(now);
    mc.terminal.ensureFreshRenderBuffer();
    CHECK("ABCD" == trimmedTextScreenshot(mc));

    auto& screen = mc.terminal.primaryScreen();

    CHECK(screen.at(LineOffset(0), ColumnOffset(0)).isFlagEnabled(CellFlag::CurlyUnderlined));
    CHECK(screen.at(LineOffset(0), ColumnOffset(1)).isFlagEnabled(CellFlag::CurlyUnderlined));
    CHECK(!screen.at(LineOffset(0), ColumnOffset(2)).isFlagEnabled(CellFlag::CurlyUnderlined));
    CHECK(!screen.at(LineOffset(0), ColumnOffset(3)).isFlagEnabled(CellFlag::CurlyUnderlined));

    CHECK(!screen.at(LineOffset(0), ColumnOffset(0)).isFlagEnabled(CellFlag::Italic));
    CHECK(!screen.at(LineOffset(0), ColumnOffset(1)).isFlagEnabled(CellFlag::Italic));
    CHECK(!screen.at(LineOffset(0), ColumnOffset(2)).isFlagEnabled(CellFlag::Italic));
    CHECK(!screen.at(LineOffset(0), ColumnOffset(3)).isFlagEnabled(CellFlag::Italic));
}

TEST_CASE("Terminal.ParsingBuffer", "[terminal]")
{
    // Test that parsingBuffer() returns the correct buffer during parsing.
    // When _parsingBuffer is not set, it should fall back to currentPtyBuffer().

    auto mock = MockTerm { ColumnCount { 10 }, LineCount { 3 } };
    auto& terminal = mock.terminal;

    // Initially, parsingBuffer() should return currentPtyBuffer() since _parsingBuffer is not set
    CHECK(terminal.parsingBuffer() == terminal.currentPtyBuffer());

    // Write some text - this will exercise the parsing path
    mock.writeToScreen("Hello");

    // After parsing completes, parsingBuffer() should still return currentPtyBuffer()
    // because _parsingBuffer is reset after each parse
    CHECK(terminal.parsingBuffer() == terminal.currentPtyBuffer());
}

TEST_CASE("Terminal.TrivialLineBufferIntegrity", "[terminal]")
{
    // Test that TrivialLineBuffer correctly stores text when written through terminal.
    // This tests the fast path where text is stored directly in a BufferFragment.

    auto mock = MockTerm { ColumnCount { 20 }, LineCount { 3 } };
    auto& terminal = mock.terminal;
    auto constexpr ClockBase = chrono::steady_clock::time_point();
    terminal.tick(ClockBase);

    // Write a simple ASCII string that should use the TrivialLineBuffer fast path
    mock.writeToScreen("ABCDEFGHIJ");

    terminal.tick(ClockBase + chrono::seconds(1));
    terminal.ensureFreshRenderBuffer();

    // Verify the text was stored correctly
    auto const& line = terminal.primaryScreen().currentLine();

    // With SoA storage, all lines use LineSoA. Verify via toUtf8.
    CHECK(line.toUtf8().substr(0, 10) == "ABCDEFGHIJ");
}

TEST_CASE("Terminal.BoxDrawingCharacters", "[terminal]")
{
    // Test that box-drawing characters (3-byte UTF-8) are handled correctly.
    // This is a regression test for the corruption seen in `tree /` output.

    auto mock = MockTerm { ColumnCount { 20 }, LineCount { 5 } };
    auto& terminal = mock.terminal;
    auto constexpr ClockBase = chrono::steady_clock::time_point();
    terminal.tick(ClockBase);

    // Write a line with box-drawing characters similar to tree output
    // "│── file" using box drawing chars
    mock.writeToScreen("\xE2\x94\x82\xE2\x94\x80\xE2\x94\x80 file\r\n");
    mock.writeToScreen("\xE2\x94\x9C\xE2\x94\x80\xE2\x94\x80 dir\r\n");

    terminal.tick(ClockBase + chrono::seconds(1));
    terminal.ensureFreshRenderBuffer();

    // Verify the text contains the expected content
    auto const line0 = terminal.primaryScreen().grid().lineAt(LineOffset(0)).toUtf8();
    auto const line1 = terminal.primaryScreen().grid().lineAt(LineOffset(1)).toUtf8();

    // Check that box-drawing characters are present (not corrupted to replacement chars)
    CHECK(line0.contains("\xE2\x94\x82")); // │
    CHECK(line1.contains("\xE2\x94\x9C")); // ├
    CHECK(line0.contains("file"));
    CHECK(line1.contains("dir"));
}

TEST_CASE("Terminal.cursorAnimationProgress.no_animation_returns_1", "[terminal]")
{
    auto mc = MockTerm { ColumnCount { 10 }, LineCount { 4 } };
    auto& terminal = mc.terminal;
    // With no animation, cursor at current position should return 1.0 (complete).
    auto const gridPos = terminal.currentScreen().cursor().position;
    CHECK(terminal.cursorAnimationProgress(gridPos) == 1.0f);
}

TEST_CASE("Terminal.onScreenScrolled.preserves_viewport_with_pixel_offset", "[terminal]")
{
    auto mc = MockTerm { PageSize { LineCount { 4 }, ColumnCount { 10 } }, LineCount { 10 } };
    auto& terminal = mc.terminal;
    auto constexpr ClockBase = chrono::steady_clock::time_point();
    terminal.tick(ClockBase);

    terminal.setCellPixelSize(vtbackend::ImageSize { vtpty::Width(10), vtpty::Height(20) });

    // Write enough lines to generate some history.
    for (auto i = 0; i < 8; ++i)
        mc.writeToScreen("line\r\n");

    // Scroll up and set a non-zero pixel offset.
    terminal.applySmoothScrollPixelDelta(5.0f);
    auto const offsetBefore = terminal.viewport().scrollOffset().value;

    // Write more content, triggering onScreenScrolled.
    for (auto i = 0; i < 4; ++i)
        mc.writeToScreen("more\r\n");

    // The viewport scroll offset should have increased to keep the view stable.
    CHECK(terminal.viewport().scrollOffset().value > offsetBefore);
}

TEST_CASE("Terminal.resizeScreen.minimal_one_by_one", "[terminal]")
{
    // Regression: when the render surface collapses below one cell (e.g. a pane shrunk to nothing,
    // or a transient layout state) the frontend clamps the page size to a minimum of 1x1 before
    // calling resizeScreen(). The backend must accept a 1x1 page without tripping the
    // clampToScreen() bounds assert (which fires when a page dimension reaches zero).
    auto mc = MockTerm { PageSize { LineCount { 10 }, ColumnCount { 20 } }, LineCount { 10 } };
    auto& terminal = mc.terminal;
    terminal.tick(chrono::steady_clock::time_point());

    REQUIRE_NOTHROW(terminal.resizeScreen(PageSize { LineCount { 1 }, ColumnCount { 1 } }));
    CHECK(terminal.pageSize().lines == LineCount { 1 });
    CHECK(terminal.pageSize().columns == ColumnCount { 1 });

    // And it can grow back from the degenerate size.
    REQUIRE_NOTHROW(terminal.resizeScreen(PageSize { LineCount { 10 }, ColumnCount { 20 } }));
    CHECK(terminal.pageSize().lines == LineCount { 10 });
}

TEST_CASE("Terminal.resizeScreen.minimal_one_by_one.with_status_line", "[terminal]")
{
    // Regression: the same degenerate 1x1 resize, but with the indicator status line VISIBLE — the
    // contour GUI default. resizeScreen derives the main page as `totalPageSize - statusLineHeight()`,
    // so a 1x1 total here would leave a ZERO-line main page (1 - 1) and trip
    // applyPageSizeToCurrentBuffer()/verifyState(). The plain test above misses this because MockTerm
    // defaults statusDisplayType to None (statusLineHeight() == 0). resizeScreen must clamp the total
    // up so at least one main-display line survives on top of the status line.
    auto mc = MockTerm { PageSize { LineCount { 10 }, ColumnCount { 20 } }, LineCount { 10 } };
    auto& terminal = mc.terminal;
    terminal.setStatusDisplay(vtbackend::StatusDisplayType::Indicator);
    terminal.tick(chrono::steady_clock::time_point());
    REQUIRE(terminal.statusLineHeight() == LineCount { 1 });

    REQUIRE_NOTHROW(terminal.resizeScreen(PageSize { LineCount { 1 }, ColumnCount { 1 } }));
    // The total was clamped up to leave one main line above the one status line; the main page
    // (what the PTY/shell sees) never collapses to zero.
    CHECK(terminal.pageSize().lines >= LineCount { 1 });
    CHECK(terminal.pageSize().columns == ColumnCount { 1 });
    CHECK(terminal.totalPageSize().lines >= LineCount { 2 });

    // And it can grow back from the degenerate size.
    REQUIRE_NOTHROW(terminal.resizeScreen(PageSize { LineCount { 10 }, ColumnCount { 20 } }));
    CHECK(terminal.pageSize().lines == LineCount { 9 }); // 10 total - 1 status line
}

TEST_CASE("Terminal.CancelSelection_no_selection", "[terminal]")
{
    // Reproducer for #1839: On a fresh terminal with no selection,
    // selectionAvailable() must return false so CancelSelection
    // does not consume the key event.
    auto mock = MockTerm { ColumnCount { 10 }, LineCount { 3 } };
    auto& terminal = mock.terminal;

    mock.writeToScreen("Hello World");
    terminal.ensureFreshRenderBuffer();

    CHECK_FALSE(terminal.selectionAvailable());

    // clearSelection() should be a safe no-op when no selection exists.
    terminal.clearSelection();
    CHECK_FALSE(terminal.selectionAvailable());
}

TEST_CASE("Terminal.CancelSelection_with_selection", "[terminal]")
{
    // Verify that creating a selection sets selectionAvailable() to true,
    // and clearSelection() resets it to false.
    auto mock = MockTerm { ColumnCount { 5 }, LineCount { 3 } };
    auto& terminal = mock.terminal;
    auto constexpr ClockBase = chrono::steady_clock::time_point();

    terminal.tick(ClockBase);
    terminal.ensureFreshRenderBuffer();
    mock.writeToScreen("Hello\r\nWorld\r\nTest!");
    terminal.tick(ClockBase + 1s);
    terminal.ensureFreshRenderBuffer();

    using namespace vtbackend;
    auto constexpr UiHandledHint = false;
    auto constexpr PixelCoordinate = vtbackend::PixelCoordinate {};

    // Initiate a mouse selection across lines to avoid division-by-zero
    // on cellPixelWidth in mock terminal (no real renderer).
    terminal.tick(ClockBase + 2s);
    terminal.sendMouseMoveEvent(
        Modifier::None, 0_lineOffset + 1_columnOffset, PixelCoordinate, UiHandledHint);
    terminal.tick(ClockBase + 3s);
    terminal.sendMousePressEvent(
        Modifier::None, MouseButton::Left, 0_lineOffset + 1_columnOffset, PixelCoordinate, UiHandledHint);
    terminal.tick(ClockBase + 4s);
    terminal.sendMouseMoveEvent(
        Modifier::None, 1_lineOffset + 2_columnOffset, PixelCoordinate, UiHandledHint);
    terminal.tick(ClockBase + 5s);
    terminal.sendMouseReleaseEvent(Modifier::None, MouseButton::Left, PixelCoordinate, UiHandledHint);

    REQUIRE(terminal.selectionAvailable());
    CHECK_FALSE(terminal.extractSelectionText().empty());

    // Now clear it, simulating what CancelSelection does.
    terminal.clearSelection();
    CHECK_FALSE(terminal.selectionAvailable());
}

TEST_CASE("Terminal.CancelSelection_double_clear", "[terminal]")
{
    // Edge case: calling clearSelection() twice must not crash or
    // have unexpected side effects.
    auto mock = MockTerm { ColumnCount { 5 }, LineCount { 3 } };
    auto& terminal = mock.terminal;
    auto constexpr ClockBase = chrono::steady_clock::time_point();

    terminal.tick(ClockBase);
    terminal.ensureFreshRenderBuffer();
    mock.writeToScreen("Hello\r\nWorld\r\nTest!");
    terminal.tick(ClockBase + 1s);
    terminal.ensureFreshRenderBuffer();

    using namespace vtbackend;
    auto constexpr UiHandledHint = false;
    auto constexpr PixelCoordinate = vtbackend::PixelCoordinate {};

    // Create a selection across lines.
    terminal.tick(ClockBase + 2s);
    terminal.sendMouseMoveEvent(
        Modifier::None, 0_lineOffset + 1_columnOffset, PixelCoordinate, UiHandledHint);
    terminal.tick(ClockBase + 3s);
    terminal.sendMousePressEvent(
        Modifier::None, MouseButton::Left, 0_lineOffset + 1_columnOffset, PixelCoordinate, UiHandledHint);
    terminal.tick(ClockBase + 4s);
    terminal.sendMouseMoveEvent(
        Modifier::None, 1_lineOffset + 2_columnOffset, PixelCoordinate, UiHandledHint);
    terminal.tick(ClockBase + 5s);
    terminal.sendMouseReleaseEvent(Modifier::None, MouseButton::Left, PixelCoordinate, UiHandledHint);

    REQUIRE(terminal.selectionAvailable());

    // First clear.
    terminal.clearSelection();
    CHECK_FALSE(terminal.selectionAvailable());

    // Second clear — must be a safe no-op.
    terminal.clearSelection();
    CHECK_FALSE(terminal.selectionAvailable());
}

TEST_CASE("Terminal.ScrollWhileSelecting", "[terminal]")
{
    // Create TE with scrollback history.
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(5) }, LineCount(10) };
    auto constexpr ClockBase = chrono::steady_clock::time_point();
    mock.terminal.tick(ClockBase);
    // Write enough lines to create history.
    mock.writeToScreen("AAAAA\r\n"
                       "BBBBB\r\n"
                       "CCCCC\r\n"
                       "DDDDD\r\n"
                       "EEEEE\r\n"
                       "FFFFF");
    // History: AAAAA, BBBBB, CCCCC. Main page: DDDDD, EEEEE, FFFFF

    using namespace vtbackend;
    auto constexpr UiHandledHint = false;
    auto constexpr PixelCoord = vtbackend::PixelCoordinate {};

    SECTION("wheel scroll up extends selection into history")
    {
        // Start a selection in the middle of the main page.
        mock.terminal.tick(1s);
        mock.terminal.sendMouseMoveEvent(
            Modifier::None, 1_lineOffset + 2_columnOffset, PixelCoord, UiHandledHint);
        mock.terminal.tick(2s);
        mock.terminal.sendMousePressEvent(
            Modifier::None, MouseButton::Left, 1_lineOffset + 2_columnOffset, PixelCoord, UiHandledHint);
        mock.terminal.tick(3s);
        mock.terminal.sendMouseMoveEvent(
            Modifier::None, 0_lineOffset + 0_columnOffset, PixelCoord, UiHandledHint);
        // Selection is InProgress.
        REQUIRE(mock.terminal.selectionAvailable());
        CHECK(mock.terminal.selector()->state() == Selection::State::InProgress);

        // Simulate wheel-scroll up (viewport change without mouse move).
        mock.terminal.viewport().scrollUp(LineCount(2));
        // extendSelectionAfterScroll is called from onViewportChanged.

        // The selection should have been extended to the new mouse position
        // relative to the scrolled viewport.
        CHECK(mock.terminal.viewport().scrollOffset() == ScrollOffset(2));
        // The selection endpoint should now be in history.
        auto const text = mock.terminal.extractSelectionText();
        CHECK_FALSE(text.empty());
    }

    SECTION("scroll without selection is no-op")
    {
        // No selection active, scroll should not crash or create a selection.
        CHECK_FALSE(mock.terminal.selectionAvailable());
        mock.terminal.viewport().scrollUp(LineCount(1));
        CHECK_FALSE(mock.terminal.selectionAvailable());
    }

    SECTION("scroll with completed selection is no-op")
    {
        // Create and complete a selection.
        mock.terminal.tick(1s);
        mock.terminal.sendMouseMoveEvent(
            Modifier::None, 0_lineOffset + 0_columnOffset, PixelCoord, UiHandledHint);
        mock.terminal.tick(2s);
        mock.terminal.sendMousePressEvent(
            Modifier::None, MouseButton::Left, 0_lineOffset + 0_columnOffset, PixelCoord, UiHandledHint);
        mock.terminal.tick(3s);
        mock.terminal.sendMouseMoveEvent(
            Modifier::None, 1_lineOffset + 2_columnOffset, PixelCoord, UiHandledHint);
        mock.terminal.tick(4s);
        mock.terminal.sendMouseReleaseEvent(Modifier::None, MouseButton::Left, PixelCoord, UiHandledHint);
        auto const textBefore = mock.terminal.extractSelectionText();
        CHECK(mock.terminal.selector()->state() == Selection::State::Complete);

        // Scroll should NOT extend the completed selection.
        mock.terminal.viewport().scrollUp(LineCount(1));
        CHECK(mock.terminal.extractSelectionText() == textBefore);
    }
}

TEST_CASE("Terminal.PerformAutoScroll", "[terminal]")
{
    // Create TE with scrollback history.
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(5) }, LineCount(10) };
    auto constexpr ClockBase = chrono::steady_clock::time_point();
    mock.terminal.tick(ClockBase);
    // Write enough lines to fill history.
    mock.writeToScreen("AAAAA\r\n"
                       "BBBBB\r\n"
                       "CCCCC\r\n"
                       "DDDDD\r\n"
                       "EEEEE\r\n"
                       "FFFFF");
    // History: AAAAA, BBBBB, CCCCC. Main page: DDDDD, EEEEE, FFFFF

    using namespace vtbackend;
    auto constexpr UiHandledHint = false;
    auto constexpr PixelCoord = vtbackend::PixelCoordinate {};

    SECTION("scrolls up and extends selection")
    {
        // Start a selection on the main page.
        mock.terminal.tick(1s);
        mock.terminal.sendMouseMoveEvent(
            Modifier::None, 1_lineOffset + 2_columnOffset, PixelCoord, UiHandledHint);
        mock.terminal.tick(2s);
        mock.terminal.sendMousePressEvent(
            Modifier::None, MouseButton::Left, 1_lineOffset + 2_columnOffset, PixelCoord, UiHandledHint);
        mock.terminal.tick(3s);
        mock.terminal.sendMouseMoveEvent(
            Modifier::None, 0_lineOffset + 0_columnOffset, PixelCoord, UiHandledHint);
        // Selection should be in progress now.
        REQUIRE(mock.terminal.selectionAvailable());
        CHECK(mock.terminal.selector()->state() == Selection::State::InProgress);

        // Perform auto-scroll up by 1 line.
        mock.terminal.performAutoScroll(-1, LineCount(1));
        CHECK(mock.terminal.viewport().scrollOffset() == ScrollOffset(1));

        // Perform auto-scroll up by 2 more lines.
        mock.terminal.performAutoScroll(-1, LineCount(2));
        CHECK(mock.terminal.viewport().scrollOffset() == ScrollOffset(3));
    }

    SECTION("does nothing without active selection")
    {
        // No selection → performAutoScroll should be a no-op.
        CHECK_FALSE(mock.terminal.selectionAvailable());
        mock.terminal.performAutoScroll(-1, LineCount(1));
        CHECK(mock.terminal.viewport().scrollOffset() == ScrollOffset(0));
    }

    SECTION("does nothing with completed selection")
    {
        // Create and complete a selection.
        mock.terminal.tick(1s);
        mock.terminal.sendMouseMoveEvent(
            Modifier::None, 0_lineOffset + 0_columnOffset, PixelCoord, UiHandledHint);
        mock.terminal.tick(2s);
        mock.terminal.sendMousePressEvent(
            Modifier::None, MouseButton::Left, 0_lineOffset + 0_columnOffset, PixelCoord, UiHandledHint);
        mock.terminal.tick(3s);
        mock.terminal.sendMouseMoveEvent(
            Modifier::None, 1_lineOffset + 2_columnOffset, PixelCoord, UiHandledHint);
        mock.terminal.tick(4s);
        mock.terminal.sendMouseReleaseEvent(Modifier::None, MouseButton::Left, PixelCoord, UiHandledHint);
        CHECK(mock.terminal.selector()->state() == Selection::State::Complete);

        // Auto-scroll should be a no-op for completed selections.
        mock.terminal.performAutoScroll(-1, LineCount(1));
        CHECK(mock.terminal.viewport().scrollOffset() == ScrollOffset(0));
    }

    SECTION("stops at history boundary")
    {
        // Start a selection.
        mock.terminal.tick(1s);
        mock.terminal.sendMouseMoveEvent(
            Modifier::None, 1_lineOffset + 0_columnOffset, PixelCoord, UiHandledHint);
        mock.terminal.tick(2s);
        mock.terminal.sendMousePressEvent(
            Modifier::None, MouseButton::Left, 1_lineOffset + 0_columnOffset, PixelCoord, UiHandledHint);
        mock.terminal.tick(3s);
        mock.terminal.sendMouseMoveEvent(
            Modifier::None, 0_lineOffset + 0_columnOffset, PixelCoord, UiHandledHint);

        // Try to scroll way past history.
        mock.terminal.performAutoScroll(-1, LineCount(100));
        // Should be capped at the available history.
        CHECK(mock.terminal.viewport().scrollOffset() <= ScrollOffset(3));
    }
}

TEST_CASE("Terminal.PassiveMouseTracking_Selection", "[terminal]")
{
    using namespace vtbackend;

    auto mock = MockTerm { ColumnCount(5), LineCount(3) };
    auto constexpr ClockBase = chrono::steady_clock::time_point();
    mock.terminal.tick(ClockBase);
    mock.terminal.ensureFreshRenderBuffer();

    mock.writeToScreen("Hello\r\n"
                       "World\r\n"
                       "Test!");

    mock.terminal.tick(ClockBase + chrono::seconds(1));
    mock.terminal.ensureFreshRenderBuffer();
    CHECK("Hello\nWorld\nTest!" == trimmedTextScreenshot(mock));

    auto constexpr UiHandledHint = false;
    auto constexpr PixelCoord = vtbackend::PixelCoordinate {};

    // Enable passive mouse tracking (DEC mode 2029).
    // This also implicitly enables MouseSGR (1006) and MouseProtocolButtonTracking (1002).
    mock.terminal.setMode(DECMode::MousePassiveTracking, true);
    CHECK(mock.terminal.isModeEnabled(DECMode::MousePassiveTracking));
    CHECK(mock.terminal.isModeEnabled(DECMode::MouseSGR));
    CHECK(mock.terminal.isModeEnabled(DECMode::MouseProtocolButtonTracking));

    // Clear any reply data generated by mode-setting.
    mock.resetReplyData();

    // Helper: perform a click-drag selection of "Hello" on line 0.
    auto const selectHello = [&] {
        mock.terminal.tick(1s);
        mock.terminal.sendMouseMoveEvent(
            Modifier::None, 0_lineOffset + 0_columnOffset, PixelCoord, UiHandledHint);
        mock.terminal.tick(2s);
        mock.terminal.sendMousePressEvent(
            Modifier::None, MouseButton::Left, 0_lineOffset + 0_columnOffset, PixelCoord, UiHandledHint);
        mock.terminal.tick(3s);
        mock.terminal.sendMouseMoveEvent(
            Modifier::None, 0_lineOffset + 4_columnOffset, PixelCoord, UiHandledHint);
        mock.terminal.tick(4s);
        mock.terminal.sendMouseReleaseEvent(Modifier::None, MouseButton::Left, PixelCoord, UiHandledHint);
        REQUIRE(mock.terminal.extractSelectionText() == "Hello");
    };

    SECTION("click-drag creates selection while passive tracking is on")
    {
        // Move cursor to start position (row 0, col 0)
        mock.terminal.tick(1s);
        mock.terminal.sendMouseMoveEvent(
            Modifier::None, 0_lineOffset + 0_columnOffset, PixelCoord, UiHandledHint);

        // Press left button — should start selection AND forward to app
        mock.terminal.tick(2s);
        auto const handled = mock.terminal.sendMousePressEvent(
            Modifier::None, MouseButton::Left, 0_lineOffset + 0_columnOffset, PixelCoord, UiHandledHint);

        // Passive tracking should return Handled{false} so session can also process action mappings
        CHECK(handled == Handled { false });

        // Selection should be in Waiting state (waiting for drag to start)
        REQUIRE(mock.terminal.selector());
        CHECK(mock.terminal.selector()->state() == Selection::State::Waiting);

        // Drag to extend selection (row 0, col 4 → selects "Hello")
        mock.terminal.tick(3s);
        mock.terminal.sendMouseMoveEvent(
            Modifier::None, 0_lineOffset + 4_columnOffset, PixelCoord, UiHandledHint);
        CHECK(mock.terminal.extractSelectionText() == "Hello");

        // Release
        mock.terminal.tick(4s);
        mock.terminal.sendMouseReleaseEvent(Modifier::None, MouseButton::Left, PixelCoord, UiHandledHint);
        CHECK(mock.terminal.extractSelectionText() == "Hello");

        // Verify mouse events were forwarded to the app via the PTY (SGR format: ESC [ < ... M/m)
        auto const& reply = mock.replyData();
        CHECK(reply.contains("\033[<"));
    }

    SECTION("click-to-deselect works with passive tracking")
    {
        selectHello();

        // Now click to deselect
        mock.terminal.tick(5s);
        mock.terminal.sendMousePressEvent(
            Modifier::None, MouseButton::Left, 0_lineOffset + 4_columnOffset, PixelCoord, UiHandledHint);
        mock.terminal.sendMouseReleaseEvent(Modifier::None, MouseButton::Left, PixelCoord, UiHandledHint);
        CHECK(mock.terminal.extractSelectionText().empty());
    }
}

TEST_CASE("Terminal.Cursorline.trivialLineUnderCursorIsHighlighted", "[terminal][vi]")
{
    // Regression: a plain-text (trivial) line under the vi cursor must be highlighted.
    // A tall page keeps all four content lines visible above the bottom status line that
    // normal mode pushes in.
    auto mc = MockTerm { PageSize { LineCount(8), ColumnCount(10) }, LineCount(0) };
    auto& terminal = mc.terminal;
    auto constexpr ClockBase = chrono::steady_clock::time_point();
    terminal.tick(ClockBase);

    // grid line 0: plain text (trivial), 1: colorized (inflated), 2: empty (trivial),
    // 3: plain text (trivial).
    mc.writeToScreen("plain0\r\n");
    mc.writeToScreen("\033[38;2;255;0;0m"); // red foreground
    mc.writeToScreen("tinted1");
    mc.writeToScreen("\033[m\r\n"); // reset SGR
    mc.writeToScreen("\r\n");
    mc.writeToScreen("plain3");

    terminal.inputHandler().setMode(vtbackend::ViMode::Normal);
    auto const defaultBg = terminal.colorPalette().defaultBackground;

    // Every line type (plain trivial, colorized inflated, empty trivial) must highlight when the
    // cursor lands on it. Assert the tint appears on exactly the cursor's screen line.
    auto seconds = 2;
    for (auto const gridLine: { 0, 1, 2, 3 })
    {
        terminal.moveNormalModeCursorTo(
            vtbackend::CellLocation { .line = LineOffset(gridLine), .column = ColumnOffset(0) });
        terminal.tick(ClockBase + chrono::seconds(seconds++));
        terminal.refreshRenderBuffer();
        auto const buf = terminal.renderBuffer();

        auto const cursorLine = renderedCursorLine(buf);
        REQUIRE(cursorLine.has_value());
        auto const bgOnCursorLine = screenLineBackground(buf, *cursorLine);
        INFO(std::format("gridLine={} renderedCursorLine={} bgOnCursorLine={}",
                         gridLine,
                         cursorLine->value,
                         bgOnCursorLine));
        // The cursor line's background must be tinted away from the default background.
        CHECK(bgOnCursorLine != defaultBg);
    }
}

TEST_CASE("Terminal.Cursorline.notShownInInsertMode", "[terminal][vi]")
{
    // The cursorline is a normal-mode affordance; insert mode must not tint any content line.
    auto mc = MockTerm { PageSize { LineCount(6), ColumnCount(10) }, LineCount(0) };
    auto& terminal = mc.terminal;
    auto constexpr ClockBase = chrono::steady_clock::time_point();
    terminal.tick(ClockBase);
    mc.writeToScreen("plainA\r\n");
    mc.writeToScreen("plainB\r\n");
    mc.writeToScreen("plainC");

    // Stays in the default insert mode (no status line, no cursorline).
    terminal.tick(ClockBase + chrono::seconds(1));
    terminal.refreshRenderBuffer();
    auto const buf = terminal.renderBuffer();
    auto const defaultBg = terminal.colorPalette().defaultBackground;

    for (auto const line: { 0, 1, 2 })
        CHECK(screenLineBackground(buf, LineOffset(line)) == defaultBg);
}

TEST_CASE("Terminal.Cursorline.nonCursorTrivialLineNotHighlighted", "[terminal][vi]")
{
    // Only the cursor's line is tinted; a sibling plain-text trivial line keeps the default bg.
    // Use a tall page so content lines stay clear of the bottom indicator status line.
    auto mc = MockTerm { PageSize { LineCount(6), ColumnCount(10) }, LineCount(0) };
    auto& terminal = mc.terminal;
    auto constexpr ClockBase = chrono::steady_clock::time_point();
    terminal.tick(ClockBase);
    mc.writeToScreen("plainA\r\n");
    mc.writeToScreen("plainB");

    terminal.inputHandler().setMode(vtbackend::ViMode::Normal);
    terminal.moveNormalModeCursorTo(
        vtbackend::CellLocation { .line = LineOffset(0), .column = ColumnOffset(0) });
    terminal.tick(ClockBase + chrono::seconds(1));
    terminal.refreshRenderBuffer();
    auto const buf = terminal.renderBuffer();
    auto const defaultBg = terminal.colorPalette().defaultBackground;

    auto const cursorLine = renderedCursorLine(buf);
    REQUIRE(cursorLine.has_value());
    CHECK(screenLineBackground(buf, *cursorLine) != defaultBg);
    // The immediately following content line (not the cursor line, well above the status line)
    // must keep the default background.
    CHECK(screenLineBackground(buf, LineOffset(cursorLine->value + 1)) == defaultBg);
}

TEST_CASE("Terminal.GraphemeCluster.selectionDoesNotShiftLayout", "[terminal][unicode]")
{
    // Each case names the layout it must produce, unselected AND selected, and which path the
    // unselected state must have taken. Naming the layout rather than comparing the two states
    // against each other matters: both derive from the same line, so two equally wrong layouts
    // would satisfy a self-consistency check without drawing the right thing. Naming the path
    // matters just as much -- a case that never reaches the batched form is not testing the
    // batched-vs-per-cell agreement it appears to be testing.
    struct TestCase
    {
        std::string_view name;
        std::string_view text;
        std::string_view layout;
        RenderPath unselectedPath;
    };

    // The first three are #1752's reproduction lines. The two that broke are the two whose BASE
    // codepoint is one column wide and which only reach two columns through VS16 -- 1F441 EYE and
    // 1F3F3 WAVING WHITE FLAG; 1F44D THUMBS UP is wide already, which is why the reporter saw it
    // survive. In all three the closing bracket belongs at column 3. They widen through a
    // continuation cell, which is what takes them off the batched path in BOTH states.
    auto constexpr Cases = std::array {
        TestCase { .name = "thumbs up + skin tone modifier",
                   .text = "[\U0001F44D\U0001F3FB]"sv,
                   .layout = "0:U+005B(w1) 1:U+1F44D+U+1F3FB(w2) 3:U+005D(w1)"sv,
                   .unselectedPath = RenderPath::PerCell },
        TestCase { .name = "rainbow flag: VS16, ZWJ, rainbow",
                   .text = "[\U0001F3F3\uFE0F\u200D\U0001F308]"sv,
                   .layout = "0:U+005B(w1) 1:U+1F3F3+U+FE0F+U+200D+U+1F308(w2) 3:U+005D(w1)"sv,
                   .unselectedPath = RenderPath::PerCell },
        TestCase { .name = "eye + VS16",
                   .text = "[\U0001F441\uFE0F]"sv,
                   .layout = "0:U+005B(w1) 1:U+1F441+U+FE0F(w2) 3:U+005D(w1)"sv,
                   .unselectedPath = RenderPath::PerCell },
        // Plain text stays on the batched path, so this is the case that actually compares the two
        // representations against each other -- the comparison #1752 is about.
        TestCase { .name = "plain text",
                   .text = "abc"sv,
                   .layout = "0:U+0061(w1) 1:U+0062(w1) 2:U+0063(w1)"sv,
                   .unselectedPath = RenderPath::Batched },
        // A cluster that stays ONE column wide writes no continuation cell, so only the cluster rule
        // itself takes its line off the batched path -- and that path stores one codepoint per
        // column (@see Line::trivialBuffer), silently discarding everything after the base. A
        // decomposed alpha + COMBINING ACUTE therefore reached the renderer as a bare alpha.
        TestCase { .name = "alpha + combining acute",
                   .text = "\u03B1\u0301x"sv,
                   .layout = "0:U+03B1+U+0301(w1) 1:U+0078(w1)"sv,
                   .unselectedPath = RenderPath::PerCell },
    };

    for (auto const& testCase: Cases)
    {
        auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(20) }, LineCount(0) };
        // Park the cursor on the NEXT line. Left on line 0, gridLineContainsCursor() forces the
        // per-cell path even unselected, and the batched half of every comparison below silently
        // stops happening.
        mock.writeToScreen(testCase.text);
        mock.writeToScreen("\r\n"sv);

        auto const [unselected, unselectedPath] = renderLineOf(mock.terminal, LineOffset(0));
        selectColumns(mock.terminal, LineOffset(0), ColumnOffset(0), ColumnOffset(19));
        auto const [selected, selectedPath] = renderLineOf(mock.terminal, LineOffset(0));

        INFO(testCase.name);
        CHECK(unselectedPath == testCase.unselectedPath);
        CHECK(selectedPath == RenderPath::PerCell); // a selection always forces per-cell
        CHECK(unselected == testCase.layout);
        CHECK(selected == testCase.layout);
    }
}

TEST_CASE("Terminal.GraphemeCluster.aWideCellOnTheLastColumnKeepsItsWidth", "[terminal][unicode]")
{
    // A wide character leaves the batched path only through the continuation cell it writes, and
    // Screen::clearAndAdvance skips that fill when a single column is left -- so a full-width
    // character on the last writable column stays on a line the batched path still claims, as a
    // TWO-column cell. The selection fallback must measure it the same way the batched path does;
    // reporting one column shrinks the selection background and any underline behind the glyph.
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(5) }, LineCount(0) };
    mock.writeToScreen("\033[1;5H\u4E16"sv); // last column of a 5-column page
    mock.writeToScreen("\033[2;1H"sv);       // cursor off the line under test

    REQUIRE(mock.terminal.primaryScreen().grid().lineAt(LineOffset(0)).isTrivialBuffer());
    REQUIRE(mock.terminal.primaryScreen().at(LineOffset(0), ColumnOffset(4)).width() == 2);

    auto const [unselected, unselectedPath] = renderLineOf(mock.terminal, LineOffset(0));
    selectColumns(mock.terminal, LineOffset(0), ColumnOffset(0), ColumnOffset(4));
    auto const [selected, selectedPath] = renderLineOf(mock.terminal, LineOffset(0));

    CHECK(unselectedPath == RenderPath::Batched);
    CHECK(selectedPath == RenderPath::PerCell);
    CHECK(unselected == "4:U+4E16(w2)");
    CHECK(selected == unselected);
}

TEST_CASE("Terminal.GraphemeCluster.batchedFallbackDoesNotInheritTheCursor", "[terminal][unicode]")
{
    // makeColorsForCell extends a block cursor over the second column of a wide glyph by consulting
    // _prevWidth/_prevHasCursor, which every emitter must reset per cell. startLine() resets them
    // for a per-cell line, but renderTrivialLine has no such entry point -- so if its fallback
    // resets only once, a line following one that ENDS in a wide glyph under the cursor is painted
    // entirely in cursor colours.
    //
    // Asserted against a control that differs only in the previous line's last cell, so no palette
    // value is hard-coded.
    auto const colorsOfSecondLine = [](std::string_view lastCellOfFirstLine) {
        auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(5) }, LineCount(0) };
        auto constexpr ClockBase = chrono::steady_clock::time_point();
        mock.terminal.tick(ClockBase);
        // The block-cursor inversion this test is about is suppressed while a cursor MOTION
        // animation is in flight (makeColorsForCell requires animationProgress >= 1). Writing text
        // is exactly what starts one, so switch the animation off rather than race it.
        mock.terminal.settings().cursorMotionAnimationDuration = chrono::milliseconds(0);

        mock.writeToScreen("\033[2;1H"sv);
        mock.writeToScreen("xyz"sv);                       // line 1: uniform text
        mock.writeToScreen("\033[1;1H\033[31mab\033[m"sv); // line 0: mixed SGR -> per-cell path
        mock.writeToScreen(std::format("\033[1;5H{}", lastCellOfFirstLine));

        // The cursor must end ON line 0's last cell, so renderCell leaves _prevHasCursor set.
        REQUIRE(mock.terminal.primaryScreen().cursor().position
                == CellLocation { .line = LineOffset(0), .column = ColumnOffset(4) });
        // Line 1 is selected in part, which sends it to renderTrivialLine's fallback; columns 0..2
        // are outside the selection, so nothing may recolor them.
        selectColumns(mock.terminal, LineOffset(1), ColumnOffset(3), ColumnOffset(4));

        mock.terminal.tick(ClockBase + chrono::seconds(1));
        mock.terminal.refreshRenderBuffer();
        auto const buf = mock.terminal.renderBuffer();
        auto result = std::vector<vtbackend::RGBColor> {};
        for (auto const& cell: buf.get().cells)
            if (cell.position.line == LineOffset(1) && cell.position.column < ColumnOffset(3))
                result.push_back(cell.attributes.backgroundColor);
        return result;
    };

    auto const afterWideGlyph = colorsOfSecondLine("\u4E16"sv); // two columns, cursor on it
    auto const afterNarrowGlyph = colorsOfSecondLine("Z"sv);    // one column, cursor on it

    REQUIRE(afterNarrowGlyph.size() == 3);
    CHECK(afterWideGlyph == afterNarrowGlyph);
}

TEST_CASE("Terminal.GraphemeCluster.aClusterLeavesTheBatchedPath", "[terminal][unicode]")
{
    // Why the layouts above can agree at all: a line the batched RenderLine path cannot express
    // must not take it. That path stores one codepoint per column, so a cell holding more than one
    // -- whether or not the cluster also grew wider -- has to leave it, exactly as a wide cell and
    // an image fragment already do. Stated on its own so a change that re-admits such a line fails
    // HERE, naming the cause, rather than as a layout mismatch above or only in the GUI.
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(20) }, LineCount(0) };
    auto const& screen = mock.terminal.primaryScreen();

    mock.writeToScreen("[\U0001F441\uFE0F]"sv);
    CHECK_FALSE(screen.grid().lineAt(LineOffset(0)).isTrivialBuffer());
    // The eye is two columns: its own, plus the one VS16 claimed for it.
    CHECK(screen.at(LineOffset(0), ColumnOffset(1)).width() == 2);

    // The same must hold when the cluster stays one column wide, which is the case that has no
    // continuation cell to break SGR uniformity on its behalf.
    mock.writeToScreen("\r\n\u03B1\u0301x"sv);
    CHECK(screen.at(LineOffset(1), ColumnOffset(0)).codepointCount() == 2);
    CHECK_FALSE(screen.grid().lineAt(LineOffset(1)).isTrivialBuffer());
    CHECK(screen.at(LineOffset(1), ColumnOffset(0)).width() == 1);
}

TEST_CASE("Terminal.hint_mode_accepts_labels_while_lock_keys_are_latched", "[terminal][locks]")
{
    for (auto const locks: LockCombinations)
    {
        INFO(std::format("lock modifiers {}", locks));
        auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(40) } };
        mock.writeToScreen("visit https://example.com now");

        mock.terminal.activateHintMode(
            vtbackend::HintModeRequest { .patterns = vtbackend::HintModeHandler::builtinPatterns(),
                                         .action = vtbackend::HintAction::Copy });
        REQUIRE(mock.terminal.isHintModeActive());
        REQUIRE(mock.terminal.hintMatches().size() == 1);

        auto const label = mock.terminal.hintMatches().at(0).label;
        REQUIRE(label.size() == 1);
        mock.resetReplyData();

        mock.sendCharEvent(static_cast<char32_t>(label[0]), locks);

        // The label character is consumed by hint mode, never forwarded to the application.
        CHECK(mock.replyData().empty());
        CHECK(!mock.terminal.isHintModeActive());
        CHECK(mock.clipboardData == "https://example.com");
    }
}

TEST_CASE("Terminal.DECUDK_fires_while_lock_keys_are_latched", "[terminal][locks]")
{
    for (auto const locks: LockCombinations)
    {
        INFO(std::format("lock modifiers {}", locks));
        auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };

        // Program F6 (UDK id 17) with "Hello" (hex: 48656C6C6F).
        mock.writeToScreen("\033P0;1|17/48656C6C6F\033\\");
        mock.terminal.flushInput();
        REQUIRE(mock.terminal.udkString(17).has_value());
        mock.resetReplyData();

        mock.sendKeyEvent(vtbackend::Key::F6, locks);

        CHECK(e(mock.replyData()) == e("Hello"));
    }
}

TEST_CASE("Terminal.DECMode.numberMappingRoundTrips", "[terminal]")
{
    using namespace vtbackend;

    // The DECMode<->number mapping is single-sourced in DECModeNumbers; SM/RM and DECRQM/DECRPM read it
    // in opposite directions. Guard that every row round-trips both ways, so a mode added to the table
    // is always both settable AND reportable -- never one without the other.
    for (auto const& [mode, number]: DECModeNumbers)
    {
        CHECK(toDECModeNum(mode) == number);
        CHECK(fromDECModeNum(number) == mode);
    }

    // A number with no row is simply unrecognised, in either query.
    CHECK(fromDECModeNum(38) == std::nullopt); // DECTEK, not implemented
    CHECK(fromDECModeNum(44) == std::nullopt); // margin bell, not implemented
    CHECK_FALSE(isValidDECMode(38));
}

TEST_CASE("TraceHandler.an_APC_body_waits_its_turn_like_every_other_sequence", "[terminal][trace]")
{
    // TraceHandler is a BUFFERING decorator: its whole job is to hold what the application sent, in
    // order, so the user can step through it. Forwarding an APC straight to the display let it
    // overtake everything still queued ahead of it -- so a kitty image was placed against the cursor
    // position that a queued CUP had not moved yet, and tracing a graphics problem showed behaviour
    // that never occurs outside trace mode.
    auto mock = MockTerm { PageSize { LineCount(4), ColumnCount(8) } };
    auto trace = vtbackend::TraceHandler { mock.terminal };

    auto cup = vtbackend::Sequence {};
    cup.setCategory(vtbackend::FunctionCategory::CSI);
    cup.setFinalChar('H');
    trace.processSequence(cup);

    trace.processAPC("Ga=T,f=32,s=2,v=2;AAAA"sv);

    REQUIRE(trace.pendingSequences().size() == 2);
    CHECK(std::holds_alternative<vtbackend::Sequence>(trace.pendingSequences()[0]));

    // Queued, and BEHIND the sequence that preceded it -- not executed on arrival.
    auto const* apc =
        std::get_if<vtbackend::TraceHandler::ApplicationProgramCommand>(&trace.pendingSequences()[1]);
    REQUIRE(apc != nullptr);
    CHECK(apc->body == "Ga=T,f=32,s=2,v=2;AAAA");

    // The body is OWNED, not viewed: it outlives the parser buffer it arrived in, because nothing
    // runs it until the user steps the trace forward.
    CHECK(mock.terminal.primaryScreen().at(LineOffset(0), ColumnOffset(0)).imageFragment() == nullptr);
}

TEST_CASE("Terminal.focus.events_reach_the_pty_only_under_DECMode_1004", "[terminal][focus]")
{
    // Focus state is tracked unconditionally (the renderer needs it for the inactive cursor shape and
    // the inactive indicator status line), but the CSI I / CSI O notification is gated on DECMode 1004.
    // The return value reports whether bytes were actually produced.
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(10) } };

    SECTION("mode off: focus is tracked, nothing reaches the PTY")
    {
        mock.discardPendingReplies();

        CHECK_FALSE(mock.terminal.sendFocusOutEvent());
        CHECK_FALSE(mock.terminal.focused());
        CHECK(mock.replyData().empty());

        CHECK_FALSE(mock.terminal.sendFocusInEvent());
        CHECK(mock.terminal.focused());
        CHECK(mock.replyData().empty());
    }

    SECTION("mode on: CSI O on focus loss, CSI I on focus gain")
    {
        mock.writeToScreen("\033[?1004h");
        mock.discardPendingReplies();

        CHECK(mock.terminal.sendFocusOutEvent());
        CHECK_FALSE(mock.terminal.focused());
        CHECK(e(mock.replyData()) == e("\033[O"s));

        mock.resetReplyData();

        CHECK(mock.terminal.sendFocusInEvent());
        CHECK(mock.terminal.focused());
        CHECK(e(mock.replyData()) == e("\033[I"s));
    }
}

TEST_CASE("Terminal.contains is exclusive on both axes", "[terminal]")
{
    // The column bound used to be `<=`, accepting a coordinate one cell past the right edge while the
    // line bound was correctly exclusive. Nothing called it yet, which is exactly why it was worth
    // fixing: the first caller to guard an index with it would have read out of bounds.
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(5) } };
    auto const& terminal = mock.terminal;

    auto const at = [](int line, int column) {
        return CellLocation { .line = LineOffset(line), .column = ColumnOffset(column) };
    };

    CHECK(terminal.contains(at(0, 0)));
    CHECK(terminal.contains(at(2, 4))); // last addressable cell

    CHECK_FALSE(terminal.contains(at(0, 5)));  // one past the right edge
    CHECK_FALSE(terminal.contains(at(3, 0)));  // one past the bottom edge
    CHECK_FALSE(terminal.contains(at(0, -1))); // negative column
    CHECK_FALSE(terminal.contains(at(-1, 0))); // negative line (scrollback is not the page)

    // The screen-level predicate answers identically for the main page.
    CHECK(terminal.currentScreen().contains(at(2, 4)));
    CHECK_FALSE(terminal.currentScreen().contains(at(2, 5)));
}

TEST_CASE("Terminal.flushInput drops pending input on a fatal PTY write error", "[terminal][pty]")
{
    // The frontend retries flushInput() for as long as hasInput() reports bytes are still pending
    // (TerminalSession::flushInput posts itself again). That is right for backpressure and wrong for a
    // dead device: bytes that can never be delivered would keep hasInput() true forever, turning the
    // retry into an unbounded loop that logs one error per iteration -- the "Failed to write to SSH
    // channel" flood seen on a corrupted SSH session (#1495).
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(10) } };
    auto& pty = static_cast<vtpty::MockPty&>(mock.terminal.device());

    SECTION("EAGAIN is backpressure: the bytes stay pending for the next attempt")
    {
        pty.setWriteBehavior(vtpty::PtyWriteBehavior::FailAgain);
        mock.terminal.sendRawInput("hello"sv);

        CHECK(mock.terminal.hasInput());

        // Once the device accepts again, the very same bytes go out.
        pty.setWriteBehavior(vtpty::PtyWriteBehavior::Accept);
        mock.terminal.flushInput();
        CHECK_FALSE(mock.terminal.hasInput());
        CHECK(pty.stdinBuffer() == "hello");
    }

    SECTION("a fatal error drops them, so the caller's retry terminates")
    {
        pty.setWriteBehavior(vtpty::PtyWriteBehavior::FailFatally);
        mock.terminal.sendRawInput("hello"sv);

        CHECK_FALSE(mock.terminal.hasInput());

        // A second flush has nothing left to send, so no further error is produced.
        mock.terminal.flushInput();
        CHECK_FALSE(mock.terminal.hasInput());
        CHECK(pty.stdinBuffer().empty());
    }
}

TEST_CASE("Terminal.hint_mode_matches_a_url_wrapped_across_rows", "[terminal][hintmode]")
{
    // 20 columns forces "https://example.com/wrapped" to wrap after column 20. Scanning per
    // physical row would find at most the truncated head; the logical line yields the whole URL.
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) }, LineCount(10) };
    mock.writeToScreen("https://example.com/wrapped");
    mock.terminal.flushInput();

    REQUIRE(mock.terminal.currentScreen().isLineWrapped(LineOffset(1)));

    mock.terminal.activateHintMode(vtbackend::HintModeRequest {
        .patterns = vtbackend::HintModeHandler::builtinPatterns(), .action = vtbackend::HintAction::Copy });

    REQUIRE(mock.terminal.hintMatches().size() == 1);
    auto const& match = mock.terminal.hintMatches().at(0);
    CHECK(match.matchedText == "https://example.com/wrapped");
    CHECK(match.start == CellLocation { .line = LineOffset(0), .column = ColumnOffset(0) });
    CHECK(match.end == CellLocation { .line = LineOffset(1), .column = ColumnOffset(6) });
}

TEST_CASE("Terminal.hint_mode_visible_scope_ignores_scrollback", "[terminal][hintmode]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(40) }, LineCount(20) };
    mock.writeToScreen("https://scrolled-away.example\r\n\r\n\r\n\r\n\r\n");
    mock.terminal.flushInput();

    // The URL is now above the page: it must not be offered under the default scope.
    REQUIRE(mock.terminal.currentScreen().historyLineCount() > LineCount(0));

    mock.terminal.activateHintMode(
        vtbackend::HintModeRequest { .patterns = vtbackend::HintModeHandler::builtinPatterns(),
                                     .action = vtbackend::HintAction::Copy,
                                     .scope = vtbackend::HintScope::Visible });

    CHECK(mock.terminal.hintMatches().empty());
}

TEST_CASE("Terminal.hint_mode_scrollback_scope_finds_history", "[terminal][hintmode]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(40) }, LineCount(20) };
    mock.writeToScreen("https://scrolled-away.example\r\n\r\n\r\n\r\n\r\n");
    mock.terminal.flushInput();

    mock.terminal.activateHintMode(
        vtbackend::HintModeRequest { .patterns = vtbackend::HintModeHandler::builtinPatterns(),
                                     .action = vtbackend::HintAction::Copy,
                                     .scope = vtbackend::HintScope::Scrollback,
                                     .scrollbackLimit = LineCount(1000) });

    REQUIRE(mock.terminal.hintMatches().size() == 1);
    auto const& match = mock.terminal.hintMatches().at(0);
    CHECK(match.matchedText == "https://scrolled-away.example");
    // A history row: a negative, scroll-invariant line offset.
    CHECK(match.start.line < LineOffset(0));
}

TEST_CASE("Terminal.hint_mode_scrollback_labels_survive_scrolling", "[terminal][hintmode]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(40) }, LineCount(20) };
    // The two URLs go in separate calls so that each line-break escape ends a literal rather than
    // sitting in front of a word: check-spelling splits identifiers on case boundaries, so an escape
    // glued ahead of a word forms a nonsense token and fails the spell gate.
    mock.writeToScreen("https://one.example\r\n");
    mock.writeToScreen("https://two.example\r\n\r\n\r\n\r\n\r\n");
    mock.terminal.flushInput();

    mock.terminal.activateHintMode(
        vtbackend::HintModeRequest { .patterns = vtbackend::HintModeHandler::builtinPatterns(),
                                     .action = vtbackend::HintAction::Copy,
                                     .scope = vtbackend::HintScope::Scrollback,
                                     .scrollbackLimit = LineCount(1000) });

    auto const before = mock.terminal.hintMatches();
    REQUIRE(before.size() == 2);

    // Scrolling reveals other rows' labels; it must not rename the ones already on screen, or a
    // label the user has read becomes wrong under their fingers.
    mock.terminal.viewport().scrollUp(LineCount(2));

    auto const after = mock.terminal.hintMatches();
    REQUIRE(after.size() == before.size());
    for (auto const i: std::views::iota(size_t { 0 }, after.size()))
    {
        CHECK(after[i].label == before[i].label);
        CHECK(after[i].start == before[i].start);
        CHECK(after[i].matchedText == before[i].matchedText);
    }
}

TEST_CASE("Terminal.hint_mode_visible_scope_scans_again_on_scroll", "[terminal][hintmode]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(40) }, LineCount(20) };
    mock.writeToScreen("https://scrolled-away.example\r\n\r\n\r\n\r\n\r\n");
    mock.terminal.flushInput();

    mock.terminal.activateHintMode(
        vtbackend::HintModeRequest { .patterns = vtbackend::HintModeHandler::builtinPatterns(),
                                     .action = vtbackend::HintAction::Copy,
                                     .scope = vtbackend::HintScope::Visible });
    REQUIRE(mock.terminal.hintMatches().empty());

    // The visible scope's scan region *is* the viewport, so scrolling the URL back into view must
    // surface it. Scroll to the very top rather than by a hardcoded count: the URL is on the oldest
    // history row, wherever the scroll accounting puts it.
    REQUIRE(mock.terminal.viewport().scrollToTop());

    REQUIRE(mock.terminal.hintMatches().size() == 1);
    CHECK(mock.terminal.hintMatches().at(0).matchedText == "https://scrolled-away.example");
}

TEST_CASE("Terminal.hint_mode_overlay_draws_labels_into_the_render_buffer", "[terminal][hintmode]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(40) }, LineCount(10) };
    mock.writeToScreen("visit https://example.com now");
    mock.terminal.flushInput();

    mock.terminal.activateHintMode(vtbackend::HintModeRequest {
        .patterns = vtbackend::HintModeHandler::builtinPatterns(), .action = vtbackend::HintAction::Copy });
    REQUIRE(mock.terminal.hintMatches().size() == 1);
    auto const& match = mock.terminal.hintMatches().at(0);
    REQUIRE(match.label == "a");

    mock.terminal.refreshRenderBuffer();
    auto const buf = mock.terminal.renderBuffer();

    // The label replaces the match's first cell.
    auto const labelCell =
        std::ranges::find_if(buf.get().cells, [&](auto const& cell) { return cell.position == match.start; });
    REQUIRE(labelCell != buf.get().cells.end());
    CHECK(labelCell->codepoints == U"a");
}

TEST_CASE("Terminal.hint_mode_overlay_reaches_a_line_without_the_cursor", "[terminal][hintmode]")
{
    // A plain, uniformly-coloured line with no cursor and no selection is rendered through the
    // trivial-line fast path, which emits one RenderLine instead of per-cell RenderCells. The hint
    // overlay only rewrites cells, so the label must not be lost to that path.
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(40) }, LineCount(10) };
    mock.writeToScreen("visit https://example.com now\r\n");
    mock.writeToScreen("second line holds the cursor");
    mock.terminal.flushInput();

    mock.terminal.activateHintMode(vtbackend::HintModeRequest {
        .patterns = vtbackend::HintModeHandler::builtinPatterns(), .action = vtbackend::HintAction::Copy });
    REQUIRE(mock.terminal.hintMatches().size() == 1);
    auto const& match = mock.terminal.hintMatches().at(0);
    REQUIRE(match.start.line == LineOffset(0));

    mock.terminal.refreshRenderBuffer();
    auto const buf = mock.terminal.renderBuffer();

    auto const labelCell =
        std::ranges::find_if(buf.get().cells, [&](auto const& cell) { return cell.position == match.start; });
    REQUIRE(labelCell != buf.get().cells.end());
    CHECK(labelCell->codepoints == U"a");
}

TEST_CASE("Terminal.hint_mode_overlay_highlights_a_wrapped_match_on_both_rows", "[terminal][hintmode]")
{
    // The overlay's match body is a run of text, not a rectangle: a wrapped match must be
    // highlighted on its continuation row too, and only up to the column it actually ends on.
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(20) }, LineCount(10) };
    mock.writeToScreen("https://example.com/wrapped");
    mock.terminal.flushInput();

    mock.terminal.activateHintMode(vtbackend::HintModeRequest {
        .patterns = vtbackend::HintModeHandler::builtinPatterns(), .action = vtbackend::HintAction::Copy });
    REQUIRE(mock.terminal.hintMatches().size() == 1);
    auto const& match = mock.terminal.hintMatches().at(0);
    REQUIRE(match.start.line == LineOffset(0));
    REQUIRE(match.end.line == LineOffset(1));

    mock.terminal.refreshRenderBuffer();
    auto const buf = mock.terminal.renderBuffer();

    auto const backgroundAt = [&](CellLocation pos) -> std::optional<vtbackend::RGBColor> {
        auto const it =
            std::ranges::find_if(buf.get().cells, [&](auto const& cell) { return cell.position == pos; });
        if (it == buf.get().cells.end())
            return std::nullopt;
        return it->attributes.backgroundColor;
    };

    // Compared within the continuation row, so cursorline colouring (the Vi cursor sits on the
    // last written row) cannot confound it: the cells the match covers are highlighted, and the
    // first cell past its end is not.
    auto const firstInside = backgroundAt(CellLocation { .line = match.end.line, .column = ColumnOffset(0) });
    auto const lastInside = backgroundAt(CellLocation { .line = match.end.line, .column = match.end.column });
    auto const firstOutside =
        backgroundAt(CellLocation { .line = match.end.line, .column = match.end.column + ColumnOffset(1) });

    REQUIRE(firstInside.has_value());
    REQUIRE(lastInside.has_value());
    REQUIRE(firstOutside.has_value());
    CHECK(*firstInside == *lastInside);   // the whole run on this row is highlighted
    CHECK(*firstOutside != *firstInside); // and the highlight stops where the match does
}

TEST_CASE("Terminal.hint_mode_dispatches_each_action", "[terminal][hintmode]")
{
    // Every HintAction routed through HintModeExecutor::onHintSelected, so a new one cannot be
    // added without a home here.
    auto const activate = [](auto& mock, vtbackend::HintAction action) {
        mock.terminal.activateHintMode(vtbackend::HintModeRequest {
            .patterns = vtbackend::HintModeHandler::builtinPatterns(), .action = action });
        REQUIRE(mock.terminal.hintMatches().size() == 1);
        mock.sendCharEvent(U'a');
    };

    SECTION("Copy")
    {
        auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(40) } };
        mock.writeToScreen("go https://example.com now");
        mock.terminal.flushInput();
        activate(mock, vtbackend::HintAction::Copy);
        CHECK(mock.clipboardData == "https://example.com");
    }

    SECTION("Open")
    {
        auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(40) } };
        mock.writeToScreen("go https://example.com now");
        mock.terminal.flushInput();
        activate(mock, vtbackend::HintAction::Open);
        CHECK(mock.openedDocument == "https://example.com");
    }

    SECTION("Paste")
    {
        auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(40) } };
        mock.writeToScreen("go https://example.com now");
        mock.terminal.flushInput();
        mock.resetReplyData();
        activate(mock, vtbackend::HintAction::Paste);
        CHECK(mock.replyData() == "https://example.com");
    }

    SECTION("CopyAndPaste")
    {
        auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(40) } };
        mock.writeToScreen("go https://example.com now");
        mock.terminal.flushInput();
        mock.resetReplyData();
        activate(mock, vtbackend::HintAction::CopyAndPaste);
        CHECK(mock.clipboardData == "https://example.com");
        CHECK(mock.replyData() == "https://example.com");
    }

    SECTION("Select")
    {
        auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(40) } };
        mock.writeToScreen("go https://example.com now");
        mock.terminal.flushInput();
        activate(mock, vtbackend::HintAction::Select);

        // Visual mode with the match pre-selected, ready for manual adjustment before yanking.
        CHECK(mock.terminal.inputHandler().mode() == vtbackend::ViMode::Visual);
        REQUIRE(mock.terminal.selector() != nullptr);
        CHECK(mock.terminal.selector()->from()
              == CellLocation { .line = LineOffset(0), .column = ColumnOffset(3) });
    }
}

TEST_CASE("Terminal.hint_mode_validates_and_resolves_paths_against_the_working_directory",
          "[terminal][hintmode]")
{
    // With a working directory known (OSC 7), the filepath pattern broadens to bare names and a
    // filesystem-existence validator keeps only the ones that are really there; the match is then
    // transformed to an absolute path so Copy and Open get something usable. The validator is
    // memoized per activation, so a name repeated across rows is stat()ed once.
    namespace fs = std::filesystem;

    auto const tmpRoot =
        fs::temp_directory_path()
        / std::format("contour-hint-cwd-{}", std::chrono::steady_clock::now().time_since_epoch().count());
    fs::create_directories(tmpRoot);
    {
        auto file = std::ofstream(tmpRoot / "Makefile");
        file << "all:\n";
    }
    auto const cleanup = crispy::Finally { [&]() { fs::remove_all(tmpRoot); } };

    auto mock = MockTerm { PageSize { LineCount(4), ColumnCount(60) }, LineCount(10) };
    mock.terminal.setCurrentWorkingDirectory("file://" + tmpRoot.generic_string());

    // "Makefile" exists and must be offered; "Nonexistent" does not and must be filtered out. The
    // repeat is what the memo cache collapses.
    mock.writeToScreen("edit Makefile please\r\n");
    mock.writeToScreen("also Makefile again\r\n");
    mock.writeToScreen("but Nonexistent is absent");
    mock.terminal.flushInput();

    mock.terminal.activateHintMode(vtbackend::HintModeRequest {
        .patterns = vtbackend::HintModeHandler::builtinPatterns(), .action = vtbackend::HintAction::Copy });

    auto const& matches = mock.terminal.hintMatches();
    auto const expected = (tmpRoot / "Makefile").generic_string();
    REQUIRE(matches.size() == 2); // the two Makefile mentions, not the missing name
    CHECK(matches[0].matchedText == expected);
    CHECK(matches[1].matchedText == expected);
    CHECK(std::ranges::none_of(matches, [](auto const& m) { return m.matchedText.contains("Nonexistent"); }));
}

TEST_CASE("Terminal.hint_mode_extends_the_scan_past_the_viewport_to_finish_a_wrapped_line",
          "[terminal][hintmode]")
{
    // A one-row viewport so a two-row wrapped match can only ever straddle its edge.
    auto mock = MockTerm { PageSize { LineCount(1), ColumnCount(20) }, LineCount(10) };
    mock.writeToScreen("https://example.com/wrapped");
    mock.writeToScreen("\r\n\r\n\r\n");
    mock.terminal.flushInput();

    auto const history = unbox<int>(mock.terminal.currentScreen().historyLineCount());
    REQUIRE(history >= 3);

    // Grid rows: the URL head, then its continuation, then the blank rows the line feeds added.
    auto const head = LineOffset(-history);
    REQUIRE_FALSE(mock.terminal.currentScreen().isLineWrapped(head));
    REQUIRE(mock.terminal.currentScreen().isLineWrapped(head + LineOffset(1)));

    auto const activateAt = [&](int scrollOffset) {
        mock.terminal.viewport().scrollTo(vtbackend::ScrollOffset(scrollOffset));
        mock.terminal.activateHintMode(
            vtbackend::HintModeRequest { .patterns = vtbackend::HintModeHandler::builtinPatterns(),
                                         .action = vtbackend::HintAction::Copy });
    };

    SECTION("the head is visible: the scan reaches down for the tail and yields the whole URL")
    {
        activateAt(history);

        REQUIRE(mock.terminal.hintMatches().size() == 1);
        auto const& match = mock.terminal.hintMatches().at(0);
        CHECK(match.matchedText == "https://example.com/wrapped");
        CHECK(match.start.line == head);
        CHECK(match.end.line == head + LineOffset(1)); // the tail is off screen, but it is matched
    }

    SECTION("only the tail is visible: the hint is not offered, because its label cannot be drawn")
    {
        activateAt(history - 1);

        CHECK(mock.terminal.hintMatches().empty());
    }
}

TEST_CASE("Terminal.hint_mode_maps_columns_past_a_wide_character", "[terminal][hintmode]")
{
    // A double-width glyph before the URL must not shift the label/highlight left: the scan text is
    // column-aligned (the wide character's continuation cell becomes a space), so the URL's grid
    // column accounts for the two cells the glyph occupies.
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(40) }, LineCount(10) };
    mock.writeToScreen("\xe4\xb8\xad https://example.com"); // 中 (2 columns) + space + URL
    mock.terminal.flushInput();

    // 中 really is double-width here, otherwise the column arithmetic below would not be exercised.
    REQUIRE(mock.terminal.currentScreen().cellWidthAt(
                CellLocation { .line = LineOffset(0), .column = ColumnOffset(0) })
            == 2);

    mock.terminal.activateHintMode(vtbackend::HintModeRequest {
        .patterns = vtbackend::HintModeHandler::builtinPatterns(), .action = vtbackend::HintAction::Copy });

    REQUIRE(mock.terminal.hintMatches().size() == 1);
    auto const& match = mock.terminal.hintMatches().at(0);
    CHECK(match.matchedText == "https://example.com");
    // 中 occupies columns 0-1 and the space column 2, so the URL starts at column 3 — not column 2,
    // which is where a codepoint-counted (wide-character-collapsing) mapping would wrongly place it.
    CHECK(match.start == CellLocation { .line = LineOffset(0), .column = ColumnOffset(3) });
}

TEST_CASE("Terminal.hint_mode_survives_a_negative_scrollback_limit", "[terminal][hintmode]")
{
    // A negative hint_scrollback_lines (an out-of-range config value) must not push the labelable
    // range past the grid and underflow the row reservation, which would terminate the terminal.
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(40) }, LineCount(20) };
    mock.writeToScreen("https://one.example\r\n\r\n\r\n\r\n\r\n");
    mock.terminal.flushInput();

    mock.terminal.activateHintMode(
        vtbackend::HintModeRequest { .patterns = vtbackend::HintModeHandler::builtinPatterns(),
                                     .action = vtbackend::HintAction::Copy,
                                     .scope = vtbackend::HintScope::Scrollback,
                                     .scrollbackLimit = vtbackend::LineCount(-100) });

    // It did not crash; the negative limit clamped to "no history", so the scrolled-away URL is out
    // of range and nothing is offered.
    CHECK(mock.terminal.isHintModeActive());
    CHECK(mock.terminal.hintMatches().empty());
}

TEST_CASE("Terminal.hint_mode_matches_track_content_scrolling", "[terminal][hintmode]")
{
    // A hint session's positions are grid-absolute. When new output scrolls the grid while the
    // session is open, each match must move up with its own text rather than staying at a now-stale
    // row that other text has scrolled into.
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(40) }, LineCount(50) };
    mock.writeToScreen("https://tracked.example\r\n");
    mock.terminal.flushInput();

    mock.terminal.activateHintMode(
        vtbackend::HintModeRequest { .patterns = vtbackend::HintModeHandler::builtinPatterns(),
                                     .action = vtbackend::HintAction::Copy,
                                     .scope = vtbackend::HintScope::Scrollback,
                                     .scrollbackLimit = vtbackend::LineCount(1000) });
    REQUIRE(mock.terminal.hintMatches().size() == 1);
    auto const before = mock.terminal.hintMatches().at(0);

    // Three more lines scroll the grid up by two; the match keeps its label and text but moves up.
    // Each line goes in its own write so a line-break escape ends its literal rather than gluing to
    // the next word (which check-spelling would read as a nonsense token).
    mock.writeToScreen("more\r\n");
    mock.writeToScreen("lines\r\n");
    mock.writeToScreen("here\r\n");
    mock.terminal.flushInput();

    REQUIRE(mock.terminal.hintMatches().size() == 1);
    auto const after = mock.terminal.hintMatches().at(0);
    CHECK(after.matchedText == before.matchedText);
    CHECK(after.label == before.label);
    CHECK(after.start.line < before.start.line); // followed its text up into history
}

TEST_CASE("Terminal.hint_mode_overlay_wraps_a_label_past_the_row_edge", "[terminal][hintmode]")
{
    // 27 matches force two-character labels. One URL starts at the very last column, so its label
    // cannot fit on that row: the second character must appear on the wrapped continuation row
    // rather than being silently dropped.
    auto urlPatterns = std::vector<vtbackend::HintPattern>();
    for (auto& pattern: vtbackend::HintModeHandler::builtinPatterns())
        if (pattern.name == "url")
            urlPatterns.push_back(std::move(pattern));

    auto mock = MockTerm { PageSize { LineCount(30), ColumnCount(20) }, LineCount(50) };
    for (auto const i: std::views::iota(0, 26))
        mock.writeToScreen(std::format("https://s{}.io\r\n", i));
    // 19 blank filler columns then the URL, so its match starts at column 19 (the last column).
    mock.writeToScreen(std::string(19, ' ') + "https://edge.example");
    mock.terminal.flushInput();

    mock.terminal.activateHintMode(vtbackend::HintModeRequest { .patterns = std::move(urlPatterns),
                                                                .action = vtbackend::HintAction::Copy });

    auto const& matches = mock.terminal.hintMatches();
    REQUIRE(matches.size() == 27);
    auto const edge =
        std::ranges::find_if(matches, [](auto const& m) { return m.matchedText == "https://edge.example"; });
    REQUIRE(edge != matches.end());
    REQUIRE(edge->label.size() == 2);
    REQUIRE(edge->start.column == ColumnOffset(19)); // the last column of a 20-wide page

    mock.terminal.refreshRenderBuffer();
    auto const buf = mock.terminal.renderBuffer();
    auto const codepointsAt = [&](CellLocation pos) -> std::optional<std::u32string> {
        auto const it =
            std::ranges::find_if(buf.get().cells, [&](auto const& cell) { return cell.position == pos; });
        if (it == buf.get().cells.end())
            return std::nullopt;
        return it->codepoints;
    };

    // First label character on the start cell, second wrapped onto the next row's first column.
    auto const wrapped = CellLocation { .line = edge->start.line + LineOffset(1), .column = ColumnOffset(0) };
    CHECK(codepointsAt(edge->start) == std::u32string(1, static_cast<char32_t>(edge->label[0])));
    CHECK(codepointsAt(wrapped) == std::u32string(1, static_cast<char32_t>(edge->label[1])));
}

TEST_CASE("Terminal reports the identity its settings named", "[terminal]")
{
    // Settings::terminalId used to reach nothing: _terminalId was default-initialized to VT525 and
    // written only by setTerminalId(), which the GUI calls from configureTerminal(). Any terminal
    // nobody reconfigures after construction -- every daemon-hosted session, and vtconformance's --
    // therefore reported VT525 whatever its profile said, and DA1/DA2/DECSCL answered accordingly.
    auto events = vtbackend::Terminal::NullEvents {};
    auto const pageSize = vtbackend::PageSize { vtbackend::LineCount(5), vtbackend::ColumnCount(20) };

    auto settings = vtbackend::Settings {};
    settings.pageSize = pageSize;
    settings.terminalId = vtbackend::VTType::VT340;

    auto const environment = crispy::testing::FakeEnvironment {};
    auto terminal = vtbackend::Terminal { events,
                                          environment,
                                          std::make_unique<vtpty::MockPty>(pageSize),
                                          settings,
                                          std::chrono::steady_clock::now() };

    CHECK(terminal.terminalId() == vtbackend::VTType::VT340);
    // The operating level follows the identity at birth, as setTerminalId() also makes it do -- a
    // terminal must not start out claiming conformance above the model it reports.
    CHECK(terminal.operatingLevel() == vtbackend::VTType::VT340);
    CHECK(vtbackend::conformanceLevelOf(terminal.operatingLevel()) == 3);
}

TEST_CASE("a terminal constructed below VT525 narrows its sequence table too", "[terminal][conformance]")
{
    // Seeding _operatingLevel from Settings::terminalId without ALSO resetting the dispatch table
    // leaves the conformance gate and the table disagreeing for any terminal nobody reconfigures
    // afterwards — which is every daemon-hosted session (vthost::SessionHost builds each Terminal
    // straight from the profile's settings and never calls setTerminalId, unlike the GUI's
    // configureTerminal()). Such a session answered DA1 as a VT220 and refused the VT420 DEC modes,
    // yet still executed VT420-only sequences off the full VT525 table.
    auto const hasFunction = [](vtbackend::Terminal const& terminal, vtbackend::Function const& wanted) {
        return std::ranges::any_of(terminal.activeSequences(),
                                   [&](vtbackend::Function const& fn) { return fn.id() == wanted.id(); });
    };

    auto events = vtbackend::Terminal::NullEvents {};
    auto const pageSize = vtbackend::PageSize { vtbackend::LineCount(5), vtbackend::ColumnCount(20) };

    auto const environment = crispy::testing::FakeEnvironment {};
    auto makeTerminal = [&](vtbackend::VTType id) {
        auto settings = vtbackend::Settings {};
        settings.pageSize = pageSize;
        settings.terminalId = id;
        return vtbackend::Terminal { events,
                                     environment,
                                     std::make_unique<vtpty::MockPty>(pageSize),
                                     settings,
                                     std::chrono::steady_clock::time_point() };
    };

    SECTION("a VT220 identity excludes the VT420-only sequences")
    {
        auto terminal = makeTerminal(vtbackend::VTType::VT220);
        CHECK(terminal.operatingLevel() == vtbackend::VTType::VT220);
        CHECK_FALSE(hasFunction(terminal, vtbackend::DECFRA)); // VT420
        CHECK(hasFunction(terminal, vtbackend::DECSCL));       // never gated: it SETS the level
        // The table the constructor produced must be the one setTerminalId would have produced —
        // that equality is the whole invariant, and what an explicit call used to be needed for.
        auto const activeCount = terminal.activeSequences().size();
        terminal.setTerminalId(vtbackend::VTType::VT220);
        CHECK(terminal.activeSequences().size() == activeCount);
    }

    SECTION("the default identity keeps the full table")
    {
        auto terminal = makeTerminal(vtbackend::Settings {}.terminalId);
        CHECK(hasFunction(terminal, vtbackend::DECFRA));
    }
}

// NOLINTEND(misc-const-correctness)

// {{{ block-atomic history eviction: the consumers a shrinking history reaches (issue #836)
namespace
{
/// Writes one OSC 133 prompt-marked command whose output is @p outputLines lines long.
void writeCommandBlock(MockTerm<vtpty::MockPty>& mock, int index, int outputLines)
{
    mock.writeToScreen(std::format("\033]133;A\033\\P{}\r\n", index));
    mock.writeToScreen("\033]133;C\033\\");
    for (auto const i: std::views::iota(0, outputLines))
        mock.writeToScreen(std::format("o{}{}\r\n", index, i));
    mock.writeToScreen("\033]133;D;0\033\\");
}

/// Writes the command blocks numbered [@p first, @p last), each with @p outputLines of output.
void writeCommandBlocks(MockTerm<vtpty::MockPty>& mock, int first, int last, int outputLines)
{
    for (auto const block: std::views::iota(first, last))
        writeCommandBlock(mock, block, outputLines);
}

/// The headroom the eviction tests share: four guaranteed rows, twelve at most.
constexpr auto TestHistoryLimits =
    vtbackend::HistoryLimits { .guaranteed = LineCount(4), .capacity = LineCount(12) };
} // namespace

TEST_CASE("Terminal.historyEviction.clampsAViewportScrolledIntoTheEvictedBlock",
          "[terminal][history-eviction]")
{
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(2), ColumnCount(8) }, TestHistoryLimits };
    writeCommandBlocks(mock, 0, 3, 3);

    // Scroll all the way up, so the viewport sits on rows the next trim is about to take.
    mock.terminal.viewport().scrollToTop();
    REQUIRE(mock.terminal.viewport().scrolled());

    // The offset must never name more history than there is: Grid::render asserts exactly this, and
    // in a release build the ring would wrap modulo its size onto unrelated rows instead. Checked
    // after every block, because a dangling offset is transient -- the buffer grows back past it.
    for (auto const block: std::views::iota(3, 8))
    {
        writeCommandBlock(mock, block, 3);
        CHECK(unbox<int>(mock.terminal.viewport().scrollOffset())
              <= unbox<int>(mock.terminal.primaryScreen().historyLineCount()));
    }
}

TEST_CASE("Terminal.historyEviction.clampsAViewportWhenAPartialRegionFeedsTheScrollback",
          "[terminal][history-eviction]")
{
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(4), ColumnCount(8) }, TestHistoryLimits };
    writeCommandBlocks(mock, 0, 3, 3);

    mock.terminal.viewport().scrollToTop();
    REQUIRE(mock.terminal.viewport().scrolled());

    // A TOP-ANCHORED partial DECSTBM region still feeds rows into the scrollback -- Grid routes it
    // through the very same scrollUp -- but the boundary Screen reports keeps it inert, because
    // the live area below the region did not move. So the one path that clamps the viewport on an
    // ordinary scroll is not reached here, and the trim's floor jump goes unnoticed.
    mock.writeToScreen("\033[1;3r");

    // Checked after every write, not just at the end: the buffer oscillates between the guarantee
    // and the capacity, so an offset left dangling by a trim is back in range by the time the next
    // few lines have been written. It is the moment right after a trim that renders wrongly.
    for (auto const i: std::views::iota(0, 40))
    {
        mock.writeToScreen(std::format("x{}\r\n", i % 10));
        CHECK(unbox<int>(mock.terminal.viewport().scrollOffset())
              <= unbox<int>(mock.terminal.primaryScreen().historyLineCount()));
    }
}

TEST_CASE("Terminal.historyEviction.foldRangesForgetAnEvictedBlock", "[terminal][history-eviction]")
{
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(2), ColumnCount(8) }, TestHistoryLimits };
    writeCommandBlocks(mock, 0, 3, 3);

    // Populate the fold cache while the first block is still there.
    auto const floorBefore = mock.terminal.primaryScreen().grid().stableRangeFloor();
    REQUIRE(!mock.terminal.foldRanges().empty());

    writeCommandBlocks(mock, 3, 8, 3);

    REQUIRE(mock.terminal.primaryScreen().grid().stableRangeFloor() > floorBefore);

    // A trim moves neither the generation nor the stable base nor the mark revision, so a fold cache
    // keyed on those alone would go on serving ranges whose head has been evicted.
    auto const floor = mock.terminal.primaryScreen().grid().stableRangeFloor();
    for (auto const& range: mock.terminal.foldRanges())
        CHECK(range.firstStableId >= floor);
}

TEST_CASE("Terminal.historyEviction.keepsTheViCursorInsideTheAddressableGrid", "[terminal][history-eviction]")
{
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(2), ColumnCount(8) }, TestHistoryLimits };
    writeCommandBlocks(mock, 0, 3, 3);

    mock.terminal.inputHandler().setMode(vtbackend::ViMode::Normal);
    mock.terminal.moveNormalModeCursorTo(CellLocation {
        .line = LineOffset::cast_from(-unbox<int>(mock.terminal.primaryScreen().historyLineCount())),
        .column = ColumnOffset(0) });

    writeCommandBlocks(mock, 3, 8, 3);

    auto const top = -unbox<int>(mock.terminal.primaryScreen().historyLineCount());
    CHECK(unbox<int>(mock.terminal.normalModeCursorPosition().line) >= top);
}
// }}}

TEST_CASE("Terminal.historyEviction.theOldestScrollbackLineIsAlwaysAPrompt", "[terminal][history-eviction]")
{
    // What the issue actually asks for, driven end to end through real OSC 133 sequences.
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(3), ColumnCount(10) },
                                           vtbackend::HistoryLimits { LineCount(10), LineCount(24) } };

    auto const outputLengths = std::array { 4, 9, 2, 7, 3, 11, 5, 6 };
    for (auto const [block, length]: crispy::views::enumerate(outputLengths))
    {
        writeCommandBlock(mock, static_cast<int>(block), length);

        auto const& grid = mock.terminal.primaryScreen().grid();
        if (grid.historyLineCount() < LineCount(10))
            continue; // not deep enough yet for anything to have been evicted

        // The oldest line still addressable is a prompt, with its command's output whole beneath it.
        auto const top = grid.addressableTop();
        UNSCOPED_INFO(std::format("block {}: top={} text=\"{}\" history={}",
                                  block,
                                  top,
                                  grid.lineText(top),
                                  grid.historyLineCount()));
        CHECK(grid.lineAt(top).marked());
        CHECK(grid.lineText(top).starts_with("P"));
    }
}

TEST_CASE("Terminal.historyEviction.aShellWithoutOsc133SimplyBoundsAtTheHardLimit",
          "[terminal][history-eviction]")
{
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(3), ColumnCount(10) },
                                           vtbackend::HistoryLimits { LineCount(10), LineCount(24) } };

    for (auto const i: std::views::iota(0, 200))
        mock.writeToScreen(std::format("line{}\r\n", i));

    // No mark anywhere, so no boundary can be honoured: the ceiling is what bounds the buffer, and
    // eviction is line-wise exactly as it was before this existed.
    CHECK(mock.terminal.primaryScreen().historyLineCount() == LineCount(24));
    CHECK(mock.terminal.primaryScreen().grid().lineText(LineOffset(-24)) == "line174   ");
}
