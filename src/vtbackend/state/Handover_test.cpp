// SPDX-License-Identifier: Apache-2.0
// Scenarios a capture taken at rest never reaches: a resize, a status line and a notification in flight on
// the alternate screen, and the limits of what a restore accepts.
#include <vtbackend/state/TerminalStateCodec.hpp>
#include <vtbackend/testing/StateHandover.hpp>
#include <vtbackend/vt/DesktopNotification.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <format>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace vtbackend;
using namespace std::string_view_literals;

namespace
{
/// A terminal that records the OSC 99 notifications it raises.
class NotificationTerm final: public MockTerm<>
{
  public:
    explicit NotificationTerm(Settings settings): MockTerm<> { std::move(settings) } {}

    void showDesktopNotification(DesktopNotification const& notification) override
    {
        _shown.push_back(notification);
    }

    /// @return The notifications raised so far, oldest first.
    [[nodiscard]] std::vector<DesktopNotification> const& shown() const noexcept { return _shown; }

  private:
    std::vector<DesktopNotification> _shown;
};

/// @return Settings for the scenario terminals: a small page with scrollback.
Settings scenarioSettings()
{
    return MockTerm<>::createSettings(
        PageSize { LineCount(8), ColumnCount(30) }, HistoryLimits::plain(LineCount(20)), 1024);
}

/// Hands @p source over into a fresh terminal built by @p make from the settings the state carries.
template <typename Term, typename Make>
std::unique_ptr<Term> handOver(MockTerm<> const& source, Make make)
{
    auto const decoded = decodeTerminalState(encodeTerminalState(source.terminal.captureState()));
    REQUIRE(decoded.has_value());
    auto const settings = settingsFor(*decoded, scenarioSettings());
    REQUIRE(settings.has_value());
    auto target = make(*settings);
    auto const restored = target->terminal.restoreState(*decoded);
    INFO(std::format("restore refused with {}", restored ? -1 : static_cast<int>(restored.error())));
    REQUIRE(restored.has_value());
    return target;
}

std::unique_ptr<MockTerm<>> handOver(MockTerm<> const& source)
{
    return handOver<MockTerm<>>(
        source, [](Settings settings) { return std::make_unique<MockTerm<>>(std::move(settings)); });
}

/// Feeds @p suffix to both and checks they are the same terminal: text and state.
void requireSameAfter(MockTerm<>& source, MockTerm<>& target, std::string_view suffix)
{
    source.writeToScreen(suffix);
    target.writeToScreen(suffix);
    CHECK(target.terminal.primaryScreen().renderMainPageText()
          == source.terminal.primaryScreen().renderMainPageText());
    CHECK(test::differingAreas(target.terminal.captureState(), source.terminal.captureState()).empty());
    CHECK(test::probeTranscript(target) == test::probeTranscript(source));
}
} // namespace

TEST_CASE("a terminal resized on the alternate screen hands over", "[Handover]")
{
    auto source = std::make_unique<MockTerm<>>(scenarioSettings());
    source->writeToScreen("on the primary screen\r\n\033[?1049hon the alternate"sv);
    source->terminal.resizeScreen(PageSize { LineCount(12), ColumnCount(40) });
    source->writeToScreen("\r\nafter the resize"sv);

    auto target = handOver(*source);

    requireSameAfter(*source, *target, "\033[?1049lback on the primary"sv);
}

TEST_CASE("a pending wrap a narrowing kept on the cut row hands over", "[Handover]")
{
    // The alternate screen does not reflow: narrowing cuts the full row and holds the cursor on the new
    // last column, its wrap still pending, so the next character goes on the next row.
    auto source = std::make_unique<MockTerm<>>(scenarioSettings());
    source->writeToScreen("\033[?1049h"sv);
    source->writeToScreen(std::string(30, 'a'));
    source->terminal.resizeScreen(PageSize { LineCount(8), ColumnCount(20) });
    REQUIRE(source->terminal.currentScreen().cursor().wrapPending);

    auto target = handOver(*source);

    requireSameAfter(*source, *target, "b"sv);
    CHECK(target->terminal.currentScreen().cursor().position.line == LineOffset(1));
}

