// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the sticky-header decision: whether the command header, the evicted-output placeholder
// or nothing covers the top row. Pure -- every input is a plain value -- so the whole input space is
// walked and checked against the rule restated once (specFor below), beside named rows that read as the
// rule's own sentences. Then the texts the header shows: the status chip and the placeholder.

#include <vtbackend/shell/StickyHeader.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <optional>
#include <string>
#include <vector>

using namespace vtbackend;

namespace
{

constexpr auto Modes =
    std::array { StickyHeaderMode::Never, StickyHeaderMode::Scrolled, StickyHeaderMode::Always };
constexpr auto Placeholders = std::array { EvictedPlaceholder::Hidden, EvictedPlaceholder::Shown };
constexpr auto Presentations =
    std::array { StickyHeaderPresentation::InGrid, StickyHeaderPresentation::Chrome };
constexpr auto Screens = std::array { ScreenType::Primary, ScreenType::Alternate };
constexpr auto Viewports = std::array { ViewportScroll::Live, ViewportScroll::Scrolled };
constexpr auto Ages = std::array { TopRowAge::Newer, TopRowAge::Oldest };
constexpr auto Cursors =
    std::array { TopRowCursor::Elsewhere, TopRowCursor::ViCursor, TopRowCursor::TextCursor };
constexpr auto States =
    std::array { CommandBlockState::Prompting, CommandBlockState::Running, CommandBlockState::Finished };
constexpr auto Heads =
    std::array { StickyHeadPosition::Visible, StickyHeadPosition::Above, StickyHeadPosition::Evicted };
constexpr auto TopRows = std::array { TopRowPart::Input, TopRowPart::Output };

constexpr auto Block = CommandBlockId(7);

/// Every block the top row can belong to, and "none" first.
[[nodiscard]] std::vector<std::optional<StickyHeaderBlockFacts>> everyBlock()
{
    auto blocks = std::vector<std::optional<StickyHeaderBlockFacts>> { std::nullopt };
    for (auto const state: States)
        for (auto const head: Heads)
            for (auto const topRow: TopRows)
                blocks.emplace_back(
                    StickyHeaderBlockFacts { .id = Block, .state = state, .head = head, .topRow = topRow });
    return blocks;
}

/// Every input the decision can be handed: one per combination of its fields, where the header is drawn
/// included -- which must decide nothing (specFor() does not read it).
[[nodiscard]] std::vector<StickyHeaderInput> everyInput()
{
    auto inputs = std::vector<StickyHeaderInput> {};
    auto const blocks = everyBlock();
    for (auto const mode: Modes)
        for (auto const placeholder: Placeholders)
            for (auto const presentation: Presentations)
                for (auto const screen: Screens)
                    for (auto const viewport: Viewports)
                        for (auto const age: Ages)
                            for (auto const cursor: Cursors)
                                for (auto const& block: blocks)
                                    inputs.push_back(StickyHeaderInput {
                                        .settings = StickyHeaderSettings { .mode = mode,
                                                                           .evictedPlaceholder = placeholder,
                                                                           .presentation = presentation },
                                        .screen = screen,
                                        .viewport = viewport,
                                        .topRowAge = age,
                                        .cursor = cursor,
                                        .block = block,
                                    });
    return inputs;
}

/// The rule, restated once, for every row of the input space to be checked against.
[[nodiscard]] std::optional<StickyHeaderKind> specFor(StickyHeaderInput const& input)
{
    if (input.screen != ScreenType::Primary || !input.block
        || input.block->state == CommandBlockState::Prompting)
        return std::nullopt;

    auto const& block = *input.block;
    // Whether the mode shows a header at this viewport position for a block whose head is above it, and no
    // cursor stands on the row it would cover.
    auto const modeAllows =
        (input.settings.mode == StickyHeaderMode::Always
         || (input.settings.mode == StickyHeaderMode::Scrolled && input.viewport == ViewportScroll::Scrolled))
        && input.cursor == TopRowCursor::Elsewhere;

    if (block.head == StickyHeadPosition::Above && block.topRow == TopRowPart::Output && modeAllows)
        return StickyHeaderKind::Command;
    // An evicted head: at the very top always -- whatever the mode, a cursor on the row or not -- anywhere
    // else wherever the mode shows a header, so the header never vanishes just because the prompt left the
    // scrollback. Never with show_evicted off.
    if (block.head == StickyHeadPosition::Evicted
        && input.settings.evictedPlaceholder == EvictedPlaceholder::Shown
        && (input.topRowAge == TopRowAge::Oldest || modeAllows))
        return StickyHeaderKind::Evicted;
    return std::nullopt;
}

/// The kind a decision names, or nullopt for none -- what the tables compare.
[[nodiscard]] std::optional<StickyHeaderKind> kindOf(std::optional<StickyHeaderDecision> const& decision)
{
    return decision ? std::optional { decision->kind } : std::nullopt;
}

/// A running block whose head is above the viewport and whose output the top row shows.
[[nodiscard]] StickyHeaderInput scrolledIntoOutput(StickyHeaderMode mode)
{
    return StickyHeaderInput {
        .settings = StickyHeaderSettings { .mode = mode,
                                           .evictedPlaceholder = EvictedPlaceholder::Shown,
                                           .presentation = StickyHeaderPresentation::InGrid },
        .screen = ScreenType::Primary,
        .viewport = ViewportScroll::Scrolled,
        .topRowAge = TopRowAge::Newer,
        .cursor = TopRowCursor::Elsewhere,
        .block = StickyHeaderBlockFacts { .id = Block,
                                          .state = CommandBlockState::Running,
                                          .head = StickyHeadPosition::Above,
                                          .topRow = TopRowPart::Output },
    };
}

/// A block whose head left the scrollback, scrolled back into what is left of its output -- but not to the
/// very top: older rows are still there.
[[nodiscard]] StickyHeaderInput scrolledIntoAnEvictedBlock(StickyHeaderMode mode)
{
    auto input = scrolledIntoOutput(mode);
    input.block->head = StickyHeadPosition::Evicted;
    return input;
}

/// A block whose head left the scrollback, seen from the very top of what is left.
[[nodiscard]] StickyHeaderInput atTheTopOfAnEvictedBlock(StickyHeaderMode mode)
{
    auto input = scrolledIntoAnEvictedBlock(mode);
    input.topRowAge = TopRowAge::Oldest;
    return input;
}

} // namespace

