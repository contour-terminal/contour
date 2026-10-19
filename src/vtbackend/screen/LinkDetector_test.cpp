// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/links/LinkMatcher.hpp>
#include <vtbackend/links/LinkPatterns.hpp>
#include <vtbackend/screen/LinkDetector.hpp>
#include <vtbackend/testing/MockTerm.hpp>

#include <core/Utf8Path.hpp>
#include <core/async/testing/ManualExecutor.hpp>
#include <core/platform/Clock.hpp>
#include <core/platform/testing/InMemoryFileSystem.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef _WIN32
    #include <thread>
#else
    #include <pthread.h>
#endif

using namespace vtbackend;
using namespace std::chrono_literals;

namespace
{
/// The directory the tests' files are in. Absolute here: on Windows a root directory without a drive is
/// relative to the current drive.
#ifdef _WIN32
constexpr auto Work = std::string_view { "C:/w" };
#else
constexpr auto Work = std::string_view { "/w" };
#endif

/// @return The file @p name in Work, as a path, which compares whatever this platform's separators.
[[nodiscard]] std::filesystem::path inWork(std::string_view name)
{
    return (core::pathFromTrustedUtf8(Work) / name).lexically_normal();
}

/// @return The file @p name in Work, as a program prints it.
[[nodiscard]] std::string printedInWork(std::string_view name)
{
    return std::string { Work } + "/" + std::string { name };
}

/// The terminal as the detector sees it, over a MockTerm's primary screen, with path bases set by the test.
class FakeSource final: public LinkSource
{
  public:
    explicit FakeSource(MockTerm<>& mock): _mock { mock } {}
    [[nodiscard]] Grid& linkGrid() override { return _mock.terminal.primaryScreen().grid(); }
    [[nodiscard]] PageIndex linkPage() const override { return page; }
    [[nodiscard]] LineOffset viewportTop() const override { return LineOffset(0); }
    [[nodiscard]] std::expected<PathBases, PathRefusal> pathBasesAt(LineOffset /*row*/) const override
    {
        return bases;
    }
    [[nodiscard]] LinkMachine localMachine() const override
    {
        return LinkMachine { .home = home, .hostname = {} };
    }

    std::expected<PathBases, PathRefusal> bases = PathBases { std::string { Work } };
    std::string home = "/home/u";
    PageIndex page {}; ///< The page the grid stands for; the grid itself stays the primary screen's.

  private:
    MockTerm<>& _mock;
};

struct Fixture
{
    /// @param chosen The detector's tuning.
    /// @param page The page; the history holds four of them.
    explicit Fixture(LinkDetectorTuning chosen = {},
                     PageSize page = PageSize { LineCount(5), ColumnCount(40) }):
        tuning { chosen }, mock { page, LineCount(unbox(page.lines) * 4) }
    {
    }

    LinkDetectorTuning tuning;
    core::async::testing::ManualExecutor match;
    core::async::testing::ManualExecutor probe;
    core::platform::testing::InMemoryFileSystem files;
    core::platform::ManualClock clock;
    MockTerm<> mock;
    FakeSource source { mock };
    int redraws = 0;
    std::unique_ptr<LinkDetector> detector =
        std::make_unique<LinkDetector>(LinkDetectionServices { .matchExecutor = match,
                                                               .probeExecutor = probe,
                                                               .fileSystem = files,
                                                               .clock = clock,
                                                               .tuning = tuning },
                                       LinkPatterns::builtins(),
                                       [this] { ++redraws; });
    std::chrono::steady_clock::time_point now {};

    LinkIndexChange frame(FrameChange change = FrameChange::Unchanged)
    {
        return detector->onFrame(source, now, change);
    }

    /// Runs one whole detection cycle for output just written.
    void detect()
    {
        (void) frame(FrameChange::ScreenChanged);
        now += 100ms;
        (void) frame();
        match.drain();
        (void) frame();
    }

    [[nodiscard]] DetectedLink const* linkAt(int line, int column)
    {
        return detector->index().at(source.linkGrid().stableLineIdOf(LineOffset(line)), ColumnOffset(column));
    }

    [[nodiscard]] std::string targetAt(int line, int column)
    {
        auto const* link = linkAt(line, column);
        return link ? link->target.target : std::string {};
    }

