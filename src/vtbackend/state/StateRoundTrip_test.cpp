// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/grid/Grid.hpp>
#include <vtbackend/state/GridState.hpp>
#include <vtbackend/state/StateFields.hpp>
#include <vtbackend/state/TerminalStateCodec.hpp>
#include <vtbackend/testing/MockTerm.hpp>
#include <vtbackend/testing/StateHandover.hpp>
#include <vtbackend/testing/Workloads.hpp>

#include <core/Escape.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <format>
#include <ranges>
#include <span>
#include <string>
#include <string_view>

using namespace vtbackend;

namespace
{
/// How much of a workload the round-trip harness feeds, and how often it captures.
constexpr auto CorpusBytes = size_t { 24 } * 1024;
constexpr auto OffsetStride = size_t { 3 } * 1024;
constexpr auto CorpusHistory = 200;

/// Feeds @p corpus to a terminal in strides and hands the harness every prefix boundary.
///
/// The harness of the live upgrade: a check that holds at every offset of the corpus holds wherever
/// the daemon happens to be interrupted. Later state kinds add their own checks to @p check.
/// @param corpus The bytes to feed.
/// @param check  Called with the terminal after each stride and the number of bytes fed so far.
template <typename Check>
void atEveryOffset(std::string_view corpus, Check const& check)
{
    auto mock =
        MockTerm { PageSize { LineCount(test::WorkloadPageLines), ColumnCount(test::WorkloadPageColumns) },
                   LineCount(CorpusHistory) };
    // Block indices rather than std::views::stride, which the libc++ that macOS ships lacks.
    auto const blockCount = (corpus.size() + OffsetStride - 1) / OffsetStride;
    for (auto const blockIndex: std::views::iota(size_t { 0 }, blockCount))
    {
        auto const start = blockIndex * OffsetStride;
        auto const chunk = corpus.substr(start, std::min(OffsetStride, corpus.size() - start));
        mock.writeToScreen(chunk);
        check(mock, start + chunk.size());
    }
}

/// Names the fragments of a grid that holds none.
class NoFragments final: public ImageFragmentNamer
{
  public:
    [[nodiscard]] uint64_t fragmentId(std::shared_ptr<ImageFragment> const& /*fragment*/) override
    {
        FAIL("the workloads draw no images");
        return 0;
    }
};

/// @return @p grid captured.
GridState captured(Grid const& grid)
{
    auto namer = NoFragments {};
    return grid.captureState(namer);
}

/// Round-trips the grid through the byte codec and compares it, line by line, with the original.
void checkGridRoundTrip(Grid const& grid)
{
    auto writer = StateWriter::payload();
    detail::encodeFields(writer, captured(grid));
    auto const bytes = std::move(writer).take();
    auto decoded = GridState {};
    REQUIRE(detail::decodeFields(bytes, decoded).has_value());
    auto restored = Grid { grid.pageSize(), false, grid.historyLimits() };
    REQUIRE(restored.restoreState(decoded, NoImagesResolver {}).has_value());

    REQUIRE(restored.historyLineCount() == grid.historyLineCount());
    CHECK(captured(restored) == captured(grid));
    for (auto const offset:
         std::views::iota(-unbox<int>(grid.historyLineCount()), unbox<int>(grid.pageSize().lines)))
    {
        INFO(std::format("offset {}", offset));
        REQUIRE(restored.lineText(LineOffset(offset)) == grid.lineText(LineOffset(offset)));
        REQUIRE(restored.stableLineIdOf(LineOffset(offset)) == grid.stableLineIdOf(LineOffset(offset)));
    }
}
} // namespace

TEST_CASE("grid round-trips at every offset of the Workloads text corpus", "[StateRoundTrip]")
{
    for (auto const& workload: test::Workloads)
    {
        DYNAMIC_SECTION(workload.name)
        {
            auto const corpus = test::makeWorkload(workload, CorpusBytes);
            atEveryOffset(corpus, [](auto& mock, size_t /*fed*/) {
                checkGridRoundTrip(mock.terminal.primaryScreen().grid());
            });
        }
    }
}