TEST_CASE("StickyHeader.decision.matchesTheSpecForEveryInput", "[stickyheader]")
{
    auto const inputs = everyInput();
    // One input per combination of the fields; a block may also be absent.
    REQUIRE(inputs.size()
            == Modes.size() * Placeholders.size() * Presentations.size() * Screens.size() * Viewports.size()
                   * Ages.size() * Cursors.size() * (1 + (States.size() * Heads.size() * TopRows.size())));

    for (auto const& input: inputs)
    {
        auto const decision = stickyHeaderFor(input);
        CHECK(kindOf(decision) == specFor(input));
        // Whatever it shows, it names the block it was handed -- the block a press on it jumps to.
        if (decision)
            CHECK(decision->block == Block);
    }
}

TEST_CASE("StickyHeader.decision.isPossibleExactlyWhereSomeBlockGetsAHeader", "[stickyheader]")
{
    // What the terminal asks before it looks the top row's block up. Never false where a block would get a
    // header -- the frame would lose it -- and never true where none can, or the lookup it saves is paid.
    auto const blocks = everyBlock();
    for (auto const mode: Modes)
        for (auto const placeholder: Placeholders)
            for (auto const screen: Screens)
                for (auto const viewport: Viewports)
                    for (auto const age: Ages)
                    {
                        auto input = StickyHeaderInput {
                            .settings =
                                StickyHeaderSettings { .mode = mode,
                                                       .evictedPlaceholder = placeholder,
                                                       .presentation = StickyHeaderPresentation::InGrid },
                            .screen = screen,
                            .viewport = viewport,
                            .topRowAge = age,
                            .cursor = TopRowCursor::Elsewhere,
                            .block = std::nullopt,
                        };
                        auto someBlockGetsOne = false;
                        for (auto const& block: blocks)
                        {
                            input.block = block;
                            someBlockGetsOne = someBlockGetsOne || stickyHeaderFor(input).has_value();
                        }
                        INFO("mode " << static_cast<int>(mode) << ", placeholder "
                                     << static_cast<int>(placeholder) << ", screen "
                                     << static_cast<int>(screen) << ", viewport "
                                     << static_cast<int>(viewport) << ", age " << static_cast<int>(age));
                        CHECK(stickyHeaderPossible(input.settings, screen, viewport, age)
                              == someBlockGetsOne);
                    }
}