    /// The target of a path's link, as a path; empty where no link is.
    [[nodiscard]] std::filesystem::path targetPathAt(int line, int column)
    {
        return core::pathFromUtf8(targetAt(line, column)).value_or(std::filesystem::path {});
    }
};
} // namespace

TEST_CASE("LinkDetector.streamingOutputIsNotScannedIdleOutputOnce", "[links]")
{
    auto fx = Fixture {};
    fx.mock.writeToScreen("see https://x.y/");
    (void) fx.frame(FrameChange::ScreenChanged);
    fx.now += 50ms;
    (void) fx.frame(FrameChange::ScreenChanged);
    fx.now += 90ms;
    (void) fx.frame(FrameChange::ScreenChanged);
    CHECK(fx.match.pending() == 0); // output kept coming: debounced
    CHECK(fx.detector->nextDue(fx.now) == 100ms);

    fx.now += 100ms;
    (void) fx.frame();
    CHECK(fx.match.pending() == 1);
    CHECK(fx.detector->nextDue(fx.now) == std::nullopt); // in flight: the job's redraw wakes the display

    fx.match.drain();
    CHECK(fx.redraws == 1);
    CHECK(fx.frame() == LinkIndexChange::Changed);
    CHECK(fx.targetAt(0, 4) == "https://x.y/");

    fx.now += 1s;
    (void) fx.frame();
    CHECK(fx.match.pending() == 0); // nothing changed: nothing scanned
    CHECK(fx.detector->nextDue(fx.now) == std::nullopt);
}

TEST_CASE("LinkDetector.continuousOutputIsScannedAfterTheMaxWait", "[links]")
{
    auto fx = Fixture {};
    for ([[maybe_unused]] auto const step: std::views::iota(0, 30))
    {
        fx.mock.writeToScreen("x");
        (void) fx.frame(FrameChange::ScreenChanged);
        if (fx.match.pending() > 0)
            break;
        fx.now += 50ms;
    }
    CHECK(fx.match.pending() == 1);
    CHECK(fx.now - std::chrono::steady_clock::time_point {} == 1000ms);
}

TEST_CASE("LinkDetector.aLineThatChangedAfterCollectionIsDroppedAndRescanned", "[links]")
{
    auto fx = Fixture {};
    fx.mock.writeToScreen("https://old.example/");
    (void) fx.frame(FrameChange::ScreenChanged);
    fx.now += 100ms;
    (void) fx.frame(); // collected, job pending
    fx.mock.writeToScreen("\rhttps://new.example/");
    fx.match.drain();
    (void) fx.frame();
    CHECK(fx.targetAt(0, 0).empty()); // stale: not published at the old text
    fx.now += 100ms;
    (void) fx.frame();
    fx.match.drain();
    (void) fx.frame();
    CHECK(fx.targetAt(0, 0) == "https://new.example/");
}

TEST_CASE("LinkDetector.aReflowDuringTheMatchDropsItsResults", "[links]")
{
    auto fx = Fixture {};
    fx.mock.writeToScreen("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa https://x.y/z");
    (void) fx.frame(FrameChange::ScreenChanged);
    fx.now += 100ms;
    (void) fx.frame();
    REQUIRE(fx.match.pending() == 1);
    fx.mock.terminal.resizeScreen(PageSize { LineCount(5), ColumnCount(30) }); // reflows: new row ids
    fx.match.drain();
    (void) fx.frame();
    CHECK(fx.detector->index().empty());
    fx.now += 100ms;
    (void) fx.frame();
    fx.match.drain();
    (void) fx.frame();
    CHECK(fx.targetAt(1, 6) == "https://x.y/z"); // wrapped at 30 columns now
}

