// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/core/Primitives.hpp>
#include <vtbackend/links/LinkDetectionPolicy.hpp>
#include <vtbackend/links/LinkIndex.hpp>
#include <vtbackend/links/LinkPatterns.hpp>
#include <vtbackend/links/LinkTypes.hpp>
#include <vtbackend/render/RenderBuffer.hpp>
#include <vtbackend/screen/LinkDetector.hpp>
#include <vtbackend/screen/LinkPresentation.hpp>
#include <vtbackend/shell/CommandBlock.hpp>
#include <vtbackend/testing/DetectingTerm.hpp>
#include <vtbackend/testing/MockTerm.hpp>
#include <vtbackend/testing/TestHelpers.hpp>

#include <core/Utf8Path.hpp>
#include <core/async/ThreadPoolExecutor.hpp>
#include <core/platform/Clock.hpp>
#include <core/platform/testing/InMemoryFileSystem.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

using namespace vtbackend;
using namespace std::chrono_literals;
using vtbackend::test::renderedRow;

namespace
{
constexpr auto OnLink = CellLocation { .line = LineOffset(0), .column = ColumnOffset(1) };

#ifndef LIBTERMINAL_PASSIVE_RENDER_BUFFER_UPDATE
// What only the detection tests below use, compiled out with them in a passive build (see there).
using vtbackend::test::DetectingTerm;

    /// The directory the tests' files are in. Absolute here: on Windows a root directory without a drive is
    /// relative to the current drive.
    #ifdef _WIN32
constexpr auto Work = std::string_view { "C:/w" };
    #else
constexpr auto Work = std::string_view { "/w" };
    #endif

/// @return The file @p name in Work, as a program prints it.
[[nodiscard]] std::string inWork(std::string_view name)
{
    return std::string { Work } + "/" + std::string { name };
}

/// @return The path @p text names, which compares whatever this platform's separators.
[[nodiscard]] std::filesystem::path asPath(std::string_view text)
{
    return core::pathFromUtf8(text).value_or(std::filesystem::path {}).lexically_normal();
}

/// Records block @p id as having run in @p directory on this machine, and stamps @p rows of the primary
/// screen with it: what a daemon snapshot hands a mirror, so that no shell has to report the directory.
void stampBlock(MockTerm<>& mock, uint32_t id, std::string_view directory, std::initializer_list<int> rows)
{
    auto record = CommandBlockRecord {};
    record.id = CommandBlockId(id);
    record.state = CommandBlockState::Finished;
    record.workingDirectory =
        WorkingDirectorySnapshot { .path = std::string { directory }, .locality = ContextLocality::Local };
    mock.terminal.adoptCommandBlock(std::move(record), AdoptMode::Snapshot);
    for (auto const row: rows)
        mock.terminal.primaryScreen().grid().lineAt(LineOffset(row)).adoptBlock(CommandBlockId(id));
}

/// @return How often one frame of @p term asked for the machine's identity: once per working directory it
///         resolved.
[[nodiscard]] int identityQueriesOfAFrame(DetectingTerm& term)
{
    auto const before = term.mock.localIdentityQueries.load();
    term.frame();
    return term.mock.localIdentityQueries.load() - before;
}
#endif
} // namespace

TEST_CASE("Terminal.links.modifierModeRevealsWhileHeldAndForgetsOnFocusLoss", "[terminal][links]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(20) } };
    mock.writeToScreen("\033]8;;https://a.b/\033\\link\033]8;;\033\\");
    mock.terminal.setLinkDecorationMode(LinkDecorationMode::Modifier);
    CHECK(renderedRow(mock, LineOffset(0)).at(0).linkRole == LinkDecorationRole::None);

    CHECK(mock.terminal.updateHeldModifiers(Modifier::Control) == LinkRevealChange::Changed);
    CHECK(renderedRow(mock, LineOffset(0)).at(0).linkRole == LinkDecorationRole::Normal);

    // Focus lost with Ctrl down (Ctrl+Alt+Tab): the frontend reports nothing held.
    CHECK(mock.terminal.updateHeldModifiers(Modifiers {}) == LinkRevealChange::Changed);
    CHECK(renderedRow(mock, LineOffset(0)).at(0).linkRole == LinkDecorationRole::None);
}

TEST_CASE("Terminal.links.aHeldModifierThatChangesNothingReportsUnchanged", "[terminal][links]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(20) } };
    // Alt is not the follow modifier (Ctrl), and no application takes clicks.
    CHECK(mock.terminal.updateHeldModifiers(Modifier::Alt) == LinkRevealChange::Unchanged);
    CHECK(mock.terminal.followModifiers() == FollowModifiers::Released);
    CHECK(mock.terminal.updateHeldModifiers(Modifier::Control) == LinkRevealChange::Changed);
    CHECK(mock.terminal.followModifiers() == FollowModifiers::Held);
}

TEST_CASE("Terminal.links.aMouseEventCarriesTheHeldChord", "[terminal][links]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(20) } };
    mock.writeToScreen("\033]8;;https://a.b/\033\\link\033]8;;\033\\");
    mock.terminal.setLinkDecorationMode(LinkDecorationMode::Modifier);
    mock.terminal.sendMouseMoveEvent(Modifier::Control, OnLink, PixelCoordinate {}, false);
    CHECK(mock.terminal.followModifiers() == FollowModifiers::Held);
    CHECK(mock.terminal.linkPointerShape() == LinkPointerShape::Hand);
    mock.terminal.sendMouseMoveEvent(Modifier::None, OnLink, PixelCoordinate {}, false);
    CHECK(mock.terminal.followModifiers() == FollowModifiers::Released);
    CHECK(mock.terminal.linkPointerShape() == LinkPointerShape::Default);
}

TEST_CASE("Terminal.links.noFollowBindingNeverHoldsTheFollowModifiers", "[terminal][links]")
{
    auto settings =
        MockTerm<>::createSettings(PageSize { LineCount(3), ColumnCount(20) }, HistoryLimits {}, 1024);
    settings.linkFollowModifiers = std::nullopt;
    auto mock = MockTerm { std::move(settings) };
    CHECK(mock.terminal.updateHeldModifiers(Modifier::Control) == LinkRevealChange::Unchanged);
    CHECK(mock.terminal.followModifiers() == FollowModifiers::Released);
}

TEST_CASE("Terminal.links.anApplicationThatTakesClicksKeepsLinksAtRest", "[terminal][links]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(20) } };
    mock.writeToScreen("\033[?1000h\033]8;;https://a.b/\033\\link\033]8;;\033\\");
    mock.terminal.sendMouseMoveEvent(Modifier::None, OnLink, PixelCoordinate {}, false);
    CHECK(renderedRow(mock, LineOffset(0)).at(1).linkRole == LinkDecorationRole::Normal);
    CHECK(mock.terminal.linkPointerShape() == LinkPointerShape::Default);

    // The bypass modifier (Shift) hands the click back to the terminal.
    CHECK(mock.terminal.updateHeldModifiers(Modifier::Shift) == LinkRevealChange::Changed);
    CHECK(renderedRow(mock, LineOffset(0)).at(1).linkRole == LinkDecorationRole::Hover);
    CHECK(mock.terminal.linkPointerShape() == LinkPointerShape::Hand);
}

TEST_CASE("Terminal.links.applicationTakesClicksOnlyWhileItTracksTheMouseInInsertMode", "[terminal][links]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(20) } };
    CHECK_FALSE(mock.terminal.applicationTakesClicks());
    mock.writeToScreen("\033[?1000h");
    CHECK(mock.terminal.applicationTakesClicks());
    // Whatever modifiers are held: the bypass is the caller's question, not this one.
    (void) mock.terminal.updateHeldModifiers(Modifier::Shift);
    CHECK(mock.terminal.applicationTakesClicks());
    // Vi Normal mode keeps clicks in the terminal.
    mock.terminal.inputHandler().setMode(ViMode::Normal);
    CHECK_FALSE(mock.terminal.applicationTakesClicks());
    mock.terminal.inputHandler().setMode(ViMode::Insert);
    mock.writeToScreen("\033[?1000l");
    CHECK_FALSE(mock.terminal.applicationTakesClicks());
}