TEST_CASE("StickyHeader.decision.isPossibleAtTheVeryTopWhileLive", "[stickyheader]")
{
    // A collapsed fold can make the live page's top row the oldest row kept. Following live output is no
    // reason to skip the lookup there: the placeholder shows at the very top whatever the mode.
    auto const settings = StickyHeaderSettings { .mode = StickyHeaderMode::Scrolled,
                                                 .evictedPlaceholder = EvictedPlaceholder::Shown,
                                                 .presentation = StickyHeaderPresentation::InGrid };
    CHECK(stickyHeaderPossible(settings, ScreenType::Primary, ViewportScroll::Live, TopRowAge::Oldest));
    CHECK_FALSE(stickyHeaderPossible(settings, ScreenType::Primary, ViewportScroll::Live, TopRowAge::Newer));
}

TEST_CASE("StickyHeader.decision.readsAsTheSpec", "[stickyheader]")
{
    SECTION("scrolled (the default): only once scrolled back into history")
    {
        auto input = scrolledIntoOutput(StickyHeaderMode::Scrolled);
        CHECK(kindOf(stickyHeaderFor(input)) == StickyHeaderKind::Command);
        input.viewport = ViewportScroll::Live;
        CHECK_FALSE(stickyHeaderFor(input).has_value());
    }

    SECTION("always: while following live output too, so `make` stays above its own output")
    {
        auto input = scrolledIntoOutput(StickyHeaderMode::Always);
        input.viewport = ViewportScroll::Live;
        CHECK(kindOf(stickyHeaderFor(input)) == StickyHeaderKind::Command);
    }

    SECTION("never hides the command header, and the evicted placeholder everywhere but the very top")
    {
        CHECK_FALSE(stickyHeaderFor(scrolledIntoOutput(StickyHeaderMode::Never)).has_value());
        CHECK(kindOf(stickyHeaderFor(atTheTopOfAnEvictedBlock(StickyHeaderMode::Never)))
              == StickyHeaderKind::Evicted);
        CHECK_FALSE(stickyHeaderFor(scrolledIntoAnEvictedBlock(StickyHeaderMode::Never)).has_value());
    }

    SECTION("the alternate screen never shows either")
    {
        auto command = scrolledIntoOutput(StickyHeaderMode::Always);
        command.screen = ScreenType::Alternate;
        CHECK_FALSE(stickyHeaderFor(command).has_value());

        auto evicted = atTheTopOfAnEvictedBlock(StickyHeaderMode::Always);
        evicted.screen = ScreenType::Alternate;
        CHECK_FALSE(stickyHeaderFor(evicted).has_value());

        auto evictedBelowTheTop = scrolledIntoAnEvictedBlock(StickyHeaderMode::Always);
        evictedBelowTheTop.screen = ScreenType::Alternate;
        CHECK_FALSE(stickyHeaderFor(evictedBelowTheTop).has_value());
    }

    SECTION("a head on screen is never repeated -- a collapsed fold shows its head, so nothing")
    {
        auto input = scrolledIntoOutput(StickyHeaderMode::Always);
        input.block->head = StickyHeadPosition::Visible;
        CHECK_FALSE(stickyHeaderFor(input).has_value());
    }

    SECTION("a top row that is still the command line shows nothing")
    {
        auto input = scrolledIntoOutput(StickyHeaderMode::Always);
        input.block->topRow = TopRowPart::Input;
        CHECK_FALSE(stickyHeaderFor(input).has_value());
    }

    SECTION("a cursor on the top row, the vi-mode one or the text cursor: the header steps aside")
    {
        for (auto const cursor: { TopRowCursor::ViCursor, TopRowCursor::TextCursor })
        {
            auto command = scrolledIntoOutput(StickyHeaderMode::Always);
            command.cursor = cursor;
            CHECK_FALSE(stickyHeaderFor(command).has_value());

            auto evicted = scrolledIntoAnEvictedBlock(StickyHeaderMode::Always);
            evicted.cursor = cursor;
            CHECK_FALSE(stickyHeaderFor(evicted).has_value());
        }
    }

    SECTION("but the placeholder at the very top shows always, over the vi-mode cursor `gg` put there too")
    {
        auto evicted = atTheTopOfAnEvictedBlock(StickyHeaderMode::Never);
        evicted.cursor = TopRowCursor::ViCursor;
        CHECK(kindOf(stickyHeaderFor(evicted)) == StickyHeaderKind::Evicted);
    }

    SECTION("a block still at its prompt shows nothing")
    {
        auto input = scrolledIntoOutput(StickyHeaderMode::Always);
        input.block->state = CommandBlockState::Prompting;
        CHECK_FALSE(stickyHeaderFor(input).has_value());
    }

    SECTION("a finished block shows its header like a running one")
    {
        auto input = scrolledIntoOutput(StickyHeaderMode::Scrolled);
        input.block->state = CommandBlockState::Finished;
        CHECK(kindOf(stickyHeaderFor(input)) == StickyHeaderKind::Command);
    }

    SECTION(
        "the evicted placeholder: at the very top in every mode, elsewhere wherever the mode shows a header")
    {
        auto input = atTheTopOfAnEvictedBlock(StickyHeaderMode::Scrolled);
        CHECK(kindOf(stickyHeaderFor(input)) == StickyHeaderKind::Evicted);

        // Below the very top, scrolled back into history: the header does not vanish just because the
        // block's prompt was evicted -- in the modes that show a header there.
        input.topRowAge = TopRowAge::Newer;
        CHECK(kindOf(stickyHeaderFor(input)) == StickyHeaderKind::Evicted);
        input.settings.mode = StickyHeaderMode::Always;
        CHECK(kindOf(stickyHeaderFor(input)) == StickyHeaderKind::Evicted);
        input.settings.mode = StickyHeaderMode::Never;
        CHECK_FALSE(stickyHeaderFor(input).has_value());

        // Following live output: only `always` shows a header there, so only `always` shows the placeholder.
        input.viewport = ViewportScroll::Live;
        input.settings.mode = StickyHeaderMode::Scrolled;
        CHECK_FALSE(stickyHeaderFor(input).has_value());
        input.settings.mode = StickyHeaderMode::Always;
        CHECK(kindOf(stickyHeaderFor(input)) == StickyHeaderKind::Evicted);
    }

    SECTION("show_evicted off: a block whose head was evicted shows nothing, anywhere, in any mode")
    {
        for (auto const mode: Modes)
        {
            auto atTheTop = atTheTopOfAnEvictedBlock(mode);
            atTheTop.settings.evictedPlaceholder = EvictedPlaceholder::Hidden;
            CHECK_FALSE(stickyHeaderFor(atTheTop).has_value());

            auto belowTheTop = scrolledIntoAnEvictedBlock(mode);
            belowTheTop.settings.evictedPlaceholder = EvictedPlaceholder::Hidden;
            CHECK_FALSE(stickyHeaderFor(belowTheTop).has_value());
        }
    }

    SECTION("no block -- no shell integration, or a dropped record -- shows nothing")
    {
        auto input = scrolledIntoOutput(StickyHeaderMode::Always);
        input.block.reset();
        CHECK_FALSE(stickyHeaderFor(input).has_value());
    }
}