TEST_CASE("LinkDetector.everyPageHasAnIndexOfItsOwn", "[links]")
{
    // VT420 pages 2-15 and the alternate screen are all ScreenType::Alternate, and every page's grid numbers
    // its rows from the same ids: only the page tells whose rows an index names.
    auto fx = Fixture {};
    fx.source.page = PageIndex(2);
    fx.mock.writeToScreen("see https://x.y/");
    fx.detect();
    REQUIRE(fx.targetAt(0, 4) == "https://x.y/");

    SECTION("a switch starts over")
    {
        fx.source.page = AlternateScreenPageIndex; // the same rows, as another page's
        CHECK(fx.frame() == LinkIndexChange::Changed);
        CHECK(fx.detector->index().empty());
        CHECK(fx.detector->indexedScreen() == ScreenType::Alternate);
        CHECK(fx.detector->indexes(LinkIndexKey { .page = AlternateScreenPageIndex,
                                                  .generation = fx.source.linkGrid().stableIdGeneration() }));
    }

    SECTION("a scan of the page left is not adopted for the page shown")
    {
        fx.mock.writeToScreen(" https://z.w/");
        (void) fx.frame(FrameChange::ScreenChanged);
        fx.now += 100ms;
        (void) fx.frame(); // collected on page 3
        REQUIRE(fx.match.pending() == 1);
        fx.source.page = AlternateScreenPageIndex;
        (void) fx.frame(); // starts over
        fx.match.drain();
        (void) fx.frame();
        CHECK(fx.detector->index().empty());
    }
}

TEST_CASE("LinkDetector.pathsAreDecoratedOnceTheProbeConfirmsThem", "[links]")
{
    auto fx = Fixture {};
    fx.files.addFile(inWork("README.md"), "");
    fx.mock.writeToScreen("README.md missing.txt");
    fx.detect();
    CHECK(fx.targetAt(0, 0).empty()); // not yet probed
    CHECK(fx.probe.pending() == 1);
    fx.probe.drain();
    CHECK(fx.frame() == LinkIndexChange::Changed);
    CHECK(fx.targetPathAt(0, 0) == inWork("README.md"));
    REQUIRE(fx.linkAt(0, 0) != nullptr);
    CHECK(fx.linkAt(0, 0)->target.pathKind == PathKind::File);
    CHECK(fx.linkAt(0, 0)->target.directory == Work);
    CHECK(fx.targetAt(0, 12).empty());
}

TEST_CASE("LinkDetector.aPathNamesTheBaseItWasFoundInWhenAnEarlierOneWasSkipped", "[links]")
{
    // An empty base yields no candidate, so the answer's index is not the base's.
    auto fx = Fixture {};
    fx.files.addFile(inWork("README.md"), "");
    fx.source.bases = PathBases { "", std::string { Work } };
    fx.mock.writeToScreen("README.md");
    fx.detect();
    fx.probe.drain();
    (void) fx.frame();
    REQUIRE(fx.linkAt(0, 0) != nullptr);
    CHECK(fx.targetPathAt(0, 0) == inWork("README.md"));
    CHECK(fx.linkAt(0, 0)->target.directory == Work);
}

TEST_CASE("LinkDetector.aPathTheProbeAlreadyKnowsIsDecoratedWithTheScan", "[links]")
{
    auto fx = Fixture {};
    fx.files.addFile(inWork("README.md"), "");
    fx.mock.writeToScreen("README.md");
    fx.detect();
    fx.probe.drain();
    (void) fx.frame();
    REQUIRE(fx.targetPathAt(0, 0) == inWork("README.md"));

    // The same name printed again: the answer is cached, so no second probe is needed.
    fx.mock.writeToScreen("\r\nREADME.md");
    fx.detect();
    CHECK(fx.probe.pending() == 0);
    CHECK(fx.targetPathAt(1, 0) == inWork("README.md"));
}

TEST_CASE("LinkDetector.aRowPrintedElsewhereGetsNoPathsButKeepsItsUrls", "[links]")
{
    auto fx = Fixture {};
    fx.files.addFile(inWork("README.md"), "");
    fx.source.bases = std::unexpected(PathRefusal::RanElsewhere);
    fx.mock.writeToScreen("README.md https://x.y/");
    fx.detect();
    CHECK(fx.probe.pending() == 0);
    CHECK(fx.targetAt(0, 12) == "https://x.y/");
}