// {{{ the whole terminal state

namespace
{
/// The terminals of the whole-state harness: the page the workloads assume, with some scrollback.
test::HandoverSetup workloadSetup()
{
    return test::HandoverSetup {
        .pageSize = PageSize { LineCount(test::WorkloadPageLines), ColumnCount(test::WorkloadPageColumns) },
        .history = LineCount(CorpusHistory),
        .checkpointStride = 256,
    };
}

/// Hands a terminal parsing @p corpus over at every offset of it up to FullSweepLimit bytes, at
/// SampledOffsetCount seeded-random ones above, and requires each handover to be exact.
void requireExactAtSampledOffsets(std::string_view corpus, test::HandoverSetup const& setup)
{
    auto const offsets = test::sampledOffsets(corpus.size());
    auto const mismatch = test::findHandoverMismatch(corpus, setup, offsets);
    if (mismatch)
        FAIL(std::format("handed over after {} of {} bytes (\"{}\" next), compared after {}: {}",
                         mismatch->offset,
                         corpus.size(),
                         core::escape(corpus.substr(mismatch->offset, 8)),
                         mismatch->checkpoint,
                         mismatch->what));
    SUCCEED();
}

/// A full-screen application: alternate screen, mouse reporting, bracketed paste and the kitty keyboard
/// protocol, repainted from the cursor home position.
std::string vimLikeSession()
{
    auto session = std::string { "shell prompt $ vim file\r\n" };
    session += "\033[?1049h\033[?1002h\033[?1006h\033[?2004h\033[>1u\033[?25l\033[2J";
    for (auto const row: std::views::iota(1, 9))
        session +=
            std::format("\033[{};1H\033[38;5;{}m{:>3} the quick brown fox\033[0m\033[K", row, row + 30, row);
    session += "\033[10;1H\033[7m-- INSERT --\033[0m\033[?25h\033[5 q\033[3;5H";
    session += "typed text\033[?1049l\033[?1002l\033[?2004l\033[<u back at the prompt\r\n";
    return session;
}

/// A Sixel image and a kitty one (direct RGB, then a placement of it), with text around them.
std::string imageSession()
{
    return std::string {
        "before\r\n\033Pq\"1;1;12;6#1;2;100;0;0#1~~~~~~~~~~~~-#2;2;0;100;0#2~~~~~~~~~~~~\033\\mid\r\n"
        "\033_Ga=T,f=24,s=2,v=1,i=7;AAAA/wAA\033\\\r\n"
        "\033_Ga=p,i=7,c=3,r=1\033\\after\r\n"
    };
}

/// Claude Code showing a picture through kitty Unicode placeholders: the transmission with its virtual
/// placement, three image rows of placeholder cells, a row naming an image that only arrives at the end, and
/// that image.
std::string placeholderSession()
{
    auto const row = [](std::string_view sgr, std::string_view rowMark) {
        auto text = std::format("\033[{}m", sgr);
        for (auto const column: { "\xCC\x85", "\xCC\x8D", "\xCC\x8E", "\xCC\x90" })
            text += std::format("\xF4\x8E\xBB\xAE{}{}", rowMark, column);
        return text + "\033[m\r\n";
    };
    auto session = std::string { "$ claude\r\n\033_Ga=T,U=1,i=300,c=4,r=3,f=24,s=4,v=3,q=2;" };
    session += std::string(48, 'A');
    session += "\033\\";
    for (auto const rowMark: { "\xCC\x85", "\xCC\x8D", "\xCC\x8E" })
        session += row("38;2;0;1;44", rowMark);
    session += row("38;5;9", "\xCC\x85");
    session += "\033_Ga=T,U=1,i=9,c=4,r=1,f=24,s=2,v=1,q=2;AAAA/wAA\033\\done\r\n";
    return session;
}
} // namespace

TEST_CASE("terminal state round-trips at sampled offsets of the Workloads corpus", "[StateRoundTrip]")
{
    for (auto const& workload: test::Workloads)
    {
        DYNAMIC_SECTION(workload.name)
        {
            requireExactAtSampledOffsets(test::makeWorkload(workload, CorpusBytes / 2), workloadSetup());
        }
    }
}