TEST_CASE("Terminal.links.linksAreRevealedByTheChordTheClickWouldMatch", "[terminal][links]")
{
    // The follow chord is Ctrl and the bypass modifier Shift (the defaults): a click matches its binding
    // after the bypass modifier is removed, and the reveal asks the same question.
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(20) } };

    SECTION("exactly the follow chord reveals")
    {
        (void) mock.terminal.updateHeldModifiers(Modifier::Control);
        CHECK(mock.terminal.followModifiers() == FollowModifiers::Held);
    }

    SECTION("more than the follow chord does not: the click would match another binding")
    {
        (void) mock.terminal.updateHeldModifiers(Modifiers { Modifier::Control } | Modifier::Alt);
        CHECK(mock.terminal.followModifiers() == FollowModifiers::Released);
    }

    SECTION("the bypass modifier does not count against it")
    {
        (void) mock.terminal.updateHeldModifiers(Modifiers { Modifier::Control } | Modifier::Shift);
        CHECK(mock.terminal.followModifiers() == FollowModifiers::Held);
    }

    SECTION("the bypass modifier alone is not the follow chord")
    {
        (void) mock.terminal.updateHeldModifiers(Modifier::Shift);
        CHECK(mock.terminal.followModifiers() == FollowModifiers::Released);
    }
}

#ifndef LIBTERMINAL_PASSIVE_RENDER_BUFFER_UPDATE
// A passive build fills on the parser thread, where detection is compiled out (Terminal::fillRenderFrame);
// these tests drive detection through the fill.

TEST_CASE("Terminal.links.detectsAUrlOnceOutputGoesQuiet", "[terminal][links]")
{
    auto term = DetectingTerm {};
    // The cursor hidden, so that neither its blink nor its motion asks for a frame: only detection may.
    term.mock.writeToScreen("\033[?25l");
    term.cycle();
    REQUIRE(term.mock.terminal.nextRender() == std::nullopt);
    term.mock.writeToScreen("see https://x.y/");
    term.frame();
    // The display asks when to draw next after every frame, and the quiet time before the scan is the answer.
    CHECK(term.mock.terminal.nextRender() == 100ms);
    term.mock.terminal.tick(term.mock.terminal.currentTime() + 100ms);
    term.frame();
    CHECK(term.match.pending() == 1);
    CHECK(term.targetAt(0, 4).empty()); // matched off the lock, not adopted yet
    term.match.drain();
    term.frame();
    CHECK(term.targetAt(0, 4) == "https://x.y/");
}

TEST_CASE("Terminal.links.withoutServicesNothingIsDetected", "[terminal][links]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(40) } };
    mock.writeToScreen("https://x.y/");
    mock.terminal.refreshRenderBuffer();
    auto const _ = std::scoped_lock { mock.terminal };
    CHECK(mock.terminal.linkDetector() == nullptr);
    CHECK(mock.terminal.detectedLinks() == nullptr);
}

TEST_CASE("Terminal.links.noWakeupForAScanWhileSynchronizedOutputHoldsTheFrame", "[terminal][links]")
{
    // While synchronized output suppresses the fill, no frame can run the scan: an overdue one would report
    // 0 ms frame after frame and spin the display. The end of the block refreshes it.
    auto term = DetectingTerm {};
    term.mock.writeToScreen("\033[?25lsee https://x.y/"); // no cursor to blink or move: only detection wakes
    term.frame();
    term.mock.terminal.tick(term.mock.terminal.currentTime() + 200ms);
    REQUIRE(term.mock.terminal.nextRender() == 0ms);
    term.mock.writeToScreen("\033[?2026h");
    CHECK(term.mock.terminal.nextRender() == std::nullopt);
}

TEST_CASE("Terminal.links.theAlternateScreenHasLinksOfItsOwn", "[terminal][links]")
{
    // Review Focus 4: the two screens' rows share stable ids. Between a switch and the next frame the index
    // still describes the screen left, and only its key keeps those links from answering here. Entering the
    // alternate screen clears it, which moves its ids on by a page: the primary screen scrolls by as much
    // first, so that the two row 0s share an id.
    auto term = DetectingTerm {};
    term.mock.writeToScreen("\n\n\n\n\n\n\n\n\n\033[H");
    term.print("https://primary.example/");
    REQUIRE(term.targetAt(0, 0) == "https://primary.example/");
    auto& terminal = term.mock.terminal;

    term.mock.writeToScreen("\033[?1049h\033[H");
    REQUIRE(terminal.displayedPage().grid().stableLineIdOf(LineOffset(0))
            == terminal.primaryScreen().grid().stableLineIdOf(LineOffset(0)));
    {
        auto const _ = std::scoped_lock { terminal };
        // No frame since the switch: the index is still the primary screen's.
        REQUIRE(terminal.linkDetector()->indexes(LinkIndexKey {
            .page = PageIndex(0), .generation = terminal.primaryScreen().grid().stableIdGeneration() }));
    }
    CHECK(term.targetAt(0, 0).empty());
    term.print("https://alternate.example/");
    CHECK(term.targetAt(0, 0) == "https://alternate.example/");

    term.mock.writeToScreen("\033[?1049l");
    CHECK(term.targetAt(0, 0).empty()); // the index is still the alternate screen's
    term.cycle();
    CHECK(term.targetAt(0, 0) == "https://primary.example/");
}

TEST_CASE("Terminal.links.everyPageHasLinksOfItsOwn", "[terminal][links]")
{
    // VT420 pages 2-15 are all ScreenType::Alternate and every page's rows count from the same stable ids: a
    // link indexed for one page and followed on another would open a target that is not on screen.
    auto term = DetectingTerm {};
    auto& terminal = term.mock.terminal;
    term.mock.writeToScreen("\033[2 P"); // PPA: page 2
    term.print("https://two.example/");
    REQUIRE(term.targetAt(0, 0) == "https://two.example/");

    term.mock.writeToScreen("\033[3 P\033[H"); // PPA: page 3
    term.mock.writeToScreen("https://three.example/");
    REQUIRE(terminal.displayedPageIndex() == PageIndex(2));
    REQUIRE(terminal.pageAt(PageIndex(1)).grid().stableLineIdOf(LineOffset(0))
            == terminal.pageAt(PageIndex(2)).grid().stableLineIdOf(LineOffset(0)));
    CHECK(term.targetAt(0, 0).empty()); // no frame since the switch: the index is page 2's
    term.cycle();
    CHECK(term.targetAt(0, 0) == "https://three.example/");

    SECTION("detection follows the page on display, not the cursor's")
    {
        // DECPCCM reset: the cursor goes back to page 2, and page 3 stays on display.
        term.mock.writeToScreen("\033[?64l\033[2 P\033[H");
        REQUIRE(terminal.displayedPageIndex() == PageIndex(2));
        REQUIRE(terminal.cursorPageIndex() == PageIndex(1));
        term.print("https://other.example/");
        CHECK(term.targetAt(0, 0) == "https://three.example/");
    }
}

TEST_CASE("Terminal.links.aNewPatternTableReplacesTheDetector", "[terminal][links]")
{
    auto term = DetectingTerm {};
    term.print("https://x.y/");
    REQUIRE(term.targetAt(0, 0) == "https://x.y/");
    auto const entries =
        std::array { LinkPatternSpec { .name = "url", .regex = {}, .activation = LinkActivation::HintOnly } };
    auto const table = std::make_shared<LinkPatterns const>(LinkPatterns::compile(entries));
    {
        auto const _ = std::scoped_lock { term.mock.terminal };
        term.mock.terminal.setLinkPatterns(table);
        REQUIRE(term.mock.terminal.linkDetector() != nullptr); // the file paths are still clickable
        CHECK(&term.mock.terminal.linkDetector()->patterns() == table.get());
    }
    term.cycle();
    CHECK(term.targetAt(0, 0).empty());
}

TEST_CASE("Terminal.links.aReloadWithTheSameTableKeepsTheLinks", "[terminal][links]")
{
    // A profile without patterns of its own applies the built-ins, the same table on every reload: replacing
    // the detector for it would drop the index and the probe cache, and the links until the next quiet time.
    auto term = DetectingTerm {};
    term.print("https://x.y/");
    REQUIRE(term.targetAt(0, 0) == "https://x.y/");
    {
        auto const _ = std::scoped_lock { term.mock.terminal };
        term.mock.terminal.setLinkPatterns(LinkPatterns::builtins());
    }
    CHECK(term.targetAt(0, 0) == "https://x.y/");
}