TEST_CASE("LinkDetector.withNoBaseOnlyAbsoluteAndHomePathsAreProbed", "[links]")
{
    auto fx = Fixture {};
    fx.files.addFile(inWork("README.md"), "");
    fx.source.bases = PathBases {};
    fx.mock.writeToScreen("README.md " + printedInWork("README.md"));
    fx.detect();
    fx.probe.drain();
    (void) fx.frame();
    CHECK(fx.targetAt(0, 0).empty());
    CHECK(fx.targetPathAt(0, 10) == inWork("README.md"));
    REQUIRE(fx.linkAt(0, 10) != nullptr);
    CHECK(fx.linkAt(0, 10)->target.directory.empty()); // an absolute path was joined to no base
}

TEST_CASE("LinkDetector.aLineWithoutLinksChangesNoIndex", "[links]")
{
    auto fx = Fixture {};
    fx.mock.writeToScreen("!!! ???"); // nothing a pattern takes, not even a bare word
    (void) fx.frame(FrameChange::ScreenChanged);
    fx.now += 100ms;
    (void) fx.frame();
    REQUIRE(fx.match.pending() == 1);
    fx.match.drain();
    CHECK(fx.frame() == LinkIndexChange::Unchanged);

    // Scanned all the same: its revision is recorded, so the next collect leaves it alone.
    (void) fx.frame(FrameChange::ScreenChanged);
    fx.now += 100ms;
    (void) fx.frame();
    CHECK(fx.detector->nextDue(fx.now) == std::nullopt); // a collect ran, and consumed the change
    CHECK(fx.match.pending() == 0);                      // but read nothing
}

TEST_CASE("LinkDetector.aLogicalLineReachingAboveTheWindowIsScannedOnce", "[links]")
{
    // No margins: the window is the viewport, rows 0-4. The URL's logical line starts on row -1, one row of
    // history above it, so the scan reaches up there; the rows it read must stay indexed, or the next collect
    // finds them unscanned and reads them again, every time.
    auto fx = Fixture { LinkDetectorTuning { .scanMarginPages = 0, .retentionMarginPages = 0 } };
    fx.mock.writeToScreen(std::string(60, 'a') + " https://x.y/\r\nb\r\nc\r\nd\r\ne");
    REQUIRE(fx.source.linkGrid().historyLineCount() == LineCount(1));
    REQUIRE(fx.source.linkGrid().isLineWrapped(LineOffset(0)));
    auto const headRow = fx.source.linkGrid().stableLineIdOf(LineOffset(-1));
    fx.detect();
    CHECK(fx.targetAt(0, 21) == "https://x.y/");
    CHECK(fx.detector->index().scannedRevision(headRow).has_value()); // the scan reached above the window

    (void) fx.frame(FrameChange::ScreenChanged);
    fx.now += 100ms;
    (void) fx.frame();
    CHECK(fx.detector->nextDue(fx.now) == std::nullopt); // a collect ran, and consumed the change
    CHECK(fx.match.pending() == 0);                      // but read nothing
    CHECK(fx.detector->index().scannedRevision(headRow).has_value()); // still indexed
}

TEST_CASE("LinkDetector.pathsWaitingForAHungProbeStayWithinTheCap", "[links]")
{
    // The probe thread never answers. Rescans of changed rows and new rows keep finding paths; the queue
    // drops what a rescan made stale and, beyond the cap, the oldest.
    auto fx = Fixture { LinkDetectorTuning { .maxAwaitingProbe = 4 } };
    fx.mock.writeToScreen("a1 b1 c1");
    fx.detect();
    REQUIRE(fx.probe.pending() == 1);
    CHECK(fx.detector->awaitingProbeCount() == 3);

    fx.mock.writeToScreen("\ra2 b2 c2"); // the same row again: its three are stale now
    fx.detect();
    CHECK(fx.detector->awaitingProbeCount() == 3);

    fx.mock.writeToScreen("\r\nd e f g h i"); // a new row, the old one unchanged: six more, four kept
    fx.detect();
    CHECK(fx.detector->awaitingProbeCount() == 4);

    fx.mock.writeToScreen("\r\nj k l m n o p q");
    fx.detect();
    CHECK(fx.detector->awaitingProbeCount() == 4);
    CHECK(fx.probe.pending() == 1); // still the one job, still hung
}