TEST_CASE("a status line shown while on the alternate screen hands over", "[Handover]")
{
    auto source = std::make_unique<MockTerm<>>(scenarioSettings());
    source->writeToScreen("primary\r\n\033[?1049halternate"sv);
    source->terminal.setStatusDisplay(StatusDisplayType::Indicator);

    auto target = handOver(*source);

    requireSameAfter(*source, *target, "\033[?1049lprimary again"sv);
}

TEST_CASE("a chunked OSC 99 notification in flight completes after the handover", "[Handover]")
{
    auto source = std::make_unique<MockTerm<>>(scenarioSettings());
    source->writeToScreen("\033]99;i=n1:d=0;first half \033\\"sv);

    auto target = handOver<NotificationTerm>(
        *source, [](Settings settings) { return std::make_unique<NotificationTerm>(std::move(settings)); });
    target->writeToScreen("\033]99;i=n1:d=1;second half\033\\"sv);

    REQUIRE(target->shown().size() == 1);
    CHECK(target->shown().front().title == "first half second half");
}

TEST_CASE("notifications p=alive reports survive the handover", "[Handover]")
{
    auto source = std::make_unique<MockTerm<>>(scenarioSettings());
    source->terminal.desktopNotificationManager().addActiveNotification("older");
    source->terminal.desktopNotificationManager().addActiveNotification("newer");

    auto target = handOver(*source);

    CHECK(target->terminal.desktopNotificationManager().activeNotifications()
          == source->terminal.desktopNotificationManager().activeNotifications());
}

TEST_CASE("the in-flight tail lets go of a large sequence's memory once it ends", "[Handover]")
{
    auto source = std::make_unique<MockTerm<>>(scenarioSettings());
    source->writeToScreen("\033Pz"sv); // a DCS nobody implements: ignored to its ST
    for ([[maybe_unused]] auto const chunk: std::views::iota(0, 64))
        source->writeToScreen(std::string(4096, '~'));
    REQUIRE(source->terminal.inFlightTailCapacity() >= size_t { 64 } * 4096);
    source->writeToScreen("\033\\"sv);
    CHECK(source->terminal.inFlightTailCapacity() <= InFlightTailRetainedCapacity);
}

TEST_CASE("a restore refuses a value that points outside what it indexes", "[Handover]")
{
    auto source = std::make_unique<MockTerm<>>(scenarioSettings());
    source->writeToScreen("some text\r\nand more\033[c"sv);
    auto const good = decodeTerminalState(encodeTerminalState(source->terminal.captureState()));
    REQUIRE(good.has_value());

    auto const isRefused = [&](auto&& tamper) {
        auto bad = *good;
        tamper(bad);
        auto const settings = settingsFor(bad, scenarioSettings());
        if (!settings)
            return true;
        auto target = std::make_unique<MockTerm<>>(*settings);
        return !target->terminal.restoreState(bad).has_value();
    };
    CHECK_FALSE(isRefused([](TerminalState&) {}));
    CHECK(isRefused([](TerminalState& s) {
        s.input.consumedBytes = static_cast<int>(s.input.pendingSequence.size()) + 1;
    }));
    CHECK(isRefused([](TerminalState& s) { s.input.consumedBytes = -1; }));
    CHECK(isRefused([](TerminalState& s) { s.paging.scrollOffset = ScrollOffset(1000); }));
    CHECK(isRefused([](TerminalState& s) { s.macros.running = { 1 }; }));
    CHECK(isRefused([](TerminalState& s) { s.margins.mainDisplay.vertical.to = LineOffset(500); }));
    CHECK(isRefused([](TerminalState& s) { s.margins.indicator.horizontal.to = ColumnOffset(500); }));
    CHECK(isRefused([](TerminalState& s) { s.colors.saved.resize(64); }));
    CHECK(isRefused([](TerminalState& s) {
        s.interaction.selection = SelectionState { .kind = SelectionKind::Linear,
                                                   .helper = SelectionHelperKind::Standard,
                                                   .from = CellLocation { LineOffset(0), ColumnOffset(0) },
                                                   .to = CellLocation { LineOffset(400), ColumnOffset(0) },
                                                   .state = Selection::State::InProgress };
    }));
    CHECK(isRefused(
        [](TerminalState& s) { s.paging.activeStatusDisplay = static_cast<ActiveStatusDisplay>(9); }));
}