TEST_CASE("Terminal.links.aNewPatternTableForgetsTheHoveredLink", "[terminal][links]")
{
    // A new table means a new detector, whose link ids start over: the hovered id would name another link of
    // the new index, or none, and the pointer would show a hand over text no link is on.
    auto term = DetectingTerm {};
    term.print("see https://x.y/z");
    test::pointAt(term.mock, 0, 6);
    REQUIRE(term.mock.terminal.hoveringLinkId() != LinkId {});
    REQUIRE(term.mock.terminal.linkPointerShape() == LinkPointerShape::Hand);

    auto const entries =
        std::array { LinkPatternSpec { .name = "url", .regex = {}, .activation = LinkActivation::HintOnly } };
    auto const _ = std::scoped_lock { term.mock.terminal };
    term.mock.terminal.setLinkPatterns(std::make_shared<LinkPatterns const>(LinkPatterns::compile(entries)));
    CHECK(term.mock.terminal.hoveringLinkId() == LinkId {});
    CHECK(term.mock.terminal.linkPointerShape() == LinkPointerShape::Default);
}

TEST_CASE("Terminal.links.aTableWithNothingClickableDetectsNothing", "[terminal][links]")
{
    auto entries = std::vector<LinkPatternSpec> {};
    for (auto const name: builtinLinkPatternNames())
        entries.push_back(LinkPatternSpec {
            .name = std::string { name }, .regex = {}, .activation = LinkActivation::HintOnly });
    auto const table = std::make_shared<LinkPatterns const>(LinkPatterns::compile(entries));
    REQUIRE_FALSE(table->hasClickable());

    auto term = DetectingTerm {};
    term.print("https://x.y/");
    REQUIRE(term.targetAt(0, 0) == "https://x.y/");
    auto const _ = std::scoped_lock { term.mock.terminal };
    term.mock.terminal.setLinkPatterns(table);
    CHECK(term.mock.terminal.linkDetector() == nullptr);
    CHECK(term.mock.terminal.detectedLinks() == nullptr);
}

TEST_CASE("Terminal.links.aCrossfadeSnapshotStartsNoMatchJob", "[terminal][links]")
{
    // A page switch builds the outgoing page's snapshot for the crossfade on the parser thread. That is no
    // frame of the display's: detection must not run there, even with a scan due.
    auto term = DetectingTerm {};
    REQUIRE(term.mock.terminal.settings().screenTransitionStyle == ScreenTransitionStyle::Fade);
    term.mock.writeToScreen("see https://x.y/");
    term.frame();
    term.mock.terminal.tick(term.mock.terminal.currentTime() + 100ms); // a scan is due
    term.mock.writeToScreen("\033[2 P");                               // PPA: page 2, with a crossfade
    REQUIRE(term.mock.terminal.isScreenTransitionActive());
    CHECK(term.match.pending() == 0);
}

TEST_CASE("Terminal.links.aSynchronizedBlockEndingWhileTheDisplayPaintsIsScanned",
          "[terminal][links][threading]")
{
    // The block (mode 2026) ends while the display reads the front buffer, so the frame it ends with waits
    // for its swap, and the batch's screenUpdated() only retries the swap. The screen still changed: the next
    // frame must hear of it, or the block's links are never scanned.
    auto term = DetectingTerm {};
    term.cycle();                      // the empty screen scanned: nothing pending
    term.mock.steadyClock.advance(1s); // the block ends long after the last frame: no throttle
    {
        auto const painting = term.mock.terminal.renderBuffer();
        // On a thread of its own, as the parser's: the swap's try_lock must fail as it does against the
        // render thread, and a mutex may not be tried by the thread that holds it.
        std::thread { [&term] {
            term.mock.writeToScreen("\033[?2026hsee https://sync.example/\033[?2026l");
        } }.join();
        REQUIRE(term.mock.terminal.renderBufferState() == RenderBufferState::TrySwapBuffers);
    }
    term.cycle();
    CHECK(term.targetAt(0, 4) == "https://sync.example/");
}

TEST_CASE("Terminal.links.aScanResolvesTheWorkingDirectoryOnce", "[terminal][links]")
{
    // Every logical line a collect reads asks where its relative paths resolve, and the terminal's own
    // directory is the same for all of them: a collect resolves it once, however many rows it reads. Each
    // resolution asks for the machine's identity.
    auto const queriesOfTheFirstCollect = [](LineCount lines) {
        auto term = DetectingTerm { PageSize { lines, ColumnCount(40) } };
        term.mock.terminal.setCurrentWorkingDirectory(test::fileUrlOf(core::pathFromTrustedUtf8(Work)));
        term.frame();
        term.mock.terminal.tick(term.mock.terminal.currentTime() + 100ms);
        auto const collecting = identityQueriesOfAFrame(term); // every row: none was scanned yet
        REQUIRE(term.match.pending() == 1);
        term.match.drain();
        term.frame();
        return std::pair { collecting, identityQueriesOfAFrame(term) };
    };
    auto const [few, idle] = queriesOfTheFirstCollect(LineCount(3));
    auto const many = queriesOfTheFirstCollect(LineCount(12)).first;
    CHECK(few > idle);  // the collect resolved it
    CHECK(many == few); // and not once more for four times the rows
}

TEST_CASE("Terminal.links.eachRowsPathsResolveWhereItsCommandRan", "[terminal][links]")
{
    // One collect resolves each block's directories once, and the terminal's once: still, the rows of a
    // block resolve against that block's directory, and the rows of none against the terminal's.
    auto term = DetectingTerm { PageSize { LineCount(6), ColumnCount(40) } };
    term.files.addFile(core::pathFromTrustedUtf8(inWork("a/notes.txt")), "");
    term.files.addFile(core::pathFromTrustedUtf8(inWork("b/notes.txt")), "");
    term.mock.terminal.setCurrentWorkingDirectory(test::fileUrlOf(core::pathFromTrustedUtf8(inWork("a"))));
    term.mock.writeToScreen("notes.txt\r\nnotes.txt\r\nnotes.txt\r\nnotes.txt");
    stampBlock(term.mock, 7, inWork("b"), { 2, 3 });
    term.cycle();
    CHECK(asPath(term.targetAt(0, 0)) == asPath(inWork("a/notes.txt")));
    CHECK(asPath(term.targetAt(1, 0)) == asPath(inWork("a/notes.txt")));
    CHECK(asPath(term.targetAt(2, 0)) == asPath(inWork("b/notes.txt")));
    CHECK(asPath(term.targetAt(3, 0)) == asPath(inWork("b/notes.txt")));
}