TEST_CASE("terminal state round-trips at every offset of a vim-like session", "[StateRoundTrip]")
{
    requireExactAtSampledOffsets(vimLikeSession(), workloadSetup());
}

TEST_CASE("terminal state round-trips at every offset of Sixel and kitty images", "[StateRoundTrip]")
{
    auto const setup = workloadSetup();
    requireExactAtSampledOffsets(imageSession(), setup);

    auto source = setup.makeTerminal();
    source->writeToScreen(imageSession());
    CHECK_FALSE(source->terminal.captureState().images.images.empty());
}

TEST_CASE("terminal state round-trips at every offset of a kitty placeholder picture", "[StateRoundTrip]")
{
    // The virtual placement, the image numbers and the placeholder origin all have to cross: a handover
    // between the cells and their late image must still let that image resolve them.
    auto const setup = workloadSetup();
    requireExactAtSampledOffsets(placeholderSession(), setup);

    auto source = setup.makeTerminal();
    source->writeToScreen(placeholderSession());
    auto const state = source->terminal.captureState();
    REQUIRE_FALSE(state.pages.empty());
    CHECK(state.pages.front().kitty.virtualPlacements.size() == 2);
}

TEST_CASE("captureState is deterministic", "[StateRoundTrip]")
{
    auto const setup = workloadSetup();
    auto source = setup.makeTerminal();
    source->writeToScreen(vimLikeSession() + imageSession()
                          + test::makeWorkload(test::Workloads.front(), CorpusBytes / 4));
    source->terminal.flushInput();
    auto const first = encodeTerminalState(source->terminal.captureState());
    auto const second = encodeTerminalState(source->terminal.captureState());
    CHECK(first == second);

    auto const decoded = decodeTerminalState(first);
    REQUIRE(decoded.has_value());
    CHECK(encodeTerminalState(*decoded) == first);
}

TEST_CASE("StateRoundTrip.programStatus.keepsOriginSinceGeneration", "[StateRoundTrip]")
{
    auto const setup = workloadSetup();
    auto source = setup.makeTerminal();
    auto record = ProgramStatusRecord {};
    record.id = "agent";
    record.state = ProgramState::Blocked;
    record.kind = BlockedKind::Permission;
    record.origin = ProgramStatusOrigin::Integration;
    record.since = std::chrono::system_clock::time_point { std::chrono::seconds(1'700'000'000) };
    record.generation = 3;
    source->terminal.replaceProgramStatus({ record }, vtbackend::ProgramStatusTiming::Carried);
    // Spend a generation on a record that is then gone: the counter stands at 5, one above the 4 that the
    // remaining record implies, so only the carried counter can say so.
    source->writeToScreen("\033]7501;state=working:id=tmp\033\\\033]7501;state=clear:id=tmp\033\\");
    REQUIRE(source->terminal.programStatus().nextGeneration() == 5);

    auto const target = handedOver(*source, setup);
    REQUIRE(target.has_value());
    auto const& restored = (*target)->terminal.programStatus();
    REQUIRE(restored.records().size() == 1);
    CHECK(restored.records().front() == record);
    CHECK(restored.nextGeneration() == 5);
}

TEST_CASE("decodeTerminalState refuses a damaged stream", "[StateRoundTrip]")
{
    auto const setup = workloadSetup();
    auto source = setup.makeTerminal();
    source->writeToScreen(vimLikeSession());
    auto const bytes = encodeTerminalState(source->terminal.captureState());

    auto const truncated = decodeTerminalState(std::span<uint8_t const> { bytes }.first(bytes.size() / 2));
    CHECK_FALSE(truncated.has_value());

    auto wrongMagic = bytes;
    wrongMagic.front() ^= 0xFF;
    CHECK(decodeTerminalState(wrongMagic) == std::unexpected { StateDecodeError::MalformedRecord });
    CHECK(decodeTerminalState(std::span<uint8_t const> {})
          == std::unexpected { StateDecodeError::Truncated });
}

// }}}