TEST_CASE("a refused restore leaves the terminal as it was constructed", "[Handover]")
{
    auto source = std::make_unique<MockTerm<>>(scenarioSettings());
    source->writeToScreen("\033]2;a title\033\\some text\r\n\033[?1049halternate\033[>1u"sv);
    auto bad = *decodeTerminalState(encodeTerminalState(source->terminal.captureState()));
    bad.input.keyboard.currentStackTop = 99;
    auto const settings = settingsFor(bad, scenarioSettings());
    REQUIRE(settings.has_value());

    auto target = std::make_unique<MockTerm<>>(*settings);
    auto const fresh = std::make_unique<MockTerm<>>(*settings);
    REQUIRE_FALSE(target->terminal.restoreState(bad).has_value());

    CHECK(encodeTerminalState(target->terminal.captureState())
          == encodeTerminalState(fresh->terminal.captureState()));
}

namespace
{
/// A terminal that records what it tells its listener about itself.
class AnnouncingTerm final: public MockTerm<>
{
  public:
    explicit AnnouncingTerm(Settings settings): MockTerm<> { std::move(settings) } {}

    void setWindowTitle(std::string_view title) override { windowTitle = title; }
    void bufferChanged(ScreenType type) override { screenType = type; }
    void setPointerShape(std::string_view cssName) override { pointerShape = cssName; }
    void workingDirectoryChanged() override { ++workingDirectoryChanges; }

    std::string windowTitle;              ///< The title it was last told.
    std::optional<ScreenType> screenType; ///< The screen it was last told it shows.
    std::string pointerShape;             ///< The pointer shape it was last told.
    int workingDirectoryChanges = 0;      ///< How often it was told the directory changed.
};
} // namespace

TEST_CASE("a restored terminal tells its listener what the restore changed", "[Handover]")
{
    auto source = std::make_unique<MockTerm<>>(scenarioSettings());
    source->writeToScreen(
        "\033]2;restored title\033\\\033]22;text\033\\\033]7;file:///tmp\033\\\033[?1049h"sv);

    auto target = handOver<AnnouncingTerm>(
        *source, [](Settings settings) { return std::make_unique<AnnouncingTerm>(std::move(settings)); });

    CHECK(target->windowTitle == "restored title");
    CHECK(target->screenType == ScreenType::Alternate);
    CHECK(target->pointerShape == "text");
    CHECK(target->workingDirectoryChanges > 0);
}

TEST_CASE("an image fragment two cells share is one object after the handover", "[Handover]")
{
    auto const image = std::make_shared<Image>(
        ImageId(1), ImageFormat::RGBA, Image::Data(16, 0), ImageSize { Width(2), Height(2) }, [](auto) {});
    auto const rasterized =
        std::make_shared<RasterizedImage>(image,
                                          ImageAlignment::TopStart,
                                          ImageResize::NoResize,
                                          RGBAColor { 0, 0, 0, 0xFF },
                                          GridSize { .lines = LineCount(1), .columns = ColumnCount(1) },
                                          ImageSize { Width(2), Height(2) });
    auto const fragment = std::make_shared<ImageFragment>(rasterized, CellLocation {});
    auto source = std::make_unique<MockTerm<>>(scenarioSettings());
    auto& grid = source->terminal.primaryScreen().grid();
    for (auto const line: { LineOffset(0), LineOffset(1) })
    {
        auto& storage = grid.lineAt(line).storage();
        storage.imageFragments.emplace().emplace(uint16_t { 0 }, fragment);
        storage.shape = LineShape::Spanning;
    }

    auto target = handOver(*source);

    auto const& restored = target->terminal.primaryScreen().grid();
    auto const& first = restored.lineAt(LineOffset(0)).storage().imageFragments;
    auto const& second = restored.lineAt(LineOffset(1)).storage().imageFragments;
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    CHECK(first->at(0).get() == second->at(0).get());
}