TEST_CASE("LinkDetector.theCapNeverDropsWhatTheProbeIsLookingAt", "[links]")
{
    // Three paths are with the probe when six more arrive under a cap of four: room for one of them, and the
    // three in flight are decorated when the probe answers.
    auto fx = Fixture { LinkDetectorTuning { .maxAwaitingProbe = std::size_t { 4 } } };
    for (auto const* name: { "a1", "b1", "c1" })
        fx.files.addFile(inWork(name), "");
    fx.mock.writeToScreen("a1 b1 c1");
    fx.detect();
    REQUIRE(fx.probe.pending() == 1);

    fx.mock.writeToScreen("\r\nd e f g h i");
    fx.detect();
    CHECK(fx.detector->awaitingProbeCount() == 4);

    fx.probe.drain();
    CHECK(fx.frame() == LinkIndexChange::Changed);
    CHECK(fx.targetPathAt(0, 0) == inWork("a1"));
    CHECK(fx.targetPathAt(0, 3) == inWork("b1"));
    CHECK(fx.targetPathAt(0, 6) == inWork("c1"));
    CHECK(fx.probe.pending() == 1); // the one that had room goes next
    CHECK(fx.detector->awaitingProbeCount() == 1);
}

TEST_CASE("LinkDetector.aHealthyProbeLosesNoPathOfAWindowFullOfWords", "[links]")
{
    // The most paths a window can print: a one-letter word and a space in every two cells. A 60x200 page,
    // scanned with the page above it, holds 120 rows of 100: 12,000 paths, each naming a file that exists. A
    // probe that answers must decorate every one.
    auto fx = Fixture { LinkDetectorTuning {}, PageSize { LineCount(60), ColumnCount(200) } };
    for (auto const letter: std::views::iota('a', static_cast<char>('z' + 1)))
        fx.files.addFile(inWork(std::string(1, letter)), "");
    auto row = std::string {};
    for (auto const word: std::views::iota(0, 100))
        row += std::string { static_cast<char>('a' + (word % 26)), ' ' };
    for (auto const line: std::views::iota(0, 120))
        fx.mock.writeToScreen(line == 0 ? row : "\r\n" + row);
    REQUIRE(fx.source.linkGrid().historyLineCount() == LineCount(60));

    fx.detect();
    REQUIRE(fx.probe.pending() == 1);
    fx.probe.drain();
    CHECK(fx.frame() == LinkIndexChange::Changed);
    auto decorated = 0;
    for (auto const line: std::views::iota(-60, 60))
        for (auto const word: std::views::iota(0, 100))
            if (fx.linkAt(line, word * 2) != nullptr)
                ++decorated;
    CHECK(decorated == 12000);
    CHECK(fx.detector->awaitingProbeCount() == 0);
}

TEST_CASE("LinkDetector.destroyedWithAJobInFlight", "[links]")
{
    auto fx = Fixture {};
    fx.mock.writeToScreen("https://x.y/");
    (void) fx.frame(FrameChange::ScreenChanged);
    fx.now += 100ms;
    (void) fx.frame();
    REQUIRE(fx.match.pending() == 1);
    fx.detector.reset();
    fx.match.drain(); // runs against the shared state; ASan would flag a use-after-free
    CHECK(fx.redraws == 0);
}