// The durations are formatCommandDuration()'s, tested on their own beside it.
TEST_CASE("StickyHeader.chip.namesTheOutcomeAndTheDuration", "[stickyheader]")
{
    using namespace std::chrono_literals;
    auto const start = std::chrono::system_clock::time_point { std::chrono::hours(1000) };

    auto record = CommandBlockRecord {};
    record.id = CommandBlockId(3);
    record.commandStartedAt = start;

    SECTION("running: how long so far")
    {
        record.state = CommandBlockState::Running;
        CHECK(stickyHeaderChip(record, start + 12s) == U"running 12s");
    }

    SECTION("running, with a wall clock stepped backwards: zero rather than a negative")
    {
        record.state = CommandBlockState::Running;
        CHECK(stickyHeaderChip(record, start - 5s) == U"running 0ms");
    }

    SECTION("running for less than a second: milliseconds")
    {
        record.state = CommandBlockState::Running;
        CHECK(stickyHeaderChip(record, start + 850ms) == U"running 850ms");
    }

    SECTION("running, its start unknown: no time")
    {
        record.state = CommandBlockState::Running;
        record.commandStartedAt.reset();
        CHECK(stickyHeaderChip(record, start + 12s) == U"running");
    }

    SECTION("success, with its duration")
    {
        record.state = CommandBlockState::Finished;
        record.exitCode = 0;
        record.duration = 3s;
        CHECK(stickyHeaderChip(record, start + 1h) == U"✓ 3s");
    }

    SECTION("failure, with its exit code and its duration")
    {
        record.state = CommandBlockState::Finished;
        record.exitCode = 2;
        record.duration = 192s;
        CHECK(stickyHeaderChip(record, start + 1h) == U"✗ 2 3m 12s");
    }

    SECTION("minutes keep two digits of seconds")
    {
        record.state = CommandBlockState::Finished;
        record.exitCode = 2;
        record.duration = 64s;
        CHECK(stickyHeaderChip(record, start + 1h) == U"✗ 2 1m 04s");
    }

    SECTION("killed by a signal: the signal's name, not the shell's 128 + n code")
    {
        record.state = CommandBlockState::Finished;
        record.exitCode = 137;
        record.duration = 3s;
        CHECK(stickyHeaderChip(record, start + 1h) == U"✗ SIGKILL 3s");

        // Named by OSC 3008 as well, it reads the same.
        record.outcome.signal = ContextSignal::Kill;
        CHECK(stickyHeaderChip(record, start + 1h) == U"✗ SIGKILL 3s");
    }

    SECTION("a crash or an interrupt OSC 3008 reported: the code, which the notification's verb says")
    {
        record.state = CommandBlockState::Finished;
        record.exitCode = 139;
        record.outcome.exit = ContextExit::Crash;
        CHECK(stickyHeaderChip(record, start + 1h) == U"✗ 139");

        record.exitCode = 130;
        record.outcome.exit = ContextExit::Interrupt;
        CHECK(stickyHeaderChip(record, start + 1h) == U"✗ 130");
    }

    SECTION("a failure only OSC 3008 reported: the code a shell would have reported")
    {
        record.state = CommandBlockState::Finished;
        record.outcome.exit = ContextExit::Failure;
        record.outcome.status = 2;
        CHECK(stickyHeaderChip(record, start + 1h) == U"✗ 2");
    }

    SECTION("a failure with no code worth printing: the mark alone")
    {
        record.state = CommandBlockState::Finished;
        record.outcome.exit = ContextExit::Failure;
        CHECK(stickyHeaderChip(record, start + 1h) == U"✗");
    }

    SECTION("an implicit end -- no exit code, no duration -- names only the outcome")
    {
        record.state = CommandBlockState::Finished;
        record.end = CommandBlockEnd::Implicit;
        CHECK(stickyHeaderChip(record, start + 1h) == U"✓");
    }

    SECTION("a block still at its prompt has run nothing to name")
    {
        record.state = CommandBlockState::Prompting;
        record.commandStartedAt.reset();
        CHECK(stickyHeaderChip(record, start + 1h).empty());
    }
}