TEST_CASE("a session that touched every kind of state answers every probe alike after the handover",
          "[Handover]")
{
    auto source = std::make_unique<MockTerm<>>(scenarioSettings());
    source->writeToScreen("\033]2;a title\033\\\033]4;1;rgb:12/34/56\033\\\033]10;rgb:aa/bb/cc\033\\"sv);
    source->writeToScreen("\033[3g\033[1;5H\033H\033[1;13H\033H\033[1;1H"sv); // tab stops at 5 and 13
    source->writeToScreen("\033[2;6r\033[?69h\033[3;20s\033[4 q\033[1\"q"sv); // margins, cursor, DECSCA
    source->writeToScreen("\033[1;3;38;5;123mstyled\033[0m\033[?1000h\033[?1006h\033[?2004h\033[>5u"sv);
    source->writeToScreen("\033[4h\033[?1h\033=\033[#P\033]4;2;rgb:01/02/03\033\\"sv); // IRM, DECCKM, DECKPAM
    source->writeToScreen("\033[?1049halternate text\033[?25l"sv);

    auto target = handOver(*source);

    CHECK(test::differingAreas(target->terminal.captureState(), source->terminal.captureState()).empty());
    CHECK(test::probeTranscript(*target) == test::probeTranscript(*source));
}

namespace
{
/// Decodes @p stream and, if it decodes, restores it into a terminal built from the settings it carries.
/// @return Whether it was accepted; any refusal is a decode error, never a crash.
bool decodesAndRestores(std::span<uint8_t const> stream)
{
    auto const decoded = decodeTerminalState(stream);
    if (!decoded)
        return false;
    auto const settings = settingsFor(*decoded, scenarioSettings());
    if (!settings)
        return false;
    auto target = std::make_unique<MockTerm<>>(*settings);
    if (!target->terminal.restoreState(*decoded))
        return false;
    // A restored terminal must be one that goes on working.
    target->writeToScreen("after the restore\r\n\033[c"sv);
    target->terminal.flushInput();
    return true;
}
} // namespace

TEST_CASE("damaged streams are refused or restored, never a crash", "[Handover][fuzz]")
{
    auto source = std::make_unique<MockTerm<>>(scenarioSettings());
    source->terminal.setCellPixelSize(ImageSize { Width(10), Height(20) });
    source->writeToScreen(
        "\033]2;fuzzed\033\\\033[31mred\033[0m\r\n\033]8;;https://x\033\\link\033]8;;\033\\\r\n"sv);
    source->writeToScreen(
        "\033Pq\"1;1;4;6#1;2;100;0;0#1~~~~\033\\\r\n\033_Ga=T,f=24,s=1,v=1,i=3;AAAA\033\\"sv);
    source->writeToScreen("\033[?1049halt\033]99;i=n:d=0;half\033\\\033[3"sv);
    auto const stream = encodeTerminalState(source->terminal.captureState());
    REQUIRE(decodesAndRestores(stream));

    // Fixed, so a failure reproduces: every run tries the same streams.
    auto rng = test::FixedSequence { 0xF022U };
    constexpr auto Truncations = 200;
    constexpr auto Mutations = 400;
    for ([[maybe_unused]] auto const attempt: std::views::iota(0, Truncations))
    {
        auto const length = static_cast<size_t>(rng() % stream.size());
        INFO(std::format("truncated to {} of {} bytes", length, stream.size()));
        CHECK_FALSE(decodesAndRestores(std::span { stream }.first(length)));
    }
    for (auto const attempt: std::views::iota(0, Mutations))
    {
        auto damaged = stream;
        auto const flips = 1 + (rng() % 4);
        for ([[maybe_unused]] auto const flip: std::views::iota(uint64_t { 0 }, flips))
            damaged[rng() % damaged.size()] ^= static_cast<uint8_t>(1U << (rng() % 8));
        INFO(std::format("mutation {}", attempt));
        std::ignore = decodesAndRestores(damaged);
    }
}