namespace
{
/// The smallest default stack a secondary thread gets (macOS): what LinkDetector documents matching must fit.
constexpr auto SmallestThreadStack = std::size_t { 512 } * 1024;

/// Runs @p job on a new thread with a stack of SmallestThreadStack, and waits for it. On Windows, which
/// offers no portable way to ask for less, on std::thread's default of 1 MiB.
void runOnSmallestStack(std::function<void()> job)
{
#ifdef _WIN32
    std::thread { std::move(job) }.join();
#else
    auto attributes = pthread_attr_t {};
    REQUIRE(pthread_attr_init(&attributes) == 0);
    REQUIRE(pthread_attr_setstacksize(&attributes, SmallestThreadStack) == 0);
    auto thread = pthread_t {};
    auto* const start = +[](void* argument) -> void* {
        (*static_cast<std::function<void()>*>(argument))();
        return nullptr;
    };
    auto const created = pthread_create(&thread, &attributes, start, &job);
    (void) pthread_attr_destroy(&attributes);
    REQUIRE(created == 0);
    REQUIRE(pthread_join(thread, nullptr) == 0);
#endif
}

/// A line that drives a regex engine through one shape to its end: @ref head once, @ref unit repeated, and
/// @ref tail once.
struct LongLine
{
    std::string_view head;
    std::string_view unit;
    std::string_view tail;
};

/// Each would drive a recursive engine one level deeper per character, or more. On each, every pattern of the
/// test either matches from where an attempt starts or fails within a few characters, so the cost stays
/// linear in the line on every engine. libstdc++'s breadth-first executor is quadratic in the line for
/// attempts that run to the end and then fail, as `(a+)+b` does at every position of a run of `a` with no `b`
/// (20 s at 36k), and the depth-first one exponential: hence the `b` at the end of that line.
constexpr auto LongLines = std::array {
    LongLine { .head = "", .unit = "ab", .tail = "" },            // (?:a|b)+, and the path grammar's word run
    LongLine { .head = "", .unit = "a", .tail = "b" },            // (a+)+b
    LongLine { .head = "", .unit = "abcdefg ", .tail = "" },      // (\w+\s?)+$
    LongLine { .head = "https://x.y/", .unit = "b", .tail = "" }, // the built-in URL pattern
};

/// @return @p shape spelled out to @p length bytes.
[[nodiscard]] std::string spelledOut(LongLine const& shape, std::size_t length)
{
    auto text = std::string { shape.head };
    text.reserve(length);
    while (text.size() + shape.tail.size() < length)
        text += shape.unit.substr(0, length - shape.tail.size() - text.size());
    text += shape.tail;
    return text;
}

/// @return How many links the match job's path through findLinks() finds in @p line.
[[nodiscard]] std::size_t linksIn(std::string const& line, LinkPatterns const& patterns)
{
    auto const rows = std::array { makePlainScanRow(line) };
    auto count = std::size_t { 0 };
    for (auto const& text: joinLogicalLines(rows, LineContinuation::No, MaxScanCells))
        count += findLinks(text, patterns, PatternSelection::ClickableOnly, LinkOverlap::Merge).size();
    return count;
}
} // namespace

TEST_CASE("LinkDetector.recursionHeavyUserPatternsFitTheSmallestThreadStack", "[links][threading]")
{
    // A crash test, not a timing test: the match job runs on the application's pool, whose threads get the
    // platform's default stack, so every accepted pattern -- the built-ins and these user-style shapes --
    // must match a budget-long line inside the smallest of them. It holds by construction, not by measure: no
    // engine the patterns run on recurses per character (see LinkDetector).
    // Each writes its URL's scheme, so that every match is a link (§7.6).
    auto const specs = std::array {
        LinkPatternSpec { .name = "alternation",
                          .regex = "(?:a|b)+",
                          .activation = LinkActivation::Clickable,
                          .urlTemplate = "https://x/{0}" },
        LinkPatternSpec { .name = "nested",
                          .regex = "(a+)+b",
                          .activation = LinkActivation::Clickable,
                          .urlTemplate = "https://x/{0}" },
        LinkPatternSpec { .name = "words",
                          .regex = R"((\w+\s?)+$)",
                          .activation = LinkActivation::Clickable,
                          .urlTemplate = "https://x/{0}" },
    };
    auto const patterns = LinkPatterns::compile(specs);
    REQUIRE(patterns.diagnostics().empty());

    auto shortLinks = std::vector<std::size_t>(LongLines.size(), 0);
    auto longLengths = std::vector<std::size_t>(LongLines.size(), 0);
    runOnSmallestStack([&] {
        for (auto const index: std::views::iota(std::size_t { 0 }, LongLines.size()))
        {
            shortLinks[index] = linksIn(spelledOut(LongLines[index], 64), patterns);
            auto const line = spelledOut(LongLines[index], MaxScanCells);
            longLengths[index] = line.size();
            // What it finds is not the question: MSVC's std::regex gives up on such a line (error_stack,
            // which findLinks() catches). That it returns at all is.
            (void) linksIn(line, patterns);
        }
    });
    for (auto const index: std::views::iota(std::size_t { 0 }, LongLines.size()))
    {
        INFO("line shape " << index);
        CHECK(shortLinks[index]
              > 0); // the table takes the shape, so the long line drives the engine through it
        CHECK(longLengths[index] == MaxScanCells);
    }
}