TEST_CASE("StickyHeader.placeholder.readsNoMoreOfTheCommandThanCanShow", "[stickyheader]")
{
    // Built every frame it shows, from a command line of up to MaxRecordedCommandLineBytes: only what a row
    // of the budget's width can show is sanitised and converted, never the whole record.
    auto const longCommand = CommandLineText { "make " + std::string(MaxRecordedCommandLineBytes - 5, 'x') };
    auto constexpr Budget = ColumnCount(20);

    auto const text = evictedPlaceholderText(longCommand, Budget);
    CHECK(text.starts_with(U"⋯ make xxxx"));
    // Four bytes are the most one column's codepoint takes, so that many bytes of the line fill the budget.
    CHECK(text.size() <= (std::size_t { 4 } * unbox<std::size_t>(Budget)) + 32);
    CHECK(text.size() >= unbox<std::size_t>(Budget));
}

TEST_CASE("StickyHeader.placeholder.namesTheCommandItLost", "[stickyheader]")
{
    auto constexpr Page = ColumnCount(80);
    CHECK(evictedPlaceholderText(CommandLineText { "make" }, Page) == U"⋯ make — earlier output evicted");

    // A command the shell never reported and the grid could not give back.
    CHECK(evictedPlaceholderText(CommandLineText {}, Page) == U"⋯ earlier output evicted");

    // A command line is untrusted: a crafted one must not put a control sequence on the screen through
    // the placeholder.
    auto const hostile = evictedPlaceholderText(CommandLineText { "make\x1b[201~" }, Page);
    CHECK_FALSE(hostile.contains(U'\x1b'));
    CHECK(hostile.contains(U'\uFFFD'));
}