TEST_CASE("XTPOPCOLORS of a slot the stack does not hold is ignored", "[Handover]")
{
    auto mock = std::make_unique<MockTerm<>>(scenarioSettings());
    mock->writeToScreen("\033]4;1;rgb:11/22/33\033\\\033[#P"sv); // XTPUSHCOLORS: one slot
    auto const before = mock->terminal.colorPalette().palette;
    mock->writeToScreen("\033]4;1;rgb:44/55/66\033\\"sv);
    auto const changed = mock->terminal.colorPalette().palette;
    mock->writeToScreen("\033[5#Q"sv); // XTPOPCOLORS of slot 5, which was never pushed
    CHECK(mock->terminal.colorPalette().palette == changed);
    CHECK(changed != before);
}

// {{{ held scrollback
namespace
{
/// The page scenarioSettings() builds, and a shorter one a resize makes of it.
constexpr auto FullPage = PageSize { LineCount(8), ColumnCount(30) };
constexpr auto ShortPage = PageSize { LineCount(4), ColumnCount(30) };

/// @return A terminal of scenarioSettings() that printed @p rows numbered rows.
std::unique_ptr<MockTerm<>> printedRows(int rows)
{
    auto mock = std::make_unique<MockTerm<>>(scenarioSettings());
    for (auto const row: std::views::iota(0, rows))
        mock->writeToScreen(std::format("row {:02}\r\n", row));
    return mock;
}

/// Checks that @p target holds every row @p source does, history included.
void requireSameRows(MockTerm<>& source, MockTerm<>& target)
{
    auto const& expected = source.terminal.primaryScreen().grid();
    auto const& actual = target.terminal.primaryScreen().grid();
    REQUIRE(actual.historyLineCount() == expected.historyLineCount());
    CHECK(actual.renderAllText() == expected.renderAllText());
    CHECK(test::differingAreas(target.terminal.captureState(), source.terminal.captureState()).empty());
}
} // namespace

TEST_CASE("scrollback a resize holds above the capacity hands over and is given back alike", "[Handover]")
{
    // A full scrollback, then a shorter page: the rows pushed out of it stay held above the capacity.
    auto source = printedRows(40);
    source->terminal.resizeScreen(ShortPage);
    REQUIRE(source->terminal.primaryScreen().grid().historyLineCount() > LineCount(20));

    auto target = handOver(*source);
    requireSameRows(*source, *target);

    SECTION("the resize undone gives every held row back")
    {
        source->terminal.resizeScreen(FullPage);
        target->terminal.resizeScreen(FullPage);
        requireSameRows(*source, *target);
    }
    SECTION("the next output settles the hold alike")
    {
        requireSameAfter(*source, *target, "settled\r\n"sv);
        CHECK(target->terminal.primaryScreen().grid().historyLineCount() == LineCount(20));
        requireSameRows(*source, *target);
    }
}

TEST_CASE("a restore refuses scrollback above the capacity that no held ring accounts for", "[Handover]")
{
    auto source = printedRows(40);
    source->terminal.resizeScreen(ShortPage);
    auto state = source->terminal.captureState();
    state.pages.front().grid.ringSlots.reset();

    auto const settings = settingsFor(state, scenarioSettings());
    REQUIRE(settings.has_value());
    auto const target = std::make_unique<MockTerm<>>(*settings);
    CHECK_FALSE(target->terminal.restoreState(state).has_value());
}
// }}}