TEST_CASE("Terminal.links.detectionRacesOutputReloadAndDestruction", "[terminal][links][threading]")
{
    // Run under the clang-tsan tree for the races. Real threads: two match, one probe. A writer thread stands
    // in for the parser thread and for the GUI thread's reload, as in Terminal_test.cpp's
    // readAheadKeepsTheScreenExactWhileRendering; this thread is the render thread, which reads nextRender()
    // without the terminal lock, as the display does.
    //
    // Declared before the executors, which drain what is queued as they are destroyed: a stopped
    // ThreadPoolExecutor runs what is submitted to it, and a probe job reads the filesystem and the clock.
    // Only the probe thread reads the filesystem, and nothing writes it once the threads run.
    auto files = core::platform::testing::InMemoryFileSystem {};
    files.addFile(core::pathFromTrustedUtf8(inWork("x.txt")), "");
    auto probeClock = core::platform::ManualClock {};
    auto match = core::async::ThreadPoolExecutor { 2 };
    auto probe = core::async::ThreadPoolExecutor { 1 };
    // The same patterns, as another table: the reload replaces the detector.
    auto const reloaded = std::make_shared<LinkPatterns const>(LinkPatterns::compile({}));

    auto mock = std::make_unique<MockTerm<>>(
        PageSize { LineCount(10), ColumnCount(60) },
        HistoryLimits {},
        1024,
        std::nullopt,
        &fixedOffsetLocalTime<>,
        LinkDetectionServices {
            .matchExecutor = match, .probeExecutor = probe, .fileSystem = files, .clock = probeClock });
    auto& terminal = mock->terminal;

    // The writer is the only thread that touches the MockPty: writeToScreen() appends to its buffer and
    // parses it on the writer's thread, as the parser thread reads its PTY, while this thread only takes the
    // terminal's lock to build frames. The redraw a match job asks for reaches MockPty::wakeupReader(), which
    // does nothing.
    auto written = std::atomic<bool> { false };
    auto writer = std::thread { [&] {
        for (auto const i: std::views::iota(0, 2000))
        {
            mock->writeToScreen(std::format("line {} https://x.y/{} {}\r\n", i, i, inWork("x.txt")));
            if (i == 1000)
            {
                auto const _ = std::scoped_lock { terminal };
                terminal.setLinkPatterns(reloaded);
            }
        }
        written.store(true, std::memory_order_release);
    } };
    // Paused between frames as a render thread waits for the next one: a thread that takes the lock straight
    // back keeps the writer waiting (pauseAfter()).
    for (auto const frame: std::views::iota(std::size_t { 1 }))
    {
        if (written.load(std::memory_order_acquire))
            break;
        terminal.tick(terminal.currentTime() + 50ms);
        terminal.refreshRenderBuffer();
        std::ignore = terminal.nextRender();
        test::pauseAfter(frame);
    }
    writer.join();

    // Engagement: frames until the page on display has a URL and a probed path, within a bound.
    auto const linksOnDisplay = [&] {
        auto kinds = std::pair { false, false }; // (a URL, a path)
        auto const _ = std::scoped_lock { terminal };
        auto const* index = terminal.detectedLinks();
        if (!index)
            return kinds;
        auto const& grid = terminal.displayedPage().grid();
        for (auto const row: std::views::iota(0, unbox<int>(grid.pageSize().lines)))
            for (auto const& span: index->onRow(grid.stableLineIdOf(LineOffset(row))))
                if (auto const* link = index->find(span.link))
                {
                    kinds.first = kinds.first || link->target.kind == LinkKind::Url;
                    kinds.second = kinds.second || link->target.kind == LinkKind::Path;
                }
        return kinds;
    };
    auto found = std::pair { false, false };
    for (auto const round: std::views::iota(std::size_t { 1 }, std::size_t { 200'000 }))
    {
        terminal.tick(terminal.currentTime() + 200ms);
        terminal.refreshRenderBuffer();
        found = linksOnDisplay();
        if (found.first && found.second)
            break;
        test::pauseAfter(round);
    }
    REQUIRE(found.first);
    REQUIRE(found.second);

    mock.reset(); // jobs may still be in flight: they finish against a detector that is gone
}

TEST_CASE("Terminal.links.aDetectedUrlIsDecoratedAtItsColumns", "[terminal][links]")
{
    auto term = DetectingTerm {};
    term.print("see https://x.y/z more");
    auto const cells = renderedRow(term.mock, LineOffset(0));
    CHECK(cells.at(0).linkRole == LinkDecorationRole::None);
    CHECK(cells.at(3).linkRole == LinkDecorationRole::None);
    CHECK(cells.at(4).linkRole == LinkDecorationRole::Normal);
    CHECK(cells.at(4).attributes.decorationColor
          == term.mock.terminal.colorPalette().hyperlinkDecoration.normal);
    CHECK(cells.at(16).linkRole == LinkDecorationRole::Normal);
    CHECK(cells.at(17).linkRole == LinkDecorationRole::None);
    CHECK(cells.at(18).linkRole == LinkDecorationRole::None);
}

TEST_CASE("Terminal.links.theStatusLineTakesNoDetectedLinkOfTheMainPage", "[terminal][links][statusline]")
{
    // Detection scans the page on display. A status line is a display of its own (DEC STD 070 ch. 14), so the
    // links found on the page decorate none of its cells: not those of its own row, whose coordinates the
    // page's row 0 shares, even where it holds the same text there.
    auto term = DetectingTerm {};
    auto& terminal = term.mock.terminal;
    auto const type = GENERATE(StatusDisplayType::Indicator, StatusDisplayType::HostWritable);
    INFO("status line: " << (type == StatusDisplayType::Indicator ? "indicator" : "host-writable"));
    switch (type)
    {
        case StatusDisplayType::Indicator: terminal.setStatusDisplay(StatusDisplayType::Indicator); break;
        case StatusDisplayType::HostWritable:
            term.mock.writeToScreen("\033[2$~"); // DECSSDT host-writable
            terminal.answerHostWritableStatusLineRequest(HostWritableStatusLineAnswer::Shown);
            term.mock.writeToScreen("\033[1$}see https://x.y/z more\033[0$}"); // DECSASD there and back
            break;
        default: FAIL("not a status line type the test covers");
    }
    REQUIRE(terminal.statusDisplayType() == type);
    term.print("\033[Hsee https://x.y/z more");

    terminal.refreshRenderBuffer();
    auto const buffer = terminal.renderBuffer();
    auto const& cells = buffer.get().cells;
    auto const statusRow = terminal.pageSize().lines.as<LineOffset>();
    // The page's link is decorated, so the frame had the links to hand: else this proves nothing.
    REQUIRE(std::ranges::any_of(cells, [](RenderCell const& cell) {
        return cell.position == CellLocation { .line = LineOffset(0), .column = ColumnOffset(4) }
               && cell.linkRole == LinkDecorationRole::Normal;
    }));
    CHECK(std::ranges::none_of(cells, [statusRow](RenderCell const& cell) {
        return cell.position.line == statusRow && cell.linkRole != LinkDecorationRole::None;
    }));
}

TEST_CASE("Terminal.links.hoveringAWrappedUrlHoversAllOfIt", "[terminal][links]")
{
    // The URL's first nine characters end row 0 (columns 11-19), the rest fill row 1.
    auto term = DetectingTerm { PageSize { LineCount(5), ColumnCount(20) } };
    term.print("aaaaaaaaaa https://example.com/long/path");
    CHECK(term.mock.terminal.linkPointerShape() == LinkPointerShape::Default);
    test::pointAt(term.mock, 1, 2);
    CHECK(renderedRow(term.mock, LineOffset(0)).at(10).linkRole == LinkDecorationRole::None);
    CHECK(renderedRow(term.mock, LineOffset(0)).at(11).linkRole == LinkDecorationRole::Hover);
    CHECK(renderedRow(term.mock, LineOffset(1)).at(2).linkRole == LinkDecorationRole::Hover);
    CHECK(term.mock.terminal.linkPointerShape() == LinkPointerShape::Hand);
    {
        auto const _ = std::scoped_lock { term.mock.terminal };
        auto const* const index = term.mock.terminal.detectedLinks();
        REQUIRE(index != nullptr);
        auto const* const hovered = index->find(term.mock.terminal.hoveringLinkId());
        REQUIRE(hovered != nullptr);
        CHECK(hovered->target.target == "https://example.com/long/path");
    }

    // Off the link: nothing hovered, and the pointer is the terminal's own again.
    test::pointAt(term.mock, 0, 2);
    CHECK(renderedRow(term.mock, LineOffset(0)).at(11).linkRole == LinkDecorationRole::Normal);
    CHECK(term.mock.terminal.linkPointerShape() == LinkPointerShape::Default);
    CHECK(term.mock.terminal.hoveringLinkId() == LinkId {});
}

TEST_CASE("Terminal.links.aLinkFoundUnderAStillPointerIsHovered", "[terminal][links]")
{
    // The pointer arrives before the scan: the frame that adopts the link finds it under the pointer, with no
    // pointer event, for the drawing and for the pointer shape alike.
    auto term = DetectingTerm {};
    term.mock.writeToScreen("see https://x.y/z");
    test::pointAt(term.mock, 0, 6);
    REQUIRE(term.mock.terminal.linkPointerShape() == LinkPointerShape::Default);
    term.cycle();
    CHECK(renderedRow(term.mock, LineOffset(0)).at(4).linkRole == LinkDecorationRole::Hover);
    CHECK(term.mock.terminal.linkPointerShape() == LinkPointerShape::Hand);
}

TEST_CASE("Terminal.links.aRowRedrawnEveryFrameIsNeverDecoratedAtStaleColumns", "[terminal][links]")
{
    // Review Focus 1: a progress bar sharing a row with a URL.
    auto term = DetectingTerm {};
    term.print(" 50% https://x.y/");
    REQUIRE(renderedRow(term.mock, LineOffset(0)).at(5).linkRole == LinkDecorationRole::Normal);
    for (auto const percent: { 60, 70, 80 })
    {
        term.mock.writeToScreen(std::format("\r{}% done, see https://x.y/", percent));
        auto const cells = renderedRow(term.mock, LineOffset(0));
        CHECK(std::ranges::none_of(
            cells, [](auto const& entry) { return entry.second.linkRole != LinkDecorationRole::None; }));
        term.mock.terminal.tick(term.mock.terminal.currentTime() + 30ms); // never quiet for the debounce
    }
    term.cycle();
    auto const cells = renderedRow(term.mock, LineOffset(0));
    CHECK(cells.at(14).linkRole == LinkDecorationRole::Normal); // "80% done, see " is 14 columns
    CHECK(cells.at(5).linkRole == LinkDecorationRole::None);
}

TEST_CASE("Terminal.links.anOsc8LinkAndADetectedOneEachKeepTheirOwnRole", "[terminal][links]")
{
    // The OSC 8 run looks like a URL itself; the scan does not read it, so its cells carry the OSC 8 link's
    // role only, and the plain URL beside it the detected one's.
    auto term = DetectingTerm { PageSize { LineCount(5), ColumnCount(60) } };
    term.print(
        "\033]8;;https://osc.example/\033\\https://inside.example/\033]8;;\033\\ https://plain.example/");
    // "https://inside.example/" is columns 0-22, "https://plain.example/" 24-45.
    auto cells = renderedRow(term.mock, LineOffset(0));
    REQUIRE(cells.at(0).linkRole == LinkDecorationRole::Normal);
    REQUIRE(cells.at(24).linkRole == LinkDecorationRole::Normal);

    test::pointAt(term.mock, 0, 3); // over the OSC 8 link
    cells = renderedRow(term.mock, LineOffset(0));
    CHECK(cells.at(0).linkRole == LinkDecorationRole::Hover);
    CHECK(cells.at(22).linkRole == LinkDecorationRole::Hover);
    CHECK(cells.at(23).linkRole == LinkDecorationRole::None);
    CHECK(cells.at(24).linkRole == LinkDecorationRole::Normal);
    CHECK(cells.at(45).linkRole == LinkDecorationRole::Normal);
    CHECK(term.mock.terminal.hoveringLinkId() == LinkId {}); // the OSC 8 link is no detected one

    test::pointAt(term.mock, 0, 30); // over the detected one
    cells = renderedRow(term.mock, LineOffset(0));
    CHECK(cells.at(0).linkRole == LinkDecorationRole::Normal);
    CHECK(cells.at(22).linkRole == LinkDecorationRole::Normal);
    CHECK(cells.at(24).linkRole == LinkDecorationRole::Hover);
    CHECK(cells.at(45).linkRole == LinkDecorationRole::Hover);
}

TEST_CASE("Terminal.links.anEvictedOsc8LinkIsNoDetectedLinkEither", "[terminal][links]")
{
    // An OSC 8 link evicted from the cache opens nothing and is drawn as no link; its text is not scanned for
    // one either, even when it reads as a URL and the row is scanned again.
    auto term = DetectingTerm { PageSize { LineCount(5), ColumnCount(60) } };
    term.print(
        "\033]8;;https://osc.example/\033\\https://inside.example/\033]8;;\033\\ https://plain.example/");
    auto& cache = term.mock.terminal.hyperlinks().cache;
    auto const osc8 = term.mock.terminal.primaryScreen().hyperlinkIdAt(
        CellLocation { .line = LineOffset(0), .column = ColumnOffset(0) });
    REQUIRE(osc8 != HyperlinkId {});
    for (auto const index: std::views::iota(std::size_t { 0 }, cache.capacity() - cache.size() + 1))
        cache.emplace(HyperlinkId { static_cast<uint16_t>(10000 + index) },
                      std::make_shared<HyperlinkInfo>(HyperlinkInfo { .userId = {}, .uri = "x:" }));
    REQUIRE_FALSE(cache.contains(osc8));

    // Something else on the row changes, so that the row is scanned again.
    term.print("\033[1;50H!");
    auto const cells = renderedRow(term.mock, LineOffset(0));
    CHECK(cells.at(0).linkRole == LinkDecorationRole::None);
    CHECK(cells.at(22).linkRole == LinkDecorationRole::None);
    CHECK(cells.at(24).linkRole == LinkDecorationRole::Normal);
    CHECK(term.targetAt(0, 0).empty());
}

TEST_CASE("Terminal.links.linkFreeRowsStayOnTheTrivialPath", "[terminal][links]")
{
    auto term = DetectingTerm {};
    // The cursor ends on row 2, so rows 0 and 1 are decided by their links alone.
    term.print("https://x.y/\r\nplain text\r\n");
    term.mock.terminal.refreshRenderBuffer();
    auto const buffer = term.mock.terminal.renderBuffer();
    auto const isRow = [](LineOffset row) {
        return [row](RenderLine const& line) {
            return line.lineOffset == row;
        };
    };
    CHECK(std::ranges::none_of(buffer.get().lines,
                               isRow(LineOffset(0)))); // the link's row leaves the fast path
    CHECK(std::ranges::any_of(buffer.get().lines, isRow(LineOffset(1)))); // a link-free row stays on it
}

TEST_CASE("Terminal.links.aLinkAtRestThatDrawsNothingKeepsItsRowOnTheTrivialPath", "[terminal][links]")
{
    // Under hyperlink_decoration.mode hover a link at rest draws nothing, so its row costs no per-cell pass.
    auto term = DetectingTerm {};
    term.print("https://x.y/\r\n");
    term.mock.terminal.setLinkDecorationMode(LinkDecorationMode::Hover);
    term.mock.terminal.refreshRenderBuffer();
    {
        auto const buffer = term.mock.terminal.renderBuffer();
        CHECK(std::ranges::any_of(buffer.get().lines,
                                  [](RenderLine const& line) { return line.lineOffset == LineOffset(0); }));
    }
    test::pointAt(term.mock, 0, 3);
    CHECK(renderedRow(term.mock, LineOffset(0)).at(0).linkRole == LinkDecorationRole::Hover);
}

TEST_CASE("Terminal.links.theHoverFollowsTheContentUnderAStillPointer", "[terminal][links]")
{
    // A scroll puts another link under a pointer that has not moved; no pointer event says so, and the frame
    // asks anyway.
    auto term =
        DetectingTerm { PageSize { LineCount(3), ColumnCount(20) }, {}, HistoryLimits::plain(LineCount(10)) };
    term.print("h1\r\nh2\r\nhttps://a.example/\r\nhttps://b.example/\r\nplain");
    // On screen: A on row 0, B on row 1, "plain" on row 2. One row back into the history: A on row 1, B on 2.
    REQUIRE(term.mock.terminal.viewport().scrollUp(LineCount(1)));
    term.cycle();
    test::pointAt(term.mock, 1, 3);
    REQUIRE(renderedRow(term.mock, LineOffset(1)).at(0).linkRole == LinkDecorationRole::Hover);  // A
    REQUIRE(renderedRow(term.mock, LineOffset(2)).at(0).linkRole == LinkDecorationRole::Normal); // B

    // Back to the live page: B is under the pointer now, and A has moved up a row.
    REQUIRE(term.mock.terminal.viewport().scrollDown(LineCount(1)));
    CHECK(renderedRow(term.mock, LineOffset(0)).at(0).linkRole == LinkDecorationRole::Normal); // A
    CHECK(renderedRow(term.mock, LineOffset(1)).at(0).linkRole == LinkDecorationRole::Hover);  // B
}

TEST_CASE("Terminal.links.theHoverReadsThePageOnDisplay", "[terminal][links]")
{
    // DECPCCM reset: the cursor writes to page 3 while page 2 stays on display. The link under the pointer is
    // page 2's; the cursor's page holds other text in that row.
    auto term = DetectingTerm {};
    auto& terminal = term.mock.terminal;
    term.mock.writeToScreen("\033[2 P"); // PPA: page 2
    term.print("https://two.example/");
    term.mock.writeToScreen("\033[?64l\033[3 P\033[Hwritten on page three");
    REQUIRE(terminal.displayedPageIndex() == PageIndex(1));
    REQUIRE(terminal.cursorPageIndex() == PageIndex(2));
    term.cycle();
    test::pointAt(term.mock, 0, 3);
    CHECK(renderedRow(term.mock, LineOffset(0)).at(0).linkRole == LinkDecorationRole::Hover);
    CHECK(terminal.linkPointerShape() == LinkPointerShape::Hand);
    auto const _ = std::scoped_lock { terminal };
    auto const* link =
        terminal.detectedLinkAt(CellLocation { .line = LineOffset(0), .column = ColumnOffset(3) });
    REQUIRE(link != nullptr);
    CHECK(link->target.target == "https://two.example/");
}

TEST_CASE("Terminal.links.aRowWrittenSinceItsScanAnswersNoLink", "[terminal][links]")
{
    auto term = DetectingTerm {};
    term.print("see https://x.y/z");
    auto const at = CellLocation { .line = LineOffset(0), .column = ColumnOffset(6) };
    {
        auto const _ = std::scoped_lock { term.mock.terminal };
        REQUIRE(term.mock.terminal.detectedLinkAt(at) != nullptr);
    }
    term.mock.writeToScreen("\rtold");
    auto const _ = std::scoped_lock { term.mock.terminal };
    CHECK(term.mock.terminal.detectedLinkAt(at) == nullptr);
}

TEST_CASE("Terminal.links.theStickyHeadersCopyOfARowDecoratesItsDetectedLinks",
          "[terminal][links][stickyheader]")
{
    // The header copies the command row of a block scrolled out of view. Its links are at rest -- the header
    // is no hover target -- whether OSC 8 ones or detected.
    auto term = DetectingTerm { PageSize { LineCount(5), ColumnCount(40) },
                                {},
                                HistoryLimits::plain(LineCount(100)) };
    test::startCommand(term.mock, "curl https://x.y/");
    test::printLines(term.mock, "out", 8);
    REQUIRE(term.mock.terminal.viewport().scrollUp(LineCount(3)));
    term.cycle();
    auto const header = test::drawFrame(term.mock.terminal).stickyHeader;
    REQUIRE(header.has_value());
    // "$ curl https://x.y/": the URL is columns 7-18.
    REQUIRE(header->cells.size() == 19);
    CHECK(header->cells[6].linkRole == LinkDecorationRole::None);
    CHECK(header->cells[7].linkRole == LinkDecorationRole::Normal);
    CHECK(header->cells[18].linkRole == LinkDecorationRole::Normal);
}

TEST_CASE("Terminal.links.lookUpPrefersTheIndexAndFallsBackToTheRow", "[terminal][links]")
{
    // A row changed since its scan answers from what it holds now.
    auto term = DetectingTerm {};
    term.print("https://old.example/");
    auto const at = CellLocation { .line = LineOffset(0), .column = ColumnOffset(2) };
    {
        auto const _ = std::scoped_lock { term.mock.terminal };
        REQUIRE(term.mock.terminal.detectedLinkAt(at) != nullptr);
        auto const lookup = term.mock.terminal.lookUpLinkAt(at);
        REQUIRE(lookup.target);
        CHECK(lookup.target->target == "https://old.example/");
    }
    term.mock.writeToScreen("\rhttps://new.example/");
    auto const _ = std::scoped_lock { term.mock.terminal };
    REQUIRE(term.mock.terminal.detectedLinkAt(at) == nullptr);
    auto const lookup = term.mock.terminal.lookUpLinkAt(at);
    REQUIRE(lookup.target);
    CHECK(lookup.target->target == "https://new.example/");
}

TEST_CASE("Terminal.links.aLookupOnARowTheIndexHoldsAsItIsRunsNoMatch", "[terminal][links]")
{
    // The index answers for a row it holds at its current revision, links or none (§7.2): a click there runs
    // no pattern. An on-demand match would find the bare word "notes" a path and hand it to the probe, which
    // a key bound to FollowHyperlink would then claim the press for.
    auto term = DetectingTerm {};
    term.print("see notes here");
    auto const _ = std::scoped_lock { term.mock.terminal };
    REQUIRE(term.mock.terminal.detectedLinks() != nullptr);
    REQUIRE(isScannedAsItIs(
        *term.mock.terminal.detectedLinks(), term.mock.terminal.displayedPage().grid(), LineOffset(0)));
    auto const lookup =
        term.mock.terminal.lookUpLinkAt(CellLocation { .line = LineOffset(0), .column = ColumnOffset(5) });
    CHECK_FALSE(lookup.target);
    CHECK_FALSE(lookup.printedPath);
    CHECK_FALSE(lookup.pathToProbe);
    CHECK_FALSE(lookup.refusal);
}

TEST_CASE("Terminal.links.aClickLooksForAPathFoundButNotConfirmedYet", "[terminal][links]")
{
    // The index holds the row as it is, but a path found there waits for its probe, or the cap on what may
    // wait dropped it. A click is the user asking: the path is handed back to be probed, and opens once
    // found.
    auto chosen = LinkDetectorTuning {};
    chosen.maxAwaitingProbe = 1;
    auto term = DetectingTerm { PageSize { LineCount(3), ColumnCount(60) }, chosen };
    term.files.addFile(asPath(inWork("a/one.txt")), "");
    term.files.addFile(asPath(inWork("a/two.txt")), "");
    term.mock.terminal.setCurrentWorkingDirectory(test::fileUrlOf(asPath(inWork("a"))));
    term.mock.writeToScreen("one.txt two.txt");
    // The collect, its match, and the frame that adopts it: both paths queue, the probe job takes the newer,
    // and the cap drops the older. The probe does not answer yet.
    term.frame();
    term.mock.terminal.tick(term.mock.terminal.currentTime() + term.tuning.debounce);
    term.frame();
    term.match.drain();
    term.frame();
    REQUIRE(term.probe.pending() == 1);
    {
        auto const _ = std::scoped_lock { term.mock.terminal };
        REQUIRE(term.mock.terminal.detectedLinks() != nullptr);
        REQUIRE(isScannedAsItIs(
            *term.mock.terminal.detectedLinks(), term.mock.terminal.displayedPage().grid(), LineOffset(0)));
    }

    auto const followedAt = [&](int column) {
        test::pointAt(term.mock, 0, column);
        return test::followedPathUnderPointer(term.mock.terminal, term.files)
            .value_or(std::nullopt)
            .transform([](std::string const& path) { return asPath(path); });
    };
    CHECK(followedAt(9) == asPath(inWork("a/two.txt"))); // its probe pending
    CHECK(followedAt(2) == asPath(inWork("a/one.txt"))); // dropped by the cap
}

TEST_CASE("Terminal.links.theDroppedPathsAreBoundedPrunedAndClearedAsTheQueuedOnesAre", "[terminal][links]")
{
    // What the cap drops is kept for a click: as many as the cap at most, the oldest out; a row written since
    // its scan forgets its paths, and a page switch or a reset all of them, as the probe's queues do.
    auto chosen = LinkDetectorTuning {};
    chosen.maxAwaitingProbe = 1;
    auto term = DetectingTerm { PageSize { LineCount(3), ColumnCount(60) }, chosen };
    term.mock.terminal.setCurrentWorkingDirectory(test::fileUrlOf(asPath(inWork("a"))));
    // one.txt at columns 0-6, two.txt at 8-14, six.txt at 16-22.
    term.mock.writeToScreen("one.txt two.txt six.txt");
    auto const collectAndMatch = [&] {
        term.frame();
        term.mock.terminal.tick(term.mock.terminal.currentTime() + term.tuning.debounce);
        term.frame();
        term.match.drain();
        term.frame();
    };
    // All three queue: the probe job takes the newest, the cap drops the other two, and keeps the newer.
    collectAndMatch();
    auto const row = [&] {
        auto const _ = std::scoped_lock { term.mock.terminal };
        return term.mock.terminal.displayedPage().grid().stableLineIdOf(LineOffset(0));
    }();
    auto const unconfirmedAt = [&](int column) {
        auto const _ = std::scoped_lock { term.mock.terminal };
        auto const* detector = term.mock.terminal.linkDetector();
        return detector != nullptr && detector->unconfirmedPathAt(row, ColumnOffset(column)) != nullptr;
    };
    REQUIRE(unconfirmedAt(17));    // six.txt, handed to the probe
    REQUIRE(unconfirmedAt(9));     // two.txt, dropped, kept
    CHECK_FALSE(unconfirmedAt(1)); // one.txt, dropped, and out past the cap

    SECTION("a row written since forgets them once it is scanned again")
    {
        term.mock.writeToScreen("\r\033[2K");
        collectAndMatch();
        CHECK_FALSE(unconfirmedAt(9));
        CHECK_FALSE(unconfirmedAt(17));
    }

    SECTION("a page switch forgets them")
    {
        term.mock.writeToScreen("\033[?1049h");
        term.frame();
        CHECK_FALSE(unconfirmedAt(9));
        CHECK_FALSE(unconfirmedAt(17));
    }

    SECTION("a reset forgets them")
    {
        term.mock.writeToScreen("\033c");
        term.frame();
        CHECK_FALSE(unconfirmedAt(9));
        CHECK_FALSE(unconfirmedAt(17));
    }
}

TEST_CASE("Terminal.links.anUnconfirmedPathIsHandedBackToProbe", "[terminal][links]")
{
    // Printed a moment ago: neither the index nor the probe cache knows it, and the lookup does not look.
    auto term = DetectingTerm {};
    term.files.addFile(core::pathFromTrustedUtf8(inWork("new.txt")), "");
    term.mock.writeToScreen(inWork("new.txt"));
    auto const _ = std::scoped_lock { term.mock.terminal };
    auto const lookup =
        term.mock.terminal.lookUpLinkAt(CellLocation { .line = LineOffset(0), .column = ColumnOffset(3) });
    CHECK_FALSE(lookup.target);
    CHECK_FALSE(lookup.refusal);
    REQUIRE(lookup.pathToProbe);
    REQUIRE(lookup.pathToProbe->candidates.size() == 1);
    CHECK(lookup.pathToProbe->candidates.front().lexically_normal() == asPath(inWork("new.txt")));
    REQUIRE(lookup.printedPath);
    CHECK(lookup.printedPath->kind == LinkKind::Path);
    CHECK(lookup.printedPath->target == inWork("new.txt"));
    CHECK(term.probe.pending() == 0); // nothing was probed on its behalf
}

TEST_CASE("Terminal.links.aPathTheProbeConfirmedIsKnownWithoutLooking", "[terminal][links]")
{
    // On a row written since its scan the index answers nothing, but the probe's cache still knows the path.
    auto term = DetectingTerm {};
    term.files.addFile(core::pathFromTrustedUtf8(inWork("a/notes.txt")), "");
    term.mock.terminal.setCurrentWorkingDirectory(test::fileUrlOf(core::pathFromTrustedUtf8(inWork("a"))));
    term.print("notes.txt");
    term.mock.writeToScreen("\rnotes.txt ok");
    auto const _ = std::scoped_lock { term.mock.terminal };
    auto const at = CellLocation { .line = LineOffset(0), .column = ColumnOffset(2) };
    REQUIRE(term.mock.terminal.detectedLinkAt(at) == nullptr);
    auto const lookup = term.mock.terminal.lookUpLinkAt(at);
    REQUIRE(lookup.target);
    CHECK(lookup.target->kind == LinkKind::Path);
    CHECK(asPath(lookup.target->target) == asPath(inWork("a/notes.txt")));
    CHECK(lookup.target->pathKind == PathKind::File);
    CHECK(asPath(lookup.target->directory) == asPath(inWork("a")));
    CHECK(lookup.target->location == std::nullopt);
    CHECK_FALSE(lookup.pathToProbe);
}

TEST_CASE("Terminal.links.hintModeAsksTheCacheDetectionFilled", "[terminal][links][hintmode]")
{
    // One cache for the paths detection, a click and hint mode look up (§12): hint mode reads what the probe
    // confirmed, and looks itself only where the cache knows nothing. Shown by a file gone since its probe,
    // which the cache still knows within its age.
    auto term = DetectingTerm { PageSize { LineCount(3), ColumnCount(60) } };
    auto const file = asPath(inWork("cached.txt"));
    term.files.addFile(file, "");
    term.print("see " + inWork("cached.txt"));
    REQUIRE(asPath(term.targetAt(0, 5)) == file);
    REQUIRE(term.files.remove(file).value_or(false));

    term.mock.terminal.flushInput();
    term.mock.terminal.activateHintMode(HintModeRequest {
        .patterns =
            HintPatternSet { .table = LinkPatterns::builtins(), .names = { "filepath" }, .resolve = {} },
        .action = HintAction::Copy });
    auto const& matches = term.mock.terminal.hintMatches();
    REQUIRE(matches.size() == 1);
    CHECK(asPath(matches[0].matchedText) == file);
    CHECK(matches[0].target.pathKind == PathKind::File);
}

TEST_CASE("Terminal.links.aPathTheProbeFoundMissingIsNoLink", "[terminal][links]")
{
    auto term = DetectingTerm {};
    term.mock.terminal.setCurrentWorkingDirectory(test::fileUrlOf(core::pathFromTrustedUtf8(inWork("a"))));
    term.print("notes.txt");
    term.mock.writeToScreen("\rnotes.txt ok");
    auto const _ = std::scoped_lock { term.mock.terminal };
    auto const lookup =
        term.mock.terminal.lookUpLinkAt(CellLocation { .line = LineOffset(0), .column = ColumnOffset(2) });
    CHECK_FALSE(lookup.target);
    CHECK_FALSE(lookup.pathToProbe);
}

// {{{ per-screen link detection
namespace
{
/// @return @p yes as a detection switch.
[[nodiscard]] constexpr DetectLinks detectIf(bool yes) noexcept
{
    return yes ? DetectLinks::Yes : DetectLinks::No;
}

/// Applies @p policy to @p term's terminal, under its lock, as a profile reload does.
void applyPolicy(DetectingTerm& term, LinkDetectionPolicy const& policy)
{
    auto const _ = std::scoped_lock { term.mock.terminal };
    term.mock.terminal.setLinkDetectionPolicy(policy);
}

/// Makes Work @p term's working directory, holding notes.txt.
void provideNotes(DetectingTerm& term)
{
    term.files.addFile(core::pathFromTrustedUtf8(inWork("notes.txt")), "");
    term.mock.terminal.setCurrentWorkingDirectory(test::fileUrlOf(core::pathFromTrustedUtf8(Work)));
}

/// Writes @p text and runs a detection cycle up to the match's adoption, the probe job not run yet.
void printUpToTheProbe(DetectingTerm& term, std::string_view text)
{
    term.mock.writeToScreen(text);
    term.frame();
    term.mock.terminal.tick(term.mock.terminal.currentTime() + term.tuning.debounce);
    term.frame();
    term.match.drain();
    term.frame();
}

/// What the tests below print: a path at column 0, a URL at column 10.
constexpr auto PathAndUrl = std::string_view { "notes.txt https://x.y/" };

/// The page the tests below print on.
constexpr auto NotesPage = PageSize { LineCount(5), ColumnCount(60) };
} // namespace

TEST_CASE("Terminal.links.pathsAreNotDetectedOnTheAlternateScreenByDefault", "[terminal][links]")
{
    auto term = DetectingTerm { NotesPage };
    provideNotes(term);
    term.print(PathAndUrl);
    CHECK(asPath(term.targetAt(0, 0)) == asPath(inWork("notes.txt")));
    CHECK(term.targetAt(0, 10) == "https://x.y/");

    term.mock.writeToScreen("\033[?1049h\033[H");
    printUpToTheProbe(term, PathAndUrl);
    CHECK(term.probe.pending() == 0); // nothing was queued for probing
    term.probe.drain();
    term.frame();
    CHECK(term.targetAt(0, 0).empty());
    CHECK(term.targetAt(0, 10) == "https://x.y/");
}

TEST_CASE("Terminal.links.otherScreensIncludeEveryNonPrimaryPage", "[terminal][links]")
{
    auto term = DetectingTerm { NotesPage };
    provideNotes(term);
    term.mock.writeToScreen("\033[3 P\033[H"); // PPA: page 3
    REQUIRE(term.mock.terminal.displayedPageIndex() == PageIndex(2));
    printUpToTheProbe(term, PathAndUrl);
    CHECK(term.probe.pending() == 0); // the probe cache is cold: a path detected would be queued
    term.probe.drain();
    term.frame();
    CHECK(term.targetAt(0, 0).empty());
    CHECK(term.targetAt(0, 10) == "https://x.y/");
}

TEST_CASE("Terminal.links.eachSwitchGovernsItsScreenAndCategory", "[terminal][links]")
{
    auto const bits = GENERATE(range(0, 16));
    CAPTURE(bits);
    auto const policy = LinkDetectionPolicy {
        .primaryScreen = { .urls = detectIf((bits & 1) != 0), .filePaths = detectIf((bits & 2) != 0) },
        .otherScreens = { .urls = detectIf((bits & 4) != 0), .filePaths = detectIf((bits & 8) != 0) },
    };
    auto term = DetectingTerm { NotesPage };
    provideNotes(term);
    applyPolicy(term, policy);

    auto const check = [&](ScreenLinkDetection screen) {
        CHECK(term.targetAt(0, 0).empty() != screen.detects(LinkCategory::FilePath));
        CHECK(term.targetAt(0, 10).empty() != screen.detects(LinkCategory::Url));
    };
    term.print(PathAndUrl);
    check(policy.primaryScreen);
    term.mock.writeToScreen("\033[?1049h\033[H");
    term.print(PathAndUrl);
    check(policy.otherScreens);
}

TEST_CASE("Terminal.links.aScreenThatDetectsNothingAsksForNoWakeup", "[terminal][links]")
{
    // No collect is scheduled for a page with both switches off: output there arms no quiet-time wake-up.
    auto term = DetectingTerm { NotesPage };
    constexpr auto Off = ScreenLinkDetection { .urls = DetectLinks::No, .filePaths = DetectLinks::No };
    applyPolicy(term, LinkDetectionPolicy { .primaryScreen = {}, .otherScreens = Off });
    // The cursor hidden, so that neither its blink nor its motion asks for a frame: only detection may.
    term.mock.writeToScreen("\033[?25l\033[?1049h\033[H");
    term.cycle();
    // Past the switch's crossfade, which asks for frames of its own.
    term.mock.terminal.tick(term.mock.terminal.currentTime() + 1s);
    term.frame();
    REQUIRE(term.mock.terminal.nextRender() == std::nullopt);
    term.mock.writeToScreen("see https://x.y/");
    term.frame();
    CHECK(term.mock.terminal.nextRender() == std::nullopt);

    term.mock.writeToScreen("\033[?1049l"); // back to the primary screen, which detects
    term.frame();
    auto const _ = std::scoped_lock { term.mock.terminal };
    CHECK(term.mock.terminal.linkDetector()->nextDue(term.mock.terminal.currentTime()) == 100ms);
}

TEST_CASE("Terminal.links.aFileUrlFollowsTheFilePathSwitch", "[terminal][links]")
{
    // A `file://` URL names a file on this machine: as far as the switches go, it is a path.
    auto term = DetectingTerm { NotesPage };
    provideNotes(term);
    auto const url = test::fileUrlOf(core::pathFromTrustedUtf8(inWork("notes.txt")));
    term.print(url);
    REQUIRE_FALSE(term.targetAt(0, 2).empty());

    term.mock.writeToScreen("\033[?1049h\033[H");
    term.print(url);
    CHECK(term.targetAt(0, 2).empty());
    {
        // Explicitly: URLs on, paths off, and still no link.
        auto const _ = std::scoped_lock { term.mock.terminal };
        REQUIRE(term.mock.terminal.linkDetector()->policy().otherScreens
                == ScreenLinkDetection { .urls = DetectLinks::Yes, .filePaths = DetectLinks::No });
    }

    auto policy = LinkDetectionPolicy {};
    policy.otherScreens.filePaths = DetectLinks::Yes;
    applyPolicy(term, policy);
    term.cycle();
    CHECK_FALSE(term.targetAt(0, 2).empty());
}

TEST_CASE("Terminal.links.aCustomPatternOpeningAFileUrlFollowsTheFilePathSwitch", "[terminal][links]")
{
    // A user pattern whose `url:` expands to a `file:` URL opens a file on this machine: it is no URL as far
    // as the switches go, and under the defaults it is never probed on the alternate screen.
    auto term = DetectingTerm { NotesPage };
    provideNotes(term);
    auto const entries = std::array { LinkPatternSpec { .name = "note",
                                                        .regex = "NOTE:([^ ]+)",
                                                        .activation = LinkActivation::Clickable,
                                                        .urlTemplate = "file://{1}" } };
    auto const table = std::make_shared<LinkPatterns const>(LinkPatterns::compile(entries));
    {
        auto const _ = std::scoped_lock { term.mock.terminal };
        term.mock.terminal.setLinkPatterns(table);
    }
    auto const printed = "NOTE:" + inWork("notes.txt");

    term.mock.writeToScreen("\033[?1049h\033[H");
    printUpToTheProbe(term, printed);
    CHECK(term.probe.pending() == 0); // a path probed would be queued
    term.probe.drain();
    term.frame();
    CHECK(term.targetAt(0, 2).empty());

    auto policy = LinkDetectionPolicy {};
    policy.otherScreens.filePaths = DetectLinks::Yes;
    applyPolicy(term, policy);
    term.cycle();
    CHECK_FALSE(term.targetAt(0, 2).empty());
}

TEST_CASE("Terminal.links.aNewPolicyReplacesTheDetector", "[terminal][links]")
{
    // Review Focus 1: a live reload while the alternate screen is shown.
    auto term = DetectingTerm { NotesPage };
    provideNotes(term);
    term.mock.writeToScreen("\033[?1049h\033[H");
    term.print(PathAndUrl);
    REQUIRE(term.targetAt(0, 0).empty());
    test::pointAt(term.mock, 0, 12);
    REQUIRE(term.mock.terminal.hoveringLinkId() != LinkId {});

    auto policy = LinkDetectionPolicy {};
    {
        auto const _ = std::scoped_lock { term.mock.terminal };
        auto const* const before = term.mock.terminal.linkDetector();
        term.mock.terminal.setLinkDetectionPolicy(policy);
        CHECK(term.mock.terminal.linkDetector() == before); // an equal policy keeps the detector
        policy.otherScreens.filePaths = DetectLinks::Yes;
        term.mock.terminal.setLinkDetectionPolicy(policy);
        REQUIRE(term.mock.terminal.linkDetector() != nullptr);
        CHECK(term.mock.terminal.linkDetector() != before);
        CHECK(term.mock.terminal.linkDetector()->policy() == policy);
        CHECK(term.mock.terminal.hoveringLinkId() == LinkId {}); // the new detector's ids start over
    }
    term.cycle();
    CHECK(asPath(term.targetAt(0, 0)) == asPath(inWork("notes.txt")));
    CHECK(term.targetAt(0, 10) == "https://x.y/");
}

TEST_CASE("Terminal.links.withEverythingOffOnlyOsc8LinksRemain", "[terminal][links]")
{
    // Review Focus 3.
    auto term = DetectingTerm { NotesPage };
    constexpr auto Off = ScreenLinkDetection { .urls = DetectLinks::No, .filePaths = DetectLinks::No };
    applyPolicy(term, LinkDetectionPolicy { .primaryScreen = Off, .otherScreens = Off });
    // The URL at columns 0-11, the OSC 8 link's one cell at 13.
    term.print("https://x.y/ \033]8;;https://h/\033\\h\033]8;;\033\\");

    auto const _ = std::scoped_lock { term.mock.terminal };
    CHECK(term.mock.terminal.linkDetector() == nullptr);
    auto const url =
        term.mock.terminal.lookUpLinkAt(CellLocation { .line = LineOffset(0), .column = ColumnOffset(2) });
    CHECK_FALSE(url.target);
    CHECK_FALSE(url.hyperlink);
    auto const osc8 =
        term.mock.terminal.lookUpLinkAt(CellLocation { .line = LineOffset(0), .column = ColumnOffset(13) });
    REQUIRE(osc8.hyperlink);
    CHECK(osc8.hyperlink->uri == "https://h/");
}

TEST_CASE("Terminal.links.aLookupOnTheAlternateScreenFindsNoPathByDefault", "[terminal][links]")
{
    auto term = DetectingTerm { NotesPage };
    provideNotes(term);
    term.mock.writeToScreen("\033[?1049h\033[H");
    term.mock.writeToScreen(PathAndUrl); // no scan: the lookup matches the row itself
    auto const _ = std::scoped_lock { term.mock.terminal };
    auto const path =
        term.mock.terminal.lookUpLinkAt(CellLocation { .line = LineOffset(0), .column = ColumnOffset(2) });
    CHECK_FALSE(path.target);
    CHECK_FALSE(path.printedPath);
    CHECK_FALSE(path.pathToProbe);
    auto const url =
        term.mock.terminal.lookUpLinkAt(CellLocation { .line = LineOffset(0), .column = ColumnOffset(12) });
    REQUIRE(url.target);
    CHECK(url.target->target == "https://x.y/");
}

TEST_CASE("Terminal.links.hintModeStillOffersPathsOnTheAlternateScreen", "[terminal][links][hintmode]")
{
    // Hint mode is the user asking: the detection switches do not govern it.
    auto term = DetectingTerm { NotesPage };
    provideNotes(term);
    term.mock.writeToScreen("\033[?1049h\033[H");
    term.print("see " + inWork("notes.txt"));
    REQUIRE(term.targetAt(0, 5).empty()); // not detected, under the default policy

    term.mock.terminal.flushInput();
    term.mock.terminal.activateHintMode(HintModeRequest {
        .patterns =
            HintPatternSet { .table = LinkPatterns::builtins(), .names = { "filepath" }, .resolve = {} },
        .action = HintAction::Copy });
    CHECK(std::ranges::any_of(term.mock.terminal.hintMatches(), [](auto const& match) {
        return asPath(match.matchedText) == asPath(inWork("notes.txt"));
    }));
}
// }}}

#endif