TEST_CASE("StickyHeader.commandLine.isTheDisplayFormCutToItsBound", "[stickyheader]")
{
    CHECK(stickyHeaderCommandLine(CommandLineText { "cmake --build" }) == "cmake --build");
    CHECK(stickyHeaderCommandLine(CommandLineText {}).empty());

    // Untrusted: no control sequence reaches the chrome.
    auto const hostile = stickyHeaderCommandLine(CommandLineText { "make\x1b[201~" });
    CHECK_FALSE(hostile.contains('\x1b'));

    // A long record is cut to the header's bound with an ellipsis, never inside a codepoint: a two-byte `ä`
    // straddling the bound goes whole.
    auto const straddling =
        CommandLineText { std::string(MaxStickyHeaderCommandLineBytes - 1, 'x') + "\xC3\xA4" };
    auto const cut = stickyHeaderCommandLine(straddling);
    CHECK(cut.size() <= MaxStickyHeaderCommandLineBytes);
    CHECK(cut.ends_with("x…"));
    auto const longest =
        stickyHeaderCommandLine(CommandLineText { std::string(MaxRecordedCommandLineBytes, 'y') });
    CHECK(longest.size() <= MaxStickyHeaderCommandLineBytes);
    CHECK(longest.ends_with("y…"));
    // One that fits is not touched.
    auto const fits = std::string(MaxStickyHeaderCommandLineBytes, 'z');
    CHECK(stickyHeaderCommandLine(CommandLineText { fits }) == fits);
}