// {{{ kitty playback
namespace
{
using namespace std::chrono_literals;

/// How far the restoring process's clock is from the capturing one's: the deadlines must be rebased.
constexpr auto ClockSkew = std::chrono::hours { 5 };

/// Places a one-pixel image under id 7 at the top left with three frames -- red, green, blue -- shown for
/// 10, 20 and 30 ms, and starts it looping @p loops times (`v=`, 1 forever).
void playThreeFrames(MockTerm<>& mock, int loops)
{
    mock.terminal.setCellPixelSize(ImageSize { Width(10), Height(20) });
    mock.writeToScreen("\033_Ga=T,f=24,i=7,s=1,v=1;/wAA\033\\"sv);
    mock.writeToScreen("\033_Ga=a,i=7,r=1,z=10\033\\"sv);
    mock.writeToScreen("\033_Ga=f,i=7,X=1,s=1,v=1,z=20;AP8A\033\\"sv);
    mock.writeToScreen("\033_Ga=f,i=7,X=1,s=1,v=1,z=30;AAD/\033\\"sv);
    mock.writeToScreen(std::format("\033_Ga=a,i=7,s=3,v={}\033\\", loops));
    mock.discardPendingReplies();
}

/// @return The first pixel the top left cell shows, as its red, green and blue; empty for no image.
std::vector<uint8_t> shownPixel(MockTerm<>& mock)
{
    auto const _ = std::lock_guard { mock.terminal };
    auto const fragment = mock.terminal.primaryScreen().at(LineOffset(0), ColumnOffset(0)).imageFragment();
    if (!fragment)
        return {};
    auto const& data = fragment->rasterizedImage().image().data();
    return { data.begin(), data.begin() + std::min<std::ptrdiff_t>(3, std::ssize(data)) };
}

/// The playback deadline @p mock published, relative to its own clock.
std::optional<std::chrono::steady_clock::duration> dueIn(MockTerm<>& mock)
{
    auto const due = mock.terminal.kittyAnimationDue();
    if (!due)
        return std::nullopt;
    return *due - mock.steadyClock.now();
}

/// Hands @p source over into a terminal whose clock reads ClockSkew later.
std::unique_ptr<MockTerm<>> handOverSkewed(MockTerm<> const& source)
{
    return handOver<MockTerm<>>(source, [](Settings settings) {
        auto target = std::make_unique<MockTerm<>>(std::move(settings));
        target->steadyClock.advance(ClockSkew);
        return target;
    });
}

/// Ticks both terminals @p steps times, @p step apart, checking after each that they show the same frame
/// and are due to move on at the same time.
void requireSamePlayback(MockTerm<>& source, MockTerm<>& target, int steps, std::chrono::milliseconds step)
{
    for (auto const index: std::views::iota(0, steps))
    {
        INFO(std::format("after {} ms", (index + 1) * step.count()));
        for (auto* const mock: { &source, &target })
        {
            mock->steadyClock.advance(step);
            mock->terminal.tick(mock->steadyClock.now());
        }
        REQUIRE(shownPixel(target) == shownPixel(source));
        REQUIRE(dueIn(target) == dueIn(source));
    }
}
} // namespace

TEST_CASE("a kitty animation handed over mid-frame plays on from where it was", "[Handover][kitty]")
{
    auto source = std::make_unique<MockTerm<>>(scenarioSettings());
    playThreeFrames(*source, 1);
    source->steadyClock.advance(15ms); // into the second frame, 15 ms before the third
    source->terminal.tick(source->steadyClock.now());
    REQUIRE(dueIn(*source) == std::chrono::steady_clock::duration { 15ms });

    auto target = handOverSkewed(*source);

    // Due at once, without a tick or a kitty command to re-arm it: the daemon's frame timer hears of it.
    CHECK(dueIn(*target) == dueIn(*source));
    CHECK(shownPixel(*target) == shownPixel(*source));
    requireSamePlayback(*source, *target, 40, 5ms);
}

TEST_CASE("a kitty animation handed over mid-loop stops after the loops v= allows", "[Handover][kitty]")
{
    // Two loops of 60 ms; the handover falls into the second.
    auto source = std::make_unique<MockTerm<>>(scenarioSettings());
    playThreeFrames(*source, 3);
    for ([[maybe_unused]] auto const step: std::views::iota(0, 14))
    {
        source->steadyClock.advance(5ms);
        source->terminal.tick(source->steadyClock.now());
    }

    auto target = handOverSkewed(*source);

    requireSamePlayback(*source, *target, 30, 5ms);
    CHECK_FALSE(dueIn(*source).has_value());
}
// }}}

TEST_CASE("a screen that logged an image dropped for want of a cell size does not log it again", "[Handover]")
{
    auto source = std::make_unique<MockTerm<>>(scenarioSettings());
    source->writeToScreen("\033Pq#0;2;100;0;0#0!6~-\033\\"sv); // a sixel, and no cell size to place it by
    REQUIRE(source->terminal.captureState().pages.front().cellSizeDropLog == CellSizeDropLog::Logged);

    auto target = handOver(*source);

    CHECK(target->terminal.captureState().pages.front().cellSizeDropLog == CellSizeDropLog::Logged);
}
